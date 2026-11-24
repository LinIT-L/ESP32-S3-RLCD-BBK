/**
 * page_gamepad.c — 手柄配置 页面模块 (P3 迁移: 从 menu_system.c 的 gamepad 弹窗体系迁移).
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 4 项 (添加设备/按键映射/连接记录/WiFi手柄) + 底部"返回".
 * ★ 2026-11-23 linit: "设备信息"与"忘记此手柄"从主菜单**下移进"连接记录"** ——
 *   两者本来就是"针对某一只已配对/已连接的手柄"的操作, 放在主菜单既要求"必须先连着"
 *   (未连接时这两项还得动态隐藏), 又和"记录"这个上下文割裂。现在:
 *     · 连接记录列表 = 每只历史手柄一行 + 末尾"设备信息"一行;
 *     · 短按历史行 = 连接它; 长按历史行 1 秒 = 弹"忘记 X ?"确认;
 *     · 点"设备信息" = 只读详情窗(叠在记录列表之上)。
 *   主菜单不再需要按连接状态动态隐藏项, 结构恒定.
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

/* 主菜单项 (UTF-8). 顺序即 case 号, 见 on_gamepad_select */
static const char *const s_gp_items[6] = {
    "\xe6\xb7\xbb\xe5\x8a\xa0\xe8\xae\xbe\xe5\xa4\x87",  /* 0 添加设备 */
    "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84",  /* 1 按键映射 */
    "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95",  /* 2 连接记录 (含设备信息/忘记此手柄) */
    "WiFi \xe6\x89\x8b\xe6\x9f\x84",                      /* 3 WiFi 手柄 */
    "\xe5\xbc\x80\xe6\x9c\xba\xe8\x87\xaa\xe5\x8a\xa8\xe5\x9b\x9e\xe8\xbf\x9e", /* 4 开机自动回连 (文字在 p_gamepad_menu 里带开关状态替换) */
    "\xe8\xbf\x94\xe5\x9b\x9e",                            /* 5 返回 */
};
/* 第 4 项的动态文字: "开机自动回连: 开/关" (跟着开关状态变) */
static char s_gp_arc_item[32];

/* ---- WiFi 手柄模式: AP直连(自建热点) / WiFi共享(局域网), 选中即存并立即重启服务 ---- */
/* WiFi共享(局域网)模式: 需要设备先连上路由器(STA). 若未连接, 弹系统"连接WiFi"
 * 界面, 并自动重连已保存的网; 连接成功后由模块 poll 自动启动网页手柄服务. */
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
    /* 未连接: 弹系统连接WiFi界面; 已保存配置则自动重连.
     * ★ 2026-10-02 修: 不要在此外 request —— settings_wifi_connect_open() 自己会按需
     *   request+记标志, 这里先 request 会让它不记标志 → 取消后 WiFi 常开。 */
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

/* 主菜单项构建. ★ 2026-11-23: 主菜单不再随连接状态增减项 —— "设备信息/忘记此手柄"已下移进
 * 连接记录, 所以这里没有隐藏逻辑, 数组恒等于 s_gp_items. s_gp_map 保留(显示位置→case),
 * 以免以后再加动态项时又要改选中逻辑. */
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
#define CONN_TIMEOUT_MS 25000         /* 连接总预算: 底层 esp_hidh_dev_open 自己就要 ~30s,
                                       * 原来给 8s 会把它掐死 → "第一次超时, 第二次自动回连" */
static int64_t s_conn_start = 0;      /* connect 发起时刻 (µs), 用于超时判断 */
static bool  s_conn_ok = false;       /* 已连上, 等待 1s "连接成功" 提示后进映射 */
static int64_t s_conn_ok_t = 0;
/* ★ 2026-10-02 竞态修复: 是否已**观察到**"连接中"。
 *   本弹窗的第一帧 poll 可能早于连接任务把 s_connecting 置真(见 enter_connect_dlg:
 *   先换弹窗 + 立刻 xTaskCreate) → 原逻辑会当场判"连接失败", 而后台其实还在连,
 *   过一会儿真连上 = 用户报的"连经常失败, 但实际上又连接成功"。 */
static bool  s_saw_connecting = false;
static bt_device_t s_conn_dev;        /* 待连接设备缓存 (供后台连接任务读取) */

static void start_key_mapping(ui_ctx_t *ctx);   /* 前向声明: 连接成功后直接衔接按键映射 */
static void p_gamepad_menu(ui_ctx_t *ctx);      /* 前向声明: 映射结束后回到菜单 */

/* 后台连接任务: esp_hidh_dev_open 同步阻塞约 2s, 放独立任务以免卡住 UI/延迟弹窗 */
/* ⚠ 先等 300ms: 点选连接前刚 bt_manager_stop_scan(), BLE 控制器还在退出扫描态,
 *   立刻 esp_hidh_dev_open 会和"扫描收尾"抢控制器 → 连接被拖慢甚至失败。
 *   (这一步是"第一次连接超时"的另一半原因。) */
static void bt_connect_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    bt_manager_connect_device(&s_conn_dev);
    vTaskDelete(NULL);
}

