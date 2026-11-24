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
#include "esp_attr.h"      /* EXT_RAM_BSS_ATTR: 扫描缓存挪到 PSRAM */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "svc_nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* V1.5.x: enable 同步等待初始化任务完成 */
#include "freertos/timers.h"   /* V1.5.x: 开机自动回连窗口软件定时器 */
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps: 按 caps 建任务(内部RAM动态栈) */
#include "esp_heap_caps.h"           /* MALLOC_CAP_INTERNAL */

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
/* 蓝牙键盘输入 sink (由 input 层注册, 见 bt_manager_set_kbd_sink) */
static bt_manager_kbd_sink_t s_kbd_sink = NULL;
static bool s_scanning = false;
static bool s_suspended = false;
static bool s_connecting = false;
static int64_t s_scan_start_ms = 0;   /* ★ 2026-10-18: 本轮扫描起点 (看门狗用, 0=未在扫) */
/* 蓝牙"电源开关": disable() 置真(彻底关栈), enable() 清假.
 * 用于锁屏/屏保省电 —— 关电后自动回连任务必须停手, 不能偷偷把栈再拉起来. */
static bool s_power_off = false;
static void bt_reconnect_task(void *arg);   /* 前向声明: init 里启动, 定义在自动回连小节 */
static bool s_last_connect_was_new = false;
static bool s_pending_new = false;   /* 发起连接时判定"是否新设备", 连接成功后在 on_connected 消费 */

EXT_RAM_BSS_ATTR static bt_device_t s_history[BT_HISTORY_MAX];   /* 只在扫描/回连时用 */
static int s_history_count = 0;
/* ★ 2026-11-24: 「开机自动回连」开关的持久值 (0=开, 1=关; NVS 键沿用 uoff)。
 *   定义前移是因为开机预检 (main.c) 与手柄页菜单都要在读之前就存在。
 *   ⚠ 唯一写入者: bt_manager_set_boot_auto_reconnect() (手柄页那一项)。 */
static uint8_t s_user_off = 0;

/* ★ 2026-11-23 linit B 档: 回连窗口状态.
 * 定义在这里(文件前部)是因为三处都要用, 而且顺序跨越大半个文件:
 *   · on_connected_cb (≈930)  真连上 → 结束窗口
 *   · bt_shutdown     (≈1700) 关蓝牙 → 清掉, 防"刚 enable 就判超时再拆栈"
 *   · bt_reconnect_task(≈1850) 窗口超时/节奏判断
 * 语义: active=true 表示"正在窗口内积极回连", end_ms 是截止时刻(ms, 来自 esp_timer)。 */
static volatile bool s_boot_win_active;
static uint32_t s_boot_win_end_ms;

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

/* ============================================================================
 * 自动回连 v2 (2026-10-02, 参照 Bluepad32 策略重做)
 * ① 身份 = **名字**, 地址只是"快路径": 这手柄(Q36)经常变 MAC, 只按地址匹配必然失败。
 * ② **省电预算**: 一轮回连最多 RC_BUDGET_MS; 用尽就**关射频**(不再无限退避重试)。
 * ③ 扫描纪律: 只在未连接、且用户没在扫列表时才扫。
 * ==========================================================================*/
#define RC_BUDGET_MS   14000
/* ★ 省电: 非开机窗口(使用中意外断连)的一轮重试预算。
 *   原实现在窗口结束后按 60→120→240→300s **永远**重试 = "盲目一直连";
 *   现在只给一轮 RC_BUDGET_MS, 用尽就关射频等唤醒。 */
static int64_t s_rc_retry_until;
static char    s_rc_target_name[33];
static int64_t s_rc_budget_until;
static bool    s_map_pending;
static char    s_map_pending_name[33];
static char    s_pending_name[33];
/* ★ 2026-11-24: 本次连接事务是否由**用户主动发起** (在手柄页点选设备 → bt_manager_connect_device)。
 *   只有用户主动连接才允许挂"待做按键映射"待办 —— 后台自动回连(开机窗口/扫描窗等待/唤醒)
 *   连上后**不再**挂待办, 于是页面层不会被自动拉进手柄页、也不会自动弹映射向导
 *   (linit 要求: 已映射的保持当前页; 未映射的也只连上, 等用户自己进手柄页配置)。
 *   一旦连接事务收敛(成功/失败)就消费掉, 不让它泄漏到下一次后台回连。 */
static bool    s_conn_user_init;

static bool rc_budget_left(void) { return esp_timer_get_time() / 1000 < s_rc_budget_until; }
static void rc_budget_start(void)
{
    /* ★ 本轮预算仍在 → **不重置**。否则每次 poll/开扫描弹窗都续期一次,
     *   预算永远用不尽, "用尽就关射频"就形同虚设 (= linit 说的"盲目一直连")。 */
    if (rc_budget_left()) return;
    s_rc_budget_until = esp_timer_get_time() / 1000 + RC_BUDGET_MS;
    ESP_LOGI(TAG, "[EV] 回连预算开始: %d ms", RC_BUDGET_MS);
}
/* 回连目标匹配: **名字相同 或 地址相同** 都算命中 (名字优先) */
static bool rc_match(const uint8_t *addr, const char *name)
{
    if (addr && memcmp(addr, s_rc_target_addr, 6) == 0) return true;
    if (s_rc_target_name[0] && name && name[0] &&
        strncmp(name, s_rc_target_name, 32) == 0) return true;
    return false;
}
static bool s_rc_target_valid = false;

/* 自动回连退避参数 (定义在此供断连回调等前置函数使用):
 * 直连上次地址, 不空转扫描; 退避 60s→120s→240s→封顶300s. */
#define BT_RECONNECT_POLL_MS   60000
#define BT_RECONNECT_MAX_MS    60000    /* ★ 2026-10-17: 5min→1min, 回连等待感太明显(社区反馈) */
#define BT_BOOT_RECONNECT_POLL_MS 3000   /* V1.5.x: 开机回连窗口内直连间隔 (低占空比) */
#define BT_BOOT_RECONNECT_WINDOW_MS 30000 /* V1.5.x: 开机回连窗口默认时长 */
/* ★ 2026-11-23 linit: 手动打开蓝牙后的回连窗口时长.
 *   用户点"打开蓝牙"的意图基本就是"我要用手柄了", 所以也给 30s 主动抓上次手柄;
 *   抓不到 → 走同一条"超时真拆栈"路径, 不长期占内存。 */
#define BT_ENABLE_RECONNECT_WINDOW_MS 30000
/* 窗口末段阈值: 剩余时间少于它就把轮间空隙拉长为 2×POLL (见回连任务里的两段式节奏).
 * 取 15s 对两种窗口都合适: 60s 窗口 → 45s 快 + 15s 慢; 30s 窗口 → 15s + 15s。 */
#define BT_BOOT_FAST_PHASE_MS 15000
static uint32_t s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
static TaskHandle_t s_reconnect_task = NULL;   /* 断连/开机可即时唤醒重连任务 (退避睡眠中也能打断) */

/* 前置声明: 扫描回调(前文)与 connect_device(后文) 都要用; esp_hidh_dev_open 会阻塞, 必须异步调用.
 * 返回值: BT_CONN_STARTED / BT_CONN_BUSY / BT_CONN_ERR (见 bt_connect_async2 定义处)
 * ★ R2: from_scan=true 表示"这次连接由扫描自身命中触发"(边扫边连), 不再等扫描收尾;
 *   其余调用方必须先等扫描事务退出 (见 bt_connect_async2 里的闸门)。 */
static int bt_connect_async2(const esp_hid_scan_result_t *r, bool from_scan);
static int bt_connect_async(const esp_hid_scan_result_t *r) { return bt_connect_async2(r, false); }
static int bt_connect_from_scan(const esp_hid_scan_result_t *r) { return bt_connect_async2(r, true); }
/* ★ R3: 前置声明 —— 连接任务/扫描任务退出时若发现"回连任务没建成"要自己执行延迟释放 */
static void bt_release_flush(void);

/* ============================================================================
 * 连接收敛 (2026-10-18) —— 修"失败一次之后后面全失败"
 * ----------------------------------------------------------------------------
 * 现象 (linit): 手柄第二/第三次连接经常失败，且**失败一次后所有后续尝试都失败**。
 * 根因（读 IDF 源码确认，非猜测）：
 *   ① `esp_hidh_dev_open()` 内部的 `WAIT_CB()` 是 `xSemaphoreTake(sem, portMAX_DELAY)`
 *      —— **无限等** GATTC 事件。连接没成（GATT 0x85 / 设备不在）时它可能一直不返回
 *      ⇒ `s_connecting` 永远为 true ⇒ 回连任务与 UI 全被"正在连接"挡住
 *      ⇒ 之后每一次尝试都失败（看起来像"坏了"）。
 *   ② 失败后没人主动断开 ⇒ Bluedroid 侧还挂着 pending create-conn / 半开链路，
 *      下次 `esp_hidh_dev_open()` 直接撞 `Already Connected` / `INVALID_STATE`，
 *      直到重启才恢复。
 * 处理：每次尝试记下 bda + 截止时间；失败或超时统一走 bt_conn_failed()
 *   → 清"连接中" → 主动断掉那条链路 → 记连败次数 → 连败 N 次由回连任务重建蓝牙栈。
 * ==========================================================================*/
/* ★ 2026-10-27 linit 反馈"连接失败": 实测日志里直连一发出去就被 L2CAP 拒
 *   (cannot start new connection at conn st: 1 = 旧链路还占着), 但这里要**干等满
 *   12s 超时**才判定失败、才去按名字重连 ⇒ 用户看到的是"连接失败", 实际 20 秒后才连上。
 *   BLE 正常连接 1~2s, L2CAP 拒绝是瞬时的 —— 12s 纯属白等。
 *   压到 4s: 正常手柄够连上, 连不上就快速切"按名字重解析"那条路(见 bt_reconnect_task)。*/
/* ★★★ 2026-11-23 修正 (linit 报"连接提示失败, 重启却能连上") ★★★
 *   原来这里只有 4s 一个阈值, 超时就 bt_conn_failed() → bt_core_cancel_pending()
 *   → esp_ble_gap_disconnect() **主动取消**。但 esp_hidh_dev_open 要做的远不止"建链路":
 *   它还要做 ATT 服务发现 + HID 报告描述符枚举, 手柄冷启动时实测 3~8s 才收敛。
 *   于是"连接正在成功"被自己在第 4 秒掐断 —— 真实日志证据:
 *       W gattc_conn_cb ... rsn=0x100   (0x100 = ESP_GATT_CONN_CONN_CANCEL, **本地取消**)
 *       E BLE_HIDH: OPEN failed: 0x85
 *   手柄侧根本没拒绝(拒绝会是 0x13/0x3E/0x3B), 是主机先撤了。重启后第一次尝试时序更好,
 *   4s 内恰好连上 ⇒ 用户感受就是"点连接失败, 重启就成功"。
 *   现在分两级:
 *     · 15s "慢" —— 只打日志, 继续等 (给冷启动/服务发现留足时间);
 *     · 25s "硬超时" —— 才真取消 (仍能救回"open 永不返回"的死锁, 比原来的 30s 略早)。 */
#define BT_CONNECT_TIMEOUT_MS   15000   /* 慢阈值: 到此只提示, 不动连接 */
#define BT_CONNECT_HARD_MS      25000   /* 硬超时: **UI 放弃等待**, 不取消底层事务 */
#define BT_CONN_FAILS_TO_RESET  2       /* 连败几次就整栈重建 */
static int64_t  s_conn_deadline_ms = 0;      /* 0 = 无进行中的尝试 */
static uint8_t  s_conn_try_bda[6];
static bool     s_conn_try_valid = false;
static int      s_conn_fail_streak = 0;
static int      s_conn_task_alive = 0;       /* 还在 open 里阻塞的任务数 (>0 时不许拆栈) */
static volatile bool s_need_stack_reset = false;
static bool     s_conn_slow_logged = false;  /* "慢"提示只打一次, 防刷屏 */

/* ★★★ 2026-11-23 蓝牙稳定性治理 R1: 事务边界三件套 ★★★
 *   s_conn_gen       连接**事务代号**。每次发起 ++, 任务/失败回调用自己的代号回报;
 *                    代号对不上 = 上一轮事务的迟到事件 ⇒ 直接丢弃, 绝不能改新一轮的状态。
 *                    (代号只是"识别迟到者", 真实生命周期仍由 s_conn_task_alive 计数同步,
 *                     两者互补, 不用代号替代任务计数。)
 *   s_conn_giveup    UI 已放弃等待(超时/用户返回)。**只影响提示**, 不影响底层事务:
 *                    事务继续跑, 成功就照常连上, 失败由 open 返回值收敛。
 *   s_release_pending 关蓝牙/拆栈被推迟 (0=无, 1=只关射频, 2=深拆栈)。
 *                    有连接任务卡在 open 里时不能拆栈(会 UAF), 只能挂起,
 *                    等任务真正退出后由回连任务执行。 */
static uint32_t s_conn_gen = 0;
static volatile bool s_conn_giveup = false;
static volatile int  s_release_pending = 0;
/* ★★ 2026-11-23 R2: 用户/系统**主动断开**的意图标记。
 *   原来 bt_manager_disconnect() 只是"现在关一下链路", 而 CLOSE 事件进来后
 *   on_disconnected_cb 看到 s_power_off==false 就立刻 bt_manager_wake_reconnect() ——
 *   用户点"断开"后 1~3 秒又自己连上, 表现为"断不掉"。
 *   现在: 主动断开先立意图, 断连回调据此**不唤醒回连**;
 *   任何一次真正的新连接意图 (bt_connect_async / enable / resume) 都把它清掉。
 *   同一个标记还负责"连接事务在飞时用户点了断开": 事务照旧跑完 (取消无效, 见 R1),
 *   但 OPEN 成功那一刻直接关掉, 不让它偷偷连上。 */
static volatile bool s_user_disconnect = false;
/* ★ R2: 本次连接事务目标设备的地址类型 (连接成功后写入连接记录要用;
 *   esp_hidh_dev_t 只给 bda, 不给 addr_type)。 */
static uint8_t s_conn_try_addr_type = 0;
/* ★ 连接任务"正在收尾"标志。s_conn_task_alive 归零之后, 任务还要跑 bt_conn_failed
 *   (里面可能动 bt_core), 才真正退出。若此刻 bt_release_flush 抢先把栈拆了,
 *   收尾代码就会踩到已释放的信号量。所以"可以释放"的条件是
 *   `alive == 0 && !exiting`, 两个都对才算事务彻底结束。 */
static volatile bool s_conn_task_exiting = false;
/* 保护 s_conn_task_alive 的"检查 + 占位"。三个上下文都会发起连接
 * (页面连接任务 / 蓝牙扫描回调 / 回连任务 bt_rec), 普通 int 的 check-then-act
 * 不是原子的: 两人同时看到 0 就会各建一个 open 任务 —— 正是要禁止的并发 open。 */
static portMUX_TYPE s_conn_mux = portMUX_INITIALIZER_UNLOCKED;

/* bt_connect_async 的三种结果: 必须与 bool 区分 ——
 * "已有事务在飞" 不是连接失败, 调用方不能记连败、不能报"连接失败"。 */
#define BT_CONN_STARTED   1
#define BT_CONN_BUSY      0
#define BT_CONN_ERR     (-1)

static void bt_conn_begin(uint32_t gen, const uint8_t bda[6], uint8_t addr_type)
{
    if (bda) { memcpy(s_conn_try_bda, bda, 6); s_conn_try_valid = true; }
    s_conn_try_addr_type = addr_type;
    s_conn_gen          = gen;
    s_conn_giveup       = false;
    s_conn_deadline_ms  = esp_timer_get_time() / 1000 + BT_CONNECT_TIMEOUT_MS;
    s_conn_slow_logged  = false;
}
static void bt_conn_end(void) { s_conn_deadline_ms = 0; }

/* gen: 事务代号 (0 = 不校验, 仅框架事件用)。
 * open_done: 本次 open 是否**已经返回**。只有它返回了才允许碰链路/拆栈 ——
 *   事务仍卡在 esp_hidh_dev_open 里时动这些资源就是 UAF。
 * ★ 2026-11-23 R1 修正(读 IDF 6.0 源码确认): 连接**建立中**调 esp_ble_gap_disconnect()
 *   走 L2CA_DisconnectReq → l2cu_find_lcb_by_bd_addr() 返回 NULL 直接 return FALSE,
 *   什么都取消不了(API 层还是异步入队, 日志看着像"已取消")。真正能终止它的只有
 *   让 open 自己返回。所以这里**不再"事务进行中就主动取消"**, 只做事后清理。 */
static void bt_conn_failed(uint32_t gen, const char *why, bool open_done)
{
    if (gen != 0 && gen != s_conn_gen) {
        ESP_LOGD(TAG, "丢弃上一轮连接事务(gen %u≠%u)的失败事件: %s",
                 (unsigned)gen, (unsigned)s_conn_gen, why);
        return;
    }
    s_conn_user_init = false;   /* ★ 2026-11-24: 事务已收敛(失败) ⇒ 不再算"用户主动连接成功" */
    s_connecting = false;
    bt_conn_end();
    bool can_cleanup = open_done && s_conn_task_alive <= 0;
    if (can_cleanup) {
        if (s_conn_try_valid) {
            bt_core_cancel_pending(s_conn_try_bda);   /* 事后清理: 半开链路/残留连接 */
            s_conn_try_valid = false;
        } else {
            bt_core_disconnect();
        }
    }
    if (s_conn_fail_streak < 99) s_conn_fail_streak++;
    ESP_LOGW(TAG, "连接失败(%s), 连败 %d 次%s", why, s_conn_fail_streak,
             can_cleanup ? "" : " [open 仍在阻塞, 不清理/不拆栈]");
    if (can_cleanup && s_conn_fail_streak >= BT_CONN_FAILS_TO_RESET) {
        s_need_stack_reset = true;
    }
    if (s_reconnect_task) xTaskNotifyGive(s_reconnect_task);
}

/* 上次连接设备名 (持久化): 用于判断"新手柄"——设备名与上次不同则清空旧映射 */
static char  s_last_conn_name[32] = {0};
static bool  s_last_conn_name_valid = false;

