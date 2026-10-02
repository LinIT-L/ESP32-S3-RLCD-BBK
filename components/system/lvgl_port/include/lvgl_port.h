/**
 * lvgl_port.h — LVGL 9.x → LinTOS 移植层 (400×300 单色反射屏)
 *
 * 只做"让 LVGL 能在本机跑起来"的四件事:
 *   ① 显示对象  : 400×300, LV_COLOR_FORMAT_I1 (1bpp), 双缓冲, 缓冲在 PSRAM
 *   ② flush 回调: 把 LVGL 的 1bpp 缓冲批量写进 ST7305 fb, 最后一块刷屏
 *   ③ tick+主循环: esp_timer 每 2ms 喂 lv_tick_inc; 独立任务跑 lv_timer_handler
 *   ④ 输入      : 触摸 pointer indev (接 input_get_touch_pos)
 *
 * 界面构建不由本层负责 —— C 侧直接写 lv_obj_*, Lua 侧走 lv_obj 绑定(后续)。
 */
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 LVGL 并接上显示/输入/心跳。可重复调用(幂等)。
 *  必须在 LCD 已初始化(st7305_init)之后调用。
 *  @param lcd 已初始化的 ST7305 句柄 (借用, 不接管生命周期) */
esp_err_t lvgl_port_init(st7305_handle_t *lcd);

/** 是否已初始化成功 */
bool lvgl_port_ready(void);

/** 取 LVGL 显示对象 (供创建屏幕/控件用), 未初始化时返回 NULL */
struct _lv_display_t *lvgl_port_display(void);

/** 暂停/恢复 LVGL 主循环任务 (进引擎游戏等需要独占屏幕时用) */
void lvgl_port_pause(bool paused);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
