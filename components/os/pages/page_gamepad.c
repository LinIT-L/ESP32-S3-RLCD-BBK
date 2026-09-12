/**
 * page_gamepad.c — 手柄配置 页面模块 (P3 迁移: 从 menu_system.c 的 gamepad 弹窗体系迁移).
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 6 项 (添加设备/按键映射/连接记录/设备信息/WiFi手柄/返回).
 * 蓝牙能力来自 bt_ctl 组件 (bt_manager_*), WiFi 手柄来自 web_gamepad.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "os_hw.h"
#include "ui_common.h"
#include "bt_manager.h"
#include "esp_attr.h"
#include "web_gamepad.h"
#include "wifi_manager.h"
#include "input.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* ---- WiFi 手柄模式: AP直连(自建热点) / WiFi共享(局域网), 选中即存并立即重启服务 ---- */
/* WiFi共享(局域网)模式: 需要设备先连上路由器(STA). 若未连接, 弹系统"连接WiFi"
 * 界面, 并自动重连已保存的网; 连接成功后由模块 poll 自动启动网页手柄服务. */
extern void settings_wifi_connect_open(ui_ctx_t *ctx);   /* 系统连接WiFi流程 (复用) */
static bool s_lan_pending = false;    /* 等待 WiFi 连接后再启动服务 */
static bool s_lan_saved_try = false;  /* 已自动尝试过保存配置重连 */

/* WiFi共享(局域网)生效: 启动服务 → 返回主桌面 → toast 提示 IP (1秒自动关, 任意键/触摸关) */
static void lan_start_service(ui_ctx_t *ctx) {
    esp_err_t e = web_gamepad_start();
    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));
    /* 先关掉连接流程弹窗, 返回主桌面 */
    os_dialog_clear_all(ctx);
    os_pop_to_main(ctx);
    if (e != ESP_OK) {
        os_dialog_toast(ctx, "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"); /* WiFi 手柄启动失败 */
        return;
    }
    if (ip[0]) {
        char msg[40];
        snprintf(msg, sizeof(msg), "IP:%s", ip);
        os_dialog_toast(ctx, msg);
    } else {
        os_dialog_toast(ctx, "WiFi \xe5\x85\xb1\xe4\xba\xab \xe5\xb7\xb2\xe7\x94\x9f\xe6\x95\x88"); /* WiFi 共享 已生效 */
    }
}
/* ---- WiFi 共享/AP 直连: 确认后才执行 (确认=开启, 取消=不动作) ---- */
static int s_wifi_confirm_act = -1;   /* 0=AP直连 1=开启WiFi共享 2=关闭WiFi共享 */

static void wifi_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    int act = s_wifi_confirm_act;
    s_wifi_confirm_act = -1;
    if (result != 0) return;   /* 取消 */
    if (act == 2) {            /* 关闭 WiFi 共享 */
        web_gamepad_stop();
        os_dialog_pop(ctx);    /* 先关掉确认框回手柄菜单 (toast 会置 handled 阻止自动关) */
        os_dialog_toast(ctx, "\xe5\x85\xb3\xe9\x97\xad WiFi \xe5\x85\xb1\xe4\xba\xab"); /* 关闭 WiFi 共享 */
        return;
    }
    web_gamepad_svc_mode_set(act);   /* act=0 AP / 1 共享 */
    /* 立即停止旧服务 */
    web_gamepad_stop();
    os_hw_release(OS_HW_BT);
    if (act == 0) {   /* AP 直连: 直接起热点 */
        os_dialog_pop(ctx);   /* 先关掉确认框 (toast 会置 handled 阻止自动关) */
        if (web_gamepad_start() == ESP_OK)
            os_dialog_toast(ctx, "AP \xe7\x9b\xb4\xe8\xbf\x9e \xe5\xb7\xb2\xe7\x94\x9f\xe6\x95\x88"); /* AP 直连 已生效 */
        else
            os_dialog_toast(ctx, "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"); /* WiFi 手柄启动失败 */
        return;
    }
    /* WiFi 共享: 需先连路由器 */
    if (wifi_manager_is_connected()) { lan_start_service(ctx); return; }
    /* 未连接: 弹系统连接WiFi界面; 若已保存配置则自动重连 */
    if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);
    s_lan_pending = true;
    s_lan_saved_try = false;
    settings_wifi_connect_open(ctx);
}

