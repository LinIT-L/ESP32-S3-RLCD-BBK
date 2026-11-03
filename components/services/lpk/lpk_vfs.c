/**
 * lpk_vfs.c — `/lpk` 的 ESP-IDF VFS 适配（真机入口，见 lpk_vfs.h 的取舍说明）
 *
 * 三条硬规矩（违反任何一条都算没做完）：
 *   ① 资源只读：对 `/lpk/ABC.MKF` 的 write 一律 EROFS，绝不"写成功但没保存"；
 *   ② 可写路径只落真实 FAT：`/lpk/save/x` → `<base>/save/x`，不进包、不落内存盘；
 *   ③ 句柄有界：包内句柄来自 lpk_mnt_t 的定长表，用完归还；
 *      真实 FAT 句柄在卸载时统一关掉，避免 FatFs 卷锁被带走（那是"退出后整机卡死"的老病因）。
 *
 * 并发：包只有一个 FILE*，`lpk_m_read()` 内部是 fseek+fread 两步，
 * 多任务同时读会互相踩位置 ⇒ 一把互斥锁把所有进 mnt 的操作串起来。
 * （仙剑读资源在游戏任务里，音乐预渲染读 mus.mkf 在音频任务里，确实是两个任务。）
 */
#include "lpk_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lpk_mount.h"

static const char *TAG = "LPKVFS";

/* 挂载状态。整块放 PSRAM（lpk_mnt_t 带 16 个句柄槽 ≈ 4.5 KB，不占内部 RAM）。 */
typedef struct {
    bool              mounted;
    lpk_mnt_t        *m;
    lpk_entry_t      *tab;         /* 条目表（PSRAM） */
    char              lpk[160];
    char              base[160];
    SemaphoreHandle_t lock;        /* 护住**句柄表**与缓存（只覆盖 open/close/stat，不覆盖读） */
    SemaphoreHandle_t slock;       /* 护住 I/O 计数器的累加（微秒级，不覆盖读） */
} lpk_vfs_ctx_t;

static lpk_vfs_ctx_t s_v;

/* ============ 预读窗专用分配器（2026-11-01）============
 * 只从 PSRAM 拿，拿不到就返回 NULL ⇒ 挂载层退化为逐次直读。
 * ⚠ 刻意**不回落内部 RAM**：内部只剩 100 KB 左右，16 句柄 × 256 KB 会直接把系统打死；
 *   性能降级可以接受，把内部 RAM 吃光不可以。 */
static void *pref_psram_alloc(unsigned long size, void *ctx) {
    (void)ctx;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}
static void pref_psram_free(void *p, void *ctx) {
    (void)ctx;
    if (p) heap_caps_free(p);
}

/* ============ PSRAM 整资源缓存（2026-11-01） ============
 * 为什么是"整资源"而不是"块缓存"
 * ---------------------------------
 * 仙剑的读法：打开 MKF → 读索引 → 按偏移反复 fread 块；切场景/回菜单时会**再开一次**。
 * 所以
 *   ① 同一个资源会被多次打开 ⇒ 整资源读进 PSRAM 后，后面全是 memcpy；
 *   ② 那一次是**一趟连续大读**，比几十次"seek+小读"快得多
 *      —— SD 是 1-bit / 40MHz，每次事务的固定开销占大头（实测小读只有 ~200 KB/s）。
 *
 * 容量：总预算 4 MB、单资源上限 2 MB、8 个槽（PSRAM 空闲 ~7 MB，留足余量）。
 * 超过上限的大资源（GOP 11 MB / RNG 4.3 MB）不进缓存，照旧按偏移读 —— 它们只会被
 * 顺序读一遍，缓存进来反而挤掉别的。 */
#define LPK_CACHE_BUDGET    (4u * 1024u * 1024u)
#define LPK_CACHE_ENTRY_MAX (2u * 1024u * 1024u)
#define LPK_CACHE_SLOTS     8

typedef struct {
    int      idx;        /* 资源下标, -1 = 空槽 */
    uint8_t *p;
    uint32_t size;
    uint32_t used_ms;    /* 最近一次使用时刻（LRU 用） */
} lpk_cslot_t;

static struct {
    bool        on;          /* 运行期可关: 串口 lpkstat 0|1 */
    lpk_cslot_t s[LPK_CACHE_SLOTS];
    uint32_t    bytes;       /* 已占用字节 */
    uint32_t    hits, loads, evicts, fails;
} s_c = { .on = false };   /* ★ 默认关：整资源预载会把 open 变成"读整个文件"，实测拖垮载入 */

/* I/O 统计：证明"慢在 SD"还是"慢在 CPU"，也用来对比优化前后 */
static struct {
    uint32_t reads, bytes, us;
} s_io;

/* ============================ 小工具 ============================ */

static int err_from_lpk(int rc) {
    switch (rc) {
    case LPK_E_NOENT: return ENOENT;
    case LPK_E_ROFS:  return EROFS;
    case LPK_E_NOMEM: return EMFILE;
    case LPK_E_PATH:  return EINVAL;
    case LPK_E_ISDIR: return EISDIR;
    case LPK_E_AMBIG: return ENOENT;     /* 仅大小写重名：宁可"没有"，不猜 */
    case LPK_E_ARG:   return EINVAL;
    default:          return EIO;
    }
}

