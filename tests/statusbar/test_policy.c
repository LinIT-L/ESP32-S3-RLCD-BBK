/**
 * tests/statusbar/test_policy.c — 状态栏显隐判据宿主机测试
 *
 * 被测对象是**生产源码** components/system/os/include/statusbar_policy.h
 * （os_statusbar_show / os_statusbar_page_req 用的同一份判据），不抄副本、不 mock。
 *
 * 覆盖 2026-11-23 状态栏统一整改的三条契约:
 *   ① 没声明时行为与历史一致：非全屏才显示；
 *   ② 声明优先于全屏属性（全屏页也能要一条栏；普通页也能明确隐藏）；
 *   ③ 声明黏在"声明时所在页"，换页自动回到 AUTO ⇒ 应用退出不会把状态带出去。
 *
 * 用法: sh tests/statusbar/run_all.sh
 */
#include <stdio.h>
#include <string.h>

#include "statusbar_policy.h"

static int g_ok = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_ok++; } \
    else { g_fail++; printf("  ✗ %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void sec(const char *s) { printf("\n[%s]\n", s); }

/* 页面 id 只是标识, 取几个互不相同的值即可 (与 os_page_t 数值无关) */
enum { P_MAIN = 0, P_LUA = 30, P_CALC = 12, P_GAME = 40 };

static void t_default(void)
{
    sec("① 未声明 ⇒ 与历史行为一致（非全屏才显示）");
    CHECK(os_sb_should_show(OS_SB_AUTO, false),  "普通页 (非全屏) ⇒ 显示");
    CHECK(!os_sb_should_show(OS_SB_AUTO, true),  "全屏页 ⇒ 隐藏");
}

static void t_declared(void)
{
    sec("② 声明优先于全屏属性");
    CHECK(os_sb_should_show(OS_SB_SHOW, true),   "全屏页明确要 ⇒ 显示（游戏只要一条栏）");
    CHECK(os_sb_should_show(OS_SB_SHOW, false),  "普通页明确要 ⇒ 显示");
    CHECK(!os_sb_should_show(OS_SB_HIDE, false), "普通页明确不要 ⇒ 隐藏");
    CHECK(!os_sb_should_show(OS_SB_HIDE, true),  "全屏页明确不要 ⇒ 隐藏");

    /* 枚举值不能漂: 内核按 int 存, 应用按 bool 传, 只有这三档 */
    CHECK(OS_SB_AUTO == -1 && OS_SB_HIDE == 0 && OS_SB_SHOW == 1,
          "三档取值固定 AUTO=-1/HIDE=0/SHOW=1");
}

static void t_scope(void)
{
    sec("③ 声明按页归属（换页自动失效, 不残留到下一页）");
    CHECK(os_sb_req_applies(P_LUA, P_LUA),    "同页 ⇒ 声明有效");
    CHECK(!os_sb_req_applies(P_LUA, P_MAIN),  "换页 ⇒ 声明失效");
    CHECK(!os_sb_req_applies(P_MAIN, P_LUA),  "反向换页同样失效");
}

/* 场景回放: 用生产判据走一遍"录音机声明显示 → 切到全屏计算器 → 回桌面"，
 * 验证状态栏显隐不会因为上一页的声明而错乱。 */
static void t_scenario(void)
{
    sec("场景: 录音机(Lua) → 全屏计算器 → 桌面");

    /* 录音机: 在 Lua 页声明 SHOW, 本页不是全屏(声明优先) */
    int decl_page = P_LUA, decl = OS_SB_SHOW;
    int req = os_sb_req_applies(decl_page, P_LUA) ? decl : OS_SB_AUTO;
    CHECK(os_sb_should_show(req, false), "录音机页 ⇒ 状态栏显示");

    /* 录音机内反复调用 app.statusbar(true) 不应改变结果 (幂等判据层面) */
    req = os_sb_req_applies(decl_page, P_LUA) ? decl : OS_SB_AUTO;
    CHECK(os_sb_should_show(req, false), "重复声明 ⇒ 结果不变");

    /* 切到全屏计算器: 声明不属于本页 ⇒ AUTO, 全屏 ⇒ 隐藏 (无残留) */
    req = os_sb_req_applies(decl_page, P_CALC) ? decl : OS_SB_AUTO;
    CHECK(req == OS_SB_AUTO, "换页后声明回到 AUTO");
    CHECK(!os_sb_should_show(req, true), "全屏计算器 ⇒ 隐藏（录音机的声明没漏出去）");

    /* 回桌面: AUTO + 非全屏 ⇒ 显示 */
    req = os_sb_req_applies(decl_page, P_MAIN) ? decl : OS_SB_AUTO;
    CHECK(os_sb_should_show(req, false), "回桌面 ⇒ 状态栏照常显示");

    /* 另一次: 应用明确 HIDE 后退出, 下一页不能被带成隐藏 */
    decl_page = P_GAME; decl = OS_SB_HIDE;
    req = os_sb_req_applies(decl_page, P_LUA) ? decl : OS_SB_AUTO;
    CHECK(req == OS_SB_AUTO && os_sb_should_show(req, false),
          "上一页声明 HIDE ⇒ 新页面不受影响");
}

static void t_idempotent(void)
{
    sec("④ 重复调用显示/隐藏是安全的（幂等, 不额外触发重绘）");
    /* 同页、同值 ⇒ 不算新声明 */
    CHECK(!os_sb_req_is_new(P_LUA, OS_SB_SHOW, P_LUA, true),  "重复 show(true) ⇒ 不重复生效");
    CHECK(!os_sb_req_is_new(P_LUA, OS_SB_HIDE, P_LUA, false), "重复 show(false) ⇒ 不重复生效");
    /* 值变了 ⇒ 算新声明 (要重绘一次) */
    CHECK(os_sb_req_is_new(P_LUA, OS_SB_HIDE, P_LUA, true),   "hide → show ⇒ 生效");
    CHECK(os_sb_req_is_new(P_LUA, OS_SB_SHOW, P_LUA, false),  "show → hide ⇒ 生效");
    /* 换页 ⇒ 算新声明 (新页面独立表态, 不受上一页影响) */
    CHECK(os_sb_req_is_new(P_LUA, OS_SB_SHOW, P_MAIN, true),  "换页后同值也要重新绑定到新页");
    /* 还没声明过 (哨兵页 -0x7FFF) ⇒ 一定算新 */
    CHECK(os_sb_req_is_new(-0x7FFF, OS_SB_AUTO, P_MAIN, false), "首次声明 ⇒ 生效");
}

int main(void)
{
    printf("状态栏显隐判据（生产源码 statusbar_policy.h）\n");
    t_default();
    t_declared();
    t_scope();
    t_scenario();
    t_idempotent();

    printf("\n---- %d 通过, %d 失败 ----\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
