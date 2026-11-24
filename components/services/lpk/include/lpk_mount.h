/**
 * lpk_mount.h — LPK 只读资源 + 真实 FAT 存档/配置的**读写分流挂载层**
 *
 * 为什么需要这一层
 * ----------------
 * 仙剑这类引擎既读资源也写存档：
 *   · 资源（ABC.MKF、music 子目录下的 mp3 ...）应当从 **LPK 里读**：一个文件装完，不解压；
 *   · 存档（save 目录下的 rpg）、配置（sdlpal.cfg）必须写到 **真实 FAT**，否则
 *     「写成功、重启就没」——写进只读包是假成功，比失败更糟。
 *
 * 所以判定**绝不能**是「先查包里有没有，没有就回退到盘上」——那会让盘上一个
 * 残留的同名文件顶掉包内资源（资源混用），也正是之前踩过的坑。
 * 这里按**路径分类**硬分流，三条规则，没有例外：
 *
 *   ① 可写路径（显式登记的目录前缀 / 单文件）→ **只走真实 FAT**（读写都走）
 *   ② 其余路径（= 游戏资源）             → **只从 LPK 读**；任何写请求返回 LPK_E_ROFS
 *   ③ 两边都不命中                        → LPK_E_NOENT（不跨类回退）
 *
 * 设备侧与主机侧共用同一份分类逻辑
 * --------------------------------
 * 真正的文件 I/O 由调用方通过 `lpk_backing_t` 注入：
 *   · 设备侧：ESP-IDF VFS 的 open/read/write/lseek/close/stat/mkdir 包装
 *             （挂载点 /lpk，见 docs/规范/LPK自包含资源包格式.md）
 *   · 主机侧：POSIX 同名函数包装（tests/lpk/lpk_mount_test.c）
 * 这样「分流规则」在本机能被完整单测，而不需要在 PC 上伪造一整套 ESP-IDF VFS。
 */
#ifndef LPK_MOUNT_H
#define LPK_MOUNT_H

#include <stdbool.h>
#include <stdint.h>
#include "lpk1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LPK_M_PATH      160       /* 后端绝对路径上限 */
#define LPK_M_MAXW      4         /* 可写目录/文件各最多 4 条 */
/* 同时打开的文件句柄上限。
 * 8 → 16（2026-11-01）：仙剑实际会同时开 mus.mkf(音乐) + voc.mkf(音效) +
 * 若干资源（PAL_LoadResources 期间一个接一个），再叠上引擎自己的临时文件，
 * 8 太贴身；句柄槽约 224 B/个，16 个 ≈ 3.6 KB，整块挂载体放 PSRAM，不吃内部 RAM。 */
#define LPK_M_MAXH      16
#define LPK_M_PREWINS   4         /* 每句柄预读窗个数（索引窗 + 数据窗 + 余量） */

/* 打开标志。
 * 取值刻意采用 **newlib / BSD 的位布局**（O_APPEND=0x8, O_CREAT=0x200, O_TRUNC=0x400）：
 *   · ESP-IDF 的 VFS 用的就是 newlib 这套 ⇒ 设备侧适配层可以原样透传；
 *   · macOS/Linux 主机的 fcntl.h 也是这套 ⇒ 主机测试同样零翻译；
 * 这样就不存在"两套标志位没对上、写模式被判成只读"这类隐蔽错。 */
#define LPK_O_RDONLY    0x0000
#define LPK_O_WRONLY    0x0001
#define LPK_O_RDWR      0x0002
#define LPK_O_ACCMODE   0x0003
#define LPK_O_APPEND    0x0008
#define LPK_O_CREAT     0x0200
#define LPK_O_TRUNC     0x0400

#define LPK_SEEK_SET    0
#define LPK_SEEK_CUR    1
#define LPK_SEEK_END    2

typedef struct {
    int          is_dir;
    unsigned long size;
} lpk_m_stat_t;

