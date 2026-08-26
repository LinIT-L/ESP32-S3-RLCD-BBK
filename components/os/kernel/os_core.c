/**
 * os_core.c — OS 内核实现.
 * 页面栈 / 模块注册 / 全局服务登记 / 事件路由 / 输入分发 / 渲染调度.
 *
 * 编译期注册表由 os_register_all_internal()（os_pages.c 提供）生成并注册所有内建模块.
 * 本文件只做"调度", 不直接引用任何具体页面内部实现 → 保持最小耦合.
 */
#include "os.h"
#include "os_internal.h"
#include "esp_log.h"

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
    s_service_count   = 0;
    for (int i = 0; i < OS_PAGE_COUNT; i++) { s_modules[i] = NULL; s_module_set[i] = false; }
    /* 编译期注册内建模块 */
    os_register_all_internal();
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

void os_handle_action(ui_ctx_t *ctx, os_action_t a)
{
    const os_module_t *m = os_cur_mod();
    if (!m) { ESP_LOGW(TAG, "action: no active module"); return; }
    if (m->action) { m->action(ctx, a); return; }
    /* 无 action 模块: BACK=返回, 其余忽略 */
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

bool os_handle_touch(ui_ctx_t *ctx, int x, int y)
{
    const os_module_t *m = os_cur_mod();
    if (!m) return false;
    if (m->touch) return m->touch(ctx, x, y);
    return false;
}

void os_tick(ui_ctx_t *ctx)
{
    for (int i = 0; i < s_service_count; i++)
        if (s_services[i]->tick) s_services[i]->tick(ctx);
    const os_module_t *m = os_cur_mod();
    if (m && m->poll) m->poll(ctx);
}

void os_render(ui_ctx_t *ctx)
{
    if (!ctx->needs_redraw) return;
    if (!ctx->lcd) return;

    const os_module_t *m = os_cur_mod();

    if (m && m->render) m->render(ctx);

    /* 全屏: 模块静态 fullscreen 或模块在 render 内设置的运行时覆盖 */
    bool fullscreen = m && (m->fullscreen || ctx->fullscreen);
    ctx->fullscreen = false;   /* 帧末复位运行时覆盖 */
    if (!fullscreen) {
        for (int i = 0; i < s_service_count; i++)
            if (s_services[i]->render) s_services[i]->render(ctx);
    }
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