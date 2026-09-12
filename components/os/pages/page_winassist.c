/**
 * page_winassist.c — 运维助手 (基于「仿真键鼠」模板改造).
 *
 * USB 复合(进入): HID 键盘 + 鼠标 + CDC 串口 (usb_hid_start).
 * 界面:
 *   - 顶部: 分类栏 Tab (系统/硬件/网络/维护/高级), 选中 Tab 左/上/右三边;
 *     当前分类下 2排x4列 快捷键按钮 (无边框, 仅名称, 靠上), 点按发组合键或 Win+R+命令.
 *   - 下部: 触控板(滑动=鼠标,单点=左键,长按=右键); 底部中央左/右键
 *     (左键左上圆, 右键右上圆, 其余直角), 直铺到底.
 *   - 返回键退出恢复串口.
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "usb_hid.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TAG "WINPT"

#define MOD_LCTRL  (1U << 0)
#define MOD_LSHIFT (1U << 1)
#define MOD_LGUI   (1U << 3)
#define HIDK_A  0x04
#define HIDK_R  0x15
#define HIDK_ESC 0x29
#define HIDK_ENTER 0x28
#define HIDK_SPACE 0x2C
#define HIDK_DOT 0x37
#define HIDK_MINUS 0x2D
#define HID_MOUSE_L 0x01
#define HID_MOUSE_R 0x02

/* 布局 */
#define WA_CAT_H    36
#define WA_BTN_Y0   WA_CAT_H           /* 按钮区顶部(紧贴分类栏下) */
#define WA_BTN_ROWS 2                  /* 按钮两排, 靠上 */
#define WA_BTN_ROW_H 45
#define WA_BTN_Y1   (WA_BTN_Y0 + WA_BTN_ROWS * WA_BTN_ROW_H)   /* 126 */
#define WA_TP_Y0    WA_BTN_Y1
#define WA_TP_Y1    299
#define WA_BTN_COLS 4
#define WA_BTN_W    (400 / WA_BTN_COLS)
#define WA_MB_Y     260
#define WA_MB_H     (WA_TP_Y1 - WA_MB_Y + 1)
#define WA_MB_W     100
#define WA_MB_XL    100
#define WA_MB_XR    200

/* 分类 */
enum { CAT_SYS = 0, CAT_HW, CAT_NET, CAT_MNT, CAT_ADV, CAT_N };
static const char *cat_names[CAT_N] = { "系统", "硬件", "网络", "维护", "高级" };

enum { KT_TASK, KT_CMD };
typedef struct { const char *label; uint8_t kind; const char *cmd; } wa_item_t;
#define WA_ITEMS_MAX 8
static const wa_item_t cat_items[CAT_N][WA_ITEMS_MAX] = {
    /* 系统 */
    { { "任务管理", KT_TASK, NULL }, { "控制面板", KT_CMD, "control" },
      { "设备管理", KT_CMD, "devmgmt.msc" }, { "系统信息", KT_CMD, "msinfo32" },
      { "命令提示", KT_CMD, "cmd" }, { "运行", KT_CMD, "cmd /k echo run" },
      { "注册表", KT_CMD, "regedit" }, { "磁盘清理", KT_CMD, "cleanmgr" } },
    /* 硬件 */
    { { "磁盘管理", KT_CMD, "diskmgmt.msc" }, { "电源选项", KT_CMD, "powercfg.cpl" },
      { "设备状态", KT_CMD, "devmgmt.msc" }, { "打印机", KT_CMD, "devices.msc" },
      { "USB设备", KT_CMD, "devmgmt.msc" }, { "音量", KT_CMD, "mmsys.cpl" },
      { "鼠标", KT_CMD, "main.cpl" }, { "键盘", KT_CMD, "main.cpl" } },
    /* 网络 */
    { { "网络连接", KT_CMD, "ncpa.cpl" }, { "IP信息", KT_CMD, "cmd /k ipconfig /all" },
      { "DNS查看", KT_CMD, "cmd /k ipconfig /displaydns" }, { "网络测试", KT_CMD, "cmd /k ping www.baidu.com" },
      { "防火墙", KT_CMD, "firewall.cpl" }, { "共享中心", KT_CMD, "wf.msc" },
      { "网络设置", KT_CMD, "ms-settings:network" }, { "网络诊断", KT_CMD, "msdt /id NetworkDiagnostics" } },
    /* 维护 */
    { { "事件日志", KT_CMD, "eventvwr.msc" }, { "启动管理", KT_CMD, "msconfig" },
      { "系统还原", KT_CMD, "rstrui" }, { "磁盘检查", KT_CMD, "cmd /k chkdsk" },
      { "服务管理", KT_CMD, "services.msc" }, { "卸载程序", KT_CMD, "appwiz.cpl" },
      { "屏幕键盘", KT_CMD, "osk" }, { "放大镜", KT_CMD, "magnify" } },
    /* 高级: 复合高端命令 */
    { { "电池报告", KT_CMD, "powercfg /batteryreport" }, { "能耗报告", KT_CMD, "powercfg /energy" },
      { "系统文件检查", KT_CMD, "cmd /k sfc /scannow" }, { "内存诊断", KT_CMD, "mdsched" },
      { "系统信息生成", KT_CMD, "msinfo32" }, { "完整诊断", KT_CMD, "dxdiag" },
      { "性能报告", KT_CMD, "perfmon /report" }, { "启动修复", KT_CMD, "cmd /k bootrec" } },
};
static int g_cat = CAT_SYS;