/* 扫描结果缓存 (转发给上层回调)
 * ★ 2026-11-02: 1.6KB, 只在"扫描那几秒"里用, 平时纯占内部 RAM ⇒ 放 PSRAM。
 *   无 DMA、不进 ISR, 拷贝发生在蓝牙回调任务里, 安全。 */
#define BT_SCAN_CACHE_MAX 40
static EXT_RAM_BSS_ATTR bt_device_t s_dev_cache[BT_SCAN_CACHE_MAX];
static int s_dev_cache_count = 0;

/* 回调 */
static bt_scan_callback_t      s_scan_cb = NULL;
static bt_device_found_cb_t    s_cont_cb = NULL;
static bt_connect_cb_t         s_connect_cb = NULL;
static bt_connect_progress_cb_t s_prog_cb = NULL;

/* ★ 2026-11-23 R3: 用户主动扫描遇到连接事务在飞时, 不直接拒绝(否则页面立刻显示
 *   "未发现设备"), 而是挂起本标志, 等事务退出后自动补发一轮扫描。 */
static volatile bool s_scan_wait_conn = false;

static bool s_inited = false;   /* bt_core 是否已初始化 */
/* V1.5.x 省电: 蓝牙按需开启 — 完整初始化(映射/NVS/核心/回连任务)是否完成.
 * 开机不再初始化; 首次 bt_manager_enable (进蓝牙/手柄设置页) 时触发. */
static volatile bool s_full_init_done = false;
static SemaphoreHandle_t s_init_done_sem = NULL;   /* enable 同步等待初始化完成 */
/* bt_init 任务(内部 RAM 静态栈)是否正在跑: 防止并发 enable 重复创建同一静态任务
 * (静态栈/TCB 是唯一的, 重复 xTaskCreateStatic 会踩内存). */
static volatile bool s_init_task_running = false;

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
#define NVS_KEY_GPM     "gpm"      /* ★ C2 手柄档案表 (名字 → 三套映射, 最多 GPM_MAX 只) */
#define NVS_KEY_UOFF    "uoff"     /* ★ 开机自动回连开关 (0=开 1=关; 沿用旧键名, 免迁移) */

/* ========================================================================
 *  NVS 存取
 * ======================================================================== */
static nvs_handle_t s_nvs = 0;
static bool s_nvs_open = false;

static void nvs_init_handle(void)
{
    if (s_nvs_open) return;
    /* ★ 2026-11-24: 原来失败只打 D 级 —— 而 D 级在发布版不输出, 于是"连接记录重启就没了"
     *   (NVS 根本没写进去) 这件事在日志里**完全静默**。改成 E 级并带错误码。 */
    esp_err_t e = svc_nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (e == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGE(TAG, "nvs namespace '%s' 打开失败: %s", NVS_NS, esp_err_to_name(e));
    }
}

static bool nvs_load_blob(const char *key, void *dst, size_t len)
{
    nvs_init_handle();
    if (!s_nvs_open) return false;
    if (svc_nvs_get_blob(s_nvs, key, dst, &len) != ESP_OK) return false;
    return true;
}

static void nvs_save_blob(const char *key, const void *src, size_t len)
{
    nvs_init_handle();
    if (!s_nvs_open) {
        /* ★ 2026-11-24: 原来直接 return, 调用方以为写成功了 —— 连接记录/映射/档案全丢
         *   却查不出原因。至少留一条 E 级痕迹。 */
        ESP_LOGE(TAG, "nvs 保存 '%s' 跳过: namespace 未打开 (%u B)", key, (unsigned)len);
        return;
    }
    esp_err_t e = svc_nvs_set_blob(s_nvs, key, src, len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs 保存 '%s' 失败: %s (%u B)", key, esp_err_to_name(e), (unsigned)len);
        return;
    }
    esp_err_t c = svc_nvs_commit(s_nvs);
    if (c != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit '%s' 失败: %s", key, esp_err_to_name(c));
    }
}

/* ============================================================================
 *  ★ 身份比较统一口径 (2026-10-02)
 *  规则 (参照 Bluepad32): **名字非空且相同 → 同一只设备**; 名字缺失才退化比地址。
 *  原先各处口径不一 —— `add_to_history()` / `reset_pad_clean()` 还额外要求 `has_name`
 *  标志为真, 而 `is_known_device()` 只看名字串。这手柄会漂 MAC 且名称有时读不到,
 *  口径不统一就会出现"同一只被判成新手柄 → 历史里多出一条重复"。
 *  现在统一走 btm_same_name(); `has_name` 保留为**只写**字段 (不动结构体 = NVS 布局不变)。
 * ==========================================================================*/
static bool btm_same_name(const char *a, const char *b)
{
    return (a && b && a[0] && b[0] && strncmp(a, b, 32) == 0);
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
 *  ★ 2026-11-24: 功能键"上升沿锁存" (事件域防抖)
 * ------------------------------------------------------------------------
 *  报障: "手柄连点快一点就没有及时反馈, 点得越快越慢"。
 *  根因 (两条叠加, 都在采样侧, 与映射算法无关):
 *    ① 主循环空闲帧周期 60ms (main.c: budget_ms = touch_hit ? 16 : 60),
 *       手柄导航是**电平查询** —— 一次短促点击整段落在两次采样之间就被彻底丢弃;
 *       点得越快, 落在间隙的概率越高 ⇒ 越快越没反应。物理键早已为此改成
 *       "5ms 扫描 + 队列 + 唤醒主循环" (input.c:1016 注释同源), 手柄没接上。
 *    ② 输入层还有 NAV_DEBOUNCE_MS=30 的稳定窗, 短脉冲先被防抖吃掉。
 *  修法: 在 HID 报告回调 (≈60~100Hz, 与 UI 帧率无关) 里确认上升沿并锁存一位;
 *       输入层 take 到就立刻投递动作 (跳过 30ms 防抖), 同时唤醒主循环。
 *  ⛔ 不改映射: 判定仍走 s_map[f] → phys_pressed(), 与长按重复的电平路径共存。
 * ======================================================================== */
#define FUNC_EDGE_STABLE_MS  12   /* 上升沿稳定确认窗 (小于一份 HID 报告的间隔会更迟钝) */

static volatile uint32_t s_func_edge;        /* 待消费的上升沿位图 (bit = func_t) */
static uint32_t         s_func_prev;         /* 已确认按下的功能键 (下降沿才回落) */
static uint32_t         s_func_cand_ms[FUNC_MAX]; /* 候选按下时刻, 0=无候选 */
static bt_key_edge_cb_t s_key_edge_cb;       /* 有边沿 → 唤醒主循环 */

void bt_manager_set_key_edge_cb(bt_key_edge_cb_t cb) { s_key_edge_cb = cb; }

/* ⚠ 位图被两个任务碰: BT 回调任务置位 (for), 主循环取走 (and)。
 *   用 __atomic 而不是 `|=` / `&=` —— 后者是读-改-写, 并发时会把刚置上的
 *   边沿清掉, 表现为"偶尔还是丢一次点击"。 */
bool bt_manager_take_key_edge(func_t f)
{
    if (f < 0 || f >= FUNC_MAX) return false;
    uint32_t bit = 1u << (uint32_t)f;
    uint32_t old = __atomic_fetch_and(&s_func_edge, ~bit, __ATOMIC_SEQ_CST);
    if (!(old & bit)) return false;
    /* 还有别的边沿在排队 → 再踢一脚, 让主循环立刻跑下一圈取它, 不必等帧预算 */
    if (__atomic_load_n(&s_func_edge, __ATOMIC_SEQ_CST) && s_key_edge_cb) s_key_edge_cb();
    return true;
}

void bt_manager_clear_key_edges(void)
{
    __atomic_store_n(&s_func_edge, 0u, __ATOMIC_SEQ_CST);
    s_func_prev = 0;
    for (int i = 0; i < FUNC_MAX; i++) s_func_cand_ms[i] = 0;
}

/* 每份 HID 报告调用一次: 确认到的上升沿置入 s_func_edge */
static void func_edge_scan(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool fired = false;
    for (int f = 0; f < FUNC_MAX; f++) {
        uint32_t bit = 1u << (uint32_t)f;
        bool p = phys_pressed(s_map[f]);
        if (!p) {
            s_func_prev &= ~bit;
            s_func_cand_ms[f] = 0;           /* 松开: 清候选, 允许下次重新确认 */
            continue;
        }
        if (s_func_prev & bit) continue;     /* 已确认按下, 等松开 */
        if (s_func_cand_ms[f] == 0) {
            s_func_cand_ms[f] = now ? now : 1u;
        } else if ((uint32_t)(now - s_func_cand_ms[f]) >= FUNC_EDGE_STABLE_MS) {
            s_func_prev |= bit;
            __atomic_fetch_or(&s_func_edge, bit, __ATOMIC_SEQ_CST);
            fired = true;
        }
    }
    if (fired && s_key_edge_cb) s_key_edge_cb();
}

/* ★ 2026-11-22: 任意物理键 —— 遍历 phys_t 全域 (方向 0..3 + 按钮 4..23).
 * 与 bt_manager_is_key_pressed() 的区别: 那个只查"映射到功能键"的 7 个,
 * 这里查**手柄上所有按得下去的键**, 供屏保"任意键唤醒"用。 */
bool bt_manager_any_key_pressed(void)
{
    for (int p = 0; p < PHYS_MAX; p++) {
        if (phys_pressed((phys_t)p)) return true;
    }
    return false;
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

    /* ★ 省电: 一轮预算用尽还没连上 → **关射频**, 不再自己重试, 等下一次唤醒
     *   (唤醒: 进手柄页 / 按任意键 / 下次开机)。原"退避 30/60/120/300s 封顶后继续
     *   循环"是常驻耗电, 已取消。 */
    if (s_auto_reconnect_pending && !rc_budget_left()) {
        ESP_LOGD(TAG, "[EV] 回连预算用尽且未连上 → 关射频省电 (等唤醒)");
        s_auto_reconnect_pending = false;
        s_rc_target_valid = false;
        bt_manager_disable();
        return;
    }
    /* 回连目标: 本次开机连接过的地址优先 (最新); 无则回退 NVS 持久化的最近历史,
     * 保证"开机窗口超时后长按确认键"等场景仍能自动回连已配对手柄. */
    if (s_last_conn_addr_valid) {
        memcpy(s_rc_target_addr, s_last_conn_addr, 6);
        snprintf(s_rc_target_name, sizeof s_rc_target_name, "%s", s_last_conn_name);
    } else if (s_history_count > 0) {
        memcpy(s_rc_target_addr, s_history[0].bd_addr, 6);
        snprintf(s_rc_target_name, sizeof s_rc_target_name, "%s", s_history[0].name);
    } else {
        return;
    }
    s_rc_target_valid = true;
    s_auto_reconnect_pending = true;
    rc_budget_start();
    ESP_LOGD(TAG, "[EV] 回连目标: name=\'%s\' addr=%02x:%02x:%02x:%02x:%02x:%02x (名字优先)",
             s_rc_target_name, s_rc_target_addr[0], s_rc_target_addr[1], s_rc_target_addr[2],
             s_rc_target_addr[3], s_rc_target_addr[4], s_rc_target_addr[5]);
    /* V1.5.x: 已有扫描在进行 (长按确认键扫描弹窗/添加设备) → 仅挂起自动回连标志,
     * 由扫描完成回调在结果里匹配回连目标连接, 不重复发起扫描 (避免 old-results 扫描风暴). */
    if (bt_manager_is_scanning()) return;
    /* ★ R2: 有连接事务在飞时扫描会被闸门拒绝 (连接用了 GAP 状态机), 别在这里白试;
     *   回连任务会在事务退出后继续推进。 */
    if (s_connecting || s_conn_task_alive > 0) {
        ESP_LOGD(TAG, "[EV] 连接事务进行中 ⇒ 暂不发起回连扫描");
        return;
    }

    /* ★ 2026-10-04 修 (实测根因): 原先这里**直接** bt_core_scan(5), 没有确保蓝牙核心已使能。
     *   本工程的蓝牙是"按需开启 + 预算用尽就关栈省内部 RAM"(见文件头与 bt_reconnect_task),
     *   所以走到这里时栈往往**是关着的** → esp_hid_scan → esp_ble_gap_set_scan_params
     *   返回 259 (ESP_ERR_INVALID_STATE) → "scan done: 0 devices" → 配对永远失败。
     *
     *   ⛔ 不能在这里直接调 core_ensure_init() 补上: 它会经 esp_bt_controller_init 禁用 cache,
     *      只有**栈在内部 RAM** 的任务才安全 (见 bt_manager_init 的注释: 放 PSRAM 栈会触发
     *      esp_task_stack_is_sane_cache_disabled() 断言 → 卡死重启)。本函数由屏保监督等
     *      上层任务驱动, 栈不保证在内部 RAM。
     *   ✅ 正解 = 交回 bt_rec 任务: 它的静态栈固定在内部 RAM, 会自己 core_ensure_init()
     *      再回连 (连不上还会按名字补一次短扫, 行为等价)。
     *   判据: s_inited 为假就说明栈没起来, 这时候扫是白扫。 */
    if (!s_inited) {
        ESP_LOGD(TAG, "[EV] 蓝牙栈未使能 → 交给 bt_rec 任务初始化后再回连 (避免 259)");
        bt_manager_wake_reconnect();
        return;
    }
    /* ★ R2: 扫描事务可能被占用 (另一个扫描/连接在飞) —— 这里不再静默丢弃,
     *   打一条 D 级说明, 回连任务下一轮会继续推进 (它自己也有扫描路径)。 */
    if (!bt_core_scan(5)) {
        ESP_LOGD(TAG, "[EV] 回连扫描未启动 (扫描事务忙) ⇒ 交回 bt_rec 下一轮处理");
    }
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

/* ============================================================================
 * ★ C2 (2026-10-02) 手柄档案表 (per-gamepad profile)
 *   问题: 一只"经常变 MAC"的手柄, 旧实现把映射存成"全局唯一一套 + 用上次连接名字
 *         判断是否新手柄" → 换手柄 / 地址漂移 就会清空或错套用户映射。
 *   方案 (Bluepad32 式): **名字即身份**。每只(同名)手柄各存一份
 *         {键映射 / 补充映射 / GB 映射}; 连接时按名字取用; 从没见过 → 建档 + 默认值。
 *   `mapped` 只在**用户真正存过映射**时置位 (映射向导完成 / 手动改键), 用于决定是否
 *   弹"请做按键映射"引导, 避免每次回连都重复打扰。
 *   NVS 新键 `gpm`, 与旧的三套全局键 (kmap/supmap/gbmap) 并存: 旧键仍作为
 *   "尚未连接任何手柄"时的默认值。
 * ==========================================================================*/
#define GPM_MAGIC   0x314D5047u   /* "GPM1" */
#define GPM_MAX     6

typedef struct {
    char   name[33];              /* 身份: 手柄设备名 (与 BLE 地址无关) */
    bool   mapped;                /* 用户是否已完成过按键映射 */
    phys_t kmap[FUNC_MAX];
    phys_t supmap[SUP_MAX];
    phys_t gbmap[GB_MAP_MAX];
} gpm_ent_t;

typedef struct {
    uint32_t  magic;
    int       count;
    gpm_ent_t e[GPM_MAX];
} gpm_t;

EXT_RAM_BSS_ATTR static gpm_t s_gpm;   /* 手柄映射表: 只有接手柄时读, 待机不占内部 RAM */

static void gpm_save(void)
{
    s_gpm.magic = GPM_MAGIC;
    nvs_save_blob(NVS_KEY_GPM, &s_gpm, sizeof(s_gpm));
}

/* 按名字查档案; create=true 且不存在(还有空间)时新建并立即落盘。 */
static gpm_ent_t *gpm_find(const char *name, bool create)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < s_gpm.count; i++) {
        if (s_gpm.e[i].name[0] && strncmp(s_gpm.e[i].name, name, 32) == 0)
            return &s_gpm.e[i];
    }
    if (!create || s_gpm.count >= GPM_MAX) return NULL;

    gpm_ent_t *e = &s_gpm.e[s_gpm.count];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->mapped = false;
    memcpy(e->kmap,   k_default_map, sizeof(e->kmap));
    memcpy(e->supmap, k_default_sup, sizeof(e->supmap));
    memcpy(e->gbmap,  k_default_gb,  sizeof(e->gbmap));
    s_gpm.count++;
    gpm_save();
    ESP_LOGI(TAG, "[EV] 手柄档案表: 新建 \'%s\' (%d/%d)", e->name, s_gpm.count, GPM_MAX);
    return e;
}

/* 把当前内存里的三套映射写回"当前手柄"的档案 (由 save_*_map 调用)。 */
static void gpm_store_cur(void)
{
    if (!s_gpm.magic) return;                 /* 档案表未载入 → 不写 */
    const char *nm = NULL;
    if (s_conn_info.available && s_conn_info.name[0])       nm = s_conn_info.name;
    else if (s_last_conn_name_valid && s_last_conn_name[0]) nm = s_last_conn_name;
    if (!nm) return;

    gpm_ent_t *e = gpm_find(nm, true);
    if (!e) return;
    memcpy(e->kmap,   s_map, sizeof(e->kmap));
    memcpy(e->supmap, s_sup, sizeof(e->supmap));
    memcpy(e->gbmap,  s_gb,  sizeof(e->gbmap));
    e->mapped = true;
    gpm_save();
    ESP_LOGI(TAG, "[EV] 手柄档案表: 已保存 \'%s\' 的映射 (mapped)", e->name);
}

static void gpm_load(void)
{
    memset(&s_gpm, 0, sizeof(s_gpm));
    if (!nvs_load_blob(NVS_KEY_GPM, &s_gpm, sizeof(s_gpm)) || s_gpm.magic != GPM_MAGIC) {
        memset(&s_gpm, 0, sizeof(s_gpm));
        s_gpm.magic = GPM_MAGIC;
        s_gpm.count = 0;
        ESP_LOGD(TAG, "[EV] 手柄档案表: 空表 (无存档或 magic 不符)");
        return;
    }
    if (s_gpm.count < 0) s_gpm.count = 0;
    if (s_gpm.count > GPM_MAX) s_gpm.count = GPM_MAX;

    /* 迁移: 老固件只有"全局一套映射"。档案表为空但记得上次连接的手柄 → 认作它的档案。 */
    if (s_gpm.count == 0 && s_last_conn_name_valid && s_last_conn_name[0]) {
        gpm_ent_t *e = gpm_find(s_last_conn_name, true);
        if (e) {
            memcpy(e->kmap,   s_map, sizeof(e->kmap));
            memcpy(e->supmap, s_sup, sizeof(e->supmap));
            memcpy(e->gbmap,  s_gb,  sizeof(e->gbmap));
            e->mapped = true;
            gpm_save();
            ESP_LOGD(TAG, "[EV] 手柄档案表: 迁移旧配置 → \'%s\'", e->name);
        }
    }
    ESP_LOGI(TAG, "[EV] 手柄档案表: 载入 %d 条 (上限 %d)", s_gpm.count, GPM_MAX);
}

