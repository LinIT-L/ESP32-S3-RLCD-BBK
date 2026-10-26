/**
 * cheater.c — 修改机内存搜索核心实现 (单会话, PSRAM 候选池).
 *
 * 只通过回调 peek/poke 访问引擎给定的 [base, base+size) 区域.
 * 16 位值按小端 (低字节在前), 候选地址区内相对偏移.
 *
 * 2026-10-25 强化 (参照 烧饼修改器 / 八门神器):
 *   ① 锁定表: 地址加入后由 cheater_lock_tick() 每帧写回, 值不再被游戏改动;
 *   ② 撤销: 每次过滤前快照候选集, cheater_undo() 回到上一步 (误选"变大/变小"时可退);
 *   ③ 性能: 首轮扫描把 read 次数从 2/字节 降到 1/字节 (滚动复用上一字节),
 *      并记录耗时供界面回显;
 *   ④ 轮次: cheater_round() 供界面显示"第 N 轮", 过滤时据此提示.
 */
#include "cheater.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <string.h>

/* 候选池: PSRAM 静态 (单会话, 两个 8192×2B = 32KB). 扫描最坏 32KB 全匹配时也够. */
#define CHEAT_STATIC_CAND CHEAT_MAX_CAND
EXT_RAM_BSS_ATTR static uint16_t s_cand[CHEAT_STATIC_CAND];
EXT_RAM_BSS_ATTR static uint16_t s_ref[CHEAT_STATIC_CAND];
/* 每候选自动识别的位宽 (CHEAT_BITS_8 / CHEAT_BITS_16).
 * 开源作弊工具惯例: 同一帧里同时按 8 位与 16 位匹配, 值存成哪个宽度都能搜到. */
EXT_RAM_BSS_ATTR static uint8_t s_wid[CHEAT_STATIC_CAND];

/* 撤销备份: 只留一层 (过滤前的候选集). 40KB PSRAM, 换"错了能退一步". */
EXT_RAM_BSS_ATTR static uint16_t s_bak_cand[CHEAT_STATIC_CAND];
EXT_RAM_BSS_ATTR static uint16_t s_bak_ref[CHEAT_STATIC_CAND];
EXT_RAM_BSS_ATTR static uint8_t  s_bak_wid[CHEAT_STATIC_CAND];
static int s_bak_count = -1;        /* -1 = 无可撤销 */

static int s_count = -1;            /* -1 = 尚未搜索 */
static int s_round = 0;
static int s_scan_ms = 0;
static cheater_region_t s_reg;
static cheater_bits_t s_bits = CHEAT_BITS_8;

/* 锁定表 (内部 RAM 常驻, 24×6 = 144 字节) */
typedef struct {
    uint16_t off;      /* 区域内相对偏移 */
    uint16_t val;      /* 每帧写回的值 */
    uint8_t  wid;      /* 8 / 16 */
    uint8_t  used;
} cheat_lock_t;
static cheat_lock_t s_lock[CHEAT_MAX_LOCK];

/* ---------------- 区域/会话 ---------------- */

static bool same_region(const cheater_region_t *a, const cheater_region_t *b)
{
    return a->base == b->base && a->size == b->size &&
           a->peek == b->peek && a->poke == b->poke;
}

void cheater_lock_clear(void)
{
    memset(s_lock, 0, sizeof(s_lock));
}

void cheater_begin(const cheater_region_t *reg, cheater_bits_t bits)
{
    cheater_region_t nr;
    memset(&nr, 0, sizeof(nr));
    if (reg) nr = *reg;

    /* 换了游戏 / 换了 RAM 布局 ⇒ 旧锁定地址指向别的东西, 必须全部作废 */
    if (!same_region(&nr, &s_reg)) {
        cheater_lock_clear();
        s_round = 0;
    }
    s_reg  = nr;
    s_bits = bits;
    s_count = -1;
    s_bak_count = -1;
    s_scan_ms = 0;
}

static inline uint8_t read_byte(uint16_t off)
{
    /* 用 int 防区域 base+size 跨 0xFFFF 截断 (如 GBC 整片 [0x8000,0x10000)) */
    if ((int)off >= (int)s_reg.base + (int)s_reg.size || !s_reg.peek) return 0;
    return s_reg.peek(off);
}

static void wr(uint16_t off, uint8_t v)
{
    if ((int)off >= (int)s_reg.base + (int)s_reg.size || !s_reg.poke) return;
    s_reg.poke(off, v);
}

/* 按候选自己的位宽读当前值 (16 位小端) */
static uint32_t cand_rd(int idx)
{
    uint16_t off = s_cand[idx];
    uint32_t b0 = read_byte(off);
    if (s_wid[idx] == CHEAT_BITS_16) {
        uint32_t b1 = read_byte((uint16_t)(off + 1));
        return b0 | (b1 << 8);
    }
    return b0;
}

static void write_off(uint16_t off, uint8_t wid, uint32_t value)
{
    if (wid == CHEAT_BITS_16) {
        wr(off, (uint8_t)(value & 0xFF));
        wr((uint16_t)(off + 1), (uint8_t)((value >> 8) & 0xFF));
    } else {
        wr(off, (uint8_t)value);
    }
}

