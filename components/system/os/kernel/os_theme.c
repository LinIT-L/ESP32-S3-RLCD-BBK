/**
 * os_theme.c — 主菜单桌面主题系统实现.
 *
 * 设计: **框架统一、参数由主题给** —— 菜单渲染/命中/拖动三处的几何全部从 os_theme_get() 取,
 * 所以"改一个框架、整体跟着变", 不需要逐个页面改。主题只描述"怎么画"(间距/尺寸/背景/装饰),
 * 不描述"画什么"(图标语义顺序 = main_icons[] 下标 = icon_idx ABI, 主题绝不能动)。
 *
 * 选择与持久化: ID 存 NVS (命名空间见 SET_NS, 键 "menu_theme")。
 * 惰性加载: 首次 os_theme_get() 时读一次 NVS, 因此不依赖启动顺序。
 */
#include "os_theme.h"
#include "theme_renderer.h"
#include "ui_common.h"
#include "svc_cfg.h"
#include <string.h>

#define THEME_NS "os_settings"       /* ⚠ 与 page_settings 的 SET_NS 同一个命名空间, 别写错 */
#define THEME_KEY "menu_theme"

/* ---- 背景钩子 ---- */
static void th_bg_plain(ui_ctx_t *ctx, st7305_handle_t *lcd) {
    (void)ctx;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
}

/* 内置主题表: **当前只有"默认"一项**。
 *
 * 2026-09-22: 曾试加过"紧凑/宽松/描边"三个只调参数的演示主题, linit 要求删除 ——
 * 他要的主题是**完全不同的视觉方案**, 不是格距/尺寸的简单缩放, 所以不摆凑数项。
 * 真正的主题(以及"下载主题")待后续设计; 本文件保留注册/选择/持久化插口给它们用:
 *   · 加一个主题 = 往 s_themes[] 里加一项 (布局参数 + draw_bg/draw_icon/draw_label 钩子)
 *   · 选择 = os_theme_set(i) + os_theme_save(); 设置页加一项滚轮/列表即可
 *   · ⛔ 主题只能改"怎么画", 不能动 main_icons[] 的顺序 (那是 icon_idx 的 ABI)
 * ⚠ 第 0 项必须与历史视觉**完全一致** (默认主题 = 零回归基线)。 */
static const os_theme_t s_themes[] = {
    {
        .name         = "\xe9\xbb\x98\xe8\xae\xa4",  /* 默认 */
        .icon_spacing = 100,
        .icon_center_y = UI_SCREEN_H / 2,
        .icon_center  = 100,
        .icon_normal  = 64,
        .label_dy     = 68,
        .draw_bg      = th_bg_plain,
        .draw_icon    = NULL,   /* 回退 page_main 默认 SF 图标 */
        .draw_label   = NULL,   /* 回退 page_main 默认标签 */
        .bg_color     = ST7305_COLOR_WHITE,
    },
};
#define THEME_COUNT ((int)(sizeof(s_themes) / sizeof(s_themes[0])))

static int  s_cur = 0;
static bool s_loaded = false;

/* 从 NVS 读一次当前主题 (惰性; 越界/无记录则回落到默认 0) */
void os_theme_load(void) {
    if (s_loaded) return;
    s_loaded = true;
    int v = (int)svc_cfg_get_u8(THEME_NS, THEME_KEY, 0);
    if (v >= 0 && v < THEME_COUNT) s_cur = v;
}

/* 写回 NVS (掉电保留), 走 svc_cfg 薄封装, 不直接调 nvs 原始 API。 */
void os_theme_save(void) {
    svc_cfg_set_u8(THEME_NS, THEME_KEY, (uint8_t)s_cur);
}

void os_theme_set(int id) {
    if (id < 0 || id >= THEME_COUNT) return;
    s_cur = id;
}

const os_theme_t *os_theme_get(void) {
    os_theme_load();
    return &s_themes[s_cur];
}
int os_theme_current(void) { os_theme_load(); return s_cur; }

int os_theme_count(void) { return THEME_COUNT; }

const char *os_theme_name(int id) {
    if (id < 0 || id >= THEME_COUNT) return "";
    return s_themes[id].name;
}

/* ============ 主题引擎绑定 (P0) ============
 * 页面(page_main) 提供 host, 引擎自己注册; os_theme 只做"当前引擎"的存放与转交。
 * ⚠ 引擎不能反向依赖 apps 组件 —— 所以这里只存指针, 不 #include 具体引擎。 */
static const theme_host_t     *s_host = NULL;
static const theme_renderer_t *s_rd   = NULL;

void os_theme_bind(const theme_host_t *host, const theme_renderer_t *rd) {
    if (host) s_host = host;
    if (rd)   s_rd   = rd;
}

const theme_host_t *os_theme_host(void) { return s_host; }

const theme_renderer_t *os_theme_renderer(void) { return s_rd; }
