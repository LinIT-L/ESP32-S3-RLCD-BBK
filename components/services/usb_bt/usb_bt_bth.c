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
    if (!data || len < 2) return 0;      /* len-1 是无符号运算: len=0 会变成 65535 */
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
        ESP_LOGD(TAG, "Unknown HCI type 0x%02x", type);
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

/* TinyUSB: 电脑下发 ACL 数据 -> 给控制器 (处理分片)
 * ★ 2026-11-22 加边界检查: 原来三处 memcpy 都用**对端声明的长度**当拷贝上限,
 *   而实际 data_len 与声明长度没有一致性校验 —— 电脑(或任何 USB 主机)发一个
 *   "头里写 8 字节、实际带 200 字节"的畸形包就能把 pkt_val 后面全冲掉。
 *   现在: 长度只认声明值, 多出来的丢弃; 续片以剩余空间为上限; 收满即发。 */
#define BTH_ACL_MAX_LEN 4096   /* 单包上限: 不设的话畸形头能让 malloc 申请 64KB */

void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len)
{
    uint8_t *in = (uint8_t *)acl_data;
    uint32_t total, cap, room;

    if (!acl_tx_data.is_new_pkt) {
        /* 续片 */
        if (!acl_tx_data.pkt_val) { acl_tx_data.is_new_pkt = true; return; }
        cap  = (uint32_t)acl_tx_data.pkt_total_len + 1u;      /* 含 1 字节 H4 类型 */
        room = (acl_tx_data.pkt_cur_offset < cap) ? (cap - acl_tx_data.pkt_cur_offset) : 0u;
        if (data_len > room) data_len = (uint16_t)room;        /* 超出声明长度的丢弃 */
        if (data_len) {
            memcpy(acl_tx_data.pkt_val + acl_tx_data.pkt_cur_offset, in, data_len);
            acl_tx_data.pkt_cur_offset += data_len;
        }
        if (acl_tx_data.pkt_cur_offset < cap) return;          /* 还没收全 */
        while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
        esp_vhci_host_send_packet(acl_tx_data.pkt_val, (uint16_t)cap);
        goto reset_params;
    }

    /* 新包首片: H4 头至少 4 字节 (handle 2B + 长度 2B) */
    if (!in || data_len < 4) return;
    uint16_t plen = 0;
    memcpy(&plen, in + 2, sizeof(plen));      /* 原写法 (uint16_t*)acl_data 是非对齐读 */
    total = (uint32_t)plen + 4u;
    if (total > BTH_ACL_MAX_LEN) {
        ESP_LOGW(TAG, "ACL 声明长度 %u 超限, 丢弃", (unsigned)total);
        return;
    }
    if (acl_tx_data.pkt_val) { free(acl_tx_data.pkt_val); acl_tx_data.pkt_val = NULL; }
    acl_tx_data.pkt_total_len  = (uint16_t)total;
    acl_tx_data.pkt_val = (uint8_t *)malloc(total + 1u);
    if (!acl_tx_data.pkt_val) { acl_tx_data.pkt_total_len = 0; return; }
    acl_tx_data.pkt_val[0] = HCIT_TYPE_ACL_DATA;
    acl_tx_data.pkt_cur_offset = 1;
    if (data_len > total) data_len = (uint16_t)total;          /* 首片不该超过声明长度 */
    memcpy(acl_tx_data.pkt_val + 1, in, data_len);
    acl_tx_data.pkt_cur_offset += data_len;
    if ((uint32_t)acl_tx_data.pkt_cur_offset < total + 1u) {   /* 还有续片 */
        acl_tx_data.is_new_pkt = false;
        return;
    }
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    esp_vhci_host_send_packet(acl_tx_data.pkt_val, (uint16_t)(total + 1u));
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
        ESP_LOGD(TAG, "release classic bt memory failed");
        return;
    }
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGD(TAG, "controller init failed");
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        ESP_LOGD(TAG, "controller enable failed");
        return;
    }
    evt_sem = xSemaphoreCreateBinary();
    acl_sem = xSemaphoreCreateBinary();
    if (!evt_sem || !acl_sem) {
        ESP_LOGD(TAG, "sem create failed");
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