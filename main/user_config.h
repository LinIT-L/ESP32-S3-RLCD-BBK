#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/* LCD 引脚定义 (来自微雪官方 ESP32-S3-RLCD-4.2 示例, 真值源) */
#define LCD_WIDTH      400    /* 横屏 (U8G2_R1 旋转后) */
#define LCD_HEIGHT     300

#define RLCD_DC_PIN    GPIO_NUM_5
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_RST_PIN   GPIO_NUM_41

/* I2C (SHTC3/PCF85063, 模拟器暂不使用) */
#define ESP32_I2C_SDA_PIN   GPIO_NUM_13
#define ESP32_I2C_SCL_PIN   GPIO_NUM_14

/* 按键 GPIO
 * 已被 LCD 占用: 5(DC),11(SCK),12(MOSI),40(CS),41(RST)   [GPIO6 空闲, TE 线不接]
 * 已被 I2C 占用: 13(SDA),14(SCL)
 *
 * 板载按键 (来自 Waveshare ESP32-S3-RLCD-4.2 官方文档):
 *   BOOT = GPIO0 (输入, 上拉) → RIGHT
 *   KEY  = GPIO18 (侧边按键, 输入, 上拉) → LEFT / CONFIRM (短按/长按)
 *
 * 设为 GPIO_NUM_NC 表示该键未连接, 不扫描
 */
#define BTN_GPIO_UP         GPIO_NUM_NC
#define BTN_GPIO_DOWN       GPIO_NUM_NC
#define BTN_GPIO_LEFT       GPIO_NUM_18   /* 板载 KEY  = 左键 (短按) */
#define BTN_GPIO_RIGHT      GPIO_NUM_0    /* 板载 BOOT = 右键 */
#define BTN_GPIO_A          GPIO_NUM_18   /* 板载 KEY  = 确认键 (长按) */
#define BTN_GPIO_B          GPIO_NUM_18   /* 板载 KEY  = 退出键 (长按) */
#define BTN_GPIO_Y          GPIO_NUM_NC
#define BTN_GPIO_START      GPIO_NUM_NC

/* gam4980 LCD 尺寸 (4980/4988 原始, 159x96) */
#define GAM4980_LCD_WIDTH  159
#define GAM4980_LCD_HEIGHT 96

/* SD 卡挂载点 (VFS) — 8.BIN/E.BIN/.gam 放这里 */
#define SD_MOUNT_POINT     "/sdcard"
#define ROM_DIR            SD_MOUNT_POINT "/gam4980"

#endif
