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
            ESP_LOGE(TAG, "OPEN failed!");
            if (s_cbs.on_connected) {
                s_cbs.on_connected(false, param->open.dev);
            }
        }
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        /* 打印原始 INPUT 报告, 排查"按键无映射" */
        ESP_LOGI(TAG, "INPUT rid=%u len=%u data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
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
        if (s_cbs.on_input) {
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
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
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
    ESP_LOGI(TAG, "bt_core_init enter");

    esp_err_t err = esp_hid_gap_init(HIDH_BLE_MODE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "HID host init...");
    /* 顺序与官方 esp_hid_host 例程一致: 先注册 gattc 回调, 再 esp_hidh_init.
     * esp_hidh_init 内部会注册 BLE GATT 事件, 需 gattc 回调先就绪. */
    err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc register failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_hidh_init = true;
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
    s_hidh_init = false;
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
        ESP_LOGE(TAG, "failed to create scan task");
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
        ESP_LOGE(TAG, "open failed");
        return false;
    }
    return true;
}

void bt_core_disconnect(void)
{
    if (s_conn_dev) {
        esp_hidh_dev_close(s_conn_dev);
    }
}

esp_hidh_dev_t *bt_core_connected_dev(void)
{
    return s_conn_dev;
}

bool bt_core_is_connected(void)
{
    return s_connected;
}