static bool conn_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_BACK) {
        /* 连接中返回 = 放弃等待: 关闭轮询与弹窗, 后台连接由 bt_manager 状态自行收敛。
         * ★ 2026-11-24: 先明确告诉 bt_manager "UI 不等了" —— 否则底层事务稍后真连上,
         *   仍会按"用户主动连接"给这只手柄挂映射待办, 哪天再连别的手柄就把向导错弹给它。 */
        bt_manager_cancel_connect();
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
            os_dialog_pop(ctx);   /* "连接成功！" 显示 1s 后收起 */
            /* ★ 2026-10-02 修"同一只手柄被引导映射两次":
             *   连接时 bt_manager 会给"尚未映射过"的手柄挂一个**一次性待办**。
             *   原实现在这里无条件 start_key_mapping(), 却**没取走待办** →
             *   用户退回桌面时 page_main 的 C5 轮询把同一个(已过期)待办又取出来弹一次。
             *   现在: 只有该待办存在(= 这只手柄还没映射过)才衔接向导, 并**顺手取走**。
             *   已映射过的手柄重连 → 只提示"连接成功", 不再强拉进向导(要重映射可从菜单进)。 */
            char pnm[33];
            if (bt_manager_take_map_pending(pnm, sizeof pnm)) {
                ESP_LOGD(TAG, "[EV] 新手柄 '%s' 连接成功 -> 衔接按键映射", pnm);
                start_key_mapping(ctx);
            } else {
                ESP_LOGI(TAG, "[EV] 手柄已映射过, 连接成功不打扰(不弹映射向导)");
            }
        } else {
            ctx->needs_redraw = true;
        }
        return;
    }
    if (!bt_manager_is_connecting()) {
        /* ★ 宽限期(1s): 还没观察到"连接中"之前不许判失败 ——
         *   连接任务可能只是还没被调度到(先弹窗后 xTaskCreate 的竞态)。
         *   没有这 1s, 就会出现"闪一下『连接失败/已断开』, 然后手柄其实连上了"。 */
        if (!s_saw_connecting && (esp_timer_get_time() - s_conn_start) < 1000000LL) {
            ctx->needs_redraw = true;
            return;
        }
        /* 连接已结束但未成功(失败/连接后被对方短连断开) → 明确提示, 不留死锁 */
        d->on_poll = NULL;
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5/\xe5\xb7\xb2\xe6\x96\xad\xe5\xbc\x80"); /* 连接失败/已断开 */
        return;
    }
    s_saw_connecting = true;   /* 确认真进入"连接中"状态, 之后才允许判失败 */
    /* ★★ 2026-10-03 修"第一次连不上、提示连接超时, 第二次自动回连成功":
     *   根因是**这里 8 秒就把连接掐了**, 而底层 esp_hidh_dev_open() 自己的预算是
     *   ~30s(设备不在范围/未广播时会一直等)。手动点选连接还叠加了两件事:
     *     ① 刚 bt_manager_stop_scan(), BLE 控制器还没退出扫描态就发起连接;
     *     ② 手柄上报的地址**每次会话会漂移** → 连接前会清掉旧绑定, 属于冷配对,
     *        比开机直连(已配对)慢得多。
     *   结果: 8s 被 cancel → 用户看到"连接超时"; 而底层其实快连上了/已留下绑定,
     *   于是"第二次就自动回连"。
     *   现在: 预算提到 25s, 中途给"按手柄键唤醒"的提示, 让用户知道在等它。 */
    int64_t elapsed = esp_timer_get_time() - s_conn_start;
    if (elapsed > (int64_t)CONN_TIMEOUT_MS * 1000LL) {
        /* ★ 2026-11-23 R1: 这里只**停止等待**, 不再"取消底层连接"。
         *   原来调 bt_manager_cancel_connect() 会发 esp_ble_gap_disconnect():
         *   连接建立中 L2CAP 侧没有 LCB, 该调用直接 return FALSE(取消无效),
         *   却把 s_connecting 清掉 ⇒ 回连任务接着发起第二个 open ⇒ Already Connected。
         *   现在 cancel 只是 UI 语义: 底层事务继续, 真连上了照样连上 (回调会通知页面),
         *   真失败由 open 返回值收敛。所以提示也改成"后台继续尝试"。 */
        bt_manager_stop_scan();
        bt_manager_cancel_connect();
        d->on_poll = NULL;
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6(\xe5\x90\x8e\xe5\x8f\xb0\xe7\xbb\xa7\xe7\xbb\xad)"); /* 连接超时(后台继续) */
        return;
    }
    /* 仍在连接: 刷新显示并保持"正在连接", 直到超时。
     * ★ 2026-10-27 linit: 删掉原来"按一下手柄键唤醒"那句提示 —— 用户不需要被教怎么按手柄。 */
    const char *txt = "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5"; /* 正在连接... */
    snprintf(d->items[0], sizeof(d->items[0]), "%s", txt);
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
    s_conn_ok = false;          /* 重置"连接成功 1s"状态 */
    s_saw_connecting = false;   /* 重置"已见到连接中"(见 conn_dlg_poll 的宽限说明) */
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
    ESP_LOGD(TAG, "[EV] 发起连接: name=%s addr=%02x:%02x:%02x:%02x:%02x:%02x type=%d",
             s_conn_dev.name, s_conn_dev.bd_addr[0], s_conn_dev.bd_addr[1], s_conn_dev.bd_addr[2],
             s_conn_dev.bd_addr[3], s_conn_dev.bd_addr[4], s_conn_dev.bd_addr[5],
             (int)s_conn_dev.addr_type);
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

/* ---- 设备信息: 只读详情窗 (从"连接记录"末项进入, 叠在记录列表之上) ---- */
static void btinfo_noop(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)result; (void)ud;
}

static void open_btinfo_dialog(ui_ctx_t *ctx) {
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
    static EXT_RAM_BSS_ATTR char items[8][40];
    static const char *ptrs[8];
    for (int i = 0; i < n; i++) {
        snprintf(items[i], sizeof(items[i]), "%.39s", lines[i]);
        ptrs[i] = items[i];
    }
    os_dialog_list(ctx, "", ptrs, n, -1, btinfo_noop, NULL);
}

/* ---- 连接记录弹窗: 每只历史手柄一行 + 末尾"设备信息"一行 ----
 * ★ 2026-11-23 linit: "忘记此手柄"与"设备信息"从主菜单下移到这里 ——
 *   短按历史行 = 连接; **长按名称 1 秒 = 弹"忘记 X ?"确认**; 点"设备信息" = 只读详情.
 *   用 row_long(只借长按命中, 不画 ×) + on_key 拦截, 见 os.h 该字段注释. */
