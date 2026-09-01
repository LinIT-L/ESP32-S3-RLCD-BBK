/**
 * os_core.c — OS 内核实现.
 * 页面栈 / 模块注册 / 全局服务登记 / 事件路由 / 输入分发 / 渲染调度.
 *
 * 编译期注册表由 os_register_all_internal()（os_pages.c 提供）生成并注册所有内建模块.
 * 本文件只做"调度", 不直接引用任何具体页面内部实现 → 保持最小耦合.
 */
#include "os.h"
#include "os_internal.h"
#include "wallpapers.h"
#include "favorites.h"
#include "input.h"         /* input_touch_hold_ms: 屏保触摸长按1秒唤醒 */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* vTaskCoreAffinitySet (ESP-IDF 5.x) */

#define TAG "OS"

#define OS_SERVICE_MAX   8
#define OS_STACK_MAX     8

/* ---- 全局注册表 ---- */
static const os_module_t   *s_modules[OS_PAGE_COUNT];
static bool                 s_module_set[OS_PAGE_COUNT];
static const os_service_t  *s_services[OS_SERVICE_MAX];
static int                  s_service_count = 0;

/* 返回链: parent page id, -1 = 顶层桌面 */
static os_page_t  s_back_stack[OS_STACK_MAX];
static int        s_stack_top = -1;

/* 当前激活模块 */
static const os_module_t *s_cur_mod = NULL;

/* 背景脏标记: 弹出 fullscreen 模态覆盖页(如赞助二维码)后, 底层背景被整屏覆盖,
 * 需在下次渲染时先清屏并重绘最底层非模态页, 再叠加当前弹窗. */
static bool s_bg_dirty = false;

void os_init(ui_ctx_t *ctx, st7305_handle_t *lcd)
{
    ctx->lcd          = lcd;
    ctx->current_page = -1;
    ctx->needs_redraw = true;
    ctx->modal_active = false;
    ctx->fullscreen   = false;
    ctx->cur          = NULL;
    s_stack_top       = -1;
    s_cur_mod         = NULL;
    s_bg_dirty        = false;
    s_service_count   = 0;
    for (int i = 0; i < OS_PAGE_COUNT; i++) { s_modules[i] = NULL; s_module_set[i] = false; }
    /* 编译期注册内建模块 */
    os_register_all_internal();
    /* 加载游戏收藏 (从 appdata 分区配置区, 不读 TF 卡) */
    favorites_init();
}

void os_register(const os_module_t *mod)
{
    if (!mod || mod->page_id < 0 || mod->page_id >= OS_PAGE_COUNT) {
        ESP_LOGW(TAG, "register: bad page id");
        return;
    }
    if (s_module_set[mod->page_id]) ESP_LOGW(TAG, "page %d dup -> %s override", mod->page_id, mod->name);
    s_modules[mod->page_id]    = mod;
    s_module_set[mod->page_id] = true;
    ESP_LOGI(TAG, "register %s (page %d)", mod->name, mod->page_id);
}

void os_register_service(const os_service_t *svc)
{
    if (!svc || s_service_count >= OS_SERVICE_MAX) { ESP_LOGW(TAG, "service full"); return; }
    s_services[s_service_count++] = svc;
    ESP_LOGI(TAG, "service +%s", svc->name);
}

/* 当前激活模块 */
static const os_module_t *os_cur_mod(void)
{
    return s_cur_mod;
}

bool os_push(ui_ctx_t *ctx, os_page_t page)
{
    if (page < 0 || page >= OS_PAGE_COUNT || !s_module_set[page]) {
        ESP_LOGW(TAG, "push: page %d not registered", (int)page);
        return false;
    }
    const os_module_t *m = s_modules[page];

    /* 模态覆盖页 (弹窗): 不退出底层, 底层保持活动状态 (pop 时也不重新 on_enter).
     * 非模态页: 正常退出旧模块 (push 时 on_exit / pop 时父 on_enter). */
    bool overlay = m->modal;
    if (s_cur_mod && !overlay && s_cur_mod->on_exit) s_cur_mod->on_exit(ctx);
    /* 父页入返回链 */
    if (s_stack_top < OS_STACK_MAX - 1) {
        s_back_stack[++s_stack_top] = (os_page_t)ctx->current_page;
    }

    s_cur_mod        = m;
    ctx->cur         = m;
    ctx->current_page = (int)page;
    ctx->modal_active = m->modal;
    if (m->on_enter) m->on_enter(ctx);
    ctx->needs_redraw = true;
    ESP_LOGI(TAG, "push -> %s", m->name);
    return true;
}