void bt_manager_save_key_map(void)
{
    nvs_save_blob(NVS_KEY_MAP, s_map, sizeof(s_map));
    gpm_store_cur();   /* ★ C2: 同步写回当前手柄档案 (并标记 mapped) */
}

void bt_manager_reset_key_map(void)
{
    memcpy(s_map, k_default_map, sizeof(s_map));
}

/* ★ 2026-10-27: 映射向导的备份/回滚。
 *   向导在 map_poll 里**边按边写 s_map**(poll_new_press 内部直接改映射)，所以中途
 *   取消 / 40s 超时若不管，内存里就留下半套映射 —— 用户回到游戏会发现方向键错乱，
 *   必须重连手柄或重启才恢复。进向导前 backup，取消/超时 restore。 */
static phys_t s_map_backup[FUNC_MAX];
static bool   s_map_backup_valid;

void bt_manager_backup_key_map(void)
{
    memcpy(s_map_backup, s_map, sizeof(s_map));
    s_map_backup_valid = true;
}

void bt_manager_restore_key_map(void)
{
    if (!s_map_backup_valid) return;
    memcpy(s_map, s_map_backup, sizeof(s_map));
    s_map_backup_valid = false;
    ESP_LOGI(TAG, "[EV] 按键映射已回滚 (取消/超时, 保持原映射)");
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
    gpm_store_cur();   /* ★ C2 */
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
    gpm_store_cur();   /* ★ C2 */
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
    /* ★ 2026-11-24 取证: 写完立刻回读 hcnt 校验。
     *   "连接记录重启就没了" 时这一行能直接区分两种故障:
     *     · 回读失败/不一致 → 根本没写进 NVS (写入或 commit 出问题);
     *     · 回读 OK 但重启后为 0 → 写入没事, 是启动时被覆盖/未加载。 */
    {
        int rb = -1;
        size_t rl = sizeof(rb);
        nvs_init_handle();
        if (s_nvs_open && svc_nvs_get_blob(s_nvs, NVS_KEY_HCNT, &rb, &rl) == ESP_OK) {
            ESP_LOGI(TAG, "历史落盘校验: 内存 %d 条 / NVS %d 条 %s",
                     s_history_count, rb, (rb == s_history_count) ? "OK" : "★不一致★");
        } else {
            ESP_LOGE(TAG, "历史落盘校验: 回读 '%s' 失败 (内存 %d 条)",
                     NVS_KEY_HCNT, s_history_count);
        }
    }
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

/* ★ C7 (2026-10-02) 忘记此手柄
 *   按**名字**(身份)同时清掉: ①连接记录 ②手柄档案(gpm) ③回连目标。
 *   手柄换了 MAC 也一样认得 —— 这正是"名字即身份"的用处。 */
bool bt_manager_forget_device(const char *name)
{
    if (!name || !name[0]) return false;

    /* ① 连接记录 (同名全删); 顺手清掉该名字名下所有 BLE 绑定 (配对钥匙) */
    int w = 0;
    for (int i = 0; i < s_history_count; i++) {
        if (btm_same_name(s_history[i].name, name)) {
            (void)bt_core_remove_bond(s_history[i].bd_addr);
            continue;
        }
        if (w != i) s_history[w] = s_history[i];
        w++;
    }
    if (w != s_history_count) { s_history_count = w; history_save(); }

    /* ② 手柄档案 (映射/是否已映射) */
    int k = 0;
    for (int i = 0; i < s_gpm.count; i++) {
        if (s_gpm.e[i].name[0] && strncmp(s_gpm.e[i].name, name, 32) == 0) continue;
        if (k != i) s_gpm.e[k] = s_gpm.e[i];
        k++;
    }
    if (k != s_gpm.count) { s_gpm.count = k; gpm_save(); }

    /* ③ 正连着这只手柄 → 清当前绑定并断开 (语义: 忘掉 = 要重新配对) */
    if (s_connected && s_conn_info.available && s_conn_info.name[0] &&
        strncmp(s_conn_info.name, name, 32) == 0) {
        if (s_conn_addr_valid) (void)bt_core_remove_bond(s_conn_addr);
        bt_manager_disconnect();
    }

    /* ④ 若正是"上次连接 / 回连目标" → 停掉回连 (不写 flash 的都不管) */
    if (s_last_conn_name_valid && strncmp(s_last_conn_name, name, 32) == 0) {
        s_last_conn_name_valid = false;
        s_last_conn_name[0] = '\0';
        nvs_save_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));
        s_last_conn_addr_valid = false;
        s_rc_target_valid = false;
        s_auto_reconnect_pending = false;
    }
    ESP_LOGD(TAG, "[EV] 已忘记手柄 \'%s\' (历史+档案已删, 回连已停)", name);
    return true;
}

/* ★ 身份判定 (参照 Bluepad32: 身份 = 名字/VID/PID, 地址只是属性)
 * 这手柄经常变 MAC, 只按地址比会把"同一只"误判成新手柄 → 又清映射又回连不上。
 * 规则: 名字相同 → 同一只; 名字都没有时退化用地址。 */
bool bt_manager_is_known_device(const uint8_t *bd_addr, const char *name)
{
    for (int i = 0; i < s_history_count; i++) {
        /* ★ 统一口径: 名字非空且相同 → 同一只 (不再要求 has_name 标志) */
        if (btm_same_name(s_history[i].name, name)) return true;
        if (bd_addr && memcmp(s_history[i].bd_addr, bd_addr, 6) == 0) return true;
    }
    return false;
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
        bool same_name = btm_same_name(dev->name, s_history[i].name);
        if (same_addr || same_name) {
            memmove(&s_history[1], &s_history[0], sizeof(bt_device_t) * i);
            s_history[0] = *dev;
            history_save();
            /* ★ 取证: "复用"= 顶替旧条目并移到最前 (不是新增一条) */
            ESP_LOGI(TAG, "[EV] 历史: 复用 \'%s\' (旧位置 %d → 最前, 共 %d 条, 按%s命中)",
                     dev->name[0] ? dev->name : "(无名)", i, s_history_count,
                     same_name ? "名字" : "地址");
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
    /* ★ 取证: "新增"= 历史里又占一格 (满则顶替最旧) */
    ESP_LOGI(TAG, "[EV] 历史: 新增 \'%s\' (%d/%d%s)", dev->name[0] ? dev->name : "(无名)",
             s_history_count, BT_HISTORY_MAX,
             s_history_count >= BT_HISTORY_MAX ? ", 已顶替最旧" : "");
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
    bt_manager_clear_key_edges();   /* 2026-11-24: 新的一轮校准, 清掉上升沿锁存 */
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
    /* ★ 2026-11-24: 每份报告（≈60~100Hz）确认一次功能键上升沿并锁存 ——
     *   手柄导航靠它摆脱"主循环 60ms 采样 + 30ms 防抖"的丢帧问题。 */
    func_edge_scan();
    /* 通用键盘: 若当前设备是键盘, 解析并(变化时)触发键盘回调 */
    bt_kbd_feed(data, len);
}

/* 默认键盘事件(第一步: 先打日志验证"透传原信号"通路; 页面/文本接线为后续) */
static void on_bt_kbd_event(const bt_kbd_state_t *st)
{
    if (!st) return;
    /* V1.2 性能: 原为 ESP_LOGI —— 按住某键时 HID 报告持续到来(≈100Hz),
     * 本循环会对"每个仍按住的键"重复打日志 → 高频刷屏并拖慢按键处理.
     * 降为 DEBUG (默认级别静默; 排查时 esp_log_level_set("BT", ESP_LOG_DEBUG)). */
    for (uint8_t i = 0; i < st->key_count; i++) {
        char ch = bt_kbd_hid_to_ascii(st->keys[i], st->modifiers);
        ESP_LOGD(TAG, "[kbd] press hid=0x%02x mods=0x%02x -> '%c'",
                 st->keys[i], st->modifiers, ch ? ch : '?');
    }
}

/* ★ 2026-11-22: 逐键边沿 → 输入层 (1:1, 不做按键映射)。
 *   键盘"什么功能就什么功能": 字母/数字/符号照原样出字符, 方向/Enter/Esc/退格
 *   交给输入层按 bt_kbd_act_t 分流 (有文本焦点给页面, 无焦点走系统导航)。
 *   这里**不参与** bt_map 的手柄映射表 —— 两条路径彻底分离。 */
static void on_bt_kbd_key_event(uint8_t usage, uint8_t mods, bool pressed)
{
    if (!s_kbd_sink) return;
    int act = bt_kbd_hid_to_action(usage);
    if (act == BTK_ACT_IGNORE) return;
    char ch = 0;
    if (act == BTK_ACT_CHAR) {
        ch = bt_kbd_hid_to_ascii(usage, mods);
        if (!ch) return;            /* 无可映射字符 (F1~F12/媒体键等) → 丢弃 */
    }
    s_kbd_sink(act, ch, pressed);
}

static void on_connected_cb(bool connected, esp_hidh_dev_t *dev)
{
    /* ★ 2026-11-24: 先消费"用户主动连接"标记 —— 这是本次连上能否引导映射的唯一依据。
     *   后台自动回连(开机/扫描窗等待/唤醒)不走 bt_manager_connect_device, 标记恒为假。 */
    bool user_init = s_conn_user_init;
    s_conn_user_init = false;
    if (connected && dev) {
        /* ★★ 2026-11-23 R2: 这次"连上"该不该算数 ★★
         *   R1 已确认: 连接建立中调用 esp_ble_gap_disconnect() 在 L2CAP 侧找不到 LCB,
         *   取消**无效**, 事务会照常跑完。所以用户点了"断开"/系统挂起/已关蓝牙之后,
         *   事务仍可能成功 —— OPEN 成功这一刻是唯一有效的处理点: 直接关掉链路,
         *   别让页面/游戏收到一个"不该存在"的连接(否则表现为"断了又自己连上")。 */
        if (s_user_disconnect || s_suspended || s_power_off) {
            ESP_LOGI(TAG, "连接已建立, 但当前状态是(用户断开=%d 挂起=%d 关电=%d) ⇒ 立即关闭",
                     (int)s_user_disconnect, (int)s_suspended, (int)s_power_off);
            s_connected = false;
            s_connecting = false;
            s_conn_giveup = false;
            bt_conn_end();
            s_conn_try_valid = false;
            /* ★★★ 2026-11-23 R3: 这里**绝不能**清 s_user_disconnect ★★★
             *   清了之后紧接着的 bt_core_disconnect() 会产生 CLOSE 事件, 而
             *   on_disconnected_cb 看到 user_asked==false 就会 bt_manager_wake_reconnect() ——
             *   正是"用户点了断开, 1~3 秒后又自己连上"的老 bug, 只是换了条路径进来
             *   (断开的瞬间恰好有连接事务在 open 里)。标记必须留给 CLOSE 回调消费:
             *   它在那里清标记并抑制本次回连。即使 CLOSE 没来也无害 ——
             *   下次显式连接/enable/resume 都会清它。 */
            bt_core_disconnect();      /* 异步关链路, CLOSE 事件会再兜一次状态 */
            if (s_connect_cb) s_connect_cb(false);
            return;
        }
        s_connected = true;
        s_connecting = false;
        s_conn_giveup = false;    /* ★ R1: 连上了就不存在"UI 已放弃" */
        bt_manager_clear_key_edges();  /* 2026-11-24: 新链路, 旧锁存一律作废 */
        /* ★ 2026-11-24: 这里**不再**自动清「开机自动回连」开关。
         *   开关的语义已从"用户关过蓝牙"变成用户在**手柄页明确选择**的偏好;
         *   连上手柄不该替用户改回来 (他可能故意关掉开机回连, 只是这次手动连了)。 */
        s_auto_reconnect_pending = false;
        bt_conn_end();            /* ★ 收敛: 清截止时间 */
        s_conn_fail_streak = 0;   /* ★ 连上了: 连败计数归零 */
        s_conn_try_valid = false;
        /* ★ R1: 事务真的连上了 ⇒ 取消任何"挂起的关/拆栈"
         *   (典型: 窗口超时那一刻任务还在 open 里, 我们把拆栈推迟了; 结果它连上了,
         *    这时当然不该再拆)。只有用户明确关电(s_power_off)时才继续释放。 */
        if (!s_power_off) s_release_pending = 0;
        /* ★ 2026-11-23 策略 2: **真正连上**才结束回连窗口。
         *   连接已异步化, "发起连接"不再等于"连上"; 窗口若在发起时就关掉,
         *   4s 后超时失败就再也不会重试了。窗口内的计时/超时逻辑在 s_connecting
         *   期间本来就会 continue, 所以这里清标志不会和窗口超时打架。 */
        s_boot_win_active = false;

        /* 记录当前连接地址 */
        const uint8_t *bda = esp_hidh_dev_bda_get(dev);
        if (bda) {
            memcpy(s_conn_addr, bda, 6);
            memcpy(s_last_conn_addr, bda, 6);
            s_conn_addr_valid = true;
            s_last_conn_addr_valid = true;
        }
        s_last_connect_was_new = s_pending_new;
        /* 填充连接信息 */
        memset(&s_conn_info, 0, sizeof(s_conn_info));
        s_conn_info.battery = 255;
        s_conn_info.vid = esp_hidh_dev_vendor_id_get(dev);
        s_conn_info.pid = esp_hidh_dev_product_id_get(dev);
        const char *nm = esp_hidh_dev_name_get(dev);
        if (!nm) nm = "";
        /* ★ R2: 名字为空时退回"发起连接时记下的名字"。
         *   否则会写进一条无名记录(列表里显示 "??"), 并按空名字查档案 ⇒ 每次都被当成
         *   新手柄重新引导映射。现场: 部分手柄的名字要等 GATT 读完才可读, OPEN 事件
         *   那一刻 esp_hidh_dev_name_get() 仍可能是空串。 */
        if (!nm[0] && s_pending_name[0]) nm = s_pending_name;
        snprintf(s_conn_info.name, sizeof(s_conn_info.name), "%.31s", nm);
        snprintf(s_conn_info.model, sizeof(s_conn_info.model), "%.23s", nm);
        s_conn_info.available = true;

        /* ★★ 2026-11-23 R2: 连接记录只在**真正连上**之后写 ★★
         *   原来在 bt_manager_connect_device() 发起连接前就写, 于是"点一下没连上"也
         *   新增/顶替一条记录, 并立刻成为下一次自动回连的目标(去抓一只从没连成功过的
         *   设备)。现在只在这里写, 语义 = "连接记录 = 真正连成功过的设备"。
         *   去重仍走 bt_manager_add_to_history 的统一口径 (名字优先, 地址兜底):
         *   地址漂移的手柄同名不同址 → 顶替旧条目并移到最前, 不会产生重复档案。 */
        {
            bt_device_t rec;
            memset(&rec, 0, sizeof(rec));
            if (bda) memcpy(rec.bd_addr, bda, 6);
            rec.addr_type = s_conn_try_addr_type;
            snprintf(rec.name, sizeof(rec.name), "%.31s", nm);
            rec.has_name = (nm[0] != '\0');
            rec.rssi = 0;   /* 连接后没有扫描 RSSI, 记 0 = 未知 (列表不依赖它排序) */
            bt_manager_add_to_history(&rec);
        }

        /* ★ C2 (2026-10-02): 按**名字**取用独立档案 (Bluepad32 式: 身份 = 名字, 地址只是属性)。
         *   同名 = 同一只手柄 → 载入它自己那份三套映射; 首次见到 → 默认值 + 建档。
         *   于是"手柄经常变 MAC"不再导致用户映射被反复清空 / 错套。 */
        gpm_ent_t *pe = gpm_find(nm, true);
        bool prof_ready = false;
        if (pe) {
            prof_ready = pe->mapped;
            if (pe->mapped) {
                memcpy(s_map, pe->kmap,   sizeof(s_map));
                memcpy(s_sup, pe->supmap, sizeof(s_sup));
                memcpy(s_gb,  pe->gbmap,  sizeof(s_gb));
                ESP_LOGI(TAG, "[EV] 载入档案 \'%s\' 的按键映射", pe->name);
            } else {
                bt_manager_reset_key_map();
                bt_manager_reset_sup_map();
                bt_manager_reset_gb_map();
                ESP_LOGI(TAG, "[EV] 手柄 \'%s\' 尚未映射 → 用默认映射", pe->name);
            }
        } else {
            bt_manager_reset_key_map();
            bt_manager_reset_sup_map();
            bt_manager_reset_gb_map();
            ESP_LOGD(TAG, "[EV] 手柄档案表已满, \'%s\' 只能用默认映射", nm);
        }
        snprintf(s_last_conn_name, sizeof(s_last_conn_name), "%.31s", nm);
        s_last_conn_name_valid = true;
        nvs_save_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));

        /* ★ C5 连接后自动映射: 该手柄**尚未做过按键映射** → 挂待办, 由手柄页的连接窗
         *   取走并衔接映射向导。
         *   注意: 这里**不落盘**(不调 save_*_map), 否则会立刻把档案标成 mapped。
         * ★ 2026-11-24 linit 调整: 只有**用户主动连接**(手柄页点选设备)才挂这个待办。
         *   后台自动回连(开机窗口/扫描窗等待/唤醒)连上后只更新连接状态 —— 不挂待办,
         *   于是不会把当前页面顶成手柄页、也不会自动弹映射向导。
         *   首次配对/手动映射不受影响: 走的就是"用户点选连接"这条(标记 user_init)。 */
        if (!prof_ready && user_init) {
            s_map_pending = true;
            snprintf(s_map_pending_name, sizeof s_map_pending_name, "%s", nm);
            ESP_LOGD(TAG, "[EV] 手柄 \'%s\' 无映射档案 → 待做按键映射 (等 UI 引导)", nm);
        }
        s_pending_new = false;

        /* 解析该设备 report descriptor */
        size_t num_maps = 0;
        esp_hid_raw_report_map_t *maps = NULL;
        if (esp_hidh_dev_report_maps_get(dev, &num_maps, &maps) == ESP_OK &&
            num_maps > 0 && maps) {
            bt_map_set_report_map(maps[0].data, maps[0].len);
            /* 通用键盘: 同一描述符喂给 bt_kbd 判定是否为键盘(页0x07) */
            bt_kbd_from_descriptor(maps[0].data, maps[0].len);
            bt_kbd_set_cb(on_bt_kbd_event);          /* 整帧快照 (诊断日志用) */
            bt_kbd_set_key_cb(on_bt_kbd_key_event);  /* ★ 逐键边沿 → 输入层 1:1 */
        }
        bt_map_calibrate_start();

        if (s_connect_cb) s_connect_cb(true);
    } else {
        /* ★ 2026-10-18: OPEN 失败事件 = 权威失败信号 → 统一收尾(断链路+记连败)，
         *   否则下一次 open 会撞 Already Connected / 半开链路，表现"失败一次后再也连不上"。
         * ★ R1: gen 传 0 表示"不校验代号"(框架事件无法携带) —— 但这路径实测在 BLE 下
         *   从不触发(ble_hidh.c 只投 p.open.status=ESP_OK), 失败由任务返回值收敛。 */
        bt_conn_failed(0, "OPEN 失败事件", true);
        if (s_connect_cb) s_connect_cb(false);
    }
}

