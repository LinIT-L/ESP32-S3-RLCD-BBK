/**
 * page_usb_net.c — USB 网卡共享 (USB-RNDIS 有线网卡) 应用页模块.
 *
 * 无标题二级菜单 (仿设置页, 退出在底部):
 *   ① 启动网卡共享  ② 关闭网卡共享  ③ 网络配置(网关)  返回
 * 启动/关闭 → 提示「正在重启...」0.5s → 写/清 NVS 标记 + 重启 (进入/退出网卡模式).
 * 网络配置 → 滚轮上下滑编辑「网关：000.000.000.000」(仿修改时间, 数字间隔大).
 * 掩码固定 255.255.255.0, IP 池 = 网关+1 自动.
 *
 * 网络数据通路 (RNDIS 设备 + DHCP + NAT + esp_wifi STA 上行共享) 为后续阶段.
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "usb_net.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "USB_NET_UI"

/* ============ 网关滚轮编辑 (仿设置页"修改时间"方案, 数字间隔大) ============
 * 4 段 0..255, 显示为 000.000.000.000, 上下滑调值, 左右切段, 确认保存. */
#define NET_GW_COL     4
#define NET_GW_GAP     26   /* 数字间隔大 */
#define NET_GW_STEP_H 22    /* 上下相邻值行距 */
/* 弹窗几何: render / touch 必须共用同一套, 否则按钮点不中 */
#define NET_GW_DLG_W   350
#define NET_GW_DLG_H   200
#define NET_GW_BTN_H   40

static uint8_t s_gw[NET_GW_COL];   /* 当前编辑的网关 4 段 */
static int     s_gw_active = 0;

/* 保存并关闭 (硬件确认键 / 触摸点"确认" 共用) */
static void net_gw_confirm(ui_ctx_t *ctx) {
    uint32_t ip = ((uint32_t)s_gw[0] << 24) | ((uint32_t)s_gw[1] << 16) |
                  ((uint32_t)s_gw[2] << 8) | (uint32_t)s_gw[3];
    usb_net_set_gateway(ip);
    os_dialog_toast(ctx, "\xe7\xbd\x91\xe5\x85\xb3\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); /* 网关已保存 */
    os_dialog_pop(ctx);
}

/* 段文本: 3 位补0 */
static void net_gw_text(int col, int steps, char *out, size_t n) {
    int v = (int)s_gw[col] + steps;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    snprintf(out, n, "%03d", v);
}

static bool net_gw_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    const int W = NET_GW_DLG_W, H = NET_GW_DLG_H;
    int bx = (UI_SCREEN_W - W) / 2, by = (UI_SCREEN_H - H) / 2;
    /* 底色 + 边框 */
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    draw_rect_outline(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_BLACK);
    /* 标题: 网关：000.000.000.000 */
    const int cy = by + 46;
    int tx = bx + (W - text_width("\xe7\xbd\x91\xe5\x85\xb3\xef\xbc\x9a")) / 2;
    draw_text(lcd, tx, cy, "\xe7\xbd\x91\xe5\x85\xb3\xef\xbc\x9a", false);   /* 网关： */
    int x = tx + text_width("\xe7\xbd\x91\xe5\x85\xb3\xef\xbc\x9a");
    for (int c = 0; c < NET_GW_COL; c++) {
        char vs[8]; net_gw_text(c, 0, vs, sizeof(vs));
        int vw = text_width(vs);
        bool act = (c == s_gw_active);
        if (act) {
            fill_rect(lcd, x - 3, cy - 3, x + vw + 2, cy + 25, ST7305_COLOR_BLACK);
            draw_text(lcd, x, cy, vs, true);
        } else {
            draw_text(lcd, x, cy, vs, false);
        }
        /* 分隔点 */
        x += vw + NET_GW_GAP;
        if (c < NET_GW_COL - 1) {
            draw_ascii_medium(lcd, x - NET_GW_GAP + 6, cy + 8, '.', false);
        }
    }
    /* 底部 确认/取消 */
    int btop = by + H - NET_GW_BTN_H;
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, btop, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, btop, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int bty = btop + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, bty, "\xe7\xa1\xae\xe8\xae\xa4", false);    /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, bty, "\xe5\x8f\x96\xe6\xb6\x88", false); /* 取消 */
    return true;
}

