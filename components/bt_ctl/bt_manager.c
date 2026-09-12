/*
 *  bt_manager.c - 蓝牙手柄对外壳 (全新重写)
 *  ---------------------------------------------------------------------------
 *  桥接 bt_core.c (连接核心) + bt_map.c (HID 解码/按键映射/NVS 存档/摇杆校准)。
 *  实现 bt_manager.h 声明的全部接口, 供菜单/游戏调用。
 *  不再依赖任何 BTstack/Bluepad32 或历史打补丁代码, 只走官方 esp_hid_host。
 */

#include "bt_manager.h"
#include "bt_core.h"
#include "bt_map.h"
#include "bt_kbd.h"
#include "esp_hid_gap.h"   /* 提供 esp_hid_scan_result_t (官方底层扫描结果结构) */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* V1.5.x: enable 同步等待初始化任务完成 */
#include "freertos/timers.h"   /* V1.5.x: 开机自动回连窗口软件定时器 */

static const char *TAG = "bt_manager";

/* ========================================================================
 *  名词表 (UI 显示用)
 * ======================================================================== */
const char *bt_manager_func_name(func_t f)
{
    static const char *names[FUNC_MAX] = {
        "上", "下", "左", "右",        /* F_UP/DOWN/LEFT/RIGHT */
        "确定", "返回", "退出", "收藏", /* F_CONFIRM/BACK/EXIT/FAV */
        "Start", "Select",            /* F_START/F_SELECT */
    };
    if (f >= 0 && f < FUNC_MAX) return names[f];
    return "?";
}

const char *bt_manager_phys_name(phys_t p)
{
    static const char *hats[4] = { "上", "下", "左", "右" };
    if (p >= P_HAT_UP && p <= P_HAT_RIGHT) return hats[p];
    if (p >= P_BTN_1 && p < PHYS_MAX) {
        static char buf[24];
        snprintf(buf, sizeof(buf), "按键%d", p - P_BTN_1 + 1);
        return buf;
    }
    return "?";
}

const char *bt_manager_sup_func_name(int idx)
{
    static const char *names[SUP_MAX] = { "F", "G", "Shift", "空格" };
    if (idx >= 0 && idx < SUP_MAX) return names[idx];
    return "?";
}

const char *bt_manager_gb_func_name(int idx)
{
    static const char *names[GB_MAP_MAX] = { "SELECT", "START" };
    if (idx >= 0 && idx < GB_MAP_MAX) return names[idx];
    return "?";
}

/* ========================================================================
 *  状态
 * ======================================================================== */
static bool s_connected = false;
static bool s_scanning = false;
static bool s_suspended = false;
static bool s_connecting = false;
/* 蓝牙"电源开关": disable() 置真(彻底关栈), enable() 清假.
 * 用于锁屏/屏保省电 —— 关电后自动回连任务必须停手, 不能偷偷把栈再拉起来. */
static bool s_power_off = false;
static void bt_reconnect_task(void *arg);   /* 前向声明: init 里启动, 定义在自动回连小节 */
static bool s_last_connect_was_new = false;
static bool s_pending_new = false;   /* 发起连接时判定"是否新设备", 连接成功后在 on_connected 消费 */

static bt_device_t s_history[BT_HISTORY_MAX];
static int s_history_count = 0;

static phys_t s_map[FUNC_MAX];
static phys_t s_sup[SUP_MAX];
static phys_t s_gb[GB_MAP_MAX];

static btm_conn_info_t s_conn_info;      /* 当前连接设备信息 */
static uint8_t s_conn_addr[6];           /* 当前连接设备地址 (供 get_connected_addr) */
static bool s_conn_addr_valid = false;
static uint8_t s_last_conn_addr[6];      /* 最近一次连接过的地址 (自动重连目标) */
static bool s_last_conn_addr_valid = false;

static bool s_auto_reconnect_pending = false;
/* V1.5.x: 自动回连目标 (本次连接过地址优先, 回退 NVS 最近历史) — 供扫描窗口匹配 */
static uint8_t s_rc_target_addr[6];
static bool s_rc_target_valid = false;

/* 自动回连退避参数 (定义在此供断连回调等前置函数使用):
 * 直连上次地址, 不空转扫描; 退避 60s→120s→240s→封顶300s. */
#define BT_RECONNECT_POLL_MS   60000
#define BT_RECONNECT_MAX_MS    300000
#define BT_BOOT_RECONNECT_POLL_MS 3000   /* V1.5.x: 开机回连窗口内直连间隔 (低占空比) */
#define BT_BOOT_RECONNECT_WINDOW_MS 30000 /* V1.5.x: 开机回连窗口默认时长 */
static uint32_t s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
static TaskHandle_t s_reconnect_task = NULL;   /* 断连/开机可即时唤醒重连任务 (退避睡眠中也能打断) */

/* 上次连接设备名 (持久化): 用于判断"新手柄"——设备名与上次不同则清空旧映射 */
static char  s_last_conn_name[32] = {0};
static bool  s_last_conn_name_valid = false;

/* 扫描结果缓存 (转发给上层回调) */
#define BT_SCAN_CACHE_MAX 40
static bt_device_t s_dev_cache[BT_SCAN_CACHE_MAX];
static int s_dev_cache_count = 0;

/* 回调 */
static bt_scan_callback_t      s_scan_cb = NULL;
static bt_device_found_cb_t    s_cont_cb = NULL;
static bt_connect_cb_t         s_connect_cb = NULL;
static bt_connect_progress_cb_t s_prog_cb = NULL;

static bool s_inited = false;   /* bt_core 是否已初始化 */
/* V1.5.x 省电: 蓝牙按需开启 — 完整初始化(映射/NVS/核心/回连任务)是否完成.
 * 开机不再初始化; 首次 bt_manager_enable (进蓝牙/手柄设置页) 时触发. */
static volatile bool s_full_init_done = false;
static SemaphoreHandle_t s_init_done_sem = NULL;   /* enable 同步等待初始化完成 */

/* ========================================================================
 *  默认映射
 * ======================================================================== */