void os_pop(ui_ctx_t *ctx)
{
    if (s_stack_top < 0) { ESP_LOGW(TAG, "pop: stack empty, stay"); return; }
    os_page_t parent = s_back_stack[s_stack_top--];

    const os_module_t *m = s_cur_mod;
    if (m && m->on_exit) m->on_exit(ctx);

    const os_module_t *pm = (parent >= 0 && parent < OS_PAGE_COUNT) ? s_modules[parent] : NULL;
    s_cur_mod        = pm;
    ctx->cur         = pm;
    ctx->current_page = pm ? (int)parent : -1;
    ctx->modal_active = pm ? pm->modal : false;
    /* 仅当被 pop 的是非模态页时, 父才需要重新 on_enter (其 push 时父已被 on_exit);
     * 被 pop 的是模态覆盖页则父未退出, 保持原状即可. */
    bool was_overlay = m && m->modal;
    if (pm && !was_overlay && pm->on_enter) pm->on_enter(ctx);
    /* 弹出的是 fullscreen 模态覆盖页(如赞助二维码): 底层背景被整屏覆盖过,
     * 需标记背景脏, 下次渲染时先重绘最底层非模态页再叠加弹窗. */
    if (was_overlay && m && m->fullscreen) s_bg_dirty = true;
    ctx->needs_redraw = true;
    ESP_LOGI(TAG, "pop -> page %d", (int)parent);
}

void os_pop_to_main(ui_ctx_t *ctx)
{
    while (s_stack_top >= 0) os_pop(ctx);
    if (s_cur_mod && s_cur_mod->on_exit) { s_cur_mod->on_exit(ctx); s_cur_mod = NULL; }
    ctx->cur = NULL; ctx->current_page = -1; ctx->modal_active = false;
    os_push(ctx, OS_PAGE_MAIN);
}

/* 全屏应用退出确认回调 (确认=退出, 取消/返回=留下; 10 秒超时自动确认退出) */
static bool s_full_back_dlg = false;
static void on_full_back_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_full_back_dlg = false;
    if (result == 0) os_pop(ctx);   /* 确认退出全屏应用 → 回上一级 */
}

/* 触摸长按1秒唤醒屏保后, 该手势遗留动作 (松手/点按/长按) 待忽略一次, 不穿透 */
static bool s_ss_touch_wake_pending = false;

void os_handle_action(ui_ctx_t *ctx, os_action_t a)
{
    /* 触摸长按唤醒屏保后的遗留触摸动作: 仅唤醒, 不穿透给模块 */
    if (s_ss_touch_wake_pending) {
        s_ss_touch_wake_pending = false;
        os_screensaver_reset();
        return;
    }
    bool was_ss = os_screensaver_active();
    os_screensaver_reset();   /* 任何按键唤醒屏保 */
    if (was_ss) return;       /* 刚被本按键唤醒: 本次仅解锁, 不穿透给模块 */

    /* 锁屏键 (软关机键) 点按: 进入壁纸屏保 */
    if (a == OS_ACTION_POWER_LOCK) {
        os_screensaver_force_enter();
        ctx->needs_redraw = true;
        return;
    }
    /* 其余屏保专用动作在非屏保态下可直接忽略 */
    if (a == OS_ACTION_POWER_HINT || a == OS_ACTION_POWER_RELEASE)
        return;

    const os_module_t *m = os_cur_mod();
    if (!m) { ESP_LOGW(TAG, "action: no active module"); return; }
    /* HOME (长按返回键3s / 手柄第7键): 强制退出到桌面主菜单 (保留原选中位) */
    if (a == OS_ACTION_HOME) {
        os_dialog_clear_all(ctx);   /* 关全部弹窗 */
        os_pop_to_main(ctx);        /* 回主菜单 (page_main static 状态保留 → 原位置) */
        return;
    }
    /* 全屏应用按返回 → 退出确认弹窗 (确认退出 / 10秒自动退出 / 返回取消) */
    if (a == OS_ACTION_BACK && m->fullscreen && !m->modal && !s_full_back_dlg) {
        s_full_back_dlg = true;
        os_dialog_confirm_ex(ctx, "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?", /* 退出程序? */
                             10000, 0, on_full_back_confirm, NULL);
        return;
    }
    if (m->action) { m->action(ctx, a); return; }
    /* 无 action 模块: BACK=返回, 其余忽略 */
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

bool os_handle_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 触摸长按唤醒屏保后的遗留触摸动作: 仅唤醒, 不穿透 */
    if (s_ss_touch_wake_pending) {
        s_ss_touch_wake_pending = false;
        os_screensaver_reset();
        return true;
    }
    /* 屏保中: 轻触不唤醒 (需长按1秒), 也不穿透给底层模块 */
    if (os_screensaver_active()) return true;
    const os_module_t *m = os_cur_mod();
    if (!m) return false;
    if (m->touch) return m->touch(ctx, x, y);
    return false;
}

