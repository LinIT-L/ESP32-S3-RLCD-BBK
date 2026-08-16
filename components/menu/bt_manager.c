#include "bt_manager.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_hidh.h"
#include "esp_hidh_bluedroid.h"
#include "esp_hidh_gattc.h"
#include "esp_hid_common.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "BT_MANAGER"

/* ========================================================================
 *  V1.0.38 按键映射核心
 *  - s_hat: 摇杆 4 向状态 (0..7=8 向, 8=居中, Q36 偏移编码: raw=0/8/0xF → 居中)
 *  - s_btn: 8 按钮 (bit 0=P_A, bit 1=P_B, ..., bit 7=P_R2)
 *  - s_map[FUNC_MAX]: 8 功能 → 12 物理输入
 *  - s_consumed_*: 上次消费的边沿 (用于 poll_new_press 边沿检测)
 *  - s_pending_press: 待消费的新按下物理输入 (12 bits, bit=phys_t)
 * ======================================================================== */
static uint8_t  s_hat = 8;     /* 0..7=8 向, 8=居中 */
static uint16_t s_btn = 0;     /* 10 按钮位 (bit0..7=A..R2, bit8=SELECT, bit9=START) */
static phys_t   s_map[FUNC_MAX];
/* 补充按键映射 (4 个功能 → 12 个物理输入), 与 s_map 独立存储 */
static phys_t   s_sup_map[SUP_MAX];
/* GB 辅助按键映射 (2 个功能: SELECT/START → 12 个物理输入), 与 s_map/s_sup_map 独立存储.
 * 只在 GB 游戏二级菜单及游戏中生效 (input.c 的 GB joypad 投递). */
static phys_t   s_gb_map[GB_MAP_MAX];

/* 上次消费的原始状态 (用于边沿检测) */
static uint8_t  s_consumed_hat = 8;
static uint16_t s_consumed_btn = 0;
/* 待消费的物理输入 (14 bits). 由 on_hid_input 累加, 由 poll_new_press 消费 */
static uint16_t s_pending_press = 0;

/* 默认映射 (与 bt_manager.h 中 F_* 顺序一致) */
static const phys_t DEFAULT_MAP[FUNC_MAX] = {
    P_HAT_UP,    /* F_UP       → 摇杆上 */
    P_HAT_DOWN,  /* F_DOWN     → 摇杆下 */
    P_HAT_LEFT,  /* F_LEFT     → 摇杆左 */
    P_HAT_RIGHT, /* F_RIGHT    → 摇杆右 */
    P_B,         /* F_CONFIRM  → 按钮 B */
    P_A,         /* F_BACK     → 按钮 A */
    P_L2,        /* F_EXIT     → 肩键 L2 */
    P_R2,        /* F_FAV      → 肩键 R2 */
};

const char *bt_manager_phys_name(phys_t p) {
    static const char *names[PHYS_MAX] = {
        "摇杆上", "摇杆下", "摇杆左", "摇杆右",
        "A 键", "B 键", "X 键", "Y 键",
        "L1 键", "R1 键", "L2 键", "R2 键",
        "Select 键", "Start 键",
    };
    if (p < 0 || p >= PHYS_MAX) return "?";
    return names[p];
}

const char *bt_manager_func_name(func_t f) {
    static const char *names[FUNC_MAX] = {
        "\xe4\xb8\x8a",         /* 上 */
        "\xe4\xb8\x8b",         /* 下 */
        "\xe5\xb7\xa6",         /* 左 */
        "\xe5\x8f\xb3",         /* 右 */
        "\xe7\xa1\xae\xe5\xae\x9a",         /* 确定 */
        "\xe8\xbf\x94\xe5\x9b\x9e",         /* 返回 */
        "\xe8\xbf\x94\xe5\x9b\x9e\xe8\x8f\x9c\xe5\x8d\x95", /* 返回菜单 */
        "\xe5\xa4\x9a\xe5\x8a\x9f\xe8\x83\xbd\xe9\x94\xae", /* 多功能键 */
        "Start",
        "Select",
    };
    if (f < 0 || f >= FUNC_MAX) return "?";
    return names[f];
}

/* 8 方向 (0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW) */
/* V1.0.39: 摇杆强制 4 方向 — 每个 8 向值只映射到 1 个 4 方向值, 移动一次只触发一次.
 * 映射规则: N→UP, NE→RIGHT, E→RIGHT, SE→DOWN, S→DOWN, SW→LEFT, W→LEFT, NW→UP */
static inline bool hat_is(int dir) {
    if (s_hat >= 8) return false;  /* 居中 */
    static const int map4[8] = {0, 1, 1, 2, 2, 3, 3, 0};  /* dir: 0=UP, 1=RIGHT, 2=DOWN, 3=LEFT */
    return map4[s_hat] == dir;
}

/* ========================================================================
 *  NVS 按键映射 (8 字节)
 * ======================================================================== */
#define NVS_NS "bt_pair"
#define NVS_KEY_ADDR "addr"
#define NVS_KEY_TYPE "atype"
#define NVS_KEY_NAME "name"
#define NVS_KEY_MAP  "keymap"
#define NVS_KEY_SUP  "supmap"   /* 补充按键映射 (4 字节) */
#define NVS_KEY_GB   "gbmap"    /* GB 辅助按键映射 (2 字节) */

#define MAX_SCAN_RESULTS 20
/* V1.0.53: 2560→3584 (14KB): HID descriptor 解析 + 主动连接日志栈溢出导致
 * 整机重启 (bt_conn task overflow). PSRAM 静态栈, 不占内部 RAM. */
#define CONNECT_TASK_STACK_WORDS 3584
EXT_RAM_BSS_ATTR static StackType_t s_connect_task_stack[CONNECT_TASK_STACK_WORDS];
static StaticTask_t s_connect_task_buf;
#define INIT_TASK_STACK_WORDS 4096
EXT_RAM_BSS_ATTR static StackType_t s_init_task_stack[INIT_TASK_STACK_WORDS];
static StaticTask_t s_init_task_buf;

static bt_device_t s_scan_results[MAX_SCAN_RESULTS];
static int s_scan_count = 0;
static bool s_scanning = false;
static bool s_connected = false;
static bool s_scan_params_ready = false;
static bool s_pending_start_scan = false;
static bt_scan_callback_t s_scan_callback = NULL;
static bt_device_found_cb_t s_device_found_cb = NULL;

