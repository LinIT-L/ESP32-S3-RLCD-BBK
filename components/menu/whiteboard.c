/**
 * @file whiteboard.c
 * @brief 白板 (画图) 独立主菜单页: 铅笔/马克笔/橡皮/直线/矩形/椭圆 + 粗细/填充,
 *        撤销/重做 (位图快照栈), 保存/加载 TF 卡 1-bit BMP, 自动持久化.
 *
 * 设计说明:
 *  - 画布保存在独立 1-bit 位图 s_canvas (400x300, PSRAM, 位=1 表示黑色笔迹).
 *    用 st7305_blit_1bit 整刷上屏, 位=1 恰好对应其"黑点"语义.
 *  - 自由画笔 (铅笔/马克笔/橡皮): 增量式直写 fb + 同步更新 s_canvas, 低延迟.
 *  - 撤销/加载/清屏: 恢复 s_canvas 后整刷 (s_wb_full), 一次性全屏刷新.
 *  - 全部状态静态变量, 无运行期动态分配; 与 mini_apps 一致的自包含 ASCII 渲染.
 */
#include "whiteboard.h"
#include "st7305.h"
#include "input.h"
#include "sd_scan.h"
#include "font_zh.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#define WB_W      ST7305_WIDTH          /* 400 */
#define WB_H      ST7305_HEIGHT         /* 300 */
#define WB_BPR    ((WB_W+7)>>3)         /* 50 字节/行 */
#define WB_BYTES  (WB_H*WB_BPR)         /* 15000 */

/* 布局: 顶部横排 7 操作 (横框+竖线分隔) + 左侧竖排 6 工具, 其余为画布. */
#define WB_TB_W    64                   /* 左工具列宽 */
#define WB_TOP_H   32                   /* 顶部操作条高 */
#define WB_CANV_H  (WB_H - WB_TOP_H)    /* 画布高 (268) */
#define WB_TOOL_N  6                    /* 工具数 (铅笔/马克笔/橡皮/直线/矩形/椭圆) */
#define WB_OP_N    5                    /* 顶部文字操作: 撤销/重做/清空/保存/加载 */
#define WB_ICON_N  13                   /* s_icon 尺寸 (仅用前 6 工具, 余保留) */
#define WB_ROW_H   (WB_CANV_H/(WB_TOOL_N+1)) /* 左槽: 6 工具 + 粗细, 38 */

#define WB_STK     10                   /* 撤销/重做快照深度 */

/* 工具 */
enum { WB_PEN, WB_MARK, WB_ERAS, WB_LINE, WB_RECT, WB_OVAL };

/* ===== 左侧/顶部工具栏图标 (20x20 位图, 位=1 黑). 共 6 工具 + 7 操作 = 13 个.
 * 后期替换图标: 只需改对应 60 字节数组 (20 行 x 每行 3 字节, 每字节 MSB 在左). ===== */
static const uint8_t s_icon[WB_ICON_N][60] = {
    { /* pen  铅笔 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x0F,0x00,0x00,0x0F,0x00,0x00,0x00,0x40,0x00,0x00,0x40,
        0x00,0x00,0x40,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x10,0x00,0x00,
        0x10,0x00,0x00,0x10,0x00,0x00,0x08,0x00,0x00,0x3C,0x00,0x00,0x7E,0x00,0x00,0x00,0x80,0x00,0x00,0x00,
    },
    { /* mark 马克笔 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x07,0x80,0x00,0x07,0xC0,0x00,0x03,0xE0,0x00,0x01,0xE0,
        0x00,0x00,0xF0,0x00,0x00,0xF8,0x00,0x00,0x7C,0x00,0x00,0x3E,0x00,0x00,0x1F,0x00,0x00,0x0F,0x00,0x00,
        0x07,0x80,0x00,0x07,0xC0,0x00,0x03,0xE0,0x00,0x01,0xE0,0x00,0x00,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* eras 橡皮 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,
        0xFF,0xC0,0x1F,0xFF,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* line 直线 */
        0x00,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x02,0x00,
        0x00,0x01,0x00,0x00,0x00,0x80,0x00,0x00,0x60,0x00,0x00,0x10,0x00,0x00,0x08,0x00,0x00,0x04,0x00,0x00,
        0x02,0x00,0x00,0x01,0x00,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,
    },
    { /* rect 矩形 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xFF,0xC0,0x10,0x00,0x40,0x10,0x00,
        0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,
        0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x1F,0xFF,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* oval 椭圆 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00,0x03,0x06,
        0x00,0x0C,0x01,0x80,0x08,0x00,0x80,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x08,0x00,0x80,0x0C,
        0x01,0x80,0x03,0x06,0x00,0x00,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* undo 撤销 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x07,0x00,0x00,0x1F,0x00,0x00,0x3F,0xFF,
        0xC0,0x1F,0xFF,0xC0,0x0F,0x00,0x00,0x07,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* redo 重做 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x0C,0x00,0x00,0x0E,0x00,0x3F,0xFF,
        0x00,0x3F,0xFF,0xC0,0x00,0x0F,0x00,0x00,0x0E,0x00,0x00,0x0C,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* size 粗细 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x1C,0x00,0x00,0x1C,0x00,0x00,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0x00,0x00,0x00,0x03,0xE0,0x00,
        0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* fill 填充 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xFF,0x80,0x10,0x00,0x80,0x10,0x00,0x80,0x10,0x00,
        0x80,0x10,0x00,0x80,0x10,0x00,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,
        0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* trash 清空 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x8C,0x00,0x01,0x8C,0x00,0x07,0xFF,0x00,0x07,0xFF,0x00,0x0F,0xFF,
        0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,
        0xCC,0x80,0x0F,0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* save 保存 */
        0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,
        0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x08,0x00,0x80,0x09,0xFC,0x80,0x09,0x04,0x80,0x09,
        0x04,0x80,0x09,0x04,0x80,0x09,0xFC,0x80,0x08,0x00,0x80,0x0F,0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* load 加载 */
        0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x60,0x00,0x00,0xF0,0x00,0x01,0xF8,0x00,0x03,0xFC,0x00,0x03,0xFE,
        0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,
        0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x3F,0xFF,0xE0,0x3F,0xFF,0xE0,0x3F,0xFF,0xE0,0x00,0x00,0x00,
    },
};

