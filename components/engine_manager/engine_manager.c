/* engine_manager.c - 统一游戏模拟器引擎生命周期管理 */
#include "engine_manager.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "ENGINE"

/* 各引擎的加载/卸载.
 * 注: gam4980_emu.h 会连带 include menu_system.h, 为避免组件间循环依赖, 这里用 extern 声明. */
extern void gam4980_emu_background_init(st7305_handle_t *lcd);
extern void gam4980_emu_unload(void);
extern esp_err_t gb_emu_background_init(void);
extern void gb_emu_unload(void);
extern esp_err_t gbc_emu_background_init(void);
extern void gbc_emu_unload(void);
extern esp_err_t nes_emu_background_init(void);
extern void nes_emu_unload(void);
extern esp_err_t arduboy_avr_background_init(st7305_handle_t *lcd);
extern void arduboy_avr_unload(void);

static void e_gam4980_load(st7305_handle_t *lcd) { gam4980_emu_background_init(lcd); }
static void e_gam4980_unload(void)               { gam4980_emu_unload(); }
static void e_gb_load(st7305_handle_t *lcd)      { (void)lcd; gb_emu_background_init(); }
static void e_gb_unload(void)                    { gb_emu_unload(); }
static void e_gbc_load(st7305_handle_t *lcd)     { (void)lcd; gbc_emu_background_init(); }
static void e_gbc_unload(void)                   { gbc_emu_unload(); }
static void e_nes_load(st7305_handle_t *lcd)     { (void)lcd; nes_emu_background_init(); }
static void e_nes_unload(void)                   { nes_emu_unload(); }
static void e_arduboy_load(st7305_handle_t *lcd) { (void)arduboy_avr_background_init(lcd); }
static void e_arduboy_unload(void)               { arduboy_avr_unload(); }

/* 引擎注册表: 未来新增模拟器只需在此加一行, 并实现对应的 load/unload */
static const engine_desc_t s_engines[ENGINE_COUNT] = {
    [ENGINE_GAM4980] = { ENGINE_GAM4980, "gam4980", e_gam4980_load, e_gam4980_unload },
    [ENGINE_GB]      = { ENGINE_GB,      "gb",      e_gb_load,      e_gb_unload },
    [ENGINE_GBC]     = { ENGINE_GBC,     "gbc",     e_gbc_load,     e_gbc_unload },
    [ENGINE_NES]     = { ENGINE_NES,     "nes",     e_nes_load,     e_nes_unload },
    [ENGINE_ARDUBOY] = { ENGINE_ARDUBOY, "arduboy", e_arduboy_load, e_arduboy_unload },
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
}