/* HID 设备数组 (供 connect/disconnect 管理) */
#define BT_HID_DEV_MAX  4
static esp_hidh_dev_t *s_hid_devs[BT_HID_DEV_MAX] = {NULL};
static int s_hid_dev_count = 0;
static uint8_t s_hid_devs_bda[BT_HID_DEV_MAX][6];
static bool    s_hid_devs_bda_set[BT_HID_DEV_MAX] = {false};

static char s_connected_dev_name[32] = "";
static const char *s_init_status = "未初始化";
static bool s_hidh_ready = false;
static bool s_initializing = false;
static bt_device_t s_pending_dev;
static bool s_has_pending_dev = false;
static bool s_connect_cancel = false;
static volatile bool s_open_failed = false;
static volatile bool s_last_connect_was_new = false;
static bt_connect_cb_t s_connect_cb = NULL;
static bt_connect_progress_cb_t s_connect_progress_cb = NULL;
static SemaphoreHandle_t s_bt_mutex = NULL;
static char s_connect_error[48] = "";
static uint8_t s_paired_addr[6] = {0};
static uint8_t s_paired_addr_type = 0;
static char   s_paired_name[32] = "";
static bool   s_has_paired = false;
static bool   s_auto_connect_pending = false;
static uint32_t s_last_disconnect_ms = 0;   /* 断开时刻, 用于重连冷却 (防协议栈崩溃) */
static bool   s_ctrl_init = false;          /* 控制器当前是否已 init (防重复 deinit) */
static bool   s_suspended = false;          /* 屏保挂起: 断开连接且禁止自动重连 */
static bt_device_t s_history[BT_HISTORY_MAX];
static int         s_history_count = 0;

/* 手柄导航开关 (映射流程中关闭) */
static bool s_nav_enabled = false;
static uint32_t s_nav_arm_until = 0;
#define NAV_ARM_MS 1200

static void bt_lock(void) {
    if (s_bt_mutex) xSemaphoreTake(s_bt_mutex, portMAX_DELAY);
}
static void bt_unlock(void) {
    if (s_bt_mutex) xSemaphoreGive(s_bt_mutex);
}

static void bt_manager_load_key_map(void);
static void bt_manager_load_paired(void);
static void bt_manager_save_paired(const bt_device_t *dev);
static void bt_manager_start_auto_connect(void);
static void bt_autoreconnect_task(void *arg);  /* V1.0.46: 前向声明 */
static void bt_manager_load_history_from_nvs(void);
static void bt_manager_save_history_to_nvs(void);

static int find_device_by_addr(const uint8_t *bda) {
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_results[i].bd_addr, bda, 6) == 0) return i;
    }
    return -1;
}
static int find_hid_dev_index(esp_hidh_dev_t *dev) {
    if (!dev) return -1;
    for (int i = 0; i < BT_HID_DEV_MAX; i++) {
        if (s_hid_devs[i] == dev) return i;
    }
    return -1;
}
static int find_free_hid_slot(void) {
    for (int i = 0; i < BT_HID_DEV_MAX; i++) {
        if (s_hid_devs[i] == NULL) return i;
    }
    return -1;
}

/* ========================================================================
 *  GAP 事件 - V1.0.45 完全恢复修改前的原始行为: 只处理 SCAN 相关事件
 *  (之前一两秒能连上时, GAP 回调就只有下面这 2 个 case)
 *  SMP 事件完全不处理 = 交给 Bluedroid 默认流程, 避免手动 reply 签名错误
 *  导致协议栈卡住 / 手柄断开. 如果某个手柄确实主动发起 SMP, 由栈默认
 *  (NoInputNoOutput + Just Works) 自动走. ======================================================================== */
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                if (s_scan_count >= MAX_SCAN_RESULTS) break;
                if (param->scan_rst.rssi < -90) break;
                uint8_t *bda = param->scan_rst.bda;
                int idx = find_device_by_addr(bda);
                if (idx >= 0) {
                    s_scan_results[idx].rssi = param->scan_rst.rssi;
                    if (s_device_found_cb) s_device_found_cb(s_scan_results, s_scan_count, true);
                    break;
                }
                uint8_t name_len = 0;
                uint8_t *name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                         ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
                if (!name) name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv,
                                                          ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
                if (!name || name_len == 0) break;
                bt_device_t *dev = &s_scan_results[s_scan_count];
                memcpy(dev->bd_addr, bda, 6);
                dev->addr_type = param->scan_rst.ble_addr_type;
                dev->rssi = param->scan_rst.rssi;
                dev->has_name = true;
                int copy_len = name_len < (int)sizeof(dev->name) - 1 ? name_len : (int)sizeof(dev->name) - 1;
                memcpy(dev->name, name, copy_len);
                dev->name[copy_len] = '\0';
                s_scan_count++;
                ESP_LOGI(TAG, "发现设备: %s (%ddBm)", dev->name, dev->rssi);
                if (s_has_paired && !s_connected && !s_has_pending_dev) {
                    bool addr_match = (memcmp(dev->bd_addr, s_paired_addr, 6) == 0);
                    /* 部分 BLE 手柄每次连接会随机化 MAC (地址变、名称不变),
                     * 仅靠 MAC 匹配会导致"一直连不上"。这里额外按名称匹配：
                     * 若发现同名设备但 MAC 不同, 用新 MAC 更新配对信息后重连. */
                    bool name_match = (s_paired_name[0] != '\0'
                                       && strcmp(dev->name, s_paired_name) == 0);
                    if (addr_match || name_match) {
                        if (!addr_match && name_match) {
                            ESP_LOGI(TAG, "检测到同名设备(新MAC), 更新配对并重连: %s", dev->name);
                            bt_manager_save_paired(dev);
                        } else {
                            ESP_LOGI(TAG, "检测到已配对设备, 自动重连: %s", dev->name);
                        }
                        bt_manager_start_auto_connect();
                    }
                }
                if (s_device_found_cb) s_device_found_cb(s_scan_results, s_scan_count, false);
            } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                if (s_scanning) esp_ble_gap_start_scanning(0);
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
            s_scan_params_ready = true;
            if (s_pending_start_scan) {
                s_pending_start_scan = false;
                esp_ble_gap_start_scanning(0);
            }
            break;
        }
        default: break;
    }
}

/* ========================================================================
 *  HIDH 事件 - V1.0.38 大幅简化: 只听 report_id=4, 直接解码到 s_hat/s_btn
 * ======================================================================== */