#define HIST_IDX_BTINFO  (-2)      /* 内部标记: 设备信息行在列表里的位置 = s_hist_n */
static char s_forget_name[33];     /* 长按选中的手柄名 (确认框回调读它) */
static void on_forget_confirm(ui_ctx_t *ctx, int result, void *ud);   /* 前向: 定义见下方 */
static void open_hist_dialog(ui_ctx_t *ctx);                          /* 前向: 忘记后刷新用 */

/* 列表里"历史条数"(不含设备信息行) —— on_key 用它区分两类行 */
static int s_hist_n = 0;

static void hist_dlg_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;
    if (result < s_hist_n) {
        const bt_device_t *dev = bt_manager_get_history_at(result);
        if (dev) enter_connect_dlg(ctx, dev);
    }
    /* result == s_hist_n (设备信息) 在 on_key 里已消费, 不会走到这 */
}

static bool hist_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_LONG_PRESS) {
        int idx = d->sel;
        if (idx >= 0 && idx < s_hist_n) {
            const bt_device_t *dev = bt_manager_get_history_at(idx);
            if (dev && dev->name[0]) {
                snprintf(s_forget_name, sizeof(s_forget_name), "%s", dev->name);
                char msg[48], nm[17];
                snprintf(nm, sizeof(nm), "%.16s", dev->name);
                snprintf(msg, sizeof(msg), "\xe5\xbf\x98\xe8\xae\xb0 \"%s\" ?", nm); /* 忘记 "X" ? */
                os_dialog_confirm_ex(ctx, msg, 0, -1, on_forget_confirm, NULL);
                ESP_LOGI(TAG, "长按连接记录 → 确认忘记: %s", nm);
            }
        }
        return true;   /* 长按一律消费: 既不改选中语义, 也不会误触发连接 */
    }
    if (a == OS_ACTION_CONFIRM && d->sel == s_hist_n) {
        open_btinfo_dialog(ctx);   /* 设备信息行: 打开详情窗, 记录列表留在下面 */
        return true;               /* 消费 ⇒ 不 dlg_finish_top, 列表不关 */
    }
    return false;
}

static void open_hist_dialog(ui_ctx_t *ctx) {
    int cnt = bt_manager_get_history_count();
    if (cnt > BT_HISTORY_MAX) cnt = BT_HISTORY_MAX;   /* 与 bt_manager 上限一致, 防越界 */
    if (cnt < 0) cnt = 0;
    s_hist_n = cnt;

    /* ★ 2026-11-01 修"连接记录点了没反应、只能退出"：
     *   cnt<=0 时原来弹 toast —— toast 是叠在菜单之上的临时层，
     *   用户点它只会被 toast 消费掉，看起来就是"整页死机"。
     *   现在永远打开一个正常列表（无历史时只有"设备信息"一行），BACK 即可返回。 */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95"); /* 连接记录 */
    int n = 0;
    for (int i = 0; i < cnt; i++) {
        const bt_device_t *dev = bt_manager_get_history_at(i);
        /* ★ 2026-11-24: 显式截断到 29 字节。dev->name 是 char[33], 而列表项是 char[30];
         *   原来只靠 snprintf 的 size 兜底, 编译期报 -Wformat-truncation(可能截断)。
         *   用 "%.29s" 把"最多写 29 字符 + NUL"写成显式意图 —— 行为不变(仍是安全截断),
         *   但告警消失, 也避免以后有人把 items 改小时出错。 */
        snprintf(dlg.items[n], sizeof(dlg.items[0]), "%.*s",
                 (int)sizeof(dlg.items[0]) - 1,
                 (dev && dev->name[0]) ? dev->name : "??");
        n++;
    }
    /* 末项: 设备信息 (原主菜单项). 放在列表末尾 = 与"记录"同上下文, 又不会挤掉手柄名 */
    snprintf(dlg.items[n], sizeof(dlg.items[0]), "%s", "\xe8\xae\xbe\xe5\xa4\x87\xe4\xbf\xa1\xe6\x81\xaf"); /* 设备信息 */
    (void)HIST_IDX_BTINFO;
    n++;
    dlg.count  = n;
    dlg.sel    = 0;
    dlg.cb     = hist_dlg_select;
    dlg.on_key = hist_dlg_key;
    dlg.row_long = true;    /* 长按行 1s → OS_ACTION_LONG_PRESS (不画 ×) */
    os_dialog_push(ctx, &dlg);
}

/* ---- 蓝牙扫描列表弹窗 (3.3: 添加设备 → 扫描结果显示在列表, 确认连接) ---- */
/* ★★★ 2026-11-22 修复「从桌面长按确认键自动弹出'正在扫描'后, 确认/返回都按不动」★★★
 *
 * 【根因】原实现在**主循环任务**里同步调 `os_hw_request(OS_HW_BT)`。而
 *   `bt_manager_enable()` 内部是 `xSemaphoreTake(s_init_done_sem, 20000)` —— 同步等待
 *   "1~2 秒"(源码注释原话:"本函数只同步等待完成")。这 1~2 秒里 `app_main` 完全停摆,
 *   `input_get_action()` 一次都不跑 ⇒ 期间按下的键全部丢失 ⇒ 用户感受就是
 *   "屏幕上出现'正在扫描', 然后确认/返回都没反应"。
 *
 * 【为什么只有"自动出现"才坏(用户实测: 手动扫描返回没问题)】
 *   · 手动路径 = 手柄页 → 添加设备: 进手柄页时蓝牙栈已经拉起 ⇒ `enable` 走幂等快路径 0ms;
 *   · 自动路径 = 任意页面(通常是桌面)长按确认键 2s: 本板蓝牙**开机不初始化、按需启动**
 *     ⇒ 必须走完整的 1~2s 初始化, 而且是在主循环线程里干等。
 *   两条路径调的是同一个 open_scan_dialog(), 差别只在"栈起来没有"。
 *
 * 【修法】栈没就绪时**只发起、不等待** —— 交给独立任务去拉, 主循环立刻返回继续跑
 *   (按键全程可用); 拉起来后由 scan_dlg_poll 每帧发现并就绪开始扫描。
 *   ⚠ 任务栈必须在内部 RAM: bt 初始化期间会关 flash/PSRAM cache, PSRAM 栈会触发
 *     esp_task_stack_is_sane_cache_disabled() 断言(bt_manager_enable 注释里踩过)。 */
