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
#include "sd_scan.h"
#include "os_pane.h"
#include "ui_common.h"
#include "gam4980_emu.h"
#include "engine_manager.h"
#include "input.h"
#include "favorites.h"
#include "esp_log.h"
#include "esp_attr.h"

#include "svc_nvs.h"
#include "svc_cfg.h"
#include "os_hw.h"        /* ★ 2026-10-30 按需蓝牙: os_hw_lease_acquire/release */
#include "bt_manager.h"   /* bt_manager_has_saved_pad (只读 NVS, 不初始化蓝牙) */
#include <string.h>
#include <stdio.h>
#include <dirent.h>

#define TAG "PGAME"

#define GAM_ROOT "/sdcard/roms/gam"

/* ==== 引擎优化设置 (3.3 游戏设置: 状态栏设置/显示模式/虚拟按键/抗锯齿) ====
 * 用 NVS 持久化 (namespace os_gset), 启动游戏时应用到 gam4980. */
typedef struct {
    uint8_t display_mode;   /* 0=点对点 1=全屏 2=拉伸 */
    uint8_t pic_opt;        /* 0=关 1=EPX 抗锯齿 */
    uint8_t vkey;           /* 0=关 1=开 2=自动 */
    uint8_t statusbar;      /* 0=隐藏 1=显示 */
    uint8_t key_sound;      /* 0=关 1=开  按键声音 (2026-09-29 新增) */
    uint8_t key_vib;        /* 0=关 1=开  触摸震动 (2026-09-29 新增) */
} gs_t;
static gs_t s_gs;

/* ★ 2026-10-30 按需蓝牙的租约 (幂等位图); 没配对过手柄时永远为空 → 不占内存 */
static os_hw_lease_t s_bt_lease;

static bool is_gam(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".gam") == 0);
}

/* ==== 步步高: 某些分类(如"益智休闲")里的游戏放在子文件夹内, 需把"含 .gam 的文件夹"也当作一项 ==== */
static bool gam_ends_gam(const char *s) {
    size_t len = strlen(s);
    return (len > 4) && (strcasecmp(s + len - 4, ".gam") == 0);
}
/* 在 dir 及其两层子目录内查找第一个 .gam, 找到写入 out 返回 true */
static bool gam_find_in(const char *dir, char *out, size_t osz, int depth) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (gam_ends_gam(e->d_name)) { snprintf(out, osz, "%s/%s", dir, e->d_name); found = true; break; }
    }
    if (!found && depth < 2) {
        rewinddir(d);
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (e->d_type == DT_DIR) {
                char sub[256];
                /* 目录+名放不下就跳过: 不用被截断的路径去找游戏 */
                if (strlen(dir) + 1 + strlen(e->d_name) >= sizeof(sub)) continue;
                snprintf(sub, sizeof(sub), "%.*s/%.*s",
                         (int)sizeof(sub) / 2 - 1, dir, (int)sizeof(sub) / 2 - 1, e->d_name);
                if (gam_find_in(sub, out, osz, depth + 1)) { found = true; break; }
            }
        }
    }
    closedir(d);
    return found;
}
/* 带路径过滤: 直接 .gam 文件, 或内含 .gam 的文件夹 (含子目录) */
static bool gam_is_game_dir(const char *path, const char *name) {
    if (gam_ends_gam(name)) return true;
    if (name[0] == '.') return false;
    char full[256], tmp[256];
    snprintf(full, sizeof(full), "%s/%s", path, name);
    return gam_find_in(full, tmp, sizeof(tmp), 0);
}

