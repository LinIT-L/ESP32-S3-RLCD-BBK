/**
 * cheater.c — 修改机内存搜索核心实现 (单会话, PSRAM 候选池).
 *
 * 只通过回调 peek/poke 访问引擎给定的 [base, base+size) 区域.
 * 16 位值按小端 (低字节在前), 候选地址区内相对偏移.
 */
#include "cheater.h"
#include "esp_attr.h"
#include <string.h>

/* 候选池: PSRAM 静态 (单会话, 两个 8192×2B = 32KB). 扫描最坏 32KB 全匹配时也够. */
#define CHEAT_STATIC_CAND CHEAT_MAX_CAND
EXT_RAM_BSS_ATTR static uint16_t s_cand[CHEAT_STATIC_CAND];
EXT_RAM_BSS_ATTR static uint16_t s_ref[CHEAT_STATIC_CAND];
/* 每候选自动识别的位宽 (CHEAT_BITS_8 / CHEAT_BITS_16).
 * 开源作弊工具惯例: 同一帧里同时按 8 位与 16 位匹配, 值存成哪个宽度都能搜到. */
EXT_RAM_BSS_ATTR static uint8_t s_wid[CHEAT_STATIC_CAND];
static int s_count = -1;            /* -1 = 尚未搜索 */
static cheater_region_t s_reg;
static cheater_bits_t s_bits = CHEAT_BITS_8;

void cheater_begin(const cheater_region_t *reg, cheater_bits_t bits)
{
    memset(&s_reg, 0, sizeof(s_reg));
    if (reg) s_reg = *reg;
    s_bits = bits;
    s_count = -1;
}

static inline uint8_t read_byte(uint16_t off)
{
    /* 用 int 防区域 base+size 跨 0xFFFF 截断 (如 GBC 整片 [0x8000,0x10000)) */
    if ((int)off >= (int)s_reg.base + (int)s_reg.size || !s_reg.peek) return 0;
    return s_reg.peek(off);
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

static void wr(uint16_t off, uint8_t v)
{
    if ((int)off >= (int)s_reg.base + (int)s_reg.size || !s_reg.poke) return;
    s_reg.poke(off, v);
}

int cheater_first_scan(uint32_t value)
{
    if (!s_reg.peek || s_reg.size == 0) { s_count = -1; return -1; }
    int n = 0;
    int hi = (int)s_reg.base + (int)s_reg.size;
    /* 逐字节窗口: 游戏变量常非 2 字节对齐存放, step=1 不遗漏任何 16 位对齐.
     * 每个位置同时按 8 位与 16 位(小端)匹配, 值存成哪个宽度都能命中 — 通用. */
    for (int off = (int)s_reg.base; off < hi; off++) {
        uint32_t b0 = read_byte((uint16_t)off);
        bool m8  = (value < 0x100) && (b0 == value);
        bool m16 = false;
        uint32_t w16 = 0;
        if (off + 1 < hi && value < 0x10000) {
            w16 = b0 | ((uint32_t)read_byte((uint16_t)(off + 1)) << 8);
            m16 = (w16 == value);
        }
        if (!m8 && !m16) continue;
        if (n >= CHEAT_MAX_CAND) break;
        s_cand[n] = (uint16_t)off;
        if (m8) {            /* 8 位也命中时优先记 8 位 (最常见的小值/开关量) */
            s_wid[n] = CHEAT_BITS_8;
            s_ref[n] = (uint16_t)b0;
        } else {
            s_wid[n] = CHEAT_BITS_16;
            s_ref[n] = (uint16_t)w16;
        }
        n++;
    }
    s_count = n;
    return n;
}

int cheater_filter(cheater_op_t op, uint32_t value)
{
    if (s_count < 0) return -1;
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
    return n;
}

int cheater_count(void) { return s_count; }

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

void cheater_write(int idx, uint32_t value)
{
    if (idx < 0 || idx >= s_count) return;
    uint16_t off = s_cand[idx];
    if (s_wid[idx] == CHEAT_BITS_16) {
        wr(off, (uint8_t)(value & 0xFF));
        wr((uint16_t)(off + 1), (uint8_t)((value >> 8) & 0xFF));
        s_ref[idx] = (uint16_t)(value & 0xFFFF);
    } else {
        wr(off, (uint8_t)value);
        s_ref[idx] = (uint16_t)(value & 0xFF);
    }
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
