#pragma once

/* ============================================================================
 * espnow_link — ESP-NOW 点对点/广播 (LinTOS 服务层)
 *
 * 做什么: 两块 (或多块) LinTOS 之间**不用路由器**直接收发小包 (≤250 字节/包)。
 *   · 典型用途: 双机联机小游戏、遥控器、对讲信标、多机排行榜同步。
 *   · 有自己的 MAC 就能点对点发; 不知道对方 MAC 就广播 (ff:ff:ff:ff:ff:ff)。
 *
 * 硬约束 (踩过的坑都在这里):
 *   ① ESP-NOW 跑在 **WiFi STA 接口**上 ⇒ 必须先 `esp_wifi_start()`。本模块会先
 *      `wifi_manager_enable()` 把射频拉起来 (没配网络也能用, 只是不联网)。
 *      ⛔ 别自己再 esp_wifi_init —— 会和 wifi_manager 抢驱动。
 *   ② **收发双方必须在同一信道**。已连路由器时信道由 AP 决定 (跟着 AP 走);
 *      没连网时默认 1 信道 ⇒ 两台都没联网也能通, 但连了不同 AP 的两台通不了。
 *      `espnow_link_channel()` 可以查当前信道, `set_channel()` 可强制指定。
 *   ③ 回调在 **WiFi 任务上下文**里跑 (栈小、不能阻塞) ⇒ 收到包只做 memcpy 进
 *      环形队列, 真正的处理交给调用方轮询 `espnow_link_recv()`。
 *   ④ 负载上限 **250 字节** (协议限制), 超了直接返回错误。
 *
 * 分层: L1 服务层, 依赖 wifi_manager + esp_wifi; 不依赖 os / apps。
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ESPNOW_MAX_PAYLOAD   250      /* 协议上限, 别改 */
#define ESPNOW_MAC_LEN       6
#define ESPNOW_LMK_LEN       16       /* 加密对等体密钥 (可选) */

/* 启动 (幂等)。会确保 WiFi 射频已启动并 esp_now_init。返回 ESP_OK 表示可用。 */
esp_err_t espnow_link_start(void);

/* 停止: 反初始化 esp-now 并清空对等体/队列。**不会**关 WiFi (用户可能正在用)。 */
void espnow_link_stop(void);

bool espnow_link_is_running(void);

/* 添加对等体。lmk = NULL 表示不加密; 给 16 字节则启用 ESP-NOW 加密。
 * 同一个 MAC 重复添加会覆盖。 */
esp_err_t espnow_link_add_peer(const uint8_t mac[ESPNOW_MAC_LEN], const uint8_t *lmk);
esp_err_t espnow_link_del_peer(const uint8_t mac[ESPNOW_MAC_LEN]);
int       espnow_link_peer_count(void);

/* 发数据给单个对等体 (必须先 add_peer) / 广播给所有处于同一信道、未加密的设备。
 * len ∈ 1..250。返回 ESP_OK 表示已交给驱动 (不代表对方收到)。 */
esp_err_t espnow_link_send(const uint8_t mac[ESPNOW_MAC_LEN], const void *data, int len);
esp_err_t espnow_link_broadcast(const void *data, int len);

/* 本机 MAC (ESP-NOW 用的就是 WiFi STA 的 MAC) / 当前信道。 */
esp_err_t espnow_link_self_mac(uint8_t out[ESPNOW_MAC_LEN]);
uint8_t   espnow_link_channel(void);
esp_err_t espnow_link_set_channel(uint8_t ch);

/* 非阻塞取一条已收到的包。
 * 返回 >0 = 负载字节数 (同时填 mac/out), 0 = 队列空, <0 = 参数错。
 * 队列满时新包被丢弃 (不阻塞 WiFi 任务)。 */
int espnow_link_recv(uint8_t mac[ESPNOW_MAC_LEN], void *out, int cap);

/* 队列里还有几条待取 (应用可以拿它做"来数据了"的轮询判断)。 */
int espnow_link_pending(void);

/* 统计 (排障用): 收/发/丢 计数 */
typedef struct {
    uint32_t rx_ok, rx_drop, tx_ok, tx_fail;
    int8_t   last_rssi;
} espnow_link_stats_t;
void espnow_link_get_stats(espnow_link_stats_t *out);
