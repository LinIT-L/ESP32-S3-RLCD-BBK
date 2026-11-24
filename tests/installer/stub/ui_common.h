/* 宿主机测试用的 ui_common.h 替身 —— 安装器只在注册第三方应用时用两个符号：
 *   ICON_APP_MGR（没有图标时的默认图标）与 ui_icon_register_extra（注册包内位图）。 */
#ifndef TEST_STUB_UI_COMMON_H
#define TEST_STUB_UI_COMMON_H

#include <stdint.h>

#define ICON_APP_MGR 0
#define ICON_MINI_GAMES 1
#define ICON_TERMINAL 2
#define ICON_SETTINGS 3
#define ICON_BOOK 4
#define ICON_WALLPAPER 5

int ui_icon_register_extra(const uint8_t *bmp512);

#endif
