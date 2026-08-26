/**
 * os_theme.h — 主菜单桌面主题系统.
 *
 * 把桌面视觉从 page_main 逻辑中解耦: 背景/图标/标签 均可由主题接管,
 * 布局参数 (图标间距/位置/尺寸) 也可由主题指定, 用于"主题调整图标位置".
 * 逻辑层 (相机/拖动/惯性/选中动画) 完全不动.
 *
 * 用法:
 *   os_theme_set(id);              [切换主题]
 *   const os_theme_t *t = os_theme_get();  [桌面渲染读取]
 *   主题设置界面: os_theme_count()/os_theme_name(i) 列出, 选择后 os_theme_set.
 */
#ifndef OS_THEME_H
#define OS_THEME_H

#include "st7305.h"
#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct os_theme os_theme_t;
struct os_theme {
    const char *name;   /* 主题名 (UTF-8) */

    /* ==== 布局参数 (图标位置调整接口) ==== */
    int  icon_spacing;  /* 图标间距 px (默认 100) */
    int  icon_center_y; /* 图标中心 Y (默认 屏高/2) */
    int  icon_center;   /* 中央图标尺寸 (默认 100) */
    int  icon_normal;   /* 其余图标尺寸 (默认 64) */
    int  label_dy;      /* 选中标签相对图标中心下偏移 (默认 68) */

    /* ==== 视觉钩子 (NULL=默认实现) ==== */
    void (*draw_bg)(ui_ctx_t *ctx, st7305_handle_t *lcd);  /* 背景 */
    void (*draw_icon)(ui_ctx_t *ctx, st7305_handle_t *lcd, /* 图标 */
                      int icon_idx, int cx, int cy, int size, bool selected);
    void (*draw_label)(ui_ctx_t *ctx, st7305_handle_t *lcd, /* 标签 */
                       const char *label, int cx, int cy, bool selected);
    st7305_color_t bg_color;   /* 背景色 (默认白) */
};

/* 切换主题 (id 越界则忽略, 保持当前) */
void os_theme_set(int id);
/* 当前主题 */
const os_theme_t *os_theme_get(void);
/* 当前主题索引 */
int os_theme_current(void);
/* 主题总数 / 主题名 (设置列表用) */
int  os_theme_count(void);
const char *os_theme_name(int id);

#ifdef __cplusplus
}
#endif

#endif /* OS_THEME_H */