/* ★ 2026-11-01 关键（照 IDF 源码核实过）：
 *   `vfs_calls.c` 在调 FS 之前已经用 `translate_path()` 把挂载点**剥掉了**，
 *   所以我们收到的是 "/ABC.MKF"、"/save/0.rpg"，挂载根本身是 "/"。
 *   （一开始按"收到 /lpk/ABC.MKF"写，结果是每个 open 都 EINVAL —— 编译期发现不了。）
 * 这里只去掉前导 '/'，得到挂载点内部的相对路径；根返回空串。 */
static const char *strip_mnt(const char *path) {
    if (!path || path[0] != '/') return NULL;
    const char *p = path + 1;
    while (*p == '/') p++;
    return p;
}

#define LOCK()   do { if (s_v.lock) xSemaphoreTake(s_v.lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_v.lock) xSemaphoreGive(s_v.lock); } while (0)

/* ---------------- PSRAM 整资源缓存 ---------------- */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static lpk_cslot_t *cache_find(int idx) {
    for (int i = 0; i < LPK_CACHE_SLOTS; i++)
        if (s_c.s[i].idx == idx) return &s_c.s[i];
    return NULL;
}

/* 找一个空槽，没有就淘汰"最久没用"的 */
static lpk_cslot_t *cache_alloc_slot(void) {
    lpk_cslot_t *best = NULL;
    for (int i = 0; i < LPK_CACHE_SLOTS; i++) {
        if (s_c.s[i].idx < 0) return &s_c.s[i];
        if (!best || s_c.s[i].used_ms < best->used_ms) best = &s_c.s[i];
    }
    if (best) {                       /* 淘汰 */
        if (best->p) { heap_caps_free(best->p); s_c.bytes -= (s_c.bytes >= best->size) ? best->size : s_c.bytes; }
        best->p = NULL; best->size = 0; best->idx = -1; s_c.evicts++;
    }
    return best;
}

/* 把某条资源整个读进 PSRAM。调用方保证已持锁。返回槽；失败返回 NULL（按原路走）。 */
static lpk_cslot_t *cache_load(int idx) {
    if (!s_c.on || !s_v.mounted || !s_v.m) return NULL;
    const lpk_entry_t *e = &s_v.m->en[idx];
    if (e->size == 0 || e->size > LPK_CACHE_ENTRY_MAX) return NULL;
    /* 预算不够就按 LRU 丢到放得下（丢的是"最久没用"的那几个，不一定是当前的） */
    while (s_c.bytes + e->size > LPK_CACHE_BUDGET) {
        lpk_cslot_t *v = NULL;
        for (int i = 0; i < LPK_CACHE_SLOTS; i++)
            if (s_c.s[i].idx >= 0 && (!v || s_c.s[i].used_ms < v->used_ms)) v = &s_c.s[i];
        if (!v) break;
        if (v->p) heap_caps_free(v->p);
        s_c.bytes -= (s_c.bytes >= v->size) ? v->size : s_c.bytes;
        v->p = NULL; v->size = 0; v->idx = -1; s_c.evicts++;
    }
    lpk_cslot_t *slot = cache_alloc_slot();
    if (!slot) return NULL;
    uint8_t *p = heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(e->size, MALLOC_CAP_8BIT);
    if (!p) { s_c.fails++; return NULL; }

    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    if (lpk_read(&s_v.m->h, e, 0, p, e->size) != LPK_OK) {
        free(p); s_c.fails++;
        ESP_LOGW(TAG, "缓存装载失败: %s (%u B)", e->name, (unsigned)e->size);
        return NULL;
    }
    s_io.us += (uint32_t)(esp_timer_get_time() / 1000) - t0;
    s_io.reads++; s_io.bytes += e->size;

    slot->idx = idx; slot->p = p; slot->size = e->size; slot->used_ms = now_ms();
    s_c.bytes += e->size; s_c.loads++;
    ESP_LOGI(TAG, "预载 %s (%u B) → PSRAM，缓存 %u/%u KB",
             e->name, (unsigned)e->size, (unsigned)(s_c.bytes / 1024),
             (unsigned)(LPK_CACHE_BUDGET / 1024));
    return slot;
}

void lpk_vfs_cache_set(bool on);
void lpk_vfs_cache_set(bool on) {
    s_c.on = on;
    /* 同步预读窗：关 = 把 pre_wsz 置 0（新读走直读）并立刻还掉已占的 PSRAM。
     * 这样 `lpkstat 0` 能一键回到「逐次直读」，是性能对照的开关。 */
    if (s_v.m) {
        if (on) lpk_m_set_prefetch(s_v.m, 64u * 1024u, 4u * 1024u * 1024u,
                                   pref_psram_alloc, pref_psram_free, NULL);
        else  { lpk_m_set_prefetch(s_v.m, 0, 0, pref_psram_alloc, pref_psram_free, NULL);
                lpk_m_drop_prefetch(s_v.m); }
    }
    if (!on) lpk_vfs_cache_flush();
}
bool lpk_vfs_cache_get(void);
bool lpk_vfs_cache_get(void) { return s_c.on; }
void lpk_vfs_cache_flush(void);
void lpk_vfs_cache_flush(void) {
    for (int i = 0; i < LPK_CACHE_SLOTS; i++) {
        if (s_c.s[i].p) heap_caps_free(s_c.s[i].p);
        s_c.s[i].p = NULL; s_c.s[i].size = 0; s_c.s[i].idx = -1;
    }
    s_c.bytes = 0;
}

