/* ltc2954.h — LTC2954 硬件电源开关控制器集成 (2026-11-13)
 *
 * ★ 四版本定案接法(2026-11-28, 以三份权威网表为准):
 *   S1_4 关机版: U6.2 /PB ← SW1 (机械开机); U6.5 /INT → GPIO1; U6.8 KILL → GPIO44
 *                + U1 DS3231(0x68)  ⇒ 有 0x68 才拉 KILL 真断电
 *   S1_1 老版  : SW1.1 → GPIO1 电源键; 无 2954 / 无 DS3231 ⇒ 不碰 KILL, 关机走深睡
 *   微雪 / 标准: 无 2954 (pwr_kill=-1) ⇒ 本模块整体禁用
 *   ⚠ KILL/INT 脚号从 board_pins 表动态取(ltc2954_kill_pin/int_pin), 不写死。
 *
 * 本模块不接管 /INT: 两版小智电源键都是 GPIO1, 由 input.c 当普通电源键轮询
 *   (开漏低有效 + 内部上拉, "低=按下"语义不变, 长短按 0.5s/2s 判定不变)。
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
 *   · 设置开关只管"固件是否响应关机请求"，**不影响 KILL 的安全电平** ——
 *     开关关掉时 KILL 仍保持"不断电"状态，绝不会因为关开关反而把整机断掉。
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
/* ★ 2026-11-28 四版本改造: KILL/INT 脚从 board_pins 表**动态取**, 不再编译期写死。
 *   小智 S1_4 关机版: KILL=GPIO44(/INT=GPIO1); 微雪/标准无 2954 ⇒ 表里为 -1。
 *   ⚠ S1_4 的 GPIO42 是电源节点(AO3415A 漏极 + VSYS_IN), 绝不能当 KILL 拉低。 */
#include "board_pins.h"
static inline int ltc2954_kill_pin(void) {
    const board_pins_t *bp = board_pins_get();
    return bp ? bp->pwr_kill : -1;
}
static inline int ltc2954_int_pin(void) {
    const board_pins_t *bp = board_pins_get();
    return bp ? bp->pwr_int : -1;
}

/* /INT 接线记录(不接管): 两版小智电源键都是 GPIO1 ——
 *   S1_1(老): SW1.1→U2.39(IO1) 直连;
 *   S1_4(关机版): U6.5(/INT)→U2.39(IO1), 由 input.c 当电源键轮询。
 *   本模块不配置/不读取它, 一个脚不能两个主人。 */


/* ---- NVS（沿用设置页现有 namespace，不新建） ---- */
#define LTC2954_CFG_NS      "os_settings"
#define LTC2954_CFG_KEY     "hw_off"   /* 0=关(默认) 1=开; 只给 remcon 调试用, UI 已无此开关 */
#define LTC2954_CFG_MANUAL  "hw_off_manual"  /* 历史键: 仅 ltc2954_set_enabled() 写入, 判定不再读 */

/* 开机调用一次（幂等）。会做板型冲突检查；冲突时返回 ESP_ERR_NOT_SUPPORTED
 * 且**完全不碰** KILL 脚，避免破坏其他板型的 GPS/触摸。 */
esp_err_t ltc2954_init(void);

/* 硬件是否可用（引脚存在且与当前板型不冲突）。 */
bool ltc2954_is_hw_available(void);

/* 设置页开关状态（NVS 持久化，开机恢复）。 */
bool ltc2954_is_enabled(void);
void ltc2954_set_enabled(bool en);

/* ★ 2026-11-21 linit「检测到时钟模块就自动启用 2954 断电」:
 *   板上装了 DS3231(0x68) 就说明是 S1_4 硬件开关机版, hw_off 应默认开。
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
 *   开关关闭 → 只把 KILL 放回安全电平（不断电），关机交给原有深睡。 */
void ltc2954_power_cut(void);

#ifdef __cplusplus
}
#endif

#endif /* LTC2954_H */
