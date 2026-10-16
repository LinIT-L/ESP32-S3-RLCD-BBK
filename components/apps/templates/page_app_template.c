/**
 * page_@@ID@@.c — @@LABEL@@ 应用页
 * 由 tools/new_app.py 生成 (V1.2 应用脚手架). 可直接编译运行, 再按需填充.
 *
 * 契约速查 (详见 docs/App制作规则.md):
 *   render(ctx)        整页渲染 (页面非 fullscreen 时, 状态栏由内核画)
 *   action(ctx, a)     按键/手柄动作 (OS_ACTION_*) —— 返回用 os_pop(ctx)
 *   touch(ctx,x,y)     触摸命中: 返回 true 表示已消费该次点击
 *   on_enter / on_exit 进入/退出: 分配与释放资源 (内存/文件/外设)
 *   poll(ctx)          每帧后台轮询 (可选, 不需要就留空)
 *
 * 依赖纪律: 只允许 #include "os_app_api.h" (应用层唯一 API 面);
 *          禁止直接 include 驱动/服务头 (st7305.h / bt_manager.h / board_battery.h …).
 */
#include "os_app_api.h"
#include "esp_log.h"

#define TAG "@@TAG@@"

/* ---------------- 页面私有状态 (static, 不出本文件) ---------------- */
static int s_page_tick = 0;      /* 示例: 简单状态 */

/* ---------------- 生命周期 ---------------- */
static void @@ID@@_on_enter(ui_ctx_t *ctx)
{
    (void)ctx;
    s_page_tick = 0;
    ESP_LOGI(TAG, "enter @@LABEL@@");
}

static void @@ID@@_on_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    /* 若有分配的内存/打开的文件/占用的外设, 必须在此释放 */
    ESP_LOGI(TAG, "exit @@LABEL@@");
}

/* ---------------- 渲染 ---------------- */
static void @@ID@@_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);          /* 1bpp: 白底 */

    /* 标题 (中文用 \xNN UTF-8 转义, 避免源文件编码问题) */
    ui_text16(lcd, 8, 32, "@@LABEL@@", false);

    /* 示例: 画一个框 + 一行提示 */
    draw_rect_outline(lcd, 8, 56, 392, 200, ST7305_COLOR_BLACK);
    ui_text16(lcd, 16, 72, "\xe6\x8c\x89 BACK \xe8\xbf\x94\xe5\x9b\x9e", false);  /* 按 BACK 返回 */
}

/* ---------------- 按键 ---------------- */
static void @@ID@@_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_BACK:    /* 硬件返回 / 底部上滑 */
        os_pop(ctx);
        break;
    case OS_ACTION_CONFIRM: /* 确认键 / 触摸点按 */
        s_page_tick++;
        ctx->needs_redraw = true;
        break;
    default:
        break;
    }
}

/* ---------------- 触摸命中 ---------------- */
static bool @@ID@@_touch(ui_ctx_t *ctx, int x, int y)
{
    (void)ctx;
    /* 返回 true = 已消费 (内核不再把它当"空白点击"); 未命中返回 false */
    if (x >= 8 && x <= 392 && y >= 56 && y <= 200) {
        ctx->needs_redraw = true;
        return true;
    }
    return false;
}

/* ---------------- 每帧轮询 (不需要可删) ---------------- */
static void @@ID@@_poll(ui_ctx_t *ctx)
{
    (void)ctx;
}

/* ---------------- 模块描述 + 注册入口 ---------------- */
static const os_module_t s_mod_@@ID@@ = {
    .name      = "@@LABEL@@",
    .page_id   = OS_PAGE_@@UPID@@,
    .on_enter  = @@ID@@_on_enter,
    .on_exit   = @@ID@@_on_exit,
    .render    = @@ID@@_render,
    .action    = @@ID@@_action,
    .touch     = @@ID@@_touch,
    .poll      = @@ID@@_poll,
    .modal     = false,
    .fullscreen = false,      /* true = 隐藏系统状态栏 (全屏绘图/游戏类) */
};

void os_page_@@ID@@_register(void)
{
    os_register(&s_mod_@@ID@@);
}
