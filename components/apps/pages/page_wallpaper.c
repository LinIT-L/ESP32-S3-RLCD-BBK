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
 * 内置项不改已存 NVS 键/默认值/控制流 (os_wp: min/wp_shut); 仅组织 UI 列表.
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
#define WP_LABEL_LIFE     "\xe4\xba\xba\xe7\x94\x9f\xe6\x8c\x87\xe5\x8d\x97"                            /* 人生指南 */
#define WP_LABEL_TEST     "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xa3\x81\xe7\xba\xb8"   /* 测试壁纸 */
#define WP_TXT_SET        "\xe5\xa3\x81\xe7\xba\xb8\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae" /* 壁纸已设置 */
#define WP_TXT_LOCKED     "\xe5\x86\x85\xe7\xbd\xae\xe5\xa3\x81\xe7\xba\xb8\xe4\xb8\x8d\xe5\x8f\xaf\xe5\x88\xa0\xe9\x99\xa4" /* 内置壁纸不可删除 */
#define WP_TXT_DELQ       "\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe5\xa3\x81\xe7\xba\xb8?" /* 删除该壁纸? */

static int s_wp_program = 0;      /* 当前动态壁纸程序 (0=星空) */
/* ★ 2026-10-26 linit「壁纸不需要设置壁纸时间」: 本页的"壁纸时间"入口已删除 ——
 *   它其实和「设置 → 休眠与电源 → 关机时间」是同一个键 (os_wp/wp_shut),
 *   两个入口两个名字只会让人困惑。关机时间统一只在设置页改。 */

int os_wallpaper_get_program(void) { return s_wp_program; }

static void wp_open_main(ui_ctx_t *ctx);
static void wp_open_dynamic(ui_ctx_t *ctx);

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
    case 1: /* 测试壁纸: 立即全屏播放当前动态壁纸 (星空) */
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
    static const char *const items[2] = { WP_LABEL_DYNAMIC, WP_LABEL_TEST };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 2;
    for (int i = 0; i < 2; i++)
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
    n++;   /* 星空 (内置, 动态) */
    /* ★ 2026-10-23 linit: **没安装就不显示这一项** ——
     *   人生指南是 Lua 应用，卸载后壁纸仍然可选会导致屏保没内容。
     *   判据: /store/apps/app/lifeguide.lua 能否打开。 */
    if (wp_lifeguide_installed()) {
        snprintf(dlg.items[n], sizeof(dlg.items[n]), "%s%s",
                 (s_wp_program == WP_PROG_LIFEGUIDE) ? "* " : "", WP_LABEL_LIFE);
        n++;   /* 人生指南 (静态文字: 每分钟随机换一条) */
    } else if (s_wp_program == WP_PROG_LIFEGUIDE) {
        s_wp_program = WP_PROG_STARS;                 /* 已卸载 → 回星空 */
        svc_cfg_set_i32(WP_NS, "prog", WP_PROG_STARS);
    }
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
    if (d->sel == 0 || d->sel == 1) {   /* 0=星空 1=人生指南 */
        s_wp_program = (d->sel == 0) ? WP_PROG_STARS : WP_PROG_LIFEGUIDE;
        svc_cfg_set_i32(WP_NS, "prog", s_wp_program);   /* 落盘: 屏保任务据此决定画哪个 */
        os_dialog_pop(ctx);          /* 弹掉本列表层 → 回父层 */
        wp_dynamic_open(ctx);        /* 重压同名层带 * 标记 (原地替换, 层级不加深) */
        os_dialog_toast(ctx, WP_TXT_SET);   /* 壁纸已设置 */
    }
    return true;
}
static void wp_open_dynamic(ui_ctx_t *ctx) { wp_dynamic_open(ctx); }

/* ★ 2026-10-26 linit: 删除"壁纸时间"子列表。
 *   原 wp_open_min / wp_min_save / wp_mins[] 一行不留 —— 它改的就是设置页的「关机时间」
 *   (os_wp/wp_shut), 同一个东西两个入口两个名字。要改关机时间请走
 *   设置 → 休眠与电源 → 关机时间 (那里有更全的档位: 5分钟~10天, 含"不关机")。 */

/* ============ 模块契约 ============ */
static void p_wp_enter(ui_ctx_t *ctx) {
    /* 读 NVS (动态壁纸; 默认星空) */
    s_wp_program = (int)svc_cfg_get_i32(WP_NS, "prog", WP_PROG_STARS);
    wp_open_main(ctx);
}

static const os_module_t s_mod_wallpaper = {
    .name       = "wallpaper",
    .page_id    = OS_PAGE_WALLPAPER,
    .on_enter   = p_wp_enter,
    .fullscreen = false,
    .exit_confirm = true,   /* ★ 2026-10-25 linit: 应用页按返回键先弹"退出程序?" */
};

void os_page_wallpaper_register(void) { os_register(&s_mod_wallpaper); }
