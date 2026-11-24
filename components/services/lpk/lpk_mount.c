/**
 * lpk_mount.c — 读写分流实现（规则见 lpk_mount.h 文件头，三条，没有例外）
 *
 * 设计上刻意**不做**的事：
 *   · 不做「LPK 里没有就回退到 FAT」—— 那会让盘上残留的同名文件顶掉包内资源；
 *   · 不把资源区的写请求偷偷改写到别的路径 —— 写不进就明确 EROFS，
 *     让引擎自己感知失败，而不是"看着写成功、重启就没了"；
 *   · 不在查找失败时分配字符串 —— 全程零 malloc（条目表由调用方提供）。
 */
#include "lpk_mount.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

/* 预读窗前置声明（定义在 lpk_m_read 前，但 lpk_unmount/lpk_m_close 要用） */
static void pre_alloc_wins(lpk_mnt_t *m, int hd);
static void pre_free_wins (lpk_mnt_t *m, int hd);

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

static bool starts_with(const char *s, const char *pfx) {
    while (*pfx) { if (*s++ != *pfx++) return false; }
    return true;
}

/* ---------------- 句柄表（并发分配用原子操作） ----------------
 * ★ 2026-11-01：真机上 open/close 由 VFS 的锁串行化，所以原本不会撞；
 *   但主机并发测试当场抓到"两个线程抢到同一个槽"——那会把两个资源的读写位置
 *   混在一起（症状是读到一半数据就错）。这里用 CAS 把槽位分配做成原子的，
 *   线程安全边界因此可以明确写成：
 *     · open / close：**可并发**（槽位分配与释放是原子的）
 *     · 同一个句柄的 read/write/seek：**不可并发**（与 POSIX 的 fread 一致）
 *     · unmount：必须没有任何任务在用（由 pal_shutdown 的顺序保证） */
