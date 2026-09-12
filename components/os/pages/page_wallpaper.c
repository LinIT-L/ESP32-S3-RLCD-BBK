/**
 * page_wallpaper.c — 壁纸 页面模块 (3.3 版壁纸设置弹窗树复刻).
 *
 * 弹窗树 (3.3 一致):
 *   壁纸设置: 壁纸类型 / 壁纸时间 / 测试壁纸 / 返回
 *   壁纸类型: 内置壁纸 / TF动态图 / 游戏壁纸 / 返回
 *     内置壁纸 → 选择壁纸程序: 星空(带*) / 返回
 *     TF动态图 → 播放速度: 慢/标准/快 / 返回
 *   壁纸时间: 1..30分钟 / 返回
 * 壁纸时间 = 动态壁纸(屏保)运行时长, 默认 10 分钟; 运行满时长后软关机(任意按键唤醒).
 * 当前 OS 仅支持星空程序, TF动态图/游戏壁纸/测试壁纸暂以"开发中"提示.
 * 私有 state 全部 static 留在本文件.
 */
#include "os.h"
#include "wallpapers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "PWP"
#define WP_NS "os_wp"

static int s_wp_program = 0;      /* 当前壁纸程序 (0=星空) */
static int s_wp_min     = 10;     /* 壁纸时间 (0=未设置; 默认 10 分钟) */

int os_wallpaper_get_program(void) { return s_wp_program; }

static void wp_open_main(ui_ctx_t *ctx);
static void wp_open_type(ui_ctx_t *ctx);
static void wp_open_prog(ui_ctx_t *ctx);
static void wp_open_speed(ui_ctx_t *ctx);
static void wp_open_min(ui_ctx_t *ctx);

/* ============ 壁纸设置 (5 项平铺列表, 3.3 版) ============ */
static bool wp_main_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;   /* 返回行走默认 */
    switch (d->sel) {
    case 0: wp_open_prog(ctx); break;        /* 内置壁纸 → 选择壁纸程序 */
    case 1: wp_open_speed(ctx); break;       /* TF图片 → 播放速度 */
    case 2: /* 游戏壁纸 */
        os_dialog_toast(ctx, "\xe6\xb8\xb8\xe6\x88\x8f\xe5\xa3\x81\xe7\xba\xb8: \xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* 游戏壁纸: 开发中 */
        break;
    case 3: wp_open_min(ctx); break;         /* 壁纸时间 */
    case 4: /* 测试壁纸: 立即全屏播放当前壁纸 (星空) */
        os_screensaver_force_enter();   /* 进入屏保全屏渲染当前壁纸, 任意输入唤醒 */
        os_dialog_pop(ctx);             /* 关闭壁纸设置弹窗, 唤醒后自动返回当前页面 */
        break;
    default: break;
    }
    return true;
}
static void wp_main_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)result; (void)ud;
    os_pop(ctx);   /* 返回 → 退出壁纸页 */
}
static void wp_open_main(ui_ctx_t *ctx) {
    static const char *const items[5] = {
        "\xe5\x86\x85\xe7\xbd\xae\xe5\xa3\x81\xe7\xba\xb8",   /* 内置壁纸 */
        "TF\xe5\x9b\xbe\xe7\x89\x87",                          /* TF图片 */
        "\xe6\xb8\xb8\xe6\x88\x8f\xe5\xa3\x81\xe7\xba\xb8",   /* 游戏壁纸 */
        "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4",   /* 壁纸时间 */
        "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xa3\x81\xe7\xba\xb8",   /* 测试壁纸 */
    };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 5;
    for (int i = 0; i < 5; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i]);
    dlg.sel = 0;
    dlg.on_key = wp_main_key;
    dlg.cb = wp_main_cb;
    os_dialog_push(ctx, &dlg);
}

