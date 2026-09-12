/**
 * page_engine.c — 五个模拟器引擎各自的"游戏选择菜单页"整合.
 *
 * GB / GBC / NES / 文曲星 复用双栏模板 os_pane (同 page_select_game):
 *   左栏: 子文件夹 (分类), 右栏: 该目录下的游戏文件列表, 选中确认后运行 (阻塞).
 * ArduBoy: 内置游戏列表 (不扫文件), 简单全屏列表页.
 *
 * 统一生命周期: 进入页面 on_enter 调 engine_manager_load(对应引擎, ctx->lcd) 后台加载;
 * 回主菜单由 os_mgr 调 engine_manager_unload_all() 卸载. fullscreen=false 显示状态栏.
 * 私有 state 一律 static 留本文件.
 */
#include "os.h"
#include "os_pane.h"
#include "ui_common.h"
#include "engine_manager.h"
#include "input.h"
#include "esp_timer.h"
#include "wallpapers.h"   /* 屏保状态 + wp_program_render(星空): 游戏内锁屏暂停 */
#include "gb_emu.h"
#include "gbc_emu.h"
#include "nes_emu.h"
#include "md_emu.h"
#include "sms_emu.h"
#include "lavax_emu.h"
#include "vpet_emu.h"
#include "arduboy_avr.h"
#include "favorites.h"
#include "virtual_keys.h"
#include "board_rlcd.h"          /* board_shim_set_gb_gray 抗锯齿 */
#include "audio_player.h"        /* 统一音量档位与 volume 接口 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PHENG"

/* ==== 启动游戏 0-100 进度条 (参考 3.3 loading_screen) ==== */
/* GB/GBC/NES 引擎启动时经 set_progress_cb 回调, 全屏绘制加载画面:
 * 文件名 y=105 居中 / 进度条 300x16 @(50,140) / 百分比 y=165 居中. */
static int             s_load_pct = -1;
static char            s_load_text[64];
static st7305_handle_t *s_load_lcd = NULL;

static void eng_loading_draw(int percent, const char *text) {
    st7305_handle_t *lcd = s_load_lcd;
    if (!lcd) return;
    const int bar_x = 50, bar_y = 140, bar_w = 300, bar_h = 16;
    if (s_load_pct < 0) {
        st7305_clear(lcd, ST7305_COLOR_WHITE);
        for (int y = 0; y < bar_h; y++) {
            st7305_draw_pixel(lcd, bar_x, bar_y + y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, bar_x + bar_w - 1, bar_y + y, ST7305_COLOR_BLACK);
        }
        for (int x = 0; x < bar_w; x++) {
            st7305_draw_pixel(lcd, bar_x + x, bar_y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, bar_x + x, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
        }
        if (text && text[0]) draw_text_centered(lcd, 105, text, false);
        s_load_pct = 0;
    }
    /* 以 percent 为目标, 逐分平滑渐进到目标值 (引擎加载瞬时完成会直接给 100,
     * 平滑爬升让进度条更像真实加载, 全程约 0.5s, 不影响秒开). */
    while (s_load_pct < percent) {
        int step = (s_load_pct >= 85) ? 1 : (s_load_pct >= 60 ? 2 : 3);
        int target = s_load_pct + step;
        if (target > percent) target = percent;
        int old_fill = (bar_w - 4) * s_load_pct / 100;
        int new_fill = (bar_w - 4) * target / 100;
        for (int y = 2; y < bar_h - 2; y++)
            for (int x = old_fill; x < new_fill; x++)
                st7305_draw_pixel(lcd, bar_x + 2 + x, bar_y + y, ST7305_COLOR_BLACK);
        s_load_pct = target;
        for (int dy = 0; dy < 14; dy++)
            for (int dx = 0; dx < 40; dx++)
                st7305_draw_pixel(lcd, 180 + dx, 165 + dy, ST7305_COLOR_WHITE);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", target);
        draw_text_centered(lcd, 165, pct, false);
        st7305_flush(lcd);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}
static void eng_load_progress_cb(int percent) { eng_loading_draw(percent, s_load_text); }
/* 从完整路径提取文件名 (不含目录), 用于加载画面显示 */
static void eng_load_basename(const char *path) {
    const char *base = strrchr(path, '/');
    snprintf(s_load_text, sizeof(s_load_text), "%s", base ? base + 1 : path);
}
/* 准备加载画面: 保存屏句柄 + 待显示文件名; 返回后可挂 set_progress_cb */
static void eng_loading_start(ui_ctx_t *ctx, const char *path) {
    s_load_lcd = ctx->lcd;
    s_load_pct = -1;
    eng_load_basename(path);
}

/* ==== 各引擎文件过滤 / 根目录 ==== */
#define GB_ROOT    "/sdcard/gb"
#define GBC_ROOT   "/sdcard/gbc"
#define NES_ROOT   "/sdcard/nes"
#define MD_ROOT    "/sdcard/md"
#define SMS_ROOT   "/sdcard/sms"
#define LAVAX_ROOT "/sdcard/lava"
#define VPET_ROOT  "/sdcard/vpet"
#define AB_ROOT    "/sdcard/AB"

static bool is_ab(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".hex") == 0);
}

