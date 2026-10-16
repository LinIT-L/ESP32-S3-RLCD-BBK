/* board_cfg.h — ★ LinTOS 载板引脚**唯一真相源** (2026-10-16 模块化)
 *
 * ★★ 以后换板/改引脚, **只改这一个文件**, 全工程生效。
 * 数值来源 = LinTOS0.71(能亮) + 项目记忆, 逐条核对过。
 * ⛔ 其它任何 .c/.h 里都不许再出现写死的 GPIO 号。
 */
#ifndef BOARD_CFG_H
#define BOARD_CFG_H
#include "soc/gpio_num.h"

/* ---------- LCD (ST7305, 4线SPI) ---------- */
#define CFG_LCD_DC        7
#define CFG_LCD_CS        11
#define CFG_LCD_SCK       9
#define CFG_LCD_MOSI      10
#define CFG_LCD_RST       (-1)     /* 只 10k 上拉, 不接 MCU */
#define CFG_LCD_FREQ_MHZ  30

/* ---------- 触摸 (CST816, I2C) ---------- */
#define CFG_TP_SDA        5
#define CFG_TP_SCL        13
#define CFG_TP_INT        12
#define CFG_TP_RST        6

/* ---------- 按键 (低电平按下) ---------- */
#define CFG_BTN_PWR       1        /* SW1: 短按锁屏 / 长按返回 / 2s 软关机 */
#define CFG_BTN_RIGHT     0        /* SW2: BOOT */
#define CFG_BTN_LEFT      4        /* SW3: 兼"确认"  */

/* ---------- 电池 ADC (ADC1 通道号) ---------- */
#define CFG_BAT_ADC_CHAN  2        /* ADC1_CH2 = GPIO3 */

/* ---------- 音频 (ES8311+ES7210 走模块内部 I2S) ---------- */
#define CFG_I2S_MCLK      40
#define CFG_I2S_BCLK      39
#define CFG_I2S_WS        45
#define CFG_I2S_DOUT      48
#define CFG_I2S_DIN       21
#define CFG_CODEC_SDA     38       /* codec I2C */
#define CFG_CODEC_SCL     47
#define CFG_PA_EN         46       /* 功放 LPA2103A SD */

/* ---------- 震动 (TM6604 + 线性马达, 边沿触发) ---------- */
#define CFG_VIB_PWM       8

/* ---------- SD (SDMMC 1-bit) ---------- */
#define CFG_SD_CLK        16
#define CFG_SD_CMD        15
#define CFG_SD_D0         17

#endif /* BOARD_CFG_H */
