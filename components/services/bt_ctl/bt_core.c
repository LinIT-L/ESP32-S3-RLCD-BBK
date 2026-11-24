/*
 *  bt_core.c - 蓝牙连接核心 (全新重写)
 *  ---------------------------------------------------------------------------
 *  底稿 = ESP-IDF 官方 examples/bluetooth/esp_hid_host。
 *  只用官方 API: esp_hid_gap_init / esp_hid_scan / esp_hidh_dev_open / esp_hidh。
 *  只走 BLE (S3 仅 BLE, HID_HOST_MODE = HIDH_BLE_MODE)。
 *  本层不做任何按键含义解析 / 映射 / NVS 存档, 只负责:
 *    扫描 / 连接 / 转发 INPUT 报告到上层回调。
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_hidh.h"

#include "esp_hid_gap.h"   /* 官方底层: esp_hid_gap_init / esp_hid_scan */

#include "bt_core.h"

static const char *TAG = "bt_core";

/* 扫描结果缓存 (链表) */
static esp_hid_scan_result_t *s_scan_results = NULL;
static size_t s_scan_count = 0;
/* ★ 2026-11-22: 链表换新/释放与"UI 按地址取一条"之间的互斥。
 *   原来 scan_task 在下一轮扫描开始时直接 free 整条链表, 而 UI 任务可能正
 *   在 bt_manager_connect_device() 里遍历它 (用户点选设备的那一刻) ——
 *   use-after-free 读, 轻则连错地址, 重则踩已释放堆块。 */
static SemaphoreHandle_t s_scan_mtx = NULL;
static void scan_list_lock(void)
{
    if (!s_scan_mtx) s_scan_mtx = xSemaphoreCreateMutex();
    if (s_scan_mtx) xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
}
static void scan_list_unlock(void) { if (s_scan_mtx) xSemaphoreGive(s_scan_mtx); }

static bt_core_cbs_t s_cbs;
static bool s_hidh_init = false;   /* esp_hidh 是否已初始化 */
/* V1.5.x: esp_hid_gap/esp_hidh 底层已初始化 — ESP-IDF 无公开 deinit 接口
 * (esp_hid_gap_init 用内部信号量判重, 重复调用必报 "Already initialised").
 * 因此 disable→enable 循环必须复用底层, 只复位 s_hidh_init 逻辑标志. */
static bool s_gap_inited = false;
static bool s_scanning = false;
static bool s_connected = false;
static esp_hidh_dev_t *s_conn_dev = NULL;   /* 当前连接设备句柄 */
static bool s_radio_on = false;              /* ★ 射频在跑=蓝牙真的开着 */

/* ★★★ 2026-10-27 linit「蓝牙显示开且关不掉」根因修复 ★★★
 *   esp_hidh 内部维护一个设备链表, `esp_hidh_deinit()` 要求它**为空**
 *   (ESP-IDF: "Please disconnect all devices first!")。
 *   但设备对象是在 OPEN 事件里创建的 —— 包括 **OPEN 失败** 的那些(连到一半失败/超时),
 *   它们既不会进 s_conn_dev, 也没有 CLOSE 回调来回收 ⇒ 残留在链表里,
 *   此后每一次 deinit 都失败 ⇒ 设置页永远显示「开」, 用户点了关也没用(实测日志):
 *     E ESP_HIDH: esp_hidh_deinit(173): Please disconnect all devices first!  ×6
 *   这里把**见过的所有 dev** 记下来, 拆栈前逐个 close/free 掉。 */
#define BT_DEV_SLOTS 4
static esp_hidh_dev_t *s_devs[BT_DEV_SLOTS];

static void dev_track(esp_hidh_dev_t *d)
{
    if (!d) return;
    for (int i = 0; i < BT_DEV_SLOTS; i++) if (s_devs[i] == d) return;
    for (int i = 0; i < BT_DEV_SLOTS; i++) if (!s_devs[i]) { s_devs[i] = d; return; }
}

static void dev_untrack(esp_hidh_dev_t *d)
{
    for (int i = 0; i < BT_DEV_SLOTS; i++) if (s_devs[i] == d) s_devs[i] = NULL;
}

static TaskHandle_t s_scan_task = NULL;

/* ============================================================================
 * ★★★ 2026-11-23 R2: 扫描事务 (独占) + 与连接事务的互斥 ★★★
 * ----------------------------------------------------------------------------
 * 依据 (ESP-IDF 6.0 components/esp_hid/src/esp_hid_gap.c + 本仓 esp_hid_gap.c):
 *   ① esp_hid_scan() 起手就 free 上一轮的 bt/ble_scan_results, 结果攒在**文件静态**
 *      链表里 ⇒ 两个线程同时进来, 后进者会把先进者的半成品链表 free 掉:
 *      轻则结果串台(点 A 连到 B), 重则踩已释放堆块。
 *   ② 两者共用同一个 ble_hidh_cb_semaphore, 一次 INQ_CMPL 只有一个 give ⇒
 *      先完成的一方取走信号, 另一方永远等不到自己的完成事件。
 *   ③ esp_hidh_dev_open() 期间控制器正在建链路/做 GATT 发现, 此时再起扫描会抢
 *      GAP 状态机与射频时隙, 两边都容易失败。
 * 所以把扫描做成**整机唯一**的独占资源:
 *   · s_scan_busy      = 有线程正卡在 esp_hid_scan 里 (闸门本体);
 *   · s_scan_task_alive= 任务式扫描还没退出的计数 (生命周期, 含"回调还在跑"窗口);
 *   · s_open_tx        = 正卡在 esp_hidh_dev_open 里的事务数 (连接事务在飞).
 * 规则: 闸门被占 ⇒ 拒绝新扫描; 连接事务在飞 ⇒ 拒绝新扫描; 扫描在跑 ⇒ 上层不得
 *       发起"用户/回连"连接 (由 bt_manager 统一等待收尾, 见 bt_connect_async)。
 * ⚠ 唯一的例外是"边扫边连": 扫描回调命中回连目标时立即连接 —— 那是扫描自身的
 *   消费者, 不算冲突方; bt_manager 保证此时不会再起第二个扫描/open。
 * ==========================================================================*/
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_scan_busy = false;
static volatile int  s_scan_task_alive = 0;
static volatile int  s_open_tx = 0;