#define SCAN_BT_WAIT_TIMEOUT_MS 8000
static volatile bool s_scan_wait_bt  = false;   /* 正在后台等蓝牙栈就绪 */
static uint32_t      s_scan_wait_ms  = 0;       /* 开始等待的时刻 */

static void scan_bt_start_task(void *arg)
{
    (void)arg;
    os_hw_request(OS_HW_BT);     /* 内部阻塞 1~2s —— 但这里不是主循环线程, 不拖 UI */
    /* 不需要额外完成标志: scan_dlg_poll 直接看 bt_manager_is_stack_ready(),
     * 那是栈到底起来没有的唯一真相源; 失败自然落到 8s 超时分支。 */
    vTaskDelete(NULL);
}

/* 丢弃本次"等待蓝牙栈"状态 (用户提前退出 / 超时 / 栈已就绪) */
static void scan_wait_bt_clear(void)
{
    s_scan_wait_bt = false;
    s_scan_wait_ms = 0;
}

static void scan_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)ud;
    if (result < 0) {
        s_auto_rc_pending = false;   /* 关闭: 放弃自动回连期望 */
        scan_wait_bt_clear();        /* ★ 同时放弃"后台拉起蓝牙"的等待 */
        bt_manager_stop_scan();      /* 关闭: 停止扫描 */
    }
}
static bool scan_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_BACK) {
        /* 扫描列表中按返回 = 立即退出并停止扫描, 不卡住 */
        s_auto_rc_pending = false;
        scan_wait_bt_clear();          /* ★ 后台还在拉起蓝牙也要一并放弃等待 */
        bt_manager_stop_scan();
        d->on_poll = NULL;
        s_scan_n = 0;
        os_dialog_pop(ctx);
        return true;
    }
    if (a == OS_ACTION_CONFIRM) {
        if (s_scan_wait_bt) return true;   /* 栈还没起: 确认无意义, 别去重扫 */
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
    /* ★ 2026-11-22: 后台正在拉起蓝牙栈 → 每帧查一次, 就绪后才真正开始扫描。
     *   这一步把"1~2s 初始化"彻底挪出主循环, 期间确认/返回全程可用。
     *   超时兜底: 内部 RAM 不足时 bt_init 可能失败, 不能让框永远停在"正在启动蓝牙"。 */
    if (s_scan_wait_bt) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (bt_manager_is_stack_ready()) {
            scan_wait_bt_clear();
            s_auto_rc_pending = bt_manager_has_last_conn() && !bt_manager_is_connected();
            bt_manager_start_scan_continuous(gp_on_scan);
            bt_manager_poll_auto_reconnect();
            d->count = 1; d->sel = 0;
            snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..."); /* 正在扫描... */
            ctx->needs_redraw = true;
            ESP_LOGI(TAG, "蓝牙栈就绪 → 开始扫描");
        } else if ((uint32_t)(now - s_scan_wait_ms) > SCAN_BT_WAIT_TIMEOUT_MS) {
            scan_wait_bt_clear();
            d->count = 1; d->sel = 0;
            snprintf(d->items[0], sizeof(d->items[0]), "%s", "\xe8\x93\x9d\xe7\x89\x99\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"); /* 蓝牙启动失败 */
            ctx->needs_redraw = true;
            ESP_LOGE(TAG, "蓝牙栈 %d ms 未就绪 ⇒ 放弃本次扫描 (按返回可退出)",
                     SCAN_BT_WAIT_TIMEOUT_MS);
        }
        return;   /* 等待期间不做下面的扫描状态判断 */
    }
    /* V1.5.x: 长按确认键扫描窗口 — 自动回连成功 → 收掉扫描窗 + 0.5s"回连成功"提示
     * ★★★ 2026-11-24 linit「主菜单画面 + 触摸全无 + 退出键却弹退出框」根因修复 ★★★
     *   原来是 `os_dialog_clear_all(ctx)` —— 把弹窗栈**全部**清空, 包括"进入前的页面"所依赖的
     *   那层菜单。若此刻当前页是 OS_PAGE_GAMEPAD(本页没有 render/touch, 界面全在弹窗栈里),
     *   清完就落到"空壳页": 屏幕留着上一帧的主菜单像素、触摸无人接收(m->touch==NULL)、
     *   按键依旧走 exit_confirm ⇒ 看着像桌面却完全点不动。对照 :721 取消映射的写法 ——
     *   那里清完会 `p_gamepad_menu()` 重建菜单, 这条路径漏了。
     *   现在只关**当前这一层扫描窗**: os_dialog_pop 在栈空时会 os_pop 掉 DIALOG 页并回到父页,
     *   父页的 on_enter 自然恢复它的界面(手柄页 → p_gamepad_menu; 桌面/其它页 → 原样)。
     *   既不清别人的弹窗, 也不把页面留在空壳状态。 */
    if (s_auto_rc_pending && bt_manager_is_connected()) {
        s_auto_rc_pending = false;
        bt_manager_stop_scan();
        s_scan_n = 0;
        s_scan_active = false;
        d->on_poll = NULL;      /* 本层即将关闭: 防下一帧重入已关闭的层 */
        os_dialog_pop(ctx);
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
    bool ready = bt_manager_is_stack_ready();
    if (ready) {
        /* 栈已就绪: 原来的同步路径(0ms, 因为 enable 是幂等快路径) */
        scan_wait_bt_clear();
        bt_manager_start_scan_continuous(gp_on_scan);
        /* V1.5.x: 长按确认键扫描窗口 — 有上次连接设备且当前未连接 → 扫描过程中自动回连,
         * 连上即 0.5s"回连成功"提示并关闭所有弹窗 (由 scan_dlg_poll 每帧检测收敛) */
        s_auto_rc_pending = bt_manager_has_last_conn() && !bt_manager_is_connected();
        bt_manager_poll_auto_reconnect();
    } else {
        /* 栈没起: 后台拉起, 主循环不阻塞 (见上方注释) */
        s_scan_wait_bt = true;
        s_scan_wait_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_auto_rc_pending = false;   /* 等扫描真正开始时再决定是否自动回连 */
        if (xTaskCreate(scan_bt_start_task, "bt_scanf", 3072, NULL, 3, NULL) != pdPASS) {
            scan_wait_bt_clear();
            ESP_LOGE(TAG, "bt_scanf 任务创建失败(内部 RAM 不足) ⇒ 本次扫描放弃");
        } else {
            ESP_LOGW(TAG, "蓝牙栈未就绪: 已转后台拉起 (主循环不阻塞)");
        }
    }
    s_scan_active = true;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s",
             ready ? "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f..."          /* 正在扫描... */
                   : "\xe6\xad\xa3\xe5\x9c\xa8\xe5\x90\xaf\xe5\x8a\xa8\xe8\x93\x9d\xe7\x89\x99..."); /* 正在启动蓝牙... */
    dlg.count = 1;
    dlg.sel = 0;
    /* ★ 2026-11-21 linit: 恢复早期版本的标准大框。
     *   之前这里加了 rows16(16px 小字) + auto_h(高度自适应) ⇒ 弹窗又窄又矮、字还小,
     *   观感不是系统标准弹窗。两者都去掉, 回到 os_dialog_push 的默认尺寸/字号。 */
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
static int64_t s_map_hold_wait = 0;     /* ★ 等待"手里按着的键"松开的起始时刻 (0=没在等) */
static volatile bool s_map_cancel_req = false;  /* ★ 触摸"取消映射"请求 (由 map_touch 置位, map_poll 处理) */