/* 画布 + 撤销/重做快照栈 (PSRAM, 静态分配) */
EXT_RAM_BSS_ATTR static uint8_t s_canvas[WB_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_undo[WB_STK][WB_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_redo[WB_STK][WB_BYTES];
static int  s_undo_n = 0, s_redo_n = 0;

static int  s_tool  = WB_PEN;
static int  s_size  = 1;         /* 粗细 1..6 */
static bool s_down  = false;     /* 手指按下中 */
static bool s_free  = false;     /* 自由画笔手势 (铅笔/马克笔/橡皮) */
static int  s_lx, s_ly;          /* 最近点 */
static int  s_ax, s_ay;          /* 形状起点 */
static bool s_full  = true;      /* 需要整屏重刷 (进入/撤销/加载/清屏) */
static bool s_inited = false;

static char   s_toast[22] = {0};
static uint32_t s_toast_ms = 0;
static int   s_top_press = -1;   /* 顶部操作按下反馈 (-1 无, 0..5 按下的按钮) */
static bool  s_exitask = false;  /* 返回键两次确认退出 (同游戏引擎) */

/* --- 多白板文件 (SD) 与加载列表模态 --- */
#define WB_FILE_MAX 40
EXT_RAM_BSS_ATTR static int    s_nfiles;
EXT_RAM_BSS_ATTR static char   s_names[WB_FILE_MAX][36];   /* 显示名 */
EXT_RAM_BSS_ATTR static char   s_paths[WB_FILE_MAX][64];   /* 完整路径 */
EXT_RAM_BSS_ATTR static char   s_cur[64];                  /* 当前文件 (空=新建) */
static bool   s_load    = false;          /* 加载列表模态 */
static int    s_lsel    = 0;              /* 列表选中行 */
static bool   s_delask  = false;          /* 删除确认态 */
static int    s_deln    = 0;              /* 待删除文件名索引 */

/* ================= 通用绘制 (自包含 ASCII 字体, 直接写 fb) ================= */
static void px(st7305_handle_t *l, int x, int y, st7305_color_t c) {
    if (x < 0 || x >= WB_W || y < 0 || y >= WB_H) return;
    st7305_draw_pixel(l, x, y, c);
}
static void filr(st7305_handle_t *l, int x0, int y0, int x1, int y1, st7305_color_t c) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    for (int y=y0; y<=y1; y++)
        for (int x=x0; x<=x1; x++)
            st7305_draw_pixel(l, x, y, c);
}
static void outline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    filr(l,x0,y0,x1,y0,ST7305_COLOR_BLACK);
    filr(l,x0,y1,x1,y1,ST7305_COLOR_BLACK);
    filr(l,x0,y0,x0,y1,ST7305_COLOR_BLACK);
    filr(l,x1,y0,x1,y1,ST7305_COLOR_BLACK);
}