static int hd_alloc(lpk_mnt_t *m) {
    for (int i = 0; i < LPK_M_MAXH; i++) {
        int zero = 0;
        if (__atomic_compare_exchange_n(&m->hd[i].used, &zero, 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return i;
        }
    }
    return -1;
}

static bool hd_ok(const lpk_mnt_t *m, int hd) {
    return hd >= 0 && hd < LPK_M_MAXH && __atomic_load_n(&m->hd[hd].used, __ATOMIC_ACQUIRE) != 0;
}

/* ---------------- 分类（唯一真相） ---------------- */

bool lpk_m_is_writable(const lpk_mnt_t *m, const char *rel) {
    if (!m || !rel) return false;
    if (!rel[0]) return false;                      /* 挂载根不是可写目标 */
    for (int i = 0; i < m->nwf; i++)
        if (strcmp(rel, m->wfile[i]) == 0) return true;
    for (int i = 0; i < m->nwd; i++) {
        size_t wn = strlen(m->wdir[i]);             /* "save/" 长度含斜杠 */
        if (starts_with(rel, m->wdir[i])) return true;             /* save/... */
        if (wn > 1 && strlen(rel) == wn - 1 &&
            strncmp(rel, m->wdir[i], wn - 1) == 0) return true;    /* 目录本身 "save" */
    }
    return false;
}

/* 把 rel 拼成后端绝对路径：base + "/" + rel。太长则返回 false。 */
static bool backend_path(const lpk_mnt_t *m, const char *rel, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", m->base, rel);
    return n > 0 && (size_t)n < cap;
}

/* 逐级 mkdir 父目录（"save/a/b.rpg" 会建 save、save/a）。
 * 忽略"已存在"的失败 —— 后端没有必要区分 EEXIST。 */
static void mkdirs_parent(const lpk_mnt_t *m, const char *rel) {
    if (!m->bk.mkdir || !m->base[0]) return;
    char buf[LPK_M_PATH];
    if (!backend_path(m, rel, buf, sizeof(buf))) return;
    char *p = buf + strlen(m->base);
    if (*p == '/') p++;
    for (; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        m->bk.mkdir(m->bk.ctx, buf);
        *p = '/';
    }
}

/* rel 是否指向一个"目录"（挂载根，或可写目录前缀本身，或它以 / 结尾） */
static bool looks_like_dir(const lpk_mnt_t *m, const char *rel) {
    if (!rel[0]) return true;
    size_t n = strlen(rel);
    if (rel[n - 1] == '/') return true;
    for (int i = 0; i < m->nwd; i++) {
        size_t wn = strlen(m->wdir[i]);              /* "save/" 去掉斜杠比较 */
        if (wn && wn - 1 == n && strncmp(rel, m->wdir[i], n) == 0) return true;
    }
    return false;
}

/* 包里**没有目录条目**（只有文件），所以"是不是目录"只能靠前缀推断：
 * 只要存在以 "rel/" 开头的资源，它就是目录。
 *
 * ★ 2026-11-01 必须补这一条：仙剑的 pal_music.c 用
 *     stat("<GamePath>/music", &st) 判断"有没有预渲染音乐"
 *   来决定走音乐模式还是音效模式。若这里不认目录，
 *   它会认为没有 music 目录 ⇒ 切到音效模式 ⇒ 画面正常、**音乐直接没了**。
 *   这类"目录能不能 stat 出来"的问题在引擎里到处都是，所以做在通用层。 */
static bool has_children(const lpk_mnt_t *m, const char *rel) {
    if (!m->en || !rel[0]) return false;
    size_t n = strlen(rel);
    for (int i = 0; i < m->h.loaded; i++) {
        if (strncmp(m->en[i].name, rel, n) == 0 && m->en[i].name[n] == '/') return true;
    }
    return false;
}

/* ---------------- 挂载 ---------------- */

int lpk_mount(lpk_mnt_t *m, const char *lpk_path, lpk_entry_t *entries, int max_entries,
              const char *base_dir, const lpk_backing_t *bk) {
    if (!m || !lpk_path || !bk) return LPK_E_ARG;
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < LPK_M_MAXH; i++) m->hd[i].fid = -1;
    m->en = entries;
    m->bk = *bk;
    /* 预读窗默认值（调用方可用 lpk_m_set_prefetch 覆盖）：
     *   64 KB/窗 × 4 窗/句柄 = 256 KB/句柄；总预算 4 MB（= 16 句柄全开也放得下）。
     * PSRAM 现在空闲 7.3 MB，留足余量；实际同时开 3~5 个句柄 ≈ 1 MB。
     * 默认分配器是 malloc（主机测试友好）；设备侧由 lpk_vfs.c 注入 heap_caps_malloc，
     * 保证这块内存**只从 PSRAM 出，绝不吃内部 RAM**。 */
    m->pre_wsz     = 64u * 1024u;
    m->pre_budget  = 4u * 1024u * 1024u;
    m->pre_total   = 0;
    m->pre_tick    = 0;
    copy_str(m->lpk_path, sizeof(m->lpk_path), lpk_path);
    copy_str(m->base, sizeof(m->base), base_dir ? base_dir : "");

    int rc = lpk_open(&m->h, lpk_path, entries, max_entries);
    if (rc != LPK_OK) { memset(m, 0, sizeof(*m)); return rc; }
    return LPK_OK;
}

void lpk_unmount(lpk_mnt_t *m) {
    if (!m) return;
    for (int i = 0; i < LPK_M_MAXH; i++) {
        if (m->hd[i].used && m->hd[i].kind == 1 && m->bk.close) m->bk.close(m->bk.ctx, m->hd[i].fid);
        pre_free_wins(m, i);
        if (m->hd[i].fp) { fclose(m->hd[i].fp); m->hd[i].fp = NULL; }
        __atomic_store_n(&m->hd[i].used, 0, __ATOMIC_RELEASE);
    }
    if (m->h.fp) lpk_close(&m->h);
    memset(m, 0, sizeof(*m));
}

void lpk_m_set_prefetch(lpk_mnt_t *m, unsigned long wsz, unsigned long budget,
                        void *(*alloc)(unsigned long, void *), void (*free_)(void *, void *),
                        void *ctx) {
    if (!m) return;
    m->pre_wsz    = wsz;
    m->pre_budget = budget;
    m->pre_alloc  = alloc;
    m->pre_free   = free_;
    m->pre_ctx    = ctx;
}