/* (any_phys_pressed 已删除: 用它做"等按键松开"的门闸会把映射界面卡死 —— 见 map_poll 注释) */
/* ★ 2026-10-02 "每个按键只映射一次": 本轮已经捕获过的物理键(32 位位图, 覆盖 PHYS_MAX=24)。
 *   向导要依次映射 10 个功能, 但没有 Start/Select 的手柄凑不出 10 个键 →
 *   用户被迫把同一个键按给多个功能。记录已用过的键, 再遇到"用过的键"就判定
 *   没有更多键可指了, 直接保存结束向导。 */
static uint32_t s_map_used = 0;
static void finish_key_mapping(ui_ctx_t *ctx);   /* 前向声明 */

/* 映射提示弹窗渲染 (3.3 notice 样式: 白底 + 3px 黑边框 + 自适应宽) */
static bool map_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    char txt[48];
    if (s_map_func >= 0 && s_map_func < FUNC_MAX)
        snprintf(txt, sizeof(txt), "\xe8\xaf\xb7\xe6\x8c\x89\xe4\xb8\x8b %s \xe6\x8c\x89\xe9\x94\xae %d/%d",  /* 请按下 X 按键 n/10 */
                 bt_manager_func_name((func_t)s_map_func), s_map_func + 1, FUNC_MAX);
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
    /* ★ 触摸取消 (标志由 map_touch 置位) —— 在这里统一收拾, 不在触摸回调里拆栈 */
    if (s_map_cancel_req) {
        s_map_cancel_req = false;
        s_map_active = false;
        s_capture_armed = false;
        s_capture_phys = -1;
        s_map_hold_wait = 0;
        input_set_gamepad_nav_enabled(true);
        bt_manager_restore_key_map();   /* 回滚: 取消不等于"半套生效" */
        os_dialog_clear_all(ctx);
        if (ctx->current_page == OS_PAGE_GAMEPAD) p_gamepad_menu(ctx);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x98\xa0\xe5\xb0\x84"); /* 已取消映射 */
        ctx->needs_redraw = true;
        return;
    }
    /* 映射结束(完成/超时): 收尾**不在这里做** —— 弹窗层与 toast 由 dlg_poll 的
     * "空 DIALOG 页自愈 + toast 超时"统一关掉 (os_dialog.c), 页面不再自己写一个
     * 1.2s 计时器重复实现同一件事 (2026-11-21)。 */
    if (!s_map_active) return;
    if (s_map_func < 0 || s_map_func >= FUNC_MAX) return;
    /* 屏蔽"进入映射界面"那次确认键: 短时间(400ms)内不捕获任何按键, 防误映射 */
    if (esp_timer_get_time() < s_map_ignore_until) return;
    /* ★★★ 2026-10-27 撤回上一版的"等手里键松开再捕获"(linit 实测: 映射界面完全按不动,
     *   而不做映射时按键一切正常)。
     *   原因: 那段用 any_phys_pressed() 当门闸, 假设"没有键按下"是常态 —— 但手柄静止时
     *   hat/摇杆状态未必是"中位"(4 向编码、零点漂移都会让它恒为某个方向) ⇒ 门闸恒真
     *   ⇒ map_poll 每帧直接 return, 永远走不到捕获, 按键全被吞掉。
     *   根治"进映射时还按着键被误配"靠下面的 400ms 屏蔽就够了, 不再加全局等待。
     *   (教训: 别用"当前无输入"做前置条件 —— 输入设备可能永远有状态。) */

    /* 映射无操作 40s 超时: 自动取消, 恢复手柄导航, 避免卡死在映射界面 */
    if ((esp_timer_get_time() - s_map_last_progress) > 40000000LL) {
        s_map_active = false;
        s_capture_armed = false;
        s_capture_phys = -1;
        input_set_gamepad_nav_enabled(true);
        /* ★ 2026-10-27: 向导是边按边写内存映射的，超时若不回滚就留下**半套映射**
         *   ⇒ 用户回游戏方向键错乱, 得重连手柄/重启才恢复。 */
        bt_manager_restore_key_map();
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
                finish_key_mapping(ctx);
                return;
            }
            bt_manager_poll_new_press_reset();
            ctx->needs_redraw = true;
        }
        return;   /* 仍按住: 等待松开 */
    }

    /* 尚未捕获: 尝试捕获当前需映射功能的按键 */
    int fc = s_map_func;
    /* ★ 2026-10-27: poll_new_press 内部会**直接改写 s_map[fc]**，所以先记住旧值 ——
     *   如果接下来判定"按的是本轮已用过的键"(= 手柄没有更多键)，要把这个重复键还原，
     *   否则完成时最后一个功能会指向一个错误的重复键。 */
    phys_t prev_map = bt_manager_get_key_map((func_t)fc);
    int r = bt_manager_poll_new_press((func_t)fc);
    if (r > 0) {
        int phys = (int)bt_manager_get_key_map((func_t)fc);
        /* ★ "每个按键只映射一次": 这个物理键本轮已经映射过了 → 用户手上没有"新键"可指
         *   (典型: 没有 Start/Select 的手柄)。**不再让他重复按同一个键**, 直接保存结束。 */
        if (phys >= 0 && phys < 32 && (s_map_used & (1u << phys))) {
            bt_manager_set_key_map((func_t)fc, prev_map);   /* 还原, 别把重复键留在映射里 */
            ESP_LOGD(TAG, "按键映射: 键[%s]本轮已映射过 → 判定没有更多键, 提前完成",
                     bt_manager_phys_name((phys_t)phys));
            s_capture_armed = false;
            s_capture_phys = -1;
            finish_key_mapping(ctx);
            return;
        }
        if (phys >= 0 && phys < 32) s_map_used |= 1u << phys;
        s_capture_phys = (phys_t)phys;                         /* 记住这次的键, 等松开再推进 */
        s_capture_armed = true;                                /* 只有真实捕获到按键才允许随后推进 */
        s_map_last_progress = esp_timer_get_time();
        ESP_LOGI(TAG, "按键映射捕获: [%s] → %s (等松开确认)",
                 bt_manager_func_name((func_t)fc),
                 bt_manager_phys_name(s_capture_phys));
        ctx->needs_redraw = true;
    }
}

