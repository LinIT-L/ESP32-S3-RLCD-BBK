/* tests/ui_scroll_hit/test_row_at.c — ui_scroll_row_at() 行号反算测试
 *
 * 目的: 固定「屏幕 y → 内容行下标」的唯一正确算法, 防止再出现
 *   `floorf(相机行) + 行内偏移` 两段相加的老写法(与渲染的像素偏移差整行,
 *   表现为"滑动后点 A 打开 B")。
 *
 * 渲染模型(应用中心 am_col_present / am_col_render_grid):
 *   列缓冲里第 k 行占 [k*row_h + extra_top, (k+1)*row_h + extra_top),
 *   屏幕 y = content_top + (缓冲 y - off_px)
 * 因此: row_at(y) 必须满足 —— 第 k 行任意可见像素反算回来都是 k。
 *
 * 编译: 由 run_all.sh 负责 (clang -std=c11 -Wall -Wextra)。
 */
#include <stdio.h>
#include <math.h>
#include "ui_scroll.h"

static int g_pass, g_fail;

static void ck(int cond, const char *what, int y, int off, int row_h, int extra, int expect, int got)
{
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("FAIL %s: y=%d off=%d row_h=%d extra=%d expect=%d got=%d\n",
           what, y, off, row_h, extra, expect, got);
}

/* 逐个像素扫描: 对每个 off, 屏幕上每个像素反算出来的行, 必须与渲染一致 */
static void sweep(const char *tag, int off, int row_h, int extra, int content_top, int screen_h)
{
    for (int y = content_top; y < screen_h; y++) {
        int dy = off + (y - content_top) - extra;
        int expect;
        if (dy < 0) {
            /* 首行上内边距内仍算第 0 行; 更靠上(不可能出现)记 -1 */
            expect = (dy + row_h > 0) ? 0 : -1;
        } else {
            expect = dy / row_h;
        }
        int got = ui_scroll_row_at(y, content_top, off, row_h, extra);
        ck(got == expect, tag, y, off, row_h, extra, expect, got);
    }
}

/* 行边界必须是"干净的一跳": 边界像素属于 k, 紧邻的上一像素属于 k-1。
 * 第 0 行的上方内边距(extra)按约定归第 0 行 —— 由调用方用 y<content_top 兜,
 * 这里只测真正位于行与行之间的边界。 */
static void boundaries(int off, int row_h, int extra, int content_top, int rows)
{
    for (int k = 1; k < rows; k++) {
        int top = content_top + k * row_h + extra - off;   /* 第 k 行首像素所在屏幕 y */
        if (top <= content_top) continue;                  /* 被视口顶部裁掉的行不测边界 */
        int a = ui_scroll_row_at(top - 1, content_top, off, row_h, extra);
        int b = ui_scroll_row_at(top,     content_top, off, row_h, extra);
        ck(b == k, "边界行号", top, off, row_h, extra, k, b);
        ck(a == k - 1, "边界上一像素", top - 1, off, row_h, extra, k - 1, a);
    }
}

/* 老写法(错误): floor(相机行) + 行内偏移 —— 用于证明它与正确算法在小数偏移下必然分叉 */
static int old_way(int y, int content_top, int off, int row_h, int extra)
{
    float cam = (float)off / (float)row_h;
    (void)extra;
    return (int)floorf(cam) + (y - content_top) / row_h;
}

int main(void)
{
    const int content_top = 40, screen_h = 300;

    /* 1) 网格: 行步距 54, 首行内边距 2 (与 am_col_render_grid 一致) */
    const int offs[] = { 0, 1, 13, 27, 28, 32, 53, 54, 55, 80, 107, 108 };
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        sweep("grid", offs[i], 54, 2, content_top, screen_h);
        boundaries(offs[i], 54, 2, content_top, 8);
    }

    /* 2) 列表: 行高 48, 无内边距 (与 am_col_render_list 一致) */
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        sweep("list", offs[i], 48, 0, content_top, screen_h);
        boundaries(offs[i], 48, 0, content_top, 8);
    }

    /* 3) 小数偏移下, 老写法与正确算法的分叉必须存在(证明修复点真实存在) */
    int diff = 0;
    for (int y = content_top; y < screen_h; y++) {
        int good = ui_scroll_row_at(y, content_top, 32, 54, 2);
        int bad  = old_way(y, content_top, 32, 54, 2);
        if (good != bad) diff++;
    }
    ck(diff > 0, "老写法应分叉", 0, 32, 54, 2, 1, diff);

    /* 4) 非法参数不应崩 */
    ck(ui_scroll_row_at(100, 40, 0, 0, 0) == -1, "row_h=0 返回 -1", 100, 0, 0, 0, -1,
       ui_scroll_row_at(100, 40, 0, 0, 0));

    printf("ui_scroll_hit: %d passed, %d failed (老写法分叉像素=%d)\n",
           g_pass, g_fail, diff);
    return g_fail ? 1 : 0;
}