/* ================= 画布访问 (位=1 黑, 位=0 白) ================= */
static inline void wb_bit(int x, int y, int on) {
    uint8_t *b = &s_canvas[y*WB_BPR + (x>>3)];
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (on) *b |= m; else *b &= ~m;
}
/* 在画布区域写一笔: 同步更新 s_canvas + fb. on=1 黑, on=0 白(橡皮). 画布 = x>=WB_TB_W && y>=WB_TOP_H. */
static void wb_ppx(st7305_handle_t *l, int x, int y, int on) {
    if (x < WB_TB_W || x >= WB_W || y < WB_TOP_H || y >= WB_H) return;
    wb_bit(x, y, on);
    st7305_draw_pixel(l, x, y, on ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
}

/* 填充圆盘 (粗笔画/橡皮头) */
static void wb_disk(st7305_handle_t *l, int cx, int cy, int r, int on) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                wb_ppx(l, cx+dx, cy+dy, on);
}
/* 粗直线: 沿 Bresenham 逐点盖圆盘, 得到平滑等宽笔画 */
static void wb_line(st7305_handle_t *l, int x0, int y0, int x1, int y1, int r, int on) {
    int dx = x1>x0 ? x1-x0 : x0-x1, sx = x0<x1 ? 1 : -1;
    int dy = y1>y0 ? y1-y0 : y0-y1, sy = y0<y1 ? 1 : -1;
    int err = (dx>dy?dx:-dy) / 2, e2;
    for (;;) {
        wb_disk(l, x0, y0, r, on);
        if (x0==x1 && y0==y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}
/* 空心椭圆 (形状工具) */
static void wb_outline_ellipse(st7305_handle_t *l, int cx, int cy, int rx, int ry) {
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    int x = 0, y = ry;
    long rx2 = (long)rx*rx, ry2 = (long)ry*ry;
    long d = ry2 - rx2*ry + rx2/4, dx = 2*ry2*x, dy = 2*rx2*y;
    while (dx < dy) {
        wb_ppx(l,cx+x,cy+y,1); wb_ppx(l,cx-x,cy+y,1);
        wb_ppx(l,cx+x,cy-y,1); wb_ppx(l,cx-x,cy-y,1);
        if (d < 0) { x++; dx += 2*ry2; d += dx + ry2; }
        else { x++; y--; dx += 2*ry2; dy -= 2*rx2; d += dx - dy + ry2; }
    }
    d = (long)rx2*(2*y+1) + (long)(-ry2)*(2*x+1) + ry2;
    while (y >= 0) {
        wb_ppx(l,cx+x,cy+y,1); wb_ppx(l,cx-x,cy+y,1);
        wb_ppx(l,cx+x,cy-y,1); wb_ppx(l,cx-x,cy-y,1);
        if (d > 0) { y--; dy -= 2*rx2; d += rx2 - dy; }
        else { y--; x++; dx += 2*ry2; dy -= 2*rx2; d += dx - dy + rx2; }
    }
}

/* ================= 撤销/重做 ================= */
static void push_undo(void) {
    if (s_undo_n < WB_STK)
        memcpy(s_undo[s_undo_n++], s_canvas, WB_BYTES);
    else {
        memmove(s_undo[0], s_undo[1], (WB_STK-1)*WB_BYTES);
        memcpy(s_undo[WB_STK-1], s_canvas, WB_BYTES);
    }
    s_redo_n = 0;
}
static void wb_undo(menu_state_t *st) {
    if (s_undo_n <= 0) return;
    if (s_redo_n < WB_STK) memcpy(s_redo[s_redo_n++], s_canvas, WB_BYTES);
    memcpy(s_canvas, s_undo[--s_undo_n], WB_BYTES);
    s_down = false; s_full = true; st->needs_redraw = true;
}
static void wb_redo(menu_state_t *st) {
    if (s_redo_n <= 0) return;
    if (s_undo_n < WB_STK) memcpy(s_undo[s_undo_n++], s_canvas, WB_BYTES);
    memcpy(s_canvas, s_redo[--s_redo_n], WB_BYTES);
    s_down = false; s_full = true; st->needs_redraw = true;
}

/* ================= 工具几何 ================= */
static int tool_radius(void) {
    switch (s_tool) {
        case WB_PEN:  return s_size ? s_size : 1;
        case WB_MARK: return s_size*2;
        case WB_ERAS: return s_size + 4;
        default:      return 1;
    }
}
static int tool_on(void) { return (s_tool == WB_ERAS) ? 0 : 1; }

/* 松开时提交形状 (直线/矩形/椭圆): 直接落到画布. */
static void commit_shape(st7305_handle_t *l) {
    switch (s_tool) {
        case WB_LINE: wb_line(l, s_ax,s_ay, s_lx,s_ly, 1, 1); break;
        case WB_RECT: {
            int x0=s_ax<s_lx?s_ax:s_lx, x1=s_ax<s_lx?s_lx:s_ax;
            int y0=s_ay<s_ly?s_ay:s_ly, y1=s_ay<s_ly?s_ly:s_ay;
                wb_line(l,x0,y0,x1,y0,1,1); wb_line(l,x0,y1,x1,y1,1,1);
                wb_line(l,x0,y0,x0,y1,1,1); wb_line(l,x1,y0,x1,y1,1,1);
            break;
        }
        case WB_OVAL: {
            int dx=s_lx-s_ax, dy=s_ly-s_ay;
            if (dx<0) dx=-dx;
            if (dy<0) dy=-dy;
            int rx = dx/2 + 1, ry = dy/2 + 1;
            int cx = (s_ax+s_lx)/2, cy = (s_ay+s_ly)/2;
            if (rx < 1) rx = 1;
            if (ry < 1) ry = 1;
            wb_outline_ellipse(l, cx,cy, rx,ry);
            break;
        }
        default: break;
    }
}

/* ================= 工具栏命中 ================= */
static int in_top_bar(int x, int y) { return (y < WB_TOP_H && x >= 0 && x < WB_W); }
static int in_tool_col(int x, int y) { return (x < WB_TB_W && y >= WB_TOP_H && y < WB_H); }

/* 顶部操作条按钮 (7 个均分): 返回 0..6 */
static int op_btn(int x) {
    int w = (WB_W + WB_OP_N-1)/WB_OP_N;
    int i = x / w;
    return (i >= WB_OP_N) ? WB_OP_N-1 : i;
}
/* 左工具列槽 (7 个: 0..5 工具, 6 粗细): 返回 0..6 */
static int tool_slot_btn(int y) {
    int i = (y - WB_TOP_H) / WB_ROW_H;
    return (i >= WB_TOOL_N+1) ? WB_TOOL_N : i;
}

static void toast(const char *s) {
    strncpy(s_toast, s, sizeof(s_toast)-1);
    s_toast[sizeof(s_toast)-1] = 0;
    s_toast_ms = (uint32_t)(esp_timer_get_time()/1000);
}

static void wb_open_loader(menu_state_t *st);   /* 加载列表模态入口 */
static bool wb_bmp_write(const char *path);
static void wb_new_name(char *out);
static int  wb_zh_w(const char *s);
static int  wb_draw_zh(st7305_handle_t *l, int x, int y, const char *str, st7305_color_t c);

/* 顶部操作 (0..4): 0撤销 1重做 2清空 3保存 4加载 */
static void handle_op(int op, menu_state_t *st) {
    switch (op) {
        case 0: wb_undo(st); return;
        case 1: wb_redo(st); return;
        case 2:
            push_undo();
            memset(s_canvas, 0, WB_BYTES);
            s_full = true;
            break;
        case 3:
            wb_new_name(s_cur);                  /* 保存总是新建一份 (时间+4位序号递增) */
            if (wb_bmp_write(s_cur)) toast("已保存");
            else                    toast("无TF卡");
            break;
        case 4: wb_open_loader(st); /* 弹出标准列表选择要加载的白板 */ return;
        default: break;
    }
    st->needs_redraw = true;
}

/* ================= 保存/加载 SD 多个白板 (1-bit BMP) ================= */
#define WB_DIR   "/sdcard"
static const int s_bmp_off = 0x3E;   /* 54 头 + 8 调色板 */

/* 写 s_canvas 到任意路径 (BMP) */
static bool wb_bmp_write(const char *path) {
    if (!sd_is_mounted()) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int stride = ((WB_W + 31) / 32) * 4;      /* 52 */
    uint32_t imgsz = (uint32_t)stride * WB_H;
    uint32_t fsz = s_bmp_off + imgsz;
    uint8_t h[54]; memset(h, 0, sizeof(h));
    h[0]='B'; h[1]='M';
    h[2]=fsz; h[3]=fsz>>8; h[4]=fsz>>16; h[5]=fsz>>24;
    h[10]=s_bmp_off;
    h[14]=40;
    h[18]=WB_W; h[19]=WB_W>>8; h[20]=WB_W>>16; h[21]=WB_W>>24;
    h[22]=WB_H; h[23]=WB_H>>8; h[24]=WB_H>>16; h[25]=WB_H>>24;
    h[26]=1; h[28]=1;
    h[34]=imgsz; h[35]=imgsz>>8; h[36]=imgsz>>16; h[37]=imgsz>>24;
    h[46]=2;
    fwrite(h, 1, 54, f);
    uint8_t pal[8] = {0,0,0,0, 255,255,255,0};
    fwrite(pal, 1, 8, f);
    uint8_t line[52];
    for (int y = 0; y < WB_H; y++) {
        int fy = WB_H - 1 - y;
        fseek(f, s_bmp_off + (long)fy*stride, SEEK_SET);
        memset(line, 0, stride);
        const uint8_t *row = &s_canvas[y*WB_BPR];
        for (int x = 0; x < WB_W; x++)
            if (!((row[x>>3] >> (7-(x&7))) & 1)) line[x>>3] |= (uint8_t)(0x80u >> (x&7));
        fwrite(line, 1, stride, f);
    }
    fclose(f);
    return true;
}
/* 读任意路径 BMP 到 s_canvas */
static bool wb_bmp_read(const char *path) {
    if (!sd_is_mounted()) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[62];
    if (fread(hdr, 1, 62, f) != 62) { fclose(f); return false; }
    if (hdr[0]!='B' || hdr[1]!='M') { fclose(f); return false; }
    int w = hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|((int)hdr[21]<<24);
    int ht = hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|((int)hdr[25]<<24);
    if (hdr[28]!=1 || w!=WB_W || ht!=WB_H) { fclose(f); return false; }
    int stride = ((WB_W + 31) / 32) * 4;
    uint8_t line[52];
    memset(s_canvas, 0, WB_BYTES);
    for (int y = 0; y < WB_H; y++) {
        int fy = WB_H - 1 - y;
        fseek(f, s_bmp_off + (long)fy*stride, SEEK_SET);
        if (fread(line, 1, stride, f) != (size_t)stride) { fclose(f); memset(s_canvas,0,WB_BYTES); return false; }
        uint8_t *row = &s_canvas[y*WB_BPR];
        for (int x = 0; x < WB_W; x++)
            if (!((line[x>>3] >> (7-(x&7))) & 1)) row[x>>3] |= (uint8_t)(0x80u >> (x&7));
    }
    fclose(f);
    s_undo_n = 0; s_redo_n = 0;
    return true;
}
/* 从文件名解析 4 位序号 ("wbHHMMnnnn.bmp" -> nnnn), 非本格式返回 -1 */
static int wb_file_seq(const char *base) {
    if (strncmp(base, "wb", 2) || strlen(base) < 10) return -1;
    const char *dot = strrchr(base, '.');
    if (!dot || strcmp(dot, ".bmp")) return -1;
    return atoi(base + 6);   /* nnnn */
}
/* 扫描并列出所有白板, 按序号倒序(最新在前). 填充 s_names/s_paths. */
static void wb_list(void) {
    s_nfiles = 0;
    if (!sd_is_mounted()) return;
    DIR *d = opendir(WB_DIR);
    if (!d) return;
    char tmp[WB_FILE_MAX][64]; int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0]=='.' || strncmp(e->d_name,"wb",2) || !strstr(e->d_name,".bmp")) continue;
        if (wb_file_seq(e->d_name) < 0) continue;
        snprintf(tmp[n], 64, "%s/%.50s", WB_DIR, e->d_name);
        n++; if (n >= WB_FILE_MAX) break;
    }
    closedir(d);
    /* 按序号倒序 (冒泡) */
    for (int i = 0; i < n-1; i++)
        for (int j = 0; j < n-1-i; j++) {
            const char *a = strrchr(tmp[j],'/')+1, *b = strrchr(tmp[j+1],'/')+1;
            if (wb_file_seq(a) < wb_file_seq(b)) { char s64[64]; strcpy(s64,tmp[j]); strcpy(tmp[j],tmp[j+1]); strcpy(tmp[j+1],s64); }
        }
    for (int i = 0; i < n; i++) {
        strcpy(s_paths[i], tmp[i]);
        const char *base = strrchr(tmp[i],'/')+1;
        /* 显示名 "HH:MM #nnnn" */
        snprintf(s_names[i], 36, "%.2s:%.2s #%.4s", base+2, base+4, base+6);
        s_nfiles = i+1;
    }
}
/* 生成一个全新文件名 "wbHHMMnnnn.bmp" (序号 = 现最大序号 +1, 从 0000 起) */
static void wb_new_name(char *out) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int max = -1;
    wb_list();
    for (int i = 0; i < s_nfiles; i++) {
        const char *base = strrchr(s_paths[i],'/')+1;
        int q = wb_file_seq(base);
        if (q > max) max = q;
    }
    int seq = (max + 1) % 10000;
    snprintf(out, 64, "%s/wb%02d%02d%04d.bmp", WB_DIR, t->tm_hour, t->tm_min, seq);
}

