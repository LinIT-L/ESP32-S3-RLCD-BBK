/**
 * svc_modtool.c — 修改机状态栏常驻服务.
 *
 * 职责:
 *  1. 非全屏/非弹窗/非屏保时, 在状态栏右侧常驻绘制 mini 修改机图标 (32x16);
 *  2. 该图标的热区放大 4 倍 (64x32, 覆盖图标左上扩充), 触摸点击弹二级菜单:
 *        - 修改机未运行: [打开修改机, 返回]
 *        - 修改机已运行: [修改机运行中... 关闭, 返回]  (确认框 → 关闭)
 *     二级弹窗用 os_dialog_push 压栈, 选"打开修改机"后 os_push 进修改机页面.
 *
 * 渲染时机: os_core 只在"当前页非全屏 && 非弹窗 && 非屏保"时调用 service render,
 * 故此处不需要再重复判屏保/全屏 (与 svc_statusbar 一致).
 */
#include "os.h"
#include "os_modtool.h"
#include "ui_common.h"
#include "modtool_icons.inc"
#include <string.h>
#include <stdio.h>

/* ============ 运行态标志 (唯一实现源; page_modtool 经 os_modtool.h 调用) ============ */
static bool s_modtool_running = false;
void os_modtool_set_running(bool on) { s_modtool_running = on; }
bool os_modtool_is_running(void)     { return s_modtool_running; }

/* ============ 绘制: mini 图标 (24x24 源图一比一, 用户要求规格) ============
 * 状态栏高 24px: 24x24 图标从 y=0 顶满整条状态栏 (y=0..24).
 * 位置: 左上"日期"区(约止于 x118)与正中"时间"区(约起于 x170)之间的空档 x=128.
 * 竖屏(300 宽)时间紧贴日期无空档 → 仅横屏绘制.
 * 热区: 状态栏整行高内横向放宽便于点按 (24->48 宽, 高度保持状态栏 24). */
#define MODTOOL_ICON_X   128   /* 横屏 400: 日期右(~118) / 时间左(~170) 空档 */
#define MODTOOL_ICON_Y   0     /* 24x24 顶满状态栏 (高 24) */

/* 热区 (易点按): 48 宽 x 24 高 (整条状态栏高), 图标水平居中于热区 */
#define MODTOOL_HOT_W    48
#define MODTOOL_HOT_H    24
static int modtool_hot_x(void) { return MODTOOL_ICON_X - 12; }   /* 图标左右各扩 12 */
static int modtool_hot_y(void) { return 0; }                      /* 状态栏整行 */

/* 二级菜单回调: 选中"打开修改机"→ 关菜单并进页面.
 * 若修改机会话已在运行, 页面 on_enter 会弹"继续运行/关闭修改机" (单一口径). */
static void modtool_menu_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result == 0) {
        os_dialog_clear_all(ctx);      /* 关掉本菜单再进页面 */
        os_push(ctx, OS_PAGE_MODTOOL);
    }
    /* result<0 = 取消/BACK 自动关, 无需处理 */
}

static bool svc_modtool_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 不开机启动: 仅当修改机会话激活(running)时才显示 mini 图标/响应点击 */
    if (!os_modtool_is_running()) return false;
    /* 竖屏(300)状态栏无空档放图标 → 不响应 */
    if (ui_screen_w() < 400) return false;
    int hx = modtool_hot_x(), hy = modtool_hot_y();
    if (x < hx || x >= hx + MODTOOL_HOT_W) return false;
    if (y < hy || y >= hy + MODTOOL_HOT_H) return false;
    /* 已弹着菜单则不再重复弹 (边沿去抖) */
    if (os_dialog_depth() > 0) return true;

    static const char *items[2] = {
        "\xe6\x89\x93\xe5\xbc\x80\xe4\xbf\xae\xe6\x94\xb9\xe6\x9c\xba",  /* 打开修改机 */
        "\xe8\xbf\x94\xe5\x9b\x9e",                                       /* 返回 */
    };
    os_dialog_list(ctx,
                   "\xe4\xbf\xae\xe6\x94\xb9\xe6\x9c\xba",  /* 修改机 */
                   items, 2, 0, modtool_menu_cb, NULL);
    return true;
}

static void svc_modtool_render(ui_ctx_t *ctx)
{
    /* 不开机启动: 仅修改机会话激活(running)时显示 mini 图标 */
    if (!os_modtool_is_running()) return;
    /* 竖屏(300)状态栏无空档放图标 → 不绘制 */
    if (ui_screen_w() < 400) return;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_draw_bitmap_1bit(lcd, MODTOOL_ICON_X, MODTOOL_ICON_Y,
                            MODTOOL_MINI_W, MODTOOL_MINI_H, modtool_mini_icon);
}

static const os_service_t s_svc_modtool = {
    .name   = "modtool",
    .render = svc_modtool_render,
    .touch  = svc_modtool_touch,
};

void os_svc_modtool_register(void)
{
    os_register_service(&s_svc_modtool);
}