static void hidh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    esp_hidh_event_data_t *data = (esp_hidh_event_data_t *)event_data;
    switch (event_id) {
        case ESP_HIDH_OPEN_EVENT: {
            if (data->open.status != ESP_OK) {
                ESP_LOGE(TAG, "HID 设备打开失败, status=%d", data->open.status);
                bt_lock();
                s_connected = false;
                bt_unlock();
                snprintf(s_connect_error, sizeof(s_connect_error),
                         "连接失败\n(status=%d)", data->open.status);
                s_open_failed = true;
            } else {
                if (s_connect_cancel) {
                    ESP_LOGI(TAG, "OPEN 成功但用户已取消, 关闭设备 %p", (void*)data->open.dev);
                    esp_hidh_dev_close(data->open.dev);
                    break;
                }
                ESP_LOGI(TAG, "HID 设备已打开 (dev=%p)", (void*)data->open.dev);
                bt_lock();
                int slot = find_free_hid_slot();
                if (slot < 0) {
                    bt_unlock();
                    esp_hidh_dev_close(data->open.dev);
                    break;
                }
                s_hid_devs[slot] = data->open.dev;
                s_hid_dev_count++;
                s_connected = (s_hid_dev_count > 0);
                memcpy(s_hid_devs_bda[slot], s_pending_dev.bd_addr, 6);
                s_hid_devs_bda_set[slot] = true;
                /* 进游戏等高负载会短暂抢占 CPU; HID 链路监督超时过短会被误断.
                 * 连接建立后请求把监督超时拉到 20s, 短时卡顿不再掉线. */
                esp_ble_conn_update_params_t cparams = {
                    .min_int = 0x06,   /* 7.5ms */
                    .max_int = 0x18,   /* 30ms */
                    .latency = 0,
                    .timeout = 2000,   /* 20s (10ms 单位) */
                };
                memcpy(cparams.bda, s_pending_dev.bd_addr, 6);
                esp_ble_gap_update_conn_params(&cparams);
                ESP_LOGI(TAG, "已请求 HID 连接参数: 监督超时=20s");
                s_nav_arm_until = xTaskGetTickCount() * portTICK_PERIOD_MS + NAV_ARM_MS;
                strncpy(s_connected_dev_name, s_pending_dev.name, sizeof(s_connected_dev_name) - 1);
                s_connected_dev_name[sizeof(s_connected_dev_name) - 1] = '\0';
                bt_unlock();
                if (s_hid_dev_count == 1) {
                    bool in_history = false;
                    for (int hi = 0; hi < s_history_count; hi++) {
                        if (memcmp(s_history[hi].bd_addr, s_pending_dev.bd_addr, 6) == 0) {
                            in_history = true;
                            break;
                        }
                    }
                    s_last_connect_was_new = !in_history;
                    bt_manager_save_paired(&s_pending_dev);
                    bt_manager_add_to_history(&s_pending_dev);
                    if (s_connect_cb) s_connect_cb(true);
                }
            }
            break;
        }
        case ESP_HIDH_INPUT_EVENT: {
            if (data->input.length == 0) break;
            /* === V1.0.40: K 模式(键盘鼠标) HID 解码 — 基于实测 ===
             * ESP32 esp_hidh 的 data 不含 report_id (rid 在 data->input.report_id 字段)
             * Q36 K模式 gamepad HID (report_id=4, len=10) 字段位置:
             *   d[0..3] = 4 摇杆轴 (0x80=中点)
             *   d[4] = hat switch: 0x00=上, 0x02=右, 0x04=下, 0x06=左, 0xFF=居中
             *   d[5] = 主按钮: bit0=A, bit1=B, bit3=X, bit4=Y, bit6=L1, bit7=R1
             *   d[6] = 副按钮: bit0=L2, bit1=R2, bit2=Select, bit3=Start */
            if (data->input.report_id != 4) break;   /* 只听 gamepad HID, 忽略 keyboard HID */
            const uint8_t *d = data->input.data;
            uint8_t len = data->input.length;
            if (len < 7) break;

            uint8_t old_hat = s_hat;
            uint16_t old_btn = s_btn;
            /* V1.0.40: hat 从 d[4] 读取 (ESP32 data 无 rid, macOS 的 d[5] 对应 ESP32 d[4]) */
            uint8_t raw_hat = d[4];
            switch (raw_hat) {
                case 0x00: s_hat = 0; break;  /* 上 N */
                case 0x02: s_hat = 2; break;  /* 右 E */
                case 0x04: s_hat = 4; break;  /* 下 S */
                case 0x06: s_hat = 6; break;  /* 左 W */
                default:   s_hat = 8; break;  /* 0xFF 或其它 = 居中 */
            }
            /* V1.0.40: 按钮从 d[5]+d[6] 重新映射到标准 10 位 s_btn
             * s_btn: bit0=A bit1=B bit2=X bit3=Y bit4=L1 bit5=R1 bit6=L2 bit7=R2
             *        bit8=SELECT bit9=START */
            s_btn = 0;
            if (d[5] & 0x01) s_btn |= 0x01;  /* A */
            if (d[5] & 0x02) s_btn |= 0x02;  /* B */
            if (d[5] & 0x08) s_btn |= 0x04;  /* X */
            if (d[5] & 0x10) s_btn |= 0x08;  /* Y */
            if (d[5] & 0x40) s_btn |= 0x10;  /* L1 */
            if (d[5] & 0x80) s_btn |= 0x20;  /* R1 */
            if (d[6] & 0x01) s_btn |= 0x40;  /* L2 */
            if (d[6] & 0x02) s_btn |= 0x80;  /* R2 */
            if (d[6] & 0x04) s_btn |= 0x100; /* Select */
            if (d[6] & 0x08) s_btn |= 0x200; /* Start */

            /* V1.0.40: 调试日志 — 打印完整 HID 报告 */
            if (s_hat != old_hat || s_btn != old_btn) {
                ESP_LOGI(TAG, "HID raw: len=%d rid=%d d[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X | hat_raw=0x%02X→s_hat=%d btn=0x%02X",
                         len, data->input.report_id,
                         d[0],d[1],d[2],d[3],d[4],d[5],d[6],(len>7?d[7]:0),
                         raw_hat, s_hat, s_btn);
            }

            /* 边沿检测 → 累加到 s_pending_press (待 poll_new_press 消费) */
            if (s_hat != old_hat) {
                for (int dir = 0; dir < 4; dir++) {
                    if (hat_is(dir)) s_pending_press |= (1u << dir);
                }
            }
            uint16_t new_btn = s_btn & ~old_btn;
            if (new_btn) s_pending_press |= ((uint16_t)new_btn) << 4;
            break;
        }
        case ESP_HIDH_CLOSE_EVENT: {
            ESP_LOGI(TAG, "HID 设备已关闭, reason=%d", (int)data->close.reason);
            s_last_disconnect_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bt_lock();
            int dev_idx = find_hid_dev_index(data->close.dev);
            if (dev_idx >= 0) {
                s_hid_devs[dev_idx] = NULL;
                s_hid_devs_bda_set[dev_idx] = false;
                memset(s_hid_devs_bda[dev_idx], 0, 6);
                if (s_hid_dev_count > 0) s_hid_dev_count--;
            }
            s_connected = (s_hid_dev_count > 0);
            if (!s_connected) {
                s_connected_dev_name[0] = '\0';
                s_hat = 8;
                s_btn = 0;
                s_pending_press = 0;
            }
            bt_unlock();
            if (data->close.dev) esp_hidh_dev_free(data->close.dev);
            if (!s_connected && !s_has_pending_dev && s_connect_cb) s_connect_cb(false);
            break;
        }
        default: break;
    }
}