/* 取闸门. 失败会打一条 W (调用方据此放弃, 不要重试/排队)。 */
static bool scan_begin(const char *who)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_scan_mux);
    if (!s_scan_busy && s_open_tx <= 0) {
        s_scan_busy = true;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_scan_mux);
    if (!ok) {
        ESP_LOGW(TAG, "扫描闸门忙(扫描=%d 连接事务=%d) ⇒ 拒绝 %s",
                 (int)s_scan_busy, (int)s_open_tx, who);
    }
    return ok;
}

static void scan_release(void)
{
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_busy = false;
    if (s_scan_task_alive > 0) s_scan_task_alive--;
    taskEXIT_CRITICAL(&s_scan_mux);
}

static void open_tx_enter(void)
{
    taskENTER_CRITICAL(&s_scan_mux);
    s_open_tx++;
    taskEXIT_CRITICAL(&s_scan_mux);
}

static void open_tx_leave(void)
{
    taskENTER_CRITICAL(&s_scan_mux);
    if (s_open_tx > 0) s_open_tx--;
    taskEXIT_CRITICAL(&s_scan_mux);
}

/* ---------- esp_hidh 事件回调 ---------- */
static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        dev_track(param->open.dev);      /* ★ 失败的对象也要登记, 否则拆栈时清不掉 */
        if (param->open.status == ESP_OK) {
            s_connected = true;
            s_conn_dev = param->open.dev;
            ESP_LOGI(TAG, "OPEN: %s", esp_hidh_dev_name_get(param->open.dev));
            if (s_cbs.on_connected) {
                s_cbs.on_connected(true, param->open.dev);
            }
        } else {
            /* ⛔ 不能在这里 esp_hidh_dev_close(): OPEN 失败时对象尚未完全建立,
             *   实测直接 LoadProhibited panic(2026-10-27)。残留对象由拆栈前的清理遍历处理。 */
            ESP_LOGW(TAG, "OPEN failed (status=%d)", (int)param->open.status);
            if (s_cbs.on_connected) {
                s_cbs.on_connected(false, param->open.dev);
            }
        }
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        /* 打印原始 INPUT 报告, 排查"按键无映射".
         * V1.2 性能: 原为 ESP_LOGI —— 本回调是**高频路径**(手柄连着时 100+ Hz),
         * 每条日志 ~70 字节经阻塞式控制台写出, 会持续占用 UART 并抖动按键处理.
         * 降为 DEBUG: 默认日志级别下静默, 需要排查时 esp_log_level_set("BT", ESP_LOG_DEBUG). */
        ESP_LOGD(TAG, "INPUT rid=%u len=%u data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 param->input.report_id, param->input.length,
                 param->input.length > 0 ? param->input.data[0] : 0,
                 param->input.length > 1 ? param->input.data[1] : 0,
                 param->input.length > 2 ? param->input.data[2] : 0,
                 param->input.length > 3 ? param->input.data[3] : 0,
                 param->input.length > 4 ? param->input.data[4] : 0,
                 param->input.length > 5 ? param->input.data[5] : 0,
                 param->input.length > 6 ? param->input.data[6] : 0,
                 param->input.length > 7 ? param->input.data[7] : 0,
                 param->input.length > 8 ? param->input.data[8] : 0,
                 param->input.length > 9 ? param->input.data[9] : 0);
        /* 边界校验: 丢弃空/过大/非法 report_id, 防异常报告越界喂给解码层.
         * HID INPUT 报告长度 ≤ 64 字节(标准规范). */
        if (s_cbs.on_input && param->input.data &&
            param->input.length > 0 && param->input.length <= 64) {
            s_cbs.on_input(param->input.dev,
                           param->input.report_id,
                           param->input.data,
                           param->input.length);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        dev_untrack(param->close.dev);
        s_connected = false;
        s_conn_dev = NULL;
        ESP_LOGI(TAG, "CLOSE: %s", param->close.dev ? esp_hidh_dev_name_get(param->close.dev) : "");
        if (s_cbs.on_disconnected) {
            s_cbs.on_disconnected(false, param->close.dev);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------- 扫描任务 (esp_hid_scan 是阻塞的) ---------- */
static void scan_task(void *arg)
{
    size_t results_len = 0;
    esp_hid_scan_result_t *results = NULL;

    /* esp_hid_scan 内部用静态 ble_scan_results 暂存; 上次结果必须已由上层 free.
     * 若上一次扫描的结果还没被消费 (esp_id_scan 返回后只在成功分支 free), 下次调用
     * 会报 "There are old scan results". 因此在发起本次扫描前先释放上一轮缓存. */
    if (s_scan_results) {
        scan_list_lock();
        esp_hid_scan_results_free(s_scan_results);
        s_scan_results = NULL;
        s_scan_count = 0;
        scan_list_unlock();
    }

    ESP_LOGI(TAG, "scan start...");
    /* ★ R2: 闸门已在 bt_core_scan() 里占好 (任务式扫描独占 s_scan_busy),
     *   这里直接用; esp_hid_scan 内部有超时上限 (见 esp_hid_gap.c)。 */
    esp_err_t err = esp_hid_scan((uint32_t)5, &results_len, &results);
    if (err == ESP_OK) {
        scan_list_lock();
        s_scan_results = results;
        s_scan_count = results_len;
        scan_list_unlock();
        ESP_LOGI(TAG, "scan done: %u results", (unsigned)results_len);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));   /* ★ 提到 W: 扫不到设备时必须能看到原因 */
        if (results) {
            esp_hid_scan_results_free(results);
        }
    }

    s_scanning = false;
    /* ★ R2: 先让上层收敛 (on_scan_done 会读结果链表), **之后**才放开闸门 ——
     *   顺序反了的话, 另一个扫描/拆栈可能在上层还在用链表时把它 free 掉。 */
    if (s_cbs.on_scan_done) {
        s_cbs.on_scan_done();
    }
    scan_release();
    /* 扫描事务到这里才算彻底结束: 通知上层"可以真的关射频/拆栈了" */
    if (s_cbs.on_scan_idle) {
        s_cbs.on_scan_idle();
    }
    vTaskDelete(NULL);
}

