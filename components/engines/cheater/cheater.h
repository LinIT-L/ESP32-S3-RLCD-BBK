/**
 * cheater.h — 修改机内存搜索核心 (引擎无关, 单会话).
 *
 * "八门神器"式多轮内存搜索:
 *   第1轮: 在引擎"可搜索 RAM 区"找所有 == 目标值的地址;
 *   后续轮: 按 (等于X/变大/变小/有变化/无变化) 过滤候选;
 *   结果  : 候选列表浏览, 对某地址写新值 (peek/poke 经 provider 回调).
 *
 * 安全边界: 只遍历 provider 给定的 [base, base+size) 区, 绝不碰其它内存.
 * 线程: 调用方保证在引擎"冻结/帧边界"调用 (与模拟同任务).
 * 候选池: 静态 PSRAM (8192 候选), 单会话 — 引擎逐个运行, 不会并发搜索.
 */
#ifndef CHEATER_H
#define CHEATER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 可搜索区域 + 读写回调 (由具体引擎提供, 只碰该引擎自己的 RAM) */
typedef struct {
    uint16_t base;          /* 区域起始偏移 (引擎 RAM 相对) */
    uint16_t size;          /* 区域字节数 */
    uint8_t  (*peek)(uint16_t off);   /* 读单字节 (off = 区域内相对偏移, 已钳制) */
    void     (*poke)(uint16_t off, uint8_t v);
} cheater_region_t;

/* 过滤关系 */
typedef enum {
    CHEAT_EQ = 0,      /* 等于某值 */
    CHEAT_GT,          /* 变大 */
    CHEAT_LT,          /* 变小 */
    CHEAT_CHANGED,     /* 有变化 */
    CHEAT_UNCHANGED,   /* 无变化 */
} cheater_op_t;

#define CHEAT_MAX_CAND  8192

typedef enum { CHEAT_BITS_8 = 8, CHEAT_BITS_16 = 16 } cheater_bits_t;

/* ---- 会话控制 ---- */
/* 开始新会话 (绑定区域+位宽). 会清空候选. */
void cheater_begin(const cheater_region_t *reg, cheater_bits_t bits);

/* 第1轮: 全区域搜 == value. 返回匹配数. */
int cheater_first_scan(uint32_t value);

/* 后续轮: 按关系过滤 (value 仅 CHEAT_EQ 用). 返回剩余候选数. */
int cheater_filter(cheater_op_t op, uint32_t value);

/* 当前候选数 (会话进行中) */
int cheater_count(void);

/* 候选地址 (区域内相对偏移) 与当前读值 (8/16 位) */
uint16_t cheater_addr(int idx);
uint32_t cheater_read(int idx);

/* 修改候选值 (写入引擎 RAM) */
void cheater_write(int idx, uint32_t value);
void cheater_remove(int idx);

#ifdef __cplusplus
}
#endif

#endif /* CHEATER_H */
