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

/* ★ 2026-10-27: 只关/开射频(不拆栈) —— 关蓝牙走这里, 保证界面与耗电都真·关闭 */
void bt_core_radio_off(void);
void bt_core_radio_on(void);

/* ★ 2026-10-30: 深度拆栈 —— 真正归还 ~67KB 内部 RAM (controller/Bluedroid/esp_hidh 全拆).
 * 只允许在"最后一个使用者退出"时调用 (bt_manager_disable_release);
 * 内部失败兜底会退回"只关射频", 不会出现关不掉。 */
void bt_core_deinit_deep(void);

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
/* 按名字连接: 起一次阻塞式短扫(5s), 在结果里按名字找到"当前"地址再连。
 * 用于 BLE 地址会漂移的手柄(历史里的旧地址直连必失败)。调用前须确保无其他扫描在跑。 */
bool bt_core_connect_by_name(const char *name);
void bt_core_disconnect(void);

/* ★ 2026-10-18: 主动取消一个"悬着的"连接（失败/超时后必须调，否则下次 open 必失败）。
 *   - 若已有 dev 句柄 → close 掉；
 *   - 再对 bda 发一次 GAP disconnect，撤掉 Bluedroid 侧 pending create-conn / 半开链路。 */
void bt_core_cancel_pending(const uint8_t bd_addr[6]);
/* 删除指定对端地址的 BLE 绑定: 手柄地址漂移(同名不同址)时清掉残留旧绑定, 让双方干净重配对 */
bool bt_core_remove_bond(const uint8_t bd_addr[6]);

/* 当前连接设备句柄 (未连接返回 NULL) */
esp_hidh_dev_t *bt_core_connected_dev(void);
bool bt_core_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_CORE_H */