/**
 * os_core.c — OS 内核实现.
 * 页面栈 / 模块注册 / 全局服务登记 / 事件路由 / 输入分发 / 渲染调度.
 *
 * 编译期注册表由 os_register_all_internal()（os_pages.c 提供）生成并注册所有内建模块.
 * 本文件只做"调度", 不直接引用任何具体页面内部实现 → 保持最小耦合.
 */
#include "os.h"
#include "os_internal.h"
#include "vibrator.h"   /* V1.2.x: UI 震动 (菜单导航/确认) */
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

/* V1.5.x: 当前页查询 — 深睡待机前保存页面, 唤醒后恢复"原页面" */
static ui_ctx_t *s_os_ctx = NULL;
int os_current_page(void) { return s_os_ctx ? s_os_ctx->current_page : -1; }

void os_init(ui_ctx_t *ctx, st7305_handle_t *lcd)
{
    s_os_ctx = ctx;
    ctx->lcd          = lcd;
    ctx->current_page = -1;
    ctx->needs_redraw = true;
    ctx->modal_active = false;
    ctx->fullscreen   = false;
    ctx->cur          = NULL;
    ctx->map_touch    = NULL;   /* 页面级触摸映射钩子: 进入页时由模块设置, 初始必为 NULL */
    s_stack_top       = -1;
    s_cur_mod         = NULL;
    s_bg_dirty        = false;
    s_service_count   = 0;
    for (int i = 0; i < OS_PAGE_COUNT; i++) { s_modules[i] = NULL; s_module_set[i] = false; }
    /* 编译期注册内建模块 */
    os_register_all_internal();
    /* 加载游戏收藏 (从 appdata 分区配置区, 不读 TF 卡) */
    favorites_init();
    /* V1.5.x: 独立屏保监视任务 — 休眠最高优先级, 不随页面/游戏阻塞 */
    wp_screensaver_supervisor_start(ctx->lcd);
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
    } else {
        /* 栈满(极端深导航): 挤掉最旧父页, 保证栈顶恒为最近父页, pop 返回链不错乱 */
        for (int i = 0; i < OS_STACK_MAX - 1; i++) s_back_stack[i] = s_back_stack[i + 1];
        s_back_stack[OS_STACK_MAX - 1] = (os_page_t)ctx->current_page;
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
    /* 屏保唤醒: 物理键/手柄直接唤醒; 触摸动作须"触摸唤醒"开关开启才唤醒.
     * V1.5.x: 关闭触摸唤醒后, 屏保态长按/滑动等触摸产生的 action 不得解除屏保. */
    if (was_ss) {
        if (input_touch_last_action() && !wp_screensaver_touch_wake_enabled()) {
            /* 触摸唤醒已关: 忽略该触摸动作, 屏保保持, 不穿透给模块 */
            s_ss_touch_wake_pending = true;   /* 遗留触摸待忽略 (避免松手后误触发) */
            return;
        }
        os_screensaver_reset();   /* 物理键/手柄或允许的触摸 → 唤醒 */
        return;
    }

    /* V1.6.x: 提示弹窗(单独 toast)显示时, 任意按键先关闭它 (除 HOME 设置键). 若当前在
     * 自驱动全屏引擎/弹窗下层, toast 由 overlay 绘制, 底层不处理 → 这里统一兜底即时关. */
    if (os_dialog_toast_active()) {
        if (a != OS_ACTION_HOME) {
            os_dialog_toast_clear();
            ctx->needs_redraw = true;
            return;
        }
    }

    /* 锁屏键 (软关机键) 点按: 进入壁纸屏保 */
    if (a == OS_ACTION_POWER_LOCK) {
        os_screensaver_force_enter();
        ctx->needs_redraw = true;
        return;
    }
    /* 其余屏保专用动作在非屏保态下可直接忽略 */
    if (a == OS_ACTION_POWER_HINT || a == OS_ACTION_POWER_RELEASE)
        return;
    /* 长按硬件确认键 2s → 任意页面进入蓝牙搜索模式.
     * 例外: 应用管家 用确认键长按 3s 切换 网格/列表 视图, 此时不进入蓝牙搜索. */
    const os_module_t *cur_m = os_cur_mod();
    if (a == OS_ACTION_BT_SEARCH && !(cur_m && cur_m->page_id == OS_PAGE_APP_MANAGER)) {
        os_page_gamepad_open_scan(ctx);
        return;
    }

    const os_module_t *m = os_cur_mod();
    if (!m) { ESP_LOGW(TAG, "action: no active module"); return; }
    /* HOME (长按返回键3s / 手柄第7键): 强制退出到桌面主菜单 (保留原选中位) */
    if (a == OS_ACTION_HOME) {
        os_dialog_clear_all(ctx);   /* 关全部弹窗 */
        os_pop_to_main(ctx);        /* 回主菜单 (page_main static 状态保留 → 原位置) */
        return;
    }
    /* V1.2.x: UI 震动 — 非游戏态下, 方向导航 / 确认 / 返回键触发按键反馈.
     * V1.5.x: 归"UI震动"开关 (vibrator_tap_ui), 与"按键模拟"通道分离 —
     * 18ms/100% 短脉冲与触摸点击手感一致; UI震动关 → 导航不震, 不影响触摸点击.
     * 覆盖主菜单/设置/弹窗/应用管家/番茄钟滚轮等所有 UI 导航 (弹窗键也经此处).
     * 游戏态不在此触发 (游戏内按键走 input 层 joypad 边沿游戏震动). */
    if (!input_is_game_mode() &&
        (a == OS_ACTION_UP || a == OS_ACTION_DOWN ||
         a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT ||
         a == OS_ACTION_CONFIRM || a == OS_ACTION_BACK)) {
        vibrator_tap_ui(18, 100);
    }
    /* 全屏应用按返回 → 退出确认弹窗 (确认退出 / 10秒自动退出 / 返回取消).
     * 门控: 仅"游戏(分类引擎)"拦截弹"退出程序?"——引擎进 eng_task_run_loop 时
     * input_set_gamepad_gamemode(true) 使 input_is_game_mode() 为真; 普通应用程序
     * (如虚拟键鼠)不在此拦截, 改由各自页面 p_action 处理返回(如先弹选择菜单).
     * 用户规范: 游戏中只有分类引擎游戏才弹"退出程序?", 应用程序不需要. */
    if (a == OS_ACTION_BACK && m->fullscreen && !m->modal && !s_full_back_dlg
        && input_is_game_mode()) {
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
    /* 页面级触摸映射 (书架竖屏/180°): 分发前统一换算, 弹窗层与页面层同享 */
    if (ctx->map_touch) ctx->map_touch(&x, &y);
    /* V1.6.x: 提示弹窗(单独 toast)显示时, 任意触摸点击先关闭它 (不落到底层模块) */
    if (os_dialog_toast_active()) {
        os_dialog_toast_clear();
        ctx->needs_redraw = true;
        return true;
    }
    const os_module_t *m = os_cur_mod();
    if (!m) return false;
    if (m->touch) return m->touch(ctx, x, y);
    /* V1.5.x: 常驻 service 热区兜底 — 当前模块无 touch 且非全屏/非弹窗时,
     * 依次询问各 service 是否命中 (状态栏 mini 图标等常驻控件). 命中即消费. */
    if (!m->fullscreen && !m->modal) {
        for (int i = 0; i < s_service_count; i++) {
            if (s_services[i]->touch && s_services[i]->touch(ctx, x, y))
                return true;
        }
    }
    return false;
}

void os_tick(ui_ctx_t *ctx)
{
    /* V1.7.x: 全局 toast 超时清理 — 任意页面/弹窗上提示 1s 自动关+重绘 (不依赖 DIALOG 页 poll) */
    os_dialog_toast_tick(ctx);

    /* V1.4.x: 每帧同步"按键模拟"上下文屏蔽 — 主菜单/软件管家页直接内容不震
     * (拖动图标/管理列表保持干净), 弹窗覆盖时 modal_active=true → 不拦截,
     * 弹窗内点击仍有按键手感; 二级页面/软件程序照常震动. */
    input_set_key_sim_ctx_block(
        !ctx->modal_active &&
        (ctx->current_page == OS_PAGE_MAIN ||
         ctx->current_page == OS_PAGE_APP_MANAGER));

    /* 资源管家: 每次"进入"主菜单/软件管家时执行一次高权限清扫+内存遗检
     * (带锁存, 只在切换到达该页那帧触发一次, 不逐帧刷屏/不逐帧扫网). */
    {
        static int s_hk_last = -2;
        int cp = ctx->current_page;
        if (cp == OS_PAGE_MAIN || cp == OS_PAGE_APP_MANAGER) {
            if (cp != s_hk_last) { s_hk_last = cp; os_housekeeper_sync(); }
        } else {
            s_hk_last = -2;
        }
    }

    for (int i = 0; i < s_service_count; i++)
        if (s_services[i]->tick) s_services[i]->tick(ctx);
    /* 屏保: 触摸长按1秒立即唤醒 (不松手); 该手势遗留动作标 pending 不穿透.
     * V1.5.x: 受"触摸唤醒"设置开关控制 (默认关) — 关=触摸不唤醒屏保,
     * 仅物理键/手柄唤醒 (os_handle_action 按键路径不受影响). */
    if (os_screensaver_active() && !input_touch_blocked() &&
        wp_screensaver_touch_wake_enabled() &&
        input_touch_hold_ms() >= 1000) {
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
    /* 屏保: 渲染交给独立监视任务(强制最高级, 不被主循环/游戏阻塞), 此处仅保持
     * 帧持续(needs_redraw), 不再在此刷屏, 避免与监视任务双任务刷屏竞争 */
    if (os_screensaver_active()) {
        ctx->needs_redraw = true;
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
    /* LCD 写者协商:
     * - 非引擎态: os_render 整体冲刷.
     * - 引擎(game)态无覆盖: 让权给 video task/引擎(各自 flush 帧), 避免双写同一 SPI.
     * - 引擎态有弹窗/toast 覆盖: 必须由 os_render flush 一次, 否则覆盖层画进 fb 却送不上屏(不可见). */
    bool overlay_active = os_dialog_toast_active() || ctx->modal_active;
    if (!input_is_game_mode() || overlay_active) st7305_flush(ctx->lcd);
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

/* V1.6.x: AP 热点激活标志 — 任一模块开热点时置位, 状态栏据此显示 NET 图标 */
static bool s_ap_active = false;
void os_ap_set(bool on) { s_ap_active = on; }
bool os_ap_active(void) { return s_ap_active; }

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