/* 正常收尾: 保存映射 + 恢复手柄导航 + 关向导/给完成提示。
 * 抽出来是因为"映射够 10 个功能"和"没有更多不同的键"两种结束方式都要走这一套。 */
static void finish_key_mapping(ui_ctx_t *ctx) {
    bt_manager_save_key_map();
    ESP_LOGI(TAG, "按键映射完成, 已保存到 NVS");
    s_map_active = false;
    s_map_cancel_req = false;               /* 已保存: 别再让触摸取消去回滚(会和 NVS 打架) */
    bt_manager_backup_key_map();            /* 把备份同步成已保存的值, 之后误调 restore 也无害 */
    input_set_gamepad_nav_enabled(true);
    os_dialog_pop(ctx);   /* 关映射弹窗回手柄主菜单 */
    os_dialog_toast(ctx, "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84\xe5\xae\x8c\xe6\x88\x90"); /* 按键映射完成 */
}

/* 映射弹窗按键: 映射期间**一切按键一律忽略**, 纯由 bt_manager_poll_new_press 硬捕获
 * 当前功能对应的键 (仅真实按下的物理键泵入映射, 不会产生任何系统动作).
 * 特别地, 返回键/退出键在此期间不作为"取消/退出"动作 —— 用户"硬设"的是按键本身,
 * 其功能作用在映射阶段不生效, 避免误终止映射. */
static bool map_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ctx; (void)d; (void)a; (void)ud;
    return true;   /* 其他键在映射期间一律忽略 */
}

/* ★ 2026-10-27: 映射期间**点屏幕 = 取消映射**。
 *   映射时 map_key 吞掉所有系统按键(这是故意的: 要能把任意键映射成"返回/退出"),
 *   所以物理返回键退不出来 —— 原来只能干等 40s 超时。触摸屏留一个明确出口。
 *   ⚠ 这里**只置标志**, 真正的清理放到 map_poll 里做: 在触摸回调内部直接
 *   os_dialog_clear_all() 会把当前这一层(回调正在用的 d)一起清掉, 回调返回后
 *   框架仍可能引用它 —— 属于"在回调里拆自己脚下地板", 不碰为妙。 */
static bool map_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)ctx; (void)d; (void)x; (void)y; (void)ud;
    s_map_cancel_req = true;      /* 真正的清理在 map_poll 里做 */
    return true;
}

static void start_key_mapping(ui_ctx_t *ctx) {
    if (!bt_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe5\x85\x88\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x89\x8b\xe6\x9f\x84"); /* 请先连接手柄 */
        return;
    }
    /* ★ 消费"待映射待办" —— 无论从哪条路进向导(本页连接成功 / 桌面 C5 / 本页菜单),
     *   都要把它取掉, 保证同一只新手柄只被引导一次。
     *   (C5 路径下桌面已经取过, 这里再取一次返回 false, 无害) */
    bt_manager_take_map_pending(NULL, 0);
    bt_manager_backup_key_map();   /* ★ 记住进向导前的映射: 取消/超时用它回滚 */
    s_map_cancel_req = false;      /* 清掉可能残留的触摸取消请求 */
    s_map_func = 0;
    s_map_used = 0;          /* 新的一轮映射: 清空"已用过的物理键" */
    s_map_active = true;
    s_capture_phys = -1;
    s_capture_armed = false;
    s_map_hold_wait = 0;     /* 清"等待松开"状态 */
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
    dlg.on_touch = map_touch;    /* ★ 点屏幕取消映射 (映射期间按键全被吞, 需要触摸出口) */
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(TAG, "按键映射启动: 请按下 [%s] 对应按键", bt_manager_func_name(F_UP));
}

