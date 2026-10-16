/**
 * page_wallpaper.c — 壁纸 页面模块 (壁纸重构版).
 *
 * 弹窗树:
 *   壁纸设置: 动态壁纸 / 壁纸时间 / 测试壁纸
 *   动态壁纸 → 列表: 星空(内置·锁定) + 以后商店下载的动画壁纸
 *   壁纸时间: 1..30分钟 / 返回
 *   测试壁纸: 立即全屏播放当前动态壁纸
 * 交互:
 *   - 点选列表项 = 切换/设为当前壁纸 (内置项带"*"标记选中, 确认后 toast).
 *   - 长按列表项 2 秒 = 弹"是否删除?"确认弹窗(复用 os_dialog_confirm_ex).
 *     · 内置项锁定: 长按仅提示"内置壁纸不可删除", 绝无删除弹窗.
 *     · 下载项可删: 确认即删除(当前未开放下载, 分支已就绪).
 * 内置项不改已存 NVS 键/默认值/控制流 (os_wp: min/wp_run); 仅组织 UI 列表.
 * 私有 state 全部 static 留在本文件.
 */
#include "os.h"
#include "wallpapers.h"

#include "svc_nvs.h"
#include "svc_cfg.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "PWP"
#define WP_NS "os_wp"

#define WP_LABEL_DYNAMIC  "\xe5\x8a\xa8\xe6\x80\x81\xe5\xa3\x81\xe7\xba\xb8"   /* 动态壁纸 */
#define WP_LABEL_STARS    "\xe6\x98\x9f\xe7\xa9\xba"                            /* 星空 */
#define WP_LABEL_WTIME    "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4"   /* 壁纸时间 */
#define WP_LABEL_TEST     "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xa3\x81\xe7\xba\xb8"   /* 测试壁纸 */
#define WP_TXT_SET        "\xe5\xa3\x81\xe7\xba\xb8\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae" /* 壁纸已设置 */
#define WP_TXT_LOCKED     "\xe5\x86\x85\xe7\xbd\xae\xe5\xa3\x81\xe7\xba\xb8\xe4\xb8\x8d\xe5\x8f\xaf\xe5\x88\xa0\xe9\x99\xa4" /* 内置壁纸不可删除 */
#define WP_TXT_DELQ       "\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe5\xa3\x81\xe7\xba\xb8?" /* 删除该壁纸? */

static int s_wp_program = 0;      /* 当前动态壁纸程序 (0=星空) */
static int s_wp_min     = 10;     /* 壁纸时间 (0=未设置; 默认 10 分钟) */

int os_wallpaper_get_program(void) { return s_wp_program; }

static void wp_open_main(ui_ctx_t *ctx);
static void wp_open_dynamic(ui_ctx_t *ctx);
static void wp_open_min(ui_ctx_t *ctx);

/* ================= 通用: 删除下载壁纸确认 (内置项绝不进入本函数) ========= */
typedef void (*wp_del_do_fn)(int idx);
static void wp_del_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    /* ud = 下载壁纸删除执行函数指针 (下载壁纸当前未开放, 占位) */
    wp_del_do_fn fn = (wp_del_do_fn)ud;
    os_dialog_pop(ctx);                                    /* 关确认弹窗 → 回列表 */
    if (result == 0 && fn) fn(0);
    ctx->needs_redraw = true;
}
static void wp_del_check(ui_ctx_t *ctx, os_dlg_stack_t *d, int builtin_n,
                         wp_del_do_fn fn) {
    /* 长按列表项: 内置项锁定(仅提示), 下载项弹删除确认. */
    if (d->sel >= 0 && d->sel < d->count) {
        if (d->sel < builtin_n) {
            os_dialog_toast(ctx, WP_TXT_LOCKED);          /* 内置壁纸不可删除 */
        } else {
            os_dialog_confirm_ex(ctx, WP_TXT_DELQ, 0, 0,
                                 wp_del_confirm_cb, (void *)fn);
        }
    }
}

/* ============ 壁纸设置 (主列表) ============ */
static bool wp_main_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;   /* 返回行走默认 */
    switch (d->sel) {
    case 0: wp_open_dynamic(ctx); break;     /* 动态壁纸 → 选择列表 */
    case 1: wp_open_min(ctx); break;         /* 壁纸时间 */
    case 2: /* 测试壁纸: 立即全屏播放当前动态壁纸 (星空) */
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
    static const char *const items[3] = { WP_LABEL_DYNAMIC, WP_LABEL_WTIME, WP_LABEL_TEST };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 3;
    for (int i = 0; i < 3; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i]);
    dlg.sel = 0;
    dlg.on_key = wp_main_key;
    dlg.cb = wp_main_cb;
    os_dialog_push(ctx, &dlg);
}

/* ============ 动态壁纸 列表 (星空内置锁定 + 以后下载动画壁纸) ============ */
/* 内置动态壁纸数 (星空). 以后商店下载的动画壁纸按 n 索引追加到列表末尾. */
#define WP_DYNAMIC_BUILTIN 1
static bool wp_dynamic_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud);
static void wp_dynamic_open(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    int n = 0;
    snprintf(dlg.items[n], sizeof(dlg.items[n]), "%s%s",
             (s_wp_program == WP_PROG_STARS) ? "* " : "", WP_LABEL_STARS);
    n++;   /* 星空 (内置) */
    /* [下载扩展点] 以后商店下载的动画壁纸程序在此按 n++ 追加列表项 (可删). */
    dlg.count = n;
    dlg.sel = 0;
    dlg.on_key = wp_dynamic_key;
    os_dialog_push(ctx, &dlg);   /* 原地压入新层 (返回逐层回退到壁纸设置主列表) */
}
static bool wp_dynamic_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_LONG_PRESS) {
        wp_del_check(ctx, d, WP_DYNAMIC_BUILTIN, NULL);
        return true;
    }
    if (a != OS_ACTION_CONFIRM) return false;
    if (d->sel >= d->count) return false;
    if (d->sel == 0) {   /* 星空 */
        s_wp_program = WP_PROG_STARS;
        os_dialog_pop(ctx);          /* 弹掉本列表层 → 回父层 */
        wp_dynamic_open(ctx);        /* 重压同名层带 * 标记 (原地替换, 层级不加深) */
        os_dialog_toast(ctx, WP_TXT_SET);   /* 壁纸已设置 */
    }
    return true;
}
static void wp_open_dynamic(ui_ctx_t *ctx) { wp_dynamic_open(ctx); }

/* ============ 壁纸时间 (屏保运行时长, 超时软关机) ============ */
static const int wp_mins[] = { 1, 2, 3, 5, 10, 15, 20, 30 };
#define WP_MINS_N (int)(sizeof(wp_mins) / sizeof(wp_mins[0]))
static void wp_min_save(int v) {
    s_wp_min = v;
    svc_cfg_set_i32(WP_NS, "wp_run", v);
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
    /* 读 NVS (壁纸时间; 动态壁纸固定星空) */
    s_wp_min = (int)svc_cfg_get_i32(WP_NS, "wp_run", 10);
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