/* 统计快照（给串口 lpkstat 用） */
void lpk_vfs_stats(uint32_t *reads, uint32_t *bytes, uint32_t *us,
                   uint32_t *hits, uint32_t *loads, uint32_t *cache_bytes);
void lpk_vfs_stats(uint32_t *reads, uint32_t *bytes, uint32_t *us,
                   uint32_t *hits, uint32_t *loads, uint32_t *cache_bytes) {
    if (reads) *reads = s_io.reads;
    if (bytes) *bytes = s_io.bytes;
    if (us) *us = s_io.us;
    if (hits) *hits = s_c.hits;
    if (loads) *loads = s_c.loads;
    if (cache_bytes) *cache_bytes = s_c.bytes;
}
void lpk_vfs_stats_reset(void);
void lpk_vfs_stats_reset(void) { s_io.reads = s_io.bytes = s_io.us = 0; s_c.hits = s_c.loads = 0; }

/* ============================ 真实 FAT 后端 ============================ */
/* 用 POSIX 同名函数实现：设备上这些调用会再次进入 VFS，前缀是 /sdcard（FATFS），
 * 与本层没有递归（本层只处理 /lpk）。 */

static int bk_open(void *c, const char *path, int flags) {
    (void)c;
    return open(path, flags, 0666);
}
static int bk_read(void *c, int fd, void *buf, size_t n) {
    (void)c;
    int r = (int)read(fd, buf, n);
    return r;
}
static int bk_write(void *c, int fd, const void *buf, size_t n) {
    (void)c;
    return (int)write(fd, buf, n);
}
static long bk_lseek(void *c, int fd, long off, int whence) {
    (void)c;
    return (long)lseek(fd, off, whence);
}
static int bk_close(void *c, int fd) {
    (void)c;
    return close(fd);
}
static int bk_stat(void *c, const char *path, lpk_m_stat_t *st) {
    (void)c;
    struct stat s;
    if (stat(path, &s) != 0) return -1;
    st->is_dir = S_ISDIR(s.st_mode) ? 1 : 0;
    st->size   = (unsigned long)s.st_size;
    return 0;
}
static int bk_mkdir(void *c, const char *path) { (void)c; return mkdir(path, 0777); }
static int bk_unlink(void *c, const char *path) { (void)c; return unlink(path); }
static int bk_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b); }

static const lpk_backing_t BK = {
    .ctx    = NULL,
    .open   = bk_open,
    .read   = bk_read,
    .write  = bk_write,
    .lseek  = bk_lseek,
    .close  = bk_close,
    .stat   = bk_stat,
    .mkdir  = bk_mkdir,
    .unlink = bk_unlink,
    .rename = bk_rename,
};

/* ============================ VFS 操作 ============================ */

static int vfs_open(void *ctx, const char *path, int flags, int mode) {
    (void)ctx; (void)mode;
    if (!s_v.mounted) { errno = ENOENT; return -1; }
    const char *rel = strip_mnt(path);
    if (!rel) { errno = EINVAL; return -1; }
    if (!rel[0]) { errno = EISDIR; return -1; }        /* 挂载根不能当文件打开 */

    LOCK();
    int hd = -1;
    /* 只读打开的资源：先看 PSRAM 缓存（命中就完全不碰 SD） */
    if (s_c.on && (flags & LPK_O_ACCMODE) == LPK_O_RDONLY) {
        int idx = lpk_find_ci(&s_v.m->h, s_v.m->en, rel);
        if (idx >= 0) {
            lpk_cslot_t *slot = cache_find(idx);
            if (!slot) slot = cache_load(idx);        /* 首次：整资源读一遍 */
            if (slot) {
                slot->used_ms = now_ms();
                s_c.hits++;
                hd = lpk_m_open_buf(s_v.m, s_v.m->en[idx].name, slot->p, slot->size);
            }
        }
        /* idx<0 / 装载失败 ⇒ 落到下面的普通打开（行为与加缓存前完全一致） */
    }
    if (hd < 0) hd = lpk_m_open(s_v.m, rel, flags);
    UNLOCK();
    if (hd < 0) { errno = err_from_lpk(hd); return -1; }
    return hd;
}

static int vfs_close(void *ctx, int fd) {
    (void)ctx;
    if (!s_v.mounted) { errno = EBADF; return -1; }
    LOCK();
    int rc = lpk_m_close(s_v.m, fd);
    UNLOCK();
    if (rc != LPK_OK) { errno = EBADF; return -1; }
    return 0;
}

/* ★★★ 2026-11-01 本轮核心修复 ★★★
 * read/write/lseek/fstat **不再持有全局锁**。
 *   理由：每个资源句柄有自己的 FILE*（见 lpk_mount.c），读写位置互不影响，
 *         读一个 1.5 MB 资源的过程不再阻塞别的句柄。
 *   仍然存在的串行点：FatFs 内部的卷锁 + sdmmc 的物理总线 —— 那是驱动层的事，
 *         粒度是"每次扇区事务"，不是"整次读"。别把它当成已经解决了。
 *   I/O 计数只用一个独立小锁护住累加（临界区约 1 微秒）。
 * ⚠ 同一个句柄被两个任务**同时**读写仍是未定义 —— 与 POSIX 一致（fread 不是线程安全的），
 *   引擎自己也不会这么用。 */
