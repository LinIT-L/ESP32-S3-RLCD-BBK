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
#include "espnow_proto.h"
#include "espnow_peer.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"
#include "wifi_probe.h"

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
static bool               s_evt_reg = false;   /* WiFi 事件监听是否已注册 */

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
    if (!data || len <= 0 || len > ESPNOW_MAX_PAYLOAD || !info || !info->src_addr) {
        s_st.rx_drop++;
        return;
    }
    const int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    s_st.last_rssi = rssi;

    /* ① 解帧 + CRC 校验 (见 espnow_proto.h)。不是本协议的包 / 坏包直接丢,
     *    绝不能拿去喂 Lua 应用 —— 同信道里可能有别人的 ESP-NOW 流量。 */
    espnow_frame_t f;
    if (!espnow_proto_parse(data, len, &f)) {
        s_st.rx_bad++;
        return;
    }

    /* ② 控制帧(发现/配对)内部消化, 不进应用队列、不打扰 Lua。
     *    ⚠ espnow_peer_on_frame 内部只动内存 + 异步投递应答帧, 不写 NVS。 */
    if (f.type != ESPNOW_FT_DATA) {
        s_st.rx_ctrl++;
        espnow_peer_on_frame(f.type, f.seq, f.payload, f.len, info->src_addr, rssi);
        return;
    }

    /* ③ 应用数据帧: 只把 payload 搬进环形队列 */
    if (!s_q || f.len <= 0) { s_st.rx_drop++; return; }

    int next = (s_head + 1) % RX_SLOTS;
    if (next == s_tail) { s_st.rx_drop++; return; }   /* 队列满 */

    rx_slot_t *slot = &s_q[s_head];
    memcpy(slot->mac, info->src_addr, ESPNOW_MAC_LEN);
    slot->len  = (uint16_t)f.len;
    slot->rssi = rssi;
    memcpy(slot->data, f.payload, (size_t)f.len);
    s_head = next;
    s_st.rx_ok++;
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) s_st.tx_ok++;
    else                                s_st.tx_fail++;
}

/* WiFi 被关掉(用户关闭 WiFi / 省电关机)时 esp-now 随之失效: 驱动内部状态已被
 * 销毁, 再 send 只会返回错误 ⇒ 这里同步标记不可用并清运行时表, 免得上层以为还能用。
 * ⚠ 事件回调里**不写 NVS**(flash 操作 + cache 关闭期 PSRAM 不可访问的坑)。 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_STOP) return;
    if (!s_on) return;
    ESP_LOGW(TAG, "WiFi 已停止 ⇒ ESP-NOW 标记为不可用（下次 start 重建）");
    s_on = false;
    s_bcast_added = false;
    s_head = s_tail = 0;
    espnow_peer_reset();
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

    /* ①b 信道独占协调: ESP-NOW 收发必须在**固定信道**上, 而 wifi_probe 的跳频嗅探
     *     会不停切信道(还会顺带切掉已连接的 AP) ⇒ 用 LinTOS 自己的服务接口让它让路,
     *     不直接碰驱动。注包攻击期间直接拒绝启动(它也在抢信道, 且状态不可控)。 */
    if (wifi_probe_attack_active()) {
        ESP_LOGW(TAG, "注包攻击进行中 ⇒ 暂不启动 ESP-NOW (信道被独占)");
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_probe_sniff_running()) {
        ESP_LOGW(TAG, "跳频嗅探运行中 ⇒ 停止嗅探, 让出固定信道给 ESP-NOW");
        wifi_probe_sniff_stop();
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
        /* ★ 2026-11-22: 上面 esp_now_init 已经成功, 失败返回前要把它退掉,
         *   否则下次 start 会在"已初始化"状态上再 init, 且常驻一份 esp-now 内部状态。 */
        esp_now_deinit();
        return e;
    }
    esp_now_register_send_cb(on_send);

    s_head = s_tail = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_bcast_added = false;
    s_on = true;

    /* ③ 配对表: 必须在 s_on 之后再载入 —— persist_load() 会把已配对设备
     *    重新 add_peer 回驱动, 而它用 espnow_link_is_running() 判是否重建。 */
    espnow_peer_init();

    /* ④ WiFi 生命周期: 用户关 WiFi / 省电关机时同步失效 (只在第一次注册) */
    if (!s_evt_reg) {
        if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                                       on_wifi_event, NULL) == ESP_OK) {
            s_evt_reg = true;
        } else {
            ESP_LOGW(TAG, "WiFi 事件监听注册失败 ⇒ 关 WiFi 后状态可能滞后");
        }
    }

    uint8_t mac[6] = {0};
    espnow_link_self_mac(mac);
    ESP_LOGI(TAG, "已启动: MAC=%02X:%02X:%02X:%02X:%02X:%02X 信道=%u 已配对=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)espnow_link_channel(), espnow_peer_paired_count());
    return ESP_OK;
}

