/**
 * os_mgr.c — 系统资源管家 (性能优先).
 *
 * 设计决策 (性能 > 优雅 > 扩展性):
 *  - 不建独立管家任务: 由主循环每帧调用 os_mgr_poll(), 内部按
 *    CONFIG_OS_MGR_POLL_MS 节流. 零任务/零锁/零 IPC, UI 动作直接在
 *    主循环线程执行 (无跨线程竞态); 空闲帧仅两次整数比较, CPU 可忽略.
 *  - 编译期裁剪: 各子功能由 CONFIG_OS_MGR_{MEM,LIFE,POWER,HEALTH} 控制,
 *    未启用整段裁剪, 零代码零内存 (CONFIG_OS_MGR 关闭时全部空实现).
 *  - 无回调表/虚函数/插件机制: 决策用直接 if 分支.
 */
#include "os.h"
#include "os_mgr.h"
#include "os_hw.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <stdio.h>

#if CONFIG_OS_MGR_POWER
#include "board_battery.h"
#endif
#if CONFIG_OS_MGR_LIFE
#include "engine_manager.h"
#endif
#if CONFIG_OS_MGR_HEALTH
#include "nvs.h"
#include "nvs_flash.h"
#endif

#define TAG "OSMGR"

#if !CONFIG_OS_MGR

/* ==== 裁剪: 全部空实现, 零开销 ==== */
void os_mgr_init(void) {}
void os_mgr_poll(ui_ctx_t *ctx, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog)
{ (void)ctx; (void)now_ms; (void)last_input_ms; (void)in_dialog; }
void os_mgr_set_idle_timeout(uint32_t ms) { (void)ms; }
void os_mgr_set_low_mem_warn(uint32_t bytes) { (void)bytes; }
uint32_t os_mgr_mem_internal(void) { return 0; }
uint32_t os_mgr_mem_psram(void) { return 0; }
uint32_t os_mgr_mem_peak_used(void) { return 0; }
uint32_t os_mgr_crash_count(void) { return 0; }

#else /* CONFIG_OS_MGR */

/* ---- 参数 (运行时可调) ---- */
static uint32_t s_idle_timeout_ms = CONFIG_OS_MGR_IDLE_TIMEOUT_MS;
static uint32_t s_low_mem_bytes   = CONFIG_OS_MGR_LOW_MEM_BYTES;

/* ---- 节流 ---- */
#define MGR_PERIOD_MS  CONFIG_OS_MGR_POLL_MS
static uint32_t s_last_poll_ms = 0;

/* ---- 内存 ---- */
static uint32_t s_mem_internal;
static uint32_t s_mem_psram;
static uint32_t s_peak_used;
static uint32_t s_base_internal;
static bool     s_base_set;
static bool     s_warned_low;

#if CONFIG_OS_MGR_POWER
static uint32_t s_bat_last_ms;
static bool     s_bat_warned;
#endif

#if CONFIG_OS_MGR_HEALTH
static uint32_t s_crash_count;
#endif

void os_mgr_init(void)
{
#if CONFIG_OS_MGR_HEALTH
    nvs_handle_t h;
    if (nvs_open("os_mgr", NVS_READWRITE, &h) == ESP_OK) {
        esp_reset_reason_t r = esp_reset_reason();
        bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                         r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP);
        if (abnormal) {
            nvs_get_u32(h, "crash_count", &s_crash_count);
            s_crash_count++;
            nvs_set_u32(h, "crash_count", s_crash_count);
            nvs_commit(h);
            ESP_LOGW(TAG, "检测到异常复位 (reason=%d), 累计异常复位=%u",
                     (int)r, (unsigned)s_crash_count);
        }
        nvs_close(h);
    }
#endif
    ESP_LOGI(TAG, "os_mgr 就绪: poll=%dms 空闲超时=%ums 内存告警=%uB",
             (int)MGR_PERIOD_MS, (unsigned)s_idle_timeout_ms, (unsigned)s_low_mem_bytes);
}