static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size) {
    (void)ctx;
    if (!s_v.mounted) { errno = EBADF; return -1; }
    /* kind: 0/1 = 要读卡, 2 = PSRAM 缓存。统计只算"真读卡"的部分，
     * 这样 lpkstat 里的 KB/s 才能直接反映 SD 的实际吞吐。 */
    bool from_sd = (fd >= 0 && fd < LPK_M_MAXH && s_v.m->hd[fd].used && s_v.m->hd[fd].kind != 2);
    uint32_t t0 = from_sd ? (uint32_t)(esp_timer_get_time() / 1000) : 0;
    int r = lpk_m_read(s_v.m, fd, dst, size);
    if (from_sd && r > 0) {
        uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        if (s_v.slock) xSemaphoreTake(s_v.slock, portMAX_DELAY);
        s_io.us += dt; s_io.reads++; s_io.bytes += (uint32_t)r;
        if (s_v.slock) xSemaphoreGive(s_v.slock);
    }
    if (r < 0) { errno = err_from_lpk(r); return -1; }
    return (ssize_t)r;
}

static ssize_t vfs_write(void *ctx, int fd, const void *src, size_t size) {
    (void)ctx;
    if (!s_v.mounted) { errno = EBADF; return -1; }
    int r = lpk_m_write(s_v.m, fd, src, size);   /* 每句柄独立状态，无需全局锁 */
    if (r < 0) { errno = err_from_lpk(r); return -1; }
    return (ssize_t)r;
}

static off_t vfs_lseek(void *ctx, int fd, off_t off, int whence) {
    (void)ctx;
    if (!s_v.mounted) { errno = EBADF; return -1; }
    long r = lpk_m_seek(s_v.m, fd, (long)off, whence);   /* 同上 */
    if (r < 0) { errno = EINVAL; return -1; }   /* fseek 越界 = EINVAL，与 newlib 一致 */
    return (off_t)r;
}

static void fill_stat(struct stat *st, bool is_dir, unsigned long size) {
    memset(st, 0, sizeof(*st));
    st->st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = (off_t)size;
    st->st_nlink = 1;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st) {
    (void)ctx;
    if (!s_v.mounted || !st) { errno = EBADF; return -1; }
    /* lpk_m_* 没有 fstat，用 seek(0,SEEK_END) 取大小再还原位置 —— 和 PAL 的 flength() 同一手法。
     * 只动本句柄的位置，不需要全局锁。 */
    long cur = lpk_m_seek(s_v.m, fd, 0, LPK_SEEK_CUR);
    long end = (cur < 0) ? -1 : lpk_m_seek(s_v.m, fd, 0, LPK_SEEK_END);
    if (cur >= 0) (void)lpk_m_seek(s_v.m, fd, cur, LPK_SEEK_SET);
    if (end < 0) { errno = EBADF; return -1; }
    fill_stat(st, false, (unsigned long)end);
    return 0;
}

static int vfs_stat(void *ctx, const char *path, struct stat *st) {
    (void)ctx;
    if (!s_v.mounted || !st) { errno = ENOENT; return -1; }
    const char *rel = strip_mnt(path);
    if (!rel) { errno = EINVAL; return -1; }
    lpk_m_stat_t s;
    /* 索引表在挂载后**只读不变**，这里只是查表 + 前缀推断，不需要锁。
     * 卸载必须在"没有任何任务在用 /lpk"之后才做（pal_shutdown 的顺序保证）。 */
    int rc = lpk_m_stat(s_v.m, rel, &s);
    if (rc != LPK_OK) { errno = err_from_lpk(rc); return -1; }
    fill_stat(st, s.is_dir != 0, s.size);
    return 0;
}

static int vfs_access(void *ctx, const char *path, int amode) {
    (void)ctx; (void)amode;
    if (!s_v.mounted) { errno = ENOENT; return -1; }
    const char *rel = strip_mnt(path);
    if (!rel) { errno = EINVAL; return -1; }
    int rc = lpk_m_access(s_v.m, rel);
    if (rc != LPK_OK) { errno = ENOENT; return -1; }
    return 0;
}

static int vfs_mkdir(void *ctx, const char *path, mode_t mode) {
    (void)ctx; (void)mode;
    if (!s_v.mounted) { errno = EROFS; return -1; }
    const char *rel = strip_mnt(path);
    if (!rel) { errno = EINVAL; return -1; }
    /* 只动真实 FAT（后端 open/rename/unlink），不碰句柄表与缓存 ⇒ 不需要全局锁 */
    int rc = lpk_m_mkdir(s_v.m, rel);
    if (rc != LPK_OK) { errno = err_from_lpk(rc); return -1; }
    return 0;
}

static int vfs_unlink(void *ctx, const char *path) {
    (void)ctx;
    if (!s_v.mounted) { errno = EROFS; return -1; }
    const char *rel = strip_mnt(path);
    if (!rel) { errno = EINVAL; return -1; }
    int rc = lpk_m_unlink(s_v.m, rel);
    if (rc != LPK_OK) { errno = err_from_lpk(rc); return -1; }
    return 0;
}

