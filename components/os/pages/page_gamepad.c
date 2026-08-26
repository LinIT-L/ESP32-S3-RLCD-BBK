/**
 * page_gamepad.c — 手柄配置 页面模块 (P3 迁移: 从 menu_system.c 的 gamepad 弹窗体系迁移).
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 6 项 (添加设备/按键映射/连接记录/设备信息/WiFi手柄/返回).
 * 蓝牙能力来自 bt_ctl 组件 (bt_manager_*), WiFi 手柄来自 web_gamepad.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "bt_manager.h"
#include "web_gamepad.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define TAG "PGPAD"

/* 主菜单项 (UTF-8) */
static const char *const s_gp_items[6] = {
    "\xe6\xb7\xbb\xe5\x8a\xa0\xe8\xae\xbe\xe5\xa4\x87",  /* 添加设备 */
    "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84",  /* 按键映射 */
    "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95",  /* 连接记录 */
    "\xe8\xae\xbe\xe5\xa4\x87\xe4\xbf\xa1\xe6\x81\xaf",  /* 设备信息 */
    "WiFi \xe6\x89\x8b\xe6\x9f\x84",                      /* WiFi 手柄 */
    "\xe8\xbf\x94\xe5\x9b\x9e",                            /* 返回 */
};

/* 扫描结果列表 */
#define GP_SCAN_MAX 16
static bt_device_t s_scan_devs[GP_SCAN_MAX];
static int  s_scan_n = 0;
static bool s_scan_active = false;

/* 电量显示 */
static void gp_batt_str(uint8_t batt, char *out, size_t sz) {
    if (batt >= 101) snprintf(out, sz, "100%%");
    else snprintf(out, sz, "%d%%", batt);
}

/* 扫描回调: 填充结果列表 (bt_ctl 在任务上下文回调) */
static void gp_on_scan(bt_device_t *results, int count, bool updated) {
    (void)updated;
    if (count > GP_SCAN_MAX) count = GP_SCAN_MAX;
    s_scan_n = count;
    for (int i = 0; i < count; i++) s_scan_devs[i] = results[i];
}

/* ---- 设备信息弹窗 (btinfo) ---- */
static bool btinfo_dlg_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    char lines[8][40];
    int n = 0;
    const btm_conn_info_t *info = bt_manager_get_conn_info();
    if (info && info->available && bt_manager_is_connected()) {
        char btmp[16];
        snprintf(lines[n++], 40, "\xe5\x90\x8d\xe7\xa7\xb0: %s", info->name[0] ? info->name : "?");
        snprintf(lines[n++], 40, "\xe5\x9e\x8b\xe5\x8f\xb7: %s", info->model[0] ? info->model : "?");
        gp_batt_str(info->battery, btmp, sizeof(btmp));
        snprintf(lines[n++], 40, "\xe7\x94\xb5\xe9\x87\x8f: %s", btmp);
        snprintf(lines[n++], 40, "VID: 0x%02X%02X", info->vid >> 8, info->vid & 0xFF);
        snprintf(lines[n++], 40, "PID: 0x%02X%02X", info->pid >> 8, info->pid & 0xFF);
    } else {
        snprintf(lines[n++], 40, "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xbe\xe5\xa4\x87");
    }
    int y0 = (UI_SCREEN_H - 140) / 2;
    draw_dialog_frame(lcd, 30, y0, UI_SCREEN_W - 60, 140, 3);
    draw_text_centered(lcd, y0 + 6, "\xe8\xae\xbe\xe5\xa4\x87\xe4\xbf\xa1\xe6\x81\xaf", false);
    for (int i = 0; i < n; i++)
        draw_text(lcd, 48, y0 + 30 + i * 20, lines[i], false);
    return true;
}

/* ---- 连接记录弹窗 (历史列表) ---- */
static void hist_dlg_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;
    if (result < bt_manager_get_history_count()) {
        const bt_device_t *dev = bt_manager_get_history_at(result);
        if (dev) bt_manager_connect_device(dev);
        os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5..."); /* 正在连接... */
    }
}

static void open_hist_dialog(ui_ctx_t *ctx) {
    int cnt = bt_manager_get_history_count();
    if (cnt <= 0) { os_dialog_toast(ctx, "\xe6\x97\xa0\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95"); return; }
    static char items[4][64];
    static const char *ptrs[4];
    for (int i = 0; i < cnt && i < 4; i++) {
        const bt_device_t *dev = bt_manager_get_history_at(i);
        snprintf(items[i], 64, "%s", dev && dev->name[0] ? dev->name : "??");
        ptrs[i] = items[i];
    }
    os_dialog_list(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95", ptrs, cnt, 0, hist_dlg_select, NULL);
}

/* ---- 主菜单选中回调 ---- */
static void on_gamepad_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0: {  /* 添加设备: 开始扫描 */
        if (!bt_manager_is_stack_ready()) {
            os_dialog_toast(ctx, "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe4\xb8\xad"); /* 初始化中 */
            break;
        }
        s_scan_n = 0; s_scan_active = true;
        bt_manager_start_scan_continuous(gp_on_scan);
        os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..."); /* 正在扫描... */
        break;
    }
    case 1:  /* 按键映射: 占位 (完整映射流程后续) */
        os_dialog_toast(ctx, "\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* 开发中 */
        break;
    case 2:  /* 连接记录 */
        open_hist_dialog(ctx);
        break;
    case 3: {  /* 设备信息 */
        os_dlg_stack_t dlg;
        memset(&dlg, 0, sizeof(dlg));
        snprintf(dlg.title, sizeof(dlg.title), "\xe8\xae\xbe\xe5\xa4\x87\xe4\xbf\xa1\xe6\x81\xaf");
        dlg.count = 0;
        dlg.on_render = btinfo_dlg_render;
        os_dialog_push(ctx, &dlg);
        break;
    }
    case 4: {  /* WiFi 手柄切换 */
        if (web_gamepad_is_running()) {
            web_gamepad_stop();
            bt_manager_enable();
            os_dialog_toast(ctx, "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad");
        } else {
            bt_manager_disable();   /* 释放内部 DMA 给 WiFi softAP */
            if (web_gamepad_start() == ESP_OK)
                os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf WiFi \xe6\x89\x8b\xe6\x9f\x84");
            else {
                bt_manager_enable();
                os_dialog_toast(ctx, "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5");
            }
        }
        break;
    }
    case 5:  /* 返回 */
        os_pop(ctx);
        break;
    default: break;
    }
}

static void p_gamepad_enter(ui_ctx_t *ctx) {
    (void)ctx;
    os_dialog_list(ctx, "\xe6\x89\x8b\xe6\x9f\x84\xe9\x85\x8d\xe7\xbd\xae", s_gp_items, 6, 0,
                   on_gamepad_select, NULL);
}

static const os_module_t s_mod_gamepad = {
    .name       = "gamepad",
    .page_id    = OS_PAGE_GAMEPAD,
    .on_enter   = p_gamepad_enter,
    .fullscreen = false,
};

void os_page_gamepad_register(void) { os_register(&s_mod_gamepad); }