/* ========================================================================
 *  按键映射查询与编辑
 * ======================================================================== */
bool bt_manager_is_key_pressed(func_t f) {
    if (f < 0 || f >= FUNC_MAX) return false;
    phys_t p = s_map[f];
    if (p < 4) return hat_is(p);              /* 摇杆 4 向 */
    return (s_btn >> (p - 4)) & 1;            /* 8 按钮位 */
}

void bt_manager_poll_new_press_reset(void) {
    s_pending_press = 0;
    s_consumed_hat = s_hat;
    s_consumed_btn = s_btn;
    /* V1.0.41: 不重新扫描当前按住的键. 否则摇杆方向键按住不放时,
     * reset 会把同一个键反复加入 pending, 导致 8 个功能瞬间全映射到同一键.
     * 现在只清空 pending, 等待 on_hid_input 的真正边沿 (释放后再按下). */
}

bool bt_manager_poll_new_press(func_t f) {
    if (f < 0 || f >= FUNC_MAX) return false;
    if (s_pending_press == 0) return false;
    for (int p = 0; p < PHYS_MAX; p++) {
        if (s_pending_press & (1u << p)) {
            s_pending_press &= ~(1u << p);
            s_map[f] = (phys_t)p;
            return true;
        }
    }
    return false;
}

void bt_manager_set_key_map(func_t f, phys_t p) {
    if (f < 0 || f >= FUNC_MAX || p < 0 || p >= PHYS_MAX) return;
    s_map[f] = p;
}

phys_t bt_manager_get_key_map(func_t f) {
    if (f < 0 || f >= FUNC_MAX) return P_HAT_UP;
    return s_map[f];
}

void bt_manager_reset_key_map(void) {
    memcpy(s_map, DEFAULT_MAP, sizeof(s_map));
    s_pending_press = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_MAP);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "按键映射已恢复默认");
}

void bt_manager_save_key_map(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_MAP, s_map, sizeof(s_map));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "按键映射已保存到 NVS");
}

static void bt_manager_load_key_map(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        memcpy(s_map, DEFAULT_MAP, sizeof(s_map));
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_MAP, NULL, &len) == ESP_OK && len == sizeof(s_map)) {
        nvs_get_blob(h, NVS_KEY_MAP, s_map, &len);
        ESP_LOGI(TAG, "从 NVS 加载按键映射 (%d 字节)", (int)len);
    } else {
        memcpy(s_map, DEFAULT_MAP, sizeof(s_map));
        ESP_LOGI(TAG, "NVS 无按键映射, 使用默认值");
    }
    nvs_close(h);
}

/* ========================================================================
 *  补充按键映射 (4 字节: PHYS_MAX=未映射)
 * ======================================================================== */
static void bt_manager_load_sup_map(void) {
    for (int i = 0; i < SUP_MAX; i++) s_sup_map[i] = PHYS_MAX;  /* 默认全部未映射 */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_SUP, NULL, &len) == ESP_OK && len == sizeof(s_sup_map)) {
        nvs_get_blob(h, NVS_KEY_SUP, s_sup_map, &len);
        ESP_LOGI(TAG, "从 NVS 加载补充按键映射 (%d 字节)", (int)len);
    } else {
        ESP_LOGI(TAG, "NVS 无补充按键映射, 使用默认(全部未映射)");
    }
    nvs_close(h);
}

const char *bt_manager_sup_func_name(int idx) {
    static const char *names[SUP_MAX] = {
        "\xe5\x8a\x9f\xe8\x83\xbd\x31(F)",    /* 功能1(F) */
        "\xe5\x8a\x9f\xe8\x83\xbd\x32(G)",    /* 功能2(G) */
        "\xe5\x8a\x9f\xe8\x83\xbd\x33(Shift)",/* 功能3(Shift) */
        "\xe5\x8a\x9f\xe8\x83\xbd\x34(\xe7\xa9\xba\xe6\xa0\xbc)", /* 功能4(空格) */
    };
    if (idx < 0 || idx >= SUP_MAX) return "?";
    return names[idx];
}

/* 消费一个待消费的物理边沿, 返回 phys (不写映射, 供 UI 校验"已占用") */
bool bt_manager_poll_sup_capture(phys_t *out) {
    if (s_pending_press == 0) return false;
    for (int p = 0; p < PHYS_MAX; p++) {
        if (s_pending_press & (1u << p)) {
            s_pending_press &= ~(1u << p);
            if (out) *out = (phys_t)p;
            return true;
        }
    }
    return false;
}

/* 直接查询某物理按键当前是否按下 (用于补充键投递, 不经过 8 键映射) */
bool bt_manager_is_phys_pressed(phys_t p) {
    if (p < 0 || p >= PHYS_MAX) return false;
    if (p < 4) return hat_is(p);              /* 摇杆 4 向 */
    return (s_btn >> (p - 4)) & 1;            /* 8 按钮位 */
}

/* 第 idx 个补充键当前是否按下 (未映射则返回 false) */
bool bt_manager_is_sup_pressed(int idx) {
    if (idx < 0 || idx >= SUP_MAX) return false;
    phys_t p = s_sup_map[idx];
    if (p < 0 || p >= PHYS_MAX) return false; /* 未映射 */
    return bt_manager_is_phys_pressed(p);
}