/* ================= 加载列表模态 (标准列表框): 选择加载 / 长按删除 ================= */
#define LD_X0    8
#define LD_Y0    (WB_TOP_H + 4)     /* 弹窗顶 */
#define LD_YB    294
#define LD_TITLE_H 28
#define LD_ROW    26

static void wb_do_load(int idx) {
    if (wb_bmp_read(s_paths[idx])) strcpy(s_cur, s_paths[idx]);
    s_load = false; s_delask = false;
}
static void wb_del_entry(int idx) {
    if (idx < 0 || idx >= s_nfiles) return;
    unlink(s_paths[idx]);
    if (strcmp(s_paths[idx], s_cur) == 0) s_cur[0] = 0;
    wb_list();
    if (s_lsel >= s_nfiles) s_lsel = s_nfiles > 0 ? s_nfiles - 1 : 0;
    s_delask = false;
}

void wb_open_loader(menu_state_t *st) {
    wb_list();
    if (s_nfiles == 0) { toast("无存档"); st->needs_redraw = true; return; }
    s_top_press = -1; s_lsel = 0; s_load = true; s_delask = false;
    st->needs_redraw = true;
}

bool wb_modal_active(void) { return s_load || s_delask; }

static void wb_draw_loader(st7305_handle_t *l) {
    int bx = LD_X0, ty0 = LD_Y0;
    filr(l, bx-2, ty0-2, WB_W-8+2, LD_YB, ST7305_COLOR_WHITE);
    outline(l, bx-2, ty0-2, WB_W-8+2, LD_YB);
    wb_draw_zh(l, bx, ty0, "打开白板", ST7305_COLOR_BLACK);
    int y0 = ty0 + LD_TITLE_H, rows = (LD_YB - y0) / LD_ROW;
    int shown = s_nfiles < rows ? s_nfiles : rows;
    for (int i = 0; i < shown; i++) {
        int yy = y0 + i*LD_ROW;
        if (i == s_lsel) {
            filr(l, bx-2, yy-1, WB_W-8, yy+LD_ROW-5, ST7305_COLOR_BLACK);
            wb_draw_zh(l, bx, yy, s_names[i], ST7305_COLOR_WHITE);
        } else {
            wb_draw_zh(l, bx, yy, s_names[i], ST7305_COLOR_BLACK);
        }
    }
    wb_draw_zh(l, bx, LD_YB - 26, "长按删除 / 返回取消", ST7305_COLOR_BLACK);
    if (s_delask) {
        const char *tk = "删除该白板?";
        const char *sy = "删除", *no = "取消";
        int w = wb_zh_w(tk) + 24;
        int x0 = (WB_W-w)/2, x1 = x0+w-1;
        int ay0 = 128, ay1 = 188;
        filr(l, x0, ay0, x1, ay1, ST7305_COLOR_WHITE);
        outline(l, x0, ay0, x1, ay1);
        wb_draw_zh(l, (WB_W-wb_zh_w(tk))/2, ay0+8, tk, ST7305_COLOR_BLACK);
        /* 按钮: [删除] [取消] */
        int bx0 = x0+10, bwx = (w-40-10)/2;
        filr(l, bx0, ay1-34, bx0+bwx-1, ay1-6, ST7305_COLOR_BLACK);
        wb_draw_zh(l, bx0+(bwx-wb_zh_w(sy))/2, ay1-30, sy, ST7305_COLOR_WHITE);
        int bx1 = x0+w-10-bwx;
        outline(l, bx1, ay1-34, bx1+bwx-1, ay1-6);
        wb_draw_zh(l, bx1+(bwx-wb_zh_w(no))/2, ay1-30, no, ST7305_COLOR_BLACK);
    }
}