/* 右栏确认: 启动游戏 (对齐旧版 3.3 模板: init → load → run). */
static void on_game_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    /* 步步高: 若选中项是"含 .gam 的文件夹", 先解析出内部的 .gam 再运行 */
    char resolved[256];
    const char *run = path;
    if (!gam_ends_gam(path)) {
        if (gam_find_in(path, resolved, sizeof(resolved), 0)) run = resolved;
        else { ESP_LOGD(TAG, "未找到 .gam: %s", path); return; }
    }
    ESP_LOGI(TAG, "启动: %s", run);
    esp_err_t r = gam4980_emu_init(ctx->lcd);
    if (r != ESP_OK) {
        ESP_LOGD(TAG, "emu 初始化失败: %s", esp_err_to_name(r));
        return;
    }
    if (gam4980_emu_load(run) != 0) {
        ESP_LOGD(TAG, "游戏加载失败: %s", run);
        return;
    }
    /* 引擎优化应用到引擎 (显示模式/虚拟按键/游戏内状态栏/抗锯齿) */
    gam4980_set_fullscreen((int)s_gs.display_mode);
    gam4980_set_vkey((int)s_gs.vkey);
    gam4980_set_status_bar(s_gs.statusbar != 0);
    gam4980_set_pic_opt((int)s_gs.pic_opt);   /* 抗锯齿 (0=关 1=EPX) */
    /* 按键声音 / 触摸震动 (2026-09-29 新增引擎优化项; BBK 由引擎自己发声音与震动) */
    gam4980_set_key_sound(s_gs.key_sound != 0);
    gam4980_set_key_vib(s_gs.key_vib != 0);
    /* V1.2 模块化: 注入引擎回调 — 破 os ⇄ gam4980 循环依赖 (引擎不再 include os.h) */
    gam4980_set_exit_confirm_cb(os_confirm_exit_blocking);
    gam4980_set_statusbar_cb(svc_statusbar_draw_game);
    input_set_gamepad_nav_enabled(false);
    input_set_gamepad_gamemode(true);   /* V1.3.x: 步步高也是"游戏" — 暗黑UI模式(除游戏外反色)进入时恢复不反色 */
    /* ★ 2026-10-30 linit「蓝牙没启用就不要占内存」: 步步高是自驱动阻塞运行
     *   (gam4980_emu_run), 不走 eng_task_run_loop, 故在这里单独按需申请蓝牙。
     *   只读 NVS 预检: 从没配对过手柄 → 不申请, 全程 0 占用; 退出即 release 真拆栈。 */
    if (bt_manager_has_saved_pad()) os_hw_lease_acquire(&s_bt_lease, OS_HW_BT);
    gam4980_emu_run();
    os_hw_lease_release(&s_bt_lease, OS_HW_BT);   /* 退出游戏即还蓝牙内存 (~71KB) */
    input_set_gamepad_gamemode(false);  /* V1.3.x: 退出步步高, 结束游戏模式标志 (暗黑UI恢复反色) */
    input_set_gamepad_nav_enabled(true);
    input_mark_activity();   /* V1.5.x: 同 eng_task_run_loop — 刷新主循环输入时钟, 防退出即进壁纸 */
    ctx->needs_redraw = true;   /* 游戏退出后重绘列表 */
}

/* ==== 引擎优化: NVS 持久化 ==== */
#define GSET_NS "os_gset"

