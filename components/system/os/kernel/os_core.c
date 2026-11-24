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
#include "power_mgr.h"    /* 电源管理: 唤醒源注册 + 深睡前钩子 (2026-10-28 从 wallpapers 拆出) */
#include "power_stat.h"   /* power_stat_on_deep_sleep(): 睡前一笔记账, 挂在 power_mgr 钩子上 */
#include "input.h"         /* input_touch_hold_ms: 屏保触摸长按1秒唤醒 */
#include "bt_manager.h"    /* bt_manager_is_connected: 接手柄时方向键齐全, 不做 RIGHT→DOWN 归一 */
#include "gesture_back.h"  /* 底部上滑=返回 的开关: gesture_back_enable (!m->swipe_back_disabled) */
#include "ui_common.h"     /* fill_rect: 底部上滑"跟手"指示条绘制 */
#include "glyph.h"         /* glyph_set_family_scope: 字体族只在白名单页面生效 */
#include "statusbar_policy.h"   /* ★ 2026-11-23: 状态栏显隐判据(纯逻辑, 主机测试同一份) */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* vTaskCoreAffinitySet (ESP-IDF 5.x) */

#define TAG "OS"

#define OS_SERVICE_MAX   8
#define OS_STACK_MAX     8

/* 长按返回 3s = HOME 的强制退出 (实现放在退出标志之后, 这里先声明) */
static void os_force_home(ui_ctx_t *ctx);

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

/* ★ 2026-11-23: 返回栈深度 —— 只读。Lua 退出收尾用它判断"还有没有父页可弹",
 *   避免栈空时 os_pop 原地早退导致循环空转。 */
int os_stack_depth(void) { return s_stack_top + 1; }

/* ★ 2026-11-23 linit: 状态栏本帧是否真的显示 (应用据此算内容起点)。
 *   值由 os_render 每帧合成后写入 ctx→statusbar_on, 这里只读, 不改状态。 */
bool os_statusbar_visible(void)
{
    return s_os_ctx ? s_os_ctx->statusbar_on : false;
}

void os_init(ui_ctx_t *ctx, st7305_handle_t *lcd)
{
    s_os_ctx = ctx;
    ctx->lcd          = lcd;
    ctx->current_page = -1;
    ctx->needs_redraw = true;
    ctx->modal_active = false;
    ctx->fullscreen   = false;
    ctx->statusbar_on = false;  /* ★ 2026-11-23: 首帧前状态栏尚未合成 */
    ctx->cur          = NULL;
    ctx->map_touch    = NULL;   /* 页面级触摸映射钩子: 进入页时由模块设置, 初始必为 NULL */
    s_stack_top       = -1;
    s_cur_mod         = NULL;
    s_bg_dirty        = false;
    s_service_count   = 0;
    for (int i = 0; i < OS_PAGE_COUNT; i++) { s_modules[i] = NULL; s_module_set[i] = false; }
    /* 编译期注册 os 内核侧全局服务 (状态栏等).
     * V1.2: 应用页/应用表注册已迁到 L4 apps 组件, 由 main 在 os_init() 后调 apps_register_all(). */
    os_register_services();
    /* 加载游戏收藏 (从 appdata 分区配置区, 不读 TF 卡) */
    favorites_init();
    /* V1.5.x: 独立屏保监视任务 — 休眠最高优先级, 不随页面/游戏阻塞 */
    wp_screensaver_supervisor_start(ctx->lcd);
    /* ★ 2026-10-28 (第 5 阶段 Step 3): 电源模块由 os 内核在此初始化。
     *   原来这一步藏在 wallpapers 的屏保监督任务里(wp_power_init), 属 ui→system 倒置;
     *   现在 ui 不再认识 power_mgr —— 关机画面在调用方(main), 硬件顺序在 power_mgr。
     *   钩子位置与原 power_stat_on_deep_sleep() 调用点一致(屏 deinit 之后、深睡之前)。 */
    power_mgr_init(ctx->lcd);
    power_mgr_set_pre_sleep_hook(power_stat_on_deep_sleep);
}

void os_register(const os_module_t *mod)
{
    if (!mod || mod->page_id < 0 || mod->page_id >= OS_PAGE_COUNT) {
        ESP_LOGD(TAG, "register: bad page id");
        return;
    }
    if (s_module_set[mod->page_id]) ESP_LOGD(TAG, "page %d dup -> %s override", mod->page_id, mod->name);
    s_modules[mod->page_id]    = mod;
    s_module_set[mod->page_id] = true;
    ESP_LOGI(TAG, "register %s (page %d)", mod->name, mod->page_id);
}