static void on_disconnected_cb(bool connected, esp_hidh_dev_t *dev)
{
    (void)connected;
    (void)dev;
    bool user_asked = s_user_disconnect;
    s_connected = false;
    s_connecting = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
    /* ★ 2026-11-24: 断连清上升沿锁存 —— 否则重新连上后第一帧会补投一个"旧按键",
     *   表现为"刚连上就自己动了一下"。 */
    bt_manager_clear_key_edges();
    bt_conn_end();            /* ★ 断连不是"失败", 只清截止时间, 不清连败计数 */
    /* 断连即快速重连: 复位退避到基础间隔 + 唤醒重连任务立即尝试, 不必等退避睡满.
     * 否则断开前若曾重连失败退避到 300s(5分钟), 断开后要干等 5 分钟才重试 →
     * 表现为"不会自动重连已连接的手柄". 掉线后 ~60s 内 (立即复位后任务被唤醒即试) 重连. */
    s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
    /* ★★ 2026-11-23 R2: 用户主动断开 / 系统挂起 ⇒ **不**自动回连 ★★
     *   原来只判 s_power_off, 于是用户点"断开"后 CLOSE 事件一进来就唤醒回连任务,
     *   1~3 秒后又自己连上 —— 用户看到的"断不掉"。意图标记在此消费并清除。 */
    if (!s_power_off && !s_suspended && !user_asked) {
        bt_manager_wake_reconnect();   /* 非主动关电/挂起/断开 → 立即重连 */
    } else if (user_asked) {
        s_user_disconnect = false;
        s_auto_reconnect_pending = false;
        s_rc_target_valid = false;
        ESP_LOGI(TAG, "已按用户意图断开 ⇒ 本次不自动回连");
    }
    if (s_connect_cb) s_connect_cb(false);
}

/* ★ 2026-10-02 边扫边连: 扫描**过程中**扫到回连目标的瞬间就发起连接。
 *
 * 原来只有 on_scan_done_cb() 里做匹配 → 必须等 BLE 整轮扫描结束(实测 ≈10s)才回连,
 * 用户感受就是"进了搜索要等快 10 秒才自动连上"。现在逐条结果一进来就匹配:
 * 历史手柄的地址或名字一出现 → 立刻发起连接, 不用等扫完。
 * (目标是"最近连接过的"手柄, 地址/名字已知, 扫到即可连, 不需要更多结果。)
 * ★ R2: 走 bt_connect_from_scan() —— 它是扫描自身的消费者, 不等扫描收尾
 *   (扫描还在跑, 等它就是死等); 但它开始后 bt_core 会拒绝任何新扫描。 */

static bool rc_try_connect_now(const esp_hid_scan_result_t *rr)
{
    if (!rr || !s_auto_reconnect_pending || !s_rc_target_valid) return false;
    if (s_connected || s_suspended || s_user_disconnect) return false;
    if (!rc_match(rr->bda, rr->name)) return false;   /* 地址 或 名字 命中都算 */

    s_pending_new = !bt_manager_is_known_device(rr->bda, rr->name);
    snprintf(s_pending_name, sizeof s_pending_name, "%s", rr->name ? rr->name : "");
    ESP_LOGD(TAG, "[EV] 回连命中(边扫边连): name=%s addr=%02x:%02x 新手柄=%d",
             rr->name ? rr->name : "-", rr->bda[0], rr->bda[1], s_pending_new);
    /* 必须异步: esp_hidh_dev_open 会阻塞(设备不在时等满连接超时) */
    int rc = bt_connect_from_scan(rr);
    if (rc == BT_CONN_BUSY) return false;      /* 已有事务在飞: 保留 pending, 下一轮再试 */
    s_auto_reconnect_pending = false;          /* 已发起/已放弃: 结果由 on_connected 收敛 */
    s_rc_target_valid = false;
    return rc == BT_CONN_STARTED;
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
                if (rc_try_connect_now(rr)) {
                    return;   /* 已发起连接: 不再停扫/回列表 (连接成功由上层 0.5s 提示收敛) */
                }
                rr = rr->next;
            }
            ESP_LOGD(TAG, "[EV] 本轮扫描未命中回连目标 (剩 %dms 预算)",
                     (int)(s_rc_budget_until - esp_timer_get_time() / 1000));
            s_auto_reconnect_pending = false;   /* 交给预算机制, 不在此放弃 */
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
            if (rc_match(r->bda, r->name)) {
                s_pending_new = !bt_manager_is_known_device(r->bda, r->name);
                snprintf(s_pending_name, sizeof s_pending_name, "%s", r->name ? r->name : "");
                /* ★ R1: 连接事务状态由 bt_connect_async 统一置位, 这里不再预置
                 *   s_connecting (预置后遇到 BUSY 会留下"连接中"却没有任务的假状态)。
                 * ★ R2: 走"扫描自身命中"入口 —— 扫描已结束, 但独占闸门要到本回调
                 *   返回后才放开, 走普通入口会白等 (也等不到) 扫描收尾。 */
                if (bt_connect_from_scan(r) == BT_CONN_BUSY) {
                    break;   /* 已有事务在飞: 保留 pending, 结果由上一个事务收敛 */
                }
                s_auto_reconnect_pending = false;
                break;
            }
            r = r->next;
        }
        if (!s_connected && !s_connecting && !s_auto_reconnect_pending) {
            /* 没找到目标设备, 暂停自动重连, 避免反复扫描 */
            s_auto_reconnect_pending = false;
        }
        return;
    }
}

/* ★ 2026-11-23 R2: 扫描事务**彻底**退出 (射频独占已放开、任务计数已归零)。
 *   这里是"关蓝牙/拆栈被推迟"的唯一可靠执行信号 —— 只靠 on_scan_done 不够:
 *   那一刻扫描任务还在跑 (它还要调本回调之后的代码), 此时拆栈/关射频仍是 UAF 风险。 */
