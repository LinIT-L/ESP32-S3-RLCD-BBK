/**
 * os_wifi.h — 系统无线探测 API (Wi-Fi 扫描/热力图/探针嗅探).
 *
 * 对 wifi_probe 子系统的系统级封装: 进入即向管家申请 WiFi 硬件
 * (os_hw_request(OS_HW_WIFI)), 退出即释放 (os_hw_release), 与蓝牙/USB-HID
 * 共用同一套按需启停框架. 应用只调 os_wifi_*, 不直接 include wifi_probe.h,
 * 也不直接初始化/反初始化 WiFi, 由系统统一调度 (避免多应用抢 WiFi 状态).
 */
#ifndef OS_WIFI_H
#define OS_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "wifi_probe.h"   /* wifi_ap_record_t / wp_probe_t / wp_ch_stat_t / WP_* */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t os_wifi_probe_init(void);      /* = os_hw_request(WIFI) + wifi_probe_init */
esp_err_t os_wifi_probe_deinit(void);    /* = wifi_probe_deinit + os_hw_release(WIFI) */
esp_err_t os_wifi_scan(void);
bool     os_wifi_is_scan_done(void);
int      os_wifi_ap_count(void);
const wifi_ap_record_t *os_wifi_ap_record(int idx);
esp_err_t os_wifi_sniff_start(void);
esp_err_t os_wifi_sniff_stop(void);
bool     os_wifi_sniff_running(void);
const wp_ch_stat_t *os_wifi_ch_stat(void);
int      os_wifi_probe_count(void);
const wp_probe_t *os_wifi_probe_get(int idx);
void     os_wifi_sniff_reset(void);
esp_err_t os_wifi_attack_start(wp_atk_t kind, const uint8_t *target_bssid);
esp_err_t os_wifi_attack_stop(void);
bool     os_wifi_attack_active(void);

#ifdef __cplusplus
}
#endif
#endif /* OS_WIFI_H */