/* 默认核心映射 (按钮按通用序号): P_A→按键1, P_B→按键2, L2→按键7, R2→按键8,
 * Start→按键10, Select→按键9. 用户可据各自手柄重设. */
static const phys_t k_default_map[FUNC_MAX] = {
    P_HAT_UP, P_HAT_DOWN, P_HAT_LEFT, P_HAT_RIGHT,  /* 方向 */
    P_BTN_1, P_BTN_2, P_BTN_7, P_BTN_8,             /* 确定/返回/退出/收藏 */
    P_BTN_10, P_BTN_9,                               /* Start/Select */
};

/* 补充功能 (F/G/Shift/空格) 默认 -> 通用按钮 (对应原 L1/R1/L2/R2) */
static const phys_t k_default_sup[SUP_MAX] = { P_BTN_5, P_BTN_6, P_BTN_7, P_BTN_8 };
static const phys_t k_default_gb[GB_MAP_MAX] = { P_BTN_9, P_BTN_10 };  /* SELECT/START */

#define NVS_NS       "btiman"
#define NVS_KEY_MAP     "kmap"     /* phys_t × FUNC_MAX  (10) */
#define NVS_KEY_SUP     "supmap"   /* phys_t × SUP_MAX   (4) */
#define NVS_KEY_GB      "gbmap"    /* phys_t × GB_MAP_MAX(2) */
#define NVS_KEY_HIST    "history"  /* bt_device_t × BT_HISTORY_MAX */
#define NVS_KEY_HCNT    "hcnt"     /* 历史条数 */
#define NVS_KEY_LNAME   "lconn"    /* 上次连接设备名 (判断新手柄用) */

/* ========================================================================
 *  NVS 存取
 * ======================================================================== */
static nvs_handle_t s_nvs = 0;
static bool s_nvs_open = false;

static void nvs_init_handle(void)
{
    if (s_nvs_open) return;
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "nvs namespace %s open failed", NVS_NS);
    }
}

static bool nvs_load_blob(const char *key, void *dst, size_t len)
{
    nvs_init_handle();
    if (!s_nvs_open) return false;
    if (nvs_get_blob(s_nvs, key, dst, &len) != ESP_OK) return false;
    return true;
}

static void nvs_save_blob(const char *key, const void *src, size_t len)
{
    nvs_init_handle();
    if (!s_nvs_open) return;
    nvs_set_blob(s_nvs, key, src, len);
    nvs_commit(s_nvs);
}

/* ========================================================================
 *  phys 查询 → tmap 解码层
 * ======================================================================== */
/* bt_map 中按钮码 = 按钮索引 (0..N). bt_map_btn_pressed(i) 查询第 i+1 个按钮. */
static bool phys_pressed(phys_t p)
{
    if (p >= P_HAT_UP && p <= P_HAT_RIGHT) {
        return bt_map_hat_dir(p); /* 0上 1下 2左 3右 */
    }
    if (p >= P_BTN_1 && p < PHYS_MAX) {
        int btn = p - P_BTN_1;   /* 按钮索引 0..N */
        return bt_map_btn_pressed(btn);
    }
    return false;
}

/* bt_map_poll_press 的 raw 码 → phys_t. 返回 (phys_t)-1 表示无效.
 * raw: <100 = 按钮索引; >=100 = 100+hatstate(方向). */
static phys_t raw_to_phys(int raw)
{
    if (raw < 0) return (phys_t)-1;
    if (raw < 100) {
        /* 按钮索引 0..N-1 -> P_BTN_1+idx */
        int b = raw;
        int max_btns = PHYS_MAX - P_BTN_1;   /* 可映射按钮数 */
        if (b < 0 || b >= max_btns) return (phys_t)-1;
        return (phys_t)(P_BTN_1 + b);
    }
    /* hat 方向态: 100+hatstate, 0/2/4/6 = 上/右/下/左 */
    switch (raw - 100) {
    case 0: return P_HAT_UP;
    case 2: return P_HAT_RIGHT;
    case 4: return P_HAT_DOWN;
    case 6: return P_HAT_LEFT;
    default: return (phys_t)-1;
    }
}

/* ========================================================================
 *  按键状态查询
 * ======================================================================== */
bool bt_manager_is_key_pressed(func_t f)
{
    if (f < 0 || f >= FUNC_MAX) return false;
    return phys_pressed(s_map[f]);
}

bool bt_manager_is_phys_pressed(phys_t p)
{
    return phys_pressed(p);
}

/* ========================================================================
 *  核心映射
 * ======================================================================== */
void bt_manager_poll_new_press_reset(void)
{
    bt_map_poll_reset();
}

void bt_manager_poll_auto_reconnect(void)
{
    if (s_connected || s_suspended) return;
    /* 回连目标: 本次开机连接过的地址优先 (最新); 无则回退 NVS 持久化的最近历史,
     * 保证"开机窗口超时后长按确认键"等场景仍能自动回连已配对手柄. */
    if (s_last_conn_addr_valid) {
        memcpy(s_rc_target_addr, s_last_conn_addr, 6);
    } else if (s_history_count > 0) {
        memcpy(s_rc_target_addr, s_history[0].bd_addr, 6);
    } else {
        return;
    }
    s_rc_target_valid = true;
    s_auto_reconnect_pending = true;
    /* V1.5.x: 已有扫描在进行 (长按确认键扫描弹窗/添加设备) → 仅挂起自动回连标志,
     * 由扫描完成回调在结果里匹配回连目标连接, 不重复发起扫描 (避免 old-results 扫描风暴). */
    if (bt_manager_is_scanning()) return;
    bt_core_scan(5);
}

/* 当前是否有任意物理输入处于按下状态 (映射启动时等待松开用) */
bool bt_manager_any_phys_down(void)
{
    return bt_map_any_button_down();
}

/* 取一帧新物理按键边沿并写入 s_map[f].
 * 返回: 1=已映射, 0=无新按键.
 * 按用户要求: 不再做任何"按键占用/重复"阻止 —— 任意键都允许干净绑定. */
