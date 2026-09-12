/* V1.0.46: Wi-Fi 连网功能 (STA 模式) — 连接路由器, 配置保存到 NVS */
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "WIFI_MANAGER"
#define NVS_NS   "wifi_cfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define NTP_NS   "os_ntp"     /* V1.7.x: 每天首次联网校时 (记上次校时日期) */
#define NTP_KEY  "last_day"

static bool s_inited = false;    /* esp_wifi_init 已调用 */
static bool s_enabled = false;   /* esp_wifi_start 已成功 */
static bool s_connecting = false;
static bool s_connected = false;
static char s_status[48] = "未连接";
static char s_ip[16] = "";            /* 已连接时纯 IP */
static char s_ssid[33] = "";
static char s_pass[65] = "";

/* ---- V1.7.x: 每天首次联网自动校时 (后台异步, 自动 settimeofday; 不影响使用) ----
 * NVS 记上次校时日期 (YYYYMMDD). 当天重复连接不重校, 跨天/掉电丢时间后自动重校. */
static int32_t ntp_day_now(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (int32_t)(tm.tm_year + 1900) * 10000 + (int32_t)(tm.tm_mon + 1) * 100 + tm.tm_mday;
}
static void ntp_save_day(int32_t d) {
    nvs_handle_t h;
    if (nvs_open(NTP_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, NTP_KEY, d); nvs_commit(h); nvs_close(h); }
}
static int32_t ntp_load_day(void) {
    int32_t d = 0; nvs_handle_t h;
    if (nvs_open(NTP_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_i32(h, NTP_KEY, &d); nvs_close(h); }
    return d;
}
static void ntp_sync_cb(struct timeval *tv) { (void)tv; ntp_save_day(ntp_day_now()); }   /* 校时成功: 记当天 */
static bool s_ntp_inited = false;   /* 本次开机 NTP 是否已初始化 */
static void ntp_maybe_sync(void) {
    if (s_ntp_inited || ntp_load_day() == ntp_day_now()) return;   /* 已校过/当天已校 */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);   /* 校时成功回调 (init 前设置) */
    if (esp_netif_sntp_init(&cfg) != ESP_OK) { ESP_LOGW(TAG, "NTP 初始化失败"); return; }
    esp_netif_sntp_start();
    s_ntp_inited = true;
    ESP_LOGI(TAG, "NTP 校时已启动 (每天首次联网)");
}

/* V1.0.46: 扫描结果 (放 PSRAM, 不占内部 RAM). 上限调高以配合 wifi_probe 万用表展示更多网络 */
#define MAX_AP 24
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_ap[MAX_AP];
static int s_ap_count = 0;
static bool s_scan_done = false;
static bool s_connected_just_now = false;  /* 刚连接成功事件 (菜单提示用) */
static bool s_suppress_reconnect = false;  /* 扫描期间禁止自动重连 (避免重连干扰扫描) */

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

/* 清空已保存的 SSID/密码 (用于"连接失败→清空重输") */
void wifi_manager_clear_saved(void){
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_SSID);
        nvs_erase_key(h, KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
    }
    s_ssid[0]=0; s_pass[0]=0; s_connected=false;
    s_status[0]=0; snprintf(s_status,sizeof(s_status),"未配置");
    ESP_LOGI(TAG, "Wi-Fi 已保存配置已清空, 请重新输入");
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
        /* 自愈: 有保存配置且未成功 → 隔1s重试, 最多5次 (应对ESP32-S3偶发握手超时).
         * 扫描期间抑制 (s_suppress_reconnect): 扫描前主动 disconnect 会触发此重连,
         * 重连 esp_wifi_connect 与扫描冲突又阻塞事件任务, 导致扫描长时间不出结果. */
        static int s_retry = 0;
        if (!s_suppress_reconnect && s_ssid[0] && s_retry < 5) {
            s_retry++;
            s_connecting = true;
            snprintf(s_status, sizeof(s_status), "连接中...");
            ESP_LOGW(TAG, "Wi-Fi 连接失败, %ds后重试 (%d/5)", 1, s_retry);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else {
            s_retry = 0;
            if (s_connecting) {
                s_connecting = false;
                snprintf(s_status, sizeof(s_status), "连接失败");
                ESP_LOGW(TAG, "Wi-Fi 连接失败");
            } else {
                snprintf(s_status, sizeof(s_status), "已断开");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_connecting = false;
        s_connected_just_now = true;
        char ipbuf[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, ipbuf, sizeof(ipbuf));
        snprintf(s_ip, sizeof(s_ip), "%s", ipbuf);
        snprintf(s_status, sizeof(s_status), "已连接 %s", ipbuf);
        ESP_LOGI(TAG, "Wi-Fi 已连接: %s", s_status);
        /* V1.7.x: 每天首次联网自动校时 (后台异步, 自动 settimeofday; 不影响使用).
         * NVS 记上次校时日期: 当天重复连接不重复校时, 跨天/掉电丢时间后自动重校. */
        ntp_maybe_sync();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        /* 扫描完成: 取回 AP 列表 */
        s_scan_done = true;
        s_suppress_reconnect = false;   /* 扫描结束, 恢复自动重连能力 */
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

/* V1.0.46: 扫描附近的 Wi-Fi 网络
 * V1.7.x: 统一扫描方式 (以网络分析为模板) — 刚开启 WiFi 后等射频稳定再扫,
 * 启动失败 (状态未就绪/忙) 自动延迟重试一次, 保证设置页"刚开 WiFi 立即扫描"也稳定快速. */
bool wifi_manager_scan_start(void) {
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;
    if (!s_enabled) {
        if (!wifi_manager_enable()) return false;
        vTaskDelay(pdMS_TO_TICKS(150));   /* 等射频稳定: 刚开启立即扫描会 NOT_STARTED/STATE */
    }
    /* 根因修复: 若 STA 正在"连接/连电源重试"保存的网, esp_wifi 不允许 scan
     * (返回 ESP_ERR_WIFI_STATE), 导致扫描卡死"正在扫描...". 扫描期间先抑制自动重连
     * (否则主动 disconnect 会触发重连 esp_wifi_connect, 与扫描冲突又阻塞事件任务,
     * 扫描长时间不出结果), 再强制断开一次让 WiFi 进入可扫描状态. */
    s_suppress_reconnect = true;
    if (s_connecting || !s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_scan_done = false;
    s_ap_count = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);   /* 保险: 扫描期间禁用省电, 联网速度优先 */
    /* 快速主动扫描: 缩短每信道驻留 (40-200ms), show_hidden=false 免去隐藏网被动监听,
     * 全 13 信道约 1~2s 完成 (此前 active 100-300ms + show_hidden 超慢). */
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 40, .max = 200 } },
    };
    esp_err_t ret = esp_wifi_scan_start(&cfg, false);
    if (ret != ESP_OK) {
        /* 状态未就绪/忙: 延迟重试一次 (刚开 WiFi 常见), 仍失败才返回 false */
        ESP_LOGW(TAG, "扫描启动失败: %s, 200ms 后重试", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(200));
        ret = esp_wifi_scan_start(&cfg, false);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s (s_enabled=%d)", esp_err_to_name(ret), s_enabled);
        s_suppress_reconnect = false;
        return false;
    }
    ESP_LOGI(TAG, "扫描已启动 (active 40-200ms)");
    return true;
}