bool bt_core_init(const bt_core_cbs_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    }
    /* V1.5.x 幂等: esp_hid_gap/esp_hidh 底层不可释放, disable→enable 循环须复用.
     * 已初始化则只恢复就绪标志并更新回调, 避免 esp_hid_gap_init 重复调用报
     * "Already initialised" (ESP-IDF 无公开 deinit, 二次调用必失败). */
    if (s_gap_inited) {
        /* ★ 2026-11-23 R3: 不再强置 s_hidh_init=true —— 轻量关射频(radio_off)现在会把
         *   esp_hidh 一起撤掉(bluedroid disable 会清 gattc app 表), 是否重建由
         *   bt_core_radio_on() 按真实状态决定, 这里强置会让它误以为还活着。 */
        ESP_LOGI(TAG, "bt_core already initialised (idempotent), reuse bottom layer");
        bt_core_radio_on();     /* ★ 上电: 上一次"关"只是把射频 disable 掉 */
        /* ★ 2026-11-23 R3: 判据用 s_radio_on(射频真的开了), **不能**把 esp_hidh 重建
         *   成功与否也算进"蓝牙可用"里 —— 那会让"开蓝牙"整个失败(UI 显示关、再点无反应),
         *   而实际射频已经开好。esp_hidh 只影响手柄连接, 已在 radio_on 里单独打 E。 */
        return s_radio_on;
    }
    ESP_LOGI(TAG, "bt_core_init enter");

    esp_err_t err = esp_hid_gap_init(HIDH_BLE_MODE);
    if (err != ESP_OK) {
        /* 兜底: 若上层异常路径残留了 esp_hid_gap 静态状态(信号量未清),
         * esp_hid_gap_init 会报 "Already initialised"。复位后重试一次即可恢复,
         * 避免蓝牙永久卡"初始化中"(实测 bug, 见 bt_core_deinit 注释)。 */
        ESP_LOGW(TAG, "esp_hid_gap_init failed (%s), 复位残留状态后重试", esp_err_to_name(err));
        esp_hid_gap_deinit();
        err = esp_hid_gap_init(HIDH_BLE_MODE);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "HID host init...");
    /* 顺序与官方 esp_hid_host 例程一致: 先注册 gattc 回调, 再 esp_hidh_init.
     * esp_hidh_init 内部会注册 BLE GATT 事件, 需 gattc 回调先就绪. */
    err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gattc register failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        /* 兜底: 若 esp_hidh 静态状态残留(异常路径未走 deinit), 复位后重试一次。 */
        ESP_LOGW(TAG, "esp_hidh_init failed (%s), 复位残留状态后重试", esp_err_to_name(err));
        esp_hidh_deinit();
        err = esp_hidh_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_gap_inited = true;
    s_hidh_init = true;
    /* V1.6.x 诊断: 打印"本机"BLE 地址 —— 手柄是靠主机地址(+名字)认主人的。
     * 若本机地址每次启动都变(模块 eFuse 无 MAC 时 IDF 可能随机派生), 手柄就永远
     * 认不出我们 → 指示灯一直闪、且每次都被当成新主机重新配对。跨启动对比本行即可判定。 */
    {
        const uint8_t *oa = esp_bt_dev_get_address();
        if (oa) {
            ESP_LOGW(TAG, "本机BLE地址: %02x:%02x:%02x:%02x:%02x:%02x (手柄据此识别主机)",
                     oa[0], oa[1], oa[2], oa[3], oa[4], oa[5]);
        }
        int bn = esp_ble_get_bond_device_num();
        ESP_LOGW(TAG, "本地已保存绑定数: %d", bn);
    }
    s_radio_on = true;      /* ★ 栈建好 = 射频在跑 */
    ESP_LOGI(TAG, "HID host init done (radio on, ready)");
    return true;
}

/* ★★★ 2026-10-27 linit「蓝牙一直显示开、关不掉」最终修复 ★★★
 *   原来 is_ready 只看 s_hidh_init(栈结构是否建过)。可这个栈**永远拆不干净**:
 *   ESP-IDF 的 esp_hidh_deinit 要求设备链表为空, 而"OPEN 失败"的对象会残留;
 *   即便绕开, esp_ble_hidh_deinit 还会报 "App Unregister Failed"。
 *   ⇒ 每次点"关"都失败, is_ready 一直 true, 界面永远显示「开」。
 *
 *   现在的语义改成**【射频是否在跑】**: 关 = 停扫描 + 断开 + controller/bluedroid disable,
 *   栈结构保留(下次 enable 毫秒级复用), 界面/耗电都真·关闭。
 *
 *   ★ 2026-10-30 linit「蓝牙没启用就不要占内存」: 上面这条"保留栈结构"的代价是
 *     **控制器/Bluedroid/esp_hidh 的约 67KB 内部 RAM 一旦分配就永不归还**
 *     (实测开机初始化前后 193,411 → 120,463 B, 差 72,948 B, 之后一直是这个数)。
 *     现在把 bt_core_full_deinit() 重新接回**唯一出口** bt_core_deinit_deep():
 *       · 只在"最后一个使用者退出"(os_hw 引用计数归零)时调用 → 游戏/手柄页退出即还内存;
 *       · 失败仍按原兜底退回"只关射频"(栈保留、功能不坏), 不会再出现关不掉;
 *       · 设置页手动关 / 睡眠流程仍走轻量 bt_core_deinit()(只关射频, 快)。 */
