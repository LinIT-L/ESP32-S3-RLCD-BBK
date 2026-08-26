/**
 * os_theme.c — 主菜单桌面主题系统实现.
 * 内置主题表 (默认主题 = 当前视觉). 主题可后续由商店下载注册.
 */
#include "os_theme.h"
#include "ui_common.h"
#include <string.h>

/* 默认背景: 白底 */
static void th_bg_default(ui_ctx_t *ctx, st7305_handle_t *lcd) {
    (void)ctx;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
}

/* 内置主题表 (默认主题 = 当前视觉) */
static const os_theme_t s_themes[] = {
    {
        .name         = "\xe9\xbb\x98\xe8\xae\xa4",  /* 默认 */
        .icon_spacing = 100,
        .icon_center_y = UI_SCREEN_H / 2,
        .icon_center  = 100,
        .icon_normal  = 64,
        .label_dy     = 68,
        .draw_bg      = th_bg_default,
        .draw_icon    = NULL,   /* 回退 page_main 默认 SF 图标 */
        .draw_label   = NULL,   /* 回退 page_main 默认标签 */
        .bg_color     = ST7305_COLOR_WHITE,
    },
};
#define THEME_COUNT ((int)(sizeof(s_themes) / sizeof(s_themes[0])))

static int s_cur = 0;

void os_theme_set(int id) {
    if (id < 0 || id >= THEME_COUNT) return;
    s_cur = id;
}

const os_theme_t *os_theme_get(void) { return &s_themes[s_cur]; }
int os_theme_current(void) { return s_cur; }

int os_theme_count(void) { return THEME_COUNT; }

const char *os_theme_name(int id) {
    if (id < 0 || id >= THEME_COUNT) return "";
    return s_themes[id].name;
}
