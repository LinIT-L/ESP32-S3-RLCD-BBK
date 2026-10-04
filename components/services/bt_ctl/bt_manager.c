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

#include "svc_nvs.h"
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
#define BT_RECONNECT_MAX_MS    300000
#define BT_BOOT_RECONNECT_POLL_MS 3000   /* V1.5.x: 开机回连窗口内直连间隔 (低占空比) */
#define BT_BOOT_RECONNECT_WINDOW_MS 30000 /* V1.5.x: 开机回连窗口默认时长 */
static uint32_t s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
static TaskHandle_t s_reconnect_task = NULL;   /* 断连/开机可即时唤醒重连任务 (退避睡眠中也能打断) */

/* 前置声明: 扫描回调(前文)与 connect_device(后文) 都要用; esp_hidh_dev_open 会阻塞, 必须异步调用 */
static bool bt_connect_async(const esp_hid_scan_result_t *r);

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

/* ========================================================================
 *  NVS 存取
 * ======================================================================== */
static nvs_handle_t s_nvs = 0;
static bool s_nvs_open = false;

static void nvs_init_handle(void)
{
    if (s_nvs_open) return;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "nvs namespace %s open failed", NVS_NS);
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
    if (!s_nvs_open) return;
    svc_nvs_set_blob(s_nvs, key, src, len);
    svc_nvs_commit(s_nvs);
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
        ESP_LOGW(TAG, "[EV] 回连预算用尽且未连上 → 关射频省电 (等唤醒)");
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
    ESP_LOGW(TAG, "[EV] 回连目标: name=\'%s\' addr=%02x:%02x:%02x:%02x:%02x:%02x (名字优先)",
             s_rc_target_name, s_rc_target_addr[0], s_rc_target_addr[1], s_rc_target_addr[2],
             s_rc_target_addr[3], s_rc_target_addr[4], s_rc_target_addr[5]);
    /* V1.5.x: 已有扫描在进行 (长按确认键扫描弹窗/添加设备) → 仅挂起自动回连标志,
     * 由扫描完成回调在结果里匹配回连目标连接, 不重复发起扫描 (避免 old-results 扫描风暴). */
    if (bt_manager_is_scanning()) return;

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
        ESP_LOGW(TAG, "[EV] 蓝牙栈未使能 → 交给 bt_rec 任务初始化后再回连 (避免 259)");
        bt_manager_wake_reconnect();
        return;
    }
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

