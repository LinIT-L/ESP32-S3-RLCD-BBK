/**
 * os_hw.c — 硬件服务懒加载调度器实现.
 * 引用计数管理: 首启调 start, 引用归零调 stop. 同服务同时一个状态.
 */
#include "os_hw.h"
#include "bt_manager.h"
#include "wifi_manager.h"
#include "usb_hid.h"
#include "usb_bt.h"      /* bt_start: USB-BT 适配器模式占用控制器时必须让出 */
#include "usb_net.h"     /* bt_start: USB 网卡模式同上 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "OSHW"

/* 前向: os_hw_request 要判"底层真实是否真开着" (定义在下面) */
bool os_hw_is_real_on(os_hw_id_t id);

typedef bool (*hw_start_t)(void);
typedef void (*hw_stop_t)(void);

typedef struct {
    const char *name;
    hw_start_t  start;
    hw_stop_t   stop;
} hw_svc_t;

/* ★★ 2026-10-14 linit: "关 WiFi 可以先退出软件、后台关" ——
 *   `wifi_manager_disable()` 里 `esp_wifi_stop()`+`esp_wifi_deinit()` 是**几百 ms 同步阻塞**;
 *   它跑在 os_pop() 的退出路径上 ⇒ 用户确认退出后界面要卡约 0.5s 才变。
 *   改成: 退页面对调(见 os_core.c os_pop 的 late_exit) 仍不够 —— 主循环还是被卡住
 *   (屏幕虽然切走了, 但这 0.5s 内按键/触摸全无响应)。所以直接**丢后台任务关**，
 *   主循环立刻继续跑。
 *   ⚠ 竞态防护: 后台关闭还在跑时若又要开 WiFi, `wifi_start()` 会先等它收尾 (见下),
 *     否则会出现"后台任务把刚开的 WiFi 又关掉"。
 *   ⚠ 任务栈必须用**内部 RAM** (xTaskCreate 默认就是) —— esp_wifi_stop 会碰 flash。 */
static TaskHandle_t s_wifi_off_task = NULL;

static void wifi_off_task(void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "WiFi 后台关闭开始");
    wifi_manager_disable();
    ESP_LOGD(TAG, "WiFi 后台关闭完成");
    s_wifi_off_task = NULL;
    vTaskDelete(NULL);   /* 一次性任务, 不返回 */
}

/* 各模块 start/stop 签名不统一 (void/bool/esp_err_t), 统一包装为调度器签名 */
static bool bt_start(void)
{
    /* ★ 2026-10-30 按需加载: USB-BT(蓝牙适配器) / USB 网卡 会独占蓝牙控制器,
     *   这时绝不能再去初始化手柄蓝牙 (原来开机路径 app_board_init_bt 也是这样让出的)。 */
    if (usb_bt_is_active() || usb_net_is_active()) {
        ESP_LOGW(TAG, "USB-BT/USB网卡占用控制器 ⇒ 不启动手柄蓝牙");
        return false;
    }
    bt_manager_enable();               /* 同步等初始化完成 (~1~2s, 由内部RAM栈的 bt_init 执行) */
    /* 返回**真实**结果: 失败时 os_hw_request 会回滚引用计数,
     * 否则会留下"计数=1 但射频没起来"的假开状态。
     * ★ 2026-11-23 R3: 必须同时要求 is_ready()(射频真的开着) —— 只看 is_enabled()
     *   (用户意图标记) 会把"初始化失败但标记还在"当成成功, 于是点关再点开都不动。 */
    return bt_manager_is_enabled() && bt_manager_is_ready();
}
/* ★ 2026-10-30: 引用计数归零时才走到这里 ⇒ 此时没有别的模块在用蓝牙,
 *   可以安全地**真拆栈**把 71KB 内部 RAM 还回去 (原来只关射频, 内存永不回收)。 */
