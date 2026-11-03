/**
 * page_main.c — 主菜单桌面 (壳).
 *
 * P0 起桌面被拆成"壳 + 主题引擎":
 *   · 本文件只负责**应用清单**(谁可见) 与 **启动应用**, 以及桌面自己的后台杂务
 *     (手柄自动映射待办 / 开机 5s 后的应用中心错峰预热)。
 *   · 画面与交互全部交给当前主题引擎 (见 theme_renderer.h), 现在绑定的是
 *     "carousel" (相机轮播) —— 代码在 components/apps/theme/r_carousel.c。
 *   · 状态栏由 svc_statusbar 独立绘制, 不归主题, 也不归本页。
 *
 * 加新主题 = 写一个新引擎 + 在 os_theme_bind 里换一个指针, 不动本文件。
 */
#include "os.h"
#include "os_theme.h"
#include "theme_renderer.h"
#include "am_internal.h"   /* am_lua_open_if_entry: 主菜单启动 Lua 应用需先设 boot 路径 */
#include "bt_manager.h"    /* 取"待做按键映射"待办 */
#include "apps_pages.h"    /* page_gamepad_request_map_on_enter */
#include "input.h"
#include "esp_timer.h"
#include "esp_log.h"

/* ============ 应用来源: 统一走 os_app 注册表 ============
 * 可见应用缓存: os_app_visible_at()/os_app_visible_count() 每次都读 NVS (flash 很慢),
 * 渲染每帧要调用十几次 → 一帧几十次 NVS ≈ 80ms 卡顿。进桌面时一次性构建成纯数组。 */
static const os_app_t *s_apps_cache[96];   /* 与 OS_APP_MAX 一致 */
static int s_apps_n = 0;
static void main_rebuild_apps(void) {
    s_apps_n = 0;
    for (int i = 0; i < os_app_count() && s_apps_n < (int)(sizeof(s_apps_cache)/sizeof(s_apps_cache[0])); i++) {
        const os_app_t *a = os_app_get(i);
        if (a && os_app_visible(a)) s_apps_cache[s_apps_n++] = a;
    }
}


/* ============ 宿主接口 (引擎通过它取数据/启动应用) ============ */
static int         host_count(void)          { return s_apps_n; }
static const char *host_label(int idx)       { return (idx >= 0 && idx < s_apps_n) ? s_apps_cache[idx]->label : ""; }
static int         host_icon(int idx)        { return (idx >= 0 && idx < s_apps_n) ? s_apps_cache[idx]->icon_idx : 0; }
static void        host_launch(ui_ctx_t *ctx, int idx) {
    if (idx < 0 || idx >= s_apps_n) return;
    /* ⚠ Lua 文件项必须走 am_lua_open_if_entry: 它要先 os_page_lua_set_boot(路径) 再进 Lua 页,
     * 直接 os_push(OS_PAGE_LUA) 会拿不到路径 → 打开空白/上次的程序。 */
    const os_app_t *a = s_apps_cache[idx];
    if (!am_lua_open_if_entry(ctx, a)) os_push(ctx, a->page);
}
static const theme_host_t s_host = {
    .count    = host_count,
    .label    = host_label,
    .icon_idx = host_icon,
    .launch   = host_launch,
};

/* 当前绑定的引擎 (P0 只有 carousel) */
extern const theme_renderer_t r_carousel;

/* ============ 事件转发 ============ */
static void p_main_render(ui_ctx_t *ctx) {
    const theme_renderer_t *r = os_theme_renderer();
    if (r && r->render) r->render(ctx);
}

static void p_main_action(ui_ctx_t *ctx, os_action_t a) {
    const theme_renderer_t *r = os_theme_renderer();
    if (r && r->action) r->action(ctx, a);
}

static bool p_main_touch(ui_ctx_t *ctx, int x, int y) {
    const theme_renderer_t *r = os_theme_renderer();
    return (r && r->touch) ? r->touch(ctx, x, y) : false;
}