/* ---- MEM: 内存采样 / 峰值 / 低水位告警 ---- */
static void mem_tick(ui_ctx_t *ctx)
{
    s_mem_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_mem_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!s_base_set) {
        s_base_set      = true;
        s_base_internal = s_mem_internal;
        s_peak_used     = 0;
        return;   /* 首拍仅建基线 */
    }
    uint32_t used = s_base_internal - s_mem_internal;
    if (used > s_peak_used) s_peak_used = used;

    if (s_low_mem_bytes && s_mem_internal < s_low_mem_bytes) {
        if (!s_warned_low) {
            s_warned_low = true;
            ESP_LOGW(TAG, "内部内存低: %u < %u, 触发引擎回收",
                     (unsigned)s_mem_internal, (unsigned)s_low_mem_bytes);
            /* 【改】系统级告警不再弹 toast(防止全屏引擎下点不掉/吞输入), 仅日志记录 */
#if CONFIG_OS_MGR_LIFE
            engine_manager_unload_all(false);   /* 低内存回收: 此刻空闲偏低, 跳过泄漏复核防误报 */
#endif
        }
    } else if (s_mem_internal >= s_low_mem_bytes + 8192) {
        s_warned_low = false;   /* 恢复阈值 +8KB 防抖 */
    }
}

/* ---- LIFE: 空闲超时自动回主菜单 + 引擎残留兜底 ---- */
static bool s_was_on_main = false;
static void life_tick(ui_ctx_t *ctx, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog)
{
    bool on_main = os_is_main(ctx);

    /* 空闲超时: 非主菜单 / 无弹窗 / 非全屏应用(游戏不打断) */
    if (s_idle_timeout_ms > 0 && last_input_ms != 0 && !in_dialog && !on_main && !os_screensaver_active()) {
        const os_module_t *m = ctx->cur;
        bool busy = m && m->fullscreen;
        if (!busy && now_ms - last_input_ms >= s_idle_timeout_ms) {
            ESP_LOGI(TAG, "空闲 %ums 自动返回主菜单",
                     (unsigned)(now_ms - last_input_ms));
            os_pop_to_main(ctx);
        }
    }

    /* 返回主菜单 → 管家及时清理引擎占用 (状态翻转即触发, 不等 60s 兜底) */
    if (on_main && !in_dialog && !s_was_on_main) {
        ESP_LOGI(TAG, "返回主菜单: 及时清理引擎占用");
        engine_manager_unload_all(true);   /* 正常回主菜单: 做泄漏复核 */
    }
    /* 引擎残留兜底: 主菜单无弹窗时每 60s 回收一次 (幂等) */
    static uint32_t s_eng_last_ms = 0;
    if (on_main && !in_dialog && now_ms - s_eng_last_ms >= 60000) {
        s_eng_last_ms = now_ms;
        engine_manager_unload_all(false);   /* 周期兜底: 重复卸载, 跳过泄漏复核 */
    }
    s_was_on_main = on_main;
}

/* ---- POWER: 低电量告警 (30s 采样, 不强制关外设) ---- */
#if CONFIG_OS_MGR_POWER
static void power_tick(ui_ctx_t *ctx, uint32_t now_ms)
{
    if (now_ms - s_bat_last_ms < 30000) return;
    s_bat_last_ms = now_ms;
    board_battery_status_t bat;
    if (board_battery_read(&bat) != ESP_OK) return;
    if (bat.percent != 255 && bat.percent < 15) {
        if (!s_bat_warned) {
            s_bat_warned = true;
            /* 【改】低电量告警不弹 toast(用户要求"不要显示电量低", 且防止全屏引擎下点不掉), 仅日志 */
            ESP_LOGW(TAG, "电量低: %d%%, 建议尽快充电", (int)bat.percent);
        }
    } else if (bat.percent >= 20) {
        s_bat_warned = false;
    }
}
#endif /* CONFIG_OS_MGR_POWER */

void os_mgr_poll(ui_ctx_t *ctx, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog)
{
    if (now_ms - s_last_poll_ms < MGR_PERIOD_MS) return;   /* 节流 */
    s_last_poll_ms = now_ms;
#if CONFIG_OS_MGR_MEM
    mem_tick(ctx);
#endif
#if CONFIG_OS_MGR_LIFE
    life_tick(ctx, now_ms, last_input_ms, in_dialog);
#endif
#if CONFIG_OS_MGR_POWER
    power_tick(ctx, now_ms);
#endif
}

void os_mgr_set_idle_timeout(uint32_t ms) { s_idle_timeout_ms = ms; }
void os_mgr_set_low_mem_warn(uint32_t bytes) { s_low_mem_bytes = bytes; s_warned_low = false; }

uint32_t os_mgr_mem_internal(void) { return s_mem_internal; }
uint32_t os_mgr_mem_psram(void)    { return s_mem_psram; }
uint32_t os_mgr_mem_peak_used(void){ return s_peak_used; }
uint32_t os_mgr_crash_count(void)  { return s_crash_count; }

#endif /* CONFIG_OS_MGR */