void bt_core_radio_on(void);    /* 前向: init 的幂等分支要用 */
void bt_core_radio_off(void);

bool bt_core_is_ready(void)
{
    return s_radio_on;
}

static void bt_core_full_deinit(void);   /* 前向: 深度拆栈 (2026-10-30 起由"最后使用者退出"路径调用) */

/* ---- 射频开/关 (bt_manager 关蓝牙走这里) ---- */
void bt_core_radio_off(void)
{
    if (!s_radio_on) return;
    bt_core_stop_scan();
    /* ★ R2: 停扫后**等扫描任务真的退出**再关底层 (最多 1.5s)。
     *   它此刻可能正卡在 esp_hid_scan 等完成事件 —— 现在就把 Bluedroid 关掉,
     *   那条事件永远不来, 任务只能等自己的超时上限才退, 期间一直占着 6KB 栈,
     *   而且它会去访问即将失效的信号量。等它退出再关, 顺序才是干净的。 */
    if (!bt_core_scan_wait_idle(1500)) {
        ESP_LOGW(TAG, "扫描任务未在 1.5s 内退出 ⇒ 仍继续关射频 (该任务会自行超时收尾)");
    }
    if (s_connected && s_conn_dev) bt_core_disconnect();   /* ★ 防 s_conn_dev 悬空 */
    for (int i = 0; i < 10 && s_connected; i++) vTaskDelay(pdMS_TO_TICKS(40));

    /* ★★★ 2026-11-23 R3 真机修复: 必须在 bluedroid disable **之前**撤掉 esp_hidh ★★★
     *   实测日志: 关蓝牙→立刻再开→连接时 `bta_gattc_process_api_open Failed,
     *   unknown client_if: 3`，之后再也连不上。
     *   原因: esp_bluedroid_disable() 会让 BTA 清空 gattc app 表, 但 esp_hidh 内部
     *   信号量仍在、hid_gattc_if 仍是旧值(3) ⇒ 重开时 esp_hidh_init 报
     *   "Already initialized" 直接跳过, 于是每次 open 都拿着失效的 client_if 去开会话。
     *   在 disable 之前(此时 client_if 仍有效) deinit 掉, radio_on 才能重新注册拿到新号。 */
    if (s_hidh_init) {
        esp_err_t ed = esp_hidh_deinit();
        /* ★ 2026-11-23 R3: ESP_ERR_INVALID_STATE = "Already deinitialized" ——
         *   这恰恰说明已经拆干净了, 必须当**成功**处理。原来把它当失败重试 3 次
         *   (真机日志连续 6 条 `esp_hidh_deinit: Already deinitialized` + 一次告警),
         *   既多花 ~300ms 才关掉射频, 又在日志里刷一片 E。 */
        for (int i = 0; i < 3 && ed != ESP_OK && ed != ESP_ERR_INVALID_STATE; i++) {
            vTaskDelay(pdMS_TO_TICKS(80));      /* 等 CLOSE 把设备对象从链表摘掉 */
            ed = esp_hidh_deinit();
        }
        if (ed == ESP_OK || ed == ESP_ERR_INVALID_STATE) {
            s_hidh_init = false;
        } else {
            /* deinit 没做成(通常是有设备对象还没被 CLOSE 摘掉): bluedroid 一 disable,
             * gattc 的 client_if 就失效了。这里也标记成"需要重建", 下次连接时
             * bt_core_hidh_ensure() 会先补 deinit 再 init, 自愈回可用状态。 */
            s_hidh_init = false;
            ESP_LOGW(TAG, "esp_hidh_deinit 未成功(%s) ⇒ 下次连接时重建", esp_err_to_name(ed));
        }
    }

    esp_err_t e1 = esp_bluedroid_disable();
    esp_err_t e2 = esp_bt_controller_disable();
    s_radio_on = false;
    s_connected = false;
    ESP_LOGI(TAG, "射频已关闭 (bluedroid=%s controller=%s) ⇒ 界面显示「关」, 无线电不再收发",
             esp_err_to_name(e1), esp_err_to_name(e2));
}

void bt_core_radio_on(void)
{
    if (s_radio_on) return;
    if (!s_gap_inited) return;             /* 栈还没建过: 由 bt_core_init 负责 */
    esp_err_t e1 = esp_bt_controller_enable(HIDH_BLE_MODE);
    esp_err_t e2 = esp_bluedroid_enable();
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "射频开启失败 (controller=%s bluedroid=%s)",
                 esp_err_to_name(e1), esp_err_to_name(e2));
        return;
    }
    /* ★★★ 2026-11-23 R3: 这里**只**开射频, 不重建 esp_hidh ★★★
     *   esp_hidh_init() 内部 esp_ble_hidh_init → esp_ble_gattc_app_register(0) 之后是
     *   `WAIT_CB()` = 无限等 GATTC 注册事件。若把它放在"开蓝牙"的同步路径上, 事件
     *   一旦不来, 整个 enable 就卡死 20s 并回滚成"开不了"(还会让 s_init_task_running
     *   永远为 true, 之后每次开都走"等它"⇒ 永久失效)。
     *   开关必须永远快且可靠 ⇒ 射频开完就返回; esp_hidh 的重建推迟到**真正要连接时**
     *   (bt_core_hidh_ensure), 那里本来就有连接看门狗兜底。 */
    s_radio_on = true;
    ESP_LOGI(TAG, "射频已开启%s", s_hidh_init ? "" : " (esp_hidh 待连接时重建)");
}

/* 对外: 关蓝牙 = 关射频 (不再拆栈, 见上面注释) */
void bt_core_deinit(void)
{
    bt_core_radio_off();
}

/* ---- 深度拆栈: 保留但**不在正常路径调用** ----
 *   它需要满足 esp_hidh 设备链表为空, 且 esp_ble_hidh_deinit 能成功 unregister app;
 *   实测这两条在本机经常不成立, 一旦失败就会留下"半拆"状态(比不拆更糟)。 */
