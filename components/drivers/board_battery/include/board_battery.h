#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t voltage_mv;
    uint8_t percent;
} board_battery_status_t;

esp_err_t board_battery_init(void);

/* 充电中检测 (板型表 .chrg, LGS4056HDA 的 CHRG 脚, 开漏低有效)。
 * 本板没接时返回 false / -1, 调用方据此退化成"靠 VBUS 推断"。 */
bool board_battery_charging(void);          /* 直接读脚 */
bool board_battery_charging_cached(void);   /* 500ms 缓存 + 变化日志, 上层 1Hz 调这个 */
int  board_battery_chrg_pin(void);
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
 *   睡眠期间**不采样**(没有主循环), 醒来自动补成 0xFF ⇒ 波形中间断开、连线跨过去。 */
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

/* ==== 一个月日档 (每天 min/max, 存 NVS, 断电不丢) ====
 * linit: "记住要把那才知道的信息都保存, 可以保存一个月 电池电压信息"。
 * 每天一条, 环形 31 天; 只在**跨天**时落盘一次 ⇒ 一个月 30 次 NVS 写, 无闪存磨损顾虑。 */
#define BOARD_BATTERY_DAYS 31
typedef struct { uint32_t day; uint16_t min_mv, max_mv; } bat_day_t;
/* 取日档 (按时间旧→新), 返回条数; day 为"日号"(非日历日期, 见实现注释) */
int board_battery_days_get(bat_day_t *out, int max);
int board_battery_history(bat_sample_t *out, int max);   /* 按时间旧→新填, 返回条数 */
uint32_t board_battery_clock_s(void);                    /* 当前 RTC 秒 (供窗口计算) */

/* 运行时诊断 (remcon `bat` 命令): 打印 raw / 脚上 mV / 分压系数 / 换算 VBAT / 百分比。
 * 拿着万用表同时量 BAT_ADC 脚与 VBAT, 和这行对照即可判定分压系数是否需要校准。 */
void board_battery_diag(void);

#ifdef __cplusplus
}
#endif

/* ★ 2026-10-16: app_board 按板型表下发电池 ADC 通道号 */
void board_battery_set_channel(int chan);