static bool is_gb(const char *name) {
    size_t len = strlen(name);
    return (len > 3) && (strcasecmp(name + len - 3, ".gb") == 0);
}
static bool is_gbc(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".gbc") == 0);
}
static bool is_nes(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".nes") == 0);
}
static bool is_md(const char *name) {
    size_t len = strlen(name);
    return (len > 3 && strcasecmp(name + len - 3, ".md") == 0) ||
           (len > 4 && strcasecmp(name + len - 4, ".gen") == 0) ||
           (len > 4 && strcasecmp(name + len - 4, ".bin") == 0);
}
static bool is_sms(const char *name) {
    size_t len = strlen(name);
    return (len > 4 && strcasecmp(name + len - 4, ".sms") == 0) ||
           (len > 3 && strcasecmp(name + len - 3, ".gg") == 0);
}
static bool is_lav(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".lav") == 0);
}
static bool is_bin(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".bin") == 0);
}

/* ==== 文曲星文件夹优化: 游戏是目录 (内含 Lava 子目录与 LavaData 数据), 非单文件 ==== */
static bool wqx_ends_lav(const char *s) {
    size_t len = strlen(s);
    return (len > 4) && (strcasecmp(s + len - 4, ".lav") == 0);
}
/* 把 "dir/name" 安全拼接到 out (避免 -Wformat-truncation 把警告当错误) */
static void wqx_join(char *out, size_t out_size, const char *dir, const char *name) {
    snprintf(out, out_size, "%s", dir);
    size_t l = strlen(out);
    size_t avail = out_size - l - 1;
    if (avail == 0) return;
    size_t nlen = strlen(name);
    if (nlen > avail) nlen = avail;
    out[l] = '/';
    memcpy(out + l + 1, name, nlen);
    out[l + 1 + nlen] = '\0';
}
/* 在游戏目录内解析第一个 .lav 文件: 优先 gdir/Lava 子目录, 其次 gdir 根.
 * 返回 0=成功并写入 out, 负值=未找到. */