int bt_manager_poll_new_press(func_t f)
{
    if (f < 0 || f >= FUNC_MAX) return 0;
    int raw = bt_map_poll_press();
    if (raw < 0) return 0;
    phys_t p = raw_to_phys(raw);
    if (p == (phys_t)-1) return 0;
    s_map[f] = p;
    return 1;
}

void bt_manager_save_key_map(void)
{
    nvs_save_blob(NVS_KEY_MAP, s_map, sizeof(s_map));
}

void bt_manager_reset_key_map(void)
{
    memcpy(s_map, k_default_map, sizeof(s_map));
}

void bt_manager_set_key_map(func_t f, phys_t p)
{
    if (f < 0 || f >= FUNC_MAX) return;
    if (p < 0 || p >= PHYS_MAX) return;
    s_map[f] = p;
}

phys_t bt_manager_get_key_map(func_t f)
{
    if (f < 0 || f >= FUNC_MAX) return (phys_t)-1;
    return s_map[f];
}

/* ========================================================================
 *  补充映射 (F/G/Shift/空格 → phys)
 * ======================================================================== */
bool bt_manager_poll_sup_capture(phys_t *out)
{
    int raw = bt_map_poll_press();
    if (raw < 0) return false;
    phys_t p = raw_to_phys(raw);
    if (p == (phys_t)-1) return false;
    if (out) *out = p;
    return true;
}

bool bt_manager_is_sup_pressed(int idx)
{
    if (idx < 0 || idx >= SUP_MAX) return false;
    return phys_pressed(s_sup[idx]);
}

void bt_manager_set_sup_map(int idx, phys_t p)
{
    if (idx < 0 || idx >= SUP_MAX) return;
    if (p < 0 || p >= PHYS_MAX) return;
    s_sup[idx] = p;
}

phys_t bt_manager_get_sup_map(int idx)
{
    if (idx < 0 || idx >= SUP_MAX) return (phys_t)-1;
    return s_sup[idx];
}

void bt_manager_save_sup_map(void)
{
    nvs_save_blob(NVS_KEY_SUP, s_sup, sizeof(s_sup));
}

void bt_manager_reset_sup_map(void)
{
    memcpy(s_sup, k_default_sup, sizeof(s_sup));
}

/* ========================================================================
 *  GB 辅助映射 (SELECT/START → phys)
 * ======================================================================== */
bool bt_manager_poll_gb_capture(phys_t *out)
{
    int raw = bt_map_poll_press();
    if (raw < 0) return false;
    phys_t p = raw_to_phys(raw);
    if (p == (phys_t)-1) return false;
    if (out) *out = p;
    return true;
}

bool bt_manager_is_gb_pressed(int idx)
{
    if (idx < 0 || idx >= GB_MAP_MAX) return false;
    return phys_pressed(s_gb[idx]);
}

void bt_manager_set_gb_map(int idx, phys_t p)
{
    if (idx < 0 || idx >= GB_MAP_MAX) return;
    if (p < 0 || p >= PHYS_MAX) return;
    s_gb[idx] = p;
}

phys_t bt_manager_get_gb_map(int idx)
{
    if (idx < 0 || idx >= GB_MAP_MAX) return (phys_t)-1;
    return s_gb[idx];
}

void bt_manager_save_gb_map(void)
{
    nvs_save_blob(NVS_KEY_GB, s_gb, sizeof(s_gb));
}

void bt_manager_reset_gb_map(void)
{
    memcpy(s_gb, k_default_gb, sizeof(s_gb));
}

bool bt_manager_gb_map_set(void)
{
    return (s_gb[GB_SELECT] != (phys_t)-1) || (s_gb[GB_START] != (phys_t)-1);
}

/* ========================================================================
 *  连接记录 (历史)
 * ======================================================================== */
static void history_save(void)
{
    /* 只存紧凑前缀: 写满 array 大小, 后面用 hcnt 记录有效数 */
    uint8_t blob[sizeof(bt_device_t) * BT_HISTORY_MAX];
    memset(blob, 0, sizeof(blob));
    memcpy(blob, s_history, sizeof(s_history));
    nvs_save_blob(NVS_KEY_HIST, blob, sizeof(blob));
    nvs_save_blob(NVS_KEY_HCNT, &s_history_count, sizeof(s_history_count));
}

int bt_manager_get_history_count(void)
{
    return s_history_count;
}

const bt_device_t *bt_manager_get_history_at(int index)
{
    if (index < 0 || index >= s_history_count) return NULL;
    return &s_history[index];
}

bool bt_manager_remove_history_at(int index)
{
    if (index < 0 || index >= s_history_count) return false;
    if (index < s_history_count - 1) {
        memmove(&s_history[index], &s_history[index + 1],
                sizeof(bt_device_t) * (s_history_count - index - 1));
    }
    s_history_count--;
    history_save();
    return true;
}

void bt_manager_clear_paired(void)
{
    memset(s_history, 0, sizeof(s_history));
    s_history_count = 0;
    s_last_conn_addr_valid = false;   /* V1.5.x: 清除配对后不再自动回连上次设备 */
    s_rc_target_valid = false;
    s_auto_reconnect_pending = false;
    history_save();
}

bool bt_manager_is_paired_device(const uint8_t *bd_addr)
{
    if (!bd_addr) return false;
    for (int i = 0; i < s_history_count; i++) {
        if (memcmp(s_history[i].bd_addr, bd_addr, 6) == 0) return true;
    }
    return false;
}

void bt_manager_add_to_history(const bt_device_t *dev)
{
    if (!dev) return;

    /* 同名或同地址去重: 命中则更新并移到最前 (表示最近连接) */
    for (int i = 0; i < s_history_count; i++) {
        bool same_addr = (memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0);
        bool same_name = (dev->has_name && s_history[i].has_name &&
                          strncmp(s_history[i].name, dev->name, 32) == 0);
        if (same_addr || same_name) {
            memmove(&s_history[1], &s_history[0], sizeof(bt_device_t) * i);
            s_history[0] = *dev;
            history_save();
            return;
        }
    }

    /* 未去过重: 上限 4 条, 超出最旧 (尾部) 顶替 */
    if (s_history_count < BT_HISTORY_MAX) {
        memmove(&s_history[1], &s_history[0],
                sizeof(bt_device_t) * s_history_count);
        s_history_count++;
    } else {
        memmove(&s_history[1], &s_history[0],
                sizeof(bt_device_t) * (BT_HISTORY_MAX - 1));
    }
    s_history[0] = *dev;
    history_save();
}