static void on_scan_idle_cb(void)
{
    if (s_release_pending && s_conn_task_alive <= 0 && !s_conn_task_exiting) {
        /* 交给回连任务统一执行释放 (与连接事务收尾走同一条路, 不另造生命周期)。
         * ★ R3: 回连任务没建成时同理 —— 自己执行, 否则这次释放永远没人做。 */
        if (s_reconnect_task) xTaskNotifyGive(s_reconnect_task);
        else                  bt_release_flush();
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

    /* ★ 2026-10-02 边扫边连: 扫到回连目标的**第一次**就发起连接, 不等整轮扫描结束。
     *   命中即 return —— 连接由 on_connected 收敛; UI 层(scan_dlg_poll)看到连上会
     *   自己停扫并关掉弹窗, 所以这里不需要也不应该去动扫描状态。 */
    if (rc_try_connect_now(r)) {
        ESP_LOGD(TAG, "[EV] 边扫边连命中, 提前发起连接(不再等整轮扫描)");
        return;
    }

    /* 查重: 已存在则仅补名字 (此前无名) / 更新 RSSI, 不重复推送 */
    for (int i = 0; i < s_dev_cache_count; i++) {
        if (memcmp(s_dev_cache[i].bd_addr, r->bda, 6) == 0) {
            s_dev_cache[i].rssi = r->rssi;
            if (!s_dev_cache[i].name[0]) {   /* 与身份口径一致: 以名字串为准 */
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
        .on_scan_idle   = on_scan_idle_cb,   /* ★ R2: 扫描事务彻底退出 → 执行被推迟的释放 */
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
/* 常驻自动回连任务 (低优先级, 省电直连上次手柄). 栈须够大: 回连会走 esp_hid/Bluedroid
 * 连接与错误处理, 2048 过小会在 HID open 失败/重连时爆栈重启 (实测 bt_rec stack overflow)。
 * ★ 2026-10-28: 补返回值检查 —— 原来直接忽略 xTaskCreate 的结果, 建任务失败时
 *   s_reconnect_task 为 NULL ⇒ bt_manager_wake_reconnect() 变成静默空操作,
 *   自动回连永久失效且日志里一个字都看不到。现在失败会打 E, 并允许下次 enable 重试。 */
static bool bt_ensure_reconnect_task(void)
{
    if (s_reconnect_task) return true;
    if (xTaskCreate(bt_reconnect_task, "bt_rec", 8192, NULL, 2, &s_reconnect_task) != pdPASS) {
        s_reconnect_task = NULL;
        ESP_LOGE(TAG, "bt_rec 任务创建失败(内部 RAM 不足?) ⇒ 自动回连不可用, 下次启用重试");
        return false;
    }
    return true;
}

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
    /* 修复: hcnt 是以 svc_nvs_set_blob(4字节) 保存的, 若用 svc_nvs_get_u8 读则类型不匹配,
     * 重启后永远读不到条数 → 连接记录为空、自动回连找不到设备. 改用 blob 读取. */
    int hcnt = 0;
    size_t hcnt_len = sizeof(hcnt);
    nvs_init_handle();
    if (s_nvs_open && svc_nvs_get_blob(s_nvs, NVS_KEY_HCNT, &hcnt, &hcnt_len) == ESP_OK) {
        s_history_count = hcnt;
        if (s_history_count < 0) s_history_count = 0;
        if (s_history_count > BT_HISTORY_MAX) s_history_count = BT_HISTORY_MAX;
    }

    /* 载入上次连接设备名 (判断新手柄用) */
    s_last_conn_name_valid = nvs_load_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));
    if (!s_last_conn_name_valid) s_last_conn_name[0] = '\0';

    /* ★ C2 (2026-10-02): 载入手柄档案表 (必须在 kmap/supmap/gbmap + lconn 之后:
     * 空表迁移需用 s_map/s_sup/s_gb 与上次连接名字)。 */
    gpm_load();

    nvs_init_handle();
    (void)core_ensure_init();

    (void)bt_ensure_reconnect_task();   /* 常驻回连任务: 失败打 E, 由 bt_manager_init 再试 */
}

/* 立即唤醒重连任务: 断连/需要即时重连时调用 (任务正按退避睡眠也能打断).
 * 无任务(未初始化)时静默忽略. */
void bt_manager_wake_reconnect(void)
{
    /* ★ 每次"被唤醒去重连"都给一轮新预算 (断连/需要即时重连时调用) */
    s_rc_retry_until = esp_timer_get_time() / 1000 + RC_BUDGET_MS;
    ESP_LOGI(TAG, "[EV] 唤醒回连: 新预算 %d ms", RC_BUDGET_MS);
    if (s_reconnect_task) xTaskNotifyGive(s_reconnect_task);
}

void bt_manager_init(void)
{
    ESP_LOGI(TAG, "bt_manager_init enter (bt_init task, 内部RAM栈)");
    bt_full_init();   /* 首次: 载入配置/映射/历史 + 核心 + 回连任务 (幂等) */
    /* 二次启用(disable→enable): bt_full_init 因幂等提前返回, 但核心已被
     * bt_core_deinit() 整栈拆掉(s_inited=false), 必须在这里重建 —— 本任务栈在
     * 内部 RAM, 是唯一安全位置; 放到调用者线程(如屏保监督任务的 PSRAM 栈)会
     * 触发 esp_task_stack_is_sane_cache_disabled() 断言 → 卡死重启. */
    if (s_full_init_done && !s_inited) (void)core_ensure_init();
    /* ★ 2026-10-28: 上次因内存不足没建成回连任务时, 在这里补建 (本任务栈在内部 RAM,
     *   是唯一安全位置; 建失败只会打 E, 不影响本次启用)。 */
    if (s_full_init_done && !s_reconnect_task) (void)bt_ensure_reconnect_task();
    s_init_task_running = false;
    if (s_init_done_sem) xSemaphoreGive(s_init_done_sem);   /* 通知 enable 等待方 */

    /* 本函数被 bt_manager_start_init_task 作为 "bt_init" 任务入口运行.
     * FreeRTOS 任务函数禁止返回, 完成初始化后必须删除自身 (否则 "Task should not return" 重启).
     * ★ 用 WithCaps 版本: 顺带释放 xTaskCreateWithCaps 分配的栈(内部 RAM)。 */
    vTaskDeleteWithCaps(NULL);
}

/* ========================================================================
 *  初始化任务栈：动态分配，且**必须内部 RAM**
 *  bt_manager_init 会调 esp_bt_controller_init/esp_hid_gap_init，期间 cache 被禁；
 *  PSRAM 栈会触发 esp_task_stack_is_sane_cache_disabled() 断言 ⇒ 不能放 PSRAM。
 *  ★ 2026-11-02 linit: 原来是 static 数组常驻 4096B，可 bt_init 初始化完就自删了，
 *    之后这 4KB 一直是死占用。改成 xTaskCreateWithCaps(MALLOC_CAP_INTERNAL)，
 *    任务删除时随栈一起归还堆 —— 峰值不变，常驻降为 0。
 * ======================================================================== */
#define BT_INIT_STACK_BYTES 4096     /* 字节(StackType_t=uint8_t) */

bool bt_manager_start_init_task(void)
{
    return xTaskCreateWithCaps((TaskFunction_t)bt_manager_init, "bt_init",
                               BT_INIT_STACK_BYTES, NULL, 1, NULL,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS;
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

/* ============================================================================
 * ★★ 2026-11-23 R2: 扫描入口统一闸门 ★★
 *   原来两个入口都是"先置 s_scanning=true, 再 bt_core_scan()" —— 而 bt_core_scan
 *   在闸门被占时**直接 return**, 于是管理器这边留下"正在扫描"却没有任何任务,
 *   UI 只能等 15s 看门狗自愈。现在: 只有 core 真的接下这次扫描才置位。
 * ==========================================================================*/
static void scan_start_common(void)
{
    /* ★★★ 2026-11-23 R3 真机修复 (点"添加设备"立即显示"未发现设备") ★★★
     *   规则: **用户主动扫描优先级最高**, 后台自动回连不得阻挡/抢占/拒绝。
     *
     *   原实现用 `s_conn_task_alive > 0 || s_conn_task_exiting` 一票否决 —— 但那个计数
     *   只表示"有连接任务对象存在", 不表示"射频正被 open 占着":
     *     · 开机回连窗口里回连任务在反复尝试(设备不在也照试);
     *     · 任务已建好还没进 open、或已经 open 完在收尾 —— 计数都还是 >0。
     *   于是用户点"添加设备"被直接拒绝, 页面立刻显示"未发现设备"(真机日志:
     *   `正在连接设备 ⇒ 暂不开始新扫描`), 而用户根本没发起过连接。
     *
     *   现在分两层, 判据全部来自 bt_core 的**真实事务状态**, 不再猜计数:
     *     ① 先让后台回连让路(停布防 + 让当前 open 尽快放弃), 不伪造空闲、不清计数;
     *     ② 是否真扫不开, 由 bt_core_scan() 的闸门裁决: 只有"open 真的在飞"
     *        (bt_core_open_busy) 才算冲突, 此时挂起扫描并在事务退出后自动补发;
     *        其它原因(另一个扫描在跑/建任务失败)如实报失败, 不假装在扫。 */
    if (s_auto_reconnect_pending || s_rc_target_valid ||
        s_conn_task_alive > 0 || s_conn_task_exiting) {
        /* 后台回连让路: 不再发起新事务; 已经发起的那个只"请它尽快收手",
         * 不清 s_connecting、不断链、不强拆(规则 3)。 */
        s_auto_reconnect_pending = false;
        s_rc_target_valid        = false;
        s_conn_giveup            = true;
    }

    s_scan_wait_conn = false;
    s_scan_start_ms = esp_timer_get_time() / 1000;   /* ★ 看门狗起点 */
    if (bt_core_scan(5)) {
        s_scanning = true;
        return;
    }

    /* ★★★ 2026-11-23 R3 真机修复 (一直正在扫描) ★★★
     *   页面每帧都会问"要不要开始扫描", 而上一帧刚起的那轮扫描通常还在跑(5s)。
     *   这里原来不分原因一律清回调并报失败, 于是:
     *     · 正在跑的那轮结果无人接收(on_scan_done 里 s_cont_cb 已被清空);
     *     · s_scanning=false, 页面自己的"正在扫描"状态再也收不到完成回调 ⇒ 永久卡在扫描中。
     *   (真机日志: 连续两条"连接事务已退出 ⇒ 补发挂起的扫描" → 第二条打印
     *    "扫描闸门忙(扫描=1) ⇒ 拒绝" + "扫描未启动", 随后 scan done: 8 results 被丢弃。)
     *   已有一轮**界面自己的**扫描在跑 = 正常状态, 保留回调、保持"扫描中", 等它出结果。 */
    if (bt_core_ui_scan_running()) {
        s_scanning = true;
        ESP_LOGD(TAG, "界面扫描已在进行 ⇒ 复用, 保留回调等它出结果");
        return;
    }
    /* 闸门被"回连的按名字短扫"占着: 它 5s 后自行释放, 且**不会**回调上层。
     * 不能假装在扫(会一直转圈), 也不能清回调(用户还在等)。挂起, 由
     * bt_manager_is_scanning() 每帧在闸门空出来后补发。 */
    if (bt_core_scan_busy()) {
        s_scan_wait_conn  = true;
        s_scanning        = true;
        s_scan_start_ms   = 0;          /* 挂起态不计看门狗 */
        ESP_LOGD(TAG, "回连短扫占用射频 ⇒ 用户扫描挂起, 待其结束立即补发");
        return;
    }

    if (bt_core_open_busy()) {
        /* 真的有连接事务卡在 esp_hidh_dev_open 里(射频被占) ⇒ 挂起, 等它返回再补发。
         * 保持 s_scanning=true: 页面显示"正在扫描", 而不是误报"未发现设备"。 */
        s_scan_wait_conn = true;
        s_scanning       = true;
        s_scan_start_ms  = 0;
        ESP_LOGW(TAG, "用户要扫描, 但连接事务正在 open ⇒ 挂起, 事务退出后自动开始");
        return;
    }

    /* 其余原因: 另一个扫描在跑 / 建任务失败 —— 不能假装在扫, UI 立刻能重试 */
    s_scan_cb  = NULL;
    s_cont_cb  = NULL;
    s_scanning = false;
    s_scan_start_ms = 0;
    ESP_LOGW(TAG, "扫描未启动 (已有扫描在跑/建任务失败)");
}

/* ★ 2026-11-23 R3: 补发被挂起的扫描。
 *   触发点: ① 连接任务真正退出后(bt_connect_task 末尾); ② 页面每帧轮询
 *   bt_manager_is_scanning() 时的兜底(见该函数)。判据用 bt_core_open_busy() —— 只有
 *   "真的还有 open 在飞"才继续等, 任务对象存在/计数不为零都不算。 */
static void bt_scan_resume_if_pending(void)
{
    if (!s_scan_wait_conn) return;
    if (bt_core_open_busy()) return;
    /* 射频还被别的扫描占着(回连短扫/上一轮任务收尾): 等它真正放开再补发,
     * 否则每帧都会白试一次(bt_core 会打"闸门忙")。 */
    if (bt_core_scan_busy()) return;
    if (!s_scan_cb && !s_cont_cb) {
        /* 页面已经退出/停了扫描: 不必补发 */
        s_scan_wait_conn = false;
        s_scanning = false;
        return;
    }
    s_scan_wait_conn = false;
    ESP_LOGI(TAG, "射频已空闲 ⇒ 补发挂起的扫描");
    scan_start_common();
}

void bt_manager_start_scan(bt_scan_callback_t callback)
{
    s_scan_cb = callback;
    s_cont_cb = NULL;
    scan_start_common();
}

void bt_manager_start_scan_continuous(bt_device_found_cb_t callback)
{
    s_cont_cb = callback;
    s_scan_cb = NULL;
    scan_start_common();
}

bool bt_manager_is_scanning(void)
{
    /* ★ R3 兜底: 挂起中的扫描(等 open 收尾) —— 页面每帧都会问 is_scanning, 这里顺便
     *   检查一次。没有"任务退出事件"可依赖时(比如计数残留/任务异常)也能自行恢复,
     *   不会出现"弹窗永远停在正在扫描"。判据仍是 bt_core 的真实 open 事务。 */
    if (s_scan_wait_conn && !bt_core_open_busy() && !bt_core_scan_busy()) {
        bt_scan_resume_if_pending();
    }
    /* ★★ 2026-10-18: "一直显示正在扫描"的自愈。
     *   bt_core_scan() 在扫描事务被占用时不会起任务, 于是一次被卡住的扫描之后,
     *   管理器这边 s_scanning=true 却永远不会收到 on_scan_done
     *   ⇒ UI 永远显示"正在扫描..."，只能重启。
     *   这里给硬上限：超过 15s（正常 5s 扫完 + 余量）就放弃本轮结果。
     * ★ R2: 只能清"我们想要结果"的意图, **不能**假装射频空闲 —— 扫描闸门由
     *   bt_core 的扫描任务自己释放 (它会超时收尾), 返回值仍以 bt_core_is_scanning()
     *   为准, 所以 UI 在任务真正退出前不会误判"可以重扫"。 */
    if (s_scanning && s_scan_start_ms &&
        (int64_t)(esp_timer_get_time() / 1000) - s_scan_start_ms > 15000) {
        ESP_LOGW(TAG, "扫描 15s 未收敛 ⇒ 放弃本轮结果 (core=%d)", (int)bt_core_is_scanning());
        s_scanning = false;
        bt_core_stop_scan();
        s_scan_start_ms = 0;
    }
    return s_scanning || bt_core_is_scanning();
}

void bt_manager_stop_scan(void)
{
    bt_core_stop_scan();
    s_scanning = false;
    s_scan_start_ms = 0;
    s_auto_reconnect_pending = false;
    s_scan_wait_conn = false;   /* ★ R3: 用户/页面主动停扫 ⇒ 取消"等事务退出后补发" */
    /* ★★ 2026-11-23 R2: 必须把回调也清掉 ★★
     *   原来只停扫描、留回调 —— 而回连任务/拆栈守卫都用 `s_scan_cb || s_cont_cb`
     *   判断"用户正在用蓝牙"。用户退出一次扫描弹窗后, 这两个指针**永远非空**:
     *     · "连败重建栈" 永远走 "正在扫描 ⇒ 稍后重试";
     *     · 开机窗口超时永远判成 "用户正在使用蓝牙, 保持开启" ⇒ 深拆栈永不执行,
     *       71KB 内部 RAM 再也还不回去。
     *   扫描已停 = 没有消费者了, 回调必须一起清。 */
    s_scan_cb = NULL;
    s_cont_cb = NULL;
    ESP_LOGD(TAG, "stop_scan: 已清扫描回调 (避免残留被当成'用户正在扫描')");
}

bool bt_manager_is_connected(void)
{
    /* ★ 2026-11-22: 排除键盘 —— 本函数全仓 25+ 处调用, 语义一直是"**有手柄**可用":
     *   input_scheme_touch() 用它决定触屏是否高亮某项、虚拟按键显隐、方向键归一化…
     *   键盘连着如果也算"有手柄", 那触屏会默认高亮、虚拟按键消失 —— 直接破坏
     *   "触屏/无手柄状态下默认不选中任何项"的既定规则。
     *   需要"任意设备连接"的地方用 bt_manager_any_connected()。 */
    return s_connected && !bt_kbd_is_keyboard();
}

bool bt_manager_any_connected(void)
{
    return s_connected;
}

bool bt_manager_is_keyboard_connected(void)
{
    return s_connected && bt_kbd_is_keyboard();
}

void bt_manager_set_kbd_sink(bt_manager_kbd_sink_t cb)
{
    s_kbd_sink = cb;
    if (cb) ESP_LOGI(TAG, "键盘输入 sink 已注册 (1:1 转发到输入层)");
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

    /* ★★ 2026-10-03 (linit: "我这手柄有两种唤醒方式, 有一种不会改变地址")
     *   **地址没变 = 就是上次那台已配对设备** → 绝不能走下面"清历史 + 清旧绑定"那条路径。
     *   那条路径是给"地址漂移"准备的: 清掉旧绑定虽然能让漂移模式重新配对干净,
     *   但它同时会 ①抹掉历史记录 ②打掉 known 标志 ③让本次连接退化成"新手柄"流程;
     *   而对"地址不变"的唤醒方式来说, **保住绑定才是快路径**
     *   —— 和开机回连同一条路(实测 5.1s AUTH SUCCESS)。
     *   所以这里提前返回, 只清输入残留。
     *   (实测手柄: "Q36 for Android", 两种唤醒方式分别是 静态地址 / 每会话漂移地址。) */
    for (int i = 0; i < s_history_count; i++) {
        if (memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0) {
            bt_map_calibrate_start();
            ESP_LOGI(TAG, "[clean] 地址未变 %02x:%02x:%02x:%02x:%02x:%02x [%s]"
                          " → 保留绑定与历史, 直接复用配对",
                     dev->bd_addr[0], dev->bd_addr[1], dev->bd_addr[2],
                     dev->bd_addr[3], dev->bd_addr[4], dev->bd_addr[5],
                     dev->name[0] ? dev->name : "?");
            return;
        }
    }

    /* 先判定"是否同一只手柄换了 BLE 地址"(同名但地址不同)。
     * 某些手柄(如 "Q36 for Android")每次会话地址都会漂移; 若一律按"新手柄"处理, 会
     * ①**每次连接都把用户自定义的按键映射重置回默认**(很烦);
     * ②旧绑定按老地址一直残留在 NVS, 造成配对状态不一致 → 手柄主动断开(HCI rsn=0x13)、
     *   随后拒绝连接(GATT 0x85)、**手柄自己的指示灯不显示已连接**。 */
    bool name_drift = false;
    for (int i = 0; i < s_history_count; i++) {
        bool same_addr = memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0;
        bool same_name = btm_same_name(dev->name, s_history[i].name);
        if (same_name && !same_addr) name_drift = true;
    }

    /* ★★ 2026-11-23 R2: 这里**不再**删"同名但地址不同"的旧记录 ★★
     *   原来在发起连接前就把旧记录删掉, 而记录现在只在**连成功之后**才写
     *   (见 on_connected_cb) —— 于是"地址漂移 + 这次又没连上"会直接丢掉用户的
     *   这条连接记录(下次连得上才补回来, 但这中间记录是空的)。
     *   去重本来就不需要它: bt_manager_add_to_history() 是**按名字去重**的,
     *   地址漂移的手柄连上后会顶替旧条目并移到最前, 永远只有一条。
     *   (BLE 绑定仍然保留 —— 两种唤醒方式各自的绑定都能复用, 原因见上面的长注释。) */
    /* 清除输入残留状态: 防"不按任何键映射就自动完成"的幽灵输入 */
    bt_map_calibrate_start();

    if (name_drift) {
        /* 同一只手柄(地址漂移): **保留**用户按键映射, 不清历史 (记录由连成功时统一去重) */
        ESP_LOGI(TAG, "[clean] 同名[%s]地址漂移: 保留按键映射与连接记录",
                 dev->name[0] ? dev->name : "?");
        return;
    }

    /* 真正的新手柄: 内存映射复位为默认, **不落盘** —— 落盘会经 gpm_store_cur 把它
     * 误标成"已做过映射", 阻止 C5 的映射引导 (见 C2 档案表)。归属关系由
     * on_connected_cb 按名字建档完成。 */
    bt_manager_reset_key_map();
    bt_manager_reset_sup_map();
    bt_manager_reset_gb_map();
    ESP_LOGI(TAG, "[clean] 已清理同名[%s]历史, 内存映射复位(不落盘)", dev->name[0] ? dev->name : "?");
}

/* V1.6.x: 异步连接执行体 —— **esp_hidh_dev_open() 是阻塞调用**(设备不在身边时会一直等到
 * BLE 连接建立超时, 实测 ~30s), 因此绝不能跑在 UI / 启动路径上, 否则界面直接冻住、
 * "正在连接…"的 8s 超时也形同虚设(代码里旧注释"esp_hidh_dev_open 异步, 安全"是错的)。
 * 统一丢到独立小任务里执行, 调用方立即返回, 状态由 on_connected 回调收敛。 */
typedef struct { esp_hid_scan_result_t r; uint32_t gen; } bt_conn_arg_t;

static void bt_connect_task(void *arg)
{
    bt_conn_arg_t *a = (bt_conn_arg_t *)arg;
    uint32_t gen = a ? a->gen : 0;
    bool ok = true;
    /* ★ 2026-10-28: 计数已由 bt_connect_async() 在创建任务**之前**占位, 这里只负责减。
     *   原来在本函数第一行 ++: xTaskCreate 返回后到本任务真正被调度之间有一个窗口,
     *   此窗口内 bt_conn_failed()/bt_reconnect_task 看到的 s_conn_task_alive 仍是 0
     *   ⇒ 误判"没有连接任务卡在 open 里" ⇒ 拆栈把刚起来的任务踩掉(UAF)。 */
    if (a) ok = bt_core_connect(&a->r);
    free(a);
    /* ★ 2026-11-23 R1: **先减计数再收尾** —— bt_core_connect 返回即代表本任务
     *   不再占用蓝牙栈; bt_conn_failed 只有看到 alive==0 才会清理链路/允许重建栈。
     *   (原来的顺序是先 failed 再 --, 导致所有失败都被判成"open 仍在阻塞"。)
     *   exiting 标志覆盖"alive 已归零但收尾还没跑完"这段窗口, 否则回连任务
     *   可能在这里抢先把栈拆掉, 下面这行 bt_conn_failed 就会踩到已释放的资源。 */
    taskENTER_CRITICAL(&s_conn_mux);
    s_conn_task_alive--;
    s_conn_task_exiting = true;
    taskEXIT_CRITICAL(&s_conn_mux);
    if (!ok) bt_conn_failed(gen, "异步 open 失败", true);
    taskENTER_CRITICAL(&s_conn_mux);
    s_conn_task_exiting = false;
    taskEXIT_CRITICAL(&s_conn_mux);
    /* 若期间有"关蓝牙/拆栈"被推迟, 事务退出即唤醒回连任务去执行真正的释放.
     * ★ R3: 回连任务没建成(内部 RAM 不足)时**没有人**会来执行这次释放 —— 那就自己收尾,
     *   否则 s_release_pending 永远挂着: 界面显示"已关"、射频仍在收发、71KB 不归还。
     *   此处条件是安全的: alive 已归零、exiting 已清、本任务不再访问蓝牙栈。 */
    if (s_release_pending && s_conn_task_alive <= 0) {
        if (s_reconnect_task) xTaskNotifyGive(s_reconnect_task);
        else                  bt_release_flush();
        /* 已经有"关蓝牙/拆栈"意图 ⇒ 不能反过来再起扫描 */
        s_scan_wait_conn = false;
    } else {
        /* ★ R3: 用户点"添加设备"时被本事务挡住的扫描, 这里补发 */
        bt_scan_resume_if_pending();
    }
    vTaskDelete(NULL);
}

/* 发起异步连接。
 * 返回 BT_CONN_STARTED = 已起任务; BT_CONN_BUSY = 已有事务在飞/扫描未收尾(未发起, 也不算失败);
 *      BT_CONN_ERR = 参数/内存/建任务失败。
 * ★ 2026-11-23 R1: **唯一连接入口 + 单飞闸门**。
 *   所有发起方(页面点选 / 边扫边连 / 开机回连窗口 / 名字重解析)都走这里。
 *   依据(IDF 6.0 源码): esp_hidh_dev_open 的 dev 对象要等它返回才从链表移除
 *   (esp_hidh.c: `if (esp_hidh_dev_get_by_bda(bda) != NULL) { "Already Connected" }`),
 *   并发第二次 open 必然失败, 且和"拆栈安全边界"直接冲突 ⇒ 一个时间窗只允许一个事务。
 * ★ 2026-11-23 R2: 与扫描事务互斥。
 *   from_scan=false (用户点选 / 回连任务) ⇒ 先等扫描事务真的退出 (最多
 *     BT_SCAN_DRAIN_MS), 等不到就返回 BUSY: 扫描正在用 GAP 状态机/射频时再 open
 *     会两边都失败, 也会让"扫描结果"和"连接对象"对不上。
 *   from_scan=true (扫描自己命中回连目标) ⇒ 本来就是扫描的消费者, 不等;
 *     之后 bt_core 会拒绝一切新扫描, 所以也不会出现"两个扫描 + 一个连接"。 */
#define BT_SCAN_DRAIN_MS  4000

static int bt_connect_async2(const esp_hid_scan_result_t *r, bool from_scan)
{
    if (!r) return BT_CONN_ERR;
    /* ★ 单飞闸门: 检查与占位必须在同一个临界区里, 否则并发调用者会同时通过。 */
    taskENTER_CRITICAL(&s_conn_mux);
    if (s_conn_task_alive > 0) {
        taskEXIT_CRITICAL(&s_conn_mux);
        ESP_LOGD(TAG, "已有连接事务在飞 ⇒ 拒绝重复 open");
        return BT_CONN_BUSY;
    }
    s_conn_task_alive++;             /* 先占位, 后续失败再回滚 */
    taskEXIT_CRITICAL(&s_conn_mux);

    /* 没有任务却自称"连接中" = 陈旧标记(历史路径残留), 先复位;
     * 但**从占位这一刻起就要算"连接中"** —— 下面可能要等扫描收尾几秒,
     * UI(conn_dlg_poll)只在 is_connecting() 为真时才会耐心等, 否则 1s 后判失败。 */
    s_connecting = false;
    s_connecting = true;
    /* 清掉上一轮的截止时间: 下面"等扫描收尾 + 建任务"这段不计入看门狗,
     * 真正的截止时间由 bt_conn_begin() 在拿到事务代号时设置。 */
    bt_conn_end();

    if (!from_scan && bt_core_scan_busy()) {
        /* 扫描事务还占着射频: 先请求停扫再等它退出 (正常情况下几十~几百 ms)。
         * 占位在此期间保留, 避免别的发起方挤进来插一脚。 */
        bt_core_stop_scan();
        ESP_LOGI(TAG, "连接前等待扫描收尾 (最多 %d ms)", BT_SCAN_DRAIN_MS);
        if (!bt_core_scan_wait_idle(BT_SCAN_DRAIN_MS)) {
            taskENTER_CRITICAL(&s_conn_mux);
            s_conn_task_alive--;
            taskEXIT_CRITICAL(&s_conn_mux);
            s_connecting = false;
            ESP_LOGW(TAG, "扫描 %d ms 内未收尾 ⇒ 本次连接暂不发起 (避免抢 GAP/射频)",
                     BT_SCAN_DRAIN_MS);
            return BT_CONN_BUSY;
        }
    }

    /* ★ R2: 新的连接意图 ⇒ 之前"用户主动断开"的意图失效 (否则这次连上也会被立刻关掉) */
    s_user_disconnect = false;

    bt_conn_arg_t *a = (bt_conn_arg_t *)malloc(sizeof(bt_conn_arg_t));
    if (!a) {
        taskENTER_CRITICAL(&s_conn_mux);
        s_conn_task_alive--;
        taskEXIT_CRITICAL(&s_conn_mux);
        s_connecting = false;
        return BT_CONN_ERR;
    }
    memset(a, 0, sizeof(*a));
    a->r = *r;   /* 浅拷贝: bt_core_connect 只用 bda/transport/ble.addr_type, 不用 name */
    a->gen = s_conn_gen + 1;
    /* ★ 记事务代号 + 目标 + 截止时间 (+ 地址类型, 连接成功后写连接记录要用) */
    bt_conn_begin(a->gen, r->bda, (uint8_t)r->ble.addr_type);
    if (xTaskCreate(bt_connect_task, "bt_conn", 4096, a, 2, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_conn_mux);
        s_conn_task_alive--;
        taskEXIT_CRITICAL(&s_conn_mux);
        free(a);
        s_connecting = false;
        s_conn_try_valid = false;
        bt_conn_end();
        return BT_CONN_ERR;
    }
    return BT_CONN_STARTED;
}

bool bt_manager_connect_device(const bt_device_t *dev)
{
    if (!dev) return false;

    /* ★★ 2026-11-23 R2: 这里**不再**写连接记录 ★★
     *   原来在发起连接前就 bt_manager_add_to_history(dev) —— 结果是"点一下没连上"
     *   也会新增/顶替一条记录, 而且 s_history[0] 立刻变成回连目标: 下次开机/回连
     *   会去抓一只其实从没连成功过的手柄。现在记录只在 **on_connected_cb 真正连上**
     *   之后写 (见那里的注释), 与"连接记录 = 连成功过的设备"语义一致。 */

    /* 在最近一次扫描结果里按地址匹配 (取一份副本: 链表可能被下一轮扫描释放) */
    esp_hid_scan_result_t hit;
    if (bt_core_scan_find(dev->bd_addr, &hit)) {
        s_pending_new = !bt_manager_is_known_device(dev->bd_addr, dev->name[0] ? dev->name : NULL);
        snprintf(s_pending_name, sizeof s_pending_name, "%s", dev->name);
        s_last_connect_was_new = s_pending_new;
        bool started = bt_connect_async(&hit) == BT_CONN_STARTED;
        if (started) s_conn_user_init = true;   /* ★ 2026-11-24: 用户点选发起的连接 */
        return started;
    }

    /* 扫描任务尚未完成 (s_scan_results 为空) 时, 实时列表里已点选的设备同样允许连接:
     * 用缓存中的地址/类型直接发起连接, 避免"扫到即点即连"在扫描结束前失败.
     * 连接层 esp_hidh_dev_open 会做真正的服务发现, 失败也由上层正常提示. */
    esp_hid_scan_result_t fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.bda, dev->bd_addr, 6);
    fake.transport = ESP_HID_TRANSPORT_BLE;
    fake.ble.addr_type = dev->addr_type;
    s_pending_new = !bt_manager_is_known_device(dev->bd_addr, dev->name[0] ? dev->name : NULL);
    snprintf(s_pending_name, sizeof s_pending_name, "%s", dev->name);
    s_last_connect_was_new = s_pending_new;
    bool started = bt_connect_async(&fake) == BT_CONN_STARTED;
    if (started) s_conn_user_init = true;       /* ★ 2026-11-24: 用户点选发起的连接 */
    return started;
}

void bt_manager_disconnect(void)
{
    /* ★★ 2026-11-23 R2: "主动断开"要能被后续事件尊重 ★★
     *   · 立意图标记: CLOSE 回调看到它就不唤醒自动回连 (否则 1~3s 后又自己连上);
     *   · 若此刻还有连接事务卡在 open 里 (R1 已确认取消无效), 由 on_connected_cb
     *     在 OPEN 成功那一刻关掉链路;
     *   · 清掉排队中的回连目标, 别让"断开"之后又冒出一个回连事务。 */
    s_user_disconnect = true;
    s_conn_giveup = true;          /* UI 不再等; 底层事务照旧收敛 */
    s_auto_reconnect_pending = false;
    s_rc_target_valid = false;
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

/* ★ 取走"待做按键映射"待办 (一次性) */
bool bt_manager_take_map_pending(char *out, size_t n)
{
    if (!s_map_pending) return false;
    s_map_pending = false;
    if (out && n) snprintf(out, n, "%s", s_map_pending_name);
    return true;
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

/* ★★★ 2026-11-23 R1: 语义改为「UI 停止等待」, 不再假装取消底层事务 ★★★
 *   原来这里会 bt_core_cancel_pending() → esp_ble_gap_disconnect()。读 IDF 6.0 源码:
 *   连接建立中 L2CAP 侧没有 LCB(l2cu_find_lcb_by_bd_addr 返回 NULL) ⇒ 直接 return FALSE,
 *   取消不生效; 而我们把 s_connecting 清掉、还发了一次断链请求 —— 结果是
 *     · 用户看到"连接超时/失败", 底层其实正在成功;
 *     · 状态被自己改成"没在连", 回连任务于是发起第二个 open ⇒ Already Connected。
 *   现在只做两件事: 标记"UI 不再等"(只影响提示) + 停掉自动回连排队。
 *   底层事务的生命周期完全由 open 的返回值 / OPEN 事件收敛。 */
void bt_manager_cancel_connect(void)
{
    s_conn_giveup = true;
    s_auto_reconnect_pending = false;
    /* ★ 2026-11-24: 用户已放弃等待(超时/退出连接窗) ⇒ 即使底层事务随后真的连上,
     *   也不再算"用户主动连接成功": 否则会在用户早已离开手柄页时挂出映射待办,
     *   下次随便连个手柄就把向导弹给一只不相关的手柄。后台连上只更新连接状态。 */
    s_conn_user_init = false;
    ESP_LOGI(TAG, "UI 放弃等待连接 (底层事务继续, 由 open 返回值收敛%s)",
             (s_conn_task_alive > 0) ? "" : "; 当前无在飞事务");
}

/* ===== 回连窗口布防 (★ 2026-11-23 linit B 档) =====
 * 只有两个时机允许"主动抓上次的手柄", 其余时间一律按省电策略走
 * (非窗口期: 断连只重试 RC_BUDGET_MS=14s, 用尽即关射频):
 *   ① 开机        → main.c 调 bt_manager_start_boot_reconnect(60000), 且仅限"配对过手柄"的机器
 *   ② 手动开蓝牙  → bt_manager_enable() 成功后自动布 BT_ENABLE_RECONNECT_WINDOW_MS(30s)
 * 窗口内: 每 BT_BOOT_RECONNECT_POLL_MS(3s) 低占空比直连上次手柄 (地址漂移时按名字重解析);
 *         连上即保持; 超时 → bt_reconnect_task 深拆栈还内存。
 * ⚠ 变量本体定义在文件前部 (s_history_count 附近), 那里注释说明原因。 */
/* 只布防, 不初始化协议栈 (调用者保证栈已就绪)。重复布防=延长截止时间, 不会叠加。 */
static void bt_boot_window_arm(uint32_t window_ms)
{
    if (window_ms == 0) window_ms = BT_BOOT_RECONNECT_WINDOW_MS;
    s_boot_win_active = true;
    s_boot_win_end_ms = (uint32_t)(esp_timer_get_time() / 1000) + window_ms;
    s_reconnect_delay_ms = BT_BOOT_RECONNECT_POLL_MS;   /* 立刻进 3s 节奏, 不必等 60s */
    ESP_LOGI(TAG, "回连窗口布防: %u ms (目标=上次手柄)", (unsigned)window_ms);
    bt_manager_wake_reconnect();
}

/* enable 成功后的自动布防。三种情况不布, 避免无谓重连/误拆:
 *   · 已经有了连接 (没必要再抓)   · 正在连接中 (别插队)
 *   · 没有任何配对历史 (开蓝牙可能是为了扫新手柄, 布防反而干扰扫描) */
static void bt_boot_window_arm_on_enable(void)
{
    if (s_connected || s_connecting) return;
    if (s_history_count <= 0) return;
    bt_boot_window_arm(BT_ENABLE_RECONNECT_WINDOW_MS);
}

static bool s_bt_on = false;   /* V1.6.x: 蓝牙是否已启用 (供键盘↔手柄模式切换记录原状态) */
/* s_user_off 已前移到文件前部 (s_history_count 之后) —— on_connected_cb 要先用到它。 */
bool bt_manager_is_enabled(void) { return s_bt_on; }   /* V1.6.x: 蓝牙当前是否启用 */

/* arm_window=false 用于"连败重建栈": 那次 enable 是失败恢复, 不是用户要用手柄。
 * ★ 2026-11-23 实测踩到的坑: 重建栈走 enable → 又自动布防 30s → 窗口被无限续命,
 *   60s 窗口永不判超时, 深拆栈永远不执行 (日志: t=72.5s 连败 2 次重建 → 又布防 30s)。 */
static void bt_manager_enable_ex(bool arm_window)
{
    s_bt_on    = true;     /* 先记"用户想开": 初始化要 1~2s, 这期间若直接息屏, 睡眠流程
                            * 仍会依此把蓝牙关掉; 失败路径会把它回滚成 false (见下)。 */
    s_power_off = false;   /* 显式开蓝牙: 允许自动回连 */
    s_suspended = false;
    /* ★ R2: 显式开蓝牙 = 用户又想要连接了 ⇒ 清掉之前"主动断开/挂起"的意图,
     *   否则回连任务照旧不唤醒、连上也会被立刻关掉。 */
    s_user_disconnect = false;
    /* ★ R1: 显式"开"优先于任何挂起的"关" —— 否则上一次关蓝牙因事务在飞被推迟,
     *   这里刚开起来, 连接任务一退出就被 bt_release_flush 又把栈拆了。 */
    s_release_pending = 0;
    /* 核心健在 → 无需任何初始化, 布防回连窗口后返回 (幂等快路径). */
    if (s_full_init_done && s_inited) {
        /* ★★★ 2026-11-23 R3 真机修复 (快捷栏/设置页开关蓝牙无效) ★★★
         *   这条快路径原来直接 return —— 但"核心健在"不等于"射频开着":
         *   上一次 bt_manager_disable() 是轻量关(bt_core_radio_off), 栈留着、射频关了;
         *   若某条路径只把 s_inited 留成 true 而射频是关的, 这里 return 后
         *   bt_core_is_ready() 仍是 false ⇒ UI 点"开"毫无反应, 再点"关"也没变化。
         *   现在: 只要射频没开就补一次 core_init (幂等分支内部会 radio_on +
         *   按需重建 esp_hidh), 保证"开"这个动作一定把射频开起来。 */
        if (!bt_core_is_ready()) {
            ESP_LOGI(TAG, "核心在但射频是关的 ⇒ 补开射频 (上次只关了 radio)");
            (void)bt_core_init(NULL);
            if (!bt_core_is_ready()) {
                s_bt_on = false;
                ESP_LOGE(TAG, "补开射频失败 ⇒ 蓝牙不可用");
                return;
            }
        }
        if (arm_window) bt_boot_window_arm_on_enable();   /* 手动开蓝牙: 也试 30s 抓上次手柄 */
        return;
    }

    /* 需要初始化 或 重建核心. 关键约束: esp_bt_controller_init / esp_hid_gap_init
     * 期间会禁用 Flash 与 PSRAM cache, 若在本调用者线程直接执行, 而该线程栈在
     * PSRAM(如屏保监督任务 sse 的 32KB PSRAM 静态栈), 会触发
     * esp_task_stack_is_sane_cache_disabled() 断言 → 卡死后重启.
     * V1.5.x 省电: 蓝牙按需开启 — 完整初始化 (映射/NVS/核心/回连任务) 统一由
     * 内部 RAM 静态栈的 bt_init 任务执行, 本函数只同步等待完成 (约 1~2 秒).
     * 首次初始化 与 "disable 后二次启用(bt_core_deinit 已拆核心)" 走同一条安全路径.
     *
     * ★ 2026-10-28: 失败路径必须把 s_bt_on 回滚成 false。原来置 true 后不管结果,
     *   "信号量创建失败/静态任务创建失败/20s 超时"都会留下 s_bt_on=true 而射频没起来
     *   ⇒ 设置页显示「开」、睡眠流程还会去 disable 一个不存在的射频。 */
    if (!s_init_done_sem) s_init_done_sem = xSemaphoreCreateBinary();
    if (!s_init_done_sem) {
        s_bt_on = false;
        ESP_LOGE(TAG, "初始化信号量创建失败(内部 RAM 不足) ⇒ 蓝牙不可用");
        return;
    }
    if (s_init_task_running) {   /* 已有初始化在跑: 等它, 不重复创建静态任务 */
        xSemaphoreTake(s_init_done_sem, pdMS_TO_TICKS(20000));
        if (!s_inited) s_bt_on = false;
        else if (arm_window) bt_boot_window_arm_on_enable();   /* 别人刚初始化完, 同样布防 */
        return;
    }
    xSemaphoreTake(s_init_done_sem, 0);   /* 丢弃上一轮残留信号, 避免假完成 */
    s_init_task_running = true;
    if (!bt_manager_start_init_task()) {
        s_init_task_running = false;
        s_bt_on = false;
        ESP_LOGE(TAG, "bt_init 任务创建失败 ⇒ 蓝牙不可用");
        return;
    }
    xSemaphoreTake(s_init_done_sem, pdMS_TO_TICKS(20000));
    if (!s_inited) {
        s_bt_on = false;
        ESP_LOGE(TAG, "蓝牙核心未就绪 (超时/失败), 下次 enable 重试");
    } else if (arm_window) {
        bt_boot_window_arm_on_enable();   /* ★ 手动开蓝牙: 30s 内主动抓上次手柄 */
    }
}

/* 公开入口: 正常启用都要布防回连窗口 (开机/main.c、设置页开关、os_hw 租约、游戏页)。 */
void bt_manager_enable(void) { bt_manager_enable_ex(true); }

/* 关蓝牙的公共实现.
 *   deep=false: 只关射频, 栈结构保留 (下次 enable 毫秒级复用). 设置页手动关 / 睡眠流程走这里.
 *   deep=true : 真拆栈, 归还 controller/Bluedroid/esp_hidh 占的内部 RAM.
 *               只在"最后一个使用者退出"(os_hw 引用计数归零)时用, 见 bt_manager_disable_release. */
/* 开机回连窗口的变量定义在 bt_manager_enable() 之前 —— 那里注释说明了原因。 */

static void bt_shutdown(bool deep)
{
    /* ★★ 2026-11-23 R2: 先请求停扫, 不管走哪个分支。
     *   扫描任务是"射频正在被使用"的唯一持有者; 先让它收到停止请求, 它会在几十~几百 ms
     *   内退出 (esp_hid_scan 有超时上限), 于是下面的延迟释放能很快落地。
     *   这一步放在最前面还有另一个作用: 睡眠流程若正好撞上扫描, 射频不会白扫满 5s
     *   而把电耗在"用户已经关屏"的时候。 */
    bt_core_stop_scan();

    /* ★ 2026-11-23 R3: 已经挂过"深拆栈"意图时, 紧随其后的这次轻量关不能把意图丢掉。
     *   典型调用: 快捷栏关闭 = os_hw_release()(→disable_release, 挂 pending=2)
     *   + bt_manager_disable()(→本函数, deep=false)。若不升级, 第二条会把
     *   s_release_pending 清成 0 而只关了射频 ⇒ 71KB 内部 RAM 永不归还。
     *   只在"事务都空闲"时升级: 有事务时仍走下面的挂起分支(意图保持=2)。 */
    if (!deep && s_release_pending == 2 &&
        s_conn_task_alive <= 0 && !s_conn_task_exiting && !bt_core_scan_busy()) {
        deep = true;
        ESP_LOGI(TAG, "本次关蓝牙合并之前挂起的深拆栈意图 ⇒ 直接深拆栈");
    }

    /* ★★★ 2026-11-23 R1: 延迟释放 (deep 与轻量关射频都适用) ★★★
     *   只要还有连接任务卡在 esp_hidh_dev_open 里, 就**不能**动底层:
     *   拆栈会释放 controller/Bluedroid/esp_hidh 的信号量与设备对象, 那个任务仍拿着
     *   它们继续跑 ⇒ UAF; 只关射频同样会把它正在用的链路抽掉。
     *   所以这里只收敛"上层状态 + 挂起释放意图", 真正的释放由 bt_release_flush()
     *   在 s_conn_task_alive 归零后执行 —— 一条路径, 不另造生命周期。
     * ★ R2 扩展: **扫描事务未收尾**同样要推迟 —— 扫描任务正卡在 esp_hid_scan 里等着
     *   Bluedroid 的完成事件, 拆掉栈之后它醒来就是已释放内存。它的退出信号由
     *   on_scan_idle 回调通知回连任务, 与连接事务共用同一条释放路径。 */
    /* ★★★ 2026-11-23 R3 真机修复 (快捷栏/设置页关不掉蓝牙) ★★★
     *   原条件把"扫描事务未收尾"也算进挂起 → 只要有一次扫描残留, 用户点"关"就只挂起
     *   意图并 return; 而挂起后的执行依赖回连任务/回调, 一旦它们不在就**永远关不掉**
     *   (界面显示关、射频还在跑, 再点开又因为状态不一致看着没反应)。
     *   区分两种关:
     *     · 轻量关射频(deep=false, 用户开关用): 扫描**不**挂起 —— bt_core_radio_off()
     *       内部会停扫并等 1.5s, 等不到也会关; 而且它只 disable 射频, 不删 esp_hid_gap
     *       的信号量/结果链表, 残余扫描任务不会 UAF(它会自行超时收尾)。
     *     · 深拆栈(deep=true): 必须严格等扫描退出, 否则任务醒来访问已释放资源。
     *   连接事务(open 在飞)两种情况都挂起 —— 那是真的不能中断。 */
    bool conn_busy = (s_conn_task_alive > 0 || s_conn_task_exiting);
    bool scan_busy = bt_core_scan_busy();
    if (conn_busy || (deep && scan_busy)) {
        s_bt_on = false;
        s_power_off = true;          /* 回连任务停手, 不再发起新事务 */
        /* 只升不降: 已经挂着"深拆栈"时, 后面再来一次"关射频"(如预算用尽分支)
         * 不能把释放等级降下去, 否则内存就永远还不回去了。 */
        if (deep) s_release_pending = 2;
        else if (s_release_pending == 0) s_release_pending = 1;
        s_connected = false;
        s_connecting = false;
        s_conn_giveup = false;
        s_scanning = false;
        s_scan_wait_conn = false;   /* 关蓝牙后不该再有"等射频补发扫描"的意图 */
        s_scan_start_ms = 0;
        s_suspended = false;
        s_conn_addr_valid = false;
        s_conn_info.available = false;
        s_need_stack_reset = false;
        s_auto_reconnect_pending = false;
        s_rc_target_valid = false;
        s_boot_win_active = false;
        s_boot_win_end_ms = 0;
        /* ★ R2: 一旦决定关蓝牙, 就不再需要扫描结果 —— 原意是摘掉回调, 免得残余扫描
         *   把结果推给已经退出的页面。但真机踩到反效果: 页面此刻正处在"扫描中",
         *   回调被摘掉后就永远收不到 on_scan_done ⇒ **一直显示正在扫描**。
         *   现在保留回调(结果由页面自己决定要不要用), 页面退出本身会调 stop_scan 摘掉它们。 */
        ESP_LOGW(TAG, "关蓝牙推迟: 连接事务/扫描尚未退出(alive=%d exiting=%d 扫描=%d) ⇒ 之后%s",
                 s_conn_task_alive, (int)s_conn_task_exiting,
                 (int)scan_busy, deep ? "深拆栈" : "关射频");
        return;
    }

    s_bt_on = false;
    s_power_off = true;    /* 彻底关蓝牙: 自动回连任务停手, 不再偷偷初始化 */
    s_release_pending = 0; /* R1: 已经走到真正释放这一步, 不再挂"延迟释放" */
    bt_core_disconnect();
    bt_core_stop_scan();
    s_connected = false;
    s_connecting = false;
    s_conn_giveup = false;      /* R1: 关蓝牙即结束本轮"放弃等待"标记 */
    s_scanning = false;
    s_scan_start_ms = 0;
    s_suspended = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
    /* ★★★ 2026-10-28 【"关了又自己开"的复现路径】★★★
     *   下面这些是**跨会话**的一次性状态, 原来一个都没清:
     *     s_need_stack_reset → bt_reconnect_task 下一轮(≤1.5s)看到它就走
     *                          `bt_manager_disable(); bt_manager_enable();` 重建栈,
     *                          而那个分支**不看 s_power_off** ⇒ 用户刚关掉的蓝牙又亮了。
     *     s_conn_fail_streak → 重新开启后继承连败计数, 第 2 次失败就直接拆栈。
     *     s_conn_try_valid / s_conn_deadline_ms → 残留的"进行中连接", 让看门狗拿旧目标收尾。
     *     s_auto_reconnect_pending / s_rc_target_valid → 重新开启后立刻去连旧目标。
     *   关蓝牙 = 本次会话结束, 这些必须归零; 其余运行态已在上面清过。 */
    s_need_stack_reset       = false;
    s_conn_fail_streak       = 0;
    s_conn_try_valid         = false;
    s_conn_deadline_ms       = 0;
    s_auto_reconnect_pending = false;
    s_rc_target_valid        = false;
    s_user_disconnect        = false;   /* ★ R2: 关蓝牙 = 会话结束, "主动断开"意图一并清 */
    s_scan_cb                = NULL;    /* ★ R2: 关蓝牙后没人该再收到扫描结果 */
    s_cont_cb                = NULL;
    s_scan_wait_conn         = false;
    s_inited = false;
    /* ★ 2026-11-23: 开机回连窗口也是跨会话状态 —— 不清会残留成"刚 enable 就判超时再拆栈" */
    s_boot_win_active        = false;
    s_boot_win_end_ms        = 0;
    if (!deep) {
        bt_core_deinit();          /* 只关射频: 快, 栈保留 */
        return;
    }
    /* ★ 2026-10-30 深度拆栈前的安全闸 (防"异步任务访问已释放资源"):
     *   ① s_power_off 已置 true → bt_rec 下一轮直接 continue, 不再碰 bt_core_*;
     *   ② 主动 notify 让它立刻从 ulTaskNotifyTake 醒来走一遍上面那条 continue;
     *   ③ 再等 80ms, 确保它已经从"可能在途"的一次 bt_core_connect/scan 调用里退出。
     *   扫描任务同理: bt_core_stop_scan() 已置标志, 这里再等它自然收尾。 */
    if (s_reconnect_task) {
        xTaskNotifyGive(s_reconnect_task);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    bt_core_deinit_deep();
    /* ★★★ 2026-11-23 R3: 拆栈可能被"扫描任务未退出"挡回 (bt_core_full_deinit 只短等 1s
     *   就 return, 为了保护正在 esp_hid_scan 里的任务)。而走到这里时 s_inited/s_bt_on 已经
     *   被清成"关"、s_release_pending 也被清 0 ⇒ 没有人会再来收尾, 结果是:
     *     界面显示"蓝牙已关"、实际射频还在跑、71KB 内部 RAM 永远不归还。
     *   检测"拆栈没做成"(扫描事务仍忙 或 射频仍开) ⇒ 重新挂起释放, 由扫描任务退出时的
     *   on_scan_idle 再触发一次 bt_release_flush。不新增机制, 只是把意图重新挂上。 */
    if (bt_core_scan_busy() || bt_core_is_ready()) {
        s_release_pending = 2;
        ESP_LOGW(TAG, "深拆栈未完成(扫描=%d 射频=%d) ⇒ 重新挂起释放, 等事务退出后重试",
                 (int)bt_core_scan_busy(), (int)bt_core_is_ready());
    }
}

void bt_manager_disable(void)
{
    bt_shutdown(false);
}

/* ★ 2026-10-30 linit「蓝牙没启用就不要占内存」: 最后使用者退出时真拆栈.
 * 由 os_hw 的引用计数归零路径调用 (bt_stop), 因此"多个模块同时用蓝牙"时
 * 只有最后一个 release 才会拆, 不会被先退出的那个误关。 */
void bt_manager_disable_release(void)
{
    bt_shutdown(true);
}

/* ============================================================================
 * ★ 2026-11-24 省电模式「蓝牙闲置自动关闭」的两个配套接口
 * ==========================================================================*/

/* 蓝牙"正被使用"的统一判据 (省电模式判定用; 只读, 不改任何状态)。
 * 逐条对应任务书"必须保护"的清单:
 *   · 已连接设备(含键盘)      → s_connected / bt_manager_any_connected()
 *   · 正在扫描 / 等待补发扫描 → s_scanning / s_scan_wait_conn / bt_core_scan_busy()
 *   · 正在配对/连接           → s_connecting / s_conn_task_alive / s_conn_task_exiting
 *   · 开机回连窗口 / 正在回连 → s_boot_win_active / s_auto_reconnect_pending / 预算未用完
 *   · 正在关/拆栈             → s_release_pending  (别在收尾时插一脚)
 * ⚠ 回连窗口是**上一轮刚修好的机制**: 这里把它整段算"忙", 于是闲置计时必然
 *   从"窗口结束之后"才开始, 不会出现"开机还在抓手柄就被判闲置关掉"。 */
bool bt_manager_is_busy(void)
{
    if (!s_bt_on) return false;
    if (s_connected || s_connecting || s_scanning || s_scan_wait_conn) return true;
    if (bt_manager_any_connected()) return true;
    if (s_boot_win_active) return true;
    if (s_auto_reconnect_pending) return true;
    if (s_rc_target_valid && rc_budget_left()) return true;
    if (s_conn_task_alive > 0 || s_conn_task_exiting) return true;
    if (s_release_pending) return true;
    if (bt_core_scan_busy()) return true;
    return false;
}

static volatile bool s_eco_release_running = false;
static void bt_eco_release_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "省电模式: 闲置关蓝牙 ⇒ 深拆栈(归还 controller/Bluedroid/esp_hidh)");
    bt_manager_disable_release();
    s_eco_release_running = false;
    vTaskDeleteWithCaps(NULL);
}

/* 省电模式专用入口: 异步深拆栈。
 * 为什么不直接让调用方调 bt_manager_disable_release(): 拆栈要经过
 * esp_bt_controller_deinit / esp_bluedroid_deinit, 期间会关 flash/PSRAM cache;
 * 而省电模式的判定点在**屏保监督任务**(栈在 PSRAM) —— 直接调会撞
 * esp_task_stack_is_sane_cache_disabled() 断言。这里自建**内部 RAM** 动态栈任务,
 * 与 bt_manager_start_init_task 的处理方式完全一致(那次是 init, 这次是 deinit)。
 * 返回 false = 已有一次在跑 / 建任务失败(内存不足), 调用方下次再试即可。 */
bool bt_manager_release_async(void)
{
    if (s_eco_release_running) return false;
    if (!s_bt_on) return false;
    s_eco_release_running = true;
    if (xTaskCreateWithCaps((TaskFunction_t)bt_eco_release_task, "bt_eco",
                            BT_INIT_STACK_BYTES, NULL, 2, NULL,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_eco_release_running = false;
        ESP_LOGE(TAG, "省电模式: 关蓝牙任务创建失败(内部 RAM 不足) ⇒ 下次再试");
        return false;
    }
    return true;
}

/* ★ 2026-11-23 R1: 执行被推迟的关蓝牙/拆栈。
 *   触发点: bt_shutdown() 发现还有连接任务卡在 esp_hidh_dev_open 里 (或扫描事务未收尾)
 *   ⇒ 只挂起意图; 连接任务退出 / 扫描任务退出 (on_scan_idle) 时 notify 回连任务,
 *   回连任务在循环顶部调用本函数。
 *   条件: 连接事务与扫描事务**都**退出 —— 只要还有一方在飞就继续等,
 *   宁可晚一点还内存, 也不在别人还在用的时候拆栈。 */
static void bt_release_flush(void)
{
    if (!s_release_pending) return;
    if (s_conn_task_alive > 0 || s_conn_task_exiting) {
        ESP_LOGD(TAG, "延迟释放等待中: 连接任务 alive=%d exiting=%d",
                 s_conn_task_alive, (int)s_conn_task_exiting);
        return;
    }
    if (bt_core_scan_busy()) {
        /* 扫描任务还在 esp_hid_scan 里: 它退出时会经 on_scan_idle 再唤醒本任务 */
        ESP_LOGD(TAG, "延迟释放等待中: 扫描事务未退出");
        return;
    }
    int mode = s_release_pending;
    s_release_pending = 0;
    ESP_LOGI(TAG, "延迟释放执行: 连接事务已退出 ⇒ %s", (mode == 2) ? "深拆栈还内存" : "关射频");
    if (mode == 2) bt_manager_disable_release();
    else           bt_manager_disable();
}

/* ========================================================================
 *  自动回连任务 (定义见文件头; 退避 60s→120s→240s→封顶300s)
 *  V1.5.x 省电: 回连扫描间隔 5s→60s, 大幅降低空闲期无效重试功耗.
 * ======================================================================== */
/* V1.5.x: 开机自动回连窗口 — 开机时若有已配对手柄, 短窗口内低占空比直连上次手柄,
 * 连上即保持; 窗口超时未连上 → 真拆栈回按需省电. 由 main.c 开机时启动.
 * (变量本体前置定义在 bt_shutdown 之前, 好让关蓝牙时能清掉它 —— 见那里的注释.) */

/* 直连最近一次手柄 (esp_hidh_dev_open 按已知地址, 无需扫描, 无线电占用极低) */
static bool bt_try_reconnect_now(void)
{
    if (s_history_count <= 0) return false;
    /* ★ 2026-11-23 R3: 用户正在扫描 / 列表页开着 ⇒ 后台回连必须让路。
     *   规则: 用户主动扫描期间, 自动回连不得抢射频、不得插连接、更不得反过来
     *   让扫描失败。这里直接返回 false(上层还会走"按名字短扫"分支, 那里同样有守卫)。 */
    if (s_scanning || s_scan_cb || s_cont_cb || bt_core_is_scanning()) {
        ESP_LOGD(TAG, "用户正在扫描 ⇒ 回连让路, 本轮不发起连接");
        return false;
    }
    const bt_device_t *d = &s_history[0];   /* s_history[0] = 最近连接 */

    /* ★★★ 2026-11-23 R3 真机修复 (快捷栏/设置页关不掉蓝牙 + 一直正在扫描) ★★★
     *   这里原来是"拿历史里的旧地址直接 open"。BLE 的规则是**外设必须在广播,
     *   主机才能连上** —— 手柄不在附近时 open 一定等满控制器超时才返回(实测 30.0s,
     *   日志 rsn=0x100 / OPEN failed 0x85)。这 30s 里:
     *     · bt_core 的 s_open_tx=1 ⇒ 扫描闸门被占;
     *     · s_conn_task_alive=1 ⇒ bt_shutdown() 只能挂起"关蓝牙"。
     *   于是用户点快捷栏"关蓝牙"没反应(射频还在), 点"添加设备"被挂起 ⇒ 一直正在扫描。
     *   而回连循环里"先短扫确认、再按当前地址连"的分支**永远走不到**: 因为本函数只要
     *   发起成功就返回 true, 上层不会再走那条分支。
     *   修法: 有名字的手柄一律交给上层那条"短扫确认存在 → 再连"的路径(见调用处);
     *   本函数只保留"没有名字、无法按名字确认"的兜底直连。这样手柄不在附近时
     *   每轮只花一次 5s 有界扫描, 不会再有 30s 的盲连接。 */
    if (d->name[0]) {
        ESP_LOGD(TAG, "回连: 有名字[%s] ⇒ 走短扫确认路径, 不盲目直连旧地址", d->name);
        return false;
    }

    esp_hid_scan_result_t fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.bda, d->bd_addr, 6);
    fake.transport = ESP_HID_TRANSPORT_BLE;
    fake.ble.addr_type = d->addr_type;
    /* ★ R2: 连上后写连接记录 / 查档案都要用名字, 先记下本次目标的名字与"是否新手柄"。
     *   不记的话, 若 esp_hidh_dev_name_get() 在 OPEN 那一刻返回空串, 会退回
     *   上一次连接残留的名字 ⇒ 记录串台、档案错套。 */
    s_pending_new = !bt_manager_is_known_device(d->bd_addr, d->name[0] ? d->name : NULL);
    snprintf(s_pending_name, sizeof s_pending_name, "%.31s", d->name);
    /* ★ 2026-11-23: 必须走**异步**连接 (与页面点选连接同一条路 bt_connect_async).
     *   原来这里直接调 bt_core_connect() → esp_hidh_dev_open() 在**本任务线程**里
     *   阻塞等 GATTC 收敛, 手柄不在身边时实测 30.0s 才返回。后果两条:
     *     ① 看门狗形同虚设 —— BT_CONNECT_TIMEOUT_MS 的检查在循环顶部,
     *        而本函数把循环卡住 30s, 超时根本没机会执行;
     *     ② 开机 60s 窗口里只够尝试 2 次 (实测 t=32.5s / 67.5s), 与"窗口内积极回连"相反,
     *        且这 60s 里射频几乎全程在尝试连接, 比"多轮短尝试"更耗电。
     *   改异步后: 任务立即返回, 看门狗按时收尾 → 窗口内每轮 ~7s, 可试 7~8 次。
     * ★ R1: 事务状态(代号/截止时间/s_connecting)已由 bt_connect_async 统一置位。 */
    int rc = bt_connect_async(&fake);
    if (rc == BT_CONN_BUSY) return true;   /* 已有事务在飞 = 本轮正在推进, 不算失败 */
    if (rc != BT_CONN_STARTED) {
        /* 建任务/内存失败: 这不是"手柄连不上", 不记连败, 只复位本次尝试的残留状态 */
        s_connecting    = false;
        s_conn_try_valid = false;
        bt_conn_end();
        ESP_LOGW(TAG, "回连: 无法发起连接事务 (内存/任务创建失败)");
        return false;
    }
    return true;   /* 已发起: 状态由 on_connected / 看门狗收敛 */
}

static void bt_reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* ★ R1: 先执行被推迟的关蓝牙/拆栈 (连接任务退出后会 notify 本任务)。
         *   放在循环最顶部: 不管后面因为 s_power_off / 窗口结束 / 预算用尽走哪个分支,
         *   挂起的释放都能第一时间落地。 */
        bt_release_flush();

        /* ★ 连接看门狗 (2026-10-18): esp_hidh_dev_open 内部是 portMAX_DELAY 无限等 GATTC 事件,
         *   连接没成时它可能一直不返回 ⇒ s_connecting 永远 true ⇒ 回连任务/UI 全被挡住
         *   = 用户说的"失败一次之后后面全失败"。
         * ★★ 2026-11-23 两段式 (证据见 BT_CONNECT_TIMEOUT_MS 注释):
         *   · 慢 (15s): 只打一次日志, 不动连接 —— 服务发现慢不等于连不上;
         *   · 硬超时 (25s): **UI 放弃等待**, 但**不取消**底层事务。
         *     为什么不再 cancel: 连接建立中 esp_ble_gap_disconnect() 在 L2CAP 侧找不到 LCB
         *     直接 return FALSE, 取消无效, 还会把 s_connecting 清掉 ⇒ 回连任务发起第二个 open
         *     ⇒ "Already Connected" ⇒ 误判失败 + 撞拆栈边界。真正能终止事务的只有 open 返回。 */
        if (s_connecting && s_conn_deadline_ms) {
            int64_t wnow    = esp_timer_get_time() / 1000;
            int64_t hard_ms = s_conn_deadline_ms + (BT_CONNECT_HARD_MS - BT_CONNECT_TIMEOUT_MS);
            if (wnow > hard_ms) {
                if (s_conn_task_alive > 0) {
                    /* 事务仍在飞: 只标记"UI 不再等", 保持 s_connecting 直到它真的返回,
                     * 期间单飞闸门挡住任何新事务 (不等 = 不误杀, 也不并发 open)。 */
                    if (!s_conn_giveup) {
                        s_conn_giveup = true;
                        ESP_LOGW(TAG, "连接事务超过 %d ms 未返回 ⇒ UI 放弃等待 (底层继续, 不取消)",
                                 BT_CONNECT_HARD_MS);
                    }
                } else {
                    bt_conn_failed(0, "硬超时(open 未收敛)", true);   /* 兜底: 没有任务却超时 */
                }
            } else if (wnow > s_conn_deadline_ms && !s_conn_slow_logged) {
                s_conn_slow_logged = true;   /* 只提示, 不动连接 */
                ESP_LOGW(TAG, "连接已超过 %d ms 仍在进行(服务发现较慢) ⇒ 继续等待, 不取消",
                         BT_CONNECT_TIMEOUT_MS);
            }
        }
        /* 连败达阈值 ⇒ 整栈重建。必须有任务还卡在 open 里就不做(会 UAF) */
        if (s_need_stack_reset) {
            if (s_power_off || s_suspended) {
                /* ★ 2026-10-28: 用户已主动关蓝牙/已挂起 ⇒ **不能**走下面的 disable+enable。
                 *   那段重建是"为了继续重连", 而此刻用户要的正是别连; 原来这里不看 s_power_off,
                 *   于是关蓝牙后 1.5s 内蓝牙自己被重新打开(与 bt_manager_disable 清标志互为双保险)。 */
                s_need_stack_reset = false;
                s_conn_fail_streak = 0;
            } else if (s_conn_task_alive > 0 || s_conn_task_exiting) {
                ESP_LOGW(TAG, "需重建蓝牙栈, 但连接任务尚未完全退出 ⇒ 稍后重试");
            } else if (s_scanning || bt_core_is_scanning() || s_scan_cb || s_cont_cb) {
                /* ★★ 不能在有扫描任务时拆栈：scan_task 正卡在 esp_hid_scan 里，
                 *   拆栈会让它拿着已释放的栈/dev 继续跑 —— 表现为"一直显示正在扫描"。 */
                ESP_LOGW(TAG, "需重建蓝牙栈, 但正在扫描 ⇒ 稍后重试");
            } else if (s_boot_win_active &&
                       (uint32_t)(esp_timer_get_time() / 1000) >= s_boot_win_end_ms) {
                /* ★ 2026-11-23: 连败 + 窗口已过期 ⇒ **不要**"重建继续连", 直接按超时真拆栈。
                 *   否则重建时 enable() 会把窗口又布防 30s ⇒ 60s 窗口永远判不到超时,
                 *   深拆栈永不执行 (实测日志: t=72.5s "连败 2 次 ⇒ 重建蓝牙栈" → 再布防 30000ms)。 */
                s_need_stack_reset = false;
                ESP_LOGI(TAG, "连败 %d 次且窗口已过期 ⇒ 真拆栈还内存 (按需开启)", s_conn_fail_streak);
                s_conn_fail_streak = 0;
                s_boot_win_active = false;
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                bt_manager_disable_release();
            } else {
                s_need_stack_reset = false;
                ESP_LOGW(TAG, "连败 %d 次 ⇒ 重建蓝牙栈", s_conn_fail_streak);
                s_conn_fail_streak = 0;
                bt_manager_disable();
                bt_manager_enable_ex(false);   /* ★ 失败恢复: 不布防, 否则窗口被续命 */
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
            }
        }
        /* 连接进行中/待重建时缩短等待, 让看门狗及时生效 */
        uint32_t wait_ms = (s_connecting || s_need_stack_reset) ? 1500 : s_reconnect_delay_ms;
        /* 可唤醒睡眠: 断连/开机时发通知立即醒来重连, 不必干等当前退避 (最长300s) 睡满 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        /* 条件: 已连接/正在连/在扫/被挂起/关电/用户已断开/无历史 → 都不自动回连 */
        if (s_connected || s_connecting || s_suspended || s_power_off || s_user_disconnect ||
            bt_manager_is_scanning() || s_history_count <= 0) {
            /* 恢复基础间隔: 只在"窗口已结束 / 已连上 / 关电"时做。
             * ⚠ 2026-11-23 修: 这里原来还包含 s_connecting —— 连接异步化之后,
             *   发起连接后 s_connecting 会保持 true 一段时间(直到回调/看门狗收敛),
             *   于是每一轮都把这个值冲回 60s; 4s 超时失败后窗口内就得干等 60s,
             *   60s 窗口只剩一次尝试(实测日志正是如此)。 */
            if (!s_boot_win_active || s_connected || s_power_off)
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;   /* 恢复基础间隔 */
            continue;
        }

        /* V1.5.x: 开机自动回连窗口 — 短窗口内低占空比(3s)直连上次手柄.
         * 连上即保持; 窗口超时仍未连上 → 关栈回按需省电 (开机场景, 手柄不在附近).
         * 若超时瞬间用户已进蓝牙页扫描/使用中 → 保持开启, 不误关. */
        if (s_boot_win_active) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms >= s_boot_win_end_ms) {
                /* 先分三种情况, 不能一律清窗口 —— 清了就再也没人来判超时了 */
                bool user_busy = s_scanning || bt_core_is_scanning() || s_scan_cb || s_cont_cb;
                bool conn_busy = (s_conn_task_alive > 0);
                if (user_busy) {
                    /* 用户正在用蓝牙(扫描/手柄页): 窗口正常结束, 但不拆栈 */
                    s_boot_win_active = false;
                    s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                    ESP_LOGI(TAG, "开机回连窗口超时: 用户正在使用蓝牙, 保持开启");
                } else if (conn_busy) {
                    /* ⚠ 2026-11-23: 连接异步化之后, 4s 看门狗判失败时 bt_connect_task
                     *   可能还卡在 esp_hidh_dev_open 里。此刻深拆栈 = 那个任务拿着已释放的
                     *   栈/dev 继续跑 (UAF) ⇒ **窗口保持 active**, 1.5s 后再来判一次。 */
                    s_reconnect_delay_ms = 1500;
                    ESP_LOGD(TAG, "窗口已到期待拆栈, 但连接任务未退出 ⇒ 稍后再判");
                } else {
                    /* ★ 2026-11-23 linit B 档: 开机窗口是"临时开一分钟", 超时必须**真拆栈**.
                     *   旧写法 bt_manager_disable() 只关射频、栈留着 ⇒ 71KB 内部堆
                     *   分配了永不归还, 这正是 2026-10-30 取消开机回连的原因。
                     *   改走 disable_release() → bt_shutdown(true) → bt_core_deinit_deep()。 */
                    s_boot_win_active = false;
                    s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                    ESP_LOGI(TAG, "开机回连窗口超时: 手柄不在附近 ⇒ 真拆栈还内存 (按需开启)");
                    bt_manager_disable_release();
                }
                continue;
            }
        }

        /* ★ 省电: 非开机窗口(使用中断连)只给一轮预算; 用尽就**关射频**等唤醒,
         *   不再按 60/120/240/300s 无限重试 (linit: "不要盲目的一直连接")。 */
        if (!s_boot_win_active &&
            (int64_t)(esp_timer_get_time() / 1000) > s_rc_retry_until) {
            ESP_LOGD(TAG, "[EV] 断连回连预算用尽 → 关射频省电 (等唤醒)");
            bt_manager_disable();
            continue;
        }

        if (!core_ensure_init()) continue;
        /* ★ 2026-11-23: 上一次异步 open 还没退出时不叠加新连接。
         *   连接异步化后, 4s 看门狗只是"判失败 + cancel_pending", 而 bt_connect_task
         *   本身可能还在 esp_hidh_dev_open 里; 同一 bda 并发 open 会撞
         *   "Already Connected"/INVALID_STATE, 也和"拆栈"条件 (s_conn_task_alive>0) 冲突。
         *   这里让回连任务等它退出 (每 1.5s 回来看一眼), 不阻塞、不叠加。 */
        if (s_conn_task_alive > 0) {
            s_reconnect_delay_ms = 1500;
            continue;
        }
        if (bt_try_reconnect_now()) {
            /* ★ 2026-11-23: 这里**不能**把 delay 拉回 60s, **也不能**关窗口 —— 异步连接
             *   只是"发起", 4s 后可能超时失败。窗口改由 on_connected_cb(真连上) 结束,
             *   节奏由上面窗口分支和失败分支维持。 */
            if (s_boot_win_active) s_reconnect_delay_ms = BT_BOOT_RECONNECT_POLL_MS;
            else                  s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
        } else {
            /* V1.6.x 关键修复: 某些手柄(如 "Q36 for Android")的 BLE 地址**每次会话都会漂移**
             * (同名不同址, 实测 `…9d:7e→:7f→:a0→:a1`) → 历史里存的旧地址直连必然失败, 自动
             * 回连对手柄永远无效。这里改按**名字**重解析它"当前"的地址再连一次。
             * ★ 2026-11-23: 改成"短扫只解析地址 + 异步连接"。原来调 bt_core_connect_by_name(),
             *   它内部同步 open → 手柄不在时 5s 扫描 + 30s 连接超时 = 一次 35s, 窗口内只够跑
             *   一轮半, 且回连任务被彻底卡住。现在扫到才连, 连不上由 4s 看门狗收尾。
             *   (短扫本身仍要 5s 且须独占蓝牙 ⇒ 先确认没有别的扫描在跑)
             * ★ R2: 再加两条守卫 —— 有连接事务在飞时短扫会被扫描闸门拒绝 (白扫),
             *   而用户自己开着扫描列表 (s_scan_cb/s_cont_cb) 时更不该插一轮短扫抢射频。 */
            if (s_history_count > 0 && s_history[0].name[0] &&
                !s_scanning && !bt_core_is_scanning() && !s_connecting &&
                s_conn_task_alive <= 0 && !s_scan_cb && !s_cont_cb) {
                esp_hid_scan_result_t hit;
                if (bt_core_scan_find_by_name(s_history[0].name, &hit)) {
                    ESP_LOGI(TAG, "按名字短扫命中[%s] ⇒ 异步连接", s_history[0].name);
                    if (bt_connect_async(&hit) == BT_CONN_STARTED) {
                        s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                        /* 窗口不在这里关: 由 on_connected_cb 在真连上时结束 */
                        continue;
                    }
                }
            }
            if (s_boot_win_active) {
                /* ★ 2026-11-23 分两段节奏 (简单固定阶段, 不做动态算法):
                 *   窗口前半 (剩余 >30s) → 3s 一轮; 后半 → 6s 一轮。
                 *   依据: 手柄通常在开机瞬间就在广播范围, 抓不到的多半一直不在,
                 *   末段拉长间隔可把射频活动减半, 又不影响"刚开机快速连上"的主诉求。
                 *   (每轮实际耗时 = 4s 连接超时 + 偶尔 5s 名字短扫, 所以这个值只是"轮间空隙") */
                uint32_t wnow = (uint32_t)(esp_timer_get_time() / 1000);
                uint32_t wleft = (wnow < s_boot_win_end_ms) ? (s_boot_win_end_ms - wnow) : 0;
                s_reconnect_delay_ms = (wleft > BT_BOOT_FAST_PHASE_MS)
                                     ? BT_BOOT_RECONNECT_POLL_MS
                                     : (BT_BOOT_RECONNECT_POLL_MS * 2);
            } else {
                /* 失败 → 指数退避, 封顶, 减少无效重试功耗 */
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                if (s_reconnect_delay_ms > BT_RECONNECT_MAX_MS) s_reconnect_delay_ms = BT_RECONNECT_MAX_MS;
            }
        }
    }
}

