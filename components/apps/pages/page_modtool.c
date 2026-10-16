/**
 * page_modtool.c — 修改机 独立入口 (标准确认弹窗).
 *
 * 修改机是独立插件 (cheat_float + cheat_ui, 见 components/cheater)。
 * 本页面只负责"从 主菜单/应用管理/状态栏 mini 图标 打开修改机"的入口确认:
 *   - 打开时弹标准小确认框 (os_dialog_push, 复用 os_dialog 的小确认模板):
 *      标题「打开修改机？」，左「退出」右「后台」两键.
 *        「后台」= 后台运行, 开启修改机会话 (游戏中显示浮标 + SELECT+START 热键可用);
 *        「退出」= 不打开/结束会话.
 *   - 标准弹窗自动遵循"双方案": 触屏不加黑 + 手柄加黑 (input_scheme_touch), 触屏优先.
 *   - 真正的搜索/冻结/改值 UI 由独立插件在游戏内承担, 与本页无关.
 */
#include "os.h"
#include "os_internal.h"   /* os_page_*_register 声明环境 */
#include "os_modtool.h"
#include "esp_log.h"
#include <string.h>

#define MT_TAG "MODTOOL"

/* 按钮文字: 左「退出」 右「后台」 */
#define MT_S0   "\xe9\x80\x80\xe5\x87\xba"   /* 退出 */
#define MT_S1   "\xe5\x90\x8e\xe5\x8f\xb0"   /* 后台 */

/* ============ 会话动作 ============ */
static void mt_do_bg(ui_ctx_t *ctx)     /* 后台: 保持/开启会话, 关闭入口返回 */
{
    os_modtool_set_running(true);
    os_dialog_clear_all(ctx);
    os_pop(ctx);
}
static void mt_do_exit(ui_ctx_t *ctx)   /* 退出: 结束修改机会话 */
{
    os_modtool_set_running(false);
    os_dialog_clear_all(ctx);
    os_pop(ctx);
}

/* 标准确认框回调: result 0=退出(左) / 1=后台(右) / -1=取消(默认后台) */
static void mt_confirm_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result == 0) mt_do_exit(ctx);
    else             mt_do_bg(ctx);   /* 1 或 -1 → 后台运行(默认/非破坏) */
}

/* ============ 页面生命周期 ============ */
static void mt_enter(ui_ctx_t *ctx)
{
    /* 打开修改机 → 标准小确认框 (双方案: 触屏不加黑/手柄加黑, 触屏优先) */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "\xe6\x89\x93\xe5\xbc\x80\xe4\xbf\xae\xe6\x94\xb9\xe6\x9c\xba\xef\xbc\x9f"); /* 打开修改机？ */
    dlg.count = 2;
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s", MT_S0);   /* 退出 */
    snprintf(dlg.items[1], sizeof(dlg.items[1]), "%s", MT_S1);   /* 后台 */
    dlg.sel = 1;              /* 默认后台 (手柄高亮; 触屏不显示高亮) */
    dlg.no_footer = true;     /* 确认框 (is_confirm=true) */
    dlg.small     = true;     /* 小自适应确认模板 */
    dlg.cb        = mt_confirm_cb;
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(MT_TAG, "修改机入口: 标准确认框 (退出/后台)");
    ctx->needs_redraw = true;
}

static void mt_render(ui_ctx_t *ctx) { (void)ctx; }   /* 内容全在标准弹窗里 */

static void mt_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_BACK) mt_do_bg(ctx);   /* 兜底: 页面若暴露, BACK 即后台运行 */
}

static bool mt_touch(ui_ctx_t *ctx, int x, int y) { (void)ctx; (void)x; (void)y; return false; }

static void mt_exit(ui_ctx_t *ctx)
{
    ESP_LOGI(MT_TAG, "修改机页面退出 (会话 running=%d)", os_modtool_is_running());
    os_dialog_clear_all(ctx);
    ctx->needs_redraw = true;
}

static const os_module_t s_mod_modtool = {
    .name       = "modtool",
    .page_id    = OS_PAGE_MODTOOL,
    .on_enter   = mt_enter,
    .on_exit    = mt_exit,
    .render     = mt_render,
    .action     = mt_action,
    .touch      = mt_touch,
    .fullscreen = true,
};

void os_page_modtool_register(void)
{
    os_register(&s_mod_modtool);
}