bool bt_manager_get_connected_addr(uint8_t *out_addr6)
{
    if (!out_addr6 || !s_conn_addr_valid) return false;
    memcpy(out_addr6, s_conn_addr, 6);
    return true;
}

/* V1.5.x: 是否有可自动回连目标 (本次连接过 或 有最近历史; 扫描弹窗自动回连判定用) */
bool bt_manager_has_last_conn(void)
{
    return s_last_conn_addr_valid || s_history_count > 0;
}

/* ========================================================================
 *  校准
 * ======================================================================== */
void bt_manager_calibrate(void)
{
    bt_map_calibrate_start();
}

bool bt_manager_calibration_done(void)
{
    return bt_map_is_calibrated();
}

uint8_t bt_manager_calibration_neutral(void)
{
    return bt_map_neutral();
}

/* ========================================================================
 *  bt_core 桥接
 * ======================================================================== */
static void on_input_cb(esp_hidh_dev_t *dev, uint8_t report_id,
                        const uint8_t *data, uint16_t len)
{
    (void)dev;
    bt_pad_frame_t pad;
    memset(&pad, 0, sizeof(pad));
    bt_map_feed(report_id, data, len, &pad);
    /* 通用键盘: 若当前设备是键盘, 解析并(变化时)触发键盘回调 */
    bt_kbd_feed(data, len);
}

/* 默认键盘事件(第一步: 先打日志验证"透传原信号"通路; 页面/文本接线为后续) */
static void on_bt_kbd_event(const bt_kbd_state_t *st)
{
    if (!st) return;
    for (uint8_t i = 0; i < st->key_count; i++) {
        char ch = bt_kbd_hid_to_ascii(st->keys[i], st->modifiers);
        ESP_LOGI(TAG, "[kbd] press hid=0x%02x mods=0x%02x -> '%c'",
                 st->keys[i], st->modifiers, ch ? ch : '?');
    }
}

static void on_connected_cb(bool connected, esp_hidh_dev_t *dev)
{
    if (connected && dev) {
        s_connected = true;
        s_connecting = false;
        s_auto_reconnect_pending = false;

        /* 记录当前连接地址 */
        const uint8_t *bda = esp_hidh_dev_bda_get(dev);
        if (bda) {
            memcpy(s_conn_addr, bda, 6);
            memcpy(s_last_conn_addr, bda, 6);
            s_conn_addr_valid = true;
            s_last_conn_addr_valid = true;
        }
        s_last_connect_was_new = s_pending_new;
        s_pending_new = false;

        /* 填充连接信息 */
        memset(&s_conn_info, 0, sizeof(s_conn_info));
        s_conn_info.battery = 255;
        s_conn_info.vid = esp_hidh_dev_vendor_id_get(dev);
        s_conn_info.pid = esp_hidh_dev_product_id_get(dev);
        const char *nm = esp_hidh_dev_name_get(dev);
        if (!nm) nm = "";
        snprintf(s_conn_info.name, sizeof(s_conn_info.name), "%s", nm);
        snprintf(s_conn_info.model, sizeof(s_conn_info.model), "%s", nm);
        s_conn_info.available = true;

        /* 新手柄判定: 设备名与上次连接不同 → 视为换新手柄, 清空上一手柄的映射,
         * 避免旧按键配置错误套用到新设备 (重新连接同名设备则保留映射).
         * 注意: 判断"新"在更新 s_last_conn_name 之前进行. */
        bool is_new_pad = (!s_last_conn_name_valid ||
                           strncmp(nm, s_last_conn_name, sizeof(s_last_conn_name)) != 0);
        if (is_new_pad) {
            bt_manager_reset_key_map();
            bt_manager_reset_sup_map();
            bt_manager_reset_gb_map();
            bt_manager_save_key_map();
            bt_manager_save_sup_map();
            bt_manager_save_gb_map();
            ESP_LOGI(TAG, "连接新手柄[%s], 已清空并重置全部按键映射", nm);
        }
        snprintf(s_last_conn_name, sizeof(s_last_conn_name), "%s", nm);
        s_last_conn_name_valid = true;
        nvs_save_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));

        /* 解析该设备 report descriptor */
        size_t num_maps = 0;
        esp_hid_raw_report_map_t *maps = NULL;
        if (esp_hidh_dev_report_maps_get(dev, &num_maps, &maps) == ESP_OK &&
            num_maps > 0 && maps) {
            bt_map_set_report_map(maps[0].data, maps[0].len);
            /* 通用键盘: 同一描述符喂给 bt_kbd 判定是否为键盘(页0x07) */
            bt_kbd_from_descriptor(maps[0].data, maps[0].len);
            bt_kbd_set_cb(on_bt_kbd_event);   /* 默认透传日志; 后续可换 OS 接线 */
        }
        bt_map_calibrate_start();

        if (s_connect_cb) s_connect_cb(true);
    } else {
        s_connecting = false;
        if (s_connect_cb) s_connect_cb(false);
    }
}

static void on_disconnected_cb(bool connected, esp_hidh_dev_t *dev)
{
    (void)connected;
    (void)dev;
    s_connected = false;
    s_connecting = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
    /* 断连即快速重连: 复位退避到基础间隔 + 唤醒重连任务立即尝试, 不必等退避睡满.
     * 否则断开前若曾重连失败退避到 300s(5分钟), 断开后要干等 5 分钟才重试 →
     * 表现为"不会自动重连已连接的手柄". 掉线后 ~60s 内 (立即复位后任务被唤醒即试) 重连. */
    s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
    if (!s_power_off) bt_manager_wake_reconnect();   /* 非主动关机断连 → 立即重连 */
    if (s_connect_cb) s_connect_cb(false);
}

