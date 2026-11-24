#pragma once

/* ============================================================================
 * espnow_peer — 设备发现 / 配对表 / 设备列表 (LinTOS 适配层)
 *
 * 这层做什么: 把 ESP-NOW 原生"只知道 MAC、必须先 add_peer"的裸接口, 补齐成
 *   一个能用的设备层 —— 发现邻居、记住名字、配对(绑定)、列出设备、解除配对。
 *
 * 为什么自己写而不是整包移植 espressif/esp-now v2.5.3:
 *   · 该组件 CMakeLists 全量注册源码, 无裁剪开关, 会连带 console/fatfs/spiffs/
 *     pcap/OTA/provisioning 一起编进来;
 *   · 它用 `-Wl,--wrap=esp_log_write/esp_log` 劫持**全项目**日志, 与 LinTOS 的
 *     svc_log + LOGRING 自留证机制直接冲突;
 *   · 安全握手依赖 protocomm + protobuf-c + mbedtls 三件套(约 1250 行),
 *     体积与复杂度远超本任务需要;
 *   · 核心层会主动 esp_wifi_set_channel 跳信道, 会打断 wifi_manager 已建立的连接。
 *   ⇒ 只**借鉴其机制**(绑定表 / 类型子集 / 收广播自动建 peer), 代码自研。
 *
 * 安全边界 (必须说清楚, 不假装):
 *   · 本层配对 = 双方交换 16 字节 LMK 并各自 add_peer(encrypt=1), 启用 ESP-NOW
 *     原生 AES 加密。可防"同信道其它设备冒充/蹭收", **不能**防配对那一刻的
 *     空口窃听 (LMK 是明文传的), 也没有前向保密与身份认证。
 *   · 真正的安全握手 (官方 security1: ECDH + proof-of-possession) 未引入。
 *     需要强安全时应在应用层加预共享密钥, 或后续单独接 protocomm。
 *
 * 硬约束:
 *   · ESP-NOW 总 peer 上限 20, **加密 peer 只有 6** (ESP_NOW_MAX_ENCRYPT_PEER_NUM);
 *   · 本层设备表上限 ESPNOW_PEER_MAX=10, 超出淘汰最久未见的非配对设备;
 *   · espnow_peer_on_frame() 跑在 WiFi 任务上下文: 只做内存操作, **不写 NVS、不阻塞**。
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "espnow_link.h"

#define ESPNOW_PEER_MAX     10    /* 设备表上限 (留余量给广播/临时 peer) */
#define ESPNOW_ENCRYPT_MAX  6     /* 驱动硬限制: 加密 peer 最多 6 个 */

typedef struct {
    uint8_t  mac[ESPNOW_MAC_LEN];
    char     name[ESPNOW_NAME_LEN];
    uint8_t  name_len;
    int8_t   rssi;
    uint32_t last_seen_ms;   /* 相对开机时刻, 0 = 从未收到 */
    bool     paired;
    bool     encrypted;
    bool     used;
} espnow_dev_t;

/* 挂载/卸载: init 幂等, 会从 NVS 载入已保存的配对表; reset 只清运行时表 */
void espnow_peer_init(void);
void espnow_peer_reset(void);

/* 本机设备名 (发现应答里带的那个; 默认取 "LinTOS") */
void        espnow_peer_set_local_name(const char *name);
const char *espnow_peer_local_name(void);

/* 设备发现: 广播 DISCOVER, window_ms 内收集 ANNOUNCE。
 * 必须 start 之后调用。窗口内重复调用只是续期。 */
esp_err_t espnow_peer_discover(uint32_t window_ms);
bool      espnow_peer_discover_active(void);

/* 设备列表 (只读指针, 调用方不要保存) */
int                 espnow_peer_dev_count(void);
const espnow_dev_t *espnow_peer_dev_get(int idx);

/* 配对 / 解除配对。key=NULL 走明文; key 且 key_len==16 走 ESP-NOW 原生加密。
 * 配对是**双向确认**: 本机立即生效, 对端收到 BIND_REQ 后自行 add_peer 并回 BIND_RSP。 */
esp_err_t espnow_peer_pair(const uint8_t mac[ESPNOW_MAC_LEN], const uint8_t *key, int key_len);
esp_err_t espnow_peer_unpair(const uint8_t mac[ESPNOW_MAC_LEN]);
int       espnow_peer_paired_count(void);

/* 该 MAC 是否已配对。espnow_link 在驱动 peer 额度用满、需要回收临时项时用它
 * 判断"这个不能删"。 */
bool espnow_peer_is_paired(const uint8_t mac[ESPNOW_MAC_LEN]);

/* 收包回调入口 (控制帧, 由 espnow_link 分发): 不写 NVS, 不阻塞 */
void espnow_peer_on_frame(uint8_t type, uint16_t seq, const uint8_t *payload, int len,
                          const uint8_t src[ESPNOW_MAC_LEN], int8_t rssi);

/* 非回调上下文调用: 配对表有变更时落 NVS (无变更立即返回) */
void espnow_peer_persist(void);
