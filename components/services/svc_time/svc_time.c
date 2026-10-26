// svc_time — 时间/校时统一服务
// 收拢: main.c 的东八区时区 + 默认时间兜底, wifi_manager 的联网 SNTP 自动校时。
//
// ★ 2026-10-26 linit: **删除 time_cal_task（走时校准）**。
//   原任务每 10s 用 esp_timer 纠一次 RTC 漂移(内部 RC 约 1%)。按 linit 决定去掉,
//   时间完全由 NTP 提供。
// ★ 2026-10-26 linit: **每次联网都校一次**(原来"当天只校一次")。
//   每次拿到 IP(wifi_manager 的 IP_EVENT_STA_GOT_IP) 都让它重新发一次 SNTP 请求:
//   首次调用走 esp_netif_sntp_init(), 之后每次走 esp_netif_sntp_start()
//   (它的语义就是 sntp_stop()+sntp_init() ⇒ 立刻重发查询, 服务器列表保留)。
//   这样"关屏保→WiFi 关→解锁→WiFi 重连"也会自动补校一次, 正好弥补 RTC 漂移。
//   10 秒内重复到达的事件会被忽略(防重复 GOT_IP 抖动连打 NTP)。
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "svc_time.h"

static const char *TAG = "SVC_TIME";

#define NTP_SERVER          "pool.ntp.org"
#define NTP_MIN_GAP_US      (10 * 1000 * 1000)   /* 10s 内重复触发不重校 */

static bool     s_ntp_inited   = false;   /* 本次开机 SNTP 是否已初始化 */
static int64_t  s_last_req_us  = 0;       /* 上次发起校时的时刻 (esp_timer) */
static uint32_t s_sync_count   = 0;       /* 本次开机校时成功次数 (诊断用) */
static time_t   s_last_sync_t  = 0;       /* 上次校时成功的 epoch */

/* 校时成功回调: 只打日志 + 记状态, 系统时间由 SNTP 自己 settimeofday */
static void ntp_sync_cb(struct timeval *tv)
{
    time_t t = (tv && tv->tv_sec) ? tv->tv_sec : time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    s_sync_count++;
    s_last_sync_t = t;
    ESP_LOGI(TAG, "校时成功 #%u: %04d-%02d-%02d %02d:%02d:%02d (北京时间)",
             (unsigned)s_sync_count,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void svc_time_ntp_sync(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_req_us && (now - s_last_req_us) < NTP_MIN_GAP_US) return;   /* 抖动保护 */

    if (!s_ntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
        cfg.sync_cb      = ntp_sync_cb;   /* 成功回调 (init 时一次性设置) */
        cfg.wait_for_sync = false;        /* 不等待: 没人调 sync_wait, 省一个信号量 */
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "NTP 初始化失败, 下次联网再试");
            return;
        }
        s_ntp_inited  = true;
        s_last_req_us = now;
        ESP_LOGI(TAG, "NTP 已启动 (本次联网校时)");
        return;
    }

    /* 已初始化: 重启 SNTP ⇒ sntp_stop()+sntp_init(), 立刻重发一次查询 (每次联网都校) */
    if (esp_netif_sntp_start() == ESP_OK) {
        s_last_req_us = now;
        ESP_LOGI(TAG, "NTP 重新校时 (本次联网, 已成功 %u 次)", (unsigned)s_sync_count);
    } else {
        ESP_LOGW(TAG, "NTP 重启失败");
    }
}

void svc_time_ntp_status(uint32_t *count, long *last_epoch)
{
    if (count)      *count      = s_sync_count;
    if (last_epoch) *last_epoch = (long)s_last_sync_t;
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