/* 后端（真实 FAT）操作表。返回 0 / 非负 = 成功，负 = 失败（errno 语义由实现决定）。 */
typedef struct {
    void *ctx;
    int  (*open )(void *ctx, const char *path, int flags);
    int  (*read )(void *ctx, int fid, void *buf, size_t n);
    int  (*write)(void *ctx, int fid, const void *buf, size_t n);
    long (*lseek)(void *ctx, int fid, long off, int whence);
    int  (*close)(void *ctx, int fid);
    int  (*stat )(void *ctx, const char *path, lpk_m_stat_t *st);
    int  (*mkdir)(void *ctx, const char *path);
    int  (*unlink)(void *ctx, const char *path);
    int  (*rename)(void *ctx, const char *from, const char *to);
} lpk_backing_t;

typedef struct {
    lpk_t         h;
    lpk_entry_t  *en;
    lpk_backing_t bk;
    char          lpk_path[LPK_M_PATH];   /* 只用于日志/诊断 */
    char          base[LPK_M_PATH];       /* 可写旁路的根目录（真实 FAT 上） */
    char          wdir [LPK_M_MAXW][LPK_MAX_NAME];  /* "save/" */
    int           nwd;
    char          wfile[LPK_M_MAXW][LPK_MAX_NAME];  /* "sdlpal.cfg" */
    int           nwf;
    /* ---- 顺序预读窗口（2026-11-01）----
     * 为什么需要：LPK / ISO 是 FAT 上**一个大文件**（41.7 MB ≈ 1300+ 簇），
     * 而目录模式是 109 个小文件（每个几十簇）。引擎的读法是
     *     fseek(某偏移) → fread(几 KB) → fseek(另一偏移) → fread(几 KB)
     * 每次 fseek 都要沿 FAT 簇链走到目标簇 ⇒ 距离放大几十倍 ⇒ 实测慢 6 倍
     * （STAGE2→3：目录 6.5 s / LPK 37.5 s，同一批资源）。
     *
     * 对策：**每个句柄 4 个 64 KB 预读窗（LRU）**。
     * ⚠ 为什么不是"一个大窗"：引擎读一个 chunk 的序列是
     *      fseek(4*chunkNum) → fread(8B) → fseek(offset) → fread(chunk)
     *   即**每读一个 chunk 就在「索引区 ↔ 数据区」跨整个文件来回跳两次**。
     *   单窗只会在每次跳转时重建（每次多读一整窗，反而更慢）。
     *   4 个窗可以让"索引窗"常驻 + "数据窗"跟着 chunk 走，来回跳都命中。
     *   64 KB 的依据：索引项 4 B/条 ⇒ 一个索引窗覆盖 16384 个 chunk；
     *                 数据 chunk 平均几 KB ⇒ 一个数据窗覆盖多个连续 chunk。
     *
     * 约束：
     *   · 缓冲一律从**外部 RAM(PSRAM)** 分配（内部 RAM 只剩 100 KB 左右，绝不占用）；
     *   · 有总预算（默认 4 MB），超了就不给新句柄分配，退化为直读，不影响正确性；
     *   · 分配失败 = 退化为直读，**不报错**（性能降级，不是功能故障）。
     * 分配器由调用方注入（主机测试不注入 ⇒ 用 malloc，行为一致）。 */
    void *(*pre_alloc)(unsigned long size, void *ctx);
    void  (*pre_free )(void *p, void *ctx);
    void         *pre_ctx;
    unsigned long pre_wsz;        /* 单个窗口大小（默认 64 KB），0 = 关闭预读 */
    unsigned long pre_budget;     /* 所有窗口的总预算字节 */
    unsigned long pre_total;      /* 已分配字节 */
    unsigned int  pre_tick;       /* LRU 计时（每次命中/建窗 +1） */

    struct {
        int          used;
        int          kind;      /* 0 = LPK 资源, 1 = 真实 FAT, 2 = 外部缓冲(PSRAM 缓存) */
        int          idx;       /* LPK: 条目下标 */
        int          fid;       /* FAT: 后端句柄 */
        long         pos;
        unsigned long size;
        const uint8_t *buf;     /* kind=2: 调用方提供的只读缓冲（本层不释放） */
        FILE        *fp;        /* kind=0: **本句柄自己的**文件句柄（2026-11-01 改） */
        /* 预读窗（只对 kind=0 有效）。pre 是一整块：LPK_PRE_WINS 个 pre_wsz 连续存放，
         * 归本句柄所有，close 时释放。 */
        uint8_t     *pre;
        unsigned long pre_cap;  /* 本句柄实际分配到的总字节（= N * pre_wsz） */
        struct { unsigned long off, len; unsigned int tick; } pw[4];
        char         name[LPK_MAX_NAME];
    } hd[LPK_M_MAXH];
} lpk_mnt_t;

