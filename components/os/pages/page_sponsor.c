/**
 * page_sponsor.c — 全屏弹窗 (第5类弹窗模板).
 *
 * 全屏显示收款码 (300x300 1bpp, 从 3.3 版提取), BACK 退出回上一级.
 * modal=true: 覆盖在设置弹窗之上, BACK 直接 os_pop (不走全屏退出确认).
 */
#include "os.h"
#include "ui_common.h"
#include "sponsor_qr.inc"   /* 收款码位图: QR_W/H + s_qr_bitmap */
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "SPONSOR"

static void p_sponsor_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    /* 全屏 1:1 显示收款码 (与旧版一致: 400x300 屏, 300x300 居中 (50,0)) */
    st7305_draw_bitmap_1bit(lcd, (UI_SCREEN_W - QR_W) / 2,
                            (UI_SCREEN_H - QR_H) / 2, QR_W, QR_H, s_qr_bitmap);
}

static void sponsor_tap(ui_ctx_t *ctx);   /* fwd: 5 连点切换 */

static void p_sponsor_action(ui_ctx_t *ctx, os_action_t a) {
    if (a == OS_ACTION_BACK) os_pop(ctx);                /* 返回上一级 (设置) */
    else if (a == OS_ACTION_CONFIRM) sponsor_tap(ctx);   /* 实体/手柄 确认键 也计入 5 连点 (与触屏一致) */
}

/* V1.1.1 彩蛋: 收款码页连点 5 下 → 切换"显示隐藏应用" (四个引擎 + 应用管理) */
static void sponsor_tap(ui_ctx_t *ctx) {
    static int s_taps = 0;
    static uint32_t s_last_ms = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_last_ms && (int32_t)(now - s_last_ms) > 3000) s_taps = 0;   /* 3s 无连点清零 */
    s_last_ms = now;
    s_taps++;
    if (s_taps >= 5) {
        s_taps = 0;
        os_app_toggle_hidden_visible();
        ESP_LOGI(TAG, "5连点 → 隐藏应用: %s", os_app_hidden_visible() ? "显示" : "隐藏");
        os_dialog_toast(ctx, os_app_hidden_visible()
                        ? "\xe5\xb7\xb2\xe6\x98\xbe\xe7\xa4\xba\xe9\x9a\x90\xe8\x97\x8f\xe6\xa8\xa1\xe5\x9d\x97" /* 已显示隐藏模块 */
                        : "\xe5\xb7\xb2\xe9\x9a\x90\xe8\x97\x8f\xe5\xaf\xb9\xe5\xba\x94\xe6\xa8\xa1\xe5\x9d\x97"); /* 已隐藏对应模块 */
    }
}

static bool p_sponsor_touch(ui_ctx_t *ctx, int x, int y) {
    (void)x; (void)y;
    sponsor_tap(ctx);
    return true;   /* 消费全部触摸, 避免误触发其他 */
}

static const os_module_t s_mod_sponsor = {
    .name       = "sponsor",
    .page_id    = OS_PAGE_SPONSOR,
    .render     = p_sponsor_render,
    .action     = p_sponsor_action,
    .touch      = p_sponsor_touch,
    .fullscreen = true,
    .modal      = true,   /* 模态覆盖: BACK 直接返回, 不触发全屏退出确认 */
};

void os_page_sponsor_register(void) { os_register(&s_mod_sponsor); }