static void gs_load(void) {
    /* 默认值 (用户要求): 状态栏设置=显示, 显示模式=全屏, 虚拟按键=自动, 抗锯齿=关,
     * 按键声音=开, 触摸震动=开 (后两项与 page_engine 的引擎优化**共用同一 NVS 键**, 全局一致) */
    s_gs.display_mode = 1; s_gs.pic_opt = 0; s_gs.vkey = 2; s_gs.statusbar = 1;
    s_gs.key_sound = 1; s_gs.key_vib = 1;
    nvs_handle_t h;
    if (svc_nvs_open(GSET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (svc_nvs_get_u8(h, "dm", &v) == ESP_OK && v <= 2) s_gs.display_mode = v;
        if (svc_nvs_get_u8(h, "aa", &v) == ESP_OK && v <= 1) s_gs.pic_opt = v;
        if (svc_nvs_get_u8(h, "vk", &v) == ESP_OK && v <= 2) s_gs.vkey = v;
        if (svc_nvs_get_u8(h, "sb", &v) == ESP_OK && v <= 1) s_gs.statusbar = v;
        /* ⚠ 2026-09-29: BBK 用**独立的 NVS 键** (bks/bkv), 不与通用引擎 (esd/evb) 共用 ——
         * 共用会导致"在别的引擎里关一次触摸震动, BBK 也跟着永久关掉"(用户实测踩到). */
        if (svc_nvs_get_u8(h, "bks", &v) == ESP_OK && v <= 1) s_gs.key_sound = v;
        if (svc_nvs_get_u8(h, "bkv", &v) == ESP_OK && v <= 1) s_gs.key_vib = v;
        svc_nvs_close(h);
    }
    /* 只应用到 BBK 引擎自身 —— **不动** input 层的全局开关 (那是通用引擎的, 各引擎独立) */
    gam4980_set_key_sound(s_gs.key_sound != 0);
    gam4980_set_key_vib(s_gs.key_vib != 0);
}
static void gs_save(void) {
    svc_cfg_set_u8(GSET_NS, "dm", s_gs.display_mode);
    svc_cfg_set_u8(GSET_NS, "aa", s_gs.pic_opt);
    svc_cfg_set_u8(GSET_NS, "vk", s_gs.vkey);
    svc_cfg_set_u8(GSET_NS, "sb", s_gs.statusbar);
    svc_cfg_set_u8(GSET_NS, "bks", s_gs.key_sound);   /* BBK 专属键 (不与通用引擎共用) */
    svc_cfg_set_u8(GSET_NS, "bkv", s_gs.key_vib);
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
static const char *gs_ks_name(void) { return s_gs.key_sound ? "\xe5\xbc\x80" : "\xe5\x85\xb3"; } /* 按键声音 开/关 */
static const char *gs_kv_name(void) { return s_gs.key_vib   ? "\xe5\xbc\x80" : "\xe5\x85\xb3"; } /* 触摸震动 开/关 */

/* 引擎优化右栏渲染 (3.3: 左对齐, 全角空格对齐冒号, 选中行反色) */
static void p_game_settings_render(ui_ctx_t *ctx, os_pane_t *p,
                                   int rx0, int rx1,
                                   int list_y, int list_bottom, int top_row, int off_mod) {
    (void)rx1; (void)list_bottom;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    char buf[64];
    for (int i = 0; i < p->settings_count; i++) {
        int idx = top_row + i;
        if (idx >= p->settings_count) break;
        switch (idx) {
        case 0: gs_pad_label(buf, sizeof(buf), 5, "\xe7\x8a\xb6\xe6\x80\x81\xe6\xa0\x8f\xe8\xae\xbe\xe7\xbd\xae", gs_sb_name()); break; /*状态栏设置*/
        case 1: gs_pad_label(buf, sizeof(buf), 5, "\xe6\x98\xbe\xe7\xa4\xba\xe6\xa8\xa1\xe5\xbc\x8f", gs_dm_name()); break; /*显示模式*/
        case 2: gs_pad_label(buf, sizeof(buf), 5, "\xe8\x99\x9a\xe6\x8b\x9f\xe6\x8c\x89\xe9\x94\xae", gs_vk_name()); break; /*虚拟按键*/
        case 3: gs_pad_label(buf, sizeof(buf), 5, "\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf", gs_aa_name()); break; /*抗锯齿*/
        case 4: gs_pad_label(buf, sizeof(buf), 5, "\xe6\x8c\x89\xe9\x94\xae\xe5\xa3\xb0\xe9\x9f\xb3", gs_ks_name()); break; /*按键声音*/
        default: gs_pad_label(buf, sizeof(buf), 5, "\xe8\xa7\xa6\xe6\x91\xb8\xe9\x9c\x87\xe5\x8a\xa8", gs_kv_name()); break; /*触摸震动*/
        }
        int row_y = list_y + i * 32 - off_mod;
        bool sel = (p->focus == 1 && idx == p->sel_item);
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
    case 3: s_gs.pic_opt ^= 1; break;                          /* 抗锯齿 关/EPX */
    case 4:                                                    /* 按键声音 开/关 (BBK 专属) */
        s_gs.key_sound ^= 1;
        gam4980_set_key_sound(s_gs.key_sound != 0);
        break;
    default:                                                   /* 触摸震动 开/关 (BBK 专属) */
        s_gs.key_vib ^= 1;
        gam4980_set_key_vib(s_gs.key_vib != 0);
        break;
    }
    gs_save();
    ctx->needs_redraw = true;
    ESP_LOGI(TAG, "引擎优化: item=%d dm=%d aa=%d vk=%d sb=%d ks=%d kv=%d",
             item, s_gs.display_mode, s_gs.pic_opt, s_gs.vkey, s_gs.statusbar,
             s_gs.key_sound, s_gs.key_vib);
}

/* ★ LINTOS 2026-10-25: 列表 pane(约 59KB) 原来静态常驻 PSRAM, 改为进页面才分配/退出即还 */
static os_pane_t *s_gp__p;
static os_pane_t *s_gp__get(void) {
    if (!s_gp__p) {
        s_gp__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_gp__p) ESP_LOGW("PANE", "列表 pane 分配失败 (%u B)", (unsigned)sizeof(os_pane_t));
    }
    if (!s_gp__p) s_gp__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_8BIT);   /* 兜底: 内部 RAM */
    return s_gp__p;
}
static inline void s_gp__free(void) { if (s_gp__p) { heap_caps_free(s_gp__p); s_gp__p = NULL; } }
#define s_gp (*s_gp__get())
static void sg_exit(ui_ctx_t *ctx) { (void)ctx; s_gp__free(); }