/* loader 触摸: 短按行=加载, 长按行=删除确认; delask 点按钮 */
static void wb_loader_touch(menu_state_t *st) {
    int tx, ty; bool down = input_get_touch_pos(&tx, &ty);
    static bool last = false; static int ox = 0, oy = 0;
    int y0 = LD_Y0 + LD_TITLE_H, rows = (LD_YB - y0) / LD_ROW;
    if (!down) {
        if (last) {
            if (s_delask) {
                /* 判断点击删除/取消按钮 */
                const char *tk = "删除该白板?";
                int w = wb_zh_w(tk) + 24, x0 = (WB_W-w)/2, x1 = x0+w-1;
                int ay0 = 128, ay1 = 188, bw = (w-50)/2;
                int bx0 = x0+10, bx1 = x1-10-bw;
                if (oy >= ay1-38 && oy <= ay1) {
                    if (ox >= bx0 && ox <= bx0+bw) { wb_del_entry(s_deln); }
                    else if (ox >= bx1 && ox <= bx1+bw) { s_delask = false; }
                }
            } else {
                int row = (oy - y0) / LD_ROW;
                int shown = s_nfiles < rows ? s_nfiles : rows;
                if (row >= 0 && row < shown) { s_lsel = row; wb_do_load(row); }
            }
            st->needs_redraw = true;
        }
        last = false; return;
    }
    if (!last) { ox = tx; oy = ty; last = true; }
    else if (!s_delask) {
        uint32_t hold = input_touch_hold_ms();
        if (hold >= 800) {
            int row = (oy - y0) / LD_ROW;
            int shown = s_nfiles < rows ? s_nfiles : rows;
            if (row >= 0 && row < shown) { s_deln = row; s_lsel = row; s_delask = true; st->needs_redraw = true; }
        }
    }
}

