#ifndef VIRTUAL_KEYS_H
#define VIRTUAL_KEYS_H

#include "st7305.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === 游戏内屏幕虚拟按键 (V1.0.68) ===
 * 与 WiFi 网页手柄同款布局: 左侧方向键 + 右侧 A/B + 底部 Select/Start.
 * 游戏设置里可开关; 开启后游戏画面底部叠加半透明风格按键 (1bit 反色 LCD 无真透明,
 * 用白色底 + 黑色描边表达, 按下反色), 手指按住屏幕按键即产生对应 joypad 输入.
 *
 * joypad 掩码 (低电平有效, 与 GB/web 手柄一致):
 *   bit0=A  bit1=B  bit2=Select  bit3=Start
 *   bit4=右 bit5=左 bit6=上      bit7=下
 * 0xFF = 全部松开; 某位清 0 = 对应键按下. */

/* 开关 (菜单进入/退出游戏时设置) */
void virtual_keys_set_enabled(bool en);
bool virtual_keys_is_enabled(void);

/* 每帧调用: 传入当前触摸 (屏幕坐标 400x300, down=false 表示无手指),
 * 更新按键状态并返回 joypad 掩码. 不读触摸芯片, 只用输入层缓存的坐标. */
uint8_t virtual_keys_poll(int touch_x, int touch_y, bool touch_down);

/* 读取最近一次 poll 的 joypad 掩码 (不重新 hit-test) */
uint8_t virtual_keys_get_joypad(void);

/* 在 fb 上叠加绘制虚拟按键 (调用前 fb 已含游戏帧, 调用后需 st7305_flush) */
void virtual_keys_draw(st7305_handle_t *lcd);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUAL_KEYS_H */
