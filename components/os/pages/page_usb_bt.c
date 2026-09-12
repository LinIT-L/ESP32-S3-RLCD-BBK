/**
 * page_usb_bt.c — USB 蓝牙适配器 (USB-BT) 应用页模块.
 *
 * 需求 (两键确认弹窗, 原规格, 无图标, 16px 文字):
 *   - 未启动时点开 → 16px「启动USB蓝牙需重启」+ [确认/取消]
 *   - 点取消 / 按返回 → 立即关闭弹窗 (不卡顿, 点击即响应)
 *   - 点确认 → 提示「正在重启...」→ 重启 → 正常进桌面 + 后台运行蓝牙适配器
 *   - 已启动时再次打开 → 16px「关闭USB蓝牙需重启」+ [关闭/取消]
 *   - 点关闭 → 提示「正在重启...」→ 重启 → 回到正常串口模式桌面
 *
 * 任一操作后台执行: 确认/关闭只写 NVS 标记后重启, 不阻塞界面, 点击立即可见反馈弹窗.
 *
 * 实现: 进 USB-BT 用 NVS 标记 + 重启 (与手柄蓝牙互斥, 官方 usb_dongle 方案).
 * 重启后 main 统一进桌面 (OS_PAGE_MAIN), 蓝牙适配器后台运行; 状态栏显示 USB-BT 图标.
 */
#include "os.h"
#include "ui_common.h"
#include "usb_bt.h"
#include <string.h>
#include <stdio.h>

#define TAG "USB_BT_UI"

/* 两键确认回调: 确认/关闭 → 提示"正在重启"并重启; 取消/返回 → 立即关弹窗并退出本页.
 * 取消不仅要关弹窗, 还要把本页一起弹出 —— 本页没有自己的画面, 只关弹窗会停在
 * 一个无 render/无 action 的空页面上, 表现就是"取消按不动、之后点什么都没反应". */
static void usbbt_confirm_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result == 0) {   /* 确认(未启动)/关闭(已启动) */
        bool active = usb_bt_is_active();
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe9\x87\x8d\xe5\x90\xaf..."); /* 正在重启... */
        /* toast 只标记重绘, 真正渲染+flush 在 os_render; 这里显式触发一次,
         * 让"正在重启"先画到屏上, 再后台延迟重启 (0.5s 可见) */
        os_render(ctx);
        if (active) {
            usb_bt_deactivate();   /* 关闭: 清标记+断开USB+重启回正常串口 (后台执行, 不返回) */
        } else {
            usb_bt_activate();     /* 启动: 写标记 + 重启进 BTH 后台 (后台执行, 不返回) */
        }
    } else {             /* 取消 / 返回 */
        os_dialog_pop(ctx);   /* 立即关闭弹窗 */
    }
}

/* 打开两键确认弹窗 (按当前状态显示 启动/关闭) */
void usb_bt_open_confirm(ui_ctx_t *ctx)
{
    bool active = usb_bt_is_active();
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    if (active) {
        /* 已启动: 关闭 USB 蓝牙需重启 */
        snprintf(dlg.title, sizeof(dlg.title), "\xe5\x85\xb3\xe9\x97\xadUSB\xe8\x93\x9d\xe7\x89\x99\xe9\x9c\x80\xe9\x87\x8d\xe5\x90\xaf"); /* 关闭USB蓝牙需重启 */
        snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe5\x85\xb3\xe9\x97\xad"); /* 关闭 */
        snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe5\x8f\x96\xe6\xb6\x88"); /* 取消 */
    } else {
        /* 未启动: 启动 USB 蓝牙需重启 */
        snprintf(dlg.title, sizeof(dlg.title), "\xe5\x90\xaf\xe5\x8a\xa8USB\xe8\x93\x9d\xe7\x89\x99\xe9\x9c\x80\xe9\x87\x8d\xe5\x90\xaf"); /* 启动USB蓝牙需重启 */
        snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe7\xa1\xae\xe8\xae\xa4"); /* 确认 */
        snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe5\x8f\x96\xe6\xb6\x88"); /* 取消 */
    }
    dlg.count = 2;
    dlg.sel = 0;
    dlg.no_footer = true;          /* 无底部"返回", 原规格 */
    dlg.small = true;              /* 小自适应确认弹窗 */
    dlg.cb = usbbt_confirm_cb;
    os_dialog_push(ctx, &dlg);
}

/* ============ 应用页模块 (主菜单打开即弹确认窗) ============
 * 本页自身没有画面 (进入即弹确认窗), 弹窗关闭后就无内容可显示.
 * 之前只挂了 on_enter: 点"取消"/按返回只关掉弹窗, 页面仍留在栈上且没有 render/action,
 * 于是表现为"取消按不动、之后点什么都没反应". 这里补 poll (弹窗栈空即自动退回上一级)
 * 与 action 兜底. */
static void p_usbbt_enter(ui_ctx_t *ctx)
{
    usb_bt_open_confirm(ctx);
}
static void p_usbbt_poll(ui_ctx_t *ctx)
{
    if (os_dialog_depth() <= 0) os_pop(ctx);
}
static void p_usbbt_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) {
        if (os_dialog_depth() > 0) return;   /* 弹窗在 → 交给弹窗层处理 */
        os_pop(ctx);
    }
}

static const os_module_t s_mod_usbbt = {
    .name      = "usb_bt",
    .page_id   = OS_PAGE_USB_BT,
    .on_enter  = p_usbbt_enter,
    .poll      = p_usbbt_poll,
    .action    = p_usbbt_action,
    .fullscreen = false,
};

void os_page_usb_bt_register(void)
{
    os_register(&s_mod_usbbt);
}