void lpk_m_drop_prefetch(lpk_mnt_t *m) {
    if (!m) return;
    for (int i = 0; i < LPK_M_MAXH; i++) pre_free_wins(m, i);
    m->pre_total = 0;
}

void lpk_m_prefetch_stat(const lpk_mnt_t *m, unsigned long *bytes, int *handles) {
    unsigned long b = 0; int n = 0;
    if (m) for (int i = 0; i < LPK_M_MAXH; i++)
        if (m->hd[i].pre) { b += m->hd[i].pre_cap; n++; }
    if (bytes)   *bytes = b;
    if (handles) *handles = n;
}

int lpk_mount_add_writable_dir(lpk_mnt_t *m, const char *dir) {
    if (!m || !dir || !dir[0] || m->nwd >= LPK_M_MAXW) return LPK_E_ARG;
    char tmp[LPK_MAX_NAME];
    copy_str(tmp, sizeof(tmp), dir);
    size_t n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == '/') tmp[--n] = 0;      /* 先去掉结尾斜杠 */
    if (n == 0 || n + 2 >= sizeof(tmp)) return LPK_E_ARG;
    /* lpk_path_ok 要求"不以 / 结尾"，所以**验不带斜杠的形式**，再补回来 */
    if (!lpk_path_ok(tmp)) return LPK_E_PATH;
    tmp[n] = '/'; tmp[n + 1] = 0;
    copy_str(m->wdir[m->nwd], sizeof(m->wdir[0]), tmp);
    m->nwd++;
    return LPK_OK;
}

int lpk_mount_add_writable_file(lpk_mnt_t *m, const char *file) {
    if (!m || !file || !file[0] || m->nwf >= LPK_M_MAXW) return LPK_E_ARG;
    if (!lpk_path_ok(file)) return LPK_E_PATH;
    copy_str(m->wfile[m->nwf], sizeof(m->wfile[0]), file);
    m->nwf++;
    return LPK_OK;
}

/* ---------------- open ---------------- */

int lpk_m_open(lpk_mnt_t *m, const char *rel, int flags) {
    if (!m || !rel) return LPK_E_ARG;
    if (!lpk_path_ok(rel)) return LPK_E_PATH;

    /* ★ 2026-11-01: 槽位**最后才占**。
     *   改成原子 CAS 之后，先占槽再因参数/文件不存在而 return，会把槽位漏掉
     *   （16 个慢慢漏完，之后所有 open 都报 NOMEM —— 主机测试当场抓到）。
     *   这里把所有失败路径都放在 hd_alloc 之前，成功后占槽不再有失败可能。 */

    if (lpk_m_is_writable(m, rel)) {
        /* ① 可写路径：读写都落到真实 FAT（读也要走它，才读得到自己写的东西） */
        if (!m->base[0] || !m->bk.open) return LPK_E_ROFS;
        char full[LPK_M_PATH];
        if (!backend_path(m, rel, full, sizeof(full))) return LPK_E_PATH;
        if (flags & (LPK_O_CREAT | LPK_O_WRONLY | LPK_O_RDWR)) mkdirs_parent(m, rel);
        int fid = m->bk.open(m->bk.ctx, full, flags);
        if (fid < 0) return LPK_E_NOENT;

        int hd = hd_alloc(m);
        if (hd < 0) { m->bk.close(m->bk.ctx, fid); return LPK_E_NOMEM; }
        m->hd[hd].kind = 1; m->hd[hd].fid = fid;
        m->hd[hd].pos = 0;  m->hd[hd].size = 0;
        copy_str(m->hd[hd].name, sizeof(m->hd[hd].name), rel);
        /* 拿一下大小，lseek(SEEK_END) 与 fstat 要用 */
        lpk_m_stat_t st;
        if (m->bk.stat && m->bk.stat(m->bk.ctx, full, &st) == 0) m->hd[hd].size = st.size;
        return hd;
    }

    /* ② 资源路径：只读。任何写意图都不落到包上，也不偷偷改写到别处。 */
    if (flags & (LPK_O_WRONLY | LPK_O_RDWR | LPK_O_CREAT | LPK_O_TRUNC)) return LPK_E_ROFS;

    /* 目录不能当文件打开（music/ 这类在包里只有子文件） */
    if (has_children(m, rel)) return LPK_E_ISDIR;

    int idx = lpk_find_ci(&m->h, m->en, rel);
    if (idx == LPK_E_AMBIG) return LPK_E_AMBIG;
    if (idx < 0)            return LPK_E_NOENT;      /* ③ 不跨类回退到 FAT */
    if (m->en[idx].size == 0) return LPK_E_NOENT;

    /* ★★★ 2026-11-01 关键修正: **每个句柄开自己的 FILE***。
     *   原来全包共用一个 FILE*，因为 fseek+fread 两步必须原子，
     *   每次读都得持全局互斥锁 —— 读一个 1.5 MB 资源就锁 3 秒多，
     *   游戏自己的下一次读、音频任务的读全在排队 ⇒ 载入要一分钟、声音也断。
     *   改成每句柄独立 FILE* 后：各自有独立读写位置，读的时候**不需要锁**，
     *   只在 FatFs 那一层按扇区交错，音频能插进来。
     *   代价: 每个打开的资源多一个 newlib FILE(约 1 KB, 含 1 KB 缓冲) + 一个 FAT 句柄。
     *   上限 LPK_M_MAXH=16 ⇒ 峰值约 16 KB 内部 RAM（实测内部余量 17 万字节，够）。 */
    FILE *pf = fopen(m->lpk_path, "rb");
    if (!pf) return LPK_E_OPEN;

    int hd = hd_alloc(m);
    if (hd < 0) { fclose(pf); return LPK_E_NOMEM; }
    m->hd[hd].kind = 0; m->hd[hd].idx = idx;
    m->hd[hd].fid = -1; m->hd[hd].pos = 0; m->hd[hd].size = m->en[idx].size;
    m->hd[hd].fp = pf;
    copy_str(m->hd[hd].name, sizeof(m->hd[hd].name), m->en[idx].name);
    return hd;
}

