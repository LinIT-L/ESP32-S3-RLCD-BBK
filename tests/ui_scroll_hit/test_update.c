/* tests/ui_scroll_hit/test_update.c — ui_scroll_update() / ui_scroll_set_offset() 语义测试
 *
 * 这两支是 2026-11-23 收口的"标准 API":
 *   ui_scroll_update      内容/几何变化时的手势安全入口（不再各页手写 init）
 *   ui_scroll_set_offset  外部定位（夹紧 + 清 sub/fling/吸附）
 *
 * 断言重点 = 修 bug 的那几条语义:
 *   · 只有内容长度变化时 **不动偏移、不清手势**（否则拖动被打断 = "滚动卡住"）
 *   · 几何变化在手势中也保留手势
 *   · 外部定位在手势中不生效（用户优先）
 */
#include <stdio.h>
#include "ui_scroll.h"

static int g_pass, g_fail;
static void ck(int cond, const char *what)
{
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("FAIL %s\n", what);
}

int main(void)
{
    ui_scroll_t s = {0};
    ui_scroll_config(&s, UI_SCROLL_LIST, 0);

    ck(ui_scroll_update(&s, 50, 200, 1000) == true, "首次 update 应建立几何");
    ck(s.row_px == 50 && s.view_px == 200 && s.content_px == 1000, "几何参数落地");
    ck(ui_scroll_update(&s, 50, 200, 1000) == false, "无变化应返回 false");

    /* ---- 拖动中内容长度变化: 偏移与手势都必须保留 ---- */
    ui_scroll_touch_xy(&s, 0, 100, true);      /* 按下 */
    ui_scroll_touch_xy(&s, 0, 70, true);       /* 上滑 30px → 判定拖动 */
    ck(s.pressed && s.dragging, "拖动状态应建立");
    int off_before = s.off;
    ck(ui_scroll_update(&s, 50, 200, 2000) == true, "内容变化应返回 true");
    ck(s.off == off_before, "内容变化不得移动偏移");
    ck(s.pressed && s.dragging, "内容变化不得清手势");

    /* ---- 拖动中几何变化: 只换参数, 手势保留 ---- */
    ck(ui_scroll_update(&s, 44, 180, 2000) == true, "几何变化应返回 true");
    ck(s.row_px == 44 && s.view_px == 180, "几何参数应更新");
    ck(s.pressed && s.dragging, "几何变化不得清手势");

    /* ---- 外部定位: 手势中不生效 ---- */
    ck(ui_scroll_set_offset(&s, 500) == false, "按住时 set_offset 应拒绝");
    ck(s.off == off_before, "按住时偏移不得改变");
    ui_scroll_touch_xy(&s, 0, 0, false);       /* 松手 */

    /* ---- 外部定位: 夹紧 + 清 sub/fling/吸附 ---- */
    s.sub = 7; s.fling = 3.0f; s.snapping = true; s.snap_to = 123;
    ck(ui_scroll_set_offset(&s, 999999) == true, "set_offset 应生效");
    ck(s.off == ui_scroll_max(&s), "set_offset 应夹到 max");
    ck(s.sub == 0 && s.fling == 0.0f && !s.snapping, "set_offset 应清 sub/fling/吸附");

    ck(ui_scroll_set_offset(&s, -50) == true, "set_offset 负数应夹到 0");
    ck(s.off == 0, "负数夹紧");

    /* ---- 非法几何容错 ---- */
    ui_scroll_t t = {0};
    ck(ui_scroll_update(&t, 0, 0, 100) == true, "非法几何也应建立");
    ck(t.row_px == 1 && t.view_px == 1, "非法几何应夹到 1");

    printf("ui_scroll_update: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