static void bt_stop(void)       { bt_manager_disable_release(); }
static bool wifi_start(void)
{
    /* 后台关闭还没收尾 → 等它跑完再开 (通常几 ms~几百 ms), 避免"关了又被开/开了又被关"的竞态 */
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (s_wifi_off_task) {
        if ((uint32_t)(esp_timer_get_time() / 1000) - t0 > 3000) break;   /* 兜底 3s 不卡死 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return wifi_manager_enable();
}

/* 关 WiFi = 丢后台任务 (见 s_wifi_off_task 注释)。退出应用立刻可交互。 */
static void wifi_stop(void)
{
    if (s_wifi_off_task) return;                       /* 已有在途关闭任务 */
    if (xTaskCreate(wifi_off_task, "wifi_off", 4096, NULL, 4, &s_wifi_off_task) != pdPASS) {
        s_wifi_off_task = NULL;
        wifi_manager_disable();                        /* 建任务失败 → 退回同步关 (不会更糟) */
    }
}
static bool usb_hid_start_ok(void) { return usb_hid_start() == ESP_OK; }
static void usb_hid_stop_ok(void)  { usb_hid_stop(); }

static const hw_svc_t s_svcs[OS_HW_COUNT] = {
    { "bt",      bt_start,         bt_stop },
    { "wifi",    wifi_start,       wifi_stop },
    { "usb_hid", usb_hid_start_ok, usb_hid_stop_ok },
};

static int  s_ref[OS_HW_COUNT];
static bool s_active[OS_HW_COUNT];

bool os_hw_request(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return false;
    if (s_ref[id] > 0) {
        /* ★★★ 2026-11-23 R3 真机修复 (快捷栏/设置页点"开蓝牙"没反应) ★★★
         *   原来这里直接 ++ 就返回 —— 把"计数>0"当成了"设备真的开着"。但计数与
         *   真实射频会脱钩: 手动关过(禁用/延迟释放)、重建失败回滚、或射频被单独
         *   disable 而计数没归零, 都会留下 ref>0 但射频关着的状态。此时再点"开"
         *   只 ++ 不启动 ⇒ 界面永远不变, 表现为"开关坏了"。
         *   现在: 计数>0 但真实状态是关的 ⇒ 补一次真正的 start (幂等), 成功才计 ref。 */
        if (!os_hw_is_real_on(id)) {
            const hw_svc_t *svc = &s_svcs[id];
            ESP_LOGW(TAG, "%s 计数=%d 但底层未开 ⇒ 补启动", svc->name, s_ref[id]);
            s_active[id] = true;
            if (svc->start && !svc->start()) {
                s_active[id] = false;
                ESP_LOGW(TAG, "%s 补启动失败", svc->name);
                return false;
            }
        }
        s_ref[id]++;
        return true;
    }
    const hw_svc_t *svc = &s_svcs[id];
    s_active[id] = true;                /* 乐观: 先刷新显示(开), 操作在后台完成 */
    bool ok = svc->start ? svc->start() : true;
    if (!ok) {
        s_active[id] = false;           /* 失败回滚显示 */
        ESP_LOGD(TAG, "%s 启动失败", svc->name);
        return false;
    }
    s_ref[id] = 1;
    ESP_LOGI(TAG, "%s 启动 (ref=%d)", svc->name, s_ref[id]);
    return true;
}

void os_hw_release(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return;
    if (s_ref[id] <= 0) return;
    if (--s_ref[id] == 0) {
        const hw_svc_t *svc = &s_svcs[id];
        s_active[id] = false;           /* 先刷新显示(关), 清理在后台完成 */
        if (svc->stop) svc->stop();
        ESP_LOGI(TAG, "%s 停止 (ref=0)", svc->name);
    }
}

/* ★ 2026-11-04: 手动关闭 = 权威操作。把引用计数直接归零 (不调 stop, 调用方自己关),
 * 否则一处 request 漏了 release, 计数永远 >0 → os_hw_request 只 ++ 不启动 →
 * "蓝牙怎么点都开不起来" (实测: 手柄页每扫一次就 request 一次, 计数无上限累积)。 */
void os_hw_reset(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return;
    if (s_ref[id] == 0 && !s_active[id]) return;
    ESP_LOGW(TAG, "%s 引用计数强制归零 (ref %d→0, 手动关闭)", s_svcs[id].name, s_ref[id]);
    s_ref[id] = 0;
    s_active[id] = false;
}

bool os_hw_is_active(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return false;
    /* ★★★ 2026-11-07 linit「引用计数与真实射频状态必须一致」★★★
     *   开机 autostart 直接调 wifi_manager_enable()/bt_manager_enable(), **不走**
     *   os_hw_request ⇒ s_active 恒 false, 而射频其实在工作。真机实测到的
     *   `WIFI active=0 connected=1` 就是这个不一致(界面显示"关", 射频却在耗电)。
     *   现在 WiFi / BT 这两个"用户可以从设置页直接开"的设备以**真实状态**为准,
     *   其余设备仍只看引用计数。发布语义更准确, 且 os_hw_release 仍有 ref<=0 保护。 */
    switch (id) {
    case OS_HW_WIFI: return s_active[id] || wifi_manager_is_enabled();
    case OS_HW_BT:   return s_active[id] || bt_manager_is_enabled() || bt_manager_is_ready();
    default:         return s_active[id];
    }
}

/* 底层真实运行态: 用户可能在设置页直接开过 (没走管家, ref=0), 只看 s_active 会误判 */
bool os_hw_is_real_on(os_hw_id_t id) {
    switch (id) {
    /* ★ 2026-11-23 R3: BT 只认**射频真的开着**(bt_core 的 s_radio_on)。
     *   原来 `|| bt_manager_is_enabled()` 会把"用户意图开但射频没起来"的残留状态
     *   也判成"已开" ⇒ os_hw_request 只 ++ 不启动 ⇒ 点"开"毫无反应。 */
    case OS_HW_BT:   return bt_manager_is_ready();
    case OS_HW_WIFI: return wifi_manager_is_enabled() || s_active[OS_HW_WIFI];
    default:         return (id >= 0 && id < OS_HW_COUNT) ? s_active[id] : false;
    }
}

/* ==== 临时占用 (linit 2026-10-14) ====
 * "原本就开" → 不碰它 (不加计数, 退出也不关); "原本关闭" → 我们 request, 退出时 release。
 * 页面用 os_hw_lease_t 存位图, acquire 幂等。 */
void os_hw_lease_acquire(os_hw_lease_t *l, os_hw_id_t id) {
    if (!l || id < 0 || id >= OS_HW_COUNT) return;
    uint8_t bit = (uint8_t)(1u << (unsigned)id);
    if (l->held & bit) return;              /* 本次已持有 */
    if (os_hw_is_real_on(id)) return;       /* 用户原本就开着 → 退出时也不动 */
    if (os_hw_request(id)) l->held |= bit;  /* 由我们临时拉起, 退出时负责还回去 */
}

void os_hw_lease_release(os_hw_lease_t *l, os_hw_id_t id) {
    if (!l || id < 0 || id >= OS_HW_COUNT) return;
    uint8_t bit = (uint8_t)(1u << (unsigned)id);
    if (!(l->held & bit)) return;           /* 不是我们开的 → 不管 */
    l->held &= (uint8_t)~bit;
    os_hw_release(id);
}
