#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  V1.0.38 按键映射方案 (大幅简化)
 *  - 8 个逻辑功能 (4 方向 + 4 功能键) → 12 个物理输入 (4 摇杆 + 8 按钮)
 *  - 不再捕获完整 HID 报告: 按 report_id=4 的固定位置 (d[4]=hat, d[6]=8 按钮)
 *  - 不再需要 settle/event_window/cooldown 等去抖机制
 *  - 摇杆始终解码, 4 向映射在用户配置中可指向任何 P_HAT_* 或 P_*
 * ======================================================================== */

/* 10 个逻辑功能 (按键映射的目标): 上下左右/确认/返回/回到菜单/多功能键 + Start/Select */
typedef enum {
    F_UP = 0,
    F_DOWN,
    F_LEFT,
    F_RIGHT,
    F_CONFIRM,    /* 确定 */
    F_BACK,       /* 返回 */
    F_EXIT,       /* 退出到菜单 */
    F_FAV,        /* 多功能键 (收藏) */
    F_START,      /* V1.0.68: Start 键 */
    F_SELECT,     /* V1.0.68: Select 键 */
    FUNC_MAX = 10
} func_t;

/* 14 个可选物理输入 (Q36 gamepad HID report_id=4 实际按键) */
typedef enum {
    P_HAT_UP = 0, P_HAT_DOWN, P_HAT_LEFT, P_HAT_RIGHT,  /* 摇杆/十字键 4 向 */
    P_A, P_B, P_X, P_Y,                                  /* 面键 */
    P_L1, P_R1, P_L2, P_R2,                              /* 肩键 */
    P_SELECT, P_START,                                   /* 副按钮 Select/Start */
    PHYS_MAX = 14
} phys_t;

/* 补充按键: 4 个逻辑功能 (F/G/Shift/空格 → BBK 功能1-4) → 12 个物理输入
 * 与 8 键核心映射 (func_t) 完全独立: 单独存储、持久化、映射流程.
 * 用途: 电子词典游戏设置里把任意手柄按键映射成 BBK 的 F/G/Shift/空格 四个功能键. */
#define SUP_MAX 4
typedef enum {
    SUP_F = 0,      /* 功能1 (F) */
    SUP_G,          /* 功能2 (G) */
    SUP_SHIFT,      /* 功能3 (Shift) */
    SUP_SPACE,      /* 功能4 (空格) */
} sup_func_t;

/* 物理输入的人类可读名称 (用于 UI 显示) */
extern const char *bt_manager_phys_name(phys_t p);
extern const char *bt_manager_func_name(func_t f);
extern const char *bt_manager_sup_func_name(int idx);

/* ========================================================================
 *  按键状态查询 (状态式, 不带边沿检测; 消费者自己负责边沿)
 * ======================================================================== */
bool bt_manager_is_key_pressed(func_t f);

/* ========================================================================
 *  按键映射 (8 个功能 → 12 个物理输入)
 *  - 启动映射时 poll_new_press_reset() 清空待消费边沿
 *  - 每帧调 poll_new_press(f) 消费一个边沿, 写入 s_map[f]
 *  - 用户确认后调 save_key_map() 持久化
 * ======================================================================== */
void bt_manager_poll_new_press_reset(void);

/* V1.0.46: 3 秒自动重连检查 (独立后台任务调用) */
void bt_manager_poll_auto_reconnect(void);
bool bt_manager_poll_new_press(func_t f);
void bt_manager_save_key_map(void);
void bt_manager_reset_key_map(void);   /* 清空并恢复默认 */

/* 直接读写映射 (供 UI 列表式编辑) */
void bt_manager_set_key_map(func_t f, phys_t p);
phys_t bt_manager_get_key_map(func_t f);

/* ========================================================================
 *  补充按键映射 (4 个功能 → 12 个物理输入)
 *  - 与 8 键核心映射完全独立存储/持久化.
 *  - 捕获阶段: poll_sup_capture() 消费一个物理边沿 (返回 phys, 不写映射, 供 UI 校验"已占用").
 *  - 确认阶段: set_sup_map() 写入; 4 个完成后 save_sup_map() 持久化 (NVS "supmap").
 *  - 清除: reset_sup_map() 全部置 PHYS_MAX(未映射) 并清 NVS.
 *  - 游戏内投递: is_sup_pressed(idx) 查询第 idx 个补充键当前是否按下 (供 joypad_state 使用).
 * ======================================================================== */
