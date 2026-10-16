/**
 * os_wifi.c — 系统无线探测 API 实现 (包装 wifi_probe + 管家 WiFi 服务).
 */
#include "os_wifi.h"
#include "os_hw.h"
#include "wifi_probe.h"

/* 临时占用 WiFi: 进入强制打开, 退出按**进入前的状态**还原
 * (linit 2026-10-14: 原来是开就继续开, 原来是关才关掉)。 */
static os_hw_lease_t s_wifi_lease;

esp_err_t os_wifi_probe_init(void) {
    os_hw_lease_acquire(&s_wifi_lease, OS_HW_WIFI);   /* 进入即开 (原本已开则不重复计数) */
    return wifi_probe_init();
}

esp_err_t os_wifi_probe_deinit(void) {
    esp_err_t e = wifi_probe_deinit();
    os_hw_lease_release(&s_wifi_lease, OS_HW_WIFI);   /* 只有本次由我们开的才关 */
    return e;
}

esp_err_t os_wifi_scan(void)                       { return wifi_probe_scan(); }
bool     os_wifi_is_scan_done(void)                { return wifi_probe_is_scan_done(); }
int      os_wifi_ap_count(void)                    { return wifi_probe_ap_count(); }
const wifi_ap_record_t *os_wifi_ap_record(int idx) { return wifi_probe_ap_record(idx); }
esp_err_t os_wifi_sniff_start(void)                { return wifi_probe_sniff_start(); }
esp_err_t os_wifi_sniff_stop(void)                 { return wifi_probe_sniff_stop(); }
bool     os_wifi_sniff_running(void)               { return wifi_probe_sniff_running(); }
const wp_ch_stat_t *os_wifi_ch_stat(void)          { return wifi_probe_ch_stat(); }
int      os_wifi_probe_count(void)                 { return wifi_probe_probe_count(); }
const wp_probe_t *os_wifi_probe_get(int idx)       { return wifi_probe_probe_get(idx); }
void     os_wifi_sniff_reset(void)                 { wifi_probe_sniff_reset(); }
esp_err_t os_wifi_attack_start(wp_atk_t kind, const uint8_t *target_bssid) { return wifi_probe_attack_start(kind, target_bssid); }
esp_err_t os_wifi_attack_stop(void)                { return wifi_probe_attack_stop(); }
bool     os_wifi_attack_active(void)               { return wifi_probe_attack_active(); }