void bt_manager_set_sup_map(int idx, phys_t p) {
    if (idx < 0 || idx >= SUP_MAX || p < 0 || p >= PHYS_MAX) return;
    s_sup_map[idx] = p;
}

phys_t bt_manager_get_sup_map(int idx) {
    if (idx < 0 || idx >= SUP_MAX) return PHYS_MAX;
    return s_sup_map[idx];
}

void bt_manager_save_sup_map(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SUP, s_sup_map, sizeof(s_sup_map));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "补充按键映射已保存到 NVS");
}

void bt_manager_reset_sup_map(void) {
    for (int i = 0; i < SUP_MAX; i++) s_sup_map[i] = PHYS_MAX;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SUP);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "补充按键映射已清除");
}

/* 物理键 p 是否已被占用 (8 键核心映射 或 其它补充键) */
bool bt_manager_sup_phys_used(phys_t p, int cur_idx) {
    if (p < 0 || p >= PHYS_MAX) return true;
    for (int f = 0; f < FUNC_MAX; f++)
        if (s_map[f] == p) return true;
    for (int j = 0; j < SUP_MAX; j++)
        if (j != cur_idx && s_sup_map[j] == p) return true;
    return false;
}

/* ========================================================================
 *  GB 辅助按键映射 (2 个功能: SELECT/START → 12 个物理输入)
 *  - 独立存储 s_gb_map[2], 持久化到 NVS "gbmap".
 *  - 只在 GB 游戏二级菜单及游戏中生效 (input.c 的 GB joypad 投递).
 *  - 捕获流程与补充按键一致 (消费 s_pending_press 边沿).
 * ======================================================================== */
static void bt_manager_load_gb_map(void) {
    for (int i = 0; i < GB_MAP_MAX; i++) s_gb_map[i] = PHYS_MAX;  /* 默认全部未映射 */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_GB, NULL, &len) == ESP_OK && len == sizeof(s_gb_map)) {
        nvs_get_blob(h, NVS_KEY_GB, s_gb_map, &len);
        ESP_LOGI(TAG, "从 NVS 加载 GB 辅助按键映射 (%d 字节)", (int)len);
    } else {
        ESP_LOGI(TAG, "NVS 无 GB 辅助按键映射, 使用默认(全部未映射)");
    }
    nvs_close(h);
}

const char *bt_manager_gb_func_name(int idx) {
    static const char *names[GB_MAP_MAX] = {
        "SELECT",   /* SELECT 键 */
        "START",    /* START 键 */
    };
    if (idx < 0 || idx >= GB_MAP_MAX) return "?";
    return names[idx];
}

/* 消费一个待消费的物理边沿, 返回 phys (不写映射, 供 UI 校验"已占用") */
bool bt_manager_poll_gb_capture(phys_t *out) {
    if (s_pending_press == 0) return false;
    for (int p = 0; p < PHYS_MAX; p++) {
        if (s_pending_press & (1u << p)) {
            s_pending_press &= ~(1u << p);
            if (out) *out = (phys_t)p;
            return true;
        }
    }
    return false;
}

/* 第 idx 个 GB 辅助键当前是否按下 (未映射则返回 false) */
bool bt_manager_is_gb_pressed(int idx) {
    if (idx < 0 || idx >= GB_MAP_MAX) return false;
    phys_t p = s_gb_map[idx];
    if (p < 0 || p >= PHYS_MAX) return false; /* 未映射 */
    return bt_manager_is_phys_pressed(p);
}

void bt_manager_set_gb_map(int idx, phys_t p) {
    if (idx < 0 || idx >= GB_MAP_MAX || p < 0 || p >= PHYS_MAX) return;
    s_gb_map[idx] = p;
}

phys_t bt_manager_get_gb_map(int idx) {
    if (idx < 0 || idx >= GB_MAP_MAX) return PHYS_MAX;
    return s_gb_map[idx];
}

void bt_manager_save_gb_map(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_GB, s_gb_map, sizeof(s_gb_map));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "GB 辅助按键映射已保存到 NVS");
}

void bt_manager_reset_gb_map(void) {
    for (int i = 0; i < GB_MAP_MAX; i++) s_gb_map[i] = PHYS_MAX;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_GB);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "GB 辅助按键映射已清除");
}

/* 物理键 p 是否已被占用 (8 键核心映射 或 其它 GB 辅助键) */
bool bt_manager_gb_phys_used(phys_t p, int cur_idx) {
    if (p < 0 || p >= PHYS_MAX) return true;
    for (int f = 0; f < FUNC_MAX; f++)
        if (s_map[f] == p) return true;
    for (int j = 0; j < GB_MAP_MAX; j++)
        if (j != cur_idx && s_gb_map[j] == p) return true;
    return false;
}

/* SELECT 或 START 是否已有任一映射 */
bool bt_manager_gb_map_set(void) {
    for (int i = 0; i < GB_MAP_MAX; i++)
        if (s_gb_map[i] >= 0 && s_gb_map[i] < PHYS_MAX) return true;
    return false;
}

/* ========================================================================
 *  蓝牙初始化/扫描/连接
 * ======================================================================== */