/* 加载/删除模态的按键处理 (由 menu_system 在 wb_modal_active 时转发) */
void wb_modal_action(menu_state_t *st, menu_action_t a) {
    if (s_delask) {
        if (a == MENU_ACTION_CONFIRM) { wb_del_entry(s_deln); }
        else if (a == MENU_ACTION_BACK || a == MENU_ACTION_HOME) { s_delask = false; }
    } else if (s_load) {
        switch (a) {
            case MENU_ACTION_UP:    if (s_lsel > 0) s_lsel--; break;
            case MENU_ACTION_DOWN:  if (s_lsel < s_nfiles-1) s_lsel++; break;
            case MENU_ACTION_CONFIRM: wb_do_load(s_lsel); break;
            case MENU_ACTION_BACK:  s_load = false; break;
            default: break;
        }
    }
    st->needs_redraw = true;
}

/* ================= 对外接口 ================= */
void wb_reset(menu_state_t *st) {
    input_set_swipe_back(false);   /* 白板全屏绘图: 屏蔽底部上滑退出 */
    if (!s_inited) {
        memset(s_canvas, 0, WB_BYTES);
        memset(s_undo, 0, sizeof(s_undo));
        memset(s_redo, 0, sizeof(s_redo));
        s_inited = true;
    }
    s_tool = WB_PEN; s_size = 1;
    s_down = false; s_free = false;
    s_undo_n = 0; s_redo_n = 0;
    s_load = false; s_delask = false;
    s_cur[0] = 0;
    wb_list();
    if (s_nfiles > 0) {                      /* 自动恢复上次(最新)状态 */
        strcpy(s_cur, s_paths[0]);
        wb_bmp_read(s_cur);
    } else {
        memset(s_canvas, 0, WB_BYTES);       /* 无存档 -> 空白画板 */
    }
    st->needs_redraw = true;
}