static void bt_core_full_deinit(void)
{
    /* ★★★ 2026-11-23 R2: 扫描任务还活着就**绝不拆栈** ★★★
     *   它正卡在 esp_hid_scan 里等着 Bluedroid 的完成事件; 一旦 controller/Bluedroid
     *   被 deinit, 它随后访问的句柄/信号量全是已释放内存 ⇒ LoadProhibited。
     *   bt_shutdown() 已经把"扫描未收尾"的释放推迟到 s_release_pending (等扫描任务
     *   退出后由 bt_release_flush 执行), 正常路径不会走到这里; 这里是最后一道闸:
     *   短等 1s 仍不空闲就**放弃本次拆栈** —— 宁可晚一点还内存, 也不冒 UAF 的风险。 */
    if (!bt_core_scan_wait_idle(1000)) {
        ESP_LOGE(TAG, "扫描任务仍未退出(busy=%d alive=%d) ⇒ 放弃本次深拆栈, 稍后重试",
                 (int)s_scan_busy, s_scan_task_alive);
        return;
    }
    /* ★ 2026-10-18: 扫描标志必须复位 —— 若拆栈时正好有个扫描任务卡在 esp_hid_scan 里，
     *   s_scanning 会一直保持 true ⇒ 之后 bt_core_scan() 直接 return，永不再扫，
     *   表现为 UI "一直显示正在扫描..."。 */
    s_scanning = false;
    s_scan_task = NULL;
    if (s_scan_results) {
        scan_list_lock();
        esp_hid_scan_results_free(s_scan_results);
        s_scan_results = NULL;
        s_scan_count = 0;
        scan_list_unlock();
    }
    /* V1.6.x 真释放底层内存: 退出蓝牙后归还内部RAM, 避免"用过蓝牙就常驻几十KB"需要软重启才释放.
     *
     * ⚠ 关键: 拆栈必须"逐层清掉各自的静态状态", 否则下次 enable 会在各层撞
     *   "Already initialized" 而永久卡"初始化中"/"连接超时"(实测):
     *     · esp_hidh_init()  → 守卫 s_esp_hidh_devices_semaphore
     *     · esp_hid_gap_init() → 守卫 bt_hidh_cb_semaphore
     *   顺序必须"依赖自下而上": esp_hidh(依赖 GATTC/Bluedroid) → controller/Bluedroid → esp_hid_gap. */

    /* 第一步: 断开并**等 CLOSE 事件真正落地**。
     * esp_hidh_deinit() 要求内部设备链已空(TAILQ_EMPTY), 而 bt_core_disconnect() 只是
     * **异步**发出关连请求, 设备对象要等 CLOSE 回调才从链表移除。若不等就 deinit →
     * 失败 → 它的静态信号量永远无法复位 → 之后 esp_hidh_init 恒报 "Already initialized"
     * → 蓝牙核心永久不就绪(界面反复"正在连接"→"连接超时")。
     * 这正是"反复连接几次后彻底连不上"的根因。 */
    if (s_conn_dev || s_connected) {
        bt_core_disconnect();
        for (int i = 0; i < 15 && (s_conn_dev || s_connected); i++) {
            vTaskDelay(pdMS_TO_TICKS(40));   /* 最多等 600ms 让 CLOSE 回调清空 */
        }
    }

    /* ★ 清掉"我们没见过 CLOSE 的"残留设备对象 (OPEN 失败的、超时没回 CLOSE 的)。
     *   不清它们, esp_hidh_deinit() 一定报 "Please disconnect all devices first!",
     *   于是蓝牙永远关不掉。 */
    for (int i = 0; i < BT_DEV_SLOTS; i++) {
        esp_hidh_dev_t *d = s_devs[i];
        if (!d) continue;
        if (esp_hidh_dev_exists(d)) {
            ESP_LOGW(TAG, "拆栈前清理残留设备对象 %p (close+free)", (void *)d);
            esp_hidh_dev_close(d);
            vTaskDelay(pdMS_TO_TICKS(60));
            if (esp_hidh_dev_exists(d)) esp_hidh_dev_free(d);
        }
        s_devs[i] = NULL;
    }

    /* 第二步: 关 esp_hidh 层(带重试)。**它失败就绝不能继续拆 Bluedroid** ——
     * 否则会留下"hidh 已废、底层已拆"的不可恢复状态(只能重启)。此时放弃本次拆栈,
     * 保留 BT 栈以保功能(代价: 蓝牙关着也占约 67KB 内部 RAM), 下次 enable 走幂等复用。 */
    esp_err_t hidh_ret = esp_hidh_deinit();
    for (int i = 0; i < 5 && hidh_ret != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(60));
        hidh_ret = esp_hidh_deinit();
    }
    if (hidh_ret != ESP_OK) {
        /* ★★★ 2026-10-27 linit「蓝牙显示开且关不掉」修复 ★★★
         *   原来这里只打一条 LOGD(默认看不见) 就 return —— 但**射频根本没关**:
         *   controller/Bluedroid 还在跑, bt_core_is_ready()(返回 s_hidh_init) 仍是 true,
         *   于是设置页 bt_is_on() 一直显示「开」, 用户点关掉了状态也变不回去。
         *   现在: 至少把射频停掉(省电+可观测), 并打 W 级日志 —— 关不掉时能一眼看到原因。 */
        ESP_LOGW(TAG, "esp_hidh_deinit 未成功(%s) ⇒ 退回只关射频(保留栈结构, 下次 enable 复用)",
                 esp_err_to_name(hidh_ret));
        esp_bluedroid_disable();
        esp_bt_controller_disable();
        /* ★ 2026-11-23: 必须同步清 s_radio_on —— bt_core_is_ready() 返回的就是它。
         *   不清的话"射频已物理关闭"但对外仍报 ready, 页面(手柄页)会跳过"等栈就绪"
         *   直接 set_scan_params ⇒ ESP_ERR_INVALID_STATE (0x103), 扫描永远失败。 */
        s_radio_on  = false;
        ESP_LOGW(TAG, "已强制关闭射频 (controller/bluedroid disable), RAM 不归还但不再耗电/被扫到");
        s_conn_dev  = NULL;
        s_connected = false;
        return;   /* s_gap_inited/s_hidh_init 保持 true → 下次 enable 幂等复用, 不会废掉底层 */
    }
    s_conn_dev  = NULL;
    s_connected = false;

    /* 第三步: controller/Bluedroid 拆栈 */