/* 小确认框: 标题在上, 左=开启/关闭 右=取消 */
static void wifi_confirm_open(ui_ctx_t *ctx, int act, const char *title, const char *btn) {
    s_wifi_confirm_act = act;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", title);
    dlg.count = 2;
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s", btn);                /* 开启/关闭 */
    snprintf(dlg.items[1], sizeof(dlg.items[1]), "%s", "\xe5\x8f\x96\xe6\xb6\x88"); /* 取消 */
    dlg.sel = 0;
    dlg.no_footer = true;
    dlg.small = true;
    dlg.cb = wifi_confirm_cb;
    os_dialog_push(ctx, &dlg);
}

static void wifi_mode_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0 || result > 1) return;   /* 只读 IP 行(≥2)忽略 */
    int lan = (result == 1) ? 1 : 0;
    /* 若 WiFi共享已在运行 (局域网/共享), 再选"共享"位置 = 关闭服务 */
    if (lan == 1 && web_gamepad_is_running() && web_gamepad_svc_mode() == 1) {
        wifi_confirm_open(ctx, 2,
            "\xe5\x85\xb3\xe9\x97\xad WiFi \xe5\x85\xb1\xe4\xba\xab\xe6\x89\x8b\xe6\x9f\x84", /* 关闭WiFi共享手柄 */
            "\xe5\x85\xb3\xe9\x97\xad");   /* 关闭 */
        return;
    }
    if (lan == 0) {
        wifi_confirm_open(ctx, 0,
            "\xe5\xbc\x80\xe5\x90\xaf AP \xe6\x89\x8b\xe6\x9f\x84", /* 开启AP手柄 */
            "\xe5\xbc\x80\xe5\x90\xaf");   /* 开启 */
        return;
    }
    wifi_confirm_open(ctx, 1,
        "\xe5\xbc\x80\xe5\x90\xaf WiFi \xe5\x85\xb1\xe4\xba\xab\xe6\x89\x8b\xe6\x9f\x84", /* 开启WiFi共享手柄 */
        "\xe5\xbc\x80\xe5\x90\xaf");       /* 开启 */
}
/* 每帧轮询: 等待 WiFi 连接成功后自动启动网页手柄 (局域网模式) */
static void p_gamepad_wifi_poll(ui_ctx_t *ctx) {
    if (!s_lan_pending) return;
    /* 仅等连接成功后再启动服务; 绝不在后台主动 enable(会重启 WiFi 驱动并与正在进行的
     * 扫描/连接冲突 → 崩溃). WiFi 是否/如何连接由用户选 WiFi共享 时弹出的系统连接流程负责. */
    if (wifi_manager_is_connected()) {
        s_lan_pending = false;
        lan_start_service(ctx);
    }
}
/* WiFi 手柄菜单: 动态显示 关闭WiFi共享/开启WiFi共享 (取决于当前是否运行共享模式);
 * 运行共享时"关闭WiFi共享"下方显示当前 IP (只读行) */