void bt_manager_init(void) {
    s_init_status = "初始化控制器...";
    s_initializing = true;
    bt_manager_load_paired();
    bt_manager_load_history_from_nvs();
    bt_manager_load_key_map();        /* V1.0.38: 开机即加载, 始终有默认 */
    bt_manager_load_sup_map();        /* 补充按键映射: 开机即加载, 默认全部未映射 */
    bt_manager_load_gb_map();         /* GB 辅助按键映射: 开机即加载, 默认全部未映射 */
    s_nav_enabled = true;             /* V1.0.38: 有默认映射, 开机即可用手柄导航 */

    if (!s_bt_mutex) s_bt_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "[1/6] 释放经典蓝牙内存 (ESP32-S3 不支持 BR/EDR)...");
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) ESP_LOGW(TAG, "释放经典蓝牙内存失败: %s", esp_err_to_name(ret));

    ESP_LOGI(TAG, "[2/6] 初始化 BT 控制器 (BLE Only)...");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器初始化失败: %s", esp_err_to_name(ret));
        s_init_status = "控制器初始化失败";
        goto done;
    }
    s_ctrl_init = true;

    ESP_LOGI(TAG, "[3/6] 启用 BT 控制器 (BLE Only)...");
    s_init_status = "启用控制器...";
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器启用失败: %s", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        s_ctrl_init = false;
        s_init_status = "控制器启用失败";
        goto done;
    }

    ESP_LOGI(TAG, "[4/6] 初始化 Bluedroid...");
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙协议栈初始化失败: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_ctrl_init = false;
        s_init_status = "协议栈初始化失败";
        goto done;
    }

    ESP_LOGI(TAG, "[5/6] 启用 Bluedroid...");
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙协议栈启用失败: %s", esp_err_to_name(ret));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_ctrl_init = false;
        s_init_status = "协议栈启用失败";
        goto done;
    }

    ESP_LOGI(TAG, "[6/6] 注册 GAP 回调...");
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret != ESP_OK) ESP_LOGW(TAG, "GAP 回调注册失败: %s", esp_err_to_name(ret));

    /* V1.0.45 重要: 不再主动调用 esp_ble_gap_set_security_param 改变默认值!
     * 之前把 SM_AUTHEN_REQ 设成 ESP_LE_AUTH_BOND 导致协议栈主动发起 SMP Bond,
     * 破坏了原本"1~2 秒连上"的无 SMP 的默认 HID 流程 → cheap 手柄 bond 失败断开.
     * 现在保持协议栈默认 SMP 参数不变. GAP 回调里保留了 SMP 事件应答代码是被动的:
     * 只有对端真的发起了 SMP 请求才会触发, 否则完全不影响 HID 连接. */

    s_init_status = "设置扫描参数...";
    static esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x0080,
        .scan_window = 0x0040,
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
    };
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) s_init_status = "扫描参数设置失败";

    ESP_LOGI(TAG, "注册 GATTC 回调 (HID Host)...");
    ret = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTC 回调注册失败: %s", esp_err_to_name(ret));
        s_init_status = "GATTC 回调注册失败";
        goto done;
    }

    ESP_LOGI(TAG, "初始化 BLE HID Host...");
    static esp_hidh_config_t hidh_config = {
        .callback = hidh_event_handler,
        .event_stack_size = 6144,  /* V1.0.40: 4096→6144, 防首次连接 HID descriptor 解析栈溢出 */
        .callback_arg = NULL,
    };
    ret = esp_hidh_init(&hidh_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID Host 初始化失败: %s", esp_err_to_name(ret));
        s_init_status = "HID Host 初始化失败";
        goto done;
    }
    ESP_LOGI(TAG, "BLE HID Host 初始化成功 (V1.0.38 按键映射)");
    s_hidh_ready = true;

    /* V1.0.46: 启动 3 秒自动重连后台任务 (菜单/游戏内都生效)
     * 栈必须够大: 连接调用链 (esp_hidh_dev_open 等) 深, 512 words 会溢出重启 */
    static StaticTask_t s_autoreconnect_tcb;
    /* V1.0.53: 自动重连任务栈放 PSRAM, 释放 16KB 内部 RAM.
     * 该任务仅在运行期执行 (非 phy 校准期), PSRAM 栈安全. */
    EXT_RAM_BSS_ATTR static StackType_t s_autoreconnect_stack[4096];
    static bool s_autoreconnect_started = false;
    if (!s_autoreconnect_started) {
        xTaskCreateStatic(bt_autoreconnect_task, "bt_autoreconn", 4096,
                          NULL, 1, s_autoreconnect_stack, &s_autoreconnect_tcb);
        s_autoreconnect_started = true;
    }

    if (s_has_paired) bt_manager_start_auto_connect();
done:
    s_initializing = false;
    vTaskDelete(NULL);
}

const char *bt_manager_get_status(void) { return s_init_status; }
bool bt_manager_is_stack_ready(void) { return s_scan_params_ready || s_hidh_ready; }

void bt_manager_start_scan(bt_scan_callback_t callback) {
    s_scan_count = 0;
    s_scanning = true;
    s_scan_callback = callback;
    s_device_found_cb = NULL;
    if (s_scan_params_ready) esp_ble_gap_start_scanning(0);
    else s_pending_start_scan = true;
}

void bt_manager_start_scan_continuous(bt_device_found_cb_t callback) {
    if (!s_scan_params_ready && !s_hidh_ready) {
        if (callback) callback(s_scan_results, 0, false);
        return;
    }
    s_scan_count = 0;
    s_scanning = true;
    s_scan_callback = NULL;
    s_device_found_cb = callback;
    esp_ble_gap_start_scanning(0);
}

bool bt_manager_is_scanning(void) { return s_scanning; }
void bt_manager_stop_scan(void) {
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_scanning();
    }
}
bool bt_manager_is_connected(void) { return s_connected; }
int bt_manager_get_scan_results(bt_device_t **results) { *results = s_scan_results; return s_scan_count; }
const char *bt_manager_get_device_name(const bt_device_t *dev) {
    return (dev && dev->has_name) ? dev->name : "未知设备";
}
const char *bt_manager_get_connected_device_name(void) { return s_connected_dev_name; }