static int wqx_resolve_lav(const char *gdir, char *out, size_t out_size) {
    if (!gdir || !out || out_size < 160) return -1;
    char sub[160];
    snprintf(sub, sizeof(sub), "%s/Lava", gdir);
    DIR *d = opendir(sub);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (wqx_ends_lav(e->d_name)) {
                wqx_join(out, out_size, sub, e->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    d = opendir(gdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (wqx_ends_lav(e->d_name)) {
                wqx_join(out, out_size, gdir, e->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}
/* 带路径过滤: 文曲星右栏把"含 .lav 的游戏文件夹"当作一项 (也接受直接 .lav 文件) */
static bool wqx_is_game_dir(const char *path, const char *name) {
    if (wqx_ends_lav(name)) return true;   /* 直接 .lav 文件 */
    if (name[0] == '.') return false;
    /* 顶层 "Lava" 是数据目录 (非游戏): 既不显示在左栏(见 exclude_dir), 也不当右栏游戏项 */
    if (strcasecmp(name, "Lava") == 0) return false;
    char full[160];
    snprintf(full, sizeof(full), "%s/%.63s", path, name);
    char lav[160];
    return (wqx_resolve_lav(full, lav, sizeof(lav)) == 0);
}

/* ==== 引擎优化 (通用: 显示模式/虚拟按键/抗锯齿/灰度/音量; NVS 持久化, 启动应用) ====
 * 抗锯齿 与 灰度 是两个独立项:
 *  - 灰度(gr): 总开关. 开=按抗锯齿选 4/5 档灰度; 关=纯黑白. 默认开.
 *  - 抗锯齿(aa): 灰度详情档位, 仅在灰度开启时生效. 开=5档更平滑, 关=4档. 默认开. */
#define EGSET_NS "os_gset"
static uint8_t s_edm = 1, s_evk = 2, s_aaf = 1, s_gray = 1;   /* 显示模式 / vkey / 抗锯齿 / 灰度 */
static uint8_t s_evo = 8;                       /* 音量档位 0-9 (默认 8=约89%) */
static uint8_t s_esnd = 1, s_evib = 1;          /* 按键声音 / 触摸震动 (引擎优化, 默认开) */
static void egs_load(void) {
    nvs_handle_t h;
    if (nvs_open(EGSET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "dm", &v) == ESP_OK && v <= 2) s_edm = v;
        if (nvs_get_u8(h, "vk", &v) == ESP_OK && v <= 2) s_evk = v;
        if (nvs_get_u8(h, "aa", &v) == ESP_OK && v <= 1) s_aaf = v;   /* 抗锯齿 */
        if (nvs_get_u8(h, "gr", &v) == ESP_OK && v <= 1) s_gray = v;  /* 灰度 */
        if (nvs_get_u8(h, "vo", &v) == ESP_OK && v < AUDIO_VOL_STEPS) s_evo = v;
        if (nvs_get_u8(h, "esd", &v) == ESP_OK && v <= 1) s_esnd = v; /* 按键声音 */
        if (nvs_get_u8(h, "evb", &v) == ESP_OK && v <= 1) s_evib = v; /* 触摸震动 */
        nvs_close(h);
    }
    /* 应用到全局输入层 (引擎游戏模式按键点击音 / 震动) */
    input_set_engine_sound(s_esnd != 0);
    input_set_engine_vib(s_evib != 0);
}
static void egs_save(void) {
    nvs_handle_t h;
    if (nvs_open(EGSET_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dm", s_edm); nvs_set_u8(h, "vk", s_evk);
        nvs_set_u8(h, "aa", s_aaf); nvs_set_u8(h, "gr", s_gray);
        nvs_set_u8(h, "vo", s_evo);
        nvs_set_u8(h, "esd", s_esnd); nvs_set_u8(h, "evb", s_evib);
        nvs_commit(h); nvs_close(h);
    }
}
static const char *edm_name(void){ return s_edm==0?"\xe7\x82\xb9\xe5\xaf\xb9\xe7\x82\xb9":(s_edm==2?"\xe6\x8b\x89\xe4\xbc\xb8":"\xe5\x85\xa8\xe5\xb1\x8f"); } /*点对点/拉伸/全屏*/
static const char *evk_name(void){ return s_evk==0?"\xe5\x85\xb3":(s_evk==1?"\xe5\xbc\x80":"\xe8\x87\xaa\xe5\x8a\xa8"); } /*关/开/自动*/
static const char *aaf_name(void){ return s_aaf?"\xe5\xbc\x80":"\xe5\x85\xb3"; }   /* 抗锯齿 开/关 */
static const char *gray_name(void){ return s_gray?"\xe5\xbc\x80":"\xe5\x85\xb3"; }  /* 灰度 开/关 */
static const char *esnd_name(void){ return s_esnd?"\xe5\xbc\x80":"\xe5\x85\xb3"; }  /* 按键声音 开/关 */
static const char *evib_name(void){ return s_evib?"\xe5\xbc\x80":"\xe5\x85\xb3"; }  /* 触摸震动 开/关 */
/* 与步步高一致的引擎优化排版: 全角空格补齐到 target 字宽, 使所有冒号竖直对齐 (左对齐) */
#define ENG_FULL_SPACE "\xe3\x80\x80"
static void eng_pad_label(char *buf, size_t len, int target, const char *name, const char *val) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    int pad = (n < target) ? target - n : 0;
    int pos = 0;
    for (int i = 0; i < pad && pos < (int)len; i++)
        pos += snprintf(buf + pos, len - (size_t)pos, "%s", ENG_FULL_SPACE);
    snprintf(buf + pos, len - (size_t)pos, "%s: %s", name, val);
}
static void eng_settings_render(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1,
                                int list_y, int list_bottom, int top_row, int off_mod) {
    (void)rx1; (void)list_bottom; st7305_handle_t *lcd = ctx->lcd; if (!lcd) return;
    char buf[48];
    for (int i = 0; i < p->settings_count; i++) {
        int idx = top_row + i;
        if (idx >= p->settings_count) break;
        switch (idx) {
        case 0: eng_pad_label(buf, sizeof(buf), 5, "\xe6\x98\xbe\xe7\xa4\xba\xe6\xa8\xa1\xe5\xbc\x8f", edm_name()); break; /*显示模式*/
        case 1: eng_pad_label(buf, sizeof(buf), 5, "\xe8\x99\x9a\xe6\x8b\x9f\xe6\x8c\x89\xe9\x94\xae", evk_name()); break; /*虚拟按键*/
        case 2: eng_pad_label(buf, sizeof(buf), 5, "\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf", aaf_name()); break; /*抗锯齿*/
        case 3: eng_pad_label(buf, sizeof(buf), 5, "\xe7\x81\xb0\xe5\xba\xa6", gray_name()); break; /*灰度*/
        case 4: { char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%d", s_evo);
                   eng_pad_label(buf, sizeof(buf), 5, "\xe9\x9f\xb3\xe9\x87\x8f", vbuf); break; } /*音量*/
        case 5: eng_pad_label(buf, sizeof(buf), 5, "\xe6\x8c\x89\xe9\x94\xae\xe5\xa3\xb0\xe9\x9f\xb3", esnd_name()); break; /*按键声音*/
        case 6: eng_pad_label(buf, sizeof(buf), 5, "\xe8\xa7\xa6\xe6\x91\xb8\xe9\x9c\x87\xe5\x8a\xa8", evib_name()); break; /*触摸震动*/
        }
        int ry = list_y + i * 32 - off_mod; bool sel = (p->focus == 1 && idx == p->sel_item);
        if (sel) fill_rect(lcd, rx0, ry, rx1, ry + 30, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 8, ry + 4, buf, sel);
    }
}
static void eng_settings_select(ui_ctx_t *ctx, os_pane_t *p, int item) {
    (void)p;
    if (item == 0) s_edm = (uint8_t)((s_edm + 1) % 3);
    else if (item == 1) s_evk = (uint8_t)((s_evk + 1) % 3);
    else if (item == 2) s_aaf ^= 1;       /* 抗锯齿 */
    else if (item == 3) s_gray ^= 1;      /* 灰度 */
    else if (item == 4) {   /* 音量档位 0-9 循环, 联调统一音量 */
        s_evo = (uint8_t)((s_evo + 1) % AUDIO_VOL_STEPS);
        audio_player_set_volume(audio_player_vol_to_percent((int)s_evo));
    }
    else if (item == 5) { s_esnd ^= 1;  input_set_engine_sound(s_esnd != 0); }   /* 按键声音 */
    else if (item == 6) { s_evib ^= 1;  input_set_engine_vib(s_evib != 0); }     /* 触摸震动 */
    egs_save(); ctx->needs_redraw = true;
}

/* 文曲星: 根目录下的 "Lava" 是数据存放目录 (不是游戏分类), 左栏不显示为类别.
 * (注意: 游戏目录/自身内部也有 Lava 子目录, 由 wqx_resolve_lav 处理, 与这里无关.) */
static bool wqx_exclude_lava(const char *name) {
    return (name && strcasecmp(name, "Lava") == 0);
}

/* ==== 双栏页面通用辅助 (设置并重建一个 os_pane) ==== */
static void pane_setup(os_pane_t *p, ui_ctx_t *ctx, const char *root,
                       bool (*is_item)(const char *),
                       os_pane_select_cb on_select) {
    (void)ctx;
    if (!p->loaded) {
        egs_load();
        p->root = root;
        p->is_item = is_item;
        p->on_select = on_select;
        p->all_label = NULL;
        /* 左栏最上方"引擎优化", 右栏内联设置 (3.3) */
        p->settings_label = "\xe5\xbc\x95\xe6\x93\x8e\xe4\xbc\x98\xe5\x8c\x96"; /* 引擎优化 */
        p->settings_count = 7;   /* 显示模式 / 虚拟按键 / 抗锯齿 / 灰度 / 音量 / 按键声音 / 触摸震动 */
        p->settings_render = eng_settings_render;
        p->on_settings_select = eng_settings_select;
        p->list_y = 30;   /* 状态栏(24)下方 */
        os_pane_reset(p);
    }
    os_pane_build_right(p);
}

static void pane_render(ui_ctx_t *ctx, os_pane_t *p) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, p);
}

static void pane_action(ui_ctx_t *ctx, os_pane_t *p, os_action_t a) {
    if (os_pane_action(ctx, p, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static bool pane_touch(ui_ctx_t *ctx, os_pane_t *p, int x, int y) {
    return os_pane_touch(ctx, p, x, y);
}

static void pane_poll(ui_ctx_t *ctx, os_pane_t *p) {
    os_pane_poll(ctx, p);
}

/* ==== 引擎运行循环 (task 型引擎 GB/GBC/NES): 每帧喂 joypad + 检测返回 → 确认退出 ==== */
typedef struct {
    void (*set_joypad)(uint8_t);
    void (*pause)(void);
    void (*resume)(void);
} eng_loop_ops_t;
extern uint8_t input_get_held_gb_joypad(void);

static bool eng_exit_confirm(ui_ctx_t *ctx, eng_loop_ops_t *ops) {
    if (ops->pause) ops->pause();
    /* 丢弃触发退出的那一次残留触摸 tap, 防止弹窗一弹出就被当作"确定"自动退出.
     * 统一确认框 (os_confirm_exit_blocking) 直绘在冻结的游戏帧上, 手柄/物理/触摸
     * 交互内置, 与步步高/文曲星完全一致; 确认=true 才强行退出, 取消则恢复游戏. */
    input_consume_tap(NULL, NULL);
    bool confirm = os_confirm_exit_blocking(ctx->lcd);
    if (ops->resume) ops->resume();
    return confirm;
}
/* 引擎前台运行: 阻塞循环喂 joypad, BACK/退出键 → 确认退出; 返回后需 stop().
 * 游戏运行阻塞主循环, 但"进入屏保/渲染/软关机"统一由独立监视任务
 * (wp_screensaver_supervisor_start) 负责, 不受本阻塞循环影响: 空闲超时
 * 监视任务强制进入壁纸并冻结画面(board_rlcd 独占), 壁纸恒在最上层. */
static void eng_task_run_loop(ui_ctx_t *ctx, eng_loop_ops_t *ops) {
    input_mark_activity();   /* V1.5.x: 进入游戏即刷新主循环输入时钟 → 任何操作重置壁纸计时 */
    input_set_gamepad_nav_enabled(false);
    input_set_gamepad_gamemode(true);   /* V1.2.x: 标记真正进入游戏, 允许手柄返回/退出键退出 */
    /* 屏幕虚拟按键: 引擎优化"虚拟按键"设置 (0=关 1/2=开) */
    virtual_keys_set_enabled(s_evk != 0);
    /* 应用引擎优化的统一音量档位 (0-9) 到全局音频 */
    audio_player_set_volume(audio_player_vol_to_percent((int)s_evo));
    /* 应用引擎优化: 灰度总开关 + 抗锯齿档位.
     * 灰度开 → 按抗锯齿选 4档(aa=0)/5档(aa=1)点聚灰度; 灰度关 → 纯黑白.
     * GB/GBC/NES 共用 board_shim 灰度. */
    board_shim_set_gb_gray(s_gray ? (s_aaf ? 2 : 1) : 0);
    /* GB/GBC/NES/VPET 必须显示 SEL/STA 两个特殊键; 只有步步高(gam4980)内部隐藏.
     * 这里显式复位, 防止步步高上次运行遗留 hide_selsta=true 影响本引擎. */
    virtual_keys_set_hide_selsta(false);
    bool exit_req = false;
    bool paused_for_ss = false;   /* 屏保激活时暂停引擎, 消除与壁纸双写闪烁 */
    while (1) {
        /* 输入采样即刷新输入层统一时钟(input_last_any_ms):
         *   - joypad: input_get_held_gb_joypad 内部 j!=FF → input_mark_activity
         *   - 物理键/手柄动作: input_get_action 有动作 → 刷新
         *   - 触摸: input_get_touch_pos 有按下 → 刷新
         * 故"正在操作"会自动把时钟拉到 now, 空闲(无输入)时时钟停摆 →
         * 独立监视任务据此强制进壁纸. 本循环不再自己判定空闲, 也不渲染屏保. */
        uint8_t j = input_get_held_gb_joypad();
        ops->set_joypad(j);
        os_action_t a = (os_action_t)input_get_action();
        { int tx, ty; if (input_get_touch_pos(&tx, &ty)) {} }

        /* 屏保被独立监视任务激活 → 暂停引擎(彻底停止往屏画帧), 消除与壁纸互闪;
         * 唤醒后恢复引擎继续运行. */
        bool ss = os_screensaver_active();
        if (ss && !paused_for_ss) { if (ops->pause) ops->pause(); paused_for_ss = true; }
        else if (!ss && paused_for_ss) { if (ops->resume) ops->resume(); paused_for_ss = false; }

        /* 屏保接管期间不响应退出/操作, 唤醒由监视任务负责 */
        if (ss) {
            a = OS_ACTION_NONE;
        } else if (a == OS_ACTION_POWER_LOCK) {
            /* 锁屏键点按 → 交给独立监视任务渲染, 本循环不自己画壁纸 */
            os_screensaver_force_enter();
            a = OS_ACTION_NONE;
        }
        if (a == OS_ACTION_HOME || a == OS_ACTION_BACK) exit_req = true;
        if (exit_req) {
            if (eng_exit_confirm(ctx, ops)) break;
            exit_req = false;
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    virtual_keys_set_enabled(false);
    input_set_gamepad_nav_enabled(true);
    input_set_gamepad_gamemode(false);  /* V1.2.x: 退出关卡, 结束游戏模式标志 */
    /* V1.5.x: 退出游戏刷新主循环输入时钟 — 引擎阻塞运行期间 main.c 的 last_input_ms
     * 停留在进游戏前旧值, 不回写则主循环恢复后 wp_screensaver_poll 立即判定空闲超时,
     * "退出游戏直接进壁纸" (且壁纸重新计时, 10 分钟软关机看似失效). 置活动标志后
     * 下一帧 input_consume_activity() 刷新 last_input_ms = now, 正常回到桌面. */
    input_mark_activity();
    st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
    ctx->needs_redraw = true;
}

/* ==== 右栏确认: 各引擎运行 (阻塞) ==== */
static void gb_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动GB: %s", path);
    gb_emu_set_save_dir("/sdcard/dict/GB");
    eng_loading_start(ctx, path);               /* 0-100 加载进度 */
    gb_emu_set_progress_cb(eng_load_progress_cb);
    gb_emu_rom_t rom;
    if (gb_emu_load_rom(path, &rom) != ESP_OK) {
        gb_emu_set_progress_cb(NULL);
        ESP_LOGE(TAG, "GB ROM 加载失败: %s", path);
        return;
    }
    eng_loading_draw(100, s_load_text);         /* 读取完成 → 100% */
    gb_emu_set_progress_cb(NULL);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = gb_emu_start(&rom);   /* 启动模拟任务 (非阻塞) */
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "GB 启动失败: %s", esp_err_to_name(st));
    } else {
        gb_emu_set_fullscreen((int)s_edm);   /* 显示模式 */
        eng_loop_ops_t ops = { gb_emu_set_joypad, gb_emu_pause, gb_emu_resume };
        eng_task_run_loop(ctx, &ops);     /* 前台运行, 直到确认退出 */
        gb_emu_stop();
        gb_emu_unload();
    }
    gb_emu_free_rom(&rom);
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

static void gbc_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动GBC: %s", path);
    gbc_emu_set_save_dir("/sdcard/dict/GBC");
    eng_loading_start(ctx, path);               /* 0-100 加载进度 */
    gbc_emu_set_progress_cb(eng_load_progress_cb);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = gbc_emu_start(path);   /* 启动模拟任务 (非阻塞, 加载期回调进度) */
    gbc_emu_set_progress_cb(NULL);              /* 加载完成, 移除回调 */
    eng_loading_draw(100, s_load_text);         /* 补齐到 100% 再进游戏, 避免 70% 就开局 */
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "GBC 启动失败: %s", esp_err_to_name(st));
    } else {
        gbc_emu_set_fullscreen((int)s_edm);   /* 显示模式 */
        eng_loop_ops_t ops = { gbc_emu_set_joypad, gbc_emu_pause, gbc_emu_resume };
        eng_task_run_loop(ctx, &ops);
        gbc_emu_stop();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

static void nes_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动NES: %s", path);
    eng_loading_start(ctx, path);               /* 0-100 加载进度 */
    nes_emu_set_progress_cb(eng_load_progress_cb);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = nes_emu_start(path);   /* 启动模拟任务 (非阻塞, 加载期回调进度) */
    nes_emu_set_progress_cb(NULL);              /* 加载完成, 移除回调 */
    eng_loading_draw(100, s_load_text);         /* 补齐到 100% 再进游戏 */
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "NES 启动失败: %s", esp_err_to_name(st));
    } else {
        nes_emu_set_fullscreen((int)s_edm);   /* 显示模式 */
        eng_loop_ops_t ops = { nes_emu_set_joypad, nes_emu_pause, nes_emu_resume };
        eng_task_run_loop(ctx, &ops);
        nes_emu_stop();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}


/* ===== 文曲星(LAVAX) 宿主接口: 该引擎自驱动运行(阻塞在 lavax_emu_load_and_run),
 * os 主循环不介入其循环, 故退出检测由宿主回调在 lavax poll 上下文中触发.
 * 确认浮层视觉沿用 os_dialog confirm 规范(见 svc_statusbar / os_dialog). */
static bool s_wqx_exit_asked = false;
bool menu_wqx_confirm_asked(void) { return s_wqx_exit_asked; }  /* 保留历史符号: lavax_platform_poll 查询浮层激活 */

/* 文曲星运行画面由自身绘制, 不能走 os 弹窗管道. 这里缓存当前 lcd, 在宿主退出检测回调里
 * 直接冻结帧直绘统一退出确认框 (os_confirm_exit_blocking). 确认=true 才真正请求退出. */
static st7305_handle_t *s_wqx_lcd = NULL;

static bool lavax_host_exit_check(void) {
    bool want = ((os_action_t)input_get_action() == OS_ACTION_BACK) ||
                ((os_action_t)input_get_action() == OS_ACTION_HOME);
    if (!want) return false;
    /* 已弹出确认框期间 (等待用户决定): 只认确认框自身交互, 不再每次触发 */
    if (s_wqx_exit_asked) return false;
    s_wqx_exit_asked = true;
    bool confirm = os_confirm_exit_blocking(s_wqx_lcd);
    s_wqx_exit_asked = false;
    return confirm;
}

static void lavax_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动文曲星: %s", path);
    s_wqx_lcd = ctx->lcd;   /* 缓存屏句柄, 供退出确认直绘用 */
    input_set_gamepad_nav_enabled(false);
    input_set_gamepad_gamemode(true);   /* V1.2.x: 文曲星自驱动阻塞运行, 标记游戏模式 */
    /* path 可能是游戏文件夹 (内含 Lava 子目录), 解析出 .lav 文件再运行 */
    char lav[160];
    const char *run = path;
    if (!wqx_ends_lav(path)) {
        if (wqx_resolve_lav(path, lav, sizeof(lav)) != 0) {
            ESP_LOGE(TAG, "文曲星: %s 内未找到 .lav", path);
            input_set_gamepad_nav_enabled(true);
            input_set_gamepad_gamemode(false);
            os_dialog_toast(ctx, "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\xe6\xb8\xb8\xe6\x88\x8f"); /* 未找到游戏 */
            return;
        }
        run = lav;
        ESP_LOGI(TAG, "文曲星已定位 .lav: %s", run);
    }
    lavax_set_display_mode((int)s_edm);      /* 显示模式: 0点对点/1全屏/2拉伸 */
    lavax_set_exit_check(lavax_host_exit_check);   /* 返回键 -> 请求退出(确认浮层后续按 os_dialog 标准接入) */
    lavax_emu_load_and_run(run);   /* 阻塞直到退出 */
    input_set_gamepad_nav_enabled(true);
    input_set_gamepad_gamemode(false);  /* V1.2.x: 文曲星退出, 结束游戏模式标志 */
    input_mark_activity();   /* V1.5.x: 同 eng_task_run_loop — 刷新主循环输入时钟, 防退出即进壁纸 */
    ctx->needs_redraw = true;
}

static void vpet_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动暴龙机: %s", path);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = vpet_emu_start(path);   /* 启动模拟任务 (非阻塞) */
    if (st != ESP_OK) ESP_LOGE(TAG, "VPET 启动失败: %s", esp_err_to_name(st));
    else {
        vpet_emu_set_display((int)s_edm, s_gray != 0);   /* 显示模式 + 灰度 */
        eng_loop_ops_t ops = { vpet_emu_set_joypad, vpet_emu_pause, vpet_emu_resume };
        eng_task_run_loop(ctx, &ops);
        vpet_emu_stop();
        vpet_emu_unload();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

/* ==== 各页面静态双栏 state (PSRAM, 省内部 RAM) ==== */
static EXT_RAM_BSS_ATTR os_pane_t s_gb_p;
static EXT_RAM_BSS_ATTR os_pane_t s_gbc_p;
static EXT_RAM_BSS_ATTR os_pane_t s_nes_p;
static EXT_RAM_BSS_ATTR os_pane_t s_md_p;
static EXT_RAM_BSS_ATTR os_pane_t s_sms_p;
static EXT_RAM_BSS_ATTR os_pane_t s_lav_p;
static EXT_RAM_BSS_ATTR os_pane_t s_vpet_p;

/* ==== GB 页面 ==== */
static void gb_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_GB, ctx->lcd);
    s_gb_p.fav_enabled = true; s_gb_p.fav_engine = FAV_ENGINE_GB;
    pane_setup(&s_gb_p, ctx, GB_ROOT, is_gb, gb_select);
}
static void gb_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_gb_p); }
static void gb_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_gb_p, a); }
static bool gb_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_gb_p, x, y); }
static void gb_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_gb_p); }

