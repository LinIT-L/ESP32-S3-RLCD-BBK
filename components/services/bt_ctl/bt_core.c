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

static bt_core_cbs_t s_cbs;
static bool s_hidh_init = false;   /* esp_hidh 是否已初始化 */
/* V1.5.x: esp_hid_gap/esp_hidh 底层已初始化 — ESP-IDF 无公开 deinit 接口
 * (esp_hid_gap_init 用内部信号量判重, 重复调用必报 "Already initialised").
 * 因此 disable→enable 循环必须复用底层, 只复位 s_hidh_init 逻辑标志. */
static bool s_gap_inited = false;
static bool s_scanning = false;
static bool s_connected = false;
static esp_hidh_dev_t *s_conn_dev = NULL;   /* 当前连接设备句柄 */

static TaskHandle_t s_scan_task = NULL;

/* ---------- esp_hidh 事件回调 ---------- */
static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            s_connected = true;
            s_conn_dev = param->open.dev;
            ESP_LOGI(TAG, "OPEN: %s", esp_hidh_dev_name_get(param->open.dev));
            if (s_cbs.on_connected) {
                s_cbs.on_connected(true, param->open.dev);
            }
        } else {
            ESP_LOGD(TAG, "OPEN failed!");
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
        esp_hid_scan_results_free(s_scan_results);
        s_scan_results = NULL;
        s_scan_count = 0;
    }

    ESP_LOGI(TAG, "scan start...");
    esp_err_t err = esp_hid_scan((uint32_t)5, &results_len, &results);
    if (err == ESP_OK) {
        s_scan_results = results;
        s_scan_count = results_len;
        ESP_LOGI(TAG, "scan done: %u results", (unsigned)results_len);
    } else {
        ESP_LOGD(TAG, "scan failed: %s", esp_err_to_name(err));
        if (results) {
            esp_hid_scan_results_free(results);
        }
    }

    s_scanning = false;
    if (s_cbs.on_scan_done) {
        s_cbs.on_scan_done();
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
        s_hidh_init = true;
        ESP_LOGI(TAG, "bt_core already initialised (idempotent), reuse bottom layer");
        return true;
    }
    ESP_LOGI(TAG, "bt_core_init enter");

    esp_err_t err = esp_hid_gap_init(HIDH_BLE_MODE);
    if (err != ESP_OK) {
        /* 兜底: 若上层异常路径残留了 esp_hid_gap 静态状态(信号量未清),
         * esp_hid_gap_init 会报 "Already initialised"。复位后重试一次即可恢复,
         * 避免蓝牙永久卡"初始化中"(实测 bug, 见 bt_core_deinit 注释)。 */
        ESP_LOGD(TAG, "esp_hid_gap_init failed (%s), 复位残留状态后重试", esp_err_to_name(err));
        esp_hid_gap_deinit();
        err = esp_hid_gap_init(HIDH_BLE_MODE);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "HID host init...");
    /* 顺序与官方 esp_hid_host 例程一致: 先注册 gattc 回调, 再 esp_hidh_init.
     * esp_hidh_init 内部会注册 BLE GATT 事件, 需 gattc 回调先就绪. */
    err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "gattc register failed: %s", esp_err_to_name(err));
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
        ESP_LOGD(TAG, "esp_hidh_init failed (%s), 复位残留状态后重试", esp_err_to_name(err));
        esp_hidh_deinit();
        err = esp_hidh_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
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
            ESP_LOGD(TAG, "本机BLE地址: %02x:%02x:%02x:%02x:%02x:%02x (手柄据此识别主机)",
                     oa[0], oa[1], oa[2], oa[3], oa[4], oa[5]);
        }
        int bn = esp_ble_get_bond_device_num();
        ESP_LOGD(TAG, "本地已保存绑定数: %d", bn);
    }
    ESP_LOGI(TAG, "HID host init done (ready)");
    return true;
}

bool bt_core_is_ready(void)
{
    return s_hidh_init;
}

void bt_core_deinit(void)
{
    if (s_scan_results) {
        esp_hid_scan_results_free(s_scan_results);
        s_scan_results = NULL;
        s_scan_count = 0;
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

    /* 第二步: 关 esp_hidh 层(带重试)。**它失败就绝不能继续拆 Bluedroid** ——
     * 否则会留下"hidh 已废、底层已拆"的不可恢复状态(只能重启)。此时放弃本次拆栈,
     * 保留 BT 栈以保功能(代价: 蓝牙关着也占约 67KB 内部 RAM), 下次 enable 走幂等复用。 */
    esp_err_t hidh_ret = esp_hidh_deinit();
    for (int i = 0; i < 5 && hidh_ret != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(60));
        hidh_ret = esp_hidh_deinit();
    }
    if (hidh_ret != ESP_OK) {
        ESP_LOGD(TAG, "esp_hidh_deinit 未成功(%s), 放弃本次拆栈以保蓝牙可用", esp_err_to_name(hidh_ret));
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
    ESP_LOGI(TAG, "bt_core deinit: bluetooth stack released");
}

void bt_core_scan(int duration_sec)
{
    if (s_scanning) {
        return;
    }
    (void)duration_sec; /* esp_hid_scan 内部按 1 秒间隔, 这里固定 5 秒 */

    s_scanning = true;
    BaseType_t ok = xTaskCreate(scan_task, "bt_scan", 6 * 1024, NULL, 2, &s_scan_task);
    if (ok != pdPASS) {
        ESP_LOGD(TAG, "failed to create scan task");
        s_scanning = false;
    }
}

bool bt_core_is_scanning(void)
{
    return s_scanning;
}

void bt_core_stop_scan(void)
{
    /* esp_hid_scan 阻塞式, 无中途停止 API; 实际停止由扫描任务自然结束 */
    s_scanning = false;
}

esp_hid_scan_result_t *bt_core_last_scan(void)
{
    return s_scan_results;
}

int bt_core_last_scan_count(void)
{
    return (int)s_scan_count;
}

bool bt_core_connect(const esp_hid_scan_result_t *r)
{
    if (!r) {
        return false;
    }
    esp_hidh_dev_t *dev = esp_hidh_dev_open(r->bda, r->transport, r->ble.addr_type);
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

    size_t n = 0;
    esp_hid_scan_result_t *res = NULL;
    esp_err_t err = esp_hid_scan(5, &n, &res);
    if (err != ESP_OK || !res) {
        if (res) esp_hid_scan_results_free(res);
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
    return ok;
}

void bt_core_disconnect(void)
{
    if (s_conn_dev) {
        esp_hidh_dev_close(s_conn_dev);
    }
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