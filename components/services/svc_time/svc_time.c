// svc_time — 时间/校时统一服务
// 收拢: main.c 的东八区时区 + 默认时间兜底, wifi_manager 的每日 SNTP 自动校时。
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "svc_nvs.h"
#include "svc_time.h"

static const char *TAG = "SVC_TIME";

/* ---- 每日首次联网自动校时: NVS 记上次校时日期 (YYYYMMDD). 当天重复连接不重校. ---- */
#define NTP_NS   "os_ntp"
#define NTP_KEY  "last_day"

static int32_t ntp_day_now(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (int32_t)(tm.tm_year + 1900) * 10000 + (int32_t)(tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static void ntp_save_day(int32_t d)
{
    nvs_handle_t h;
    if (svc_nvs_open(NTP_NS, NVS_READWRITE, &h) == ESP_OK) { svc_nvs_set_i32(h, NTP_KEY, d); svc_nvs_commit(h); svc_nvs_close(h); }
}

static int32_t ntp_load_day(void)
{
    int32_t d = 0; nvs_handle_t h;
    if (svc_nvs_open(NTP_NS, NVS_READONLY, &h) == ESP_OK) { svc_nvs_get_i32(h, NTP_KEY, &d); svc_nvs_close(h); }
    return d;
}

static void ntp_sync_cb(struct timeval *tv) { (void)tv; ntp_save_day(ntp_day_now()); }   /* 校时成功: 记当天 */

static bool s_ntp_inited = false;   /* 本次开机 NTP 是否已初始化 */

void svc_time_ntp_sync(void)
{
    if (s_ntp_inited || ntp_load_day() == ntp_day_now()) return;   /* 已校过/当天已校 */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);   /* 校时成功回调 (init 前设置) */
    if (esp_netif_sntp_init(&cfg) != ESP_OK) { ESP_LOGW(TAG, "NTP 初始化失败"); return; }
    esp_netif_sntp_start();
    s_ntp_inited = true;
    ESP_LOGI(TAG, "NTP 校时已启动 (每天首次联网)");
}

void svc_time_init(void)
{
    /* 统一北京时间时区 (东八区): 状态栏/日期/校时后显示都用 localtime → 北京时间 */
    setenv("TZ", "UTC-8", 1);
    tzset();

    /* 默认时间 2026-09-01: 设备未联网校时(无真实时间, 早于2025)时用该默认值做日期兜底;
     * 联网后 svc_time_ntp_sync 的 SNTP 会自动校时覆盖 (今天的真实时间晚于该默认, 故不冲突). */
    time_t t0 = time(NULL);
    if (t0 < 1735689600) {   /* 2025-01-01 00:00 UTC 之前 = 未校时/初始 1970 */
        struct timeval tv = { 1788220800, 0 };   /* 2026-09-01 00:00 UTC */
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "未联网校时, 设默认时间 2026-09-01");
    }
}