/* 连接任务 */
#define CONNECT_MAX_ATTEMPTS   1
#define CONNECT_ATTEMPT_US     (5 * 1000 * 1000)
static void connect_task(void *arg) {
    ESP_LOGI(TAG, "连接任务启动: %s", bt_manager_get_device_name(&s_pending_dev));
    if (!s_hidh_ready) {
        ESP_LOGE(TAG, "HID Host 未就绪, 取消连接");
        snprintf(s_connect_error, sizeof(s_connect_error), "蓝牙未就绪");
        bt_lock();
        s_has_pending_dev = false;
        bt_unlock();
        if (s_connect_cb) s_connect_cb(false);
        vTaskDelete(NULL);
        return;
    }

    bool success = false;
    for (int attempt = 1; attempt <= CONNECT_MAX_ATTEMPTS && !success && !s_connect_cancel; attempt++) {
        if (s_scanning) bt_manager_stop_scan();
        vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? 200 : 800));
        if (s_connected) { success = true; break; }

        s_open_failed = false;
        if (attempt > 1) {
            ESP_LOGW(TAG, "自动重试连接 (第 %d/%d 次)", attempt, CONNECT_MAX_ATTEMPTS);
            if (s_connect_progress_cb) {
                char msg[32];
                snprintf(msg, sizeof(msg), "自动重试 (%d/%d)...", attempt, CONNECT_MAX_ATTEMPTS);
                s_connect_progress_cb(msg);
            }
        } else {
            if (s_connect_progress_cb) s_connect_progress_cb("打开设备...");
        }

        int64_t start_us = esp_timer_get_time();
        esp_hidh_dev_t *dev = esp_hidh_dev_open((uint8_t *)s_pending_dev.bd_addr,
                                                 ESP_HID_TRANSPORT_BLE,
                                                 s_pending_dev.addr_type);
        if (dev == NULL) {
            ESP_LOGE(TAG, "设备打开失败 (dev_open 返回 NULL), 尝试 %d", attempt);
            snprintf(s_connect_error, sizeof(s_connect_error), "连接失败\n请重试");
            continue;
        }
        if (s_connect_progress_cb) s_connect_progress_cb("等待连接...");

        while (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (s_connect_cancel) break;
            if (s_open_failed) break;
            int64_t elapsed_us = esp_timer_get_time() - start_us;
            if (elapsed_us > CONNECT_ATTEMPT_US) {
                ESP_LOGE(TAG, "本次尝试超时");
                snprintf(s_connect_error, sizeof(s_connect_error), "连接超时");
                esp_hidh_dev_close(dev);
                vTaskDelay(pdMS_TO_TICKS(300));
                break;
            }
        }
        if (s_connected) success = true;
    }

    bool cancelled = false;
    bt_lock();
    s_has_pending_dev = false;
    cancelled = s_connect_cancel;
    s_connect_cancel = false;
    bt_unlock();
    if (success) {
        ESP_LOGI(TAG, "连接成功");
    } else if (cancelled) {
        ESP_LOGI(TAG, "连接已取消");
    } else {
        ESP_LOGE(TAG, "连接失败");
        if (s_connect_error[0] == '\0') snprintf(s_connect_error, sizeof(s_connect_error), "连接失败");
        if (s_connect_cb) s_connect_cb(false);
    }
    vTaskDelete(NULL);
}

bool bt_manager_connect_device(const bt_device_t *dev) {
    bt_lock();
    /* V1.0.60: 断开后 10 秒冷却. 手柄休眠/掉线后立刻重连会在 Bluedroid/HID
     * 协议栈清理未完成时 esp_hidh_dev_open -> LoadProhibited 崩溃 (开机约 5 分钟
     * 自动重启循环的根因). 冷却期后协议栈状态干净, 重连才安全. */
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_last_disconnect_ms &&
        (uint32_t)(now_ms - s_last_disconnect_ms) < 10000) {
        snprintf(s_connect_error, sizeof(s_connect_error), "断开冷却中, 稍后重连");
        bt_unlock();
        return false;
    }
    if (s_connected) {
        snprintf(s_connect_error, sizeof(s_connect_error), "已有设备连接");
        bt_unlock();
        return false;
    }
    if (!s_hidh_ready) {
        snprintf(s_connect_error, sizeof(s_connect_error), "蓝牙未就绪");
        bt_unlock();
        return false;
    }
    if (s_has_pending_dev) {
        if (memcmp(s_pending_dev.bd_addr, dev->bd_addr, 6) == 0) {
            bt_unlock();
            return true;
        }
        snprintf(s_connect_error, sizeof(s_connect_error), "正在连接其他设备");
        bt_unlock();
        return false;
    }
    memcpy(&s_pending_dev, dev, sizeof(bt_device_t));
    s_has_pending_dev = true;
    s_connect_cancel = false;
    s_connect_error[0] = '\0';
    bt_unlock();

    TaskHandle_t conn_task = xTaskCreateStatic(connect_task, "bt_conn",
                                               CONNECT_TASK_STACK_WORDS, NULL, 5,
                                               s_connect_task_stack, &s_connect_task_buf);
    if (conn_task == NULL) {
        snprintf(s_connect_error, sizeof(s_connect_error), "连接任务异常");
        bt_lock();
        s_has_pending_dev = false;
        bt_unlock();
        return false;
    }
    return true;
}

const char *bt_manager_get_connect_error(void) {
    return s_connect_error[0] ? s_connect_error : "连接失败";
}
bool bt_manager_last_connect_was_new(void) { return s_last_connect_was_new; }
bool bt_manager_is_connecting(void) { return s_has_pending_dev; }
void bt_manager_cancel_connect(void) {
    bt_lock();
    if (s_has_pending_dev) s_connect_cancel = true;
    bt_unlock();
}
void bt_manager_set_connect_callback(bt_connect_cb_t cb) { s_connect_cb = cb; }
void bt_manager_set_connect_progress_cb(bt_connect_progress_cb_t cb) { s_connect_progress_cb = cb; }

void bt_manager_disconnect(void) {
    bt_lock();
    for (int i = 0; i < BT_HID_DEV_MAX; i++) {
        if (s_hid_devs[i]) {
            esp_hidh_dev_close(s_hid_devs[i]);
            s_hid_devs[i] = NULL;
        }
    }
    s_hid_dev_count = 0;
    s_connected = false;
    s_has_pending_dev = false;
    s_connected_dev_name[0] = '\0';
    s_hat = 8;
    s_btn = 0;
    s_pending_press = 0;
    bt_unlock();
}

bool bt_manager_get_connected_addr(uint8_t *out_addr6) {
    if (!out_addr6) return false;
    for (int i = 0; i < BT_HID_DEV_MAX; i++) {
        if (s_hid_devs_bda_set[i] && s_hid_devs[i]) {
            memcpy(out_addr6, s_hid_devs_bda[i], 6);
            return true;
        }
    }
    return false;
}

/* 配对/历史 */
static void bt_manager_save_paired(const bt_device_t *dev) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_ADDR, dev->bd_addr, 6);
    nvs_set_u8(h, NVS_KEY_TYPE, dev->addr_type);
    nvs_set_str(h, NVS_KEY_NAME, dev->name);
    nvs_commit(h);
    nvs_close(h);
    memcpy(s_paired_addr, dev->bd_addr, 6);
    s_paired_addr_type = dev->addr_type;
    strncpy(s_paired_name, dev->name, sizeof(s_paired_name) - 1);
    s_paired_name[sizeof(s_paired_name) - 1] = '\0';
    s_has_paired = true;
}

