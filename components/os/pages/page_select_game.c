/**
 * page_select_game.c — 游戏 页面模块 (3.3 游戏菜单样式复刻).
 *
 * 双栏统一模板 os_pane:
 *   左栏: [引擎优化] [全部?] [子文件夹...]  (引擎优化在列表最上方, 非单独按钮)
 *   右栏: 引擎优化 → 内联设置项 (直接切换值连 API, 不弹窗); 其余 → 游戏列表
 * 全屏 (不显示状态栏); 启动游戏走 gam4980 引擎 (阻塞运行).
 * 私有 state 一律 static 留在本文件.
 */
#include "os.h"
#include "os_pane.h"
#include "ui_common.h"
#include "gam4980_emu.h"
#include "engine_manager.h"
#include "input.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

#define TAG "PGAME"

#define GAM_ROOT "/sdcard/gam"

/* ==== 引擎优化设置 (3.3 游戏设置: 状态栏设置/显示模式/虚拟按键/抗锯齿) ====
 * 用 NVS 持久化 (namespace os_gset), 启动游戏时应用到 gam4980. */
typedef struct {
    uint8_t display_mode;   /* 0=点对点 1=全屏 2=拉伸 */
    uint8_t pic_opt;        /* 0=关 1=EPX 抗锯齿 */
    uint8_t vkey;           /* 0=关 1=开 2=自动 */
    uint8_t statusbar;      /* 0=隐藏 1=显示 */
} gs_t;
static gs_t s_gs;

static bool is_gam(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".gam") == 0);
}

/* 右栏确认: 启动游戏 (对齐旧版 3.3 模板: init → load → run). */
static void on_game_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动: %s", path);
    esp_err_t r = gam4980_emu_init(ctx->lcd);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "emu 初始化失败: %s", esp_err_to_name(r));
        return;
    }
    if (gam4980_emu_load(path) != 0) {
        ESP_LOGE(TAG, "游戏加载失败: %s", path);
        return;
    }
    /* 引擎优化应用到引擎 (显示模式/虚拟按键/游戏内状态栏) */
    gam4980_set_fullscreen((int)s_gs.display_mode);
    gam4980_set_vkey((int)s_gs.vkey);
    gam4980_set_status_bar(s_gs.statusbar != 0);
    input_set_gamepad_nav_enabled(false);
    gam4980_emu_run();
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;   /* 游戏退出后重绘列表 */
}

/* ==== 引擎优化: NVS 持久化 ==== */
#define GSET_NS "os_gset"