static void p_select_game_enter(ui_ctx_t *ctx) {
    gs_load();   /* 读取引擎优化设置 */
    /* ★ 2026-10-24: 先等 TF 卡就绪再列游戏 —— 挂载是后台任务, 早一步扫就是空列表
     *   (linit: "换了张卡引擎里一个游戏都没有")。已挂载时立即返回。 */
    sd_wait_ready(1200, true);
    /* 管家后台默认加载 gam4980 引擎 (幂等; 返回主菜单由 os_mgr 及时清理) */
    engine_manager_load(ENGINE_GAM4980, ctx->lcd);
    if (!s_gp.loaded) {
        s_gp.root = GAM_ROOT;
        s_gp.is_item = is_gam;
        s_gp.on_select = on_game_select;
        s_gp.all_label = NULL;
        /* 步步高收藏栏 (与其它引擎一致: 长按收藏/取消收藏) */
        s_gp.fav_enabled = true;
        s_gp.fav_engine = FAV_ENGINE_BBK;
        /* 引擎优化合并进左栏最上方 (3.3: 选中后右栏内联设置项, 不弹窗) */
        s_gp.settings_label = "\xe5\xbc\x95\xe6\x93\x8e\xe4\xbc\x98\xe5\x8c\x96"; /* 引擎优化 */
        s_gp.settings_count = 6;   /* 状态栏设置/显示模式/虚拟按键/抗锯齿/按键声音/触摸震动 */
        s_gp.settings_render = p_game_settings_render;
        s_gp.on_settings_select = p_game_settings_select;
        s_gp.list_y = 30;   /* 3.3: 状态栏(24)下方 */
        os_pane_reset(&s_gp);
    }
    s_gp.is_item_d = gam_is_game_dir;   /* 把含 .gam 的子文件夹也当右栏一项 (益智休闲等) */
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
    .on_exit   = sg_exit,
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