static const os_module_t s_mod_gb = {
    .name       = "engine_gb",
    .page_id    = OS_PAGE_GB_GAME,
    .on_enter   = gb_enter,
    .render     = gb_render,
    .action     = gb_action,
    .touch      = gb_touch,
    .poll       = gb_poll,
    .fullscreen = false,
};

/* ==== GBC 页面 (复用 ENGINE_GB, 存档目录独立) ==== */
static void gbc_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_GB, ctx->lcd);
    s_gbc_p.fav_enabled = true; s_gbc_p.fav_engine = FAV_ENGINE_GBC;
    pane_setup(&s_gbc_p, ctx, GBC_ROOT, is_gbc, gbc_select);
}
static void gbc_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_gbc_p); }
static void gbc_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_gbc_p, a); }
static bool gbc_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_gbc_p, x, y); }
static void gbc_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_gbc_p); }

static const os_module_t s_mod_gbc = {
    .name       = "engine_gbc",
    .page_id    = OS_PAGE_GBC_GAME,
    .on_enter   = gbc_enter,
    .render     = gbc_render,
    .action     = gbc_action,
    .touch      = gbc_touch,
    .poll       = gbc_poll,
    .fullscreen = false,
};

/* ==== NES 页面 ==== */
static void nes_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_NES, ctx->lcd);
    s_nes_p.fav_enabled = true; s_nes_p.fav_engine = FAV_ENGINE_FC;   /* NES 合并到 FC */
    pane_setup(&s_nes_p, ctx, NES_ROOT, is_nes, nes_select);
}
static void nes_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_nes_p); }
static void nes_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_nes_p, a); }
static bool nes_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_nes_p, x, y); }
static void nes_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_nes_p); }

