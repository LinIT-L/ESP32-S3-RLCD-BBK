/**
 * @file sd_arb.h
 * @brief TF/SD 卡访问仲裁 —— 本机访问 / USB MSC 接管的唯一状态机与闸门。
 *
 * 为什么需要这一层（2026-11-24 定位到的真实故障）:
 *   USB MSC "挂载到电脑" 时, 原来的流程是 `esp_vfs_fat_unregister_path()`。
 *   IDF 这个函数会 `_lock_close(&ctx->lock)` + `free(ctx)` —— **不检查还有没有打开的
 *   FILE\***。于是：
 *     · 别的任务正好在 f_open/f_read/f_write（持有被删掉的卷互斥量）→ 取已删除的
 *       信号量 / 访问已 free 的 fat_ctx → 崩;
 *     · 或某个模块手里攥着 FILE* 跨过交卡点, 之后 fclose/fread → 悬垂指针 UAF → 崩。
 *   两者都是**内存生命周期问题**, 靠"再开 FatFs 重入锁"根本解决不了。
 *
 * 因此把"谁能碰卡"收成一个显式状态机 + 一个在途计数:
 *
 *   OFF ──挂载成功──> LOCAL ──交卡──> HANDOFF ──> USB
 *     ^                 ^                            │
 *     │                 └────── RESTORE <──收卡──────┘
 *     └──挂载/重挂失败──────┘
 *
 *   LOCAL   : 本机拥有, sd_arb_enter() 才返回 true
 *   HANDOFF : 交卡过渡窗口, 新访问一律拒绝, 等 s_inflight 归零
 *   USB     : 卡在电脑手里, 本机任何挂载/格式化/文件访问都必须让路
 *   RESTORE : 收卡过渡窗口, 同上拒绝访问
 *   OFF     : 没卡/未挂载, 拒绝访问(调用方按"无卡"处理)
 *
 * 用法（调用方必须成对）:
 *   if (!sd_arb_enter()) return;      // 拿不到就**当作没有卡**, 不要重试成忙等
 *   ... fopen/fread/opendir/stat ...  // 全部在这段里完成
 *   sd_arb_leave();
 *
 * ⚠ 这不是 FatFs 卷锁的替代品: FatFs 卷锁保证"同一时刻只有一个 FatFs 调用",
 *   本模块保证"交卡时没有本机调用在跑、也没有跨交卡点的句柄"。
 * ⚠ enter/leave 只做端口临界区计数（微秒级），可以在渲染循环里调用。
 *
 * ★ 2026-11-25 补充: enter/leave 现在还成对管理一把 ESP_PM_NO_LIGHT_SLEEP 锁
 *   （见 sd_arb.c 顶部的说明）。IDF 6.0.3 的 sdmmc 驱动只在**单次事务**期间持
 *   ESP_PM_APB_FREQ_MAX，一次逻辑访问由多次事务组成，事务之间仍可能进浅睡；
 *   这把锁把"整段逻辑访问"盖住。没有在途访问时锁计数为 0，不影响正常省电。
 */
#ifndef SD_ARB_H
#define SD_ARB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_ARB_OFF = 0,   /* 未挂载 / 无卡: 本机不可访问 */
    SD_ARB_LOCAL,     /* 本机拥有, 允许访问 */
    SD_ARB_HANDOFF,   /* 正在交给 USB MSC: 拒绝新访问, 等在途收尾 */
    SD_ARB_USB,       /* 卡归 USB MSC (电脑) */
    SD_ARB_RESTORE,   /* 正在从 USB 收回: 拒绝访问 */
} sd_arb_state_t;

/* 幂等; 只是把状态显式置为 OFF。不建任何堆对象/信号量。 */
void sd_arb_init(void);

sd_arb_state_t sd_arb_state(void);
const char   *sd_arb_state_str(void);

/* 本机现在能不能访问卡（等价于 state==LOCAL）。轻量, 不占锁。 */
bool sd_arb_available(void);

/* 进入/退出一次本机访问。返回 false 时**不要**调用 sd_arb_leave()。 */
bool sd_arb_enter(void);
void sd_arb_leave(void);

/* 当前在途访问数（调试/日志用） */
int sd_arb_inflight(void);

/* 等所有在途访问退出, 最多 timeout_ms; 已归零立刻返回 true。 */
bool sd_arb_wait_idle(uint32_t timeout_ms);

/* 状态迁移 —— 只允许 sd_scan (挂载/卸载/交卡) 调用。 */
void sd_arb_set_state(sd_arb_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* SD_ARB_H */
