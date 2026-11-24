#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ★ 2026-11-21 定案: 充电三态只用**一根 CHRG**(模组 29 脚 → GPIO44, 开漏低有效)。
 *   CHRG 低                     → ACTIVE 充电中
 *   CHRG 高 + VBAT ≥ 4150mV     → FULL   已充满(充电结束 IC 释放 CHRG, 电压已到顶)
 *   CHRG 高 + 电压没到          → NONE   未充电
 * ⛔ GPIO2 / GPIO43 是扩展脚(预留给外接), 固件不得占用 ⇒ 不做 VBUS 分压, 不用 DONE 专线。
 * ⚠ 单线分不出"插着线但没在充"与"没插线"(CHRG 都高), 这是单 IO 的信息上限;
 *   由电压补足 —— "满电拔线显示满格"本来也符合直觉。 */
typedef enum {
    BAT_CHG_NONE   = 0,
    BAT_CHG_ACTIVE = 1,
    BAT_CHG_FULL   = 2,
} bat_chg_state_t;

typedef struct {
    uint32_t voltage_mv;
    uint8_t percent;
    uint8_t chg_state;      /* bat_chg_state_t: 0/1/2 (read_cached 填, read 恒 0) */
} board_battery_status_t;

esp_err_t board_battery_init(void);

/* 充电中检测 (板型表 .chrg, LGS4056HDA 的 CHRG 脚, 开漏低有效)。
 * 本板没接时返回 false / -1 —— 小智板 GPIO44 空闲时读到上拉高, 同样报"未充电"。 */
bool board_battery_charging(void);          /* 直接读脚 */
bool board_battery_charging_cached(void);   /* 500ms 缓存 + 变化日志, 上层 1Hz 调这个 */
/* ★ 2026-11-21: 三态 (NONE/ACTIVE/FULL), 走 60s 缓存, UI 画图标用这个 */
bat_chg_state_t board_battery_charge_state(void);
int  board_battery_chrg_pin(void);          /* CHRG 脚号 (-1 = 未接) */
esp_err_t board_battery_read(board_battery_status_t *status);

/* 统一 60s 节流缓存读取 (linit 2026-09-20: 全系统单一采样节拍器).
 * 首次调用立即采样; 之后 60s 内直接返回缓存, ADC 至多 60s 打一次.
 * 状态栏/低电量告警/网页手柄/终端 battery 命令/Lua app.sys.battery()
 * 全部走本接口, 采样失败时不污染旧缓存 (返回错误, 缓存保持上次值). */
esp_err_t board_battery_read_cached(board_battery_status_t *status);

/* ==== 电压历史 (「设置 → 休眠与电源 → 电池信息」的波形图) ====
 * ★★ 精简存储 (linit 2026-10-14 "存储的精简一点"): **1 字节 / 分钟**
 *   ① 电压只存 10mV 分辨率 ⇒ 1 字节 (3.00~5.54V, 254 档够画图)
 *   ② **时间不存** —— 采样是"均匀每分钟一格", 只存一个基准时间, 第 i 格时间 = 基准 + i×60
 *   ③ 睡眠/关机没采的分钟 = 哨兵 `0xFF`(无数据), 画图时直接跨过去连上
 *   ⇒ 一天 1.44KB · **7 天 10KB** (旧方案带 u32 时间戳要 6B/条, 相差 6 倍)。保留期=7 天。
 * ⚠ 存储位置: 运行期数组在 **PSRAM**(10KB), 另按"天"分块**持久化到 NVS**(ns `batcal`, 7×1440B)。
 *   睡眠期间**不采样**(没有主循环), 醒来自动补成 0xFF ⇒ 波形中间断开、连线跨过去。
 * ⚠ 这是**画图用的历史**，不参与电量计算（2026-10-31 精简后，电量只由当前电压查表得出）。 */
typedef struct { uint32_t t; uint16_t mv; } bat_sample_t;
#define BOARD_BATTERY_WAVE_MAX 720           /* 一次最多取多少点给绘制 (够 330px 屏) */
/* ★ 2026-10-26 linit: 波形图最长只显示**最近 24 小时** (原来 7 天)。
 *   存储仍是 7 天 1 字节/分钟 (BAT_KEEP_DAYS=7, 10KB PSRAM + 按天 NVS),
 *   这里只是**显示窗口上限** —— 以后要看 7 天改回 (7u * 86400u) 即可, 数据都还在。 */
#define BOARD_BATTERY_SPAN_MAX (24u * 3600u) /* 最长显示 24 小时 */

/* 已有历史的时间跨度(秒); 无数据返回 0 */
uint32_t board_battery_hist_span_s(void);
/* 取窗口 window_s 内的波形点 (自动抽稀到 ≤max 条), 按时间旧→新; 返回条数。
 * ⚠ 抽稀规则天然实现"近密远疏": 2 小时窗口 → 1 分钟一点; 7 天窗口 → 约 15 分钟一点。 */
int board_battery_wave(bat_sample_t *out, int max, uint32_t window_s);
uint32_t board_battery_clock_s(void);                    /* 当前 RTC 秒 (供窗口计算) */

/* 运行时诊断 (remcon `bat`): raw / 校准后脚上电压 / 分压后 VBAT / 电量 / CHRG 状态 / 校准是否成功。
 * 拿万用表同时量 VBAT 与这里的数字对照，即可判断分压系数要不要重新标 (batcal)。 */
void board_battery_diag(void);

/* ==== 真机测试记录 (remcon `batlog`) ====
 * 每次真采样（60s 一次，和状态栏同一条路）记一条 12B 记录，事后 `batlog dump` 打出 CSV。
 *   · **关闭时**只有一个判断 —— 不采样、不分配、不写 Flash；
 *   · 开启时也不额外打 ADC，只搭车已有的那次采样；不建任务；
 *   · 120 条 = 1.4KB，放 **PSRAM**，不占内部 RAM。
 * ⚠ 纯诊断，**不参与任何电量计算**。 */
#define BOARD_BATTERY_TRACE_MAX 120
bool board_battery_trace_enable(bool on);   /* 返回设置后的状态 */
bool board_battery_trace_is_on(void);
int  board_battery_trace_count(void);       /* 已记录条数 */
int  board_battery_trace_dump(void);        /* 打印全部记录（旧→新），返回条数 */
void board_battery_trace_clear(void);       /* 清空记录（不清开关状态） */

#ifdef __cplusplus
}
#endif

/* ★ 2026-10-16: app_board 按板型表下发电池 ADC 通道号 */
void board_battery_set_channel(int chan);