static bool net_gw_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    switch (a) {
    case OS_ACTION_LEFT:
        s_gw_active = (s_gw_active + NET_GW_COL - 1) % NET_GW_COL; ctx->needs_redraw = true; return true;
    case OS_ACTION_RIGHT:
        s_gw_active = (s_gw_active + 1) % NET_GW_COL; ctx->needs_redraw = true; return true;
    case OS_ACTION_UP: {
        int v = (int)s_gw[s_gw_active] + 1; if (v > 255) v = 255; s_gw[s_gw_active] = (uint8_t)v;
        ctx->needs_redraw = true; return true;
    }
    case OS_ACTION_DOWN: {
        int v = (int)s_gw[s_gw_active] - 1; if (v < 0) v = 0; s_gw[s_gw_active] = (uint8_t)v;
        ctx->needs_redraw = true; return true;
    }
    case OS_ACTION_CONFIRM:
        net_gw_confirm(ctx);
        return true;
    default: return false;   /* BACK → 默认取消 */
    }
}

/* 触摸: 底部左=确认 右=取消; 内容区按 x 四等分点段切焦点.
 * 之前漏了 on_touch, 弹窗只画了按钮却没人处理点击 → "确认/取消按不动". */
static bool net_gw_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    const int W = NET_GW_DLG_W, H = NET_GW_DLG_H;
    int bx = (UI_SCREEN_W - W) / 2, by = (UI_SCREEN_H - H) / 2;
    if (x < bx || x > bx + W - 1 || y < by || y > by + H - 1) return false;
    if (y >= by + H - NET_GW_BTN_H) {
        int mid = bx + W / 2;
        if (x < mid) net_gw_confirm(ctx);   /* 确认 */
        else         os_dialog_pop(ctx);    /* 取消 */
        ctx->needs_redraw = true;
        return true;
    }
    int seg = (x - bx) / (W / NET_GW_COL);
    if (seg < 0) seg = 0;
    if (seg > NET_GW_COL - 1) seg = NET_GW_COL - 1;
    if (seg != s_gw_active) { s_gw_active = seg; ctx->needs_redraw = true; }
    return true;
}

/* 上下拖动调当前段: 累积余差, 每满 NET_GW_STEP_H 像素 ±1 */
static void net_gw_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    static int s_t_last = -1, s_t_acc = 0;
    int tx, ty;
    if (!input_get_touch_pos(&tx, &ty)) { s_t_last = -1; s_t_acc = 0; return; }
    if (s_t_last < 0) { s_t_last = ty; s_t_acc = 0; return; }
    s_t_acc += ty - s_t_last;
    s_t_last = ty;
    bool changed = false;
    while (s_t_acc <= -NET_GW_STEP_H) {
        int v = (int)s_gw[s_gw_active] + 1; if (v > 255) v = 255;
        s_gw[s_gw_active] = (uint8_t)v; s_t_acc += NET_GW_STEP_H; changed = true;
    }
    while (s_t_acc >= NET_GW_STEP_H) {
        int v = (int)s_gw[s_gw_active] - 1; if (v < 0) v = 0;
        s_gw[s_gw_active] = (uint8_t)v; s_t_acc -= NET_GW_STEP_H; changed = true;
    }
    if (changed) ctx->needs_redraw = true;
}

/* 打开网关编辑 */
static void open_gateway_edit(ui_ctx_t *ctx) {
    uint32_t gw = 0;
    usb_net_get_gateway(&gw);
    s_gw[0] = (gw >> 24) & 0xFF; s_gw[1] = (gw >> 16) & 0xFF;
    s_gw[2] = (gw >> 8) & 0xFF;  s_gw[3] = gw & 0xFF;
    s_gw_active = 0;
    os_dlg_stack_t d;
    memset(&d, 0, sizeof(d));
    d.no_footer = true;
    d.on_render = net_gw_render;
    d.on_key    = net_gw_key;
    d.on_touch  = net_gw_touch;
    d.on_poll   = net_gw_poll;
    os_dialog_push(ctx, &d);
}

/* ============ 启动/关闭 (一键切换, 提示 0.5s 重启) ============ */
static void net_run_done(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) { os_dialog_pop(ctx); return; }
    bool on = usb_net_is_running() || usb_net_is_active();   /* 运行时或已置标记都视为"开" */
    os_dialog_pop(ctx);
    os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe9\x87\x8d\xe5\x90\xaf..."); /* 正在重启... */
    os_render(ctx);
    if (on) usb_net_deactivate();   /* 关闭 → 恢复串口 */
    else    usb_net_activate();
}

/* 设置页导出: 打开与内置一致的"连接WiFi"流程 (扫描→选网→输密码) */
extern void settings_wifi_connect_open(ui_ctx_t *ctx);