/* ---------------- 外部缓冲（PSRAM 整资源缓存） ---------------- */

int lpk_m_open_buf(lpk_mnt_t *m, const char *name, const void *buf, unsigned long size) {
    if (!m || !name || !buf || size == 0) return LPK_E_ARG;
    int hd = hd_alloc(m);
    if (hd < 0) return LPK_E_NOMEM;
    m->hd[hd].kind = 2;                        /* used 已由 hd_alloc 原子置 1 */
    m->hd[hd].idx  = -1;
    m->hd[hd].fid  = -1;
    m->hd[hd].pos  = 0;
    m->hd[hd].size = size;
    m->hd[hd].buf  = (const uint8_t *)buf;
    copy_str(m->hd[hd].name, sizeof(m->hd[hd].name), name);
    return hd;
}

/* ---------------- read / write / seek / close ---------------- */

/* ---------------- 预读窗：分配 / 释放 / 取用 ---------------- */

/* 为本句柄分配预读窗（整块连续，LPK_M_PREWINS 个 pre_wsz）。
 * 分配失败 **不是错误** —— 只是退化为逐次直读（功能不变，性能降级）。 */
static void pre_alloc_wins(lpk_mnt_t *m, int hd) {
    if (m->hd[hd].pre || m->pre_wsz == 0) return;
    const unsigned long wsz = m->pre_wsz;
    unsigned long total;
    if (m->hd[hd].size <= wsz)                                  total = wsz;
    else if (m->hd[hd].size <= wsz * LPK_M_PREWINS)              total = ((m->hd[hd].size + wsz - 1) / wsz) * wsz;
    else                                                        total = wsz * LPK_M_PREWINS;
    if (m->pre_budget && m->pre_total + total > m->pre_budget) return;   /* 超预算：不装窗 */

    void *p = m->pre_alloc ? m->pre_alloc(total, m->pre_ctx) : malloc(total);
    if (!p) return;
    m->hd[hd].pre     = (uint8_t *)p;
    m->hd[hd].pre_cap = total;
    for (int i = 0; i < LPK_M_PREWINS; i++) { m->hd[hd].pw[i].off = 0; m->hd[hd].pw[i].len = 0; m->hd[hd].pw[i].tick = 0; }
    m->pre_total += total;
}

