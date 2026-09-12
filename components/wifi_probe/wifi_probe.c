/**
 * wifi_probe.c — Wi-Fi 万用表/嗅探库.
 *
 * 底层全部走 ESP-IDF 原生 API (esp_wifi):
 *   - promiscuous rx 回调 (esp_wifi_set_promiscuous_rx_cb) 抓原始 mgmt 帧
 *   - 主动扫描 (esp_wifi_scan_start / get_ap_records, show_hidden=1)
 *   - 信道跳频 (esp_wifi_set_channel)
 *   - 原始帧注入 (esp_wifi_80211_tx) — 仅预留, 见 ATTACK 段
 *
 * 与系统 wifi_manager 既有的 STA 连接共用射频, 但互不排他: 本库在"工具页"
 * 使用期间把射频切到监听跳频, 退出时交回. 不做任何联网/鉴权.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "wifi_manager.h"
#include "wifi_probe.h"

#define TAG "WIFI_PROBE"

/* 被动监听缓冲 (PSRAM) */
EXT_RAM_BSS_ATTR static wp_ch_stat_t s_ch[WP_CH_COUNT];
EXT_RAM_BSS_ATTR static wp_probe_t    s_probe[WP_MAX_PROBE];
static int s_probe_count = 0;

/* 状态 */
static bool s_inited = false;
static bool s_sniffing = false;
static bool s_attack_active = false;
static TaskHandle_t s_hop_task = NULL;
static volatile uint8_t s_cur_channel = 0;

/* ---------------- 802.11 管理帧解析 ----------------
 * promiscuous rx 返回完整 802.11 帧 (含 header, 无 FCS).
 * frame control 首字节: type=[7:6]=00 mgmt; subtype=[7:4]&0x0f 但实际在小端:
 *   实际 subtype 取 fc[0] 的低 4 位 (WIFI uses little-endian framing, fc0 的低4位是 subtype).
 */
typedef struct __attribute__((packed)) {
    uint8_t  fc[2];
    uint16_t duration;
    uint8_t  da[6];
    uint8_t  sa[6];
    uint8_t  bssid[6];
    uint16_t seq;
} wlan_mgmt_hdr_t;

#define IEEE_FC_TYPE(fc0)  ((fc0) >> 6)          /* 0=mgmt */
#define IEEE_FC_SUBTYPE(fc0) ((fc0) & 0x0f)
#define IEEE_TYPE_MGMT 0
#define IEEE_SUB_BEACON 0x8
#define IEEE_SUB_PROBEREQ 0x4

/* 解析 tagged parameters 中的 SSID element (id==0). 返回: >0 ok, 0 no ssid, -1 hidden(len 0) */
static int parse_ssid(const uint8_t *body, int body_len, char *out, int out_cap)
{
    int i = 0;
    while (i + 2 <= body_len) {
        int id = body[i];
        int len = body[i + 1];
        if (i + 2 + len > body_len) break;
        if (id == 0) {
            if (len == 0) return -1;                       /* 隐藏 SSID */
            int n = len; if (n >= out_cap) n = out_cap - 1;
            memcpy(out, &body[i + 2], n);
            out[n] = '\0';
            return n;
        }
        i += 2 + len;
    }
    return 0;
}

static void promisc_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < (int)sizeof(wlan_mgmt_hdr_t)) return;

    uint8_t fc0 = p[0];
    if (IEEE_FC_TYPE(fc0) != IEEE_TYPE_MGMT) return;
    uint8_t sub = IEEE_FC_SUBTYPE(fc0);

    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t chan = s_cur_channel; if (chan < 1 || chan > WP_CH_COUNT) chan = 1;
    wp_ch_stat_t *c = &s_ch[chan - 1];
    c->rssi_sum += rssi; c->rssi_n++;

    if (sub == IEEE_SUB_BEACON) {
        c->beacon++;
    } else if (sub == IEEE_SUB_PROBEREQ) {
        c->probe++;
        const uint8_t *body = p + sizeof(wlan_mgmt_hdr_t);
        int body_len = len - (int)sizeof(wlan_mgmt_hdr_t);
        char name[33]; int n = parse_ssid(body, body_len, name, sizeof(name));
        if (n <= 0) return;
        for (int i = 0; i < s_probe_count; i++) {
            if (strcmp(s_probe[i].ssid, name) == 0) { s_probe[i].count++; return; }
        }
        if (s_probe_count < WP_MAX_PROBE) {
            wp_probe_t *pr = &s_probe[s_probe_count];
            snprintf(pr->ssid, sizeof(pr->ssid), "%s", name);
            memcpy(pr->sa, &p[10], 6);      /* mgmt header SA 偏移 10 */
            pr->channel = chan;
            pr->rssi = rssi;
            pr->count = 1;
            s_probe_count++;
        }
    }
}