static void on_scan_done_cb(void)
{
    s_scanning = false;

    /* 链表 → 数组缓存 (仅保留有名字的设备, 屏蔽周边无名干扰项) */
    s_dev_cache_count = 0;
    esp_hid_scan_result_t *r = bt_core_last_scan();
    while (r && s_dev_cache_count < BT_SCAN_CACHE_MAX) {
        if (r->name && r->name[0] != '\0') {
            bt_device_t *d = &s_dev_cache[s_dev_cache_count];
            memset(d, 0, sizeof(*d));
            memcpy(d->bd_addr, r->bda, 6);
            d->addr_type = r->ble.addr_type;
            d->rssi = r->rssi;
            snprintf(d->name, sizeof(d->name), "%s", r->name);
            d->has_name = true;
            s_dev_cache_count++;
        }
        r = r->next;
    }

    /* 转发回调 */
    ESP_LOGI(TAG, "scan done: %d devices (first=%s)", s_dev_cache_count,
             s_dev_cache_count > 0 ? s_dev_cache[0].name : "-");
    if (s_scan_cb) {
        s_scan_cb(s_dev_cache, s_dev_cache_count);
    } else if (s_cont_cb) {
        s_cont_cb(s_dev_cache, s_dev_cache_count, true);
    }

    /* 连续扫描: 扫描完成即停, 不做无限重扫 (用户主动进列表页时一次即可;
     * 无限自旋会反复触发 esp_hid_scan 报 old-results, 造成扫描风暴/列表闪烁). */
    if (s_cont_cb && !s_connected && !s_suspended) {
        /* V1.5.x: 自动回连 — 长按确认键扫描窗口内挂了回连标志时, 在本次结果里
         * 匹配回连目标地址 (本次连接过/最近历史), 命中即直接连接 (无需用户点选);
         * 未命中则照常列表. */
        if (s_auto_reconnect_pending && s_rc_target_valid) {
            esp_hid_scan_result_t *rr = bt_core_last_scan();
            while (rr) {
                if (memcmp(rr->bda, s_rc_target_addr, 6) == 0) {
                    s_pending_new = !bt_manager_is_paired_device(s_rc_target_addr);
                    s_connecting = true;
                    s_auto_reconnect_pending = false;   /* 已发起: 结果由 on_connected 收敛 */
                    s_rc_target_valid = false;
                    bt_core_connect(rr);                /* esp_hidh_dev_open 异步, 安全 */
                    return;   /* 不再停扫/回列表 (连接成功由上层 0.5s 提示收敛) */
                }
                rr = rr->next;
            }
            s_auto_reconnect_pending = false;   /* 结果里没有回连目标: 放弃自动回连, 列表照常 */
            s_rc_target_valid = false;
        }
        /* 不再自动重扫, 保持一次扫描结果, 交由用户在列表点选连接 */
        bt_core_stop_scan();
        s_scanning = false;
        return;
    }

    /* 自动重连: 未连接且挂着目标地址则尝试匹配 */
    if (s_auto_reconnect_pending && !s_connected && !s_suspended) {
        r = bt_core_last_scan();
        while (r) {
            if (memcmp(r->bda, s_last_conn_addr, 6) == 0) {
                s_pending_new = !bt_manager_is_paired_device(s_last_conn_addr);
                s_connecting = true;
                bt_core_connect(r);
                break;
            }
            r = r->next;
        }
        if (!s_connected && !s_connecting) {
            /* 没找到目标设备, 暂停自动重连, 避免反复扫描 */
            s_auto_reconnect_pending = false;
        }
        return;
    }
}

/* 扫描过程中每发现/更新一个 BLE 设备 (esp_hid_gap 实时回调, 蓝牙栈事件上下文):
 * 追加/更新缓存并即时通知 UI, 让设备"扫到就显示", 不必等 5 秒扫描结束.
 * 只在连续扫描(列表 UI)进行中推送; 无名设备不显示 (名字补到时会再次回调). */
static void on_scan_result_cb(const esp_hid_scan_result_t *r)
{
    if (!r) return;
    if (!r->name || r->name[0] == '\0') return;   /* 与完成时一致: 无名设备不显示 */
    if (!s_cont_cb || !s_scanning) return;        /* 仅列表 UI 场景推送 */

    /* 查重: 已存在则仅补名字 (此前无名) / 更新 RSSI, 不重复推送 */
    for (int i = 0; i < s_dev_cache_count; i++) {
        if (memcmp(s_dev_cache[i].bd_addr, r->bda, 6) == 0) {
            s_dev_cache[i].rssi = r->rssi;
            if (!s_dev_cache[i].has_name) {
                snprintf(s_dev_cache[i].name, sizeof(s_dev_cache[i].name), "%s", r->name);
                s_dev_cache[i].has_name = true;
                if (s_cont_cb) s_cont_cb(s_dev_cache, s_dev_cache_count, true);
            }
            return;
        }
    }

    /* 新设备: 追加并即时通知 UI */
    if (s_dev_cache_count < BT_SCAN_CACHE_MAX) {
        bt_device_t *d = &s_dev_cache[s_dev_cache_count];
        memset(d, 0, sizeof(*d));
        memcpy(d->bd_addr, r->bda, 6);
        d->addr_type = r->ble.addr_type;
        d->rssi = r->rssi;
        snprintf(d->name, sizeof(d->name), "%s", r->name);
        d->has_name = true;
        s_dev_cache_count++;
        if (s_cont_cb) s_cont_cb(s_dev_cache, s_dev_cache_count, true);
    }
}

/* 确保 bt_core 只初始化一次 */
static bool core_ensure_init(void)
{
    if (s_inited) return bt_core_is_ready();

    static bt_core_cbs_t s_core_cbs = {
        .on_input       = on_input_cb,
        .on_connected   = on_connected_cb,
        .on_disconnected= on_disconnected_cb,
        .on_scan_done   = on_scan_done_cb,
    };

    if (bt_core_init(&s_core_cbs)) {
        s_inited = true;
        /* 注册扫描过程的实时设备回调 (扫到即显示) */
        esp_hid_scan_set_result_cb(on_scan_result_cb);
        return true;
    }
    return false;
}