/* ---------------- 扫描 ---------------- */

int cheater_first_scan(uint32_t value)
{
    if (!s_reg.peek || s_reg.size == 0) { s_count = -1; return -1; }

    int64_t t0 = esp_timer_get_time();
    int n = 0;
    const int lo = (int)s_reg.base;
    const int hi = lo + (int)s_reg.size;
    const bool want8  = (value < 0x100);
    const bool want16 = (value < 0x10000);

    /* 逐字节窗口: 游戏变量常非 2 字节对齐存放, step=1 不遗漏任何 16 位对齐.
     * 每个位置同时按 8 位与 16 位(小端)匹配, 值存成哪个宽度都能命中 — 通用.
     * ★ 滚动复用: 本轮的 b1 就是下一轮的 b0 ⇒ 每字节只 read 一次 (原来两次). */
    uint32_t b0 = read_byte((uint16_t)lo);
    for (int off = lo; off < hi; off++) {
        const bool has_next = (off + 1 < hi);
        uint32_t b1 = has_next ? read_byte((uint16_t)(off + 1)) : 0;
        bool m8  = want8  && (b0 == value);
        bool m16 = false;
        uint32_t w16 = 0;
        if (has_next && want16) {
            w16 = b0 | (b1 << 8);
            m16 = (w16 == value);
        }
        b0 = b1;                       /* ← 复用给下一轮 */
        if (!m8 && !m16) continue;
        if (n >= CHEAT_MAX_CAND) break;
        s_cand[n] = (uint16_t)off;
        if (m8) {            /* 8 位也命中时优先记 8 位 (最常见的小值/开关量) */
            s_wid[n] = CHEAT_BITS_8;
            s_ref[n] = (uint16_t)(value & 0xFF);   /* 命中即等于 value, 直接作参考值 */
        } else {
            s_wid[n] = CHEAT_BITS_16;
            s_ref[n] = (uint16_t)(value & 0xFFFF);
        }
        n++;
    }
    s_count  = n;
    s_round  = 1;
    s_bak_count = -1;                  /* 新一轮搜索: 旧的撤销点失效 */
    s_scan_ms = (int)((esp_timer_get_time() - t0) / 1000);
    return n;
}

int cheater_filter(cheater_op_t op, uint32_t value)
{
    if (s_count < 0) return -1;

    /* 过滤前快照一份撤销点 (误选"变大/变小"时能退回来) */
    if (s_count > 0) {
        memcpy(s_bak_cand, s_cand, (size_t)s_count * sizeof(s_cand[0]));
        memcpy(s_bak_ref,  s_ref,  (size_t)s_count * sizeof(s_ref[0]));
        memcpy(s_bak_wid,  s_wid,  (size_t)s_count * sizeof(s_wid[0]));
        s_bak_count = s_count;
    }

    int64_t t0 = esp_timer_get_time();
    int n = 0;
    int hi = (int)s_reg.base + (int)s_reg.size;
    for (int i = 0; i < s_count; i++) {
        uint16_t off = s_cand[i];
        if (s_wid[i] == CHEAT_BITS_16 && (int)off + 1 >= hi)
            continue;   /* 16 位读跨出区域, 丢弃 */
        uint32_t cur = cand_rd(i);
        uint32_t prev = s_ref[i];
        bool keep = false;
        switch (op) {
            case CHEAT_EQ:        keep = (cur == value); break;
            case CHEAT_GT:        keep = (cur > prev); break;
            case CHEAT_LT:        keep = (cur < prev); break;
            case CHEAT_CHANGED:   keep = (cur != prev); break;
            case CHEAT_UNCHANGED: keep = (cur == prev); break;
        }
        if (keep) {
            s_cand[n] = off;
            s_wid[n]  = s_wid[i];
            s_ref[n]  = (uint16_t)cur;
            n++;
        }
    }
    s_count = n;
    s_round++;
    s_scan_ms = (int)((esp_timer_get_time() - t0) / 1000);
    return n;
}

int cheater_count(void)    { return s_count; }
int cheater_round(void)    { return s_round; }
int cheater_scan_ms(void)  { return s_scan_ms; }

/* ---------------- 撤销 ---------------- */

bool cheater_can_undo(void) { return s_bak_count >= 0; }

int cheater_undo(void)
{
    if (s_bak_count < 0) return -1;
    memcpy(s_cand, s_bak_cand, (size_t)s_bak_count * sizeof(s_cand[0]));
    memcpy(s_ref,  s_bak_ref,  (size_t)s_bak_count * sizeof(s_ref[0]));
    memcpy(s_wid,  s_bak_wid,  (size_t)s_bak_count * sizeof(s_wid[0]));
    s_count = s_bak_count;
    s_bak_count = -1;              /* 只留一层: 撤销后不能再撤 */
    if (s_round > 1) s_round--;
    return s_count;
}

/* ---------------- 候选读写 ---------------- */

uint16_t cheater_addr(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return s_cand[idx];
}