static int vfs_rename(void *ctx, const char *src, const char *dst) {
    (void)ctx;
    if (!s_v.mounted) { errno = EROFS; return -1; }
    const char *a = strip_mnt(src), *b = strip_mnt(dst);
    if (!a || !b) { errno = EINVAL; return -1; }
    int rc = lpk_m_rename(s_v.m, a, b);
    if (rc != LPK_OK) { errno = err_from_lpk(rc); return -1; }
    return 0;
}

static int vfs_fsync(void *ctx, int fd) {
    (void)ctx; (void)fd;
    return 0;    /* 包内资源不可变；真实 FAT 的写入已经同步落到 FatFs */
}

static int vfs_ftruncate(void *ctx, int fd, off_t length) {
    (void)ctx; (void)fd; (void)length;
    errno = EROFS;
    return -1;
}

static int vfs_truncate(void *ctx, const char *path, off_t length) {
    (void)ctx; (void)path; (void)length;
    errno = EROFS;
    return -1;
}

static esp_vfs_t s_vfs = {
    .flags   = ESP_VFS_FLAG_CONTEXT_PTR,
    .open_p  = vfs_open,
    .close_p = vfs_close,
    .read_p  = vfs_read,
    .write_p = vfs_write,
    .lseek_p = vfs_lseek,
    .fstat_p = vfs_fstat,
    .fsync_p = vfs_fsync,
    .stat_p  = vfs_stat,
    .access_p = vfs_access,
    .mkdir_p  = vfs_mkdir,
    .unlink_p = vfs_unlink,
    .rename_p = vfs_rename,
    .truncate_p  = vfs_truncate,
    .ftruncate_p = vfs_ftruncate,
};

/* ============================ 挂载 / 卸载 ============================ */

static void free_all(void) {
    if (s_v.m) {
        lpk_unmount(s_v.m);
        free(s_v.m);
        s_v.m = NULL;
    }
    if (s_v.tab) { free(s_v.tab); s_v.tab = NULL; }
    if (s_v.lock)  { vSemaphoreDelete(s_v.lock);  s_v.lock  = NULL; }
    if (s_v.slock) { vSemaphoreDelete(s_v.slock); s_v.slock = NULL; }
    s_v.mounted = false;
    s_v.lpk[0] = 0;
    s_v.base[0] = 0;
}