/* ★ C7 (2026-10-02) 忘记此手柄: 确认后清历史+档案并停止回连
 * ★ 2026-11-23: 入口从主菜单移到"连接记录"的长按, 所以确认后必须**回到并刷新记录列表**
 *   (那条记录已经删了, 不刷新就会留一行死条目, 再点还会去连一个已忘记的手柄)。 */
static void on_forget_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;                 /* 非"确定" */
    if (!s_forget_name[0]) return;
    char nm[17];
    snprintf(nm, sizeof(nm), "%.16s", s_forget_name);
    if (bt_manager_forget_device(s_forget_name)) {
        os_dialog_pop(ctx);      /* 关确认框 → 回到连接记录列表 */
        os_dialog_pop(ctx);      /* 关掉旧的记录列表 (内容已过期) */
        open_hist_dialog(ctx);   /* 按最新历史重开 */
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xbf\x98\xe8\xae\xb0\xe6\xad\xa4\xe6\x89\x8b\xe6\x9f\x84"); /* 已忘记此手柄 */
        ESP_LOGI(TAG, "[EV] 用户忘记手柄(长按记录): %s", nm);
    }
}

/* ---- 主菜单选中回调 ---- */
static void on_gamepad_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) { os_pop(ctx); return; }   /* 返回 → 退出手柄页 */
    int raw = (result >= 0 && result < 6) ? s_gp_map[result] : result;  /* 显示位置 -> case */
    switch (raw) {
    case 0: {  /* 添加设备: 扫描蓝牙列表弹窗 (3.3), 确认连接 */
        /* V1.6.x fix(死锁): 原逻辑"未就绪就只弹'初始化中'并 return"会**永久卡住** ——
         * 唯一能触发"按需启动蓝牙"的 open_scan_dialog() 恰好被这道守卫挡在外面,
         * 于是蓝牙一旦关栈(开机回连窗口超时 / 屏保省电 / os_hw_release), 本菜单就
         * 永远显示"初始化中", 再也起不来(实测 bug)。
         * 改为: 先按需请求启动 —— os_hw_request→bt_manager_enable 由内部RAM栈任务
         * 同步执行并阻塞至就绪; 就绪后再打开扫描弹窗; 仅当初始化真的失败才提示重试。 */
        /* ★ 2026-11-22: 上面那段"同步请求"已删除 —— 它和桌面长按确认键那条路径一样,
         *   会在主循环线程里阻塞 1~2 秒。现在统一交给 open_scan_dialog():
         *   · 栈已就绪 → 直接开扫(0ms);
         *   · 栈没就绪 → 后台任务拉起 + 框上显示"正在启动蓝牙…", 主循环不阻塞, 就绪
         *     后由 scan_dlg_poll 自动接着开始扫描。
         *   这也顺带解决了原注释里那个死锁的根: 不再有"初始化中 → break"这条死路。 */
        open_scan_dialog(ctx);
        break;
    }
    case 1:  /* 按键映射 (3.3 原版流程) */
        start_key_mapping(ctx);
        break;
    case 2:  /* 连接记录: 列表 + 末尾"设备信息"; 短按连接, 长按 1s 忘记 */
        open_hist_dialog(ctx);
        break;
    case 3:  /* WiFi 手柄: 选 AP直连 / WiFi共享 */
        open_wifi_mode(ctx);
        break;
    case 4: {  /* ★ 2026-11-24: 开机自动回连 开⇄关 (默认开, 立即落盘) */
        /* 只影响"开机那一次": 关掉后仍可手动「添加设备」/开蓝牙/进游戏抓手柄。
         * 原地改这一行文字并保持弹窗 (同一框变形), 不重新压栈 ⇒ 连点也不会叠层。 */
        bool turn_on = !bt_manager_is_boot_auto_reconnect();
        bt_manager_set_boot_auto_reconnect(turn_on);
        os_dlg_stack_t *d = os_dialog_top_mut(ctx);
        if (d) {
            os_dialog_mark_handled(ctx);   /* 已接管: dlg_finish_top 不再自动关窗 */
            snprintf(d->items[result], sizeof(d->items[0]),
                     "\xe8\x87\xaa\xe5\x8a\xa8\xe5\x9b\x9e\xe8\xbf\x9e: %s",   /* 自动回连: 开/关 */
                     turn_on ? "\xe5\xbc\x80" : "\xe5\x85\xb3");   /* 开 / 关 */
            d->sel = result;
        }
        ESP_LOGI(TAG, "[EV] 开机自动回连 → %s", turn_on ? "开" : "关");
        break;
    }
    case 5:  /* 返回 (底部固定返回行一般走 result<0; 此项兜底) */
        os_pop(ctx);
        break;
    default: break;
    }
}

/* ★ C5 连接后自动映射 (2026-10-02): 映射向导原来是本文件私有的, 只有"在本页点选设备
 *   连上"或"在本页菜单点按键映射"才进得去 → **后台自动回连永远进不了向导**。
 *   现在: 任何页面(主要是主菜单)发现"连上了没见过的手柄", 就调用本函数 + os_push 本页,
 *   on_enter 里直接把向导叠在菜单之上, 用户一进来就是映射流程。
 * ⚠ 2026-11-24 linit: **桌面那条自动跳转已移除** (后台回连不再抢占界面), 当前没有调用方。
 *   接口保留(API 只增不改): 以后若要恢复"某处主动引导映射", 仍是 os_push(OS_PAGE_GAMEPAD)
 *   + 本函数, 不要在各处复制向导启动逻辑。 */