void os_register_service(const os_service_t *svc)
{
    if (!svc || s_service_count >= OS_SERVICE_MAX) { ESP_LOGD(TAG, "service full"); return; }
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
        ESP_LOGD(TAG, "push: page %d not registered", (int)page);
        return false;
    }
    const os_module_t *m = s_modules[page];

    /* 模态覆盖页 (弹窗): 不退出底层, 底层保持活动状态 (pop 时也不重新 on_enter).
     * 非模态页: 正常退出旧模块 (push 时 on_exit / pop 时父 on_enter). */
    bool overlay = m->modal;
    if (s_cur_mod && !overlay && s_cur_mod->on_exit) s_cur_mod->on_exit(ctx);
    /* 父页入返回链。
     * ★ 2026-11-22: current_page < 0 = "当前没有页面"(强制回桌面后被清成 -1),
     *   这时**不压栈**。原来无条件压, 于是 os_pop_to_main 之后栈里躺着一个 -1:
     *   用户在桌面按返回 → pop 出 -1 → pm=NULL、s_cur_mod=NULL、current_page=-1,
     *   整屏没有页面可渲染(白屏/残留), 只能重启。栈空时 os_pop 会早退留在桌面,
     *   这才是"桌面按返回没反应"的正确行为。压/弹仍一一对称(不压 ⇒ 少弹一个)。 */
    if (ctx->current_page >= 0) {
        if (s_stack_top < OS_STACK_MAX - 1) {
            s_back_stack[++s_stack_top] = (os_page_t)ctx->current_page;
        } else {
            /* 栈满(极端深导航): 挤掉最旧父页, 保证栈顶恒为最近父页, pop 返回链不错乱 */
            for (int i = 0; i < OS_STACK_MAX - 1; i++) s_back_stack[i] = s_back_stack[i + 1];
            s_back_stack[OS_STACK_MAX - 1] = (os_page_t)ctx->current_page;
        }
    }

    /* ★ 2026-11-22: 旋转状态在**唯一切换点**兜一次底 —— 竖屏(300×400)只属于电子书/阅读器。
     *   原来只靠各页自己的 on_exit 复位: 只要有一条退出路径没走到(命令式翻页 / 弹窗链路 /
     *   页面被直接 push 覆盖), `ui_set_portrait(true)` 与面板 MADCTL 就会残留, 之后进的
     *   每一页都按竖屏布局渲染 —— 现场表现就是"整屏侧着了", 而且手动按返回又正常(走了 on_exit)。
     *   放在这里而不是各页重复判断: 需要竖屏的页(书架)在自己的 on_enter 里再打开,
     *   顺序天然正确; 模态覆盖页(弹窗)不动底层旋转, 否则阅读器里弹窗会被掰回横屏。 */
    /* ★ 2026-11-22: 判据补上"驱动侧真的还挂着一块旋转缓冲"。
     *   只看 ui_screen_w() 会漏掉 180°(r_flip1) 这种情况 —— 它不改逻辑宽高,
     *   但送屏前要做内存翻转, 残留下来就是"界面上下颠倒"。
     *   ⚠ 覆盖页(弹窗)不动: 阅读器里弹窗必须保持竖屏。 */
    bool rot_dirty = (ctx->lcd && (ctx->lcd->pt_pfb || ctx->lcd->r_flip1));
    if (!overlay && (ui_is_portrait() || rot_dirty)) {
        /* ★ 2026-11-22: 用驱动层的**完整复位**而不是 st7305_set_rotation(lcd,0,NULL)。
         *   后者只清绘制缓冲 (pt_pfb/pt_rot/r_flip1), **不动控制器**; 而这条兜底路径的
         *   职责是"把系统拉回横屏", 面板侧 MADCTL 与地址窗口也必须一起回横屏,
         *   否则 fb 内容正确也会被面板再转一次 —— 表现就是"整屏侧着"。
         *   驱动里本来就有 st7305_reset_rotation() 做全套, 这里用它 (单一实现)。
         *   只在切页时调一次, 多发几条 SPI 命令的开销可忽略。 */
        if (ctx->lcd) st7305_reset_rotation(ctx->lcd);
        ui_set_rotation_mode(0);
        input_set_screen_rotation(0);
        ctx->map_touch = NULL;
    }

    s_cur_mod        = m;
    ctx->cur         = m;
    ctx->current_page = (int)page;
    ctx->modal_active = m->modal;
    /* 进入非模态页: 按该页是否屏蔽上滑返回, 统一设置全局手势开关 (单一权威来源).
     * 模态覆盖页不改变底层页的上滑返回状态. */
    if (!overlay) gesture_back_enable(!m->swipe_back_disabled);
    /* 底部拖动屏蔽带: 全屏绘图页(白板)要整面可用, 其余页保持屏蔽 (默认行为不变) */
    if (!overlay) input_set_bottom_shield(!m->no_bottom_shield);
    /* V1.7.x: 页面打开耗时体检 — on_enter 是"点开应用/进页面要等一会儿"的头号嫌疑
     * (扫描目录/读 NVS/卸载引擎都在这里跑). ≥5ms 打一行, 定位是哪一页慢. */
    uint64_t t_enter0 = (uint64_t)esp_timer_get_time();
    if (m->on_enter) m->on_enter(ctx);
    uint64_t d_enter = (uint64_t)esp_timer_get_time() - t_enter0;
    ctx->needs_redraw = true;
    ESP_LOGI(TAG, "push -> %s", m->name);
    /* 页面打开耗时体检 — on_enter 是"点开应用/进页面要等一会儿"的头号嫌疑
     * (扫描目录/读 NVS/加载引擎都在这里跑)。
     * ★ 2026-11-21: 原来 ≥5ms 走 LOGD, 现场(info 级)根本看不到, 等于没有埋点。
     *   改成 ≥20ms 用 WARN —— 与"退出慢"同一口径, 现场日志就能直接列出慢页面。 */
    if (d_enter >= 20000)
        ESP_LOGW(TAG, "打开慢: push -> %s 的 on_enter=%llums",
                 m->name, (unsigned long long)(d_enter / 1000));
    return true;
}

void os_pop(ui_ctx_t *ctx)
{    if (s_stack_top < 0) { ESP_LOGD(TAG, "pop: stack empty, stay"); return; }
    /* ★★★ 2026-10-31: 离开页面前**必须先收回快捷栏的触摸独占**。
     *   原来只有"按 BACK 键"会关快捷栏; 如果快捷栏开着时页面被别的路径弹掉
     *   (游戏自己退出/超时回主菜单/弹窗链路), overlay 锁会**留在置位状态**,
     *   之后所有普通调用者都拿不到触摸 ⇒ 整机触摸失灵, 只能重启。
     *   os_quickbar_close() 内部会解锁 + 释放虚拟手柄的按下态。 */
    if (os_quickbar_is_open()) os_quickbar_close();
    os_page_t parent = s_back_stack[s_stack_top--];

    const os_module_t *m = s_cur_mod;
    bool was_overlay = m && m->modal;
    const os_module_t *pm = (parent >= 0 && parent < OS_PAGE_COUNT) ? s_modules[parent] : NULL;

    /* ★ 2026-11-01 诊断(可关): 页面被弹走时留下"从哪页 → 到哪页", 并标出当前是否
     *   在弹窗流程里。排查"应用中心连 Wi-Fi 后被弹回主菜单"靠这一行定位:
     *   有它 ⇒ 页面真的被 pop 了(且能看到是不是弹窗链路的锅); 没它 ⇒ 不是 pop 的问题。 */
    ESP_LOGI(TAG, "pop: %s -> %s (back_stack=%d modal=%d)",
             m ? m->name : "?", pm ? pm->name : "(main)", s_stack_top, (int)was_overlay);

    /* ★★ 2026-10-14 linit 反馈"确认退出后要干等 0.5 秒才真的退" —— 根因:
     *   `os_pop` 原来是**先跑本页 on_exit, 再切父页**, 而 on_exit 里有几百 ms 的阻塞动作
     *   (关 WiFi = `esp_wifi_stop()`+`esp_wifi_deinit()`, 拆 USB HID, 卸载引擎/字体)。
     *   渲染在本帧末尾才发生 ⇒ 屏幕上那一页一直挂着, 用户看到的就是"卡半秒才退"。
     *   修法: **全屏应用页退出时, 先把父页切上来刷一帧(画面立刻变), 再执行阻塞的 on_exit**。
     *   顺序对调只影响"先清理后建页"的时序; 菜单层(非全屏)pop 频繁且 on_exit 很轻, 保持原序不加开销。 */
    bool late_exit = m && m->on_exit && !was_overlay && (m->fullscreen || m->defer_exit);

    /* 退出耗时体检 —— ≥30ms 打一行, 定位是哪一段慢 (配合 late_exit 修法验证) */
    uint64_t t_pop0 = (uint64_t)esp_timer_get_time();
    uint64_t d_exit = 0;
    if (m && m->on_exit && !late_exit) {
        m->on_exit(ctx);
        d_exit = (uint64_t)esp_timer_get_time() - t_pop0;
    }

    s_cur_mod        = pm;
    ctx->cur         = pm;
    ctx->current_page = pm ? (int)parent : -1;
    ctx->modal_active = pm ? pm->modal : false;
    /* 返回到真实父页(非模态覆盖): 按父页是否屏蔽上滑返回, 统一恢复全局手势开关.
     * 这样即使某页的 on_exit 被跳过(直接 os_pop), 返回后上滑返回也必定恢复,
     * 不会再卡成"全局失效". 弹出的只是模态覆盖层时不动底层页状态. */
    if (pm && !was_overlay) gesture_back_enable(!pm->swipe_back_disabled);
    /* 回到父页时一并恢复父页的底部屏蔽带状态 (与上滑返回同一套"单一权威来源") */
    if (pm && !was_overlay) input_set_bottom_shield(!pm->no_bottom_shield);
    /* 仅当被 pop 的是非模态页时, 父才需要重新 on_enter (其 push 时父已被 on_exit);
     * 被 pop 的是模态覆盖页则父未退出, 保持原状即可. */
    if (pm && !was_overlay && pm->on_enter) pm->on_enter(ctx);
    /* 弹出的是 fullscreen 模态覆盖页(如赞助二维码): 底层背景被整屏覆盖过,
     * 需标记背景脏, 下次渲染时先重绘最底层非模态页再叠加弹窗. */
    if (was_overlay && m && m->fullscreen) s_bg_dirty = true;
    ctx->needs_redraw = true;

    if (late_exit) {
        /* ① 先把父页刷上屏 —— 用户确认退出后**立刻**看到界面变化, 不再干等 */
        os_render(ctx);
        /* ② 再做阻塞的退出清理 (关射频/拆 USB/卸载引擎); 画面已经切走, 卡这段无所谓 */
        uint64_t t_ex = (uint64_t)esp_timer_get_time();
        m->on_exit(ctx);
        d_exit = (uint64_t)esp_timer_get_time() - t_ex;
        ctx->needs_redraw = true;   /* 清理可能改了全局态(如 fullscreen/theme), 收尾再刷一帧 */
    }

    {   /* 退出慢体检: ≥30ms 打一行 */
        uint64_t d_all = (uint64_t)esp_timer_get_time() - t_pop0;
        if (d_all >= 30000)
            ESP_LOGW(TAG, "退出慢: %s.on_exit=%llums%s 合计=%llums",
                     m ? m->name : "-", (unsigned long long)(d_exit / 1000),
                     late_exit ? " (已改成先刷屏后清理)" : "",
                     (unsigned long long)(d_all / 1000));
    }
    ESP_LOGI(TAG, "pop -> page %d", (int)parent);
}

