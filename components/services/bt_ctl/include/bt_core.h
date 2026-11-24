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
/* ★ 2026-11-23 R2: 扫描事务**彻底**结束 (射频独占已释放、任务计数已归零)。
 * 此时才可以真正去关射频/拆栈; on_scan_done 期间严格说扫描任务还在跑。 */
typedef void (*bt_core_scan_idle_cb_t)(void);

typedef struct {
    bt_core_input_cb_t   on_input;        /* HID INPUT 报告 (裸数据, 未解码) */
    bt_core_conn_cb_t    on_connected;    /* 连接成功 */
    bt_core_conn_cb_t    on_disconnected; /* 断开 */
    bt_core_scan_done_cb_t on_scan_done;  /* 一次扫描完成 (结果已缓存) */
    bt_core_scan_idle_cb_t on_scan_idle;  /* ★ R2: 扫描事务已完全退出 (可安全释放) */
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

/* ============================================================================
 *  扫描 (2026-11-23 R2 起是**独占资源**)
 * ----------------------------------------------------------------------------
 *  为什么必须独占 (IDF 6.0 源码依据):
 *    · esp_hid_scan() 起手 free 上一轮结果, 结果攒在**文件静态**链表里 ⇒ 两个线程
 *      同时进来, 后进者会把先进者的半成品链表 free 掉 (UAF + 结果串台);
 *    · 两者共用同一个完成信号量 ⇒ 一次 INQ_CMPL 只有一个 give, 先完成的取走后,
 *      另一方永远等不到自己的完成事件 (只能靠超时逃出来)。
 *  因此所有扫描入口 (任务式长扫 / 按名字短扫) 都过同一个闸门, 且:
 *    · 连接事务进行中 (有线程卡在 esp_hidh_dev_open) 时**拒绝**启动新扫描;
 *    · bt_core_stop_scan() 只表达"不要结果了", 不会放开闸门 (任务退出时才放)。
 * ==========================================================================*/
/* 启动一次扫描任务 (异步, 5s). 返回 false = 已被占用/连接事务在飞/建任务失败. */
bool bt_core_scan(int duration_sec);
/* 射频是否正在扫 (含内联短扫). 这是"扫描真的在跑"的唯一真相源。 */
bool bt_core_is_scanning(void);
/* 扫描事务是否仍占用 (含任务尚未退出). 拆栈/关射频前必须为 false。 */
bool bt_core_scan_busy(void);
/* 扫描闸门是否被"界面要结果"的那轮扫描占着 (会回调 on_scan_done)。
 * 与 bt_core_scan_busy 的区别: 后者也包含回连用的"按名字短扫"(不回调上层)。 */
bool bt_core_ui_scan_running(void);
/* ★ 2026-11-23 R3: **真的有连接事务卡在 esp_hidh_dev_open 里** (不是"有任务对象存在")。
 *   扫描入口用它区分"射频正被连接流程占着"和"只是回连任务/残留状态还在"——
 *   后者绝不能用来拒绝用户主动扫描。 */
bool bt_core_open_busy(void);
/* 等扫描事务退出, 最多 ms 毫秒. true = 已空闲 (可以安全释放)。 */
bool bt_core_scan_wait_idle(uint32_t timeout_ms);
/* 表达"不要本轮结果了". 会顺带请求控制器停扫, 但**不放开闸门**。 */
void bt_core_stop_scan(void);

/* 取最近一次扫描结果 (链表; 每次 scan 后重建).
 * ⚠ 只在 on_scan_done 回调里安全: 下一轮扫描开始时整条链表会被释放。
 *   需要跨任务/延后使用某一条结果时, 用 bt_core_scan_find() 拷一份出来。 */
esp_hid_scan_result_t *bt_core_last_scan(void);
int bt_core_last_scan_count(void);
/* ★ 2026-11-22: 在内部互斥下按地址把一条结果**拷进 out** (name 不拷, 置 NULL)。
 *   返回 true = 命中。用于 UI 任务点选设备 —— 原来 UI 直接遍历 bt_core_last_scan()
 *   的链表, 而扫描任务开始下一轮时会把整条链表 free 掉 (use-after-free 读)。 */
bool bt_core_scan_find(const uint8_t bd_addr[6], esp_hid_scan_result_t *out);

/* 连接 / 断开 */
bool bt_core_connect(const esp_hid_scan_result_t *r);
/* 按名字连接: 起一次阻塞式短扫(5s), 在结果里按名字找到"当前"地址再连。
 * 用于 BLE 地址会漂移的手柄(历史里的旧地址直连必失败)。
 * ⚠ 内部会**同步** open (手柄不在时阻塞可达 30s) ⇒ 只能用在 UI/用户主动场景,
 *   自动回连请用 bt_core_scan_find_by_name() + 异步连接。
 * ★ R2: 短扫同样要过扫描闸门; 已被占用时返回 false (不排队、不并行)。 */
bool bt_core_connect_by_name(const char *name);
/* ★ 2026-11-23: 按名字短扫(5s), **只解析地址不连接**, 结果拷进 out (name 置 NULL)。
 *   给自动回连用: 让调用方把连接丢到独立任务, 免得回连任务被 open 卡住 30s。
 *   ★ R2: 过扫描闸门; 扫描/连接事务已占用时返回 false (调用方下一轮再试)。 */
bool bt_core_scan_find_by_name(const char *name, esp_hid_scan_result_t *out);
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