/**
 * os_hw.c — 硬件服务懒加载调度器实现.
 * 引用计数管理: 首启调 start, 引用归零调 stop. 同服务同时一个状态.
 */
#include "os_hw.h"
#include "bt_manager.h"
#include "wifi_manager.h"
#include "usb_hid.h"
#include "esp_log.h"

#define TAG "OSHW"

typedef bool (*hw_start_t)(void);
typedef void (*hw_stop_t)(void);

typedef struct {
    const char *name;
    hw_start_t  start;
    hw_stop_t   stop;
} hw_svc_t;

/* 各模块 start/stop 签名不统一 (void/bool/esp_err_t), 统一包装为调度器签名 */
static bool bt_start(void)      { bt_manager_enable(); return true; }
static void bt_stop(void)       { bt_manager_disable(); }
static bool wifi_start(void)    { return wifi_manager_enable(); }
static void wifi_stop(void)     { wifi_manager_disable(); }
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
    if (s_ref[id] > 0) { s_ref[id]++; return true; }   /* 已在用: 共享 */
    const hw_svc_t *svc = &s_svcs[id];
    bool ok = svc->start ? svc->start() : true;
    if (!ok) {
        ESP_LOGW(TAG, "%s 启动失败", svc->name);
        return false;
    }
    s_ref[id] = 1;
    s_active[id] = true;
    ESP_LOGI(TAG, "%s 启动 (ref=%d)", svc->name, s_ref[id]);
    return true;
}

void os_hw_release(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return;
    if (s_ref[id] <= 0) return;
    if (--s_ref[id] == 0) {
        const hw_svc_t *svc = &s_svcs[id];
        if (svc->stop) svc->stop();
        s_active[id] = false;
        ESP_LOGI(TAG, "%s 停止 (ref=0)", svc->name);
    }
}

bool os_hw_is_active(os_hw_id_t id) {
    if (id < 0 || id >= OS_HW_COUNT) return false;
    return s_active[id];
}