/* 轻量预检: NVS 里的配对手柄历史条数. **只读 NVS, 不碰蓝牙核心/协议栈**,
 * 因此可以在"还没初始化蓝牙"的时候安全调用 (bt_manager_start_boot_reconnect
 * 与 bt_manager_has_saved_pad 共用). */
static int bt_nvs_history_count(void)
{
    int hcnt = 0;
    size_t len = sizeof(hcnt);
    nvs_init_handle();
    if (s_nvs_open && svc_nvs_get_blob(s_nvs, NVS_KEY_HCNT, &hcnt, &len) == ESP_OK) {
        if (hcnt < 0) hcnt = 0;
        if (hcnt > BT_HISTORY_MAX) hcnt = BT_HISTORY_MAX;
    }
    return hcnt;
}

/* ★ 2026-10-30: "有没有配对过手柄" —— 给游戏/手柄入口判断要不要拉起蓝牙栈.
 *   true  → 值得为手柄初始化 (约 1~2s, 换 71KB 内部 RAM);
 *   false → 从没配对过, 一个字节都不占 (绝大多数只用物理键的用户走这条)。
 *   ⚠ 只读 NVS, 不初始化核心, 每次进游戏调一次 (~ms 级)。 */
bool bt_manager_has_saved_pad(void)
{
    return bt_nvs_history_count() > 0;
}