static const os_module_t s_mod_nes = {
    .name       = "engine_nes",
    .page_id    = OS_PAGE_NES_GAME,
    .on_enter   = nes_enter,
    .render     = nes_render,
    .action     = nes_action,
    .touch      = nes_touch,
    .poll       = nes_poll,
    .fullscreen = false,
};

/* ==== MD/Genesis 页面 ==== */
static void md_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动MD: %s", path);
    eng_loading_start(ctx, path);
    md_emu_set_progress_cb(eng_load_progress_cb);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = md_emu_start(path);
    md_emu_set_progress_cb(NULL);
    eng_loading_draw(100, s_load_text);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "MD 启动失败: %s", esp_err_to_name(st));
    } else {
        md_emu_set_fullscreen((int)s_edm);
        eng_loop_ops_t ops = { md_emu_set_joypad, md_emu_pause, md_emu_resume };
        eng_task_run_loop(ctx, &ops);
        md_emu_stop();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

static void md_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_MD, ctx->lcd);
    s_md_p.fav_enabled = true; s_md_p.fav_engine = FAV_ENGINE_MD;
    pane_setup(&s_md_p, ctx, MD_ROOT, is_md, md_select);
}
static void md_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_md_p); }
static void md_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_md_p, a); }
static bool md_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_md_p, x, y); }
static void md_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_md_p); }