static void bt_manager_load_paired(void) {
    nvs_handle_t h;
    s_has_paired = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_ADDR, NULL, &len) == ESP_OK && len == 6) {
        nvs_get_blob(h, NVS_KEY_ADDR, s_paired_addr, &len);
        nvs_get_u8(h, NVS_KEY_TYPE, &s_paired_addr_type);
        size_t nlen = sizeof(s_paired_name);
        if (nvs_get_str(h, NVS_KEY_NAME, s_paired_name, &nlen) != ESP_OK) s_paired_name[0] = '\0';
        s_has_paired = true;
    }
    nvs_close(h);
}

void bt_manager_clear_paired(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ADDR);
        nvs_erase_key(h, NVS_KEY_TYPE);
        nvs_erase_key(h, NVS_KEY_NAME);
        nvs_commit(h);
        nvs_close(h);
    }
    s_has_paired = false;
    memset(s_paired_addr, 0, 6);
    s_paired_name[0] = '\0';
}

bool bt_manager_is_paired_device(const uint8_t *bd_addr) {
    if (!s_has_paired || !bd_addr) return false;
    return memcmp(s_paired_addr, bd_addr, 6) == 0;
}

static void bt_manager_save_history_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "hist_n", (uint8_t)s_history_count);
    for (int i = 0; i < s_history_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist%d", i);
        nvs_set_blob(h, key, &s_history[i], sizeof(bt_device_t));
    }
    for (int i = s_history_count; i < BT_HISTORY_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist%d", i);
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void bt_manager_load_history_from_nvs(void) {
    s_history_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    if (nvs_get_u8(h, "hist_n", &n) != ESP_OK) { nvs_close(h); return; }
    if (n > BT_HISTORY_MAX) n = BT_HISTORY_MAX;
    for (int i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist%d", i);
        size_t len = sizeof(bt_device_t);
        if (nvs_get_blob(h, key, &s_history[s_history_count], &len) == ESP_OK
            && len == sizeof(bt_device_t)) s_history_count++;
    }
    nvs_close(h);
}

int bt_manager_get_history_count(void) { return s_history_count; }
const bt_device_t *bt_manager_get_history_at(int index) {
    if (index < 0 || index >= s_history_count) return NULL;
    return &s_history[index];
}
bool bt_manager_remove_history_at(int index) {
    if (index < 0 || index >= s_history_count) return false;
    for (int i = index; i < s_history_count - 1; i++) s_history[i] = s_history[i + 1];
    memset(&s_history[s_history_count - 1], 0, sizeof(bt_device_t));
    s_history_count--;
    bt_manager_save_history_to_nvs();
    return true;
}

void bt_manager_add_to_history(const bt_device_t *dev) {
    if (!dev || dev->name[0] == '\0') return;
    int found = -1;
    for (int i = 0; i < s_history_count; i++) {
        if (memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0) { found = i; break; }
    }
    bt_device_t entry = *dev;
    if (found >= 0) {
        for (int i = found; i > 0; i--) s_history[i] = s_history[i - 1];
        s_history[0] = entry;
    } else {
        if (s_history_count >= BT_HISTORY_MAX) {
            for (int i = BT_HISTORY_MAX - 1; i > 0; i--) s_history[i] = s_history[i - 1];
            s_history[0] = entry;
        } else {
            for (int i = s_history_count; i > 0; i--) s_history[i] = s_history[i - 1];
            s_history[0] = entry;
            s_history_count++;
        }
    }
    bt_manager_save_history_to_nvs();
}

static void bt_manager_start_auto_connect(void) {
    if (!s_has_paired || s_connected || s_has_pending_dev) return;
    if (!s_hidh_ready) { s_auto_connect_pending = true; return; }
    bt_device_t dev;
    memset(&dev, 0, sizeof(dev));
    memcpy(dev.bd_addr, s_paired_addr, 6);
    dev.addr_type = s_paired_addr_type;
    strncpy(dev.name, s_paired_name, sizeof(dev.name) - 1);
    dev.has_name = true;
    bt_manager_connect_device(&dev);
}

/* V1.0.46: 每 3 秒自动重连已配对设备 (无论处于菜单还是游戏内)
 * 已连接成功则跳过; 连接中 (s_has_pending_dev) 也跳过, 避免重复发起. */
static uint32_t s_last_reconnect_ms = 0;

void bt_manager_poll_auto_reconnect(void) {
    if (s_suspended) return;                 /* 屏保挂起期间不自动重连 */
    if (!s_hidh_ready || s_initializing) return;
    if (!s_has_paired || s_connected || s_has_pending_dev) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_last_reconnect_ms < 3000) return;
    s_last_reconnect_ms = now;
    bt_manager_start_auto_connect();
}

static void bt_autoreconnect_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        bt_manager_poll_auto_reconnect();
    }
}

void bt_manager_disable(void) {
    int wait_count = 0;
    while (s_initializing && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_count++;
    }
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_scanning();
    }
    wait_count = 0;
    while (s_has_pending_dev && wait_count < 20) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_count++;
    }
    bt_manager_disconnect();
    if (s_hidh_ready) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        s_hidh_ready = false;
    }
    if (s_ctrl_init) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_ctrl_init = false;
    }
    s_scan_params_ready = false;
    s_init_status = "已关闭";
}

void bt_manager_enable(void) {
    if (s_hidh_ready) return;
    if (s_initializing) return;
    TaskHandle_t init_task = xTaskCreateStatic((TaskFunction_t)bt_manager_init, "bt_init",
                                               INIT_TASK_STACK_WORDS, NULL, 5,
                                               s_init_task_stack, &s_init_task_buf);
    if (init_task == NULL) s_init_status = "初始化失败(内存不足)";
}
bool bt_manager_is_ready(void) { return s_hidh_ready; }

/* 屏保软挂起: 断开全部设备 + 停止扫描, 协议栈保持存活 (不 deinit).
 * 相比完整 disable/enable, 避免 BBK 引擎占用内部内存时蓝牙重新初始化
 * 内存不足导致崩溃重启. */
void bt_manager_suspend(void) {
    s_suspended = true;
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_scanning();
    }
    bt_manager_disconnect();
    ESP_LOGI(TAG, "蓝牙已挂起 (屏保): 断开连接, 停止扫描");
}

void bt_manager_resume(void) {
    if (!s_suspended) return;
    s_suspended = false;
    ESP_LOGI(TAG, "蓝牙已恢复 (退出屏保): 自动重连");
    bt_manager_start_auto_connect();
}

bool bt_manager_is_suspended(void) { return s_suspended; }
