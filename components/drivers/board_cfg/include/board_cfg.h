/* board_cfg.h — ★ LinTOS 载板引脚**唯一真相源** (2026-10-16 模块化)
 *
 * ★★ 换板只改下面一行 `CFG_BOARD_WEIXUE`, 重新编译即可, 全工程生效。
 * ⛔ 其它任何 .c/.h 里都不许再出现写死的 GPIO 号。
 */
#ifndef BOARD_CFG_H
#define BOARD_CFG_H
#include "soc/gpio_num.h"

#define CFG_BOARD_WEIXUE   0   /* 1 = 微雪 ESP32-S3-RLCD-4.2 原板 (无触摸)
                                * 0 = 小智 ESP32-S3-AI-2E 模块 + 新载板 */

#if CFG_BOARD_WEIXUE
/* ================= 微雪 ESP32-S3-RLCD-4.2 ================= */
#define CFG_LCD_DC        5
#define CFG_LCD_CS        40
#define CFG_LCD_SCK       11
#define CFG_LCD_MOSI      12
#define CFG_LCD_RST       (-1)   /* ★2026-10-16 微雪实物: DC 与 RESET 同为 GPIO5 ⇒ 固件不驱动 */
#define CFG_LCD_FREQ_MHZ  60
#define CFG_TP_SDA        (-1)   /* 本板无触摸 */
#define CFG_TP_SCL        (-1)
#define CFG_TP_INT        (-1)
#define CFG_TP_RST        (-1)
#define CFG_BTN_PWR       18     /* 板载 KEY */
#define CFG_BTN_RIGHT     0      /* 板载 BOOT */
#define CFG_BTN_LEFT      18
#define CFG_BAT_ADC_CHAN  3      /* ADC1_CH3 = GPIO4 */
#define CFG_I2S_MCLK      16
#define CFG_I2S_BCLK      9
#define CFG_I2S_WS        45
#define CFG_I2S_DOUT      8
#define CFG_I2S_DIN       10
#define CFG_CODEC_SDA     13
#define CFG_CODEC_SCL     14
#define CFG_PA_EN         46
#define CFG_VIB_PWM       8
#define CFG_SD_CLK        38
#define CFG_SD_CMD        21
#define CFG_SD_D0         39
#else
/* ================= 小智 ESP32-S3-AI-2E + 新载板 ================= */
#define CFG_LCD_DC        7
#define CFG_LCD_CS        11
#define CFG_LCD_SCK       9
#define CFG_LCD_MOSI      10
#define CFG_LCD_RST       (-1)
#define CFG_LCD_FREQ_MHZ  30
#define CFG_TP_SDA        5
#define CFG_TP_SCL        13
#define CFG_TP_INT        12
#define CFG_TP_RST        6
#define CFG_BTN_PWR       1
#define CFG_BTN_RIGHT     0
#define CFG_BTN_LEFT      4
#define CFG_BAT_ADC_CHAN  2      /* GPIO3 */
#define CFG_I2S_MCLK      40
#define CFG_I2S_BCLK      39
#define CFG_I2S_WS        45
#define CFG_I2S_DOUT      48
#define CFG_I2S_DIN       21
#define CFG_CODEC_SDA     38
#define CFG_CODEC_SCL     47
#define CFG_PA_EN         46
#define CFG_VIB_PWM       8
#define CFG_SD_CLK        16
#define CFG_SD_CMD        15
#define CFG_SD_D0         17
#endif

/* GPS 两板相同: TX/RX 43/44, EXTINT 不接(-1) —— ⛔ 绝不能设成 8(震动脚) */
#define CFG_GPS_TX        43
#define CFG_GPS_RX        44
#define CFG_GPS_EXTINT    (-1)

#endif /* BOARD_CFG_H */