/* ★ 2026-11-24 (语义收窄): 这里原来的用途是"区分用户主动关蓝牙 vs 系统省电关"。
 *   现在**只有一个写入者**: 手柄页的「开机自动回连」开关 (经 bt_manager_set_boot_auto_reconnect)。
 *   设置页开/关蓝牙、连上手柄都**不再**改它 —— 那些是即时行为, 不是长期偏好。
 *   读 NVS 一个字节 (uoff, 沿用旧键免迁移), 轻量、不初始化协议栈。 */
bool bt_manager_is_user_off(void)
{
    uint8_t v = 0;
    size_t len = sizeof(v);
    nvs_init_handle();
    if (s_nvs_open && svc_nvs_get_blob(s_nvs, NVS_KEY_UOFF, &v, &len) == ESP_OK) {
        s_user_off = v ? 1 : 0;
    }
    return s_user_off != 0;
}

/* 底层写入口 (不建议业务层直接调, 用下面的语义化封装)。
 * off=true ⇒ 「开机自动回连」关; off=false ⇒ 开。 */
void bt_manager_set_user_off(bool off)
{
    s_user_off = off ? 1 : 0;
    nvs_init_handle();
    if (s_nvs_open) nvs_save_blob(NVS_KEY_UOFF, &s_user_off, sizeof(s_user_off));
    ESP_LOGI(TAG, "开机自动回连: %s", off ? "关 (下次开机不主动抓手柄)"
                                            : "开 (开机 60s 内抓上次手柄)");
}

