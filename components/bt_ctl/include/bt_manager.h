#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"   /* 供 bt_manager_get_init_stack 的 StackType_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  蓝牙手柄模块 (全新重写, 底稿 = ESP-IDF 官方 examples/bluetooth/esp_hid_host)
 *  ---------------------------------------------------------------------------
 *  本头文件只定义对外接口 (供菜单/游戏调用). 内部实现分层:
 *    bt_core.c  : 官方 esp_hid_host 连接核心 (扫描/连接/INPUT 事件/记录/自动重连)
 *    bt_map.c   : 通用 HID 报告解码 + 按键映射表 + NVS 存档 + 摇杆校准
 *    bt_manager.c: 对外壳, 桥接 bt_core + bt_map, 实现本头文件声明的所有接口.
 *  不再依赖任何蓝方(BTstack/Bluepad32)或历史打补丁代码.
 */

/* ========================================================================
 *  按键映射 - 物理输入 / 逻辑功能 定义
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
    F_START,      /* Start 键 */
    F_SELECT,     /* Select 键 */
    FUNC_MAX = 10
} func_t;

/* 物理输入: 4 方向 + 通用按钮 1~N.
 * 通用化原因: 不同手柄按钮数/布局各异, 不绑定 A/B/X/Y 等固定命名,
 * 按钮按每台手柄 report descriptor 的按钮序号统一编号, 兼容任意手柄. */
#ifndef PHYS_T_DEFINED
#define PHYS_T_DEFINED
typedef enum {
    P_HAT_UP = 0, P_HAT_DOWN, P_HAT_LEFT, P_HAT_RIGHT,  /* 方向 0..3 */
    P_BTN_1 = 4, P_BTN_2, P_BTN_3, P_BTN_4, P_BTN_5,    /* 通用按钮 4.. */
    P_BTN_6, P_BTN_7, P_BTN_8, P_BTN_9, P_BTN_10,
    P_BTN_11, P_BTN_12, P_BTN_13, P_BTN_14, P_BTN_15, P_BTN_16,
    PHYS_MAX = 20
} phys_t;
#endif

/* 物理输入的人类可读名称 (用于 UI 显示) */
extern const char *bt_manager_phys_name(phys_t p);
extern const char *bt_manager_func_name(func_t f);

/* ========================================================================
 *  补充按键映射 (4 个逻辑功能 → 任意物理输入, 独立持久化)
 *  用途: 电子词典游戏设置里把任意手柄按键映射成 BBK 的 F/G/Shift/空格 四个功能键.
 * ======================================================================== */
#define SUP_MAX 4
typedef enum {
    SUP_F = 0,      /* 功能1 (F) */
    SUP_G,          /* 功能2 (G) */
    SUP_SHIFT,      /* 功能3 (Shift) */
    SUP_SPACE,      /* 功能4 (空格) */
} sup_func_t;

extern const char *bt_manager_sup_func_name(int idx);

/* ========================================================================
 *  按键状态查询 (状态式, 不带边沿检测; 消费者自己负责边沿)
 * ======================================================================== */
bool bt_manager_is_key_pressed(func_t f);

/* ========================================================================
 *  按键映射 (8 个功能 → 物理输入)
 * ======================================================================== */
void bt_manager_poll_new_press_reset(void);
void bt_manager_poll_auto_reconnect(void);
/* 取一帧新物理按键边沿并写入 s_map[f].
 * 返回: 1=已映射, 0=无新按键, -1=按键已被其它功能占用(重复映射, 拒绝). */
int bt_manager_poll_new_press(func_t f);
void bt_manager_save_key_map(void);
void bt_manager_reset_key_map(void);   /* 清空并恢复默认 */
void bt_manager_set_key_map(func_t f, phys_t p);
phys_t bt_manager_get_key_map(func_t f);
/* 当前是否有任意物理输入处于按下状态 (映射启动时等待松开用). */
bool bt_manager_any_phys_down(void);
/* 物理键 p 是否已被其它核心功能/补充键/GB 键占用 (排除当前正在映射的 cur_f).
 * 用于核心 8 键映射时拒绝重复按键. */
bool bt_manager_core_phys_used(phys_t p, func_t cur_f);