static void open_wifi_mode(ui_ctx_t *ctx) {
    static const char *it_off[3] = { "AP\xe7\x9b\xb4\xe8\xbf\x9e", "WiFi\xe5\x85\xb1\xe4\xba\xab", NULL };     /* AP直连 / WiFi共享 */
    bool running_share = web_gamepad_is_running() && web_gamepad_svc_mode() == 1;
    if (running_share) {
        char it_on[3][32];
        const char *ptrs[4];
        int n = 2;
        snprintf(it_on[0], sizeof(it_on[0]), "%s", "AP\xe7\x9b\xb4\xe8\xbf\x9e");
        snprintf(it_on[1], sizeof(it_on[1]), "%s", "\xe5\x85\xb3\xe9\x97\xad WiFi \xe5\x85\xb1\xe4\xba\xab"); /* 关闭WiFi共享 */
        char ip[16] = "";
        if (wifi_manager_get_ip(ip, sizeof(ip)) && ip[0]) {   /* IP 显示在关闭项下面 */
            snprintf(it_on[2], sizeof(it_on[2]), "IP:%s", ip);
            n = 3;
        }
        ptrs[0] = it_on[0]; ptrs[1] = it_on[1];
        if (n == 3) ptrs[2] = it_on[2];
        ptrs[n] = NULL;
        os_dialog_list(ctx, "", ptrs, n, 1, wifi_mode_cb, NULL);
        return;
    }
    os_dialog_list(ctx, "", it_off, 2, web_gamepad_svc_mode() ? 1 : 0, wifi_mode_cb, NULL);
}

/* 主菜单项动态构建: 未连接设备时隐藏"设备信息", 保持底部"返回"为末项.
 * s_gp_map[dst]=src 把显示位置映射回原始 switch case, 保证选中逻辑不错位. */
static const char *s_gp_items_dyn[6];
static int s_gp_map[6];
/* 扫描结果列表 */
#define GP_SCAN_MAX 16
static bt_device_t s_scan_devs[GP_SCAN_MAX];
static int  s_scan_n = 0;
static bool s_scan_active = false;
/* V1.5.x: 长按确认键扫描窗口 — 期望自动回连上次手柄 (连上即 0.5s"回连成功" + 关全部弹窗) */
static bool s_auto_rc_pending = false;

/* ---- 连接中弹窗 (处理 成功/失败/对方断开/超时, 避免"正在连接"永久卡住) ---- */
static int64_t s_conn_start = 0;      /* connect 发起时刻 (µs), 用于超时判断 */
static bool  s_conn_ok = false;       /* 已连上, 等待 1s "连接成功" 提示后进映射 */
static int64_t s_conn_ok_t = 0;
static bt_device_t s_conn_dev;        /* 待连接设备缓存 (供后台连接任务读取) */

static void start_key_mapping(ui_ctx_t *ctx);   /* 前向声明: 连接成功后直接衔接按键映射 */

/* 后台连接任务: esp_hidh_dev_open 同步阻塞约 2s, 放独立任务以免卡住 UI/延迟弹窗 */
static void bt_connect_task(void *arg) {
    (void)arg;
    bt_manager_connect_device(&s_conn_dev);
    vTaskDelete(NULL);
}

static bool conn_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_BACK) {
        /* 连接中返回 = 放弃等待: 关闭轮询与弹窗, 后台连接由 bt_manager 状态自行收敛 */
        d->on_poll = NULL;
        os_dialog_pop(ctx);
        return true;
    }
    return false;
}
static void conn_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    if (bt_manager_is_connected()) {
        int64_t now = esp_timer_get_time();
        /* "正在连接"至少显示 600ms 再切"连接成功", 保证点击后有明确反馈 */
        if (!s_conn_ok && (now - s_conn_start) < 600000LL) {
            snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5"); /* 正在连接... */
            ctx->needs_redraw = true;
            return;
        }
        /* 已连接: 先提示"连接成功"约 1s 再收起 */
        if (!s_conn_ok) {
            s_conn_ok = true;
            s_conn_ok_t = esp_timer_get_time();
            d->count = 1; d->sel = 0;
            snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x81"); /* 连接成功 */
            ctx->needs_redraw = true;
            return;
        }
        if ((now - s_conn_ok_t) >= 1000000LL) {
            s_conn_ok = false;
            d->on_poll = NULL;
            os_dialog_pop(ctx);   /* "连接成功！" 显示 1s 后收起, 衔接按键映射 */
            start_key_mapping(ctx);
        } else {
            ctx->needs_redraw = true;
        }
        return;
    }
    if (!bt_manager_is_connecting()) {
        /* 连接已结束但未成功(失败/连接后被对方短连断开) → 明确提示, 不留死锁 */
        d->on_poll = NULL;
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5/\xe5\xb7\xb2\xe6\x96\xad\xe5\xbc\x80"); /* 连接失败/已断开 */
        return;
    }
    if ((esp_timer_get_time() - s_conn_start) > 8000000LL) {
        /* 8s 仍未通 → 强行中止本轮, 提示超时, 不再无限等 */
        bt_manager_stop_scan();
        d->on_poll = NULL;
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6"); /* 连接超时 */
        return;
    }
    /* 仍在连接: 刷新显示并保持"正在连接" */
    snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5"); /* 正在连接... */
    ctx->needs_redraw = true;
}