#if defined(CONFIG_BT_BLUEDROID_ENABLED)
    esp_bt_controller_disable();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_deinit();
#else
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
#endif
    /* 第四步: 清 esp_hid_gap 静态信号量 (源自 IDF 例程、只提供 init, deinit 由本项目补齐) */
    esp_hid_gap_deinit();
    s_gap_inited = false;   /* 下次 bt_core_init 从全新状态完整初始化 */
    s_hidh_init = false;
    /* ★★★ 2026-11-23 关键修复 (深拆栈后"蓝牙再也开不起来") ★★★
     *   原来这里漏了 s_radio_on, 而 bt_core_is_ready() 恰恰返回它 ⇒ 拆栈后对外
     *   仍报"栈就绪": 手柄页因此跳过"等栈就绪"直接去 set_scan_params, 必然得到
     *   ESP_ERR_INVALID_STATE (0x103), 表现为"扫描一直失败/没有设备"(实测日志)。
     *   深拆栈的语义是"栈连影子都不剩", radio 标志必须一并归零。 */
    s_radio_on  = false;
    ESP_LOGI(TAG, "bt_core deinit: bluetooth stack released");
}

/* ============================================================================
 *  对外: 深度拆栈 —— 真正归还内部 RAM
 *  调用者: bt_manager 的"最后使用者退出"路径 (bt_manager_disable_release)。
 *  语义与兜底:
 *    · 成功 → controller/Bluedroid/esp_hidh 全拆, s_gap_inited/s_hidh_init 归零,
 *             下次 enable 走 bt_core_init 完整重建 (bt_manager_init 已处理这条路径);
 *    · 失败 → bt_core_full_deinit 内部已退回"只关射频"并保留栈结构,
 *             此时 RAM 不归还但功能不坏, 下一次 enable 幂等复用 (与改造前一致)。
 *  ⛔ 不要在"射频还在被扫描任务/连接任务使用"时调用 (看门狗/扫描会 UAF),
 *     bt_manager_disable_release 已保证先置 s_power_off 并断开。
 * ========================================================================== */
void bt_core_deinit_deep(void)
{
    bt_core_full_deinit();
}

bool bt_core_scan(int duration_sec)
{
    (void)duration_sec; /* esp_hid_scan 内部按 1 秒间隔, 这里固定 5 秒 */

    if (!s_radio_on) {
        /* 栈没起来时扫也是白扫 (esp_ble_gap_set_scan_params 会返回 INVALID_STATE) */
        ESP_LOGW(TAG, "射频未开启 ⇒ 拒绝扫描 (调用方应先 bt_core_init/radio_on)");
        return false;
    }
    /* ★ R2 单飞: 检查与占位必须在同一临界区 (scan_begin 内部完成) */
    if (!scan_begin("任务式扫描")) {
        return false;
    }
    /* 生命周期计数先加: 闸门放开前它就代表"还有一个扫描任务活着" */
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_task_alive++;
    taskEXIT_CRITICAL(&s_scan_mux);

    s_scanning = true;
    BaseType_t ok = xTaskCreate(scan_task, "bt_scan", 6 * 1024, NULL, 2, &s_scan_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "扫描任务创建失败(内部 RAM 不足?) ⇒ 放弃本次扫描");
        s_scanning = false;
        scan_release();          /* 顺带把 alive 减回去 */
        s_scan_task = NULL;
        return false;
    }
    return true;
}

/* 射频是否正在扫: 以"有线程卡在 esp_hid_scan 里"为唯一真相源。
 * ⚠ 不要退回 s_scanning —— 那个只是"用户想要结果"的意图标志, 上层 stop 之后
 *   射频其实还在扫, 拿它判断会让调用方以为可以安全拆栈/并发起扫。 */
bool bt_core_is_scanning(void)
{
    return s_scan_busy;
}

bool bt_core_scan_busy(void)
{
    return s_scan_busy || s_scan_task_alive > 0;
}

/* ★ 2026-11-23 R3: 占用扫描闸门的是不是"界面自己要结果"的那轮扫描。
 *   bt_core_scan() 起的任务会走 scan_task → 结束后回调 on_scan_done(结果交给上层);
 *   而"按名字短扫"(回连用)直接用 esp_hid_scan, 结果自己消费, **不会**回调上层。
 *   上层必须能区分这两者: 前者可以"复用并等回调", 后者只能等它 5s 退出后重扫,
 *   否则界面会停在一个永远不会来的扫描完成回调上。
 *   判据用 s_scan_task_alive(只有 bt_core_scan 会加) —— 不看 s_scanning, 它只是意图标志。 */
bool bt_core_ui_scan_running(void)
{
    bool running;
    taskENTER_CRITICAL(&s_scan_mux);
    running = (s_scan_task_alive > 0);
    taskEXIT_CRITICAL(&s_scan_mux);
    return running;
}

/* ★ 2026-11-23 R3: 是否**真的有**连接事务卡在 esp_hidh_dev_open 里。
 *   这是"射频被连接流程占着"的唯一真相源 —— 扫不开就是因为它, 而不是因为
 *   "连接任务对象还存在"(任务可能只是建好了还没进 open, 或正在收尾边缘)。
 *   用它替代上层对 s_conn_task_alive 的猜测: 计数只表达"有任务", 不表达"在占射频"。 */
bool bt_core_open_busy(void)
{
    return s_open_tx > 0;
}

