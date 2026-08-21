#ifndef TOUCH_PANEL_H
#define TOUCH_PANEL_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file touch_panel.h
 * @brief 电容触摸屏驱动 (自动识别 GT911 / CST816 / FT6236)
 *
 * 接线 (TP-VDD=3.3V, TP-GND=GND, 另外 TP-SDA/SCL 需 4.7~10k 上拉到 3.3V,
 * 若面板自带则无需外加):
 *   TP-SDA   -> GPIO 15
 *   TP-SCL   -> GPIO 7
 *   TP-INT   -> GPIO 17
 *   TP-RESET -> GPIO 2
 *
 * 坐标是面板原始分辨率, 本驱动只负责读点; 手势方向由调用方(input.c)按增量判定,
 * 因此无需映射到 400x300 屏幕坐标.
 */

/* ==== 触摸面板引脚定义 (可在此修改) ==== */
#define TP_SDA_PIN   GPIO_NUM_15
#define TP_SCL_PIN   GPIO_NUM_7
#define TP_INT_PIN   GPIO_NUM_17
#define TP_RST_PIN   GPIO_NUM_2

/* ==== 面板分辨率默认值 (原始坐标最大值) ====
 * GT911 会从配置寄存器 0x8048/0x804A 精确读出, 覆盖此默认值;
 * CST816/FT6236 无标准分辨率寄存器, 用此默认值映射到 400x300 屏幕.
 * 本机实测: 这块 CST816 面板 = 400x300 (与屏幕 1:1), 见 HANDOVER 6.12. */
#define TP_DEFAULT_RES_X   400
#define TP_DEFAULT_RES_Y   300

/* ==== 芯片类型 ==== */
typedef enum {
    TP_CHIP_NONE = 0,   /* 未检测到触摸芯片 */
    TP_CHIP_GT911,      /* Goodix GT911 (4.2"/4.3" 常见, 带 RESET) */
    TP_CHIP_CST816,     /* Hynitron CST816S/T */
    TP_CHIP_FT6236,     /* FocalTech FT6236 */
} tp_chip_t;

/* ==== 单个触摸点 ==== */
typedef struct {
    bool    pressed;   /* 是否有手指按下 */
    int16_t x;         /* 原始 X 坐标 */
    int16_t y;         /* 原始 Y 坐标 */
} tp_point_t;

/* 初始化: 配置 I2C + RESET/INT GPIO, 复位 GT911, 探测芯片型号.
 * 返回探测到的芯片类型; TP_CHIP_NONE 表示未检测到 (之后 read 恒返回 false). */
tp_chip_t touch_panel_init(void);

/* 读取一个触摸点 (每帧轮询一次). 返回 false 表示读取失败/无有效数据. */
bool touch_panel_read(tp_point_t *pt);

/* V1.0.68: 禁用触摸屏: 卸载 I2C 驱动(释放内存), 之后 read 恒 false. 可重新 init. */
void touch_panel_deinit(void);

/* V1.0.68 fix: 连续 I2C 读失败(芯片挂死/总线卡住)时自动恢复:
 * RST 脉冲复位芯片 + 重装 I2C 驱动 + 重新探测. 由 input.c 调用. */
void touch_panel_recover(void);

/* V1.0.68: 非阻塞请求触摸恢复 (input.c 连续读失败时调用, 由后台看门狗执行) */
void touch_panel_request_recover(void);

/* 返回当前检测到的芯片类型 (调试/日志用). */
tp_chip_t touch_panel_get_chip(void);

/* V1.0.69: 是否检测到触摸屏 (有触摸机型 = true, 无触摸机型 = false).
 * 供 input.c 按机型分支物理键语义用. */
bool touch_panel_is_present(void);

/* 返回面板分辨率 (原始坐标最大值), 用于把触摸点映射到屏幕坐标.
 * GT911 从寄存器读出; 其它芯片返回 TP_DEFAULT_RES_X/Y. */
void touch_panel_get_resolution(int *max_x, int *max_y);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_PANEL_H */