static void gs_load(void) {
    /* 默认值 (用户要求): 状态栏设置=显示, 显示模式=全屏, 虚拟按键=自动, 抗锯齿=关 */
    s_gs.display_mode = 1; s_gs.pic_opt = 0; s_gs.vkey = 2; s_gs.statusbar = 1;
    nvs_handle_t h;
    if (nvs_open(GSET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "dm", &v) == ESP_OK && v <= 2) s_gs.display_mode = v;
        if (nvs_get_u8(h, "aa", &v) == ESP_OK && v <= 1) s_gs.pic_opt = v;
        if (nvs_get_u8(h, "vk", &v) == ESP_OK && v <= 2) s_gs.vkey = v;
        if (nvs_get_u8(h, "sb", &v) == ESP_OK && v <= 1) s_gs.statusbar = v;
        nvs_close(h);
    }
}
static void gs_save(void) {
    nvs_handle_t h;
    if (nvs_open(GSET_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dm", s_gs.display_mode);
        nvs_set_u8(h, "aa", s_gs.pic_opt);
        nvs_set_u8(h, "vk", s_gs.vkey);
        nvs_set_u8(h, "sb", s_gs.statusbar);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 3.3: 全角空格补齐到 5 字宽, 使所有冒号竖直对齐 */
#define GS_FULL_SPACE "\xe3\x80\x80"
static void gs_pad_label(char *buf, size_t len, int target, const char *name, const char *val) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    int pad = (n < target) ? target - n : 0;
    int pos = 0;
    for (int i = 0; i < pad && pos < (int)len; i++)
        pos += snprintf(buf + pos, len - (size_t)pos, "%s", GS_FULL_SPACE);
    snprintf(buf + pos, len - (size_t)pos, "%s: %s", name, val);
}

static const char *gs_sb_name(void) { return s_gs.statusbar ? "\xe6\x98\xbe\xe7\xa4\xba" : "\xe9\x9a\x90\xe8\x97\x8f"; } /* 显示/隐藏 */
static const char *gs_dm_name(void) {
    switch (s_gs.display_mode) { case 0: return "\xe7\x82\xb9\xe5\xaf\xb9\xe7\x82\xb9"; /*点对点*/
        case 2: return "\xe6\x8b\x89\xe4\xbc\xb8"; /*拉伸*/ default: return "\xe5\x85\xa8\xe5\xb1\x8f"; /*全屏*/ }
}
static const char *gs_vk_name(void) {
    switch (s_gs.vkey) { case 1: return "\xe5\xbc\x80"; /*开*/ case 2: return "\xe8\x87\xaa\xe5\x8a\xa8"; /*自动*/
        default: return "\xe5\x85\xb3"; /*关*/ }
}
static const char *gs_aa_name(void) { return s_gs.pic_opt ? "EPX" : "\xe5\x85\xb3"; } /*关*/

/* 引擎优化右栏渲染 (3.3: 左对齐, 全角空格对齐冒号, 选中行反色) */
static void p_game_settings_render(ui_ctx_t *ctx, os_pane_t *p,
                                   int rx0, int rx1, int y) {
    (void)rx1;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    char buf[64];
    for (int i = 0; i < p->settings_count; i++) {
        switch (i) {
        case 0: gs_pad_label(buf, sizeof(buf), 5, "\xe7\x8a\xb6\xe6\x80\x81\xe6\xa0\x8f\xe8\xae\xbe\xe7\xbd\xae", gs_sb_name()); break; /*状态栏设置*/
        case 1: gs_pad_label(buf, sizeof(buf), 5, "\xe6\x98\xbe\xe7\xa4\xba\xe6\xa8\xa1\xe5\xbc\x8f", gs_dm_name()); break; /*显示模式*/
        case 2: gs_pad_label(buf, sizeof(buf), 5, "\xe8\x99\x9a\xe6\x8b\x9f\xe6\x8c\x89\xe9\x94\xae", gs_vk_name()); break; /*虚拟按键*/
        default: gs_pad_label(buf, sizeof(buf), 5, "\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf", gs_aa_name()); break; /*抗锯齿*/
        }
        int row_y = y + i * 32;
        bool sel = (p->focus == 1 && i == p->sel_item);
        if (sel) fill_rect(lcd, rx0, row_y, rx1, row_y + 30, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 8, row_y + 4, buf, sel);   /* 文字垂直居中 */
    }
}

/* 引擎优化右栏选中: 直接切换该设置 (连 API), 不弹窗 */
static void p_game_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    switch (item) {
    case 0: s_gs.statusbar ^= 1; break;                       /* 状态栏设置 显示/隐藏 */
    case 1: s_gs.display_mode = (uint8_t)((s_gs.display_mode + 1) % 3); break;  /* 显示模式 循环 */
    case 2: s_gs.vkey = (uint8_t)((s_gs.vkey + 1) % 3); break;                  /* 虚拟按键 循环 */
    default: s_gs.pic_opt ^= 1; break;                        /* 抗锯齿 关/EPX */
    }
    gs_save();
    ctx->needs_redraw = true;
    ESP_LOGI(TAG, "引擎优化: item=%d dm=%d aa=%d vk=%d sb=%d",
             item, s_gs.display_mode, s_gs.pic_opt, s_gs.vkey, s_gs.statusbar);
}

static EXT_RAM_BSS_ATTR os_pane_t s_gp;   /* 双栏模板状态 (PSRAM, 省 30KB 内部 RAM) */

static void p_select_game_enter(ui_ctx_t *ctx) {
    gs_load();   /* 读取引擎优化设置 */
    /* 管家后台默认加载 gam4980 引擎 (幂等; 返回主菜单由 os_mgr 及时清理) */
    engine_manager_load(ENGINE_GAM4980, ctx->lcd);
    if (!s_gp.loaded) {
        s_gp.root = GAM_ROOT;
        s_gp.is_item = is_gam;
        s_gp.on_select = on_game_select;
        s_gp.all_label = NULL;
        /* 引擎优化合并进左栏最上方 (3.3: 选中后右栏内联设置项, 不弹窗) */
        s_gp.settings_label = "\xe5\xbc\x95\xe6\x93\x8e\xe4\xbc\x98\xe5\x8c\x96"; /* 引擎优化 */
        s_gp.settings_count = 4;
        s_gp.settings_render = p_game_settings_render;
        s_gp.on_settings_select = p_game_settings_select;
        s_gp.list_y = 30;   /* 3.3: 状态栏(24)下方 */
        os_pane_reset(&s_gp);
    }
    os_pane_build_right(&s_gp);
}

static void p_select_game_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, &s_gp);
}

static void p_select_game_action(ui_ctx_t *ctx, os_action_t a) {
    if (os_pane_action(ctx, &s_gp, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static bool p_select_game_touch(ui_ctx_t *ctx, int x, int y) {
    return os_pane_touch(ctx, &s_gp, x, y);
}

static void p_select_game_poll(ui_ctx_t *ctx) {
    os_pane_poll(ctx, &s_gp);   /* 右栏触摸拖动滚动 */
}

static const os_module_t s_mod_select_game = {
    .name       = "select_game",
    .page_id    = OS_PAGE_SELECT_GAME,
    .on_enter   = p_select_game_enter,
    .render     = p_select_game_render,
    .action     = p_select_game_action,
    .touch      = p_select_game_touch,
    .poll       = p_select_game_poll,
    .fullscreen = false,   /* 3.3: 游戏菜单显示状态栏 */
};

void os_page_select_game_register(void) { os_register(&s_mod_select_game); }
