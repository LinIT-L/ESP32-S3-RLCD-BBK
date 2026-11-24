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
    /* 空闲高于基线 (进程内其它任务释放了内存) ⇒ 基线随之上移, 差额归零,
     * 不再"先改基线再重算一遍" (原来那两步等价于把正差额清零)。 */
    if (d_int > 0) { s_base_internal = free_int; d_int = 0; }
    if (d_ps  > 0) { s_base_psram   = free_ps;  d_ps  = 0; }
    if (d_int < -(int64_t)TAG_MEM_BASE_INTERNAL_MIN ||
        d_ps  < -(int64_t)TAG_MEM_BASE_PSRAM_MIN) {
        ESP_LOGD(TAG, "[LEAK] 返回主菜单检测到内存未完全释放: 内部空闲比基线少 %lld (基线%u), "
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
#if LINTOS_ENABLE_MD
extern esp_err_t md_emu_background_init(void);
extern void md_emu_unload(void);
#endif
extern esp_err_t sms_emu_background_init(void);
extern void sms_emu_unload(void);

static esp_err_t e_gam4980_load(st7305_handle_t *lcd) { gam4980_emu_background_init(lcd); return ESP_OK; }
static void e_gam4980_unload(void)               { gam4980_emu_unload(); }
static esp_err_t e_gb_load(st7305_handle_t *lcd) { (void)lcd; return gb_emu_background_init(); }
static void e_gb_unload(void)                    { gb_emu_unload(); }
static esp_err_t e_nes_load(st7305_handle_t *lcd){ (void)lcd; return nes_emu_background_init(); }
static void e_nes_unload(void)                   { nes_emu_unload(); }
static esp_err_t e_arduboy_load(st7305_handle_t *lcd) { return arduboy_avr_background_init(lcd); }
static void e_arduboy_unload(void)               { arduboy_avr_unload(); }
static esp_err_t e_lavax_load(st7305_handle_t *lcd)   { return lavax_emu_background_init(lcd); }
static void e_lavax_unload(void)                 { lavax_emu_unload(); }
static esp_err_t e_vpet_load(st7305_handle_t *lcd)    { return vpet_emu_background_init(lcd); }
static void e_vpet_unload(void)                  { vpet_emu_unload(); }
#if LINTOS_ENABLE_MD
static esp_err_t e_md_load(st7305_handle_t *lcd) { (void)lcd; return md_emu_background_init(); }
static void e_md_unload(void)                    { md_emu_unload(); }
#endif
static esp_err_t e_sms_load(st7305_handle_t *lcd){ (void)lcd; return sms_emu_background_init(); }
static void e_sms_unload(void)                   { sms_emu_unload(); }

/* 引擎注册表: 未来新增模拟器只需在此加一行, 并实现对应的 load/unload */
static const engine_desc_t s_engines[ENGINE_COUNT] = {
    [ENGINE_GAM4980] = { ENGINE_GAM4980, "gam4980", e_gam4980_load, e_gam4980_unload },
    [ENGINE_GB]      = { ENGINE_GB,      "gb",      e_gb_load,      e_gb_unload },
    [ENGINE_NES]     = { ENGINE_NES,     "nes",     e_nes_load,     e_nes_unload },
    [ENGINE_ARDUBOY] = { ENGINE_ARDUBOY, "arduboy", e_arduboy_load, e_arduboy_unload },
    [ENGINE_LAVAX]   = { ENGINE_LAVAX,   "lavax",   e_lavax_load,   e_lavax_unload },
    [ENGINE_VPET]    = { ENGINE_VPET,    "vpet",    e_vpet_load,    e_vpet_unload },
#if LINTOS_ENABLE_MD
    [ENGINE_MD]      = { ENGINE_MD,      "md",      e_md_load,      e_md_unload },
#endif
    [ENGINE_SMS]     = { ENGINE_SMS,     "sms",     e_sms_load,     e_sms_unload },
};

/* 已加载标记: 只有 load() 真正发起过的引擎才允许 unload().
 * 目的 (2026-11-21): 页面退出即卸载, 未加载过的引擎绝不能碰 ——
 * 个别引擎 unload 会进虚拟机/文件系统内部状态, 对未 init 的实例不安全. */
static bool s_loaded[ENGINE_COUNT];

esp_err_t engine_manager_load(engine_id_t id, st7305_handle_t *lcd)
{
    if (id < 0 || id >= ENGINE_COUNT || !s_engines[id].load) return ESP_ERR_INVALID_ARG;
    if (s_loaded[id]) return ESP_OK;       /* 幂等: 重复进入同一页不重复加载 */

    /* ★ 2026-11-22: 原来"先置位再加载且 load 返回 void" ⇒ 加载失败也留下 s_loaded=true,
     *   之后每次进入这一页都被幂等短路, 该引擎本次开机彻底不可用 (只能重启)。
     *   现在同步失败即回滚标记, 下次进入可以重试。
     *   gam4980 是异步加载 (返回时 retro_init 还在后台任务里跑), 只能报"已发起"。 */
    esp_err_t err = s_engines[id].load(lcd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "引擎 %s 加载失败: %s ⇒ 保留未加载状态, 可重试",
                 s_engines[id].name ? s_engines[id].name : "?", esp_err_to_name(err));
        return err;
    }
    s_loaded[id] = true;
    ESP_LOGI(TAG, "[MEM] 引擎加载后: 内部空闲=%u 最大块=%u PSRAM空闲=%u 最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

bool engine_manager_is_loaded(engine_id_t id)
{
    return (id >= 0 && id < ENGINE_COUNT) ? s_loaded[id] : false;
}

void engine_manager_unload(engine_id_t id)
{
    if (id < 0 || id >= ENGINE_COUNT) return;
    if (!s_loaded[id]) return;             /* 没加载过 ⇒ 一个字都不动 */
    /* ★ 2026-11-22 (第4回合): 标记改到 unload() **之后**清。
     *   引擎的 unload 在"模拟任务 100ms 没退出"时会主动跳过内存释放并 return ——
     *   这是第1回合为防止野指针崩溃加的兜底。但原来是**先**把 s_loaded 置 false:
     *     ⇒ 管理器认为这一项已卸载, 之后 unload_all() 直接跳过它,
     *       残留的任务/PSRAM 缓冲再没有第二次退出机会, 只能等整机复位。
     *   现在反过来: unload 走完才算卸载完成, 没成功的留给下一次
     *   unload_all()/退出页面再试一次; 全项目没有任何地方在 unload 期间
     *   读 s_loaded 做判断 (engine_manager_is_loaded 目前零调用), 不会引入递归。 */
    if (s_engines[id].unload)
        s_engines[id].unload();
    s_loaded[id] = false;
    ESP_LOGI(TAG, "[MEM] 引擎 %s 卸载后: 内部空闲=%u 最大块=%u PSRAM空闲=%u 最大块=%u",
             s_engines[id].name ? s_engines[id].name : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void engine_manager_unload_all(bool do_leak_check)
{
    for (int i = 0; i < ENGINE_COUNT; i++)
        engine_manager_unload((engine_id_t)i);
    ESP_LOGI(TAG, "[MEM] 引擎全部卸载后: 内部空闲=%u 最大块=%u PSRAM空闲=%u 最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    /* 泄漏复核仅在"正常回主菜单"时做: 低内存回收/周期兜底时此刻空闲偏低
     * 或重复卸载, 做基线比对会误报 [LEAK], 故由调用方决定是否跳过. */
    if (do_leak_check)
        engine_mem_leak_check();
}