/* 关闭当前弹窗; 若弹窗栈随之清空, 本页就没有内容可显示 → 直接退回上一级.
 * (同步处理, 不等 poll, 避免中间那一帧停留在没有 render 的空页面上) */
static void usbnet_close_or_leave(ui_ctx_t *ctx) {
    os_dialog_pop(ctx);
    if (os_dialog_depth() <= 0) os_pop(ctx);
}

/* ============ 二级菜单 (- 无标题, 退出在下) ============ */
static void usbnet_menu_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    bool on = usb_net_is_running() || usb_net_is_active();
    switch (result) {
    case 0:   /* 开启/关闭网卡模块 (一键切换) */
        os_dialog_confirm_ex(ctx,
            on ? "\xe7\xa1\xae\xe5\xae\x9a\xe5\x85\xb3\xe9\x97\xad\xe7\xbd\x91\xe5\x8d\xa1\xe6\xa8\xa1\xe5\x9d\x97?" : /* 确定关闭网卡模块? */
                 "\xe7\xa1\xae\xe5\xae\x9a\xe5\xbc\x80\xe5\x90\xaf\xe7\xbd\x91\xe5\x8d\xa1\xe6\xa8\xa1\xe5\x9d\x97?",    /* 确定开启网卡模块? */
            0, 0, net_run_done, NULL);
        break;
    case 1: settings_wifi_connect_open(ctx); break;   /* 连接WiFi (复用内置流程) */
    case 2: open_gateway_edit(ctx); break;            /* 网络配置 (网关) */
    default: usbnet_close_or_leave(ctx); break;       /* 返回 → 关菜单并退出本页 */
    }
}

void usb_net_open_menu(ui_ctx_t *ctx) {
    bool on = usb_net_is_running() || usb_net_is_active();
    os_dlg_stack_t d;
    memset(&d, 0, sizeof(d));
    d.count = 4;    /* 3 项 + 底部"返回" */
    /* 第 0 项按状态切换文案: 关闭态=开启网卡模块, 开启态=关闭网卡模块 */
    snprintf(d.items[0], sizeof(d.items[0]), "%s",
             on ? "\xe5\x85\xb3\xe9\x97\xad\xe7\xbd\x91\xe5\x8d\xa1\xe6\xa8\xa1\xe5\x9d\x97"   /* 关闭网卡模块 */
                : "\xe5\xbc\x80\xe5\x90\xaf\xe7\xbd\x91\xe5\x8d\xa1\xe6\xa8\xa1\xe5\x9d\x97"); /* 开启网卡模块 */
    snprintf(d.items[1], sizeof(d.items[1]), "WiFi \xe8\xbf\x9e\xe6\x8e\xa5"); /* 连接WiFi */
    snprintf(d.items[2], sizeof(d.items[2]), "\xe7\xbd\x91\xe7\xbb\x9c\xe9\x85\x8d\xe7\xbd\xae"); /* 网络配置 */
    d.sel = 0;
    d.cb = usbnet_menu_cb;
    /* 无标题; 底部退行为末项, 需 footer 返回字段 */
    snprintf(d.footer, sizeof(d.footer), "%s", "\xe8\xbf\x94\xe5\x9b\x9e"); /* 返回 */
    os_dialog_push(ctx, &d);
}

/* 应用页模块
 * 本页自身没有画面 (进入即弹菜单), 弹窗全部关闭后就无内容可显示.
 * 之前只挂了 on_enter: 点"返回"/"取消"只关掉弹窗, 页面仍留在栈上且没有 render/action,
 * 于是表现为"点了没反应、卡住". 这里补 poll (弹窗栈空即自动退回上一级) 与 action 兜底. */
static void p_usbnet_enter(ui_ctx_t *ctx) {
    usb_net_open_menu(ctx);
}
static void p_usbnet_poll(ui_ctx_t *ctx) {
    if (os_dialog_depth() <= 0) os_pop(ctx);
}
static void p_usbnet_action(ui_ctx_t *ctx, os_action_t a) {
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) {
        if (os_dialog_depth() > 0) return;   /* 弹窗在 → 交给弹窗层处理 */
        os_pop(ctx);
    }
}

static const os_module_t s_mod_usbnet = {
    .name      = "usb_net",
    .page_id   = OS_PAGE_USB_NET,
    .on_enter  = p_usbnet_enter,
    .poll      = p_usbnet_poll,
    .action    = p_usbnet_action,
    .fullscreen = false,
};

void os_page_usb_net_register(void) {
    os_register(&s_mod_usbnet);
}