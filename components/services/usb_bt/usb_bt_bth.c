/*
 * SPDX-FileCopyrightText: 2020-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 (移植适配)
 *
 * USB-BT 蓝牙适配器的 HCI 桥接: USB BTH 端点 <-> esp_vhci (蓝牙控制器虚接口).
 * 移植自 esp-iot-solution usb_dongle 示例的 tusb_bth.c.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_bt.h"
#include "tinyusb.h"
#include "tusb_config.h"
#include "class/bth/bth_device.h"

#define TAG "usb_bth"

/* HCI 消息类型 (H4) */
#define HCIT_TYPE_COMMAND   1
#define HCIT_TYPE_ACL_DATA  2
#define HCIT_TYPE_SCO_DATA  3
#define HCIT_TYPE_EVENT     4

/* ACL 数据分片跟踪 */
typedef struct acl_data {
    bool is_new_pkt;
    uint16_t pkt_total_len;
    uint16_t pkt_cur_offset;
    uint8_t *pkt_val;
} acl_data_t;

static SemaphoreHandle_t evt_sem = NULL;
static SemaphoreHandle_t acl_sem = NULL;

static acl_data_t acl_tx_data = {
    .is_new_pkt = true,
    .pkt_total_len = 0,
    .pkt_cur_offset = 0,
    .pkt_val = NULL
};

/* BT 控制器回调: 上电完成, 检查 esp_vhci_host_check_send_available 即可 */
static void controller_rcv_pkt_ready(void)
{
}

/* BT 控制器回调: 控制器产生数据发往电脑 */
static int host_rcv_pkt(uint8_t *data, uint16_t len)
{
    uint16_t act_len = len - 1;
    uint8_t type = data[0];
    uint8_t *rp = NULL;

    switch (type) {
    case HCIT_TYPE_ACL_DATA:
        rp = (uint8_t *)malloc(act_len);
        if (!rp) return 0;
        memcpy(rp, data + 1, act_len);
        tud_bt_acl_data_send(rp, act_len);
        xSemaphoreTake(acl_sem, portMAX_DELAY);
        free(rp);
        break;
    case HCIT_TYPE_EVENT:
        rp = (uint8_t *)malloc(act_len);
        if (!rp) return 0;
        memcpy(rp, data + 1, act_len);
        tud_bt_event_send(rp, act_len);
        xSemaphoreTake(evt_sem, portMAX_DELAY);
        free(rp);
        break;
    default:
        ESP_LOGW(TAG, "Unknown HCI type 0x%02x", type);
        break;
    }
    return 0;
}

/* TinyUSB: 电脑下发 HCI 命令 -> 给控制器 */
void tud_bt_hci_cmd_cb(void *hci_cmd, size_t cmd_len)
{
    uint8_t *tx = (uint8_t *)malloc(cmd_len + 1);
    if (!tx) return;
    tx[0] = HCIT_TYPE_COMMAND;
    memcpy(tx + 1, hci_cmd, cmd_len);
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    esp_vhci_host_send_packet(tx, cmd_len + 1);
    free(tx);
}

/* TinyUSB: 电脑下发 ACL 数据 -> 给控制器 (处理分片) */
void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len)
{
    if (acl_tx_data.is_new_pkt) {
        acl_tx_data.pkt_total_len = *(((uint16_t *)acl_data) + 1) + 4;
        acl_tx_data.pkt_cur_offset = 0;
        acl_tx_data.pkt_val = (uint8_t *)malloc(acl_tx_data.pkt_total_len + 1);
        if (!acl_tx_data.pkt_val) return;
        memset(acl_tx_data.pkt_val, 0, acl_tx_data.pkt_total_len + 1);
        acl_tx_data.pkt_val[0] = HCIT_TYPE_ACL_DATA;
        acl_tx_data.pkt_cur_offset++;
        memcpy(acl_tx_data.pkt_val + acl_tx_data.pkt_cur_offset, acl_data, data_len);
        acl_tx_data.pkt_cur_offset += data_len;
        if (data_len < acl_tx_data.pkt_total_len) {
            acl_tx_data.is_new_pkt = false;
            return;
        }
        while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
        esp_vhci_host_send_packet(acl_tx_data.pkt_val, data_len + 1);
        goto reset_params;
    } else {
        memcpy(acl_tx_data.pkt_val + acl_tx_data.pkt_cur_offset, acl_data, data_len);
        acl_tx_data.pkt_cur_offset += data_len;
        if ((acl_tx_data.pkt_cur_offset - 1) == acl_tx_data.pkt_total_len) {
            while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
            esp_vhci_host_send_packet(acl_tx_data.pkt_val, acl_tx_data.pkt_total_len + 1);
            goto reset_params;
        }
    }
    return;
reset_params:
    acl_tx_data.is_new_pkt = true;
    acl_tx_data.pkt_total_len = 0;
    acl_tx_data.pkt_cur_offset = 0;
    if (acl_tx_data.pkt_val) { free(acl_tx_data.pkt_val); acl_tx_data.pkt_val = NULL; }
}

void tud_bt_event_sent_cb(uint16_t sent_bytes)
{
    (void)sent_bytes;
    if (evt_sem) xSemaphoreGive(evt_sem);
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes)
{
    (void)sent_bytes;
    if (acl_sem) xSemaphoreGive(acl_sem);
}

static esp_vhci_host_callback_t vhci_host_cb = {
    .notify_host_send_available = controller_rcv_pkt_ready,
    .notify_host_recv = host_rcv_pkt,
};

/* 初始化纯蓝牙控制器(不启 Bluedroid), 由 esp_vhci 桥接到 USB BTH.
 * 前提: 手柄蓝牙(esp_hid_gap/Bluedroid)未初始化 (由 main 依 NVS 标记跳过). */
void tusb_bth_init(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(TAG, "release classic bt memory failed");
        return;
    }
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed");
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed");
        return;
    }
    evt_sem = xSemaphoreCreateBinary();
    acl_sem = xSemaphoreCreateBinary();
    if (!evt_sem || !acl_sem) {
        ESP_LOGE(TAG, "sem create failed");
        return;
    }
    esp_vhci_host_register_callback(&vhci_host_cb);
    ESP_LOGI(TAG, "USB-BT 控制器就绪 (BLE HCI 桥接)");
}

/* 关闭 BTH: 停掉纯蓝牙控制器 (esp_vhci 桥), 供后续恢复手柄蓝牙.
 * 与 tusb_bth_init 对应, 需在切回 USB Serial/JTAG 之前调用. */
void tusb_bth_deinit(void)
{
    if (evt_sem) { vSemaphoreDelete(evt_sem); evt_sem = NULL; }
    if (acl_sem) { vSemaphoreDelete(acl_sem); acl_sem = NULL; }
    if (acl_tx_data.pkt_val) { free(acl_tx_data.pkt_val); acl_tx_data.pkt_val = NULL; }
    /* 清掉 vhci 回调, 避免残留指向已释放的 sem */
    esp_vhci_host_callback_t empty = { .notify_host_send_available = NULL, .notify_host_recv = NULL };
    esp_vhci_host_register_callback(&empty);
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "USB-BT 控制器已关闭");
}