/* V1.0.46: Wi-Fi 连网功能 (STA 模式) — 连接路由器, 配置保存到 NVS */
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#define TAG "WIFI_MANAGER"
#define NVS_NS   "wifi_cfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"

static bool s_inited = false;    /* esp_wifi_init 已调用 */
static bool s_enabled = false;   /* esp_wifi_start 已成功 */
static bool s_connecting = false;
static bool s_connected = false;
static char s_status[48] = "未连接";
static char s_ssid[33] = "";
static char s_pass[65] = "";

/* V1.0.46: 扫描结果 */
#define MAX_AP 16
static wifi_ap_record_t s_ap[MAX_AP];
static int s_ap_count = 0;
static bool s_scan_done = false;
static bool s_connected_just_now = false;  /* 刚连接成功事件 (菜单提示用) */
static bool s_ntp_started = false;         /* V1.0.67: NTP 校时是否已启动 */

/* ---------- NVS 配置持久化 ---------- */
static void save_cfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, KEY_SSID, s_ssid);
    nvs_set_str(h, KEY_PASS, s_pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Wi-Fi 配置已保存: %s", s_ssid);
}

static void load_cfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_ssid);
    if (nvs_get_str(h, KEY_SSID, s_ssid, &len) != ESP_OK) s_ssid[0] = '\0';
    len = sizeof(s_pass);
    if (nvs_get_str(h, KEY_PASS, s_pass, &len) != ESP_OK) s_pass[0] = '\0';
    nvs_close(h);
    if (s_ssid[0]) {
        ESP_LOGI(TAG, "已加载保存的 Wi-Fi 配置: %s", s_ssid);
    }
}

void wifi_manager_get_saved(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    snprintf(ssid, ssid_sz, "%s", s_ssid);
    snprintf(pass, pass_sz, "%s", s_pass);
}

/* ---------- 事件处理 ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* WiFi 启动后, 如果有保存的配置则发起连接 */
        if (s_ssid[0]) {
            ESP_LOGI(TAG, "STA 启动, 连接 %s ...", s_ssid);
            s_connecting = true;
            snprintf(s_status, sizeof(s_status), "连接中...");
            esp_wifi_connect();
        } else {
            snprintf(s_status, sizeof(s_status), "未配置");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_connecting) {
            s_connecting = false;
            snprintf(s_status, sizeof(s_status), "连接失败");
            ESP_LOGW(TAG, "Wi-Fi 连接失败");
        } else {
            snprintf(s_status, sizeof(s_status), "已断开");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_connecting = false;
        s_connected_just_now = true;
        char ipbuf[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, ipbuf, sizeof(ipbuf));
        snprintf(s_status, sizeof(s_status), "已连接 %s", ipbuf);
        ESP_LOGI(TAG, "Wi-Fi 已连接: %s", s_status);
        /* V1.0.67: 连上 WiFi 后启动一次 NTP 校时 (异步, 自动 settimeofday) */
        if (!s_ntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&cfg) == ESP_OK) {
                esp_netif_sntp_start();
                s_ntp_started = true;
                ESP_LOGI(TAG, "NTP 校时已启动");
            } else {
                ESP_LOGW(TAG, "NTP 初始化失败");
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        /* 扫描完成: 取回 AP 列表 */
        s_scan_done = true;
        uint16_t num = MAX_AP;
        if (esp_wifi_scan_get_ap_records(&num, s_ap) == ESP_OK) {
            s_ap_count = num;
        } else {
            s_ap_count = 0;
        }
        /* 按信号强度排序 (简单插入排序) */
        for (int i = 1; i < s_ap_count; i++) {
            wifi_ap_record_t tmp = s_ap[i];
            int j = i - 1;
            while (j >= 0 && s_ap[j].rssi < tmp.rssi) {
                s_ap[j + 1] = s_ap[j];
                j--;
            }
            s_ap[j + 1] = tmp;
        }
        ESP_LOGI(TAG, "扫描完成: %d 个网络", s_ap_count);
    }
}

/* V1.0.46: 扫描附近的 Wi-Fi 网络 */
bool wifi_manager_scan_start(void) {
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;
    if (!s_enabled) {
        if (!wifi_manager_enable()) return false;
    }
    s_scan_done = false;
    s_ap_count = 0;
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t ret = esp_wifi_scan_start(&cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s (s_enabled=%d)", esp_err_to_name(ret), s_enabled);
        return false;
    }
    ESP_LOGI(TAG, "扫描已启动 (active 100-300ms)");
    return true;
}

void wifi_manager_scan_stop(void) {
    esp_wifi_scan_stop();
    s_scan_done = true;   /* 停止后视为完成, 避免轮询卡"扫描中" */
    ESP_LOGI(TAG, "扫描已停止");
}

bool wifi_manager_is_scan_done(void) { return s_scan_done; }
int  wifi_manager_get_scan_count(void) { return s_ap_count; }

bool wifi_manager_get_scan_ssid(int idx, char *out, size_t sz) {
    if (idx < 0 || idx >= s_ap_count || !out || sz == 0) return false;
    snprintf(out, sz, "%s", s_ap[idx].ssid);
    return true;
}

void wifi_manager_init(void) {
    if (s_inited) return;

    load_cfg();

    ESP_LOGI(TAG, "初始化 WiFi 子系统 (STA)...");
    esp_netif_init();
    /* V1.0.67: 防重复创建 (网页手柄 AP 可能已先初始化过 esp_wifi/esp_netif) */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(ret));
        return;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_inited = true;
    ESP_LOGI(TAG, "WiFi 子系统初始化完成 (STA)");
}

bool wifi_manager_enable(void) {
    if (s_enabled) return true;
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    /* V1.0.67: 网页手柄可能切过 AP 模式, 这里强制回 STA */
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 启动失败");
        return false;
    }
    s_enabled = true;
    ESP_LOGI(TAG, "WiFi 射频已启动");
    return true;
}

bool wifi_manager_disable(void) {
    if (!s_enabled) return true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_enabled = false;
    s_connected = false;
    s_connecting = false;
    snprintf(s_status, sizeof(s_status), "未连接");
    ESP_LOGI(TAG, "WiFi 已关闭");
    return true;
}

bool wifi_manager_is_enabled(void) { return s_enabled; }
bool wifi_manager_is_connected(void) { return s_connected; }

bool wifi_manager_consume_connected_event(void) {
    bool v = s_connected_just_now;
    s_connected_just_now = false;
    return v;
}
const char *wifi_manager_get_status(void) { return s_status; }

bool wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    save_cfg();

    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    if (!s_enabled) {
        if (esp_wifi_start() != ESP_OK) return false;
        s_enabled = true;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_disconnect();  /* 清除旧连接状态 */
    s_connecting = true;
    snprintf(s_status, sizeof(s_status), "连接中...");
    if (esp_wifi_connect() != ESP_OK) {
        s_connecting = false;
        snprintf(s_status, sizeof(s_status), "连接失败");
        return false;
    }
    ESP_LOGI(TAG, "正在连接 %s ...", s_ssid);
    return true;
}