static uint8_t wa_char_hid(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(HIDK_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(HIDK_A + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(0x1E + (c - '0'));
    switch (c) {
        case ' ': return HIDK_SPACE;
        case '.': return HIDK_DOT;
        case '-': return HIDK_MINUS;
        case ':': return 0x33;
        case '/': return 0x38;
        default: return 0;
    }
}
static void wa_send_str(const char *s) {
    for (; *s; s++) {
        char c = *s;
        uint8_t mod = 0, k = wa_char_hid(c);
        if (k == 0) continue;
        if (c >= 'A' && c <= 'Z') mod |= MOD_LSHIFT;
        usb_hid_key_tap(mod, k);
    }
}
static void wa_win_cmd(const char *cmd) {
    usb_hid_key_tap(MOD_LGUI, HIDK_R);
    vTaskDelay(pdMS_TO_TICKS(60));
    wa_send_str(cmd);
    vTaskDelay(pdMS_TO_TICKS(40));
    usb_hid_key_tap(0, HIDK_ENTER);
}
static void wa_item_exec(const wa_item_t *it) {
    if (it->kind == KT_TASK) usb_hid_key_tap(MOD_LCTRL | MOD_LSHIFT, HIDK_ESC);
    else if (it->cmd) wa_win_cmd(it->cmd);
}

static void wa_draw_btn_corner(st7305_handle_t *l, int x0, int y0, int x1, int y1, int r, bool top_left); /* fwd */
static void wa_label(st7305_handle_t *l, int x, int w, int y, int h, const char *s, bool inv) {
    int n = (int)(strlen(s) / 3);
    int tw = n * 16;
    if (tw > w) tw = w;
    int tx = x + (w - tw) / 2, ty = y + (h - 16) / 2;
    if (ty < 0) ty = 0;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    for (int i = 0; i < n; i++) {
        int idx = font_zh16_find_utf8(s + i * 3);
        if (idx < 0) continue;
        const uint8_t *bmp = zh16_font_data[idx];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                int byte = bmp[r * 2 + (c / 8)];
                st7305_draw_pixel(l, tx + i * 16 + c, ty + r, (byte & (1 << (7 - (c % 8)))) ? fg : bg);
            }
    }
}

static void ws_render(ui_ctx_t *ctx) {
    st7305_handle_t *l = ctx->lcd;
    if (!l) return;
    st7305_clear(l, ST7305_COLOR_WHITE);

    /* 分类 Tab: 选中左/上/右三边, 未选只有下边 */
    int cat_w = 400 / CAT_N;
    for (int c = 0; c < CAT_N; c++) {
        bool sel = (c == g_cat);
        int c0 = c * cat_w, c1 = c0 + cat_w - 1;
        if (sel) {
            draw_vline(l, c0, 0, WA_CAT_H - 1, ST7305_COLOR_BLACK);
            draw_hline(l, c0, c1, 0, ST7305_COLOR_BLACK);
            draw_vline(l, c1, 0, WA_CAT_H - 1, ST7305_COLOR_BLACK);
        } else {
            draw_hline(l, c0, c1, WA_CAT_H - 1, ST7305_COLOR_BLACK);
        }
        wa_label(l, c0, cat_w, 4, WA_CAT_H - 8, cat_names[c], false);
    }

    /* 按钮: 两排靠上, 无边框, 仅名称 */
    for (int r = 0; r < WA_BTN_ROWS; r++) {
        int by = WA_BTN_Y0 + r * WA_BTN_ROW_H;
        for (int c = 0; c < WA_BTN_COLS; c++) {
            int idx = r * WA_BTN_COLS + c;
            if (idx >= WA_ITEMS_MAX) break;
            const wa_item_t *it = &cat_items[g_cat][idx];
            wa_label(l, c * WA_BTN_W, WA_BTN_W, by, WA_BTN_ROW_H, it->label, false);
        }
    }

    /* 触控板无外框直铺到底; 底部中央并排左/右键 (左键左上圆, 右键右上圆) */
    wa_draw_btn_corner(l, WA_MB_XL, WA_MB_Y, WA_MB_XL + WA_MB_W - 1, WA_MB_Y + WA_MB_H - 1, 8, true);
    wa_draw_btn_corner(l, WA_MB_XR, WA_MB_Y, WA_MB_XR + WA_MB_W - 1, WA_MB_Y + WA_MB_H - 1, 8, false);
    wa_label(l, WA_MB_XL, WA_MB_W, WA_MB_Y, WA_MB_H, "左键", false);
    wa_label(l, WA_MB_XR, WA_MB_W, WA_MB_Y, WA_MB_H, "右键", false);
}

static void wa_draw_btn_corner(st7305_handle_t *l, int x0, int y0, int x1, int y1, int r, bool top_left) {
    if (r < 1 || r * 2 >= x1 - x0 + 1 || r * 2 >= y1 - y0 + 1) {
        draw_rect_outline(l, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        return;
    }
    if (top_left) {
        draw_vline(l, x1, y0, y1, ST7305_COLOR_BLACK);
        draw_hline(l, x0 + r, x1, y0, ST7305_COLOR_BLACK);
        draw_hline(l, x0, x1, y1, ST7305_COLOR_BLACK);
        draw_vline(l, x0, y0 + r, y1, ST7305_COLOR_BLACK);
        for (int px = x0; px <= x0 + r; px++) {
            int dx = px - (x0 + r), dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(l, px, y0 + dy, ST7305_COLOR_BLACK);
        }
    } else {
        draw_vline(l, x0, y0, y1, ST7305_COLOR_BLACK);
        draw_hline(l, x0, x1 - r, y0, ST7305_COLOR_BLACK);
        draw_hline(l, x0, x1, y1, ST7305_COLOR_BLACK);
        draw_vline(l, x1, y0 + r, y1, ST7305_COLOR_BLACK);
        for (int px = x1 - r; px <= x1; px++) {
            int dx = px - (x1 - r), dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(l, px, y0 + dy, ST7305_COLOR_BLACK);
        }
    }
}

/* ===== 触控板(当鼠标) ===== */
static bool s_tp_active;
static int  s_tp_last_x, s_tp_last_y, s_tp_accum_x, s_tp_accum_y, s_tp_moved;
static uint32_t s_tp_down_ms;
static bool s_tp_right_sent;
static uint32_t ws_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void wa_trackpad(ui_ctx_t *ctx) {
    (void)ctx;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    uint32_t now = ws_now();
    bool in_tp = down && tx >= 0 && tx < 400 && ty >= WA_TP_Y0 && ty <= WA_TP_Y1;
    if (in_tp && ty >= WA_MB_Y) {
        bool on_btn = (tx >= WA_MB_XL && tx < WA_MB_XL + WA_MB_W) ||
                      (tx >= WA_MB_XR && tx < WA_MB_XR + WA_MB_W);
        if (on_btn) in_tp = false;
    }
    if (down && !s_tp_active) {
        if (in_tp) {
            s_tp_active = true; s_tp_last_x = tx; s_tp_last_y = ty;
            s_tp_down_ms = now; s_tp_moved = 0; s_tp_accum_x = s_tp_accum_y = 0; s_tp_right_sent = false;
        }
        return;
    }
    if (!s_tp_active) return;
    if (down) {
        if (in_tp) {
            int dx = tx - s_tp_last_x, dy = ty - s_tp_last_y;
            s_tp_last_x = tx; s_tp_last_y = ty;
            if (dx != 0 || dy != 0) {
                int sp = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                s_tp_moved += sp;
                float g = 1.0f + 0.25f * (float)sp; if (g > 6.0f) g = 6.0f;
                s_tp_accum_x += (int)(dx * g); s_tp_accum_y += (int)(dy * g);
                int mx = s_tp_accum_x, my = s_tp_accum_y;
                if (mx > 127) mx = 127; else if (mx < -128) mx = -128;
                if (my > 127) my = 127; else if (my < -128) my = -128;
                if (mx || my) usb_hid_mouse_move((int8_t)mx, (int8_t)my);
                s_tp_accum_x = 0; s_tp_accum_y = 0;
            }
            if (!s_tp_right_sent && s_tp_moved < 12 && now - s_tp_down_ms >= 500) {
                usb_hid_mouse_click(HID_MOUSE_R); s_tp_right_sent = true;
            }
        }
    } else {
        if (s_tp_moved < 12 && !s_tp_right_sent) usb_hid_mouse_click(HID_MOUSE_L);
        s_tp_active = false; s_tp_moved = 0;
    }
}

static bool ws_touch(ui_ctx_t *ctx, int x, int y) {
    if (y < WA_CAT_H) {   /* 分类栏 */
        int c = x / (400 / CAT_N);
        if (c >= 0 && c < CAT_N) g_cat = c;
        ctx->needs_redraw = true;
        return false;
    }
    if (y >= WA_BTN_Y0 && y < WA_BTN_Y1) {   /* 按钮 2排x4列 */
        int row = (y - WA_BTN_Y0) / WA_BTN_ROW_H;
        int col = x / WA_BTN_W;
        int idx = row * WA_BTN_COLS + col;
        if (row < WA_BTN_ROWS && col < WA_BTN_COLS && idx < WA_ITEMS_MAX)
            wa_item_exec(&cat_items[g_cat][idx]);
        return true;
    }
    if (y >= WA_MB_Y && y < WA_MB_Y + WA_MB_H) {
        if (x >= WA_MB_XL && x < WA_MB_XL + WA_MB_W) usb_hid_mouse_click(HID_MOUSE_L);
        else if (x >= WA_MB_XR && x < WA_MB_XR + WA_MB_W) usb_hid_mouse_click(HID_MOUSE_R);
        return true;
    }
    return false;
}

static void ws_poll(ui_ctx_t *ctx) { wa_trackpad(ctx); }
static void ws_action(ui_ctx_t *ctx, os_action_t action) {
    if (action == OS_ACTION_BACK || action == OS_ACTION_HOME) {
        usb_hid_key_release();
        usb_hid_mouse_release(HID_MOUSE_L | HID_MOUSE_R);
        usb_hid_stop();
        os_pop(ctx);
    }
}
static void ws_enter(ui_ctx_t *ctx) {
    esp_err_t ret = usb_hid_start();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "运维助手 USB 启动失败: %s", esp_err_to_name(ret)); os_pop(ctx); return; }
    input_set_swipe_back(false);
    s_tp_active = false;
}
static void ws_exit(ui_ctx_t *ctx) {
    (void)ctx;
    input_set_swipe_back(true);
    usb_hid_key_release();
    usb_hid_mouse_release(HID_MOUSE_L | HID_MOUSE_R);
    usb_hid_stop();
}

static const os_module_t s_mod_winassist = {
    .name = "winassist", .page_id = OS_PAGE_WINASSIST,
    .on_enter = ws_enter, .on_exit = ws_exit,
    .render = ws_render, .action = ws_action, .touch = ws_touch, .poll = ws_poll,
    .fullscreen = true,
};
void os_page_winassist_register(void) { os_register(&s_mod_winassist); }