/* hw_cfg.h — ★ 一个固件通吃三块板: 开机探测芯片指纹 ⇒ 自动定板型 + 自动启用硬件
 * 板型指纹(按命中概率排序, 命中即止):
 *   ① 小智一体版 : I2C 38/47 上有 0x18 (ES8311)
 *   ② 微雪原版   : I2C 13/14 上有 0x18 (ES8311)
 *   ③ 标准扩展版 : I2C 15/7  上有 0x15 (CST816 触摸)
 * ★ **只探测一次**（linit 2026-10-18）：一块板刷一份固件，硬件不会自己变。
 *   NVS 有 `board_id`+`det_mask` ⇒ 直接用，开机 **0 次 I2C 访问**；
 *   没有 ⇒ 探一次并把两者一起记下。以后要重探只能走设置页「重新自检」。
 */
#ifndef HW_CFG_H
#define HW_CFG_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ★★★ 配置格式版本 (linit 2026-10-16): 只要"配置语义"有变化就 +1。
 *   固件启动时比对 NVS 里的 cfg_ver, 不一致 ⇒ 自动作废旧存档(尤其 en_mask),
 *   彻底避免"刷固件后触摸/按键行为变了"这类问题。
 *   5: 三块板正式入表(标准扩展版), 音频不再按板型关闭。 */
#define HWCFG_VER 6

/* 板型（与 board_id_t 数值对齐：0/1/2） */
enum { HW_BOARD_XIAOZHI = 0, HW_BOARD_WEIXUE = 1, HW_BOARD_STD = 2,
       HW_BOARD_HOMEMADE = HW_BOARD_STD };   /* 旧名，保留兼容 */

/* 硬件项(位序与 en_mask/det_mask 一致) */
typedef enum {
    HW_TOUCH = 0, HW_SD, HW_AUDIO, HW_BATTERY, HW_GPS, HW_VIB, HW_COUNT
} hw_id_t;

int  hw_cfg_init(void);                    /* 探测+定板型+载NVS; 返回板型 */
int  hw_cfg_board_id(void);                /* 0=小智 1=微雪 2=自制 */
const char *hw_cfg_board_name(void);
bool hw_cfg_detected(hw_id_t id);          /* 探测到没 */
bool hw_cfg_enabled(hw_id_t id);           /* 该不该启动(默认=detected) */
void hw_cfg_set_enabled(hw_id_t id, bool on);  /* 设置页改, 立即存 NVS */
void hw_cfg_rescan(void);                  /* 重新自检 */
void hw_cfg_set_board(int id);             /* 手动指定板型 0=小智 1=微雪 2=标准扩展 (落盘, 重启完全生效) */
uint8_t hw_cfg_det_mask(void);
uint8_t hw_cfg_en_mask(void);

/* ==== GPS 选装件的一次性识别 ====
 * GPS 不能用 I2C 指纹判, 又不是每块板都焊 ⇒ 单独记一个"探过没"。
 *   首次开机: hw_cfg_gps_probed()==false → 上层后台跑一次真探测(听 UBX/NMEA);
 *   探到 → present 落盘(det/开 置位); 没探到 → 记"未识别", 以后开机不再自动尝试;
 *   要再试: 设置→硬件 → 点 GPS 那一行 / 重新自检 ⇒ hw_cfg_gps_invalidate() 后再探。 */
bool hw_cfg_gps_probed(void);
void hw_cfg_set_gps_present(bool present);
void hw_cfg_gps_invalidate(void);

/* 供 board_pins 运行时选板 (取代原来的编译期 BOARD_IS_WEIXUE) */
/* 自适应轮询 (linit 2026-10-22): 开机识别不到触摸时, 每 10 秒换一个目标反复试。
 *   which: 0=本板触摸组 1=小智codec(38/47) 2=微雪codec(13/14)
 *   probe 返回 1 = 命中(值得提交)。commit 会落盘并重启(不返回)。
 *   ⛔ 只探当前板型自己的引脚, 不跨板 (跨板会破坏 LCD 引脚)。 */
/* 手动指定过板型(串口 board xxx / 设置里选过) ⇒ 开机不再自动轮询/改板型。
 *   清掉的唯一入口: hw_cfg_rescan() (设置 → 硬件管理 → 重新自检)。 */
bool hw_cfg_board_forced(void);
/* 本次开机硬件指纹全没命中 ⇒ main 进"识别轮询界面", 识别成功才进系统 */
bool hw_cfg_identify_failed(void);

/* 板型已固化, 但本次开机实测触摸没应答 ⇒ main 起后台补探任务 (不卡系统)。
 * 补探命中后调 hw_cfg_touch_commit(true) 把触摸位写回 det/en。 */
bool hw_cfg_touch_missing(void);
void hw_cfg_touch_commit(bool present);
/* ★ 2026-10-27: 开机不再 I2C 探测(存档即信), 由触摸驱动 init 结果兜底:
 *   驱动 init 失败时调本函数 ⇒ 置 missing ⇒ main 起后台补探。 */
void hw_cfg_touch_mark_missing(void);

int  hw_cfg_probe_fingerprint(int which);
void hw_cfg_adapt_commit(int which);

void hw_cfg_board_apply(void);

#ifdef __cplusplus
}
#endif
#endif /* HW_CFG_H */