void wifi_manager_scan_stop(void) {
    esp_wifi_scan_stop();
    s_scan_done = true;   /* 停止后视为完成, 避免轮询卡"扫描中" */
    s_suppress_reconnect = false;   /* 结束扫描, 恢复自动重连能力 */
    ESP_LOGI(TAG, "扫描已停止");
}

bool wifi_manager_is_scan_done(void) { return s_scan_done; }
int  wifi_manager_get_scan_count(void) { return s_ap_count; }

bool wifi_manager_get_scan_ssid(int idx, char *out, size_t sz) {
    if (idx < 0 || idx >= s_ap_count || !out || sz == 0) return false;
    snprintf(out, sz, "%s", s_ap[idx].ssid);
    return true;
}

const wifi_ap_record_t *wifi_manager_get_scan_records(void) {
    return s_ap_count > 0 ? s_ap : NULL;
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
    /* 关闭 WiFi 省电: 省电会间歇休眠射频, 导致扫描时快时慢/卡死在"扫描中".
     * 始终唤醒射频, 扫描才能稳定快速 (代价是待机功耗略增). */
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_enabled = true;
    ESP_LOGI(TAG, "WiFi 射频已启动 (省电已关闭)");
    return true;
}

bool wifi_manager_disable(void) {
    if (!s_enabled) return true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* V1.3.x: 完整释放 esp_wifi_init 分配的内部内存 (仅 stop 不释放, 开/关内存不回落).
     * 网页手柄 (web_gamepad) 直接初始化 esp_wifi AP 模式共享本子系统 —
     * 检测到 AP/APSTA 在用时不 deinit, 只停 STA 射频, 避免破坏手柄 AP. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
        esp_wifi_deinit();
        s_inited = false;
        ESP_LOGI(TAG, "WiFi 已 deinit (内部内存已释放)");
    }
    s_enabled = false;
    s_connected = false;
    s_connecting = false;
    snprintf(s_status, sizeof(s_status), "未连接");
    ESP_LOGI(TAG, "WiFi 已关闭");
    return true;
}

bool wifi_manager_is_enabled(void) { return s_enabled; }
bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_connecting(void) { return s_connecting; }

bool wifi_manager_consume_connected_event(void) {
    bool v = s_connected_just_now;
    s_connected_just_now = false;
    return v;
}
const char *wifi_manager_get_status(void) { return s_status; }

/* V1.6.x: 返回已连接 WiFi 的纯 IP 串 (未连接则为空串) */
bool wifi_manager_get_ip(char *out, size_t sz) {
    if (!out || sz == 0) return false;
    out[0] = '\0';
    if (!s_connected || !s_ip[0]) return false;
    snprintf(out, sz, "%s", s_ip);
    return true;
}

bool wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    save_cfg();

    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    if (!s_enabled) {
        if (esp_wifi_start() != ESP_OK) return false;
        esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电, 保证扫描/连接稳定 */
        s_enabled = true;
    }

    wifi_config_t cfg = { 0 };
    /* V1.3.x: 原 strncpy(sizeof-1) 对 32 字符 SSID/64 字符密码会截断且不补 NUL,
     * 在 -O2 (stringop-truncation) 下触发 Werror, 且驱动按 C 串读可能越界.
     * 改 memcpy 复制满尺寸-1 + 显式置 NUL, 保证驱动收到合法 C 串. */
    memcpy(cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = '\0';
    memcpy(cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.password[sizeof(cfg.sta.password) - 1] = '\0';
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
