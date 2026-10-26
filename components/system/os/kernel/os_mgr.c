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

#include "svc_nvs.h"
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
    if (svc_nvs_open("os_mgr", NVS_READWRITE, &h) == ESP_OK) {
        esp_reset_reason_t r = esp_reset_reason();
        bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                         r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP);
        if (abnormal) {
            svc_nvs_get_u32(h, "crash_count", &s_crash_count);
            s_crash_count++;
            svc_nvs_set_u32(h, "crash_count", s_crash_count);
            svc_nvs_commit(h);
            ESP_LOGD(TAG, "检测到异常复位 (reason=%d), 累计异常复位=%u",
                     (int)r, (unsigned)s_crash_count);
        }
        svc_nvs_close(h);
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
            ESP_LOGD(TAG, "内部内存低: %u < %u, 触发引擎回收",
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
static void orphan_task_tick(bool on_main);   /* 引擎任务残留看门狗 (定义在下面) */
static void life_tick(ui_ctx_t *ctx, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog)
{
    bool on_main = os_is_main(ctx);

    /* 空闲超时: 非主菜单 / 无弹窗 / 非全屏应用(游戏不打断) */
    if (s_idle_timeout_ms > 0 && last_input_ms != 0 && !in_dialog && !on_main && !os_screensaver_active()) {
        const os_module_t *m = ctx->cur;
        /* busy = 全屏应用(游戏) 或 标记了 keep_on_idle 的工具页(收藏夹等) */
        bool busy = m && (m->fullscreen || m->keep_on_idle);
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
    orphan_task_tick(on_main);
}

/* ★★★ 2026-10-25 linit「管家负责这些 / 其他模块有没有类似问题」的审计结论:
 *   各引擎的 *_stop() 都只是**置标志**, 靠任务自己跑到循环顶部退出 (vTaskDelete(NULL));
 *   一旦任务卡在事件等待/音频/加载里, 标志永远没人看 ⇒ 任务留在后台继续占 CPU/内存。
 *   engine_manager_unload_all() 只回收**缓冲区**, 不删任务 ⇒ 拦不住这种残留。
 *   这里做**任务名**级兜底: 回主菜单后连续 3 次仍按名字查到引擎任务, 就强删。
 *   (xTaskGetHandle 依赖 FreeRTOS 任务名; 名字表与各引擎 xTaskCreate 的名字保持一致) */
static int s_orphan_hits = 0;
static void orphan_task_tick(bool on_main)
{
    static const char *const names[] = {
        "sdlpal", "sdl_aud", "gbc_emu", "nes_emu", "md_emu", "sms_emu",
        "arduboy_avr", "vpet_emu", "gam4980",
    };
    if (!on_main) { s_orphan_hits = 0; return; }
    TaskHandle_t h = NULL; const char *nm = NULL;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t) { h = t; nm = names[i]; break; }
    }
    if (!h) { s_orphan_hits = 0; return; }
    if (++s_orphan_hits < 3) return;      /* 连续 3 个周期(≈3s)才判残留, 避开正常收尾 */
    ESP_LOGW(TAG, "引擎任务残留: %s 仍在跑 ⇒ 强制结束 (内部=%u PSRAM=%u)",
             nm, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    vTaskDelete(h);
    s_orphan_hits = 0;
}

/* ---- POWER: 低电量告警 (60s 节拍, 与状态栏共用 board_battery 缓存采样; 不强制关外设) ---- */
#if CONFIG_OS_MGR_POWER
static void power_tick(ui_ctx_t *ctx, uint32_t now_ms)
{
    if (now_ms - s_bat_last_ms < 60000) return;
    s_bat_last_ms = now_ms;
    board_battery_status_t bat;
    if (board_battery_read_cached(&bat) != ESP_OK) return;
    if (bat.percent != 255 && bat.percent < 15) {
        if (!s_bat_warned) {
            s_bat_warned = true;
            /* 【改】低电量告警不弹 toast(用户要求"不要显示电量低", 且防止全屏引擎下点不掉), 仅日志 */
            ESP_LOGD(TAG, "电量低: %d%%, 建议尽快充电", (int)bat.percent);
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
