#ifndef USER_CONFIG_H
/* ★ 2026-10-13 按键引脚唯一真相源 = components/system/board_pins/include/board_pins.h */
#include "board_pins.h"

#define USER_CONFIG_H

/* LCD 引脚定义 (来自微雪官方 ESP32-S3-RLCD-4.2 示例, 真值源) */
#define LCD_WIDTH      400    /* 横屏 (U8G2_R1 旋转后) */
#define LCD_HEIGHT     300

/* LCD 引脚 —— ★ 2026-10-16: 真相源已统一到 `components/system/board_pins/include/board_pins.h`。
 * ⛔ 下面这些以前是**死宏**: 全工程没有任何代码引用, 改它"屏不刷新"就是这个原因
 *    (驱动真正用的是 `st7305_default_config()` 里硬编码的值, 现由 app_board 从 board_pins 下发)。
 *    现在它们只是 board_pins 的别名, 仅作兼容书写; **换板请改 board_pins.h 的板型表**。 */
#define RLCD_DC_PIN    BOARD_LCD_DC
#define RLCD_CS_PIN    BOARD_LCD_CS
#define RLCD_SCK_PIN   BOARD_LCD_SCK
#define RLCD_MOSI_PIN  BOARD_LCD_MOSI
#define RLCD_RST_PIN   BOARD_LCD_RST

/* I2C (SHTC3/PCF85063, 模拟器暂不使用) */
#define ESP32_I2C_SDA_PIN   GPIO_NUM_NC
#define ESP32_I2C_SCL_PIN   GPIO_NUM_NC

/* 按键 GPIO
 * 已被 LCD 占用: 5(DC),11(SCK),12(MOSI),41(RST),**42(CS)**
 *   (2026-10-04 小智 ESP32-S3 核心模块板: 模块把 IO40 占了 ⇒ 屏 CS 从 40 挪到 42;
 *    42 原来归震动 PWM, 新板震动电源默认常开 ⇒ 震动只用 GPIO48 一根线, 42 让出)
 * 已被 I2C 占用: 13(SDA),14(SCL)
 * V1.0.70: tone_player 改走 I2S(PCM 合成→ES8311/NS4168), GPIO6 释放为闲置
 * (旧: 6 曾为 tone_player 方波输出; 原 GPIO48 与旧震动 EN 冲突已移脚, 现 48 归震动 PWM)
 *
 * 板载按键 (来自 Waveshare ESP32-S3-RLCD-4.2 官方文档):
 *   BOOT = GPIO0 (输入, 上拉) → RIGHT
 *   KEY  = GPIO18 (侧边按键, 输入, 上拉) → LEFT / CONFIRM (短按/长按)
 *
 * 设为 GPIO_NUM_NC 表示该键未连接, 不扫描
 */
#define BTN_GPIO_UP  BOARD_BTN_UP
#define BTN_GPIO_DOWN  BOARD_BTN_DOWN
#define BTN_GPIO_LEFT  BOARD_BTN_LEFT   /* 板载 KEY  = 左键 (短按) */
#define BTN_GPIO_RIGHT  BOARD_BTN_RIGHT    /* 板载 BOOT = 右键 */
#define BTN_GPIO_A  BOARD_BTN_A   /* 板载 KEY  = 确认键 (长按) */
#define BTN_GPIO_B  BOARD_BTN_B   /* 板载 KEY  = 退出键 (长按) */
#define BTN_GPIO_Y          GPIO_NUM_NC
#define BTN_GPIO_START      GPIO_NUM_NC

/* gam4980 LCD 尺寸 (4980/4988 原始, 159x96) */
#define GAM4980_LCD_WIDTH  159
#define GAM4980_LCD_HEIGHT 96

/* SD 卡挂载点 (VFS) — 8.BIN/E.BIN/.gam 放这里 */
#define SD_MOUNT_POINT     "/sdcard"
#define ROM_DIR            SD_MOUNT_POINT "/gam4980"

#endif
