/* hw_cfg.h — ★ 一块固件通吃三块板: 开机探测芯片指纹 ⇒ 自动定板型 + 自动启用硬件
 * 板型指纹(按命中概率排序, 命中即止):
 *   ① 小智   : I2C 38/47 上有 0x18 (ES8311)
 *   ② 微雪   : I2C 13/14 上有 0x18 (ES8311)
 *   ③ 自制   : I2C 15/7  上有 0x15 (CST816 触摸)
 * 都不中 ⇒ 用 NVS 上次的 board_id 兜底 ⇒ 再无 ⇒ 默认小智(不会变砖)
 */
#ifndef HW_CFG_H
#define HW_CFG_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 板型 */
enum { HW_BOARD_XIAOZHI = 0, HW_BOARD_WEIXUE = 1, HW_BOARD_HOMEMADE = 2 };

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
uint8_t hw_cfg_det_mask(void);
uint8_t hw_cfg_en_mask(void);

/* 供 board_pins 运行时选板 (取代原来的编译期 BOARD_IS_WEIXUE) */
void hw_cfg_board_apply(void);

#ifdef __cplusplus
}
#endif
#endif /* HW_CFG_H */