void os_pop_to_main(ui_ctx_t *ctx)
{
    /* 先清弹窗栈: os_pop 只弹页面栈, 若本函数在弹窗栈非空(s_top>=0)时被调用,
     * DIALOG 页会被弹掉而 s_top 残留, 之后 os_dialog_push 因 s_top<0 不成立
     * 不再压 DIALOG 页 → 弹窗永远画不出来 (os_mgr 空闲超时路径即踩此坑). */
    os_dialog_clear_all(ctx);

    /* ★ 2026-11-22: 改成**一次清栈**, 不再逐层 os_pop。
     *   原实现 `while (s_stack_top >= 0) os_pop(ctx)` 会给每一层都跑一遍
     *   "子页 on_exit + 父页 on_enter" —— 那是"用户逐级按返回"的语义。
     *   强制回桌面(长按返回 3s / 空闲超时 / 手柄 HOME)用不着中间层重建,
     *   而且有两个实害:
     *     ① 深链路(游戏内 → 游戏列表 → 应用中心 → 桌面)要连跑 3 个 on_enter,
     *        里面有扫描目录/读 NVS, 用户按完得等一两秒才看到桌面;
     *     ② 某层 on_enter 若自己 os_push(引擎列表/商店页常见), s_stack_top 被
     *        重新推高 ⇒ while 条件再次成立, 循环收不敛, 表现就是"长按返回卡住"。
     *   资源只需要释放一次: 弹窗关净后 s_cur_mod 就是最底层非模态页,
     *   跑它自己的 on_exit(卸载引擎/关射频/停任务都在里面); 中间层在各自
     *   push 进下一页时就已 on_exit 过, 不会再占资源。 */
    if (os_quickbar_is_open()) os_quickbar_close();
    if (s_cur_mod && s_cur_mod->on_exit) s_cur_mod->on_exit(ctx);
    s_cur_mod   = NULL;
    s_stack_top = -1;
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

/* ★ 2026-11-22: 屏保唤醒后的"输入屏蔽窗口" (ms).
 *   唤醒通常由"按下"触发, 而同一次按压还会继续产出后续动作:
 *     · 手柄方向键 —— 按住 450ms 后每 110ms 自动重复, 会连着穿透到页面(菜单一直滚);
 *     · 物理键长按 —— 长按动作(LONG_PRESS/HOME)在松手前后补发。
 *   屏保刚退出的这 350ms 内一律丢弃动作, 让"按一下 = 只退出壁纸"成立。
 *   350ms 是折中: 比最慢的自动重复起始(450ms)短, 不影响用户唤醒后马上按返回。 */
#define OS_SS_WAKE_SWALLOW_MS 350
static uint32_t s_ss_swallow_until = 0;

void os_handle_action(ui_ctx_t *ctx, os_action_t a)
{
    /* 触摸长按唤醒屏保后的遗留触摸动作: 仅唤醒, 不穿透给模块 */
    if (s_ss_touch_wake_pending) {
        s_ss_touch_wake_pending = false;
        os_screensaver_reset();
        return;
    }
    uint32_t now_a = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool was_ss = os_screensaver_active();
    /* 屏保唤醒: 物理键/手柄直接唤醒; 触摸动作须"触摸唤醒"开关开启才唤醒.
     * V1.5.x: 关闭触摸唤醒后, 屏保态长按/滑动等触摸产生的 action 不得解除屏保. */
    if (was_ss) {
        /* ★ 2026-11-22: 排"确认/返回按下去没反应" —— 若屏保标志残留, 每个动作都会
         *   走到这里只做唤醒就 return, 页面永远收不到 ⇒ 症状与按键坏掉完全一样。
         *   打一行日志, 现场据此区分"按键没进来" vs "进来了被屏保吃掉"。 */
        ESP_LOGW(TAG, "动作 a=%d 进屏保分支: 只唤醒不穿透 (屏保标志此前为 1)", (int)a);
        if (input_touch_last_action() && !wp_screensaver_touch_wake_enabled()) {
            /* 触摸唤醒已关: 忽略该触摸动作, 屏保保持, 不穿透给模块 */
            s_ss_touch_wake_pending = true;   /* 遗留触摸待忽略 (避免松手后误触发) */
            return;
        }
        os_screensaver_reset();   /* 物理键/手柄或允许的触摸 → 唤醒 */
        s_ss_swallow_until = now_a + OS_SS_WAKE_SWALLOW_MS;
        return;
    }
    /* 刚被唤醒: 同一次按压的后续动作(自动重复/长按补发)不要落到页面上 */
    if (s_ss_swallow_until != 0 && (int32_t)(now_a - s_ss_swallow_until) < 0) {
        ESP_LOGW(TAG, "动作 a=%d 被唤醒吞掉窗口丢弃 (剩 %dms)",
                 (int)a, (int)(s_ss_swallow_until - now_a));
        return;
    }
    s_ss_swallow_until = 0;

    /* V1.6.x: 提示弹窗(单独 toast)显示时, 任意按键先关闭它 (除 HOME 设置键). 若当前在
     * 自驱动全屏引擎/弹窗下层, toast 由 overlay 绘制, 底层不处理 → 这里统一兜底即时关. */
    if (os_dialog_toast_active()) {
        if (a != OS_ACTION_HOME) {
            os_dialog_toast_dismiss(ctx);   /* 一并收掉 toast 开的弹窗页, 否则下层页会被下一次 BACK 关掉 */
            return;
        }
    }

    /* 锁屏键 (软关机键) 点按: 进入壁纸屏保 */
    if (a == OS_ACTION_POWER_LOCK) {
        os_screensaver_force_enter();
        ctx->needs_redraw = true;
        return;
    }
    /* ★ 2026-10-29 linit: 快捷栏打开时, **返回键优先把快捷栏收起**,
     *   而不是直接退出页面/弹出"退出程序?"。收起后本次按键即消费掉。 */
    if (a == OS_ACTION_BACK && os_quickbar_is_open()) {
        os_quickbar_close();
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
    if (!m) { ESP_LOGD(TAG, "action: no active module"); return; }

    /* ★ 2026-11-20 linit（微雪无触摸机型反馈"换不了下一个菜单、方向不能用"）:
     *   该机型只有 BOOT 一个方向键，短按投 **RIGHT**（input.c 无触摸分支）。
     *   桌面 carousel 正是用 RIGHT 切图标 ⇒ 保留；但设置 / 应用中心 / 文件列表这些
     *   二级页**全部用 UP/DOWN 移动选中**，原样投 RIGHT 就等于方向键全废。
     *   这里做一次"导航归一"：**无触摸 + 未接手柄 + 非桌面 + 非全屏应用** 时 RIGHT 视作 DOWN。
     *   ⚠ 有触摸机型（BOOT=返回）、接了手柄（方向键齐全）、桌面、全屏应用（Lua/引擎自用 RIGHT）
     *     行为完全不变。
     *   ★ 2026-11-23: Lua 页的状态栏改成"按页声明"后模块静态 fullscreen=false 了,
     *     这条判据必须显式排除 OS_PAGE_LUA —— 否则 Lua 游戏(数独/木鱼)在无触摸机型上
     *     的 RIGHT 会被悄悄改成 DOWN，表现为"右键不响应/乱跳"。 */
    if (a == OS_ACTION_RIGHT && !input_has_touch() && !bt_manager_is_connected() &&
        m->page_id != OS_PAGE_MAIN && m->page_id != OS_PAGE_LUA && !m->fullscreen) {
        a = OS_ACTION_DOWN;
    }

    /* HOME (长按返回键3s / 手柄第7键): **强制**退出所有界面回桌面 (linit 2026-11-22)。
     *   原行为: 全屏应用会先弹"退出程序?"(10s 超时)。现在 3s 长按定义为"逃生口"——
     *   不管当前是什么状态(全屏应用/弹窗/快捷栏/屏保), 一律直接回桌面, 不再询问。
     *   短按返回仍按各页语义走 (含 exit_confirm), 所以正常退出路径的确认没有丢。
     *   ⚠ 引擎(模拟器)页由 eng_task_run_loop 自己拦 HOME 并退出, 不经过这里。 */
    if (a == OS_ACTION_HOME) {
        os_force_home(ctx);
        return;
    }
    /* V1.2.x: UI 震动 — 非游戏态下, 方向导航 / 确认 / 返回键触发按键反馈.
     * V1.5.x: 归"UI震动"开关 (vibrator_tap_ui), 与"按键模拟"通道分离 —
     * 18ms/100% 短脉冲与触摸点击手感一致; UI震动关 → 导航不震, 不影响触摸点击.
     * 覆盖主菜单/设置/弹窗/应用管家/番茄钟滚轮等所有 UI 导航 (弹窗键也经此处).
     * 游戏态不在此触发 (游戏内按键走 input 层 joypad 边沿游戏震动).
     * ★ 2026-11-24 linit「手柄按左右怎么感觉震动2次」: 主菜单的左右切换**不在这里震** ——
     *   桌面轮播引擎自己按"跨过一个图标"震一次 (r_carousel.c car_poll 的齿轮震动),
     *   两处都震 ⇒ 手柄/键盘按一次左或右连震两下。只排 LEFT/RIGHT 这两个动作,
     *   主菜单上的确认/返回仍走这里 (轮播不给自己震), 上/下本就没有轮播反馈也跟着这里。 */
    bool car_self_vib = (m->page_id == OS_PAGE_MAIN &&
                         (a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT));
    if (!input_is_game_mode() && !car_self_vib && !os_dialog_wheel_open()) {
        /* ★ 2026-11-24: 按键反馈按**语义**分档(不再是所有键都 18ms/100 一模一样):
         *   上下/左右 = 移动一格(tick), 确认 = confirm, 返回 = back(强→弱双击)。
         *   弹窗/设置/列表的键都经过这里, 所以一处改动全体受益。
         *   ⚠ 档位滚轮例外: 它自己按"真变了一格"发 STEP(音量/音调比列表更明显),
         *     这里必须让路, 否则一次按键两次震动(confirm 被冷却拒后回落基础 tap)。 */
        if (a == OS_ACTION_CONFIRM) {
            vibrator_sem(VIB_SEM_CONFIRM);
        } else if (a == OS_ACTION_BACK) {
            vibrator_sem(VIB_SEM_BACK);
        } else if (a == OS_ACTION_UP || a == OS_ACTION_DOWN ||
                   a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT) {
            vibrator_sem(VIB_SEM_TICK);
        }
    }
    /* ① 引擎(分类游戏)按返回 → 退出确认弹窗 (确认退出 / 10秒自动退出 / 返回取消).
     * 门控: input_is_game_mode() 为真 (引擎进 eng_task_run_loop 时置位). 原逻辑保持不动. */
    if (a == OS_ACTION_BACK && m->fullscreen && !m->modal && !s_full_back_dlg
        && input_is_game_mode()) {
        s_full_back_dlg = true;
        os_dialog_confirm_ex(ctx, "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?", /* 退出程序? */
                             10000, 0, on_full_back_confirm, NULL);
        return;
    }
    /* ② ★ 2026-10-14 linit 全局规范: **应用页按硬件返回键也要先弹"退出程序?"确认, 确认才退**.
     * 由模块声明 `.exit_confirm = true` 开启 (应用页默认开)。
     * 不开启的三类排除项见 os.h 的 os_module_t::exit_confirm 注释:
     *   引擎类 / 弹窗类 / 页内 BACK 自带语义或已自建确认 (它们自行调 os_request_exit_confirm)。
     * 引擎跑在游戏态, 上面 ① 已先返回, 不会重复弹。 */
    if (a == OS_ACTION_BACK && m->exit_confirm && !m->modal) {
        os_request_exit_confirm(ctx);
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
        os_dialog_toast_dismiss(ctx);
        ctx->needs_redraw = true;
        return true;
    }
    const os_module_t *m = os_cur_mod();
    if (!m) return false;
    /* ★ 2026-10-29: 全局顶层覆盖服务 (下拉快捷栏) **先于模块**受理。
     *   必须放在模块之前: 全屏游戏/应用自己的 touch 会在下面直接 return true,
     *   轮不到服务层 —— 那样"游戏里下拉不出快捷栏"。
     *   服务自身只在"抽屉已展开"或"点在顶部 200×10 起手区"时消费, 其余返回
     *   false 放行, 所以普通点击/滑动不受影响。 */
    for (int i = 0; i < s_service_count; i++) {
        if (s_services[i]->overlay_top && s_services[i]->touch &&
            s_services[i]->touch(ctx, x, y))
            return true;
    }
    if (m->touch) return m->touch(ctx, x, y);
    /* V1.5.x: 常驻 service 热区兜底 — 当前模块无 touch 且状态栏真的在显示时,
     * 依次询问各 service 是否命中 (状态栏 mini 图标等常驻控件). 命中即消费.
     * ★ 2026-11-23: 判据从 "非全屏" 改成 "状态栏可见" —— 这些热区都画在顶部 24px
     *   状态栏里; 全屏应用或**声明不要状态栏**的 Lua 页上, 那一带属于应用自己的画面,
     *   不能再被常驻图标偷走点击 (否则全屏 Lua 游戏点左上角会莫名弹出修改机菜单)。 */
    if (ctx->statusbar_on && !m->modal) {
        for (int i = 0; i < s_service_count; i++) {
            if (s_services[i]->overlay_top) continue;
            if (s_services[i]->touch && s_services[i]->touch(ctx, x, y))
                return true;
        }
    }
    return false;
}

/* ==== 全屏应用"退出确认"统一入口 (linit 2026-10-14) ====
 * 无线探测 / 网络检测 / 电脑诊断 这类全屏应用: 按硬件返回键先弹"退出程序?", **确认才退出**。
 * 各页只在"真要退出"的那一处调本函数 —— 页内 BACK 的其它语义(关详情/回上一步/回列表)不受影响。
 * ⚠ 不能在弹窗回调里直接 `os_pop` 页面: 回调是在 `dlg_finish_top()` 内部跑的, 此刻栈顶**还是弹窗层**,
 *   直接弹会把弹窗弹掉、应用却留着。所以只置 pending, 由下一帧 `os_tick` 统一弹页面。 */
static bool s_exit_ask_open    = false;
static bool s_exit_ask_pending = false;

static void on_exit_ask(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_exit_ask_open = false;
    if (result == 0) {                      /* 0=确定退出; 1/-1(取消/返回)=留在原页 */
        s_exit_ask_pending = true;
        if (ctx) ctx->needs_redraw = true;
    }
}

/* ★★★ 2026-10-28: 延迟退出一页 —— 供"弹窗里点取消/确定之后要离开当前页"的场景.
 *   不能在弹窗回调里直接 os_pop: 那一刻弹窗页还在栈顶, os_pop 只会弹掉弹窗层,
 *   调用方的页面原地不动 (回调返回后 dlg_finish_top 才真正关弹窗).
 *   所以这里只置标志, 由 os_tick 在"弹窗已关"的下一帧执行.
 *   典型用法: 频谱仪"需占用64KB, 是否启用?"点取消 ⇒ 直接退回桌面,
 *   而不是停在"射频未就绪"的空页上还要用户再按一次返回. */
static bool s_pop_pending = false;
void os_request_pop(ui_ctx_t *ctx) {
    s_pop_pending = true;
    if (ctx) ctx->needs_redraw = true;
}

/* 长按返回 3s (HOME): 强制退出所有界面回桌面 (linit 2026-11-22)。
 * 顺序: 清"挂起中的退出意图/确认框" → 清弹窗栈 → 收快捷栏 → 退屏保 → 回桌面。
 * 为什么必须先清标志: os_dialog_clear_all 是"无回调直清", 而 s_exit_ask_open /
 * s_full_back_dlg 只在弹窗回调里复位 ⇒ 不清就会永久停在 true, 之后按返回再也不弹确认框。 */
static bool s_home_pending = false;   /* 引擎页请求"强制回桌面", 由 os_tick 消费 */

static void os_force_home(ui_ctx_t *ctx)
{
    s_exit_ask_pending = false;
    s_exit_ask_open    = false;
    s_full_back_dlg    = false;
    s_pop_pending      = false;
    s_home_pending     = false;

    if (os_dialog_depth() > 0) os_dialog_clear_all(ctx);
    if (os_quickbar_is_open()) os_quickbar_close();
    if (os_screensaver_active()) os_screensaver_reset();
    os_pop_to_main(ctx);
    if (ctx) ctx->needs_redraw = true;
}

/* 引擎(模拟器)页专用: 请求"收尾完成后强制回桌面"。
 * 为什么不能就地 os_force_home:
 *   eng_task_run_loop 跑在**引擎前台阻塞循环**里, 此刻 ROM/模拟任务还没 stop/unload。
 *   就地清栈会让 page_engine 的 on_exit 先跑一遍(卸引擎), 紧接着调用方 gb_select 等
 *   再 gb_emu_stop()/gb_emu_unload() 又卸一次 —— 重复卸载。走延迟队列后时点刚好落在
 *   "引擎已彻底停掉、主循环恢复"的下一帧, 顺序天然正确。
 *   同一套机制也避免了在 8 个 xx_select() 里各改一遍(必漏)。 */
void os_request_home(ui_ctx_t *ctx)
{
    s_home_pending = true;
    if (ctx) ctx->needs_redraw = true;
}

void os_request_exit_confirm(ui_ctx_t *ctx) {
    if (!ctx) return;
    if (s_exit_ask_open || s_exit_ask_pending) return;   /* 已弹/已排队, 防重复 */
    s_exit_ask_open = true;
    os_dialog_confirm_ex(ctx, "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?", /* 退出程序? */
                         0, -1, on_exit_ask, NULL);
}

void os_tick(ui_ctx_t *ctx)
{
    /* ★★★ 2026-11-22 修复「返回键按下去没反应, 重启才好」的一类根因 ★★★
     *   s_full_back_dlg / s_exit_ask_open 这两个"已弹窗"标志**只在弹窗回调里复位**,
     *   而 os_dialog_clear_all() 是"无回调直清" —— 全仓有 7 处页面在调
     *   (mp3 / gamepad / usb_hid / pomodoro / modtool / perfmon / svc_modtool)。
     *   于是只要弹窗被直清一次(切页 / 页面自己收尾 / HOME), 标志就永久停在 true:
     *     · s_full_back_dlg 卡住 ⇒ 全屏应用里按返回永远进不了 `!s_full_back_dlg` 分支,
     *       "退出程序?"再也不弹, 表现就是**返回键没反应**;
     *     · s_exit_ask_open 卡住 ⇒ os_request_exit_confirm() 第一行就 return, 同理。
     *   两者都只有重启(变量归零)才恢复 —— 与用户报的症状完全一致。
     *   main.c 里 s_pwr_dlg_open 早已用同样办法修过, 这里补齐剩下的两个。
     *   判据用**实际弹窗深度**, 不猜: 深度 0 就说明没有任何弹窗, 标志没有理由为 true。 */
    if (os_dialog_depth() <= 0) {
        if (s_full_back_dlg) {
            s_full_back_dlg = false;
            ESP_LOGW(TAG, "标志对齐: s_full_back_dlg 卡在 true(弹窗已被直清) → 复位, 返回键恢复");
        }
        if (s_exit_ask_open) {
            s_exit_ask_open = false;
            ESP_LOGW(TAG, "标志对齐: s_exit_ask_open 卡在 true(弹窗已被直清) → 复位");
        }
    }

    /* 引擎页请求的"强制回桌面"优先消费: 引擎已在自己那边 break 并 stop/unload 完,
     * 主循环恢复的这一刻才真正清栈 —— 此时 page_engine.on_exit 里再动引擎是安全的。
     * 必须排在下面几个挂起标志之前: 那些是要"退一层", 这里是要"退到底"。 */
    if (s_home_pending) {
        ESP_LOGI(TAG, "强制回桌面 (引擎页长按返回)");
        s_home_pending = false;
        os_force_home(ctx);
        return;
    }

    /* 全屏应用退出确认: 弹窗已关 → 这一帧才真正退页面 (见 os_request_exit_confirm 注释)
     * ★ 2026-11-22: 原来**无条件先清零**再判条件 —— 弹窗还没关掉(或 modal 未退)时,
     *   这次退出请求就被静默丢弃, 用户按了"确定退出"却没退, 需要再按一次。
     *   改成与下面 s_pop_pending 同一语义: 条件满足才消费。 */
    if (s_exit_ask_pending && os_dialog_depth() <= 0 && !ctx->modal_active) {
        s_exit_ask_pending = false;
        os_pop(ctx);
        ctx->needs_redraw = true;
    }
    /* ★ 2026-10-28: 延迟退页 (见 os_request_pop). 必须等弹窗真的关掉再 pop,
     *   否则只弹掉弹窗层, 页面还留在前台。 */
    if (s_pop_pending && os_dialog_depth() <= 0 && !ctx->modal_active) {
        s_pop_pending = false;
        os_pop(ctx);
        ctx->needs_redraw = true;
    }

    /* V1.7.x: 全局 toast 超时清理 — 任意页面/弹窗上提示 1s 自动关+重绘 (不依赖 DIALOG 页 poll) */
    os_dialog_toast_tick(ctx);

    /* ★★★ 2026-11-01 兜底: 「弹窗栈已空, 页面却还停在 DIALOG」= 残留空白弹窗页。
     *   成因: `os_dialog_toast_ms()` 在 s_top<0 时会 os_push(OS_PAGE_DIALOG) 自开一页,
     *         而 `os_dialog_pop()` 在 s_top<0 时**什么都不做** ⇒ 这一页关不掉。
     *   后果(真机实测): 残留页挡在上面, 用户按返回时被 os_pop 的是**下层真页面**
     *         (应用中心 → 直接回主菜单 → on_exit 复位联网会话标志 → 再进商店又弹确认)。
     *         page_gamepad.c 早就手工兜过同一个坑("清掉独立 toast 残留的弹窗页")。
     *   这里做成全局兜底: 栈空 + 无 toast + 当前页是 DIALOG ⇒ 一定是残留, 收掉。
     *   风险: 极低 —— 正常弹窗一定 s_top>=0; 正常 toast 一定 toast_active()。 */
    if (ctx->current_page == OS_PAGE_DIALOG && os_dialog_depth() <= 0 && !os_dialog_toast_active()) {
        ESP_LOGW(TAG, "残留空白弹窗页(弹窗栈已空) → 自动收掉, 避免误弹下层页面");
        os_pop(ctx);
        ctx->needs_redraw = true;
    }

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
    /* V1.1.0: 已删除"跟手"指示条 (用户明确不要黑线), 不再需要底部上滑进行中置脏. */
    /* 屏保全屏占据时跳过页面轮询 (避免残留拖拽状态被触屏 poll 触发) */
    if (os_screensaver_active()) return;
    const os_module_t *m = os_cur_mod();
    if (m && m->poll) m->poll(ctx);
}


/* ==== 底部上滑返回 ====
 * 注: 历史上曾有一根"跟手"黑色指示横条 (draw_back_gesture_hint), 用户明确表示不需要显示,
 * 已于 2026-09-20 删除 —— 上滑返回手势本身保留, 仅不再画提示条. */

/* === 性能统计 (2026-10-24): 给 remcon fps 用 === */
static uint32_t s_frames_total = 0;
static uint64_t s_acc_us = 0;
static uint32_t s_acc_n = 0;
static uint32_t s_avg_us = 0;
static uint32_t s_last_draw_us = 0;
static uint32_t s_last_flush_us = 0;
/* 帧间隔抖动: 平均耗时好看但偶发 40ms 卡顿最影响"顺不顺"的手感,
 * 所以额外记最大间隔 + 超过 30ms 的帧数(掉帧次数)。 */
static uint32_t s_max_gap_us = 0;
static uint32_t s_slow_frames = 0;
static uint32_t s_last_frame_end_us = 0;

void os_get_frame_gap(uint32_t *max_gap_us, uint32_t *slow_frames, uint32_t *last_frame_us)
{
    if (max_gap_us)   *max_gap_us = s_max_gap_us;
    if (slow_frames)  *slow_frames = s_slow_frames;
    if (last_frame_us) *last_frame_us = s_last_draw_us + s_last_flush_us;
}
void os_reset_frame_gap(void) { s_max_gap_us = 0; s_slow_frames = 0; s_last_frame_end_us = 0; }

void os_get_render_stats(uint32_t *frames, uint32_t *avg_us, uint32_t *last_draw_us, uint32_t *last_flush_us)
{
    if (frames)        *frames = s_frames_total;
    if (avg_us)        *avg_us = s_avg_us;
    if (last_draw_us)  *last_draw_us = s_last_draw_us;
    if (last_flush_us) *last_flush_us = s_last_flush_us;
}

/* ★★★ 2026-10-28: 作废"已送屏背景" —— 下一帧先整屏清白, 再重绘最底层非模态页,
 *   然后才叠当前页/弹窗.
 *   为什么需要它: dlg_render 是**只画弹窗自己那块**, 不清屏、不重绘背景(注释见
 *   os_dialog.c "弹窗不清屏: 弹窗之外保留原背景"). 所以一旦背景被"旁路刷屏"盖过
 *   —— 屏保壁纸独占刷屏、全屏模态页(赞助二维码)、引擎直刷视频帧 ——
 *   弹窗叠上去时底下留的就是那幅残影, 看着像"弹窗浮在壁纸上/花屏".
 *   调用点: 屏保态长按关机 → 退出屏保前调用一次.
 *   ⚠ 副作用是正面的: 整屏清白 + 重绘 = 全屏刷新, 正好满足"弹窗首次出现必须整屏刷". */
void os_invalidate_bg(void) { s_bg_dirty = true; }

/* ★ 2026-11-23: 请求下一帧重绘 (幂等)。供全局服务/内核对"屏幕内容已变"表态,
 *   不持有 ctx 的调用点(如状态栏显隐接口)用它, 不必各自去摸 ctx。 */
void os_request_redraw(void)
{
    if (s_os_ctx) s_os_ctx->needs_redraw = true;
}

void os_render(ui_ctx_t *ctx)
{
    /* 屏保: 渲染交给独立监视任务(强制最高级, 不被主循环/游戏阻塞), 此处仅保持
     * 帧持续(needs_redraw), 不再在此刷屏, 避免与监视任务双任务刷屏竞争 */
    static bool     s_prev_ss = false;
    static uint32_t s_wake_ms = 0;
    bool ss_now = os_screensaver_active();
    if (ss_now) { ctx->needs_redraw = true; s_prev_ss = true; return; }
    uint32_t nowms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_prev_ss) { s_wake_ms = nowms; s_prev_ss = false; }
    /* ★ 唤醒后**持续**强制重绘 1 秒, 不再只靠"退出那一帧"的边沿。
     *   原因: 屏保监督任务在另一核(52ms 一帧), 它那一帧可能落在窗口之后 ——
     *   那一帧会把屏幕刷成壁纸, 而此时 needs_redraw 已是 false ⇒ 没人再画,
     *   屏幕就停在"最后一张壁纸", 必须再摸一下才回到菜单。
     *   窗口从 200ms 提到 1000ms: 覆盖监督任务最长的一个整轮(含它 250ms 的空闲分支)。
     *   代价只是唤醒后多画 1 秒(同一画面, 指纹相同会被跳过送屏)。 */
    bool woke_up = (s_wake_ms != 0) && ((uint32_t)(nowms - s_wake_ms) < 1000u);
    if (!ctx->needs_redraw && !woke_up) return;

    /* ★★★ 2026-10-25 linit「主界面不够流畅，保持 16fps 但要丝滑」:
     *   帧率上限**放在这里**做"跳过"，不在 st7305_flush 里 vTaskDelay 等待。
     *   以前的等待会阻塞主循环最长 62ms —— 滑动时输入处理和下一帧都被推迟，手感发滞。
     *   现在: 距上一帧不足 62ms 就本轮不画(needs_redraw 保持 true, 下一轮必画), 
     *   输入/滚动每轮都在跑 ⇒ 16fps 匀速, 但操作跟手。
     *   屏保/唤醒那 1 秒的强制重绘不受限(帧内容相同, 送屏会被忽略)。 */
    /* ★ 2026-10-25 linit「非常卡 不要限制了」⇒ 这里不再按 16fps 跳过。
     *   UI 本来就是"有变化才重绘", 自己就会限流; 再硬性跳过反而丢帧、显得卡。
     *   (驱动层同样已解除限速。) */
    if (!ctx->lcd) return;

    const os_module_t *m = os_cur_mod();

    /* ★ 2026-11-22 linit「设置里切字体, 只改主菜单 / 游戏选择引擎菜单 / 阅读器,
     *   其他内置应用和 Lua 应用字体不要修改」+ 后续补充「设置里的弹窗要一起改」。
     *
     *   在**整帧起点**按当前页面设一次作用域, 而不是散在各页面里各判一次 ——
     *   同一判据只存在这一处, 新增页面默认落在"不改"那一边 (安全侧)。
     *
     *   ⚠ 弹窗有个坑: os_dialog_push 的**首层**会 os_push(OS_PAGE_DIALOG), 所以
     *     "设置里弹出的对话框" current_page 是 DIALOG 而不是 SETTINGS ——
     *     直接按 current_page 判会把所有弹窗都判成"非白名单", 字体永远不跟随。
     *     这里取返回栈顶的**归属页面**来判: 设置的弹窗父页是 SETTINGS(跟随),
     *     MP3 的弹窗父页是 MP3(不跟随), 各归各的。 */
    {
        os_page_t pg = (os_page_t)os_current_page();
        if (pg == OS_PAGE_DIALOG && s_stack_top >= 0 && s_stack_top < OS_STACK_MAX)
            pg = s_back_stack[s_stack_top];          /* 弹窗 → 用父页面判定 */
        bool follow = (pg == OS_PAGE_MAIN          /* 主菜单 (含车轱辘应用名) */
                    || pg == OS_PAGE_SELECT_GAME   /* 游戏选择 */
                    || pg == OS_PAGE_BOOK          /* 阅读器 */
                    || pg == OS_PAGE_SETTINGS);    /* 设置 (含其全部弹窗) */
        /* ★ 2026-11-24: 白名单里的 OS_PAGE_GAME_SETTINGS 已随该废弃 id 一并删除 ——
         *   那个页面从来没有模块注册, 判据永远不会命中 (引擎内设置现在是弹窗, 归属页是引擎页,
         *   本来就走"不跟随"), 删除是纯清理, 不改变任何页面的字体行为。 */
        glyph_set_family_scope(follow);
        /* 只在**判定结果变化**时打一行 —— 用来确证"哪个页面跟随/不跟随",
         * 不逐帧刷屏。排字体问题时这是唯一能证明作用域真的切对了的证据。 */
        static int s_scope_logged = -1;
        if ((int)follow != s_scope_logged) {
            s_scope_logged = (int)follow;
            ESP_LOGI(TAG, "FONTSCOPE cur=%d 归属页=%d follow=%d",
                     (int)os_current_page(), (int)pg, (int)follow);
        }
    }

    /* V1.7.x: 帧耗时体检起点 (含 flush) —— ≥50ms 就是肉眼可感的"卡/打开慢" */
    uint64_t t_frame0 = (uint64_t)esp_timer_get_time();

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
    /* 状态栏: 只在"全屏"时隐藏 (游戏/键鼠/终端等全屏应用及其退出确认框).
     * 普通页面上的 modal 弹窗(存储/设置菜单等)不隐藏状态栏 —— 弹窗是 350×250 居中,
     * 与顶部 24px 状态条不重叠, 保留系统栏信息(时间/电量)更符合预期.
     * 注: "弹窗叠在全屏页之上"的情况已由上面的 pm->fullscreen 判定置 fullscreen=true 覆盖. */
    /* ★ 2026-11-23 linit 状态栏统一整改: 显隐不再只看 fullscreen ——
     *   应用可以用 os_statusbar_show(true/false) **明确声明**本页要不要 (声明优先),
     *   没声明才按页面属性(全屏=隐藏)推。判据在 statusbar_policy.h (纯逻辑, 主机可测)。
     *   声明黏在"声明时所在页", 换页自动失效 ⇒ 应用退出不必还原, 不会残留到下一页。 */
    bool sb_show = os_sb_should_show(os_statusbar_page_req(), fullscreen);
    /* ★★★ 2026-11-23 linit (第2轮): 弹窗**不再**改变状态栏显隐 ★★★
     *   上一轮这里写的是 `if (ctx->modal_active) sb_show = false;` —— "有 modal 就一律不画"。
     *   实际观感是: 同一个页面上, 弹窗弹出 ⇒ 顶部状态栏消失, 弹窗关掉 ⇒ 又冒出来,
     *   在二级弹窗(设置项/列表详情)反复开合时就是用户说的"一会显示一会不显示", 像坏了。
     *
     *   现在回归"弹窗只负责弹窗": 状态栏完全沿用**背景页**的判定 ——
     *     · 背景页本来有状态栏(普通页 / 显式 os_statusbar_show(true)) → 继续画、继续刷新;
     *     · 背景页本来没有(全屏页, 或全屏页上的退出确认框) → 不画, 也不强行画。
     *   "全屏页上的弹窗"在 816-820 已由 pm->fullscreen 置 fullscreen=true, 所以这里
     *   只用 fullscreen 判定就够了; os_statusbar_page_req() 在 DIALOG 期间也已沿用到
     *   最近一个非弹窗页(sb_effective_page), 声明不会因为弹窗而失效。 */
    ctx->statusbar_on = sb_show;
    /* ★ 滚动期间不重画状态栏: 时间/电量/信号都没变，每帧重画纯属浪费。 */
    extern bool os_dialog_scrolling(void);
    if (sb_show && !os_dialog_scrolling()) {
        /* 状态栏显隐状态机日志: 仅在显↔隐翻转时打一行, 供排查"状态栏消失"
         * (区分全屏门控的正常隐藏 vs 异常隐藏). */
        static bool s_sb_prev = true;
        if (sb_show != s_sb_prev) {
            ESP_LOGI(TAG, "\xe7\x8a\xb6\xe6\x80\x81\xe6\xa0\x8f%s (fs=%d modal=%d page=%d \xe6\xa0\x88\xe6\xb7\xb1=%d \xe5\xbc\xb9\xe7\xaa\x97\xe5\xb1\x82=%d)", /* 状态栏显示/隐藏 */
                     sb_show ? "\xe6\x98\xbe\xe7\xa4\xba" : "\xe9\x9a\x90\xe8\x97\x8f",
                     (int)fullscreen, (int)ctx->modal_active, ctx->current_page,
                     s_stack_top, os_dialog_depth());
            s_sb_prev = sb_show;
        }
        for (int i = 0; i < s_service_count; i++)
            if (!s_services[i]->overlay_top && s_services[i]->render)
                s_services[i]->render(ctx);
    }
    /* else: 本帧不画状态栏 (全屏页 / 应用声明隐藏 / 滚动中)。
     * 恢复显示时状态栏服务会**无条件整条重画**, 所以这里不需要额外作废缓存。 */
    /* ★ 2026-10-29: overlay_top 服务 (下拉快捷栏) —— 不受 fullscreen / 滚动门控,
     *   恒画在模块 render 之上。是否真的落笔由服务自己判断(收起时什么也不画),
     *   所以普通页面没有额外开销。 */
    for (int i = 0; i < s_service_count; i++) {
        if (s_services[i]->overlay_top && s_services[i]->render)
            s_services[i]->render(ctx);
    }
    /* 提示弹窗恒最顶: 所有模块/服务渲染之后, 把 toast 覆盖在最上层再整体 flush */
    os_dialog_draw_overlay(ctx);
    /* LCD 写者协商:
     * - 非引擎态: os_render 整体冲刷.
     * - 引擎(game)态无覆盖: 让权给 video task/引擎(各自 flush 帧), 避免双写同一 SPI.
     * - 引擎态有弹窗/toast 覆盖: 必须由 os_render flush 一次, 否则覆盖层画进 fb 却送不上屏(不可见). */
    bool overlay_active = os_dialog_toast_active() || ctx->modal_active || os_quickbar_is_open();
    uint64_t t_flush0 = (uint64_t)esp_timer_get_time();   /* flush 段计时起点 */
    /* 屏保刚退出(woke_up)强制 flush: 覆盖 game 模式"让权给引擎自行刷屏"的空档,
     * 否则恢复画面停在旧帧(表现为"按解锁没进桌面/没刷新") */
    if (!input_is_game_mode() || overlay_active || woke_up) st7305_flush(ctx->lcd);
    {
        uint64_t t_now = (uint64_t)esp_timer_get_time();
        uint64_t d_frame = t_now - t_frame0;
        uint64_t d_flush = t_now - t_flush0;
        /* 统计 (供 remcon fps 查看真实帧率/帧耗时) */
        if (s_last_frame_end_us) {
            uint32_t gap = (uint32_t)(t_now - s_last_frame_end_us);
            if (gap > s_max_gap_us) s_max_gap_us = gap;
            if (gap > 30000) s_slow_frames++;
        }
        s_last_frame_end_us = (uint32_t)t_now;
        s_frames_total++;
        s_acc_us += d_frame;
        s_last_draw_us = d_frame - d_flush;
        s_last_flush_us = d_flush;
        if (++s_acc_n >= 64) {
            s_avg_us = (uint32_t)(s_acc_us / s_acc_n);
            s_acc_us = 0; s_acc_n = 0;
        }
        if (d_frame >= 20000)   /* 临时: 20ms 以上都打, 用于定位弹窗拖动卡 */
            ESP_LOGD(TAG, "render 慢: 总 %llu us = 绘制 %llu + flush %llu (page=%d %s)",
                     d_frame, d_frame - d_flush, d_flush, ctx->current_page, m ? m->name : "-");
    }
    ctx->needs_redraw = false;
}

bool os_is_main(const ui_ctx_t *ctx)
{
    return ctx->current_page == OS_PAGE_MAIN;
}

/* 页面模块是否已注册 —— 供开机自检: 应用表里有 app 但页面没注册时, 点了会"毫无反应" */
bool os_page_registered(int page)
{
    return page >= 0 && page < OS_PAGE_COUNT && s_module_set[page];
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