void wb_autosave(void) {
    if (!s_cur[0]) wb_new_name(s_cur);       /* 退出前自动存档 */
    wb_bmp_write(s_cur);
    input_set_swipe_back(true);
}

bool wb_exit_asking(void) { return s_exitask; }
void wb_exit_set(bool on) { s_exitask = on; }

/* 画单个图标 (s_icon[n], 20x20) 到 (ox,oy). inv=0: 位1黑/位0白底; inv=1: 反色(黑底白图标). */
static void wb_icon(st7305_handle_t *l, int n, int ox, int oy, int inv) {
    const uint8_t *p = s_icon[n];
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++) {
            int bit = (p[y*3 + (x>>3)] >> (7-(x&7))) & 1;
            if (inv) px(l, ox+x, oy+y, bit ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
            else     px(l, ox+x, oy+y, bit ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        }
}

/* 文字像素宽 (font_zh 24px, 中英文均 24px/字符) */
static int wb_zh_w(const char *s) {
    const uint8_t *p=(const uint8_t*)s; int w=0;
    while(*p){ w+=24; if((*p&0xf0)==0xe0) p+=3; else p++; }
    return w;
}
/* 绘制一行文字 (中文 font_zh_find_utf8 / ASCII font_zh_find_ascii), 返回像素宽 */
static int wb_draw_zh(st7305_handle_t *l, int x, int y, const char *str, st7305_color_t c) {
    const uint8_t *p=(const uint8_t*)str; int cx=x;
    while(*p){
        int idx;
        if((*p&0xf0)==0xe0){ idx=font_zh_find_utf8((const char*)p); p+=3; }
        else               { idx=font_zh_find_ascii(*p); p++; }
        if(idx>=0){
            const uint8_t*b=font_zh_get_bitmap_by_index(idx);
            if(b) for(int row=0;row<24;row++){ const uint8_t*src=b+row*3;
                for(int col=0;col<24;col++)
                    if((src[col>>3]>>(7-(col&7)))&1) px(l,cx+col,y+row,c); }
        }
        cx+=24;
    }
    return cx-x;
}

/* 确认退出浮层: 单行"退出画板？"小弹窗 (与系统 draw_notice_popup 统一风格) */
static void wb_draw_exitask(st7305_handle_t *l) {
    const char *t = "退出画板？";
    int lw = wb_zh_w(t);
    int x0 = (WB_W-lw)/2 - 6, x1 = (WB_W+lw)/2 + 6;
    int y0 = 138, y1 = y0 + 24 + 6;
    filr(l, x0, y0, x1, y1, ST7305_COLOR_WHITE);
    outline(l, x0, y0, x1, y1);
    wb_draw_zh(l, (WB_W-lw)/2, y0+3, t, ST7305_COLOR_BLACK);
}

/* 顶部操作条: 白底横框, 5 个中文文字按钮均分, 竖线分隔. 按下时整钮反色黑底白字. */
static void wb_draw_topbar(st7305_handle_t *l) {
    static const char *const lab[WB_OP_N] = {"撤销","重做","清空","保存","加载"};
    filr(l, 0, 0, WB_W-1, WB_TOP_H-1, ST7305_COLOR_WHITE);
    /* 横框: 上下边框线 */
    filr(l, 0, 0, WB_W-1, 0, ST7305_COLOR_BLACK);
    filr(l, 0, WB_TOP_H-1, WB_W-1, WB_TOP_H-1, ST7305_COLOR_BLACK);
    int w = (WB_W + WB_OP_N-1)/WB_OP_N;           /* 80 */
    for (int i = 0; i < WB_OP_N; i++) {
        int bx = i*w;
        if (i > 0) filr(l, bx, 2, bx, WB_TOP_H-3, ST7305_COLOR_BLACK); /* 竖线分隔 */
        st7305_color_t c;
        if (s_top_press == i) { filr(l, bx+1, 2, bx+w-2, WB_TOP_H-3, ST7305_COLOR_BLACK); c = ST7305_COLOR_WHITE; }
        else                  c = ST7305_COLOR_BLACK;
        int lw = wb_zh_w(lab[i]);
        wb_draw_zh(l, bx + (w-lw)/2, (WB_TOP_H-24)/2, lab[i], c);
    }
}

/* 左工具列: 白底, 前 6 槽工具图标(选中反色黑底白图标), 末槽(6)为粗细圆点. */
static void wb_draw_toolcol(st7305_handle_t *l) {
    filr(l, 0, WB_TOP_H, WB_TB_W-1, WB_H-1, ST7305_COLOR_WHITE);
    for (int i = 0; i <= WB_TOOL_N; i++) {
        int y = WB_TOP_H + i*WB_ROW_H;
        int oy = y + (WB_ROW_H-20)/2;
        if (i < WB_TOOL_N) {
            int sel = (i == s_tool);                     /* 选中工具反色 */
            if (sel) filr(l, 0, y, WB_TB_W-1, y+WB_ROW_H-1, ST7305_COLOR_BLACK);
            wb_icon(l, i, (WB_TB_W-20)/2, oy, sel);
        } else {                                          /* 粗细: 画当前粗细圆点 */
            int cx = WB_TB_W/2, cy = y + WB_ROW_H/2, r = 2 + s_size;
            if (r > 8) r = 8;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++)
                    if (dx*dx + dy*dy <= r*r)
                        px(l, cx+dx, cy+dy, ST7305_COLOR_BLACK);
        }
    }
}