/* ========================================================================
 *  生命周期 / 扫描 / 连接 / 状态
 * ======================================================================== */
/* ========================================================================
 *  完整初始化 (V1.5.x 拆出, 供 bt_manager_init 任务入口 与 enable 惰性触发共用)
 *  幂等: 只执行一次; disable 后再次 enable 由 core_ensure_init 重新初始化核心.
 * ======================================================================== */
static void bt_full_init(void)
{
    if (s_full_init_done) return;
    s_full_init_done = true;   /* 先置位防重入 (初始化任务/主循环并发) */
    ESP_LOGI(TAG, "bt_full_init: 首次启用, 载入配置并初始化核心");

    /* 默认映射 */
    memcpy(s_map, k_default_map, sizeof(s_map));
    memcpy(s_sup, k_default_sup, sizeof(s_sup));
    memcpy(s_gb, k_default_gb, sizeof(s_gb));

    /* 载入存档 (无则用默认) */
    if (!nvs_load_blob(NVS_KEY_MAP, s_map, sizeof(s_map))) {
        memcpy(s_map, k_default_map, sizeof(s_map));
    }
    if (!nvs_load_blob(NVS_KEY_SUP, s_sup, sizeof(s_sup))) {
        memcpy(s_sup, k_default_sup, sizeof(s_sup));
    }
    if (!nvs_load_blob(NVS_KEY_GB, s_gb, sizeof(s_gb))) {
        memcpy(s_gb, k_default_gb, sizeof(s_gb));
    }

    uint8_t hist_blob[sizeof(s_history)];
    if (nvs_load_blob(NVS_KEY_HIST, hist_blob, sizeof(hist_blob))) {
        memcpy(s_history, hist_blob, sizeof(s_history));
    } else {
        memset(s_history, 0, sizeof(s_history));
    }
    s_history_count = 0;
    /* 修复: hcnt 是以 nvs_set_blob(4字节) 保存的, 若用 nvs_get_u8 读则类型不匹配,
     * 重启后永远读不到条数 → 连接记录为空、自动回连找不到设备. 改用 blob 读取. */
    int hcnt = 0;
    size_t hcnt_len = sizeof(hcnt);
    nvs_init_handle();
    if (s_nvs_open && nvs_get_blob(s_nvs, NVS_KEY_HCNT, &hcnt, &hcnt_len) == ESP_OK) {
        s_history_count = hcnt;
        if (s_history_count < 0) s_history_count = 0;
        if (s_history_count > BT_HISTORY_MAX) s_history_count = BT_HISTORY_MAX;
    }

    /* 载入上次连接设备名 (判断新手柄用) */
    s_last_conn_name_valid = nvs_load_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));
    if (!s_last_conn_name_valid) s_last_conn_name[0] = '\0';

    nvs_init_handle();
    (void)core_ensure_init();

    /* 常驻自动回连任务 (低优先级, 省电直连上次手柄). 栈须够大: 回连会走 esp_hid/Bluedroid
     * 连接与错误处理, 2048 过小会在 HID open 失败/重连时爆栈重启 (实测 bt_rec stack overflow). */
    xTaskCreate(bt_reconnect_task, "bt_rec", 8192, NULL, 2, &s_reconnect_task);
}

/* 立即唤醒重连任务: 断连/需要即时重连时调用 (任务正按退避睡眠也能打断).
 * 无任务(未初始化)时静默忽略. */
void bt_manager_wake_reconnect(void)
{
    if (s_reconnect_task) xTaskNotifyGive(s_reconnect_task);
}

void bt_manager_init(void)
{
    ESP_LOGI(TAG, "bt_manager_init enter (bt_init task)");
    bt_full_init();
    if (s_init_done_sem) xSemaphoreGive(s_init_done_sem);   /* 通知 enable 等待方 */

    /* 本函数被 bt_manager_enable 作为 "bt_init" 任务入口运行. FreeRTOS 任务函数禁止返回,
     * 完成初始化后必须删除自身 (否则报 "Task should not return" abort 重启). */
    vTaskDelete(NULL);
}

/* ========================================================================
 *  初始化任务静态栈 (内部 RAM)
 *  app_board.c 开机时用 xTaskCreateStatic 以 bt_manager_init 作入口创建 bt_init 任务.
 *  bt_manager_init 会调用 esp_bt_controller_init/esp_hid_gap_init, 期间禁用 cache,
 *  故该栈 MUST 放内部 RAM (PSRAM 栈会断言重启). 由 get_init_stack 共享给 app_board.
 * ======================================================================== */
#define INIT_TASK_STACK_WORDS 4096   /* 16KB, 内部 RAM */
static StackType_t s_init_task_stack[INIT_TASK_STACK_WORDS];

StackType_t *bt_manager_get_init_stack(int *words_out)
{
    if (words_out) *words_out = INIT_TASK_STACK_WORDS;
    return s_init_task_stack;
}

const char *bt_manager_get_status(void)
{
    static char buf[48];
    if (s_suspended) {
        return "已挂起";
    } else if (s_connected) {
        snprintf(buf, sizeof(buf), "已连接: %s", s_conn_info.name);
        return buf;
    } else if (s_scanning) {
        return "扫描中...";
    }
    return "未连接";
}

void bt_manager_start_scan(bt_scan_callback_t callback)
{
    s_scan_cb = callback;
    s_cont_cb = NULL;
    s_scanning = true;
    bt_core_scan(5);
}

void bt_manager_start_scan_continuous(bt_device_found_cb_t callback)
{
    s_cont_cb = callback;
    s_scan_cb = NULL;
    s_scanning = true;
    bt_core_scan(5);
}

bool bt_manager_is_scanning(void)
{
    return s_scanning || bt_core_is_scanning();
}

void bt_manager_stop_scan(void)
{
    bt_core_stop_scan();
    s_scanning = false;
    s_auto_reconnect_pending = false;
}

bool bt_manager_is_connected(void)
{
    return s_connected;
}

int bt_manager_get_scan_results(bt_device_t **results)
{
    if (results) *results = s_dev_cache;
    return s_dev_cache_count;
}