bool bt_core_scan_wait_idle(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (s_scan_busy || s_scan_task_alive > 0) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return true;
}

void bt_core_stop_scan(void)
{
    /* esp_hid_scan 是阻塞式, 没有"中途停止"的公开 API: 这里做两件事 ——
     *   ① 清"想要结果"的意图标志 (上层据此不再等回调);
     *   ② **请求控制器停扫** (esp_ble_gap_stop_scanning): 让扫描任务尽快从
     *      esp_hid_scan 里返回, 而不是继续白扫满 5s。用户点选设备后要立刻连接,
     *      原来只能靠 page_gamepad 里"先等 300ms"来躲这个冲突。
     * ⚠ 绝不能在这里清 s_scan_busy: 那等于放开闸门, 让第二个线程进 esp_hid_scan
     *   (正是 R2 要消除的并发)。闸门只在扫描任务真正退出时由 scan_release() 放开。 */
    s_scanning = false;
    if (s_scan_busy) {
        esp_err_t e = esp_ble_gap_stop_scanning();
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "stop_scan: 已请求停止扫描 (等扫描任务退出后闸门才放开)");
        } else {
            ESP_LOGD(TAG, "stop_scan: 停扫请求返回 %s (可能尚未真正进入扫描态)", esp_err_to_name(e));
        }
    }
}

esp_hid_scan_result_t *bt_core_last_scan(void)
{
    return s_scan_results;
}

int bt_core_last_scan_count(void)
{
    return (int)s_scan_count;
}

bool bt_core_scan_find(const uint8_t bd_addr[6], esp_hid_scan_result_t *out)
{
    if (!bd_addr || !out) return false;
    bool found = false;
    scan_list_lock();
    for (esp_hid_scan_result_t *r = s_scan_results; r; r = r->next) {
        if (memcmp(r->bda, bd_addr, 6) == 0) {
            *out = *r;
            /* name 指向链表节点内的内存, 节点随后可能被释放 ⇒ 不外带 */
            out->name = NULL;
            out->next = NULL;
            found = true;
            break;
        }
    }
    scan_list_unlock();
    return found;
}

/* ★ 2026-11-23 R3: 按需重建 esp_hidh 层 (只在"真要连接"时做, 不在开蓝牙路径上做)。
 *   背景: 轻量关射频(radio_off)在 bluedroid disable 之前会 esp_hidh_deinit(),
 *   否则 BTA 清空 gattc app 表后 hid_gattc_if 变成失效值, 每次 open 都是
 *   `bta_gattc_process_api_open Failed, unknown client_if: N`。
 *   重建要重新注册 gattc app 才能拿到新 client_if; 它内部是无限等注册事件,
 *   所以只能放在连接路径(有看门狗兜底), 绝不能放在 enable/开蓝牙路径。 */
static bool bt_core_hidh_ensure(void)
{
    if (s_hidh_init) return true;
    if (!s_gap_inited) return false;
    esp_err_t er = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "重建 esp_hidh: gattc 回调注册失败 (%s)", esp_err_to_name(er));
        return false;
    }
    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    esp_err_t eh = esp_hidh_init(&config);
    if (eh == ESP_ERR_INVALID_STATE) {
        /* 上一次 deinit 没做成(设备对象未摘掉) ⇒ esp_hidh 内部信号量还在。
         * 先补一次 deinit 再建, 否则永久 "Already initialized" (连接全废)。 */
        ESP_LOGW(TAG, "esp_hidh 残留初始化状态 ⇒ 补 deinit 后重建");
        (void)esp_hidh_deinit();
        eh = esp_hidh_init(&config);
    }
    if (eh != ESP_OK) {
        ESP_LOGE(TAG, "重建 esp_hidh 失败 (%s)", esp_err_to_name(eh));
        return false;
    }
    s_hidh_init = true;
    ESP_LOGI(TAG, "esp_hidh 已重建 (gattc client_if 重新注册)");
    return true;
}

bool bt_core_connect(const esp_hid_scan_result_t *r)
{
    if (!r) {
        return false;
    }
    /* 轻量关过蓝牙 ⇒ esp_hidh 已被撤掉, 连接前必须先重建 (拿新的 client_if) */
    if (!bt_core_hidh_ensure()) {
        ESP_LOGE(TAG, "esp_hidh 未就绪 ⇒ 本次连接放弃 (不会卡住调用方)");
        return false;
    }
    /* ★ R2: 登记"连接事务在飞"。esp_hidh_dev_open 返回前, 控制器的 GAP 状态机
     *   正处在建链路/发现服务阶段 —— 此时起扫描会抢状态机, 也会让 open 撞
     *   INVALID_STATE。bt_core_scan() 靠这个计数拒扫 (连接事务期间禁止冲突扫描)。 */
    open_tx_enter();
    /* esp_hidh_dev_open 的 bda 形参不是 const ⇒ 显式去 const 说明"我们不会改它" */
    esp_hidh_dev_t *dev = esp_hidh_dev_open((uint8_t *)r->bda, r->transport, r->ble.addr_type);
    open_tx_leave();
    if (!dev) {
        ESP_LOGD(TAG, "open failed");
        return false;
    }
    return true;
}

/* V1.6.x: 按"名字"连接 —— 某些手柄(如 "Q36 for Android")的 BLE 地址**每次会话都会漂移**,
 * 按历史里存的旧地址直连必然失败(日志: bt_core open failed / BLE_HIDH OPEN failed 0x85)。
 * 做法: 起一次**阻塞式短扫(5s)**, 在结果里按名字找到它"当前"的地址再连。
 * 返回 true 表示已发起连接(真正是否连上由 OPEN 事件收敛)。
 * 注意: 调用方需确保当前没有别的扫描在跑(否则会与页面扫描抢蓝牙)。 */
