/* engine_manager.c - 统一游戏模拟器引擎生命周期管理 */
#include "engine_manager.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "ENGINE"

/* === 基线内存泄漏检测 ===
 * 记录"全部引擎已卸载"时的干净基线 (内部 RAM / PSRAM 空闲大小).
 * 每次 unload_all 后与基线对比: 若空闲内存明显小于基线, 说明有引擎没有完全卸载
 * (堆内存泄漏), 打印警告便于定位. 基线取历史最小值, 避免脏基线与碎片累计误报.
 * 阈值: 内部 RAM 泄漏 >32KB 或 PSRAM 泄漏 >256KB 视为异常 (各引擎核心/缓冲规模量级). */
#define TAG_MEM_BASE_INTERNAL_MIN  (32 * 1024)
#define TAG_MEM_BASE_PSRAM_MIN     (256 * 1024)
static uint32_t s_base_internal = 0;
static uint32_t s_base_psram = 0;
static bool     s_base_valid = false;

static void engine_mem_baseline_update(void)
{
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* 取历史最小值作为"最干净"基线 (开机后各任务延迟初始化逐渐释放后趋于稳定) */
    if (!s_base_valid || free_int < s_base_internal)
        s_base_internal = free_int;
    if (!s_base_valid || free_ps < s_base_psram)
        s_base_psram = free_ps;
    s_base_valid = true;
}

/* 返回主菜单时调用: 复核当前内存相对基线的释放情况, 检测是否有引擎未成功卸载 */
static void engine_mem_leak_check(void)
{
    if (!s_base_valid) { engine_mem_baseline_update(); return; }
    uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int64_t d_int = (int64_t)free_int - (int64_t)s_base_internal;
    int64_t d_ps  = (int64_t)free_ps  - (int64_t)s_base_psram;
    /* 向上偏移一档作为"允许的净余量", 防止基线波动导致的巨额负值掩盖真实泄漏 */
    if (d_ps > 0) s_base_psram = free_ps;      /* 更新基线到更高可用 */
    if (d_int > 0) s_base_internal = free_int;
    d_int = (int64_t)free_int - (int64_t)s_base_internal;
    d_ps  = (int64_t)free_ps  - (int64_t)s_base_psram;
    if (d_int < -(int64_t)TAG_MEM_BASE_INTERNAL_MIN ||
        d_ps  < -(int64_t)TAG_MEM_BASE_PSRAM_MIN) {
        ESP_LOGW(TAG, "[LEAK] 返回主菜单检测到内存未完全释放: 内部空闲比基线少 %lld (基线%u), "
                 "PSRAM 空闲比基线少 %lld (基线%u) — 引擎可能未成功卸载", (long long)(-d_int),
                 (unsigned)s_base_internal, (long long)(-d_ps), (unsigned)s_base_psram);
    } else {
        ESP_LOGI(TAG, "[MEM] 返回主菜单复核正常: 内部空闲=%u (基线%u), PSRAM空闲=%u (基线%u)",
                 (unsigned)free_int, (unsigned)s_base_internal,
                 (unsigned)free_ps, (unsigned)s_base_psram);
    }
}

/* 各引擎的加载/卸载.
 * 注: gam4980_emu.h 会连带 include menu_system.h, 为避免组件间循环依赖, 这里用 extern 声明. */
extern void gam4980_emu_background_init(st7305_handle_t *lcd);
extern void gam4980_emu_unload(void);
extern esp_err_t gb_emu_background_init(void);
extern void gb_emu_unload(void);
extern esp_err_t nes_emu_background_init(void);
extern void nes_emu_unload(void);
extern esp_err_t arduboy_avr_background_init(st7305_handle_t *lcd);
extern void arduboy_avr_unload(void);
extern esp_err_t lavax_emu_background_init(st7305_handle_t *lcd);
extern void lavax_emu_unload(void);
extern esp_err_t vpet_emu_background_init(st7305_handle_t *lcd);
extern void vpet_emu_unload(void);

static void e_gam4980_load(st7305_handle_t *lcd) { gam4980_emu_background_init(lcd); }
static void e_gam4980_unload(void)               { gam4980_emu_unload(); }
static void e_gb_load(st7305_handle_t *lcd)      { (void)lcd; gb_emu_background_init(); }
static void e_gb_unload(void)                    { gb_emu_unload(); }
static void e_nes_load(st7305_handle_t *lcd)     { (void)lcd; nes_emu_background_init(); }
static void e_nes_unload(void)                   { nes_emu_unload(); }
static void e_arduboy_load(st7305_handle_t *lcd) { (void)arduboy_avr_background_init(lcd); }
static void e_arduboy_unload(void)               { arduboy_avr_unload(); }
static void e_lavax_load(st7305_handle_t *lcd)   { (void)lavax_emu_background_init(lcd); }
static void e_lavax_unload(void)                 { lavax_emu_unload(); }
static void e_vpet_load(st7305_handle_t *lcd)    { (void)vpet_emu_background_init(lcd); }
static void e_vpet_unload(void)                  { vpet_emu_unload(); }

/* 引擎注册表: 未来新增模拟器只需在此加一行, 并实现对应的 load/unload */
static const engine_desc_t s_engines[ENGINE_COUNT] = {
    [ENGINE_GAM4980] = { ENGINE_GAM4980, "gam4980", e_gam4980_load, e_gam4980_unload },
    [ENGINE_GB]      = { ENGINE_GB,      "gb",      e_gb_load,      e_gb_unload },
    [ENGINE_NES]     = { ENGINE_NES,     "nes",     e_nes_load,     e_nes_unload },
    [ENGINE_ARDUBOY] = { ENGINE_ARDUBOY, "arduboy", e_arduboy_load, e_arduboy_unload },
    [ENGINE_LAVAX]   = { ENGINE_LAVAX,   "lavax",   e_lavax_load,   e_lavax_unload },
    [ENGINE_VPET]    = { ENGINE_VPET,    "vpet",    e_vpet_load,    e_vpet_unload },
};

void engine_manager_load(engine_id_t id, st7305_handle_t *lcd)
{
    if (id >= 0 && id < ENGINE_COUNT && s_engines[id].load) {
        s_engines[id].load(lcd);
        ESP_LOGI(TAG, "[MEM] 引擎加载后: 内部空闲=%u 最大块=%u PSRAM空闲=%u 最大块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
}

void engine_manager_unload_all(void)
{
    for (int i = 0; i < ENGINE_COUNT; i++) {
        if (s_engines[i].unload)
            s_engines[i].unload();
    }
    ESP_LOGI(TAG, "[MEM] 引擎全部卸载后: 内部空闲=%u 最大块=%u PSRAM空闲=%u 最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    /* 返回主菜单复核: 检测是否有引擎未成功卸载 (内存泄漏) */
    engine_mem_leak_check();
}