/* 挂载：打开 LPK + 记录后端。成功返回 LPK_OK。
 * base_dir 是「可写旁路」的根目录（真实 FAT 上，例如 /sdcard/apps/app/pal_data）；
 * 传 NULL/空串 = 不做写旁路（任何写请求都会被拒）。 */
int  lpk_mount(lpk_mnt_t *m, const char *lpk_path, lpk_entry_t *entries, int max_entries,
               const char *base_dir, const lpk_backing_t *bk);
void lpk_unmount(lpk_mnt_t *m);

/* 配置预读窗口。
 *   wsz    : 单个窗口字节数（建议 32~128 KB；0 = 关闭预读，回到逐次直读）
 *   budget : 所有窗口合计上限（建议 ≤ PSRAM 空闲的一半）
 *   alloc/free: 分配器；传 NULL 用 malloc/free（主机测试）。
 * 必须在 lpk_mount() 之后、首次 open 之前调用；也可运行期改 cap（新句柄生效）。 */
void lpk_m_set_prefetch(lpk_mnt_t *m, unsigned long wsz, unsigned long budget,
                        void *(*alloc)(unsigned long, void *), void (*free_)(void *, void *),
                        void *ctx);

/* 释放所有句柄的预读窗（运行期关预读 / 卸载前清理用）。之后新读走直读。 */
void lpk_m_drop_prefetch(lpk_mnt_t *m);

/* 诊断：当前预读窗口占用的总字节 / 已启用窗口的句柄数 */
void lpk_m_prefetch_stat(const lpk_mnt_t *m, unsigned long *bytes, int *handles);

/* 登记可写路径。dir 形如 "save/"（必须带结尾斜杠），file 形如 "sdlpal.cfg"。 */
int  lpk_mount_add_writable_dir (lpk_mnt_t *m, const char *dir);
int  lpk_mount_add_writable_file(lpk_mnt_t *m, const char *file);

/* 该相对路径会写到真实 FAT 吗？（诊断/测试用） */
bool lpk_m_is_writable(const lpk_mnt_t *m, const char *rel);

/* ---- 文件操作（rel 是**相对挂载点**的路径, 如 "save/0.rpg"）---- */
int  lpk_m_open (lpk_mnt_t *m, const char *rel, int flags);

/* 以**调用方给的只读缓冲**打开（2026-11-01：给 PSRAM 整资源缓存用）。
 * 之后 read/seek/size 都在这块内存里做，一次都不碰 SD。
 * 缓冲生命周期由调用方管：close 只归还句柄槽，不 free(buf)。
 * name 只用于诊断/日志；buf/size 必须与包里该资源一致（调用方负责核对）。 */
int  lpk_m_open_buf(lpk_mnt_t *m, const char *name, const void *buf, unsigned long size);
int  lpk_m_read (lpk_mnt_t *m, int hd, void *buf, size_t n);
int  lpk_m_write(lpk_mnt_t *m, int hd, const void *buf, size_t n);
long lpk_m_seek (lpk_mnt_t *m, int hd, long off, int whence);
int  lpk_m_close(lpk_mnt_t *m, int hd);

/* 元数据 / 目录 / 删除 / 改名（PAL 的 access()、stat()、scandir 都走这里） */
int  lpk_m_stat  (lpk_mnt_t *m, const char *rel, lpk_m_stat_t *st);
int  lpk_m_access(lpk_mnt_t *m, const char *rel);
int  lpk_m_mkdir (lpk_mnt_t *m, const char *rel);
int  lpk_m_unlink(lpk_mnt_t *m, const char *rel);
int  lpk_m_rename(lpk_mnt_t *m, const char *from, const char *to);

#ifdef __cplusplus
}
#endif

#endif /* LPK_MOUNT_H */