static bool s_want_map;
void page_gamepad_request_map_on_enter(void) { s_want_map = true; }

/* 手柄配置菜单 —— 进入本页时、以及映射向导结束后都用它当落点 (复用, 别写两份) */
static void p_gamepad_menu(ui_ctx_t *ctx) {
    /* ★ 2026-11-23: 不再按连接状态隐藏项 —— "设备信息/忘记此手柄"已移进"连接记录",
     *   主菜单恒为固定项 + 底部返回, 结构稳定(旧版未连接时会少两项, 行列位置会跳)。
     * ★ 2026-11-24: 新增第 5 项「开机自动回连: 开/关」—— 文字随开关状态生成,
     *   其余项仍是静态串, 布局与选中逻辑不变。 */
    snprintf(s_gp_arc_item, sizeof s_gp_arc_item,
             "\xe8\x87\xaa\xe5\x8a\xa8\xe5\x9b\x9e\xe8\xbf\x9e: %s",   /* 自动回连: 开/关 */
             bt_manager_is_boot_auto_reconnect()
                 ? "\xe5\xbc\x80"                                /* 开 */
                 : "\xe5\x85\xb3");                               /* 关 */
    int count = 0;
    for (int src = 0; src < 6; src++) {
        s_gp_items_dyn[count] = (src == 4) ? s_gp_arc_item : s_gp_items[src];
        s_gp_map[count] = src;
        count++;
    }
    os_dialog_list(ctx, "\xe6\x89\x8b\xe6\x9f\x84\xe9\x85\x8d\xe7\xbd\xae", s_gp_items_dyn, count, 0,
                   on_gamepad_select, NULL);
}

static void p_gamepad_enter(ui_ctx_t *ctx) {
    p_gamepad_menu(ctx);
    /* ★ 被"连接后自动映射"点名进来的 → 直接在菜单之上打开按键映射向导。
     *   弹窗栈只渲染栈顶 ⇒ 用户看到的直接就是「请按下 X 按键」，
     *   关掉/做完向导后回到下面这层菜单 (linit 2026-10-27: 连上就提示按键，不要先弹提示框)。 */
    if (s_want_map) {
        s_want_map = false;
        ESP_LOGD(TAG, "[EV] 自动进入按键映射 (连接后自动映射)");
        start_key_mapping(ctx);
    }
}

/* ★ 2026-10-02 修"手柄没法控制任何界面":
 *   start_key_mapping() 会 input_set_gamepad_nav_enabled(false) 关掉手柄导航(防干扰映射),
 *   而恢复(true)只写在 map_poll() 的"超时/完成"两条路径里 —— 本模块**原来没有 on_exit**。
 *   于是: 映射向导还开着时用**物理返回键**离开手柄页 → 那条 dialog 随页面一起消失,
 *   map_poll 再也不跑 → nav 永远停在 false → 手柄之后在**任何**界面都导航不了。
 *   这里补 on_exit 兜底恢复(幂等), 并清掉映射中间态。 */
static void p_gamepad_exit(ui_ctx_t *ctx) {
    (void)ctx;
    if (s_map_active) {
        ESP_LOGD(TAG, "[EV] 带着未完成的映射离开手柄页 → 取消映射并恢复手柄导航");
    }
    s_map_active = false;
    s_capture_armed = false;
    s_capture_phys = -1;
    input_set_gamepad_nav_enabled(true);   /* ★ 幂等兜底: 无论如何都把导航恢复 */
    /* ★ 本页**不走** os_hw_lease 还原蓝牙: 退出后手柄连接仍要继续用, 蓝牙不能关
     *   (linit 2026-10-14: 手柄页除外)。 */
}

/* ★ 2026-11-24: 空壳页兜底 —— 本页**没有 render/touch**, 界面全在弹窗栈里。
 *   "弹窗栈空了, 页面却还停在本页" 就是空壳状态: 屏幕留着上一帧画面(看着像主菜单)、
 *   触摸无人接收 (os_handle_touch 里 m->touch==NULL 直接丢弃)、只剩物理键还能走 exit_confirm。
 *   成因是所有"清窗"路径里只要有一条漏了重建菜单/退页 (2026-11-24 就是手柄回连那条),
 *   页面就会永久卡在这里。这里整帧检查把它收敛掉: 空了就按正常入口重建菜单(幂等)。
 *   与 USB_BT/USB_NET 的 poll 兜底同一思路(那两个页选择直接 os_pop 退页); 本页选"恢复菜单",
 *   因为用户很可能正站在手柄配置里, 把他弹回桌面比留在菜单更突兀。 */
static void p_gamepad_poll(ui_ctx_t *ctx) {
    if (os_dialog_depth() > 0) return;   /* 有弹窗 = 界面正常 */
    ESP_LOGW(TAG, "手柄页弹窗栈为空(空壳状态) → 重建手柄菜单");
    p_gamepad_menu(ctx);
}

static const os_module_t s_mod_gamepad = {
    .name       = "gamepad",
    .page_id    = OS_PAGE_GAMEPAD,
    .on_enter   = p_gamepad_enter,
    .on_exit    = p_gamepad_exit,
    .poll       = p_gamepad_poll,
    .fullscreen = false,
    .exit_confirm = true,   /* ★ 2026-10-25 linit: 应用页按返回键先弹"退出程序?" */
};

/* 网页手柄等待 WiFi 连接: 用全局 service 而非模块 poll, 保证在任意页面
 * (尤其是"连接WiFi"/输密码页) 期间都能检测到连接成功并自动启动服务. */
static const os_service_t s_svc_webpad_wifi = {
    .name = "webpad_wifi",
    .tick = p_gamepad_wifi_poll,
};

void os_page_gamepad_register(void) { os_register(&s_mod_gamepad); os_register_service(&s_svc_webpad_wifi); }