const char *bt_manager_get_device_name(const bt_device_t *dev)
{
    if (!dev) return "";
    return dev->name;
}

const char *bt_manager_get_connected_device_name(void)
{
    if (!s_connected) return NULL;
    return s_conn_info.name;
}

void bt_manager_reset_input_state(void)
{
    bt_map_calibrate_start();
}

void bt_manager_reset_pad_clean(const bt_device_t *dev)
{
    if (!dev) return;
    /* 删除与该设备同名/同地址的历史记录 */
    for (int i = s_history_count - 1; i >= 0; i--) {
        bool same_addr = memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0;
        bool same_name = (dev->has_name && s_history[i].has_name &&
                          strncmp(s_history[i].name, dev->name, 32) == 0);
        if (same_addr || same_name) bt_manager_remove_history_at(i);
    }
    /* 重置其按键配置为默认并保存 (清掉旧手柄可能带进来的脏配置) */
    bt_manager_reset_key_map();
    bt_manager_reset_sup_map();
    bt_manager_reset_gb_map();
    bt_manager_save_key_map();
    bt_manager_save_sup_map();
    bt_manager_save_gb_map();
    /* 清除输入残留状态: 防"不按任何键映射就自动完成"的幽灵输入 */
    bt_map_calibrate_start();
    ESP_LOGI(TAG, "[clean] 已清理同名[%s]历史记录与配置", dev->name[0] ? dev->name : "?");
}

bool bt_manager_connect_device(const bt_device_t *dev)
{
    if (!dev) return false;

    /* 在最近一次扫描结果里按地址匹配 */
    esp_hid_scan_result_t *r = bt_core_last_scan();
    while (r) {
        if (memcmp(r->bda, dev->bd_addr, 6) == 0) {
            s_pending_new = !bt_manager_is_paired_device(dev->bd_addr);
            s_last_connect_was_new = s_pending_new;
            s_connecting = true;
            bt_manager_add_to_history(dev);
            return bt_core_connect(r);
        }
        r = r->next;
    }

    /* 扫描任务尚未完成 (s_scan_results 为空) 时, 实时列表里已点选的设备同样允许连接:
     * 用缓存中的地址/类型直接发起连接, 避免"扫到即点即连"在扫描结束前失败.
     * 连接层 esp_hidh_dev_open 会做真正的服务发现, 失败也由上层正常提示. */
    esp_hid_scan_result_t fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.bda, dev->bd_addr, 6);
    fake.transport = ESP_HID_TRANSPORT_BLE;
    fake.ble.addr_type = dev->addr_type;
    s_pending_new = !bt_manager_is_paired_device(dev->bd_addr);
    s_last_connect_was_new = s_pending_new;
    s_connecting = true;
    bt_manager_add_to_history(dev);
    return bt_core_connect(&fake);
}

void bt_manager_disconnect(void)
{
    s_connecting = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
    bt_core_disconnect();
}

const btm_conn_info_t *bt_manager_get_conn_info(void)
{
    if (!s_connected || !s_conn_info.available) return NULL;
    return &s_conn_info;
}

void bt_manager_set_connect_callback(bt_connect_cb_t cb)
{
    s_connect_cb = cb;
}

void bt_manager_set_connect_progress_cb(bt_connect_progress_cb_t cb)
{
    s_prog_cb = cb;
}

const char *bt_manager_get_connect_error(void)
{
    return "连接失败，请重试";
}

bool bt_manager_last_connect_was_new(void)
{
    return s_last_connect_was_new;
}

bool bt_manager_is_connecting(void)
{
    return s_connecting;
}

void bt_manager_cancel_connect(void)
{
    s_connecting = false;
    s_auto_reconnect_pending = false;
    bt_core_disconnect();
}

static bool s_bt_on = false;   /* V1.6.x: 蓝牙是否已启用 (供键盘↔手柄模式切换记录原状态) */
bool bt_manager_is_enabled(void) { return s_bt_on; }   /* V1.6.x: 蓝牙当前是否启用 */

void bt_manager_enable(void)
{
    s_bt_on = true;
    s_power_off = false;   /* 显式开蓝牙: 允许自动回连 */
    s_suspended = false;
    /* V1.5.x 省电: 蓝牙按需开启 — 首次 enable 才做完整初始化 (映射/NVS/核心/回连任务).
     * 初始化任务用内部 RAM 静态栈 (esp_bt_controller_init 期间禁 cache, PSRAM 栈会断言),
     * 这里同步等待其完成 (蓝牙栈启动约 1~2 秒), 避免上层紧接着扫描时核心未就绪. */
    if (!s_full_init_done) {
        int words = 0;
        StackType_t *stack = bt_manager_get_init_stack(&words);
        static StaticTask_t bt_task_buf;
        if (stack && xTaskCreateStatic((TaskFunction_t)bt_manager_init, "bt_init",
                                       words, NULL, 1, stack, &bt_task_buf)) {
            if (!s_init_done_sem) s_init_done_sem = xSemaphoreCreateBinary();
            if (s_init_done_sem) xSemaphoreTake(s_init_done_sem, pdMS_TO_TICKS(20000));
        }
        if (!s_full_init_done) ESP_LOGW(TAG, "蓝牙完整初始化未完成 (超时/失败), 下次 enable 重试核心");
    }
    (void)core_ensure_init();   /* disable→enable 再启用时核心重新初始化 */
}

void bt_manager_disable(void)
{
    s_bt_on = false;
    s_power_off = true;    /* 彻底关蓝牙: 自动回连任务停手, 不再偷偷初始化 */
    bt_core_disconnect();
    bt_core_stop_scan();
    s_connected = false;
    s_connecting = false;
    s_scanning = false;
    s_suspended = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
    s_inited = false;
    bt_core_deinit();
}

/* ========================================================================
 *  自动回连任务 (定义见文件头; 退避 60s→120s→240s→封顶300s)
 *  V1.5.x 省电: 回连扫描间隔 5s→60s, 大幅降低空闲期无效重试功耗.
 * ======================================================================== */