static void pre_free_wins(lpk_mnt_t *m, int hd) {
    if (!m->hd[hd].pre) return;
    if (m->pre_free) m->pre_free(m->hd[hd].pre, m->pre_ctx);
    else             free(m->hd[hd].pre);
    if (m->pre_total >= m->hd[hd].pre_cap) m->pre_total -= m->hd[hd].pre_cap;
    else                                   m->pre_total = 0;
    m->hd[hd].pre     = NULL;
    m->hd[hd].pre_cap = 0;
    for (int i = 0; i < LPK_M_PREWINS; i++) { m->hd[hd].pw[i].off = 0; m->hd[hd].pw[i].len = 0; m->hd[hd].pw[i].tick = 0; }
}

int lpk_m_read(lpk_mnt_t *m, int hd, void *buf, size_t n) {
    if (!hd_ok(m, hd) || !buf) return LPK_E_ARG;
    if (n == 0) return 0;
    if (m->hd[hd].kind == 1) {
        if (!m->bk.read) return LPK_E_IO;
        return m->bk.read(m->bk.ctx, m->hd[hd].fid, buf, n);
    }
    if (m->hd[hd].kind == 2) {                        /* PSRAM 缓冲：纯 memcpy，不碰 SD */
        long left = (long)m->hd[hd].size - m->hd[hd].pos;
        if (left <= 0) return 0;
        if ((long)n > left) n = (size_t)left;
        memcpy(buf, m->hd[hd].buf + m->hd[hd].pos, n);
        m->hd[hd].pos += (long)n;
        return (int)n;
    }
    long left = (long)m->hd[hd].size - m->hd[hd].pos;
    if (left <= 0) return 0;                          /* EOF: 与 fread 一致, 返回 0 而不是错误 */
    if ((long)n > left) n = (size_t)left;
    /* 用**本句柄的**文件句柄读：不需要全局锁，别的任务同时读别的资源互不阻塞。 */
    if (!m->hd[hd].fp) return LPK_E_IO;
    const lpk_entry_t *e = &m->en[m->hd[hd].idx];
    unsigned long pos = (unsigned long)m->hd[hd].pos;

    if (m->pre_wsz) {
        pre_alloc_wins(m, hd);
        if (m->hd[hd].pre) {
            const int nwin = (int)(m->hd[hd].pre_cap / m->pre_wsz);

            /* ① 命中任一窗 ⇒ 纯 memcpy，一次都不碰 SD */
            for (int i = 0; i < nwin && i < LPK_M_PREWINS; i++) {
                if (m->hd[hd].pw[i].len &&
                    pos >= m->hd[hd].pw[i].off &&
                    pos + n <= m->hd[hd].pw[i].off + m->hd[hd].pw[i].len) {
                    m->hd[hd].pw[i].tick = ++m->pre_tick;
                    memcpy(buf, m->hd[hd].pre + (unsigned long)i * m->pre_wsz
                                + (pos - m->hd[hd].pw[i].off), n);
                    m->hd[hd].pos += (long)n;
                    return (int)n;
                }
            }

            /* ② 未命中且请求比一个窗小 ⇒ 建窗（一次顺序读一整窗）
             *    引擎是「索引区 ↔ 数据区」来回跳，4 个窗正好各留位置，
             *    所以这里淘汰的是最久没用的那个，不会把热点窗踢掉。 */
            if (n <= m->pre_wsz) {
                int slot = 0, empty = -1;
                unsigned int best = 0xFFFFFFFFu;
                for (int i = 0; i < nwin && i < LPK_M_PREWINS; i++) {
                    if (m->hd[hd].pw[i].len == 0) { empty = i; break; }
                    if (m->hd[hd].pw[i].tick < best) { best = m->hd[hd].pw[i].tick; slot = i; }
                }
                if (empty >= 0) slot = empty;

                unsigned long len = m->hd[hd].size - pos;
                if (len > m->pre_wsz) len = m->pre_wsz;
                uint8_t *dst = m->hd[hd].pre + (unsigned long)slot * m->pre_wsz;
                if (fseek(m->hd[hd].fp, (long)(e->off + (uint32_t)pos), SEEK_SET) == 0 &&
                    fread(dst, 1, len, m->hd[hd].fp) == len) {
                    m->hd[hd].pw[slot].off  = pos;
                    m->hd[hd].pw[slot].len  = len;
                    m->hd[hd].pw[slot].tick = ++m->pre_tick;
                    memcpy(buf, dst, n);
                    m->hd[hd].pos += (long)n;
                    return (int)n;
                }
                m->hd[hd].pw[slot].len = 0;      /* 建窗失败：作废该槽，走直读 */
            }
        }
    }

    /* ③ 直读：请求大于一个窗 / 没窗 / 建窗失败 */
    if (fseek(m->hd[hd].fp, (long)(e->off + (uint32_t)pos), SEEK_SET) != 0) return LPK_E_IO;
    if (fread(buf, 1, n, m->hd[hd].fp) != n) return LPK_E_IO;
    m->hd[hd].pos += (long)n;

    /* 直读过的地方如果正好被某个窗覆盖，把窗作废，免得读到陈旧的（这里是同一文件，不会陈旧）
     * —— 但 pos 前进后窗口仍有效，保留即可，无需处理。 */
    return (int)n;
}