bool bt_core_connect_by_name(const char *name)
{
    if (!name || !name[0]) return false;
    if (!scan_begin("按名字连接")) return false;

    size_t n = 0;
    esp_hid_scan_result_t *res = NULL;
    esp_err_t err = esp_hid_scan(5, &n, &res);
    if (err != ESP_OK || !res) {
        if (res) esp_hid_scan_results_free(res);
        scan_release();
        ESP_LOGD(TAG, "按名字重解析[%s]: 扫描失败(%s)", name, esp_err_to_name(err));
        return false;
    }

    bool ok = false;
    for (esp_hid_scan_result_t *r = res; r; r = r->next) {
        if (r->name && strncmp(r->name, name, 32) == 0) {
            ESP_LOGI(TAG, "按名字命中[%s]: 当前地址 %02x:%02x:%02x:%02x:%02x:%02x (历史里的旧地址已失效)",
                     name, r->bda[0], r->bda[1], r->bda[2], r->bda[3], r->bda[4], r->bda[5]);
            ok = bt_core_connect(r);
            break;
        }
    }
    if (!ok) ESP_LOGD(TAG, "按名字重解析[%s]: 扫到 %u 条结果但无同名设备", name, (unsigned)n);
    esp_hid_scan_results_free(res);
    /* 放开闸门要在 open 之后: open 期间仍属于"连接事务", 此时不该让新扫描插进来 */
    scan_release();
    return ok;
}

/* ★ 2026-11-23: 与上面同一次"按名字短扫", 但**只把命中的地址拷出来, 不在这里 open**。
 * 为什么需要: 自动回连任务若直接调 bt_core_connect(), esp_hidh_dev_open 会阻塞到
 * GATTC 收敛(手柄不在时实测 30s), 把 4s 看门狗和"窗口内多轮尝试"全部废掉。
 * 交给调用方 bt_connect_async() → 独立任务里 open。 */
bool bt_core_scan_find_by_name(const char *name, esp_hid_scan_result_t *out)
{
    if (!name || !name[0] || !out) return false;
    /* ★ R2: 短扫同样占扫描闸门 —— 否则它会和页面扫描/另一个短扫同时进
     *   esp_hid_scan, 互相 free 静态结果链表 (UAF) 并抢同一个完成信号量。
     *   被占用时**直接返回 false**, 不等待: 调用方 (回连任务) 下一轮再试,
     *   绝不在这里阻塞住回连节奏。 */
    if (!scan_begin("按名字短扫")) return false;

    size_t n = 0;
    esp_hid_scan_result_t *res = NULL;
    esp_err_t err = esp_hid_scan(5, &n, &res);
    if (err != ESP_OK || !res) {
        if (res) esp_hid_scan_results_free(res);
        scan_release();
        ESP_LOGD(TAG, "按名字短扫[%s]: 扫描失败(%s)", name, esp_err_to_name(err));
        return false;
    }

    bool ok = false;
    for (esp_hid_scan_result_t *r = res; r; r = r->next) {
        if (r->name && strncmp(r->name, name, 32) == 0) {
            *out = *r;
            out->name = NULL;   /* 结果链表马上释放, 不能留悬空指针 (连接不用 name) */
            out->next = NULL;
            ESP_LOGI(TAG, "按名字短扫命中[%s]: 当前地址 %02x:%02x:%02x:%02x:%02x:%02x",
                     name, out->bda[0], out->bda[1], out->bda[2],
                     out->bda[3], out->bda[4], out->bda[5]);
            ok = true;
            break;
        }
    }
    if (!ok) ESP_LOGD(TAG, "按名字短扫[%s]: 扫到 %u 条结果但无同名设备", name, (unsigned)n);
    esp_hid_scan_results_free(res);
    scan_release();
    return ok;
}

void bt_core_disconnect(void)
{
    if (s_conn_dev) {
        esp_hidh_dev_close(s_conn_dev);
    }
}

/* ★ 2026-10-18: 失败/超时后的收尾。
 *   只 close s_conn_dev 不够 —— 失败路径上 s_conn_dev 是 NULL（open 没成功过），
 *   而 Bluedroid 侧可能还挂着一次 pending create-conn 或半开链路：
 *   下次 esp_hidh_dev_open() 就会撞 "Already Connected" / INVALID_STATE，
 *   表现为"失败一次之后永远连不上，只能重启"。
 *   ⇒ 这里对 bda 主动发一次 GAP disconnect，把链路彻底撤掉。 */
void bt_core_cancel_pending(const uint8_t bd_addr[6])
{
    if (s_conn_dev) {
        esp_hidh_dev_close(s_conn_dev);
        s_conn_dev = NULL;
        s_connected = false;
    }
    if (!bd_addr) return;
    esp_err_t err = esp_ble_gap_disconnect((uint8_t *)bd_addr);
    ESP_LOGD(TAG, "cancel_pending %02x:%02x:%02x:%02x:%02x:%02x -> %s",
             bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5],
             esp_err_to_name(err));
}

/* 删除指定对端地址的 BLE 绑定(配对密钥)。
 * 用途: 某些手柄(如 "Q36 for Android")的 BLE 地址会**每次会话漂移**(同名不同址),
 * 旧绑定会按老地址一直残留在 NVS 里, 造成"新地址要重新配对、而手柄侧以为自己已绑定"
 * 的状态不一致 —— 表现为手柄主动断开(HCI rsn=0x13)、随后拒绝连接(GATT 0x85)、
 * 且**手柄自己的指示灯不显示已连接**。清掉旧绑定让双方回到干净状态重新配对。 */
bool bt_core_remove_bond(const uint8_t bd_addr[6])
{
    if (!bd_addr) return false;
    esp_err_t err = esp_ble_remove_bond_device((uint8_t *)bd_addr);
    ESP_LOGD(TAG, "remove_bond %02x:%02x:%02x:%02x:%02x:%02x -> %s",
             bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5],
             esp_err_to_name(err));
    return (err == ESP_OK);
}

esp_hidh_dev_t *bt_core_connected_dev(void)
{
    return s_conn_dev;
}

bool bt_core_is_connected(void)
{
    return s_connected;
}