/* 连接状态小窗渲染: 白底+3px 黑边框+自适应宽, 居中画 items[0] 文本 */
static bool conn_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int tw = text_width(d->items[0]);
    int w = 3 * 2 + 3 * 2 + tw;
    const int h = 36;
    int x0 = (UI_SCREEN_W - w) / 2, y0 = (UI_SCREEN_H - h) / 2;
    fill_rect(lcd, x0, y0, x0 + w - 1, y0 + h - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 3; k++) {
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + w - 1 - k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
    }
    draw_text_centered(lcd, y0 + 6, d->items[0], false);
    return true;
}

/* 发起连接并把当前顶层弹窗原地变成"正在连接"小提示窗 (扫描/历史记录共用) */
static void enter_connect_dlg(ui_ctx_t *ctx, const bt_device_t *dev) {
    if (dev) memcpy(&s_conn_dev, dev, sizeof(s_conn_dev));
    s_conn_start = esp_timer_get_time();
    s_conn_ok = false;   /* 重置"连接成功 1s"状态 */
    /* 连接前清理: 删同名历史记录 + 重置该设备配置为默认 + 清输入残留(防幽灵输入) */
    bt_manager_reset_pad_clean(&s_conn_dev);
    /* 原地把当前列表弹窗(扫描/历史)替换为"正在连接"小窗: 列表随之消失, 不留后台双层 */
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (d) {
        snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5"); /* 正在连接... */
        d->count = 1; d->sel = 0;
        d->on_render = conn_render;
        d->on_poll = conn_dlg_poll;
        d->on_key  = conn_dlg_key;
        ctx->needs_redraw = true;
    } else {   /* 无弹窗时新建 */
        os_dlg_stack_t nd;
        memset(&nd, 0, sizeof(nd));
        snprintf(nd.items[0], sizeof(nd.items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5"); /* 正在连接... */
        nd.count = 1; nd.sel = 0;
        nd.on_render = conn_render;
        nd.on_poll = conn_dlg_poll;
        nd.on_key  = conn_dlg_key;
        os_dialog_push(ctx, &nd);
    }
    /* 立即弹出"正在连接"后, 在后台连接任务里发起同步连接(不阻塞 UI), 保证点击秒反馈 */
    xTaskCreate(bt_connect_task, "bt_conn", 4096, NULL, 10, NULL);
}

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

/* ---- 设备信息: 列表弹窗只读回调 (项无操作, BACK 返回) ---- */
static void btinfo_noop(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)result; (void)ud;
}

/* ---- 连接记录弹窗 (历史列表) ---- */
static void hist_dlg_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;
    if (result < bt_manager_get_history_count()) {
        const bt_device_t *dev = bt_manager_get_history_at(result);
        if (dev) enter_connect_dlg(ctx, dev);
    }
}