bool bt_manager_poll_sup_capture(phys_t *out);
bool bt_manager_is_phys_pressed(phys_t p);
bool bt_manager_is_sup_pressed(int idx);
void bt_manager_set_sup_map(int idx, phys_t p);
phys_t bt_manager_get_sup_map(int idx);
void bt_manager_save_sup_map(void);
void bt_manager_reset_sup_map(void);
/* 物理键 p 是否已被占用 (8 键核心映射 或 其它补充键). 用于映射时"已占用的按键无效". */
bool bt_manager_sup_phys_used(phys_t p, int cur_idx);

/* ========================================================================
 *  GB 辅助按键映射 (2 个功能: SELECT/START → 12 个物理输入)
 *  - 与 8 键核心映射(s_map) 完全独立存储/持久化 (NVS "gbmap").
 *  - 只在 GB 游戏二级菜单及游戏中生效 (input.c 的 GB joypad 投递).
 *  - 捕获流程与补充按键一致: 捕获物理边沿 → 确认(F_CONFIRM)/跳过(F_BACK).
 *  - 清除: reset_gb_map() 全部置 PHYS_MAX(未映射) 并清 NVS.
 * ======================================================================== */
#define GB_MAP_MAX 2
typedef enum {
    GB_SELECT = 0,   /* SELECT 键 */
    GB_START,        /* START 键 */
} gb_func_t;

bool bt_manager_poll_gb_capture(phys_t *out);
bool bt_manager_is_gb_pressed(int idx);
void bt_manager_set_gb_map(int idx, phys_t p);
phys_t bt_manager_get_gb_map(int idx);
void bt_manager_save_gb_map(void);
void bt_manager_reset_gb_map(void);
/* 物理键 p 是否已被占用 (8 键核心映射 或 其它 GB 辅助键). 用于映射时"已占用的按键无效". */
bool bt_manager_gb_phys_used(phys_t p, int cur_idx);
/* SELECT 或 START 是否已有任一映射 (用于进入 GB 菜单时判断是否提示映射). */
bool bt_manager_gb_map_set(void);
const char *bt_manager_gb_func_name(int idx);

/* ========================================================================
 *  蓝牙基础 API (保持不变)
 * ======================================================================== */
typedef struct {
    uint8_t bd_addr[6];
    uint8_t addr_type;
    char name[32];
    int8_t rssi;
    bool has_name;
} bt_device_t;

typedef void (*bt_scan_callback_t)(bt_device_t *results, int count);
typedef void (*bt_device_found_cb_t)(bt_device_t *results, int count, bool updated);
typedef void (*bt_connect_cb_t)(bool connected);
typedef void (*bt_connect_progress_cb_t)(const char *stage);

void bt_manager_init(void);
const char *bt_manager_get_status(void);
void bt_manager_start_scan(bt_scan_callback_t callback);
void bt_manager_start_scan_continuous(bt_device_found_cb_t callback);
bool bt_manager_is_scanning(void);
void bt_manager_stop_scan(void);
bool bt_manager_is_connected(void);
int bt_manager_get_scan_results(bt_device_t **results);
const char *bt_manager_get_device_name(const bt_device_t *dev);
const char *bt_manager_get_connected_device_name(void);
bool bt_manager_connect_device(const bt_device_t *dev);
void bt_manager_disconnect(void);
void bt_manager_set_connect_callback(bt_connect_cb_t cb);
void bt_manager_set_connect_progress_cb(bt_connect_progress_cb_t cb);
const char *bt_manager_get_connect_error(void);
bool bt_manager_last_connect_was_new(void);
bool bt_manager_is_connecting(void);
void bt_manager_cancel_connect(void);
void bt_manager_enable(void);
void bt_manager_disable(void);
bool bt_manager_is_ready(void);
bool bt_manager_is_stack_ready(void);
/* 屏保软挂起: 断开连接 + 停止扫描, 协议栈保持存活 (不重新初始化, 防内存不足崩溃) */
void bt_manager_suspend(void);
void bt_manager_resume(void);
bool bt_manager_is_suspended(void);

/* ========================================================================
 *  连接记录 (NVS 持久化)
 * ======================================================================== */
#define BT_HISTORY_MAX  8
int bt_manager_get_history_count(void);
const bt_device_t *bt_manager_get_history_at(int index);
bool bt_manager_remove_history_at(int index);
void bt_manager_clear_paired(void);
bool bt_manager_is_paired_device(const uint8_t *bd_addr);
void bt_manager_add_to_history(const bt_device_t *dev);
bool bt_manager_get_connected_addr(uint8_t *out_addr6);

/* ========================================================================
 *  无 BT 时的 stub
 * ======================================================================== */
