/* statusbar_policy.h — 状态栏显隐判据 (纯逻辑, 零依赖, 宿主机可测)
 *
 * 2026-11-23 linit: 状态栏统一整改。背景是"每个应用各画一套"导致的三类问题:
 *   ① Lua 应用 app.statusbar() 原来是**立刻画一次** ⇒ 时间/电量永远停在那一刻(不刷新);
 *   ② 应用自己写死的 20/24px 与系统实际高度不一致 ⇒ 顶部一圈被盖住或留白;
 *   ③ 有的应用退出后不还原反白/隐藏状态 ⇒ 残留到下一页。
 *
 * 现在只有一条规则: **应用只声明要不要, 显隐与刷新由 OS 决定**。
 *   声明 = os_statusbar_show(bool) / Lua 的 app.statusbar(bool)  (黏在本页, 换页自动失效)
 *   判定 = 本文件 os_sb_should_show(): 声明优先, 没声明按页面是否全屏推。
 *
 * 单独成文件是为了让主机测试直接编译生产判据（tests/statusbar_policy），
 * 而不是在测试里重写一份"看起来一样"的逻辑。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 页面声明值.  AUTO = 没表态 (按页面属性推), HIDE/SHOW = 明确要/不要. */
enum {
    OS_SB_AUTO = -1,
    OS_SB_HIDE = 0,
    OS_SB_SHOW = 1,
};

/* 声明的归属: 只对"声明时所在的那一页"有效。换页 ⇒ 回到 AUTO,
 * 这样应用退出/切页都不需要额外还原一次, 也不会把状态带到下一页。 */
static inline bool os_sb_req_applies(int req_page, int cur_page)
{
    return req_page == cur_page;
}

/* 本帧到底画不画状态栏。
 *   req        : OS_SB_AUTO / OS_SB_HIDE / OS_SB_SHOW (已经过"归属页"过滤)
 *   fullscreen : 模块静态 fullscreen || 运行时 fullscreen 覆盖
 * 语义: 明确声明 > 页面属性。声明 SHOW 的全屏页(游戏要一条栏)也照画;
 *       声明 HIDE 的普通页也照藏。两者都没说 → 非全屏才画（与历史行为一致）。 */
static inline bool os_sb_should_show(int req, bool fullscreen)
{
    if (req == OS_SB_SHOW) return true;
    if (req == OS_SB_HIDE) return false;
    return !fullscreen;
}

/* 这次声明是否"新"的（同页同值 = 不是）。
 * 用途: os_statusbar_show() 靠它做到**幂等** —— 应用在 render 里每帧调也不会
 * 反复触发重绘/打断墨水屏刷新; 重复调用显示/隐藏是安全的。 */
static inline bool os_sb_req_is_new(int req_page, int req, int cur_page, bool on)
{
    int want = on ? OS_SB_SHOW : OS_SB_HIDE;
    return !(req_page == cur_page && req == want);
}

#ifdef __cplusplus
}
#endif
