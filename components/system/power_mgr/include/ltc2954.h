/* ltc2954.h — LTC2954 硬件电源开关控制器集成 (2026-11-13)
 *
 * ★ 定案接法(2026-11-21 实测通过, PCB1_3):
 *     SW1 非地端 ──→ U6 第 2 脚 /PB     纯机械开机; 模块不参与这条通路
 *     U6 第 5 脚 /INT ──→ GPIO44        由 input.c 当普通电源键轮询(btn_pwr=44, 见下)
 *     U6 第 8 脚 KILL ──→ GPIO42        固件拉低即断电(本模块只管这一根)
 *     D1(肖特基 S4) ── 拆除不装         它把 /PB 接回 GPIO1, 是"关机 1s 后自动复电"的根因
 *
 * INT_GPIO = -1 的含义是 **"本模块不接管 /INT"**, 不是"没接":
 *   /INT 已经接到 GPIO1, 由 input.c 当作普通电源键轮询(开漏低有效 + 内部上拉,
 *   "低=按下"的语义与原来完全一致, 长短按 0.5s/2s 判定不变)。
 *   本模块若再去配置/轮询同一个脚, 只会和 input.c 抢所有权。
 *
 * ⛔ 历史结论留档(避免重犯):
 *   ① 曾用 /INT 做"用户要求关机"的触发源 ⇒ 按一下电源键就整机断电, 体验是"重启"。已废。
 *   ② 曾写"USB 5V 绕过 AO3415A 给模块供电" ⇒ **错**。Q1 的 2/3 脚断开就是彻底断电,
 *      实测 `pwrstate` 报 reset=1(POWERON)、RTC 域丢失即为铁证。
 *   ③ 曾怀疑"2954 自己掉电导致 /EN 复位" ⇒ **错**。真因是 D1 把掉电中的 GPIO1
 *      反过来拽低了 /PB, 芯片据此认为"又按了一次键"。拆掉 D1 后不再复电。
 *   ④ 曾怀疑"PDT 锁不住、必须加 2N7002 自锁" ⇒ 不需要, 硬件本身是好的。
 *
 * 设计原则:
 *   · 复用 power_mgr_off() 这条**唯一**安全关机流程，本模块只负责"发通知 / 收通知 / 最后断电"。
 *   · 设置开关只管"固件是否响应关机请求"，**不影响 GPIO42 的安全电平** ——
 *     开关关掉时 GPIO42 仍保持"不断电"状态，绝不会因为关开关反而把整机断掉。
 *   · 断电只走 ltc2954_power_cut() 一处，重复调用只是把 KILL 再拉低一次（幂等）。
 *   · 硬件自身的长按强制断电不受固件影响（LTC2954 内部定时器直连 EN），本模块不干预。
 */
#ifndef LTC2954_H
#define LTC2954_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 引脚（★ 2026-11-15 按新小智板网表确定） ---- */
#define LTC2954_KILL_GPIO   42   /* 输出: 硬件断电    —— 模组 35 脚 IO42 */

/* /INT 的实物接线（网表 Netlist_PCB1_3 + 实测：按 PB 时 pin44 1→0→1、pin1 恒高）:
 *     U6.5 /INT ──→ GPIO44 ──→ input.c 当电源键轮询（board_pins.c: btn_pwr=44）
 *   本模块**不接管**这个脚，也不据它产生关机请求:
 *     · 一个脚不能两个主人 —— input.c 已经在读它做 0.5s/2s 判定;
 *     · 若拿 /INT 当"按下即断电"的触发源，按一下电源键就整机断电，体验等于"重启";
 *     · 关机的唯一入口是 input.c 的 2s 长按 → 确认框 → power_mgr_off()。
 *   ⛔ 这里只是接线记录，不代表固件读它。要改这条语义必须同时改 input.c 的所有权。 */
#define LTC2954_INT_GPIO    44   /* 仅作文档: /INT 所在脚，本模块不配置 */

/* ---- NVS（沿用设置页现有 namespace，不新建） ---- */
#define LTC2954_CFG_NS      "os_settings"
#define LTC2954_CFG_KEY     "hw_off"   /* 0=关(默认) 1=开; 只给 remcon 调试用, UI 已无此开关 */
#define LTC2954_CFG_MANUAL  "hw_off_manual"  /* 历史键: 仅 ltc2954_set_enabled() 写入, 判定不再读 */

/* 开机调用一次（幂等）。会做板型冲突检查；冲突时返回 ESP_ERR_NOT_SUPPORTED
 * 且**完全不碰** GPIO42，避免破坏其他板型的 GPS/触摸。 */
esp_err_t ltc2954_init(void);

/* 硬件是否可用（引脚存在且与当前板型不冲突）。 */
bool ltc2954_is_hw_available(void);

/* 设置页开关状态（NVS 持久化，开机恢复）。 */
bool ltc2954_is_enabled(void);
void ltc2954_set_enabled(bool en);

/* ★ 2026-11-21 linit「检测到时钟模块就自动启用 2954 断电」:
 *   板上装了 DS3231(0x68) 就说明是 PCB1_3 硬件开关机版, hw_off 应默认开。
 *   由 main 在 svc_rtc 尝试挂载之后调用(判定 hw_cfg_detected(HW_RTC) 放 main 侧 ——
 *   power_mgr 已经 REQUIRES svc_rtc, 反过来依赖会成环)。
 *   幂等: 已经开着就什么都不做。判据只有"时钟芯片在不在"，不再读任何用户开关。 */
void ltc2954_autoconf_from_rtc(void);

/* ★ 2026-11-20 linit「关机确认框点确认就真断电」:
 *   只对**本次**关机生效, 不写 NVS、不改设置页那个开关的状态。
 *   理由: 板上焊了 LTC2954 就该真断电, 不该因为开关默认关而退化成深睡;
 *         但也不能反过来把用户的"关"覆盖掉(那等于开关失效)。
 *   内部仍要求 hw_ok(引脚与板型不冲突), 不可用时自然回落到深睡。 */
void ltc2954_force_cut_once(void);
bool ltc2954_is_cut_requested(void);   /* enabled || 本次强制 */

/* 关机流程末尾调用（power_mgr_off 内，深睡之前）:
 *   开关开启 → 拉低 KILL 触发真断电，并等待若干毫秒；
 *   开关关闭 → 只把 GPIO42 放回安全电平（不断电），关机交给原有深睡。 */
void ltc2954_power_cut(void);

#ifdef __cplusplus
}
#endif

#endif /* LTC2954_H */