static void p_main_poll(ui_ctx_t *ctx) {
    /* ★ 2026-11-01: 应用注册表会**异步增长** —— 开机后 am_lua 在后台扫 TF 卡/内置分区的
     *   Lua 应用，扫完才逐个 os_app_register()。只在 on_enter 建一次缓存的话，
     *   这些条目要等用户下次进桌面才出现（"我刚加的桌面图标，重启后不见了"）。
     *   这里盯住注册表条数，一变就重建缓存。只比一个 int，几乎零开销；
     *   重建读 NVS，但只在真的新增应用时发生。 */
    {
        /* ★ 2026-11-03: 应用表由**统一应用核心**异步填充（内置在开机注册，
         *   TF/Flash 上的 Lua 与已装 LPK 在后台扫描后由 am_core_tick 注册进来）。
         *   桌面必须自己"取货"，否则不进应用中心就永远看不到卡里的应用。 */
        extern bool am_core_tick(void);
        bool lu = am_core_tick();
        static int s_last_cnt = -1;
        static uint32_t s_last_gen = 0;
        int cnt = os_app_count();
        /* ★ 2026-11-01: 除了"条数变了"，还要看**可见性代次** —— 用户在应用中心
         *   添加/删除桌面图标时，条目数一点没变（还是那些应用，只是显示/不显示）。
         *   原来这里只比条数 ⇒ 删了图标回到桌面它还在（"删不掉"），加了的也不出现。
         *   现在两个都盯：条数（新装应用） + 代次（图标的增删）。 */
        uint32_t gen = os_app_vis_gen();
        if (lu || cnt != s_last_cnt || gen != s_last_gen) {
            s_last_cnt = cnt;
            s_last_gen = gen;
            main_rebuild_apps();
            ctx->needs_redraw = true;
        }
    }
    /* ★ C5 连接后自动映射: 连上"名字从没见过"的手柄 → 自动跳到手柄页并打开映射向导。
     *   放在桌面的 poll 里: 后台自动回连通常都是在桌面完成的, 这里一定能接住。 */
    {
        static char nm[33];
        if (bt_manager_take_map_pending(nm, sizeof nm)) {
            ESP_LOGI("MAIN", "[EV] 桌面收到待映射: '%s' → 跳手柄页开映射", nm);
            /* ★ 2026-10-27 linit: **不再弹「检测到新手柄，打开按键映射」的提示**。
             *   连上就该直接看到「请按下 X 按键」，少一次无意义的弹窗。
             *   手柄页 enter 时会按 s_want_map 自动把映射向导叠在菜单之上
             *   （弹窗栈只渲染栈顶 ⇒ 用户看到的就是向导本身），这里只负责跳页。 */
            page_gamepad_request_map_on_enter();
            os_push(ctx, OS_PAGE_GAMEPAD);
            ctx->needs_redraw = true;
            return;
        }
    }

    /* ★★★ 2026-11-01 linit：**恢复提前加载，但改用 PSRAM 承载**（不占核心内存）。
     *   上一版删掉预热是因为它开机就吃掉 47KB **内部** RAM。
     *   真正的根因是 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 —— 所有 <4KB 的小块
     *   被强制塞进内部堆，而商店/Lua 扫描恰好是几千个小分配堆出来的。
     *   现在该阈值已置 0 ⇒ 普通 malloc 一律优先 PSRAM（要内部的驱动本来就显式用
     *   heap_caps_malloc(MALLOC_CAP_INTERNAL/DMA)，不受影响）。
     *   于是预热只花 PSRAM（7.3MB 池，给 256KB 都无所谓），换来进应用中心秒开。
     *   延迟 8s 触发，避开开机瞬间的电流/CPU 峰值。 */
    {
        static bool s_warm_done = false;
        if (!s_warm_done && os_millis() > 8000) {
            s_warm_done = true;
            am_store_prefetch();          /* 统一应用核心 + 商店索引预扫描 —— 走 PSRAM */
            ESP_LOGI("MAIN", "预热(PSRAM): 应用中心预扫描已启动, 不占核心内存");
        }
    }
    if (os_modal_active(ctx)) return;   /* 弹窗/覆盖期间禁止背景拖动 */

    const theme_renderer_t *r = os_theme_renderer();
    if (r && r->poll) r->poll(ctx);
}

/* 进入桌面: 重建可见应用缓存 (应用管理/隐藏开关可能已变更) + 绑定主题引擎 */
static void p_main_enter(ui_ctx_t *ctx) {
    main_rebuild_apps();
    os_theme_bind(&s_host, &r_carousel);
    const theme_renderer_t *r = os_theme_renderer();
    if (r && r->enter) r->enter(ctx);
    ctx->needs_redraw = true;
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_main = {
    .name      = "main",
    .page_id   = OS_PAGE_MAIN,
    .on_enter  = p_main_enter,
    .render    = p_main_render,
    .action    = p_main_action,
    .touch     = p_main_touch,
    .poll      = p_main_poll,
    .fullscreen = false,
};

void os_page_main_register(void) { os_register(&s_mod_main); }