/* ★ 2026-11-24: 「开机自动回连」开关 (手柄页可切换, 默认开)。
 *   与 uoff 是同一份持久数据 (uoff=1 ⇔ 开关关), 不新增 NVS 键:
 *   · 默认: NVS 没有该键 ⇒ s_user_off=0 ⇒ 开关显示"开" (符合"默认开启");
 *   · 老用户 (uoff=1): 开关显示"关", 但**现在看得见、点一下就能开**,
 *     不再是以前那种"藏在设置页蓝牙开关里、开机悄悄不回连"的隐形状态。 */
bool bt_manager_is_boot_auto_reconnect(void)
{
    return !bt_manager_is_user_off();
}

void bt_manager_set_boot_auto_reconnect(bool on)
{
    bt_manager_set_user_off(!on);
}

/* V1.5.x: 开机自动回连窗口 — 启动入口 (main.c 开机时调用).
 * 先轻量预检 NVS 历史 (不初始化蓝牙核心): 无已配对手柄 → 完全不拉起蓝牙栈 (省电).
 * 有历史 → 惰性完整初始化, 在 window_ms 窗口内低占空比直连上次手柄;
 * 窗口超时未连上 → bt_reconnect_task 自动 disable 回按需省电.
 * 已在连接中 → 直接跳过窗口. */
void bt_manager_start_boot_reconnect(uint32_t window_ms)
{
    /* 轻量预检: NVS 历史条数 (hcnt 是 svc_nvs_set_blob 保存的 4 字节, 用 blob 读) */
    int hcnt = bt_nvs_history_count();
    if (hcnt <= 0) {
        ESP_LOGI(TAG, "开机回连: 无已配对手柄历史, 蓝牙不启动 (按需开启)");
        return;
    }
    if (s_connected || s_connecting) {
        ESP_LOGI(TAG, "开机回连: 已连接/连接中, 跳过窗口");
        return;
    }
    /* ★ 2026-11-24: 用 arm_window=false —— 回连窗口改由**本函数**统一布一次。
     *   原来调 bt_manager_enable() (内部 arm_window=true): 它初始化完先布 30s,
     *   本函数随后又布 60s ⇒ 同一次开机布防两次 (两条"回连窗口布防"日志 + 两次
     *   唤醒回连任务), 后一次只是覆盖前一次的截止时间。现在只布一次,
     *   实际生效窗口仍是 60s, 行为等价、日志干净。 */
    bt_manager_enable_ex(false);
    if (!s_full_init_done) {
        ESP_LOGD(TAG, "开机回连: 完整初始化未完成, 窗口跳过");
        return;
    }
    if (window_ms == 0) window_ms = BT_BOOT_RECONNECT_WINDOW_MS;
    ESP_LOGI(TAG, "开机回连窗口启动: %u ms (异步直连, 不阻塞启动)", (unsigned)window_ms);
    bt_boot_window_arm(window_ms);   /* 统一布防: 标志 + 3s 节奏 + 唤醒回连任务 */
    /* ★ 关键修复(开机慢 30~60s 的根因): **不要在本函数里同步直连**。
     * bt_try_reconnect_now() → bt_core_connect() → esp_hidh_dev_open() 是**阻塞调用**,
     * 手柄不在身边时会一直等到 BLE 连接建立超时(实测 ~30s)才返回; 而本函数是在
     * main.c 的**启动路径**里被调用的 → 整个主循环被拖后 30s(实测:
     * `开机回连窗口启动` t=3.8s, `OS 主循环` t=33.9s), 用户感知就是"开机要 1 分钟"。
     * 改为只唤醒回连任务(bt_rec), 由它在自己的任务上下文里异步去连 —— 与官方
     * "启动路径不做阻塞式网络/蓝牙连接"的做法一致。
     * (唤醒动作已在 bt_boot_window_arm() 内部完成, 这里不再重复。) */
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
    /* ★ R2: 挂起 = "别连了"。和用户主动断开同一条路: 立意图 (CLOSE 回调不唤醒回连)、
     *   清排队目标、并让正在飞的连接事务在 OPEN 成功那一刻被立刻关掉。 */
    s_suspended = true;
    s_user_disconnect = true;
    s_auto_reconnect_pending = false;
    s_rc_target_valid = false;
    bt_core_stop_scan();
    s_scanning = false;
    s_scan_start_ms = 0;
    bt_core_disconnect();
    s_connected = false;
    s_connecting = false;
    s_conn_addr_valid = false;
    s_conn_info.available = false;
}

void bt_manager_resume(void)
{
    /* 核心被拆(disable 过) → 重建必须走内部 RAM 栈(见 bt_manager_enable 注释):
     * 本线程栈可能在 PSRAM, 直接重建 controller 会断言重启.
     * 核心健在 → 仅幂等刷新回调, 无 cache 禁用风险, 可在本线程直接调用. */
    if (!s_inited) bt_manager_enable();
    else (void)core_ensure_init();
    s_suspended = false;
    /* ★ R2: 恢复 = 允许再次连接; 挂起期间立的"不连"意图到此失效 */
    s_user_disconnect = false;
}

bool bt_manager_is_suspended(void)
{
    return s_suspended;
}