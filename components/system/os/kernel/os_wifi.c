/**
 * os_wifi.c — 系统无线探测 API 实现 (包装 wifi_probe + 管家 WiFi 服务).
 */
#include "os_wifi.h"
#include "os_hw.h"
#include "wifi_probe.h"

esp_err_t os_wifi_probe_init(void) {
    os_hw_request(OS_HW_WIFI);   /* 申请 WiFi 硬件 (失败不致命, 兼容原行为) */
    return wifi_probe_init();
}

esp_err_t os_wifi_probe_deinit(void) {
    esp_err_t e = wifi_probe_deinit();
    os_hw_release(OS_HW_WIFI);
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