static gpm_t s_gpm;

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
        ESP_LOGW(TAG, "[EV] 手柄档案表: 空表 (无存档或 magic 不符)");
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
            ESP_LOGW(TAG, "[EV] 手柄档案表: 迁移旧配置 → \'%s\'", e->name);
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
    ESP_LOGW(TAG, "[EV] 已忘记手柄 \'%s\' (历史+档案已删, 回连已停)", name);
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
    /* V1.2 性能: 原为 ESP_LOGI —— 按住某键时 HID 报告持续到来(≈100Hz),
     * 本循环会对"每个仍按住的键"重复打日志 → 高频刷屏并拖慢按键处理.
     * 降为 DEBUG (默认级别静默; 排查时 esp_log_level_set("BT", ESP_LOG_DEBUG)). */
    for (uint8_t i = 0; i < st->key_count; i++) {
        char ch = bt_kbd_hid_to_ascii(st->keys[i], st->modifiers);
        ESP_LOGD(TAG, "[kbd] press hid=0x%02x mods=0x%02x -> '%c'",
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
            ESP_LOGW(TAG, "[EV] 手柄档案表已满, \'%s\' 只能用默认映射", nm);
        }
        snprintf(s_last_conn_name, sizeof(s_last_conn_name), "%s", nm);
        s_last_conn_name_valid = true;
        nvs_save_blob(NVS_KEY_LNAME, s_last_conn_name, sizeof(s_last_conn_name));

        /* ★ C5 连接后自动映射: 该手柄**尚未做过按键映射** → 挂待办, 由 UI 层(任意页面)
         *   取走并引导 —— 这样**后台自动回连**上来的手柄也能被引导。
         *   注意: 这里**不落盘**(不调 save_*_map), 否则会立刻把档案标成 mapped。 */
        if (!prof_ready) {
            s_map_pending = true;
            snprintf(s_map_pending_name, sizeof s_map_pending_name, "%s", nm);
            ESP_LOGW(TAG, "[EV] 手柄 \'%s\' 无映射档案 → 待做按键映射 (等 UI 引导)", nm);
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

/* ★ 2026-10-02 边扫边连: 扫描**过程中**扫到回连目标的瞬间就发起连接。
 *
 * 原来只有 on_scan_done_cb() 里做匹配 → 必须等 BLE 整轮扫描结束(实测 ≈10s)才回连,
 * 用户感受就是"进了搜索要等快 10 秒才自动连上"。现在逐条结果一进来就匹配:
 * 历史手柄的地址或名字一出现 → 立刻 bt_connect_async(), 不用等扫完。
 * (目标是"最近连接过的"手柄, 地址/名字已知, 扫到即可连, 不需要更多结果。) */
static bool bt_connect_async(const esp_hid_scan_result_t *r);   /* 定义在后面 */

static bool rc_try_connect_now(const esp_hid_scan_result_t *rr)
{
    if (!rr || !s_auto_reconnect_pending || !s_rc_target_valid) return false;
    if (s_connected || s_suspended) return false;
    if (!rc_match(rr->bda, rr->name)) return false;   /* 地址 或 名字 命中都算 */

    s_pending_new = !bt_manager_is_known_device(rr->bda, rr->name);
    snprintf(s_pending_name, sizeof s_pending_name, "%s", rr->name ? rr->name : "");
    ESP_LOGW(TAG, "[EV] 回连命中(边扫边连): name=%s addr=%02x:%02x 新手柄=%d",
             rr->name ? rr->name : "-", rr->bda[0], rr->bda[1], s_pending_new);
    s_connecting = true;
    s_auto_reconnect_pending = false;   /* 已发起: 结果由 on_connected 收敛 */
    s_rc_target_valid = false;
    /* 必须异步: esp_hidh_dev_open 会阻塞(设备不在时等满连接超时) */
    bt_connect_async(rr);
    return true;
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
            ESP_LOGW(TAG, "[EV] 本轮扫描未命中回连目标 (剩 %dms 预算)",
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
                s_connecting = true;
                bt_connect_async(r);   /* 异步, 不在扫描回调里阻塞 */
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

    /* ★ 2026-10-02 边扫边连: 扫到回连目标的**第一次**就发起连接, 不等整轮扫描结束。
     *   命中即 return —— 连接由 on_connected 收敛; UI 层(scan_dlg_poll)看到连上会
     *   自己停扫并关掉弹窗, 所以这里不需要也不应该去动扫描状态。 */
    if (rc_try_connect_now(r)) {
        ESP_LOGW(TAG, "[EV] 边扫边连命中, 提前发起连接(不再等整轮扫描)");
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

    /* 常驻自动回连任务 (低优先级, 省电直连上次手柄). 栈须够大: 回连会走 esp_hid/Bluedroid
     * 连接与错误处理, 2048 过小会在 HID open 失败/重连时爆栈重启 (实测 bt_rec stack overflow). */
    xTaskCreate(bt_reconnect_task, "bt_rec", 8192, NULL, 2, &s_reconnect_task);
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
    s_init_task_running = false;
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

    /* 清理"同名但地址已漂移掉"的旧记录 —— **只去重历史, 不再删它的 BLE 绑定**。
     *
     * ★★ 2026-10-03 关键修正 (linit: "我这手柄有两种唤醒方式, 有一种不会改变地址"):
     *   ① `bt_manager_add_to_history()` 是**按名字去重**的 → 历史里 "Q36 for Android"
     *      永远只有 1 条, 地址会随唤醒方式在 A/B 之间**轮流顶替**;
     *   ② 于是"同名不同址 → 删绑定"这条规则会**把另一种唤醒方式的绑定删掉**:
     *        A 连过(有绑定A) → 切 B(删掉绑定A、历史变成B) → 再切回 A(A 已无绑定) = 冷配对。
     *      两种唤醒方式互相破坏, 每次都是"重新配对" → 8s 不够 → 用户看到"连接超时"。
     *   ③ 其实**不需要删绑定**: ESP32 侧本来就能存多个绑定, 两种地址各留一条正好各自复用;
     *      而"手柄换了新地址"这种情况, 我们本来就没有那个新地址的绑定 → 自然会走新配对,
     *      删旧绑定对"新地址配对"没有任何帮助。
     *   所以: 这里只把历史去重(保持列表干净), 绑定**留着**。
     *   要彻底清绑定请用「忘记此手柄」(bt_manager_forget_device)。 */
    for (int i = s_history_count - 1; i >= 0; i--) {
        bool same_addr = memcmp(s_history[i].bd_addr, dev->bd_addr, 6) == 0;
        bool same_name = btm_same_name(dev->name, s_history[i].name);
        if (same_addr) continue;                 /* 当前这条不动 */
        if (same_name) {
            ESP_LOGI(TAG, "[clean] 同名但地址不同: 去重历史 %02x:%02x:%02x:%02x:%02x:%02x"
                          " (绑定保留, 供另一种唤醒方式复用)",
                     s_history[i].bd_addr[0], s_history[i].bd_addr[1], s_history[i].bd_addr[2],
                     s_history[i].bd_addr[3], s_history[i].bd_addr[4], s_history[i].bd_addr[5]);
            bt_manager_remove_history_at(i);
        }
    }

    /* 清除输入残留状态: 防"不按任何键映射就自动完成"的幽灵输入 */
    bt_map_calibrate_start();

    if (name_drift) {
        /* 同一只手柄(地址漂移): **保留**用户按键映射, 只清旧绑定与旧历史 */
        ESP_LOGI(TAG, "[clean] 同名[%s]地址漂移: 保留按键映射, 仅清旧绑定/历史",
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
typedef struct { esp_hid_scan_result_t r; } bt_conn_arg_t;

static void bt_connect_task(void *arg)
{
    bt_conn_arg_t *a = (bt_conn_arg_t *)arg;
    if (a) {
        if (!bt_core_connect(&a->r)) {
            ESP_LOGW(TAG, "异步连接失败(超时/不在范围)");
            s_connecting = false;   /* 复位"连接中", 允许后续重试 */
        }
        free(a);
    }
    vTaskDelete(NULL);
}

/* 发起异步连接; 返回 false 仅表示"没能起任务"(不代表连接失败)。 */
static bool bt_connect_async(const esp_hid_scan_result_t *r)
{
    if (!r) return false;
    bt_conn_arg_t *a = (bt_conn_arg_t *)malloc(sizeof(bt_conn_arg_t));
    if (!a) return false;
    memset(a, 0, sizeof(*a));
    a->r = *r;   /* 浅拷贝: bt_core_connect 只用 bda/transport/ble.addr_type, 不用 name */
    if (xTaskCreate(bt_connect_task, "bt_conn", 4096, a, 2, NULL) != pdPASS) {
        free(a);
        return false;
    }
    return true;
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
            return bt_connect_async(r);
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
    s_pending_new = !bt_manager_is_known_device(dev->bd_addr, dev->name[0] ? dev->name : NULL);
    snprintf(s_pending_name, sizeof s_pending_name, "%s", dev->name);
    s_last_connect_was_new = s_pending_new;
    s_connecting = true;
    bt_manager_add_to_history(dev);
    return bt_connect_async(&fake);
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
    /* 核心健在 → 无需任何初始化, 直接返回 (幂等快路径). */
    if (s_full_init_done && s_inited) return;

    /* 需要初始化 或 重建核心. 关键约束: esp_bt_controller_init / esp_hid_gap_init
     * 期间会禁用 Flash 与 PSRAM cache, 若在本调用者线程直接执行, 而该线程栈在
     * PSRAM(如屏保监督任务 sse 的 32KB PSRAM 静态栈), 会触发
     * esp_task_stack_is_sane_cache_disabled() 断言 → 卡死后重启.
     * V1.5.x 省电: 蓝牙按需开启 — 完整初始化 (映射/NVS/核心/回连任务) 统一由
     * 内部 RAM 静态栈的 bt_init 任务执行, 本函数只同步等待完成 (约 1~2 秒).
     * 首次初始化 与 "disable 后二次启用(bt_core_deinit 已拆核心)" 走同一条安全路径. */
    if (!s_init_done_sem) s_init_done_sem = xSemaphoreCreateBinary();
    if (!s_init_done_sem) return;
    if (s_init_task_running) {   /* 已有初始化在跑: 等它, 不重复创建静态任务 */
        xSemaphoreTake(s_init_done_sem, pdMS_TO_TICKS(20000));
        return;
    }
    int words = 0;
    StackType_t *stack = bt_manager_get_init_stack(&words);
    static StaticTask_t bt_task_buf;
    xSemaphoreTake(s_init_done_sem, 0);   /* 丢弃上一轮残留信号, 避免假完成 */
    s_init_task_running = true;
    if (!stack || !xTaskCreateStatic((TaskFunction_t)bt_manager_init, "bt_init",
                                     words, NULL, 1, stack, &bt_task_buf)) {
        s_init_task_running = false;
        ESP_LOGE(TAG, "bt_init 任务创建失败, 蓝牙不可用");
        return;
    }
    xSemaphoreTake(s_init_done_sem, pdMS_TO_TICKS(20000));
    if (!s_inited) ESP_LOGW(TAG, "蓝牙核心未就绪 (超时/失败), 下次 enable 重试");
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

        /* ★ 省电: 非开机窗口(使用中断连)只给一轮预算; 用尽就**关射频**等唤醒,
         *   不再按 60/120/240/300s 无限重试 (linit: "不要盲目的一直连接")。 */
        if (!s_boot_win_active &&
            (int64_t)(esp_timer_get_time() / 1000) > s_rc_retry_until) {
            ESP_LOGW(TAG, "[EV] 断连回连预算用尽 → 关射频省电 (等唤醒)");
            bt_manager_disable();
            continue;
        }

        if (!core_ensure_init()) continue;
        if (bt_try_reconnect_now()) {
            s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;   /* 连上/连上中 → 回基础间隔 */
            s_boot_win_active = false;                     /* 连上即结束开机窗口 */
        } else {
            /* V1.6.x 关键修复: 某些手柄(如 "Q36 for Android")的 BLE 地址**每次会话都会漂移**
             * (同名不同址, 实测 `…9d:7e→:7f→:a0→:a1`) → 历史里存的旧地址直连必然失败, 自动
             * 回连对手柄永远无效。这里改按**名字**重解析它"当前"的地址再连一次。
             * (bt_core_connect_by_name 内部起一次 5s 阻塞短扫, 故先确保没有别的扫描在跑) */
            if (s_history_count > 0 && s_history[0].name[0] &&
                !s_scanning && !bt_core_is_scanning() && !s_connecting) {
                ESP_LOGI(TAG, "地址直连失败, 改按名字重解析[%s]", s_history[0].name);
                if (bt_core_connect_by_name(s_history[0].name)) {
                    s_connecting = true;
                    s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
                    s_boot_win_active = false;             /* 已发起连接 → 结束开机窗口 */
                    continue;
                }
            }
            if (s_boot_win_active) {
                s_reconnect_delay_ms = BT_BOOT_RECONNECT_POLL_MS;  /* 窗口内 3s 低占空比重试 */
            } else {
                /* 失败 → 指数退避, 封顶, 减少无效重试功耗 */
                s_reconnect_delay_ms = BT_RECONNECT_POLL_MS;
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
    /* 轻量预检: NVS 历史条数 (hcnt 是 svc_nvs_set_blob 保存的 4 字节, 用 blob 读) */
    int hcnt = 0;
    {
        size_t len = sizeof(hcnt);
        nvs_init_handle();
        if (s_nvs_open && svc_nvs_get_blob(s_nvs, NVS_KEY_HCNT, &hcnt, &len) == ESP_OK) {
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
    ESP_LOGI(TAG, "开机回连窗口启动: %u ms (异步直连, 不阻塞启动)", (unsigned)window_ms);
    /* ★ 关键修复(开机慢 30~60s 的根因): **不要在本函数里同步直连**。
     * bt_try_reconnect_now() → bt_core_connect() → esp_hidh_dev_open() 是**阻塞调用**,
     * 手柄不在身边时会一直等到 BLE 连接建立超时(实测 ~30s)才返回; 而本函数是在
     * main.c 的**启动路径**里被调用的 → 整个主循环被拖后 30s(实测:
     * `开机回连窗口启动` t=3.8s, `OS 主循环` t=33.9s), 用户感知就是"开机要 1 分钟"。
     * 改为只唤醒回连任务(bt_rec), 由它在自己的任务上下文里异步去连 —— 与官方
     * "启动路径不做阻塞式网络/蓝牙连接"的做法一致。 */
    bt_manager_wake_reconnect();
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
    /* 核心被拆(disable 过) → 重建必须走内部 RAM 栈(见 bt_manager_enable 注释):
     * 本线程栈可能在 PSRAM, 直接重建 controller 会断言重启.
     * 核心健在 → 仅幂等刷新回调, 无 cache 禁用风险, 可在本线程直接调用. */
    if (!s_inited) bt_manager_enable();
    else (void)core_ensure_init();
    s_suspended = false;
}

bool bt_manager_is_suspended(void)
{
    return s_suspended;
}