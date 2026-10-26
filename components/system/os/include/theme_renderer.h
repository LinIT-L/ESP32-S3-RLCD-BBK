/**
 * theme_renderer.h — 桌面主题引擎接口.
 *
 * 分工 (P0 起):
 *   · 引擎 Renderer —— 内置的 C 代码, 负责"桌面整个画面怎么组织 + 怎么交互":
 *     背景 / 装饰 / 图标区 / 文字层 / 底栏指示 五层全归它画, 拖动·惯性·选中·按键也归它。
 *   · 宿主 Host —— 由 page_main 提供: 应用清单(数量/标签/图标下标) + 启动应用。
 *     引擎**不直接碰** os_app / NVS, 只通过 host 取数据, 这样引擎才能被整体替换。
 *
 * ⛔ 引擎只画桌面区域: 状态栏由 svc_statusbar 独立绘制, 不归主题;
 *    应用中心/设置/弹窗也都不受主题影响。
 * ⛔ icon_idx 是 main_icons[] 下标 = ABI, 引擎只能读来画, 绝不能改顺序。
 */
#ifndef THEME_RENDERER_H
#define THEME_RENDERER_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 桌面数据/动作提供方 (page_main 实现) */
typedef struct {
    int         (*count)(void);                 /* 可见应用数 */
    const char *(*label)(int idx);              /* 应用名 (UTF-8) */
    int         (*icon_idx)(int idx);           /* main_icons[] 下标 */
    void        (*launch)(ui_ctx_t *ctx, int idx);  /* 启动第 idx 个应用 */
} theme_host_t;

/* 主题引擎 (每种布局一个, 全部是内置 C 代码) */
typedef struct theme_renderer {
    const char *id;                                       /* "carousel" / "grid" / ... */

    /* 可选: 进入/退出桌面时的一次性初始化 (相机归位、清缓存等) */
    void (*enter)(ui_ctx_t *ctx);
    void (*exit)(ui_ctx_t *ctx);

    /* 必填: 整屏绘制 (只画桌面区域, 不画状态栏) */
    void (*render)(ui_ctx_t *ctx);

    /* 必填: 每帧轮询 (拖动/惯性/震动节奏), 仅当没有弹窗覆盖时调用 */
    void (*poll)(ui_ctx_t *ctx);

    /* 必填: 按键 / 触摸; 返回 true=已消费 */
    bool (*action)(ui_ctx_t *ctx, os_action_t a);
    bool (*touch)(ui_ctx_t *ctx, int x, int y);
} theme_renderer_t;

/* 绑定宿主与引擎 (page_main 在 on_enter 调一次, 幂等) */
void os_theme_bind(const theme_host_t *host, const theme_renderer_t *rd);
const theme_host_t     *os_theme_host(void);
const theme_renderer_t *os_theme_renderer(void);

#ifdef __cplusplus
}
#endif

#endif /* THEME_RENDERER_H */