/* ========================================================================
 *  补充按键映射 (4 个功能 → 物理输入, 独立持久化)
 * ======================================================================== */
bool bt_manager_poll_sup_capture(phys_t *out);
bool bt_manager_is_phys_pressed(phys_t p);
bool bt_manager_is_sup_pressed(int idx);
void bt_manager_set_sup_map(int idx, phys_t p);
phys_t bt_manager_get_sup_map(int idx);
void bt_manager_save_sup_map(void);
void bt_manager_reset_sup_map(void);
/* 物理键 p 是否已被占用 (核心映射 或 其它补充键). 用于映射时"已占用的按键无效". */
bool bt_manager_sup_phys_used(phys_t p, int cur_idx);

/* ========================================================================
 *  GB 辅助按键映射 (2 个功能: SELECT/START → 物理输入, 独立持久化)
 *  只在 GB 游戏二级菜单及游戏中生效.
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
/* 物理键 p 是否已被占用 (核心映射 或 其它 GB 辅助键). */
bool bt_manager_gb_phys_used(phys_t p, int cur_idx);
/* SELECT 或 START 是否已有任一映射 (用于进入 GB 菜单时判断是否提示映射). */
bool bt_manager_gb_map_set(void);
const char *bt_manager_gb_func_name(int idx);

/* ========================================================================
 *  蓝牙基础 API
 * ======================================================================== */
#ifndef BT_DEVICE_T_DEFINED
#define BT_DEVICE_T_DEFINED
typedef struct {
    uint8_t bd_addr[6];
    uint8_t addr_type;
    char name[32];
    int8_t rssi;
    bool has_name;
} bt_device_t;
#endif

typedef void (*bt_scan_callback_t)(bt_device_t *results, int count);
typedef void (*bt_device_found_cb_t)(bt_device_t *results, int count, bool updated);
typedef void (*bt_connect_cb_t)(bool connected);
typedef void (*bt_connect_progress_cb_t)(const char *stage);

/* ========================================================================
 *  设备信息 (供"设备信息"页展示; 由 bt_manager_get_conn_info() 返回)
 * ======================================================================== */
typedef struct {
    char    model[24];   /* 控制器型号字符串 */
    char    name[32];    /* 设备名 */
    uint8_t battery;     /* 0=空 .. 254=满, 255=不可用 */
    uint16_t vid;
    uint16_t pid;
    bool     available;  /* 是否有有效信息 (连接中) */
} btm_conn_info_t;

/* ========================================================================
 *  摇杆自适应校准
 *  - 连接成功后自动采样摇杆/十字键静止中性值作为校准基准, 并加死区抑制漂移.
 *  - 运行时可按一次"校准键"触发 bt_manager_calibrate() 重新采样.
 * ======================================================================== */
void bt_manager_calibrate(void);                 /* 重新采样中性值 */
bool bt_manager_calibration_done(void);          /* 首次校准是否完成 */
uint8_t bt_manager_calibration_neutral(void);    /* 当前中性值 */

void bt_manager_init(void);
/* 取得蓝牙初始化任务用的内部 RAM 静态栈 (供 app_board.c 开机 bt_init 复用) */
StackType_t *bt_manager_get_init_stack(int *words_out);
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
/* 设备信息页: 取当前已连接设备信息. 返回 NULL 表示无连接. */
const btm_conn_info_t *bt_manager_get_conn_info(void);
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
/* 屏保软挂起: 断开连接 + 停止扫描, 协议栈保持存活 */
void bt_manager_suspend(void);
void bt_manager_resume(void);
bool bt_manager_is_suspended(void);

/* ========================================================================
 *  连接记录 (NVS 持久化, 上限 4 条)
 * ======================================================================== */
#define BT_HISTORY_MAX  4
int bt_manager_get_history_count(void);
const bt_device_t *bt_manager_get_history_at(int index);
bool bt_manager_remove_history_at(int index);
void bt_manager_clear_paired(void);
bool bt_manager_is_paired_device(const uint8_t *bd_addr);
void bt_manager_add_to_history(const bt_device_t *dev);
bool bt_manager_get_connected_addr(uint8_t *out_addr6);

#ifdef __cplusplus
}
#endif

#endif /* BT_MANAGER_H */