#ifndef BT_CORE_H
#define BT_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_hidh.h"
#include "esp_hid_gap.h"   /* 定义 esp_hid_scan_result_t / esp_hid_scan 扫描 API */

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  bt_core - 蓝牙连接核心 (全新重写)
 *  ---------------------------------------------------------------------------
 *  底稿 = ESP-IDF 官方 examples/bluetooth/esp_hid_host, 只走 BLE (S3 仅 BLE).
 *  职责(官方 API 即可可靠覆盖的部分):
 *    - 协议栈 + esp_hidh 初始化
 *    - 一次性阻塞扫描, 收集 HID 设备结果到内部缓存
 *    - 连接 / 断开
 *    - 转发 INPUT 报告与连接状态到上层回调
 *  连续扫描/自动重连/按键映射/NVS 记录 等策略全部由上层 bt_manager.c 桥接.
 */

typedef void (*bt_core_input_cb_t)(esp_hidh_dev_t *dev,
                                   uint8_t report_id,
                                   const uint8_t *data, uint16_t len);
typedef void (*bt_core_conn_cb_t)(bool connected, esp_hidh_dev_t *dev);
typedef void (*bt_core_scan_done_cb_t)(void);

typedef struct {
    bt_core_input_cb_t   on_input;        /* HID INPUT 报告 (裸数据, 未解码) */
    bt_core_conn_cb_t    on_connected;    /* 连接成功 */
    bt_core_conn_cb_t    on_disconnected; /* 断开 */
    bt_core_scan_done_cb_t on_scan_done;  /* 一次扫描完成 (结果已缓存) */
} bt_core_cbs_t;

/* 生命周期: 初始化协议栈 + esp_hidh. 返回 true 表示就绪. */
bool bt_core_init(const bt_core_cbs_t *cbs);
bool bt_core_is_ready(void);
void bt_core_deinit(void);

/* 扫描: 阻塞 duration_sec 秒, 收集 HID 设备到内部缓存, 完成后调 on_scan_done.
 * 需在普通任务中调用 (esp_hid_scan 是同步阻塞函数). */
void bt_core_scan(int duration_sec);
bool bt_core_is_scanning(void);
void bt_core_stop_scan(void);

/* 取最近一次扫描结果 (链表; 每次 scan 后重建). */
esp_hid_scan_result_t *bt_core_last_scan(void);
int bt_core_last_scan_count(void);

/* 连接 / 断开 */
bool bt_core_connect(const esp_hid_scan_result_t *r);
void bt_core_disconnect(void);

/* 当前连接设备句柄 (未连接返回 NULL) */
esp_hidh_dev_t *bt_core_connected_dev(void);
bool bt_core_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_CORE_H */