int lpk_m_write(lpk_mnt_t *m, int hd, const void *buf, size_t n) {
    if (!hd_ok(m, hd) || !buf) return LPK_E_ARG;
    if (m->hd[hd].kind != 1) return LPK_E_ROFS;        /* 资源永不接受写入 */
    if (!m->bk.write) return LPK_E_IO;
    int w = m->bk.write(m->bk.ctx, m->hd[hd].fid, buf, n);
    if (w > 0) {
        m->hd[hd].pos += w;
        if ((unsigned long)m->hd[hd].pos > m->hd[hd].size) m->hd[hd].size = (unsigned long)m->hd[hd].pos;
    }
    return w;
}

long lpk_m_seek(lpk_mnt_t *m, int hd, long off, int whence) {
    if (!hd_ok(m, hd)) return LPK_E_ARG;
    long base = (whence == LPK_SEEK_SET) ? 0
              : (whence == LPK_SEEK_CUR) ? m->hd[hd].pos
              : (long)m->hd[hd].size;
    long np = base + off;
    if (np < 0 || (unsigned long)np > m->hd[hd].size) return LPK_E_ARG;   /* 越界不走 */
    if (m->hd[hd].kind == 1 && m->bk.lseek) {
        if (m->bk.lseek(m->bk.ctx, m->hd[hd].fid, np, LPK_SEEK_SET) < 0) return LPK_E_IO;
    }
    m->hd[hd].pos = np;
    return np;
}

int lpk_m_close(lpk_mnt_t *m, int hd) {
    if (!m || hd < 0 || hd >= LPK_M_MAXH) return LPK_E_ARG;
    /* 原子"认领"这个槽：只有抢到 1→0 的那一次关闭生效，重复关闭返回 E_ARG */
    if (__atomic_exchange_n(&m->hd[hd].used, 0, __ATOMIC_ACQ_REL) == 0) return LPK_E_ARG;
    if (m->hd[hd].kind == 1 && m->bk.close) m->bk.close(m->bk.ctx, m->hd[hd].fid);
    /* kind=2: 缓冲归调用方（缓存池）所有，这里只还句柄槽 */
    pre_free_wins(m, hd);                      /* 预读窗归本句柄，先还再清结构 */
    if (m->hd[hd].fp) { fclose(m->hd[hd].fp); m->hd[hd].fp = NULL; }
    memset(&m->hd[hd], 0, sizeof(m->hd[hd]));
    m->hd[hd].fid = -1;
    return LPK_OK;
}

/* ---------------- 元数据 ---------------- */