/* ---------------- 信道跳频任务 (被动监听) ---------------- */
static void hop_task(void *arg)
{
    int ch = 1;
    while (s_sniffing) {
        s_cur_channel = (uint8_t)ch;
        esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(200));
        ch = (ch >= WP_CH_COUNT) ? 1 : (ch + 1);
    }
    vTaskDelete(NULL);
}

/* ---------------- 对外 API ---------------- */

esp_err_t wifi_probe_init(void)
{
    if (s_inited) return ESP_OK;
    if (!wifi_manager_is_enabled()) {
        if (!wifi_manager_enable()) {
            ESP_LOGE(TAG, "无法启动 WiFi 射频");
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t err = esp_wifi_set_promiscuous_rx_cb(&promisc_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set promiscuous rx cb: %s", esp_err_to_name(err));
        return err;
    }
    memset(s_ch, 0, sizeof(s_ch));
    s_probe_count = 0;
    s_inited = true;
    ESP_LOGI(TAG, "wifi_probe init done");
    return ESP_OK;
}

esp_err_t wifi_probe_deinit(void)
{
    wifi_probe_sniff_stop();
    s_inited = false;
    return ESP_OK;
}

/* --- 主动扫描: 全 14 信道, show_hidden=1. 非阻塞: 仅启动, 结果经 wifi_manager 事件异步写入.
 * 结果由 wifi_manager 内部持有 (其 SCAN_DONE 处理已抽取一次, 此处不再重复抽取), 页面轮询读取. */
esp_err_t wifi_probe_scan(void)
{
    if (!wifi_manager_is_enabled()) {
        if (!wifi_manager_enable()) return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_scan_start()) {
        ESP_LOGE(TAG, "主动扫描启动失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool wifi_probe_is_scan_done(void) { return wifi_manager_is_scan_done(); }

int wifi_probe_ap_count(void) { return wifi_manager_get_scan_count(); }

const wifi_ap_record_t *wifi_probe_ap_record(int idx)
{
    const wifi_ap_record_t *recs = wifi_manager_get_scan_records();
    if (!recs) return NULL;
    if (idx < 0 || idx >= wifi_manager_get_scan_count()) return NULL;
    return &recs[idx];
}

/* --- 被动监听 --- */
esp_err_t wifi_probe_sniff_start(void)
{
    if (s_sniffing) return ESP_OK;
    if (!s_inited) {
        esp_err_t e = wifi_probe_init();
        if (e != ESP_OK) return e;
    }
    s_sniffing = true;
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) { s_sniffing = false; return err; }
    if (xTaskCreate(hop_task, "wp_hop", 3072, NULL, 4, &s_hop_task) != pdPASS) {
        s_sniffing = false;
        esp_wifi_set_promiscuous(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_probe_sniff_stop(void)
{
    if (!s_sniffing) return ESP_OK;
    s_sniffing = false;
    if (s_hop_task) { vTaskDelete(s_hop_task); s_hop_task = NULL; }
    esp_wifi_set_promiscuous(false);
    s_cur_channel = 0;
    return ESP_OK;
}

bool wifi_probe_sniff_running(void) { return s_sniffing; }

const wp_ch_stat_t *wifi_probe_ch_stat(void) { return s_ch; }

int wifi_probe_probe_count(void) { return s_probe_count; }

const wp_probe_t *wifi_probe_probe_get(int idx)
{
    if (idx < 0 || idx >= s_probe_count) return NULL;
    return &s_probe[idx];
}

void wifi_probe_sniff_reset(void)
{
    memset(s_ch, 0, sizeof(s_ch));
    s_probe_count = 0;
    memset(s_probe, 0, sizeof(s_probe));
}

/* --- 主动攻击 (预留接口; S3 法规固件默认禁用) --- */
esp_err_t wifi_probe_attack_start(wp_atk_t kind, const uint8_t *target_bssid)
{
    (void)kind; (void)target_bssid;
#ifdef CONFIG_WIFI_PROBE_ENABLE_ATTACK
    s_attack_active = true;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "主动攻击未启用 (S3 法规固件限制源帧注入); 已预留接口");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wifi_probe_attack_stop(void)
{
    s_attack_active = false;
    return ESP_OK;
}

bool wifi_probe_attack_active(void) { return s_attack_active; }