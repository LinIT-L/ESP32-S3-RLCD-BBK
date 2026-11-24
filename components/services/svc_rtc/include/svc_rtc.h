/**
 * svc_rtc.h — 外挂 RTC（DS3231MZ+，I2C 0x68）读写 + 自动校时
 *
 * ★ 2026-11-20: 小智版 PCB1_3 起焊了 DS3231MZ+（封装内自带 MEMS 谐振器 + 温补，
 *   ±5ppm ≈ 每月 13 秒，不用外接晶振），靠 VBAT 直连主电池走时。
 *
 * 分工（重要）:
 *   本模块 = **I2C 读写 + 策略**；settimeofday 只在 svc_time 里出现一次。
 *   读到的芯片时间交给 svc_time_set_from_rtc()，由它决定采纳与否。
 *
 * 自动校时策略（谁更新听谁的）:
 *   ① 开机: 系统时间无效(<2025) 且芯片没掉过电(OSF=0) ⇒ 用芯片时间
 *   ② 每 60s: 读芯片与系统时间比对
 *        · 系统刚被 NTP 校准过(NETWORK_VALID) ⇒ 系统最准，把芯片写成系统时间
 *        · 否则（离线很久） ⇒ 芯片更准（±5ppm vs 内部 RC），修系统时间
 *   ③ 关机前: 再写一次芯片
 *
 * ⛔ 本模块不自建 I2C 总线：DS3231 与触摸共用 board_pins 的 tp_sda/tp_scl，
 *   总线由 touch_panel 建好后用 i2c_master_get_bus_handle() 拿现成的。
 *   同端口重复 i2c_new_master_bus 会失败（IDF 限制）。
 */
#ifndef LIN_TOS_SVC_RTC_H
#define LIN_TOS_SVC_RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_RTC_I2C_ADDR    0x68
/* 比对周期（秒）。★ 2026-11-24: 60 → 300。
 *   DS3231MZ 带温补 ±5ppm ≈ 每月 13 秒，60s 比一轮等于每分钟允许一次跳秒；
 *   5 分钟一轮足够抑制内部 RC 漂移，又把 I2C 事务与时间跳变压到 1/5。 */
#define SVC_RTC_PERIOD_S    300
/* 运行期校准阈值（秒）: 差值小于它就不动系统时间（避免频繁跳秒） */
#define SVC_RTC_CORRECT_MIN_S   5
/* 运行期倒退保护阈值（秒）: 系统时间已可信时，芯片比系统**早**超过这么多 ⇒
 *   判为芯片掉电/旧值/被写错，不采纳（否则旧时间会把系统时间拉回）。 */
#define SVC_RTC_BACKWARD_MAX_S  600
/* 两次写芯片的最小间隔（秒），用来合并 main 主循环与周期任务的回写 */
#define SVC_RTC_WRITE_MIN_GAP_S 60

/* 启动自动校时（挂一个低优先级周期任务）。
 * 返回 false = 已启动过。真正"有没有芯片"看 svc_rtc_present()。 */
bool svc_rtc_init(void);

/* ★ 2026-11-24: 开机**同步**注入 —— 调用方保证板级初始化(触摸/I2C 总线)已就绪,
 *   即 main 在 app_board_init() 返回之后立刻调用。内部完成 attach + 首读 + 注入
 *   (一次 I2C 读, <5ms), 让系统时间在 Logo/桌面之前就是对的时间。
 *   幂等: 已完成注入时直接返回 true。
 *   返回 false = 本板没芯片 / 总线不通 / 用户关掉了时钟。 */
bool svc_rtc_boot_sync(void);

/* 芯片是否应答（任务挂载成功后为 true） */
bool svc_rtc_present(void);

/* 芯片掉过电（OSF=1：VBAT 断开/首次上电）⇒ 里面的时间不可信 */
bool svc_rtc_lost_power(void);

/* 读芯片时间（UTC epoch 秒）。总线错/数据非法返回 false */
bool svc_rtc_read(int64_t *epoch);

/* 写芯片时间（UTC epoch 秒），并清 OSF */
bool svc_rtc_write(int64_t epoch);

/* 立即把**系统时间**写入芯片（关机前 / 手动设时后） */
void svc_rtc_save_now(void);

/* 诊断：最近一次"系统 - 芯片"的绝对偏差（秒），-1 = 还没比过 */
int  svc_rtc_last_drift(void);

/* ★ 2026-11-21 诊断: 读芯片原始 7 字节时间寄存器 + 状态寄存器(0x0F)。
 *   用途: 分辨"关机期间芯片停走(VBAT 没电)" 还是 "走了但 OSF 被置位"。
 *   regs[0..6] = 秒/分/时/周/日/月/年 的原始 BCD; *status = 0x0F 原值。
 *   ⛔ 只读: 不改任何寄存器, 不清 OSF。 */
bool svc_rtc_dump(uint8_t regs[7], uint8_t *status);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_RTC_H */