static const os_module_t s_mod_md = {
    .name       = "engine_md",
    .page_id    = OS_PAGE_MD_GAME,
    .on_enter   = md_enter,
    .render     = md_render,
    .action     = md_action,
    .touch      = md_touch,
    .poll       = md_poll,
    .fullscreen = false,
};

/* ==== SMS/GG 页面 ==== */
static void sms_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动SMS: %s", path);
    eng_loading_start(ctx, path);
    sms_emu_set_progress_cb(eng_load_progress_cb);
    input_set_gamepad_nav_enabled(false);
    esp_err_t st = sms_emu_start(path);
    sms_emu_set_progress_cb(NULL);
    eng_loading_draw(100, s_load_text);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "SMS 启动失败: %s", esp_err_to_name(st));
    } else {
        sms_emu_set_fullscreen((int)s_edm);
        eng_loop_ops_t ops = { sms_emu_set_joypad, sms_emu_pause, sms_emu_resume };
        eng_task_run_loop(ctx, &ops);
        sms_emu_stop();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

static void sms_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_SMS, ctx->lcd);
    s_sms_p.fav_enabled = true; s_sms_p.fav_engine = FAV_ENGINE_SMS;
    pane_setup(&s_sms_p, ctx, SMS_ROOT, is_sms, sms_select);
}
static void sms_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_sms_p); }
static void sms_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_sms_p, a); }
static bool sms_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_sms_p, x, y); }
static void sms_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_sms_p); }

