/**
 * cheater.h — 修改机内存搜索核心 (引擎无关, 单会话).
 *
 * 参照「八门神器 / 烧饼修改器」的核心能力:
 *   第1轮: 在引擎"可搜索 RAM 区"找所有 == 目标值的地址 (8/16 位自动);
 *   后续轮: 按 (等于X / 变大 / 变小 / 有变化 / 无变化) 过滤候选, **可撤销一步**;
 *   结果  : 候选列表浏览, 对某地址写新值 (peek/poke 经 provider 回调);
 *   锁定  : 把地址加入锁定表, 由 cheater_lock_tick() 每帧写回 —— 值再也变不回去
 *           (改血量/金币后游戏自己扣/加都无效, 这是修改器最常用的一招).
 *
 * 安全边界: 只遍历 provider 给定的 [base, base+size) 区, 绝不碰其它内存.
 * 线程: 搜索/过滤须在引擎"冻结/帧边界"调用 (与模拟同任务);
 *       cheater_lock_tick() 由显示合成帧调用 (只写固定几个地址, 不遍历区域).
 * 候选池: 静态 PSRAM (8192 候选 + 一份撤销备份), 单会话 — 引擎逐个运行.
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
#define CHEAT_MAX_LOCK  24     /* 锁定表容量 (够用: 一般只锁血/钱/弹药几条) */

typedef enum { CHEAT_BITS_8 = 8, CHEAT_BITS_16 = 16 } cheater_bits_t;

/* ---- 会话控制 ---- */
/* 开始新会话 (绑定区域+位宽). 会清空候选; 区域变化时同时清空锁定表. */
void cheater_begin(const cheater_region_t *reg, cheater_bits_t bits);

/* 第1轮: 全区域搜 == value. 返回匹配数 (-1 = 未登记区域). */
int cheater_first_scan(uint32_t value);

/* 后续轮: 按关系过滤 (value 仅 CHEAT_EQ 用). 返回剩余候选数. 过滤前自动存一份撤销点. */
int cheater_filter(cheater_op_t op, uint32_t value);

/* 当前候选数 (会话进行中), -1 = 尚未搜索 */
int cheater_count(void);

/* 已搜过几轮 (0=未搜, 1=首轮, >1=过滤后). 用于界面显示"第 N 轮". */
int cheater_round(void);

/* 上次搜索/过滤耗时 (毫秒取整). 性能可观测, 也用于界面回显. */
int cheater_scan_ms(void);

/* ---- 撤销 ---- */
bool cheater_can_undo(void);
/* 回到上一次过滤前的候选集. 返回恢复后的候选数; -1 = 无可撤销. */
int  cheater_undo(void);

/* ---- 候选读写 ---- */
/* 候选地址 (区域内相对偏移) 与当前读值 (8/16 位) */
uint16_t cheater_addr(int idx);
uint32_t cheater_read(int idx);
/* 候选位宽: 8 或 16 (界面显示 B / W) */
uint8_t  cheater_width(int idx);

/* 修改候选值 (写入引擎 RAM) */
void cheater_write(int idx, uint32_t value);
void cheater_remove(int idx);

/* ---- 锁定 (每帧自动写回) ---- */
/* 锁定候选 idx. 返回锁定序号 0..CHEAT_MAX_LOCK-1; -1 = 表满/参数无效.
 * 同一地址重复锁定返回已有序号 (幂等). */
int  cheater_lock(int idx);
/* 候选 idx 是否已在锁定表里 (用于界面打标记) */
bool cheater_cand_locked(int idx);
/* 候选 idx 对应的锁定表下标; -1 = 未锁定 (界面"解锁"用) */
int  cheater_cand_lock_index(int idx);
/* 解除候选 idx 的锁定; 返回原锁定序号, -1 = 本来就没锁 */
int  cheater_cand_unlock(int idx);
int  cheater_lock_count(void);
/* 第 n 个"已锁"槽的锁定序号 (n = 0..count-1), -1 = 越界/空.
 * 锁定表中间可能有空洞, 界面按 0..count-1 连续下标浏览时用这个换算. */
int  cheater_lock_nth(int n);
uint16_t cheater_lock_addr(int l);
uint32_t cheater_lock_value(int l);
uint8_t  cheater_lock_width(int l);
void cheater_lock_set_value(int l, uint32_t v);
/* 解除第 l 条锁定 (按锁定表下标, 不是候选下标) */
void cheater_unlock(int l);
/* 全部解锁 (换游戏/退出时用) */
void cheater_lock_clear(void);
/* 每帧写回所有锁定地址. 由显示合成帧 (cheat_float_overlay_tick) 调用. */
void cheater_lock_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* CHEATER_H */