#if !CONFIG_BT_ENABLED
static inline void bt_manager_init(void) {}
static inline const char *bt_manager_get_status(void) { return "未启用"; }
static inline void bt_manager_start_scan(bt_scan_callback_t c) { (void)c; }
static inline void bt_manager_start_scan_continuous(bt_device_found_cb_t c) { (void)c; }
static inline bool bt_manager_is_scanning(void) { return false; }
static inline void bt_manager_stop_scan(void) {}
static inline bool bt_manager_is_connected(void) { return false; }
static inline int bt_manager_get_scan_results(bt_device_t **r) { (void)r; return 0; }
static inline const char *bt_manager_get_device_name(const bt_device_t *d) { (void)d; return ""; }
static inline const char *bt_manager_get_connected_device_name(void) { return ""; }
static inline bool bt_manager_connect_device(const bt_device_t *d) { (void)d; return false; }
static inline void bt_manager_disconnect(void) {}
static inline void bt_manager_poll_new_press_reset(void) {}
static inline bool bt_manager_poll_new_press(func_t f) { (void)f; return false; }
static inline void bt_manager_save_key_map(void) {}
static inline void bt_manager_reset_key_map(void) {}
static inline void bt_manager_set_key_map(func_t f, phys_t p) { (void)f; (void)p; }
static inline phys_t bt_manager_get_key_map(func_t f) { (void)f; return P_HAT_UP; }
static inline bool bt_manager_is_key_pressed(func_t f) { (void)f; return false; }
static inline const char *bt_manager_sup_func_name(int i) { (void)i; return ""; }
static inline bool bt_manager_poll_sup_capture(phys_t *o) { (void)o; return false; }
static inline bool bt_manager_is_phys_pressed(phys_t p) { (void)p; return false; }
static inline bool bt_manager_is_sup_pressed(int i) { (void)i; return false; }
static inline void bt_manager_set_sup_map(int i, phys_t p) { (void)i; (void)p; }
static inline phys_t bt_manager_get_sup_map(int i) { (void)i; return P_HAT_UP; }
static inline void bt_manager_save_sup_map(void) {}
static inline void bt_manager_reset_sup_map(void) {}
static inline bool bt_manager_sup_phys_used(phys_t p, int i) { (void)p; (void)i; return false; }
static inline bool bt_manager_poll_gb_capture(phys_t *o) { (void)o; return false; }
static inline bool bt_manager_is_gb_pressed(int i) { (void)i; return false; }
static inline void bt_manager_set_gb_map(int i, phys_t p) { (void)i; (void)p; }
static inline phys_t bt_manager_get_gb_map(int i) { (void)i; return P_HAT_UP; }
static inline void bt_manager_save_gb_map(void) {}
static inline void bt_manager_reset_gb_map(void) {}
static inline bool bt_manager_gb_phys_used(phys_t p, int i) { (void)p; (void)i; return false; }
static inline bool bt_manager_gb_map_set(void) { return false; }
static inline const char *bt_manager_gb_func_name(int i) { (void)i; return ""; }
static inline const char *bt_manager_phys_name(phys_t p) { (void)p; return ""; }
static inline const char *bt_manager_func_name(func_t f) { (void)f; return ""; }
static inline void bt_manager_set_connect_callback(bt_connect_cb_t c) { (void)c; }
static inline void bt_manager_set_connect_progress_cb(bt_connect_progress_cb_t c) { (void)c; }
static inline const char *bt_manager_get_connect_error(void) { return "蓝牙未启用"; }
static inline bool bt_manager_last_connect_was_new(void) { return false; }
static inline bool bt_manager_is_connecting(void) { return false; }
static inline void bt_manager_cancel_connect(void) {}
static inline void bt_manager_enable(void) {}
static inline void bt_manager_disable(void) {}
static inline bool bt_manager_is_ready(void) { return false; }
static inline bool bt_manager_is_stack_ready(void) { return false; }
static inline void bt_manager_suspend(void) {}
static inline void bt_manager_resume(void) {}
static inline bool bt_manager_is_suspended(void) { return false; }
static inline int bt_manager_get_history_count(void) { return 0; }
static inline const bt_device_t *bt_manager_get_history_at(int i) { (void)i; return NULL; }
static inline bool bt_manager_remove_history_at(int i) { (void)i; return false; }
static inline void bt_manager_add_to_history(const bt_device_t *d) { (void)d; }
static inline bool bt_manager_get_connected_addr(uint8_t *o) { (void)o; return false; }
#endif /* !CONFIG_BT_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BT_MANAGER_H */
