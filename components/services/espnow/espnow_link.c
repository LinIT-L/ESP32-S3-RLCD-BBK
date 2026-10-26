/* ============================================================================
 * espnow_link.c — ESP-NOW 点对点/广播 (见 include/espnow_link.h 的约束说明)
 *
 * 设计要点:
 *   · 收包回调只做"搬进环形队列", 不解析、不打印、不 malloc (WiFi 任务上下文);
 *   · 队列放 **PSRAM** (内部 RAM 紧张, 这里只是 memcpy 缓冲);
 *   · 所有 API 都加锁, 因为发送可能来自 UI 任务、收包来自 WiFi 任务;
 *   · 不碰 esp_wifi_init/start —— 射频归 wifi_manager 管, 本模块只借。
 * ==========================================================================*/
#include <string.h>
#include "espnow_link.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

static const char *TAG = "ESPNOW";

#define RX_SLOTS 12

typedef struct {
    uint8_t mac[ESPNOW_MAC_LEN];
    uint16_t len;
    int8_t   rssi;
    uint8_t  data[ESPNOW_MAX_PAYLOAD];
} rx_slot_t;

static bool               s_on = false;
static SemaphoreHandle_t  s_lock = NULL;
static rx_slot_t         *s_q = NULL;        /* PSRAM 环形队列 */
static volatile int       s_head = 0, s_tail = 0;
static espnow_link_stats_t s_st;
static bool               s_bcast_added = false;

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_q) {
        s_q = (rx_slot_t *)heap_caps_malloc(sizeof(rx_slot_t) * RX_SLOTS, MALLOC_CAP_SPIRAM);
        if (!s_q) {   /* PSRAM 不可用时退回内部 RAM (仍比没有强) */
            s_q = (rx_slot_t *)malloc(sizeof(rx_slot_t) * RX_SLOTS);
            ESP_LOGW(TAG, "PSRAM 分配失败, 收包队列退回内部 RAM (%u B)",
                     (unsigned)(sizeof(rx_slot_t) * RX_SLOTS));
        }
        s_head = s_tail = 0;
    }
}

/* ---- 收包回调: WiFi 任务上下文, 只搬数据 ---- */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!data || len <= 0 || len > ESPNOW_MAX_PAYLOAD) return;
    if (!s_q) { s_st.rx_drop++; return; }

    int next = (s_head + 1) % RX_SLOTS;
    if (next == s_tail) { s_st.rx_drop++; return; }   /* 队列满 */

    rx_slot_t *slot = &s_q[s_head];
    if (info && info->src_addr) memcpy(slot->mac, info->src_addr, ESPNOW_MAC_LEN);
    else memset(slot->mac, 0, ESPNOW_MAC_LEN);
    slot->len  = (uint16_t)len;
    slot->rssi = info && info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    memcpy(slot->data, data, (size_t)len);
    s_head = next;
    s_st.rx_ok++;
    s_st.last_rssi = slot->rssi;
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) s_st.tx_ok++;
    else                                s_st.tx_fail++;
}

esp_err_t espnow_link_start(void)
{
    ensure_lock();
    if (s_on) return ESP_OK;

    /* ① 射频: 交给 wifi_manager 拉 (幂等; 没配网络也能起, 只是不联网)
     * ⚠ wifi_manager_enable() 返回的是 **bool**(true=成功), 不是 esp_err_t ——
     *   拿它和 ESP_OK(0) 比会把"成功"判成失败。 */
    if (!wifi_manager_enable()) {
        ESP_LOGW(TAG, "WiFi 射频启动失败 ⇒ ESP-NOW 不可用");
        return ESP_FAIL;
    }
    esp_err_t e = ESP_OK;

    /* ② esp-now 本体。若已被别的模块 init 过, esp_now_init 会返回错误码而不是
     *    真正的失败 ⇒ **不拿它的返回值判定**, 用后面注册回调的返回值来判可用性。 */
    e = esp_now_init();
    if (e != ESP_OK)
        ESP_LOGW(TAG, "esp_now_init: %s (可能是重复初始化, 继续试探)", esp_err_to_name(e));

    e = esp_now_register_recv_cb(on_recv);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "注册收包回调失败: %s ⇒ esp-now 不可用", esp_err_to_name(e));
        return e;
    }
    esp_now_register_send_cb(on_send);

    s_head = s_tail = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_bcast_added = false;
    s_on = true;

    uint8_t mac[6] = {0};
    espnow_link_self_mac(mac);
    ESP_LOGI(TAG, "已启动: MAC=%02X:%02X:%02X:%02X:%02X:%02X 信道=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)espnow_link_channel());
    return ESP_OK;
}