static void open_hist_dialog(ui_ctx_t *ctx) {
    int cnt = bt_manager_get_history_count();
    if (cnt <= 0) { os_dialog_toast(ctx, "\xe6\x97\xa0\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95"); return; }
    EXT_RAM_BSS_ATTR static char items[4][64];
    static const char *ptrs[4];
    for (int i = 0; i < cnt && i < 4; i++) {
        const bt_device_t *dev = bt_manager_get_history_at(i);
        snprintf(items[i], 64, "%s", dev && dev->name[0] ? dev->name : "??");
        ptrs[i] = items[i];
    }
    os_dialog_list(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95", ptrs, cnt, 0, hist_dlg_select, NULL);
}

/* ---- 蓝牙扫描列表弹窗 (3.3: 添加设备 → 扫描结果显示在列表, 确认连接) ---- */
static void scan_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)ud;
    if (result < 0) {
        s_auto_rc_pending = false;   /* 关闭: 放弃自动回连期望 */
        bt_manager_stop_scan();      /* 关闭: 停止扫描 */
    }
}
static bool scan_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_BACK) {
        /* 扫描列表中按返回 = 立即退出并停止扫描, 不卡住 */
        s_auto_rc_pending = false;
        bt_manager_stop_scan();
        d->on_poll = NULL;
        s_scan_n = 0;
        os_dialog_pop(ctx);
        return true;
    }
    if (a == OS_ACTION_CONFIRM) {
        if (s_scan_n > 0 && d->sel >= 0 && d->sel < s_scan_n) {
            bt_manager_stop_scan();
            enter_connect_dlg(ctx, &s_scan_devs[d->sel]);
        } else if (!bt_manager_is_scanning()) {
            /* 无设备时确认 → 重新开始搜索 */
            bt_manager_stop_scan();
            s_scan_n = 0;
            bt_manager_start_scan_continuous(gp_on_scan);
            snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..."); /* 正在扫描... */
            d->count = 1; d->sel = 0;
            ctx->needs_redraw = true;
        }
        return true;   /* 扫描中/无设备: 忽略确认或已重扫 */
    }
    return false;   /* UP/DOWN 走默认 */
}
static void scan_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    /* V1.5.x: 长按确认键扫描窗口 — 自动回连成功 → 关闭所有弹窗 + 0.5s"回连成功"提示 */
    if (s_auto_rc_pending && bt_manager_is_connected()) {
        s_auto_rc_pending = false;
        bt_manager_stop_scan();
        os_dialog_clear_all(ctx);   /* 关闭全部弹窗 (扫描列表等), 回到进入前页面 */
        os_dialog_toast_ms(ctx, "\xe5\x9b\x9e\xe8\xbf\x9e\xe6\x88\x90\xe5\x8a\x9f", 500); /* 回连成功 */
        return;
    }
    static bool s_prev_scanning = false;
    bool scanning = bt_manager_is_scanning();
    /* 结果数变化 或 扫描状态变化 → 刷新 */
    if (s_scan_n != d->count || scanning != s_prev_scanning) {
        if (s_scan_n > 0) {
            d->count = s_scan_n;
            for (int i = 0; i < s_scan_n; i++) {
                const char *nm = bt_manager_get_device_name(&s_scan_devs[i]);
                snprintf(d->items[i], sizeof(d->items[i]), "%s", nm[0] ? nm : "??");
            }
        } else {
            /* 扫描未结束时恒显示"正在扫描", 结束且无结果才提示"未发现设备" */
            d->count = 1;
            snprintf(d->items[0], sizeof(d->items[0]), "%s",
                     scanning ? "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..." /* 正在扫描... */
                              : "\xe6\x9c\xaa\xe5\x8f\x91\xe7\x8e\xb0\xe8\xae\xbe\xe5\xa4\x87"); /* 未发现设备 */
        }
        if (d->sel >= d->count) d->sel = d->count - 1;
        s_prev_scanning = scanning;
        ctx->needs_redraw = true;
    }
}
static void open_scan_dialog(ui_ctx_t *ctx) {
    s_scan_n = 0;
    os_hw_request(OS_HW_BT);   /* V1.5.x 蓝牙按需开启: 进手柄页才首次启用再扫 (走 os_hw 引用计数) */
    bt_manager_start_scan_continuous(gp_on_scan);
    /* V1.5.x: 长按确认键扫描窗口 — 有上次连接设备且当前未连接 → 扫描过程中自动回连,
     * 连上即 0.5s"回连成功"提示并关闭所有弹窗 (由 scan_dlg_poll 每帧检测收敛) */
    s_auto_rc_pending = bt_manager_has_last_conn() && !bt_manager_is_connected();
    bt_manager_poll_auto_reconnect();
    s_scan_active = true;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..."); /* 正在扫描... */
    dlg.count = 1;
    dlg.sel = 0;
    dlg.on_poll = scan_dlg_poll;
    dlg.on_key = scan_dlg_key;
    dlg.cb = scan_dlg_cb;
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(TAG, "蓝牙扫描开始");
}
/* 全局: 长按硬件确认键 2s → 从任意页面直接进入蓝牙搜索弹窗 */
void os_page_gamepad_open_scan(ui_ctx_t *ctx) { open_scan_dialog(ctx); }