void espnow_link_stop(void)
{
    /* 先落盘: 正常任务上下文里写 NVS 才安全 (WiFi 事件路径不会走到这里) */
    espnow_peer_persist();
    if (!s_on) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    s_on = false;
    s_bcast_added = false;
    s_head = s_tail = 0;
    espnow_peer_reset();
    ESP_LOGI(TAG, "已停止 (WiFi 不动, 交给 wifi_manager)");
}

bool espnow_link_is_running(void) { return s_on; }

/* peer 表满了: 淘汰一个"未配对、未加密、非广播"的临时 peer 再重试。
 * 为什么需要: 驱动总 peer 上限 20 (ESP_NOW_MAX_TOTAL_PEER_NUM), 而设备发现在
 * 收广播时会不断自动建临时 peer (官方示例同机制), 不回收就会把额度吃满,
 * 之后真正要配对时 add_peer 直接失败。 */
static bool evict_temp_peer(void)
{
    static const uint8_t bcast[ESPNOW_MAC_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_now_peer_info_t pi;
    bool from_head = true;
    for (int i = 0; i < ESP_NOW_MAX_TOTAL_PEER_NUM; i++) {
        memset(&pi, 0, sizeof(pi));
        if (esp_now_fetch_peer(from_head, &pi) != ESP_OK) break;
        from_head = false;
        if (memcmp(pi.peer_addr, bcast, ESPNOW_MAC_LEN) == 0) continue;
        if (pi.encrypt) continue;                          /* 加密 peer 有独立额度 */
        if (espnow_peer_is_paired(pi.peer_addr)) continue;  /* 已配对的不淘汰 */
        if (esp_now_del_peer(pi.peer_addr) == ESP_OK) {
            ESP_LOGD(TAG, "peer 满 ⇒ 回收临时项 %02X:%02X:%02X:%02X:%02X:%02X",
                     pi.peer_addr[0], pi.peer_addr[1], pi.peer_addr[2],
                     pi.peer_addr[3], pi.peer_addr[4], pi.peer_addr[5]);
            return true;
        }
    }
    return false;
}

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
    if (e == ESP_ERR_ESPNOW_FULL && evict_temp_peer()) {
        e = esp_now_add_peer(&p);             /* 回收一个临时项后重试一次 */
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

/* ---- 帧发送: 所有出口都过这里, 保证"发出去的一定是本协议帧" ---- */

/* 发送完整帧 (已封装)。目标非广播且 peer 不存在时自动补建 —— 这条来自 IDF 官方
 * 示例(收到广播后回单播必须先 add_peer), 否则发送会返回 ESP_ERR_ESPNOW_NOT_FOUND。 */
esp_err_t espnow_link_send_frame(const uint8_t mac[ESPNOW_MAC_LEN],
                                 const uint8_t *frame, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!mac || !frame || len <= 0 || len > ESPNOW_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;

    static const uint8_t bcast[ESPNOW_MAC_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (memcmp(mac, bcast, ESPNOW_MAC_LEN) != 0 && !esp_now_is_peer_exist(mac)) {
        esp_err_t e = espnow_link_add_peer(mac, NULL);
        if (e != ESP_OK) return e;
    }

    esp_err_t e = esp_now_send(mac, frame, (size_t)len);
    if (e != ESP_OK) {
        s_st.tx_fail++;
        ESP_LOGD(TAG, "send 失败: %s", esp_err_to_name(e));
    }
    return e;
}

static esp_err_t bcast_raw(const uint8_t *frame, int len)
{
    static const uint8_t bcast[ESPNOW_MAC_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!s_bcast_added) {
        /* 广播对等体只能加一次; 加密模式下广播必须是非加密对等体 */
        espnow_link_add_peer(bcast, NULL);
        s_bcast_added = true;
    }
    return espnow_link_send_frame(bcast, frame, len);
}

esp_err_t espnow_link_broadcast_frame(uint8_t type, const void *payload, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (len < 0 || len > ESPNOW_APP_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    int n = espnow_proto_pack(buf, sizeof(buf), type,
                              espnow_proto_next_seq(type), payload, len);
    if (n <= 0) return ESP_ERR_INVALID_ARG;
    return bcast_raw(buf, n);
}

esp_err_t espnow_link_send(const uint8_t mac[ESPNOW_MAC_LEN], const void *data, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!mac || !data || len <= 0 || len > ESPNOW_APP_MAX) return ESP_ERR_INVALID_ARG;

    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    int n = espnow_proto_pack(buf, sizeof(buf), ESPNOW_FT_DATA,
                              espnow_proto_next_seq(ESPNOW_FT_DATA), data, len);
    if (n <= 0) return ESP_ERR_INVALID_ARG;
    return espnow_link_send_frame(mac, buf, n);
}

esp_err_t espnow_link_broadcast(const void *data, int len)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!data || len <= 0 || len > ESPNOW_APP_MAX) return ESP_ERR_INVALID_ARG;

    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    int n = espnow_proto_pack(buf, sizeof(buf), ESPNOW_FT_DATA,
                              espnow_proto_next_seq(ESPNOW_FT_DATA), data, len);
    if (n <= 0) return ESP_ERR_INVALID_ARG;
    return bcast_raw(buf, n);
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