void espnow_link_stop(void)
{
    if (!s_on) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    s_on = false;
    s_bcast_added = false;
    s_head = s_tail = 0;
    ESP_LOGI(TAG, "已停止 (WiFi 不动, 交给 wifi_manager)");
}

bool espnow_link_is_running(void) { return s_on; }

esp_err_t espnow_link_add_peer(const uint8_t mac[ESPNOW_MAC_LEN], const uint8_t *lmk)
{
    if (!s_on || !mac) return ESP_ERR_INVALID_STATE;
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, ESPNOW_MAC_LEN);
    p.channel = 0;                 /* 0 = 跟随当前信道 (连了 AP 就跟着 AP) */
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = (lmk != NULL);
    if (lmk) memcpy(p.lmk, lmk, ESPNOW_LMK_LEN);
    esp_err_t e = esp_now_add_peer(&p);
    if (e == ESP_ERR_ESPNOW_EXIST) {          /* 已存在: 先删后加, 让新 lmk 生效 */
        esp_now_del_peer(mac);
        e = esp_now_add_peer(&p);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "add_peer 失败: %s", esp_err_to_name(e));
    return e;
}

esp_err_t espnow_link_del_peer(const uint8_t mac[ESPNOW_MAC_LEN])
{
    if (!s_on || !mac) return ESP_ERR_INVALID_STATE;
    return esp_now_del_peer(mac);
}

int espnow_link_peer_count(void)
{
    if (!s_on) return 0;
    esp_now_peer_num_t n;
    if (esp_now_get_peer_num(&n) != ESP_OK) return 0;
    return n.total_num;
}

esp_err_t espnow_link_send(const uint8_t mac[ESPNOW_MAC_LEN], const void *data, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!mac || !data || len <= 0 || len > ESPNOW_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;
    esp_err_t e = esp_now_send(mac, (const uint8_t *)data, (size_t)len);
    if (e != ESP_OK) {
        s_st.tx_fail++;
        ESP_LOGD(TAG, "send 失败: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t espnow_link_broadcast(const void *data, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!s_bcast_added) {
        /* 广播对等体只能加一次; 加密模式下广播必须是非加密对等体 */
        espnow_link_add_peer(bcast, NULL);
        s_bcast_added = true;
    }
    return espnow_link_send(bcast, data, len);
}

esp_err_t espnow_link_self_mac(uint8_t out[ESPNOW_MAC_LEN])
{
    if (!out) return ESP_ERR_INVALID_ARG;
    return esp_wifi_get_mac(WIFI_IF_STA, out);
}

uint8_t espnow_link_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&primary, &sec) != ESP_OK) return 0;
    return primary;
}

esp_err_t espnow_link_set_channel(uint8_t ch)
{
    if (ch < 1 || ch > 13) return ESP_ERR_INVALID_ARG;
    return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

int espnow_link_recv(uint8_t mac[ESPNOW_MAC_LEN], void *out, int cap)
{
    if (!out || cap <= 0) return -1;
    if (!s_lock || s_tail == s_head) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    if (s_tail == s_head) { xSemaphoreGive(s_lock); return 0; }
    rx_slot_t *slot = &s_q[s_tail];
    int n = slot->len;
    if (n > cap) n = cap;
    if (mac) memcpy(mac, slot->mac, ESPNOW_MAC_LEN);
    memcpy(out, slot->data, (size_t)n);
    s_tail = (s_tail + 1) % RX_SLOTS;
    xSemaphoreGive(s_lock);
    return n;
}

int espnow_link_pending(void)
{
    int h = s_head, t = s_tail;
    return (h >= t) ? (h - t) : (RX_SLOTS - t + h);
}

void espnow_link_get_stats(espnow_link_stats_t *out)
{
    if (out) *out = s_st;
}
