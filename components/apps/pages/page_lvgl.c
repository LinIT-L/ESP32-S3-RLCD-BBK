/**
 * page_lvgl.c — LVGL 移植层自检页 (演示 + 回归用).
 *
 * 目的: 在真机上用最少的代码验证 "LVGL 能不能在这块 400×300 单色反射屏上正常跑"。
 * 内容: 一屏 LVGL 控件 —— 标题 Label / 进度条(自动动画) / 圆弧 / 列表 / 开关,
 *       外加一个 500ms 的 lv_timer 让进度条来回走, 用于肉眼确认动画是否推进。
 *
 * ⚠ 屏幕归属: 本页是 fullscreen 模块, 打开期间**屏幕归 LVGL 所有** ——
 *   本页 render() 故意什么都不画 (画了会和 LVGL 抢 framebuffer)。
 *   LVGL 的刷新由 lvgl_port 的 flush 回调直接写 ST7305 并 st7305_flush()。
 *   为防止 OS 侧渲染把屏幕清掉, 这里在 **LVGL 任务上下文**里挂一个 lv_timer
 *   周期性 invalidate 整屏 (不能在 OS 的 render/poll 里直接调 lv_obj_* —— 跨任务访问
 *   LVGL 对象不是线程安全的)。
 *
 * 退出: BACK / HOME 键, 或触摸左上角"返回"钮。
 */
#include <stdio.h>
#include <string.h>

#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "esp_log.h"

#define TAG "LVGLDEMO"

static lv_obj_t *s_bar;
static lv_obj_t *s_arc;
static lv_timer_t *s_inval;
static int       s_dir = 1;
static int       s_val;

/* 500ms 心跳: 进度条来回走 + 强制整屏重绘 (都在 LVGL 任务里执行) */
static void demo_tick(lv_timer_t *t)
{
    (void)t;
    s_val += s_dir * 10;
    if (s_val >= 100) { s_val = 100; s_dir = -1; }
    if (s_val <= 0)   { s_val = 0;   s_dir =  1; }
    if (s_bar) lv_bar_set_value(s_bar, s_val, LV_ANIM_ON);
    if (s_arc) lv_arc_set_value(s_arc, s_val);
    lv_obj_invalidate(lv_screen_active());
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL " LVGL_VERSION_INFO "  400x300  1bpp");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "port ok - widgets: label/bar/arc/list/switch");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 8, 28);

    /* 进度条 */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 240, 18);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 8, 56);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    /* 圆弧 */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 90, 90);
    lv_obj_align(s_arc, LV_ALIGN_TOP_RIGHT, -8, 44);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    /* 列表 */
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, 230, 130);
    lv_obj_align(list, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_list_add_button(list, NULL, "List item A");
    lv_list_add_button(list, NULL, "List item B");
    lv_list_add_button(list, NULL, "List item C");

    /* 开关 */
    lv_obj_t *sw = lv_switch_create(scr);
    lv_obj_align(sw, LV_ALIGN_BOTTOM_RIGHT, -90, -30);
    lv_obj_add_state(sw, LV_STATE_CHECKED);

    lv_obj_t *lbt = lv_label_create(scr);
    lv_label_set_text(lbt, "BACK to exit");
    lv_obj_align(lbt, LV_ALIGN_BOTTOM_RIGHT, -8, -8);

    s_inval = lv_timer_create(demo_tick, 500, NULL);
}

static void lv_enter(ui_ctx_t *ctx)
{
    if (!ctx || !ctx->lcd) { os_pop(ctx); return; }
    esp_err_t e = lvgl_port_init(ctx->lcd);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init 失败: %s", esp_err_to_name(e));
        os_pop(ctx);
        return;
    }
    build_ui();
    ESP_LOGI(TAG, "LVGL 演示页已进入");
}

static void lv_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_inval) { lv_timer_delete(s_inval); s_inval = NULL; }
    lv_obj_clean(lv_screen_active());
    ESP_LOGI(TAG, "LVGL 演示页已退出");
}

/* 故意不画: 本页期间屏幕归 LVGL 所有 */
static void lv_render(ui_ctx_t *ctx) { (void)ctx; }
static void lv_poll(ui_ctx_t *ctx)   { (void)ctx; }

static bool lv_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 左上角一小块作"返回"热区, 其余交给 LVGL 的 pointer indev 处理 */
    if (x < 90 && y < 40) { os_pop(ctx); return true; }
    return true;
}

static void lv_action(ui_ctx_t *ctx, os_action_t action)
{
    if (action == OS_ACTION_BACK || action == OS_ACTION_HOME) os_pop(ctx);
}

static const os_module_t s_mod_lvgl = {
    .name = "lvgl", .page_id = OS_PAGE_LVGL_DEMO,
    .on_enter = lv_enter, .on_exit = lv_exit,
    .render = lv_render, .action = lv_action, .touch = lv_touch, .poll = lv_poll,
    .fullscreen = true,
};

void os_page_lvgl_register(void) { os_register(&s_mod_lvgl); }