/* ---- 按键映射 (3.3 原版: 遍历 10 功能, 提示弹窗"请按下 X 按键", 捕获即自动进入下一个) ---- */
static int  s_map_func = 0;      /* 当前映射功能索引 (0..FUNC_MAX-1) */
static bool s_map_active = false;
static phys_t s_capture_phys = -1;   /* 已捕获的物理键 (松开确认用) */
static bool s_capture_armed = false; /* 是否已真实捕获到一个按键 (bool 门控, 规避 enum>=0 恒真 UB) */
static int64_t s_map_last_progress = 0;  /* 上次映射推进时刻, 超时未动自动取消 */
static int64_t s_map_ignore_until = 0;  /* 屏蔽"进入映射界面"那次确认的时间点, 防其被误映射成第一个键 */

/* 映射提示弹窗渲染 (3.3 notice 样式: 白底 + 3px 黑边框 + 自适应宽) */
static bool map_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    char txt[48];
    if (s_map_func >= 0 && s_map_func < FUNC_MAX)
        snprintf(txt, sizeof(txt), "\xe8\xaf\xb7\xe6\x8c\x89\xe4\xb8\x8b %s \xe6\x8c\x89\xe9\x94\xae",  /* 请按下 X 按键 */
                 bt_manager_func_name((func_t)s_map_func));
    else
        snprintf(txt, sizeof(txt), "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84\xe5\xae\x8c\xe6\x88\x90"); /* 按键映射完成 */
    int tw = text_width(txt);
    int w = 3 * 2 + 3 * 2 + tw;
    const int h = 36;
    int x0 = (UI_SCREEN_W - w) / 2, y0 = (UI_SCREEN_H - h) / 2;
    fill_rect(lcd, x0, y0, x0 + w - 1, y0 + h - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 3; k++) {
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + w - 1 - k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
    }
    draw_text_centered(lcd, y0 + 6, txt, false);
    return true;
}