static const os_module_t s_mod_sms = {
    .name       = "engine_sms",
    .page_id    = OS_PAGE_SMS_GAME,
    .on_enter   = sms_enter,
    .render     = sms_render,
    .action     = sms_action,
    .touch      = sms_touch,
    .poll       = sms_poll,
    .fullscreen = false,
};

/* ==== 文曲星 (LavaX) 页面 ==== */
static void lavax_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_LAVAX, ctx->lcd);
    s_lav_p.fav_enabled = true; s_lav_p.fav_engine = FAV_ENGINE_WQX;
    s_lav_p.exclude_dir = wqx_exclude_lava;   /* 左栏不显示 Lava 数据目录 (首次 reset 前设置) */
    pane_setup(&s_lav_p, ctx, LAVAX_ROOT, is_lav, lavax_select);
    /* 文曲星: 游戏是目录 (内含 Lava 子目录与 LavaData 数据), 用带路径过滤把"游戏文件夹"当右栏一项 */
    s_lav_p.is_item_d = wqx_is_game_dir;
}
static void lavax_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_lav_p); }
static void lavax_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_lav_p, a); }
static bool lavax_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_lav_p, x, y); }
static void lavax_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_lav_p); }

static const os_module_t s_mod_lavax = {
    .name       = "engine_lavax",
    .page_id    = OS_PAGE_WQX,
    .on_enter   = lavax_enter,
    .render     = lavax_render,
    .action     = lavax_action,
    .touch      = lavax_touch,
    .poll       = lavax_poll,
    .fullscreen = false,
};