int lpk_m_stat(lpk_mnt_t *m, const char *rel, lpk_m_stat_t *st) {
    if (!m || !rel || !st) return LPK_E_ARG;
    if (!rel[0]) { st->is_dir = 1; st->size = 0; return LPK_OK; }   /* 挂载根 */

    if (lpk_m_is_writable(m, rel)) {
        if (!m->base[0] || !m->bk.stat) return LPK_E_NOENT;
        char full[LPK_M_PATH];
        if (!backend_path(m, rel, full, sizeof(full))) return LPK_E_PATH;
        if (m->bk.stat(m->bk.ctx, full, st) != 0) {
            /* 还没建过（首次存档 / 首次写配置）—— 返回 NOENT，
             * 让引擎走"新建"分支，而不是看到一个 0 字节的假文件。 */
            return LPK_E_NOENT;
        }
        return LPK_OK;
    }

    if (looks_like_dir(m, rel)) { st->is_dir = 1; st->size = 0; return LPK_OK; }
    /* 包里没有目录条目 ⇒ 用"有没有子文件"推断（pal_music 判断 music 目录就靠这个） */
    if (has_children(m, rel)) { st->is_dir = 1; st->size = 0; return LPK_OK; }
    int idx = lpk_find_ci(&m->h, m->en, rel);
    if (idx == LPK_E_AMBIG) return LPK_E_AMBIG;
    if (idx < 0) return LPK_E_NOENT;
    st->is_dir = 0;
    st->size = m->en[idx].size;
    return LPK_OK;
}

int lpk_m_access(lpk_mnt_t *m, const char *rel) {
    if (!m || !rel) return LPK_E_ARG;
    if (!rel[0]) return LPK_OK;                       /* 挂载根永远存在 */
    lpk_m_stat_t st;
    return (lpk_m_stat(m, rel, &st) == LPK_OK) ? LPK_OK : LPK_E_NOENT;
}

int lpk_m_mkdir(lpk_mnt_t *m, const char *rel) {
    if (!m || !rel || !lpk_path_ok(rel)) return LPK_E_PATH;
    if (!lpk_m_is_writable(m, rel)) return LPK_E_ROFS;   /* 不往包里建目录 */
    if (!m->base[0] || !m->bk.mkdir) return LPK_E_ROFS;
    char full[LPK_M_PATH];
    if (!backend_path(m, rel, full, sizeof(full))) return LPK_E_PATH;
    mkdirs_parent(m, rel);
    /* 后端 mkdir 失败时不要急着报错：目录已存在也会"失败"（EEXIST/FR_EXIST），
     * 而这对调用方是**成功**。所以补一次 stat 确认它是目录。 */
    if (m->bk.mkdir(m->bk.ctx, full) == 0) return LPK_OK;
    lpk_m_stat_t st;
    if (m->bk.stat && m->bk.stat(m->bk.ctx, full, &st) == 0 && st.is_dir) return LPK_OK;
    return LPK_E_IO;
}

int lpk_m_unlink(lpk_mnt_t *m, const char *rel) {
    if (!m || !rel || !lpk_path_ok(rel)) return LPK_E_PATH;
    if (!lpk_m_is_writable(m, rel)) return LPK_E_ROFS;   /* 资源不可删 */
    if (!m->base[0] || !m->bk.unlink) return LPK_E_ROFS;
    char full[LPK_M_PATH];
    if (!backend_path(m, rel, full, sizeof(full))) return LPK_E_PATH;
    return (m->bk.unlink(m->bk.ctx, full) == 0) ? LPK_OK : LPK_E_NOENT;
}

int lpk_m_rename(lpk_mnt_t *m, const char *from, const char *to) {
    if (!m || !from || !to) return LPK_E_ARG;
    if (!lpk_path_ok(from) || !lpk_path_ok(to)) return LPK_E_PATH;
    /* 两端都必须在可写区：不允许把资源"改名"进包，也不允许把存档改名成资源名 */
    if (!lpk_m_is_writable(m, from) || !lpk_m_is_writable(m, to)) return LPK_E_ROFS;
    if (!m->base[0] || !m->bk.rename) return LPK_E_ROFS;
    char a[LPK_M_PATH], b[LPK_M_PATH];
    if (!backend_path(m, from, a, sizeof(a)) || !backend_path(m, to, b, sizeof(b))) return LPK_E_PATH;
    mkdirs_parent(m, to);
    return (m->bk.rename(m->bk.ctx, a, b) == 0) ? LPK_OK : LPK_E_IO;
}
