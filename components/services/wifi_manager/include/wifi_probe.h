/**
 * wifi_probe.h — Wi-Fi 万用表/嗅探库 (ESP-IDF 原生).
 *
 * 功能类目 (对应 ESP32-TOOLS 提炼):
 *   1. 全信道主动扫描  (AP 清单: SSID/BSSID/信道/RSSI/加密类型, 含隐藏SSID)
 *   2. 信道拥挤度/信号热力图  (遍历 1..14 信道统计 beacon/探针密度 + 平均RSSI)
 *   3. Beacon 被动抓取
 *   4. 探针请求嗅探 + 隐藏 SSID 揭示  (probe request 中携带的 SSID)
 *   5. 主动攻击模块 (deauth / beacon flood) — 仅预留接口, S3 默认不启用.
 *
 * 设计: 与系统 wifi_manager(STA 连网) 相互独立. 本库自管一套 promiscuous
 * 原始帧回调 + 信道跳频任务, 不进任何目标网络, 不破坏已连的 WiFi.
 * 缓冲一律放 PSRAM (EXT_RAM_BSS_ATTR), 不占内部 RAM.
 */
#ifndef WIFI_PROBE_H
#define WIFI_PROBE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"   /* wifi_ap_record_t */

#ifdef __cplusplus
extern "C" {
#endif

#define WP_MAX_AP        24     /* 主动扫描 AP 上限 */
#define WP_MAX_PROBE     24     /* 去重探针请求(客户端在找的SSID) 上限 */
#define WP_CH_COUNT      14     /* 2.4G 信道 1..14 */

/* 单条探针请求 (客户端在广播里请求/寻找的 SSID) */
typedef struct {
    char     ssid[33];
    uint8_t  sa[6];       /* 发起方 MAC */
    uint8_t  channel;
    int8_t   rssi;
    uint32_t count;       /* 捕获次数 */
} wp_probe_t;

/* 单信道统计 (被动监听) */
typedef struct {
    uint32_t beacon;      /* beacon 帧数 */
    uint32_t probe;       /* probe request 帧数 */
    int      rssi_sum;    /* 累加 rssi */
    uint32_t rssi_n;      /* 采样数 */
} wp_ch_stat_t;

/* 主动攻击结果 (预留接口回传状态) */
typedef enum {
    WP_ATK_DEAUTH = 0,     /* 802.11 deauth 干扰 */
    WP_ATK_BEACON,         /* 伪造 beacon 洪水 */
} wp_atk_t;

/* ============ 生命周期 ============ */
esp_err_t wifi_probe_init(void);      /* 幂等初始化 (注册 promiscuous 回调) */
esp_err_t wifi_probe_deinit(void);

/* ============ 主动扫描 (非阻塞启动; 结果经 wifi_manager 事件异步刷新) ============ */
esp_err_t wifi_probe_scan(void);      /* 全 14 信道 active 扫描 (含隐藏SSID), 启动后立即返回 */
bool     wifi_probe_is_scan_done(void);
int      wifi_probe_ap_count(void);
const wifi_ap_record_t *wifi_probe_ap_record(int idx);   /* 原生记录 (含 ssid/rssi/channel/authmode) */

/* ============ 被动监听 (信道跳频, 需启动后才有数据) ============ */
esp_err_t wifi_probe_sniff_start(void);   /* 进入 promiscuous + 跳频任务 */
esp_err_t wifi_probe_sniff_stop(void);
bool     wifi_probe_sniff_running(void);
const wp_ch_stat_t *wifi_probe_ch_stat(void);   /* 返回 WP_CH_COUNT 数组 */
int      wifi_probe_probe_count(void);
const wp_probe_t *wifi_probe_probe_get(int idx);
/* 重置被动统计 (开始新一轮监听时清 0) */
void     wifi_probe_sniff_reset(void);

/* ============ 主动攻击 (预留接口; S3 默认返回 ESP_ERR_NOT_SUPPORTED) ============
 * 依赖 esp_wifi_80211_tx 原始帧注入. S3 法规固件可能限制, 是否可用需真机验证.
 * 通过 CONFIG_WIFI_PROBE_ENABLE_ATTACK=y 编译期启用实现占位. */
esp_err_t wifi_probe_attack_start(wp_atk_t kind, const uint8_t *target_bssid);
esp_err_t wifi_probe_attack_stop(void);
bool     wifi_probe_attack_active(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROBE_H */