uint32_t cheater_read(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return cand_rd(idx);
}

uint8_t cheater_width(int idx)
{
    if (idx < 0 || idx >= s_count) return 8;
    return s_wid[idx];
}

void cheater_write(int idx, uint32_t value)
{
    if (idx < 0 || idx >= s_count) return;
    uint16_t off = s_cand[idx];
    write_off(off, s_wid[idx], value);
    s_ref[idx] = (uint16_t)(s_wid[idx] == CHEAT_BITS_16 ? (value & 0xFFFF) : (value & 0xFF));
}

/* 删除第 idx 条候选记录 (左移覆盖, 保序) */
void cheater_remove(int idx)
{
    if (s_count < 0 || idx < 0 || idx >= s_count) return;
    for (int i = idx; i < s_count - 1; i++) {
        s_cand[i] = s_cand[i + 1];
        s_ref[i]  = s_ref[i + 1];
        s_wid[i]  = s_wid[i + 1];
    }
    s_count--;
}

/* ---------------- 锁定 ---------------- */

static int lock_find(uint16_t off, uint8_t wid)
{
    for (int i = 0; i < CHEAT_MAX_LOCK; i++)
        if (s_lock[i].used && s_lock[i].off == off && s_lock[i].wid == wid) return i;
    return -1;
}

int cheater_lock(int idx)
{
    if (idx < 0 || idx >= s_count) return -1;
    uint16_t off = s_cand[idx];
    uint8_t  wid = s_wid[idx];
    int e = lock_find(off, wid);
    if (e >= 0) return e;                  /* 幂等: 已锁就返回原序号 */
    for (int i = 0; i < CHEAT_MAX_LOCK; i++) {
        if (!s_lock[i].used) {
            s_lock[i].used = 1;
            s_lock[i].off  = off;
            s_lock[i].wid  = wid;
            s_lock[i].val  = (uint16_t)cand_rd(idx);
            write_off(off, wid, s_lock[i].val);
            return i;
        }
    }
    return -1;                             /* 表满 */
}

bool cheater_cand_locked(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    return lock_find(s_cand[idx], s_wid[idx]) >= 0;
}

int cheater_cand_lock_index(int idx)
{
    if (idx < 0 || idx >= s_count) return -1;
    return lock_find(s_cand[idx], s_wid[idx]);
}

int cheater_cand_unlock(int idx)
{
    int e = cheater_cand_lock_index(idx);
    if (e < 0) return -1;
    s_lock[e].used = 0;
    return e;
}

int cheater_lock_count(void)
{
    int n = 0;
    for (int i = 0; i < CHEAT_MAX_LOCK; i++) if (s_lock[i].used) n++;
    return n;
}

int cheater_lock_nth(int n)
{
    if (n < 0) return -1;
    for (int i = 0; i < CHEAT_MAX_LOCK; i++) {
        if (!s_lock[i].used) continue;
        if (n-- == 0) return i;
    }
    return -1;
}

uint16_t cheater_lock_addr(int l)
{
    if (l < 0 || l >= CHEAT_MAX_LOCK || !s_lock[l].used) return 0;
    return s_lock[l].off;
}

uint32_t cheater_lock_value(int l)
{
    if (l < 0 || l >= CHEAT_MAX_LOCK || !s_lock[l].used) return 0;
    /* 回读实时值 (锁定期间应该恒等于设定值, 回读可验证锁定是否真的生效) */
    uint32_t b0 = read_byte(s_lock[l].off);
    if (s_lock[l].wid == CHEAT_BITS_16)
        return b0 | ((uint32_t)read_byte((uint16_t)(s_lock[l].off + 1)) << 8);
    return b0;
}

uint8_t cheater_lock_width(int l)
{
    if (l < 0 || l >= CHEAT_MAX_LOCK || !s_lock[l].used) return 8;
    return s_lock[l].wid;
}

void cheater_lock_set_value(int l, uint32_t v)
{
    if (l < 0 || l >= CHEAT_MAX_LOCK || !s_lock[l].used) return;
    if (s_lock[l].wid == CHEAT_BITS_8 && v > 0xFF) v &= 0xFF;
    if (s_lock[l].wid == CHEAT_BITS_16 && v > 0xFFFF) v &= 0xFFFF;
    s_lock[l].val = (uint16_t)v;
    write_off(s_lock[l].off, s_lock[l].wid, v);
    /* 同步候选里的参考值, 免得下次过滤把它当成"有变化" */
    for (int i = 0; i < s_count; i++)
        if (s_cand[i] == s_lock[l].off && s_wid[i] == s_lock[l].wid) {
            s_ref[i] = s_lock[l].val;
            break;
        }
}

void cheater_unlock(int l)
{
    if (l < 0 || l >= CHEAT_MAX_LOCK) return;
    s_lock[l].used = 0;
}

void cheater_lock_tick(void)
{
    if (!s_reg.poke) return;
    for (int i = 0; i < CHEAT_MAX_LOCK; i++) {
        if (!s_lock[i].used) continue;
        write_off(s_lock[i].off, s_lock[i].wid, s_lock[i].val);
    }
}