static void wb_draw_toast(st7305_handle_t *l) {
    if (!s_toast[0]) return;
    uint32_t now = (uint32_t)(esp_timer_get_time()/1000);
    if (now - s_toast_ms > 1000) { s_toast[0] = 0; return; }
    int lw = wb_zh_w(s_toast);
    int x = (WB_W-lw)/2, y = 138, h = 24;
    filr(l, x-6, y-4, x+lw+6, y+h+2, ST7305_COLOR_WHITE);
    outline(l, x-6, y-4, x+lw+6, y+h+2);
    wb_draw_zh(l, x, y, s_toast, ST7305_COLOR_BLACK);
}

void wb_render(menu_state_t *st) {
    st7305_handle_t *l = st->lcd;
    /* 先整屏清白再画: 白板页独占全屏, 必须清掉其它界面(二级菜单/锁屏)残留帧.
     * 白板 st7305_blit_1bit 只写黑不写白, 不清屏会让旧画面留在白点处. */
    st7305_clear(l, ST7305_COLOR_WHITE);
    st7305_blit_1bit(l, 0, 0, WB_W, WB_H, s_canvas);
    wb_draw_topbar(l);
    wb_draw_toolcol(l);
    wb_draw_toast(l);
    if (s_exitask) wb_draw_exitask(l);
    else if (s_load) wb_draw_loader(l);
}

bool wb_touch(menu_state_t *st, int x, int y) {
    /* 绘制/工具点击全部由 wb_poll 用 input_get_touch_pos 连续跟踪处理,
     * 这里吞掉离散点击, 防止被当成 CONFIRM. */
    (void)st; (void)x; (void)y;
    return true;
}

void wb_action(menu_state_t *st, menu_action_t a) {
    switch (a) {
        case MENU_ACTION_RIGHT: s_tool = (s_tool+1) % WB_TOOL_N; break;
        case MENU_ACTION_LEFT:  s_tool = (s_tool==0) ? WB_TOOL_N-1 : s_tool-1; break;
        case MENU_ACTION_UP:    s_size = (s_size >= 6) ? 6 : s_size+1; break;
        case MENU_ACTION_DOWN:  s_size = (s_size <= 1) ? 1 : s_size-1; break;
        default: break;
    }
    st->needs_redraw = true;
}

void wb_poll(menu_state_t *st) {
    int tx, ty;

    if (s_exitask) return;   /* 确认退出浮层: 冻结绘制, 等按键 (确定退出/返回取消) */
    if (s_load)    { wb_loader_touch(st); return; }   /* 加载/删除列表: 触摸交互 */
    bool down = input_get_touch_pos(&tx, &ty);

    if (!down) {
        if (s_down && !s_free) {          /* 松开: 提交形状 */
            commit_shape(st->lcd);
            st->needs_redraw = true;
        }
        if (s_top_press >= 0) {           /* 松开清除顶部按下反馈 */
            s_top_press = -1;
            st->needs_redraw = true;
        }
        s_down = false;
        return;
    }

    if (!s_down) {
        /* 按下沿: 顶部操作 / 左侧工具则响应 (s_down 置位锁定, 防止按住重复触发) */
        if (in_top_bar(tx, ty)) {
            int o = op_btn(tx);
            s_top_press = o;
            handle_op(o, st);
            s_down = true;
            st->needs_redraw = true;
            return;
        }
        if (in_tool_col(tx, ty)) {
            int slot = tool_slot_btn(ty);
            if (slot == WB_TOOL_N) {      /* 粗细槽: 点击循环切换粗细 */
                s_size = (s_size >= 6) ? 1 : s_size + 1;
            } else {
                s_tool = slot;            /* 工具选择 */
            }
            s_down = true;
            st->needs_redraw = true;
            return;
        }
        if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) { s_down = false; return; } /* 画布外 */
        s_down = true;
        s_free = (s_tool==WB_PEN || s_tool==WB_MARK || s_tool==WB_ERAS);
        s_ax = s_lx = tx; s_ay = s_ly = ty;
        if (s_free) {
            push_undo();
            wb_disk(st->lcd, tx, ty, tool_radius(), tool_on());
            st->needs_redraw = true;
        }
        return;
    }

    /* 移动中 */
    if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) return;   /* 拖出画布仅停止 */
    if (s_free) {
        if (tx==s_lx && ty==s_ly) return;
        wb_line(st->lcd, s_lx,s_ly, tx,ty, tool_radius(), tool_on());
        s_lx = tx; s_ly = ty;
        st->needs_redraw = true;
    } else {
        s_lx = tx; s_ly = ty;    /* 形状预览在资源受限下延后到松开提交, 不实时上屏 */
    }
}