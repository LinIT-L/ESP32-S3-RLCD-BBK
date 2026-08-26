/**
 * os_pages.c — OS 页面装配层.
 *
 * 集中登记所有内建模块(页面)与全局服务。
 * 迁移规则: 每新增/迁移一个页面, 在此一行注册即可; 页面私有实现留在各自 *_module.c。
 *
 * 注意: 为避免 os 组件与 menu 组件循环依赖, 具体页面模块ry用 "os" 之外的
 * 界面绘制原语(ui_common)直接 include 由其提供。
 */
#include "os.h"
#include "os_internal.h"

/* ========== 页面模块定义 ========== */

/* ---- P1 起迁入: 主菜单(桌面) ---- */
static void m_main_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    /* 占位: 提示当前为 OS 骨架桌面 (P1 迁移 real main_menu) */
    st7305_draw_text(lcd, 120, 140, "LinTOS OS skeleton");
    ctx->needs_redraw = false;
}
static void m_main_action(ui_ctx_t *ctx, os_action_t a)
{
    (void)ctx; (void)a;
    /* P1 迁移: 主菜单方向/确认进入各应用页 */
}
static const os_module_t s_mod_main = {
    .name = "main",
    .page_id = OS_PAGE_MAIN,
    .render = m_main_render,
    .action = m_main_action,
    .fullscreen = false,
};

/* ========== 全局服务 ========== */

/* ---- 状态栏: 实现见 services/svc_statusbar.c ---- */

/* ========== 装配 ========== */
void os_register_all_internal(void)
{
    os_register(&s_mod_main);
    os_svc_statusbar_register();
}