esp_err_t lpk_vfs_mount(const char *lpk_path, const char *base_dir,
                        const lpk_vfs_wr_t *w, int nw) {
    if (!lpk_path || !lpk_path[0]) return ESP_ERR_INVALID_ARG;
    if (s_v.mounted) lpk_vfs_unmount();

    /* ① 先只读头拿条数：不按 LPK_MAX_ENTRIES(8192) 分配，省 1.6 MB 的浪费 */
    lpk_t h;
    int rc = lpk_open(&h, lpk_path, NULL, 0);
    if (rc != LPK_OK) {
        ESP_LOGE(TAG, "打开失败 rc=%d: %s", rc, lpk_path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t cnt = h.count;
    lpk_close(&h);
    if (cnt == 0) {
        ESP_LOGE(TAG, "包里没有资源条目: %s", lpk_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ② 条目表 + 挂载体都放 PSRAM */
    size_t bytes = (size_t)cnt * sizeof(lpk_entry_t);
    s_v.tab = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_v.tab) s_v.tab = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    s_v.m = heap_caps_malloc(sizeof(lpk_mnt_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_v.m) s_v.m = heap_caps_malloc(sizeof(lpk_mnt_t), MALLOC_CAP_8BIT);
    if (!s_v.tab || !s_v.m) {
        ESP_LOGE(TAG, "内存不足: 表 %u B + 挂载体 %u B",
                 (unsigned)bytes, (unsigned)sizeof(lpk_mnt_t));
        free_all();
        return ESP_ERR_NO_MEM;
    }
    s_v.lock  = xSemaphoreCreateMutex();
    s_v.slock = xSemaphoreCreateMutex();
    if (!s_v.lock || !s_v.slock) { free_all(); return ESP_ERR_NO_MEM; }

    rc = lpk_mount(s_v.m, lpk_path, s_v.tab, (int)cnt, base_dir ? base_dir : "", &BK);
    if (rc != LPK_OK) {
        ESP_LOGE(TAG, "lpk_mount rc=%d", rc);
        free_all();
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* 预读窗：64 KB/窗 × 4 窗/句柄，总预算 4 MB（全在 PSRAM）。
     * 依据见 lpk_mount.h：引擎每读一个 chunk 就在「索引区 ↔ 数据区」跨整个文件跳两次，
     * 4 个窗让索引窗常驻、数据窗跟 chunk 走。运行期可用 `lpkstat 0` 关掉做对照。 */
    lpk_m_set_prefetch(s_v.m, 64u * 1024u, 4u * 1024u * 1024u,
                       pref_psram_alloc, pref_psram_free, NULL);

    for (int i = 0; i < nw; i++) {
        if (!w[i].path) continue;
        int r2 = w[i].is_dir ? lpk_mount_add_writable_dir(s_v.m, w[i].path)
                             : lpk_mount_add_writable_file(s_v.m, w[i].path);
        if (r2 != LPK_OK) {
            ESP_LOGE(TAG, "可写登记失败 %s rc=%d", w[i].path, r2);
            free_all();
            return ESP_ERR_INVALID_ARG;
        }
    }
    snprintf(s_v.lpk, sizeof(s_v.lpk), "%s", lpk_path);
    snprintf(s_v.base, sizeof(s_v.base), "%s", base_dir ? base_dir : "");

    esp_err_t e = esp_vfs_register(LPK_VFS_MNT, &s_vfs, &s_v);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register(%s) 失败: %s", LPK_VFS_MNT, esp_err_to_name(e));
        free_all();
        return e;
    }
    s_v.mounted = true;
    ESP_LOGW(TAG, "已挂载 %s ⇐ %s（%u 条, 表 %u B, 可写旁路 %s, %d 条）",
             LPK_VFS_MNT, lpk_path, (unsigned)cnt, (unsigned)bytes,
             s_v.base[0] ? s_v.base : "(无)", nw);
    return ESP_OK;
}

esp_err_t lpk_vfs_unmount(void) {
    if (!s_v.mounted) return ESP_OK;
    {   /* 预读窗用量记一行，方便确认"是不是真的走了预读" */
        unsigned long pb = 0; int ph = 0;
        lpk_m_prefetch_stat(s_v.m, &pb, &ph);
        if (pb) ESP_LOGI(TAG, "预读窗: %d 个句柄 / %u KB（PSRAM）", ph, (unsigned)(pb / 1024));
    }
    esp_err_t e = esp_vfs_unregister(LPK_VFS_MNT);
    ESP_LOGW(TAG, "卸载 %s (%s) | 缓存 %u KB / %u 次预载, I/O %u 次 %u KB %u ms",
             LPK_VFS_MNT, esp_err_to_name(e), (unsigned)(s_c.bytes / 1024),
             (unsigned)s_c.loads, (unsigned)s_io.reads,
             (unsigned)(s_io.bytes / 1024), (unsigned)s_io.us);
    lpk_vfs_cache_flush();
    free_all();
    return e;
}

bool lpk_vfs_is_mounted(void) { return s_v.mounted; }
const char *lpk_vfs_lpk_path(void) { return s_v.mounted ? s_v.lpk : ""; }
int lpk_vfs_file_count(void) { return (s_v.mounted && s_v.m) ? s_v.m->h.loaded : 0; }

int lpk_vfs_open_handles(void) {
    if (!s_v.mounted || !s_v.m) return 0;
    int n = 0;
    LOCK();
    for (int i = 0; i < LPK_M_MAXH; i++) if (s_v.m->hd[i].used) n++;
    UNLOCK();
    return n;
}

/* ============================ 设备侧自检 ============================ */
/* 全部走 fopen/fseek/fread —— 与仙剑 util.c 的调用方式一模一样，
 * 所以这里 PASS 就等于"引擎那条路通"。 */

typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    int    fails;
    int    passes;
} st_out_t;

static void st_line(st_out_t *o, bool ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void st_line(st_out_t *o, bool ok, const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ok) o->passes++; else o->fails++;
    if (n < 0) return;
    if (o->p && o->len + (size_t)n + 16 < o->cap) {
        o->len += (size_t)snprintf(o->p + o->len, o->cap - o->len, "%s %s\r\n",
                                   ok ? "PASS" : "FAIL", buf);
    }
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* MKF: u32 offsets[count+1]，count = offsets[0]/4 - 1。
 * 这正是仙剑 palcommon.c PAL_OpenMKF/PAL_ReadMKFChunk 走的格式。 */
static void st_mkf(st_out_t *o, const char *name, uint8_t *a, uint8_t *b, uint32_t cap) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", LPK_VFS_MNT, name);
    FILE *f = fopen(path, "rb");
    if (!f) { st_line(o, false, "%s 打开失败", name); return; }

    if (fseek(f, 0, SEEK_END) != 0) { st_line(o, false, "%s seek end 失败", name); fclose(f); return; }
    long sz = ftell(f);
    if (sz <= 8) { st_line(o, false, "%s 尺寸异常 %ld", name, sz); fclose(f); return; }

    uint8_t b4[4];
    rewind(f);
    if (fread(b4, 1, 4, f) != 4) { st_line(o, false, "%s 读索引头失败", name); fclose(f); return; }
    uint32_t first = rd32le(b4);
    if (first < 8 || (first % 4) != 0) { st_line(o, false, "%s 索引头非法 %u", name, (unsigned)first); fclose(f); return; }
    uint32_t nblk = first / 4 - 1;
    if ((long)((nblk + 1) * 4) > sz) { st_line(o, false, "%s 索引越界 %u", name, (unsigned)nblk); fclose(f); return; }

    /* 索引本身：逐项读，验证单调不减且都落在文件内 */
    uint32_t off_next = 0;
    long bad = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= nblk; i++) {
        uint8_t raw[4];
        if (fseek(f, (long)i * 4, SEEK_SET) != 0 || fread(raw, 1, 4, f) != 4) { bad++; break; }
        uint32_t v = rd32le(raw);
        if (v < prev || (long)v > sz) bad++;
        prev = v;
        off_next = v;
    }
    st_line(o, bad == 0, "%s 索引 %u 块, 末偏移 %u / 文件 %ld", name, (unsigned)nblk,
            (unsigned)off_next, sz);

    /* 随机块读：头、中间、尾三块，各读两遍(第二遍通过**重开句柄**)，必须逐字节一致 */
    uint32_t pick[3] = { nblk > 2 ? 1u : 0u, nblk / 2, nblk ? nblk - 1 : 0 };
    for (int k = 0; k < 3; k++) {
        uint32_t i = pick[k];
        uint32_t o0 = 0, o1 = 0;
        if (fseek(f, (long)i * 4, SEEK_SET) != 0 || fread(b4, 1, 4, f) != 4) continue;
        o0 = rd32le(b4);
        if (fseek(f, (long)(i + 1) * 4, SEEK_SET) != 0 || fread(b4, 1, 4, f) != 4) continue;
        o1 = rd32le(b4);
        uint32_t len = o1 - o0;
        if (len == 0) {                              /* 空块合法（off[0]==off[1] 就是索引占位） */
            st_line(o, true, "%s 第%u块 @%u 为空块(合法)", name, (unsigned)i, (unsigned)o0);
            continue;
        }
        if (len > cap || o1 > (uint32_t)sz) {
            st_line(o, false, "%s 第%u块 长度异常 %u", name, (unsigned)i, (unsigned)len);
            continue;
        }
        /* 第一遍：当前句柄 */
        bool ok = fseek(f, (long)o0, SEEK_SET) == 0 && fread(a, 1, len, f) == len;
        /* 第二遍：重开句柄读同一块 */
        FILE *f2 = fopen(path, "rb");
        ok = ok && f2 && fseek(f2, (long)o0, SEEK_SET) == 0 && fread(b, 1, len, f2) == len;
        ok = ok && memcmp(a, b, len) == 0;
        if (f2) fclose(f2);
        st_line(o, ok, "%s 第%u块 @%u +%u 两次读一致", name, (unsigned)i,
                (unsigned)o0, (unsigned)len);
    }
    fclose(f);
}

int lpk_vfs_selftest(bool full_crc, char *out, size_t cap) {
    st_out_t o = { .p = out, .cap = cap, .len = 0, .fails = 0, .passes = 0 };
    if (out && cap) out[0] = 0;
    if (!s_v.mounted) { st_line(&o, false, "未挂载 %s", LPK_VFS_MNT); return o.fails; }

    st_line(&o, true, "挂载点 %s, 包 %s, %d 条, 可写旁路 %s",
            LPK_VFS_MNT, s_v.lpk, lpk_vfs_file_count(), s_v.base[0] ? s_v.base : "(无)");

    /* A. 目录与 stat/access */
    struct stat st;
    st_line(&o, stat(LPK_VFS_MNT, &st) == 0 && S_ISDIR(st.st_mode), "stat %s = 目录", LPK_VFS_MNT);
    /* 包里没有目录条目，靠前缀推断 —— pal_music.c 就是 stat("/lpk/music") 决定音乐模式的 */
    st_line(&o, stat(LPK_VFS_MNT "/music", &st) == 0 && S_ISDIR(st.st_mode),
            "stat %s/music = 目录（音乐模式判定依赖它）", LPK_VFS_MNT);
    st_line(&o, stat(LPK_VFS_MNT "/nosuchdir", &st) != 0, "不存在的目录 → 拒绝");
    st_line(&o, access(LPK_VFS_MNT "/ABC.MKF", 0) == 0, "access ABC.MKF");
    st_line(&o, access(LPK_VFS_MNT "/没有这个文件.MKF", 0) != 0, "不存在的资源 → 拒绝");

    /* B. 仙剑真实访问序列：ABC / GOP / FBP
     *    ⚠ 读块缓冲用 heap 分配、跑完即还 —— 别做成 static（那会常驻 16 KB 内部 RAM，
     *      而这段代码只在手敲命令时跑）。 */
    {
        const uint32_t blk = 8192;
        uint8_t *b1 = heap_caps_malloc(blk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *b2 = heap_caps_malloc(blk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b1) b1 = malloc(blk);
        if (!b2) b2 = malloc(blk);
        if (b1 && b2) {
            st_mkf(&o, "ABC.MKF", b1, b2, blk);
            st_mkf(&o, "GOP.MKF", b1, b2, blk);
            st_mkf(&o, "FBP.MKF", b1, b2, blk);
        } else {
            st_line(&o, false, "读块缓冲分配失败(2×%u B)", (unsigned)blk);
        }
        free(b1);
        free(b2);
    }

    /* C. 资源只读（写必须明确失败，不能"假成功"） */
    {
        FILE *f = fopen(LPK_VFS_MNT "/ABC.MKF", "wb");
        st_line(&o, f == NULL, "资源以写方式打开 → 被拒(%s)", f ? "竟然成功" : "EROFS");
        if (f) fclose(f);
    }

    /* D. 写分流：save/ 与 sdlpal.cfg 必须落到真实 FAT，并且在 /lpk 上读得回来 */
    {
        const char *rel = "save/_lpk_selftest.bin";
        char onlpk[96], onfat[220];
        snprintf(onlpk, sizeof(onlpk), "%s/%s", LPK_VFS_MNT, rel);
        snprintf(onfat, sizeof(onfat), "%s/%s", s_v.base, rel);
        uint8_t w[64], r[64];
        for (int i = 0; i < (int)sizeof(w); i++) w[i] = (uint8_t)(i * 7 + 3);

        mkdir(LPK_VFS_MNT "/save", 0777);        /* 已存在也应算成功 */
        FILE *fw = fopen(onlpk, "wb");
        bool ok = fw && fwrite(w, 1, sizeof(w), fw) == sizeof(w);
        if (fw) fclose(fw);
        st_line(&o, ok, "写 %s (+%u B)", onlpk, (unsigned)sizeof(w));

        FILE *fr = fopen(onlpk, "rb");
        size_t got = fr ? fread(r, 1, sizeof(r), fr) : 0;
        if (fr) fclose(fr);
        st_line(&o, got == sizeof(r) && memcmp(w, r, sizeof(w)) == 0,
                "回读 %s (%u B)", onlpk, (unsigned)got);

        /* 真值检查：直接看卡上的文件，而不是"接口说成功" */
        FILE *fd = fopen(onfat, "rb");
        size_t got2 = fd ? fread(r, 1, sizeof(r), fd) : 0;
        if (fd) fclose(fd);
        st_line(&o, got2 == sizeof(r) && memcmp(w, r, sizeof(w)) == 0,
                "真实 FAT 上存在且内容一致: %s", onfat);

        st_line(&o, unlink(onlpk) == 0, "删除 %s", onlpk);
        struct stat st2;
        st_line(&o, stat(onfat, &st2) != 0, "真实 FAT 上已删除: %s", onfat);
    }

    /* E. 句柄耗尽后释放可复用（PAL 最多同时开几个 MKF，这里压到上限验证不泄漏） */
    {
        char path[96];
        snprintf(path, sizeof(path), "%s/ABC.MKF", LPK_VFS_MNT);
        FILE *fs[LPK_M_MAXH + 4];
        int n = 0;
        for (n = 0; n < LPK_M_MAXH; n++) {
            fs[n] = fopen(path, "rb");
            if (!fs[n]) break;
        }
        st_line(&o, n == LPK_M_MAXH, "同时打开 %d/%d 个句柄", n, LPK_M_MAXH);
        st_line(&o, n < LPK_M_MAXH + 1 && fopen(path, "rb") == NULL,
                "句柄耗尽后第 %d 个被拒(不越界)", LPK_M_MAXH + 1);
        for (int i = 0; i < n; i++) fclose(fs[i]);
        FILE *again = fopen(path, "rb");
        st_line(&o, again != NULL, "释放后句柄可复用");
        if (again) fclose(again);
        st_line(&o, lpk_vfs_open_handles() == 0, "全部关闭后句柄数归零 (%d)",
                lpk_vfs_open_handles());
    }

    /* F. 错误路径 / 目录穿越 */
    {
        char bad1[96], bad2[96];
        snprintf(bad1, sizeof(bad1), "%s/../etc/passwd", LPK_VFS_MNT);
        snprintf(bad2, sizeof(bad2), "%s//ABC.MKF", LPK_VFS_MNT);
        FILE *f1 = fopen(bad1, "rb");
        FILE *f2 = fopen(bad2, "rb");
        st_line(&o, f1 == NULL, "目录穿越 ../ 被拒");
        st_line(&o, f2 == NULL, "空路径段 // 被拒");
        if (f1) fclose(f1);
        if (f2) fclose(f2);
    }

    /* G. 逐条 CRC（可选，慢）：这条能证明"读出来的每个字节都对" */
    if (full_crc) {
        int bad = 0, done = 0;
        uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(8192);
        if (buf) {
            for (int i = 0; i < s_v.m->h.loaded; i++) {
                const lpk_entry_t *e = &s_v.m->en[i];
                char p[200];
                snprintf(p, sizeof(p), "%s/%s", LPK_VFS_MNT, e->name);
                FILE *f = fopen(p, "rb");
                if (!f) { bad++; continue; }
                uint32_t crc = 0, left = e->size;
                bool ok = true;
                while (left) {
                    size_t want = left < 8192 ? left : 8192;
                    if (fread(buf, 1, want, f) != want) { ok = false; break; }
                    crc = lpk_crc32(crc, buf, (uint32_t)want);
                    left -= (uint32_t)want;
                }
                fclose(f);
                if (!ok || (e->crc32 && crc != e->crc32)) {
                    bad++;
                    if (bad <= 3) st_line(&o, false, "CRC 不符: %s (%08x vs %08x)",
                                          e->name, (unsigned)crc, (unsigned)e->crc32);
                }
                done++;
            }
            free(buf);
        } else {
            st_line(&o, false, "CRC 校验缓冲分配失败");
        }
        st_line(&o, bad == 0, "逐条 CRC: %d/%d 一致", done - bad, done);
    }

    if (o.p && o.len + 48 < o.cap) {
        o.len += (size_t)snprintf(o.p + o.len, o.cap - o.len,
                                  "SUMMARY %d pass / %d fail\r\n", o.passes, o.fails);
    }
    return o.fails;
}