/* 每帧轮询: 捕获手柄按键, 推进映射 (V1.0.41 起捕获即自动进入下一个) */
static void map_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    if (!s_map_active) return;
    if (s_map_func < 0 || s_map_func >= FUNC_MAX) return;
    /* 屏蔽"进入映射界面"那次确认键: 短时间(400ms)内不捕获任何按键, 防误映射 */
    if (esp_timer_get_time() < s_map_ignore_until) return;

    /* 映射无操作 40s 超时: 自动取消, 恢复手柄导航, 避免卡死在映射界面 */
    if ((esp_timer_get_time() - s_map_last_progress) > 40000000LL) {
        s_map_active = false;
        s_capture_armed = false;
        s_capture_phys = -1;
        input_set_gamepad_nav_enabled(true);
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe6\x98\xa0\xe5\xb0\x84\xe8\xb6\x85\xe6\x97\xb6\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88"); /* 映射超时已取消 */
        return;
    }

    /* 已捕获一个真实按键: 等它真正松开才确认并推进 (防按一次因抖动连配多个) */
    if (s_capture_armed) {
        if (!bt_manager_is_phys_pressed(s_capture_phys)) {
            s_capture_armed = false;      /* 已松开, 本次映射确认完成 */
            s_capture_phys = -1;
            s_map_last_progress = esp_timer_get_time();
            s_map_func++;
            if (s_map_func >= FUNC_MAX) {
                bt_manager_save_key_map();
                ESP_LOGI(TAG, "按键映射完成, 已保存到 NVS");
                s_map_active = false;
                input_set_gamepad_nav_enabled(true);
                os_dialog_pop(ctx);   /* 关映射弹窗回手柄主菜单 */
                os_dialog_toast(ctx, "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84\xe5\xae\x8c\xe6\x88\x90"); /* 按键映射完成 */
                return;
            }
            bt_manager_poll_new_press_reset();
            ctx->needs_redraw = true;
        }
        return;   /* 仍按住: 等待松开 */
    }

    /* 尚未捕获: 尝试捕获当前需映射功能的按键 */
    int fc = s_map_func;
    int r = bt_manager_poll_new_press((func_t)fc);
    if (r > 0) {
        s_capture_phys = bt_manager_get_key_map((func_t)fc);   /* 记住这次的键, 等松开再推进 */
        s_capture_armed = true;                                /* 只有真实捕获到按键才允许随后推进 */
        s_map_last_progress = esp_timer_get_time();
        ESP_LOGI(TAG, "按键映射捕获: [%s] → %s (等松开确认)",
                 bt_manager_func_name((func_t)fc),
                 bt_manager_phys_name(s_capture_phys));
        ctx->needs_redraw = true;
    }
}

/* 映射弹窗按键: 映射期间**一切按键一律忽略**, 纯由 bt_manager_poll_new_press 硬捕获
 * 当前功能对应的键 (仅真实按下的物理键泵入映射, 不会产生任何系统动作).
 * 特别地, 返回键/退出键在此期间不作为"取消/退出"动作 —— 用户"硬设"的是按键本身,
 * 其功能作用在映射阶段不生效, 避免误终止映射. */
static bool map_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ctx; (void)d; (void)a; (void)ud;
    return true;   /* 其他键在映射期间一律忽略 */
}

static void start_key_mapping(ui_ctx_t *ctx) {
    if (!bt_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe5\x85\x88\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x89\x8b\xe6\x9f\x84"); /* 请先连接手柄 */
        return;
    }
    s_map_func = 0;
    s_map_active = true;
    s_capture_phys = -1;
    s_capture_armed = false;
    s_map_last_progress = esp_timer_get_time();
    s_map_ignore_until = esp_timer_get_time() + 400000LL;   /* 400ms 内屏蔽, 吞掉"进入页面"的那次确认 */
    /* 清输入残留状态: 防止旧手柄/幽灵状态让"不按任何键映射就自动完成" */
    bt_manager_reset_input_state();
    bt_manager_poll_new_press_reset();
    input_set_gamepad_nav_enabled(false);   /* 禁用手柄导航, 防干扰映射 */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.on_render = map_render;
    dlg.on_key = map_key;
    dlg.on_poll = map_poll;
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(TAG, "按键映射启动: 请按下 [%s] 对应按键", bt_manager_func_name(F_UP));
}