/* ============ 选择壁纸程序 (内置壁纸 → 星空) ============ */
static bool wp_prog_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;
    if (d->sel == 0) {   /* 星空 */
        s_wp_program = WP_PROG_STARS;
        os_dialog_pop(ctx);
        wp_open_prog(ctx);   /* 重开带 * 标记 */
        os_dialog_toast(ctx, "\xe5\xa3\x81\xe7\xba\xb8\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 壁纸已设置 */
    }
    return true;
}
static void wp_open_prog(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s%s",
             (s_wp_program == WP_PROG_STARS) ? "* " : "",
             "\xe6\x98\x9f\xe7\xa9\xba");   /* 星空 */
    dlg.count = 1;
    dlg.sel = 0;
    dlg.on_key = wp_prog_key;
    os_dialog_push(ctx, &dlg);
}

/* ============ TF动态图 播放速度 ============ */
static void wp_open_speed(ui_ctx_t *ctx);
static bool wp_speed_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;
    os_dialog_toast(ctx, "TF\xe5\x8a\xa8\xe6\x80\x81\xe5\x9b\xbe: \xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* TF动态图: 开发中 */
    return true;
}
static void wp_open_speed(ui_ctx_t *ctx) {
    static const char *const items[3] = {
        "\xe6\x85\xa2 (5\xe5\xb8\xa7/\xe7\xa7\x92)",   /* 慢 (5帧/秒) */
        "\xe6\xa0\x87\xe5\x87\x86 (8\xe5\xb8\xa7/\xe7\xa7\x92)", /* 标准 (8帧/秒) */
        "\xe5\xbf\xab (12\xe5\xb8\xa7/\xe7\xa7\x92)",  /* 快 (12帧/秒) */
    };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 3;
    for (int i = 0; i < 3; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i]);
    dlg.sel = 0;
    dlg.on_key = wp_speed_key;
    os_dialog_push(ctx, &dlg);
}

/* ============ 壁纸时间 (屏保运行时长, 超时软关机) ============ */
static const int wp_mins[] = { 1, 2, 3, 5, 10, 15, 20, 30 };
#define WP_MINS_N (int)(sizeof(wp_mins) / sizeof(wp_mins[0]))
static void wp_open_min(ui_ctx_t *ctx);
static void wp_min_save(int v) {
    s_wp_min = v;
    nvs_handle_t h;
    if (nvs_open(WP_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "wp_run", v);
        nvs_commit(h);
        nvs_close(h);
    }
    wp_screensaver_set_run_ms((v <= 0) ? UINT32_MAX : ((uint32_t)v * 60000UL));
}
static bool wp_min_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;
    if (d->sel < WP_MINS_N) wp_min_save(wp_mins[d->sel]);
    os_dialog_pop(ctx);
    wp_open_min(ctx);
    os_dialog_toast(ctx, "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 壁纸时间已设置 */
    return true;
}
static void wp_open_min(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = WP_MINS_N;
    for (int i = 0; i < WP_MINS_N; i++) {
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s%d\xe5\x88\x86\xe9\x92\x9f",   /* X分钟 */
                 (s_wp_min == wp_mins[i]) ? "* " : "", wp_mins[i]);
    }
    dlg.sel = 0;
    dlg.on_key = wp_min_key;
    os_dialog_push(ctx, &dlg);
}

/* ============ 模块契约 ============ */
static void p_wp_enter(ui_ctx_t *ctx) {
    /* 读 NVS (壁纸时间; 程序固定星空) */
    nvs_handle_t h;
    if (nvs_open(WP_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "wp_run", &v) == ESP_OK) s_wp_min = (int)v;
        nvs_close(h);
    }
    s_wp_program = WP_PROG_STARS;
    wp_open_main(ctx);
}

static const os_module_t s_mod_wallpaper = {
    .name       = "wallpaper",
    .page_id    = OS_PAGE_WALLPAPER,
    .on_enter   = p_wp_enter,
    .fullscreen = false,
};

void os_page_wallpaper_register(void) { os_register(&s_mod_wallpaper); }