void os_tick(ui_ctx_t *ctx)
{
    for (int i = 0; i < s_service_count; i++)
        if (s_services[i]->tick) s_services[i]->tick(ctx);
    /* 屏保: 触摸长按1秒立即唤醒 (不松手); 该手势遗留动作标 pending 不穿透 */
    if (os_screensaver_active() && input_touch_hold_ms() >= 1000) {
        s_ss_touch_wake_pending = true;
        os_screensaver_reset();
    }
    /* 屏保全屏占据时跳过页面轮询 (避免残留拖拽状态被触屏 poll 触发) */
    if (os_screensaver_active()) return;
    const os_module_t *m = os_cur_mod();
    if (m && m->poll) m->poll(ctx);
}

static uint32_t os_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void os_render(ui_ctx_t *ctx)
{
    /* 屏保: 全屏渲染星空 (忽略状态栏/弹窗, 每帧强制重绘以保持动画), 输入自动唤醒 */
    if (os_screensaver_active()) {
        if (!ctx->lcd) return;
        wp_program_render(ctx->lcd, WP_PROG_STARS, os_now_ms());
        st7305_flush(ctx->lcd);
        ctx->needs_redraw = true;   /* 持续动画 */
        return;
    }
    if (!ctx->needs_redraw) return;
    if (!ctx->lcd) return;

    const os_module_t *m = os_cur_mod();

    /* 背景脏: 从全屏模态覆盖页(赞助二维码)返回后, 先整屏清白并重绘
     * 最底层非模态页作为背景, 再叠加当前弹窗/页面, 避免残留二维码. */
    if (s_bg_dirty) {
        s_bg_dirty = false;
        st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
        /* 从栈底向上找第一个非模态页, 重绘其背景 */
        for (int i = 0; i <= s_stack_top; i++) {
            os_page_t pid = s_back_stack[i];
            if (pid < 0 || pid >= OS_PAGE_COUNT) continue;
            const os_module_t *bm = s_modules[pid];
            if (bm && !bm->modal && bm->render) {
                bm->render(ctx);
                break;
            }
        }
    }

    if (m && m->render) m->render(ctx);

    /* 全屏: 模块静态 fullscreen 或模块在 render 内设置的运行时覆盖 */
    bool fullscreen = m && (m->fullscreen || ctx->fullscreen);
    ctx->fullscreen = false;   /* 帧末复位运行时覆盖 */
    /* 模态弹窗叠在"全屏页"之上时(如游戏/键鼠/终端 的退出确认框),
     * 状态栏不画: 避免"还没退出, 状态栏已重叠到原界面" */
    if (!fullscreen && ctx->modal_active) {
        os_page_t parent = (s_stack_top >= 0) ? s_back_stack[s_stack_top] : -1;
        const os_module_t *pm = (parent >= 0 && parent < OS_PAGE_COUNT) ? s_modules[parent] : NULL;
        if (pm && pm->fullscreen) fullscreen = true;
    }
    if (!fullscreen && !ctx->modal_active) {   /* 弹窗(modal)时不显示状态栏 */
        for (int i = 0; i < s_service_count; i++)
            if (s_services[i]->render) s_services[i]->render(ctx);
    }
    /* 提示弹窗恒最顶: 所有模块/服务渲染之后, 把 toast 覆盖在最上层再整体 flush */
    os_dialog_draw_overlay(ctx);
    st7305_flush(ctx->lcd);
    ctx->needs_redraw = false;
}

bool os_is_main(const ui_ctx_t *ctx)
{
    return ctx->current_page == OS_PAGE_MAIN;
}

bool os_modal_active(const ui_ctx_t *ctx)
{
    return ctx->modal_active;
}

/* ==== CPU 双核静态亲和性工具 (从 os_mgr 迁出归位, 系统范围工具) ==== */
int os_core_current_core(void) { return (int)esp_cpu_get_core_id(); }

void os_core_pin_task(void *task_handle, int core)
{
    if (!task_handle) return;
#if CONFIG_FREERTOS_SMP
    UBaseType_t mask = (core < 0) ? 0x03U : (1U << (unsigned)core);
    vTaskCoreAffinitySet((TaskHandle_t)task_handle, mask);
#else
    /* 经典 IDF FreeRTOS (CONFIG_FREERTOS_SMP=n) 无运行时改核 API:
     * 双核绑定请在创建任务时用 xTaskCreatePinnedToCore(..., core, ...) 指定. */
    (void)core;
#endif
}