/* V1.5.x: 开机自动回连窗口 — 开机时若有已配对手柄, 短窗口内低占空比直连上次手柄,
 * 连上即保持; 窗口超时未连上 → 关栈回按需省电. 由 main.c 开机时启动. */
static volatile bool s_boot_win_active = false;
static uint32_t s_boot_win_end_ms = 0;

/* 直连最近一次手柄 (esp_hidh_dev_open 按已知地址, 无需扫描, 无线电占用极低) */
static bool bt_try_reconnect_now(void)
{
    if (s_history_count <= 0) return false;
    const bt_device_t *d = &s_history[0];   /* s_history[0] = 最近连接 */
    esp_hid_scan_result_t fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.bda, d->bd_addr, 6);
    fake.transport = ESP_HID_TRANSPORT_BLE;
    fake.ble.addr_type = d->addr_type;
    s_connecting = true;
    if (!bt_core_connect(&fake)) {
        s_connecting = false;   /* 立即失败(设备不在): 复位连接中状态 */
        return false;
    }
    return true;   /* 成功: 状态由 on_connected 回调收敛 */
}

static void bt_reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 可唤醒睡眠: 断连/开机时发通知立即醒来重连, 不必干等当前退避 (最长300s) 睡满 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_reconnect_delay_ms));
        /* 条件: 已连接/正在连/在扫/被挂起/关电/无历史 → 都不自动回连 */
        if (s_connected || s_connecting || s_suspended || s_power_off ||
            bt_manager_is_scanning() || s_history_count <= 0) {
            if (!s_boot_win_active || s_connected || s_connecting || s_power_off)
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;   /* 恢复基础间隔 */
            continue;
        }

        /* V1.5.x: 开机自动回连窗口 — 短窗口内低占空比(3s)直连上次手柄.
         * 连上即保持; 窗口超时仍未连上 → 关栈回按需省电 (开机场景, 手柄不在附近).
         * 若超时瞬间用户已进蓝牙页扫描/使用中 → 保持开启, 不误关. */
        if (s_boot_win_active) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms >= s_boot_win_end_ms) {
                s_boot_win_active = false;
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                if (!s_scanning && !bt_core_is_scanning() && !s_scan_cb && !s_cont_cb) {
                    ESP_LOGI(TAG, "开机回连窗口超时: 手柄不在附近, 蓝牙休眠 (按需开启)");
                    bt_manager_disable();
                } else {
                    ESP_LOGI(TAG, "开机回连窗口超时: 用户正在使用蓝牙, 保持开启");
                }
                continue;
            }
        }

        if (!core_ensure_init()) continue;
        if (bt_try_reconnect_now()) {
            s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;   /* 连上/连上中 → 回基础间隔 */
            s_boot_win_active = false;                     /* 连上即结束开机窗口 */
        } else {
            if (s_boot_win_active) {
                s_reconnect_delay_ms = BT_BOOT_RECONNECT_POLL_MS;  /* 窗口内 3s 低占空比重试 */
            } else {
                /* 失败 → 指数退避, 封顶, 减少无效重试功耗 */
                s_reconnect_delay_ms *= 2;
                if (s_reconnect_delay_ms > BT_RECONNECT_MAX_MS) s_reconnect_delay_ms = BT_RECONNECT_MAX_MS;
            }
        }
    }
}

/* V1.5.x: 开机自动回连窗口 — 启动入口 (main.c 开机时调用).
 * 先轻量预检 NVS 历史 (不初始化蓝牙核心): 无已配对手柄 → 完全不拉起蓝牙栈 (省电).
 * 有历史 → 惰性完整初始化, 在 window_ms 窗口内低占空比直连上次手柄;
 * 窗口超时未连上 → bt_reconnect_task 自动 disable 回按需省电.
 * 已在连接中 → 直接跳过窗口. */
void bt_manager_start_boot_reconnect(uint32_t window_ms)
{
    /* 轻量预检: NVS 历史条数 (hcnt 是 nvs_set_blob 保存的 4 字节, 用 blob 读) */
    int hcnt = 0;
    {
        size_t len = sizeof(hcnt);
        nvs_init_handle();
        if (s_nvs_open && nvs_get_blob(s_nvs, NVS_KEY_HCNT, &hcnt, &len) == ESP_OK) {
            if (hcnt < 0) hcnt = 0;
            if (hcnt > BT_HISTORY_MAX) hcnt = BT_HISTORY_MAX;
        }
    }
    if (hcnt <= 0) {
        ESP_LOGI(TAG, "开机回连: 无已配对手柄历史, 蓝牙不启动 (按需开启)");
        return;
    }
    if (s_connected || s_connecting) {
        ESP_LOGI(TAG, "开机回连: 已连接/连接中, 跳过窗口");
        return;
    }
    bt_manager_enable();   /* 惰性完整初始化 (映射/NVS/核心/回连任务) */
    if (!s_full_init_done) {
        ESP_LOGW(TAG, "开机回连: 完整初始化未完成, 窗口跳过");
        return;
    }
    if (window_ms == 0) window_ms = BT_BOOT_RECONNECT_WINDOW_MS;
    s_boot_win_active = true;
    s_boot_win_end_ms = (uint32_t)(esp_timer_get_time() / 1000) + window_ms;
    s_reconnect_delay_ms = BT_BOOT_RECONNECT_POLL_MS;   /* 立即进入低占空比重试节奏 */
    ESP_LOGI(TAG, "开机回连窗口启动: %u ms 低占空比直连上次手柄", (unsigned)window_ms);
    bt_try_reconnect_now();   /* 手柄在身边: 立即直连, 不必等 3s */
}

bool bt_manager_is_ready(void)
{
    return bt_core_is_ready();
}

bool bt_manager_is_stack_ready(void)
{
    return bt_core_is_ready();
}

void bt_manager_suspend(void)
{
    s_suspended = true;
    bt_core_stop_scan();
    s_scanning = false;
    bt_core_disconnect();
    s_connected = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
}

void bt_manager_resume(void)
{
    (void)core_ensure_init();
    s_suspended = false;
}

bool bt_manager_is_suspended(void)
{
    return s_suspended;
}