/* ==== 暴龙机 (VPET) 页面 ==== */
static void vpet_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_VPET, ctx->lcd);
    s_vpet_p.fav_enabled = true; s_vpet_p.fav_engine = FAV_ENGINE_VPET;
    pane_setup(&s_vpet_p, ctx, VPET_ROOT, is_bin, vpet_select);
}
static void vpet_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_vpet_p); }
static void vpet_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_vpet_p, a); }
static bool vpet_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_vpet_p, x, y); }
static void vpet_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_vpet_p); }

static const os_module_t s_mod_vpet = {
    .name       = "engine_vpet",
    .page_id    = OS_PAGE_VPET,
    .on_enter   = vpet_enter,
    .render     = vpet_render,
    .action     = vpet_action,
    .touch      = vpet_touch,
    .poll       = vpet_poll,
    .fullscreen = false,
};

/* ==== ArduBoy 页面 (os_pane 双栏: 扫描 /sdcard/AB 下的 .hex, 经 simavr ATmega32u4 运行) ==== */
static EXT_RAM_BSS_ATTR os_pane_t s_ab_p;

static void ab_select(ui_ctx_t *ctx, os_pane_t *p, const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动ArduBoy: %s", path);
    input_set_gamepad_nav_enabled(false);
    /* ArduBoy 虚拟按键: 十字方向 + A/B (STANDARD) */
    virtual_keys_set_layout(VK_LAYOUT_STANDARD);
    esp_err_t st = arduboy_avr_start(path);   /* 启动模拟任务 (非阻塞) */
    if (st != ESP_OK) ESP_LOGE(TAG, "ArduBoy 启动失败: %s", esp_err_to_name(st));
    else {
        arduboy_avr_set_fullscreen((int)s_edm);   /* 显示模式 (引擎优化) */
        eng_loop_ops_t ops = { arduboy_avr_set_joypad, arduboy_avr_pause, arduboy_avr_resume };
        eng_task_run_loop(ctx, &ops);
        arduboy_avr_stop();
    }
    input_set_gamepad_nav_enabled(true);
    ctx->needs_redraw = true;
}

static void ab_enter(ui_ctx_t *ctx) {
    engine_manager_load(ENGINE_ARDUBOY, ctx->lcd);
    s_ab_p.fav_enabled = true; s_ab_p.fav_engine = FAV_ENGINE_AB;
    pane_setup(&s_ab_p, ctx, AB_ROOT, is_ab, ab_select);
}
static void ab_render(ui_ctx_t *ctx)   { pane_render(ctx, &s_ab_p); }
static void ab_action(ui_ctx_t *ctx, os_action_t a) { pane_action(ctx, &s_ab_p, a); }
static bool ab_touch(ui_ctx_t *ctx, int x, int y)   { return pane_touch(ctx, &s_ab_p, x, y); }
static void ab_poll(ui_ctx_t *ctx)     { pane_poll(ctx, &s_ab_p); }

static const os_module_t s_mod_arduboy = {
    .name       = "engine_arduboy",
    .page_id    = OS_PAGE_ARDUBOY_GAME,
    .on_enter   = ab_enter,
    .render     = ab_render,
    .action     = ab_action,
    .touch      = ab_touch,
    .poll       = ab_poll,
    .fullscreen = false,
};

/* ==== 注册全部引擎页面 ==== */
void os_page_engine_register_all(void) {
    os_register(&s_mod_gb);
    os_register(&s_mod_gbc);
    os_register(&s_mod_nes);
    os_register(&s_mod_md);
    os_register(&s_mod_sms);
    os_register(&s_mod_lavax);
    os_register(&s_mod_vpet);
    os_register(&s_mod_arduboy);
}