/* ---- 主菜单选中回调 ---- */
static void on_gamepad_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) { os_pop(ctx); return; }   /* 返回 → 退出手柄页 */
    int raw = (result >= 0 && result < 6) ? s_gp_map[result] : result;  /* 显示位置 -> 原始case */
    switch (raw) {
    case 0: {  /* 添加设备: 扫描蓝牙列表弹窗 (3.3), 确认连接 */
        if (!bt_manager_is_stack_ready()) {
            os_dialog_toast(ctx, "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe4\xb8\xad"); /* 初始化中 */
            break;
        }
        open_scan_dialog(ctx);
        break;
    }
    case 1:  /* 按键映射 (3.3 原版流程) */
        start_key_mapping(ctx);
        break;
    case 2:  /* 连接记录 */
        open_hist_dialog(ctx);
        break;
    case 3: {  /* 设备信息: 列表弹窗 (只读) */
        char lines[8][40];
        int n = 0;
        const btm_conn_info_t *info = bt_manager_get_conn_info();
        if (info && info->available && bt_manager_is_connected()) {
            char btmp[16];
            snprintf(lines[n++], 40, "\xe5\x90\x8d\xe7\xa7\xb0: %s", info->name[0] ? info->name : "?");   /* 名称 */
            snprintf(lines[n++], 40, "\xe5\x9e\x8b\xe5\x8f\xb7: %s", info->model[0] ? info->model : "?");  /* 型号 */
            gp_batt_str(info->battery, btmp, sizeof(btmp));
            snprintf(lines[n++], 40, "\xe7\x94\xb5\xe9\x87\x8f: %s", btmp);  /* 电量 */
            snprintf(lines[n++], 40, "VID: 0x%02X%02X", info->vid >> 8, info->vid & 0xFF);
            snprintf(lines[n++], 40, "PID: 0x%02X%02X", info->pid >> 8, info->pid & 0xFF);
        } else {
            snprintf(lines[n++], 40, "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xbe\xe5\xa4\x87");  /* 未连接设备 */
        }
        EXT_RAM_BSS_ATTR static char items[8][40];
        static const char *ptrs[8];
        for (int i = 0; i < n; i++) {
            snprintf(items[i], sizeof(items[i]), "%s", lines[i]);
            ptrs[i] = items[i];
        }
        os_dialog_list(ctx, "", ptrs, n, -1, btinfo_noop, NULL);
        break;
    }
    case 4: open_wifi_mode(ctx); break;   /* WiFi 手柄: 选 AP直连 / WiFi共享 */
    case 5:  /* 返回 (底部固定返回行一般走 result<0; 此项兜底) */
        os_pop(ctx);
        break;
    default: break;
    }
}

static void p_gamepad_enter(ui_ctx_t *ctx) {
    (void)ctx;
    bool conn = bt_manager_is_connected();
    int count = 0;
    for (int src = 0; src < 6; src++) {
        if (src == 3 && !conn) continue;   /* 未连接设备: 隐藏"设备信息" */
        s_gp_items_dyn[count] = s_gp_items[src];
        s_gp_map[count] = src;
        count++;
    }
    os_dialog_list(ctx, "\xe6\x89\x8b\xe6\x9f\x84\xe9\x85\x8d\xe7\xbd\xae", s_gp_items_dyn, count, 0,
                   on_gamepad_select, NULL);
}

static const os_module_t s_mod_gamepad = {
    .name       = "gamepad",
    .page_id    = OS_PAGE_GAMEPAD,
    .on_enter   = p_gamepad_enter,
    .fullscreen = false,
};

/* 网页手柄等待 WiFi 连接: 用全局 service 而非模块 poll, 保证在任意页面
 * (尤其是"连接WiFi"/输密码页) 期间都能检测到连接成功并自动启动服务. */
static const os_service_t s_svc_webpad_wifi = {
    .name = "webpad_wifi",
    .tick = p_gamepad_wifi_poll,
};

void os_page_gamepad_register(void) { os_register(&s_mod_gamepad); os_register_service(&s_svc_webpad_wifi); }
