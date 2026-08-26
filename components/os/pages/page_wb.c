/**
 * page_wb.c — 白板 (画图) 页面模块 (P4 迁移: 从 menu/whiteboard.c 改签名为 ui_ctx + 私有 state).
 *
 * 铅笔/马克笔/橡皮/直线/矩形/椭圆 + 粗细, 撤销/重做 (位图快照栈),
 * 保存/加载 TF 卡 1-bit BMP, 自动持久化; 全屏绘图 (隐藏状态栏).
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
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
#define WB_BPR    ((WB_W+7)>>3)
#define WB_BYTES  (WB_H*WB_BPR)

#define WB_TB_W    64
#define WB_TOP_H   32
#define WB_CANV_H  (WB_H - WB_TOP_H)
#define WB_TOOL_N  6
#define WB_OP_N    5
#define WB_ICON_N  13
#define WB_ROW_H   (WB_CANV_H/(WB_TOOL_N+1))

#define WB_STK     10

enum { WB_PEN, WB_MARK, WB_ERAS, WB_LINE, WB_RECT, WB_OVAL };

static const uint8_t s_icon[WB_ICON_N][60] = {
    { /* pen */
        0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x0F,0x00,0x00,0x0F,0x00,0x00,0x00,0x40,0x00,0x00,0x40,
        0x00,0x00,0x40,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x10,0x00,0x00,
        0x10,0x00,0x00,0x10,0x00,0x00,0x08,0x00,0x00,0x3C,0x00,0x00,0x7E,0x00,0x00,0x00,0x80,0x00,0x00,0x00,
    },
    { /* mark */
        0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x07,0x80,0x00,0x07,0xC0,0x00,0x03,0xE0,0x00,0x01,0xE0,
        0x00,0x00,0xF0,0x00,0x00,0xF8,0x00,0x00,0x7C,0x00,0x00,0x3E,0x00,0x00,0x1F,0x00,0x00,0x0F,0x00,0x00,
        0x07,0x80,0x00,0x07,0xC0,0x00,0x03,0xE0,0x00,0x01,0xE0,0x00,0x00,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* eras */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,0xFF,0xC0,0x1F,
        0xFF,0xC0,0x1F,0xFF,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* line */
        0x00,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x02,0x00,
        0x00,0x01,0x00,0x00,0x00,0x80,0x00,0x00,0x60,0x00,0x00,0x10,0x00,0x00,0x08,0x00,0x00,0x04,0x00,0x00,
        0x02,0x00,0x00,0x01,0x00,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,0x00,0x01,0xF0,
    },
    { /* rect */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xFF,0xC0,0x10,0x00,0x40,0x10,0x00,
        0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x10,
        0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x1F,0xFF,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* oval */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00,0x03,0x06,
        0x00,0x0C,0x01,0x80,0x08,0x00,0x80,0x10,0x00,0x40,0x10,0x00,0x40,0x10,0x00,0x40,0x08,0x00,0x80,0x0C,
        0x01,0x80,0x03,0x06,0x00,0x00,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* undo */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x07,0x00,0x00,0x1F,0x00,0x00,0x3F,0xFF,
        0xC0,0x1F,0xFF,0xC0,0x0F,0x00,0x00,0x07,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* redo */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x0C,0x00,0x00,0x0E,0x00,0x3F,0xFF,
        0x00,0x3F,0xFF,0xC0,0x00,0x0F,0x00,0x00,0x0E,0x00,0x00,0x0C,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* size */
        0x00,0x00,0x00,0x00,0x00,0x00,0x1C,0x00,0x00,0x1C,0x00,0x00,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0xF0,0x00,0x00,0x00,0x00,0x00,0x03,0xE0,0x00,
        0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* fill */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xFF,0x80,0x10,0x00,0x80,0x10,0x00,0x80,0x10,0x00,
        0x80,0x10,0x00,0x80,0x10,0x00,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,
        0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* trash */
        0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x8C,0x00,0x01,0x8C,0x00,0x07,0xFF,0x00,0x07,0xFF,0x00,0x0F,0xFF,
        0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,0xCC,0x80,0x08,
        0xCC,0x80,0x0F,0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* save */
        0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,
        0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x09,0xFC,0x80,0x08,0x00,0x80,0x09,0xFC,0x80,0x09,0x04,0x80,0x09,
        0x04,0x80,0x09,0x04,0x80,0x09,0xFC,0x80,0x08,0x00,0x80,0x0F,0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    },
    { /* load */
        0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x60,0x00,0x00,0xF0,0x00,0x01,0xF8,0x00,0x03,0xFC,0x00,0x03,0xFE,
        0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x00,
        0x70,0x00,0x00,0x70,0x00,0x00,0x70,0x00,0x3F,0xFF,0xE0,0x3F,0xFF,0xE0,0x3F,0xFF,0xE0,0x00,0x00,0x00,
    },
};

EXT_RAM_BSS_ATTR static uint8_t s_canvas[WB_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_undo[WB_STK][WB_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_redo[WB_STK][WB_BYTES];
static int  s_undo_n = 0, s_redo_n = 0;

static int  s_tool  = WB_PEN;
static int  s_size  = 1;
static bool s_down  = false;
static bool s_free  = false;
static int  s_lx, s_ly;
static int  s_ax, s_ay;
static bool s_full  = true;
static bool s_inited = false;

static char   s_toast[22] = {0};
static uint32_t s_toast_ms = 0;
static int   s_top_press = -1;
static bool  s_exitask = false;

#define WB_FILE_MAX 40
EXT_RAM_BSS_ATTR static int    s_nfiles;
EXT_RAM_BSS_ATTR static char   s_names[WB_FILE_MAX][36];
EXT_RAM_BSS_ATTR static char   s_paths[WB_FILE_MAX][64];
EXT_RAM_BSS_ATTR static char   s_cur[64];
static bool   s_load    = false;
static int    s_lsel    = 0;
static bool   s_delask  = false;
static int    s_deln    = 0;

/* ================= 通用绘制 ================= */
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

static inline void wb_bit(int x, int y, int on) {
    uint8_t *b = &s_canvas[y*WB_BPR + (x>>3)];
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (on) *b |= m; else *b &= ~m;
}
static void wb_ppx(st7305_handle_t *l, int x, int y, int on) {
    if (x < WB_TB_W || x >= WB_W || y < WB_TOP_H || y >= WB_H) return;
    wb_bit(x, y, on);
    st7305_draw_pixel(l, x, y, on ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
}
static void wb_disk(st7305_handle_t *l, int cx, int cy, int r, int on) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                wb_ppx(l, cx+dx, cy+dy, on);
}
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
    if (s_undo_n < WB_STK) memcpy(s_undo[s_undo_n++], s_canvas, WB_BYTES);
    else { memmove(s_undo[0], s_undo[1], (WB_STK-1)*WB_BYTES); memcpy(s_undo[WB_STK-1], s_canvas, WB_BYTES); }
    s_redo_n = 0;
}
static void wb_undo(ui_ctx_t *ctx) {
    if (s_undo_n <= 0) return;
    if (s_redo_n < WB_STK) memcpy(s_redo[s_redo_n++], s_canvas, WB_BYTES);
    memcpy(s_canvas, s_undo[--s_undo_n], WB_BYTES);
    s_down = false; s_full = true; ctx->needs_redraw = true;
}
static void wb_redo(ui_ctx_t *ctx) {
    if (s_redo_n <= 0) return;
    if (s_undo_n < WB_STK) memcpy(s_undo[s_undo_n++], s_canvas, WB_BYTES);
    memcpy(s_canvas, s_redo[--s_redo_n], WB_BYTES);
    s_down = false; s_full = true; ctx->needs_redraw = true;
}

static int tool_radius(void) {
    switch (s_tool) {
        case WB_PEN:  return s_size ? s_size : 1;
        case WB_MARK: return s_size*2;
        case WB_ERAS: return s_size + 4;
        default:      return 1;
    }
}
static int tool_on(void) { return (s_tool == WB_ERAS) ? 0 : 1; }

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

static int in_top_bar(int x, int y) { return (y < WB_TOP_H && x >= 0 && x < WB_W); }
static int in_tool_col(int x, int y) { return (x < WB_TB_W && y >= WB_TOP_H && y < WB_H); }
static int op_btn(int x) { int w = (WB_W + WB_OP_N-1)/WB_OP_N; int i = x / w; return (i >= WB_OP_N) ? WB_OP_N-1 : i; }
static int tool_slot_btn(int y) { int i = (y - WB_TOP_H) / WB_ROW_H; return (i >= WB_TOOL_N+1) ? WB_TOOL_N : i; }

static void toast(const char *s) {
    strncpy(s_toast, s, sizeof(s_toast)-1);
    s_toast[sizeof(s_toast)-1] = 0;
    s_toast_ms = (uint32_t)(esp_timer_get_time()/1000);
}

static void wb_open_loader(ui_ctx_t *ctx);
static bool wb_bmp_write(const char *path);
static void wb_new_name(char *out);
static int  wb_zh_w(const char *s);
static int  wb_draw_zh(st7305_handle_t *l, int x, int y, const char *str, st7305_color_t c);
static void wb_icon(st7305_handle_t *l, int n, int ox, int oy, int inv);

static void handle_op(int op, ui_ctx_t *ctx) {
    switch (op) {
        case 0: wb_undo(ctx); return;
        case 1: wb_redo(ctx); return;
        case 2: push_undo(); memset(s_canvas, 0, WB_BYTES); s_full = true; break;
        case 3:
            wb_new_name(s_cur);
            if (wb_bmp_write(s_cur)) toast("\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); /* 已保存 */
            else                    toast("\xe6\x97\xa0TF\xe5\x8d\xa1");           /* 无TF卡 */
            break;
        case 4: wb_open_loader(ctx); return;
        default: break;
    }
    ctx->needs_redraw = true;
}

/* ================= 保存/加载 (1-bit BMP) ================= */
#define WB_DIR   "/sdcard"
static const int s_bmp_off = 0x3E;

static bool wb_bmp_write(const char *path) {
    if (!sd_is_mounted()) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int stride = ((WB_W + 31) / 32) * 4;
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
static int wb_file_seq(const char *base) {
    if (strncmp(base, "wb", 2) || strlen(base) < 10) return -1;
    const char *dot = strrchr(base, '.');
    if (!dot || strcmp(dot, ".bmp")) return -1;
    return atoi(base + 6);
}
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
    for (int i = 0; i < n-1; i++)
        for (int j = 0; j < n-1-i; j++) {
            const char *a = strrchr(tmp[j],'/')+1, *b = strrchr(tmp[j+1],'/')+1;
            if (wb_file_seq(a) < wb_file_seq(b)) { char s64[64]; strcpy(s64,tmp[j]); strcpy(tmp[j],tmp[j+1]); strcpy(tmp[j+1],s64); }
        }
    for (int i = 0; i < n; i++) {
        strcpy(s_paths[i], tmp[i]);
        const char *base = strrchr(tmp[i],'/')+1;
        snprintf(s_names[i], 36, "%.2s:%.2s #%.4s", base+2, base+4, base+6);
        s_nfiles = i+1;
    }
}
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

/* ================= 加载列表模态 ================= */
#define LD_X0    8
#define LD_Y0    (WB_TOP_H + 4)
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

static void wb_open_loader(ui_ctx_t *ctx) {
    wb_list();
    if (s_nfiles == 0) { toast("\xe6\x97\xa0\xe5\xad\x98\xe6\xa1\xa3"); ctx->needs_redraw = true; return; } /* 无存档 */
    s_top_press = -1; s_lsel = 0; s_load = true; s_delask = false;
    ctx->needs_redraw = true;
}

static void wb_draw_loader(st7305_handle_t *l) {
    int bx = LD_X0, ty0 = LD_Y0;
    filr(l, bx-2, ty0-2, WB_W-8+2, LD_YB, ST7305_COLOR_WHITE);
    outline(l, bx-2, ty0-2, WB_W-8+2, LD_YB);
    wb_draw_zh(l, bx, ty0, "\xe6\x89\x93\xe5\xbc\x80\xe7\x99\xbd\xe6\x9d\xbf", ST7305_COLOR_BLACK); /* 打开白板 */
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
    wb_draw_zh(l, bx, LD_YB - 26, "\xe9\x95\xbf\xe6\x8c\x89\xe5\x88\xa0\xe9\x99\xa4 / \xe8\xbf\x94\xe5\x9b\x9e\xe5\x8f\x96\xe6\xb6\x88", ST7305_COLOR_BLACK); /* 长按删除 / 返回取消 */
    if (s_delask) {
        const char *tk = "\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe7\x99\xbd\xe6\x9d\xbf?"; /* 删除该白板? */
        const char *sy = "\xe5\x88\xa0\xe9\x99\xa4", *no = "\xe5\x8f\x96\xe6\xb6\x88"; /* 删除/取消 */
        int w = wb_zh_w(tk) + 24;
        int x0 = (WB_W-w)/2, x1 = x0+w-1;
        int ay0 = 128, ay1 = 188;
        filr(l, x0, ay0, x1, ay1, ST7305_COLOR_WHITE);
        outline(l, x0, ay0, x1, ay1);
        wb_draw_zh(l, (WB_W-wb_zh_w(tk))/2, ay0+8, tk, ST7305_COLOR_BLACK);
        int bx0 = x0+10, bwx = (w-40-10)/2;
        filr(l, bx0, ay1-34, bx0+bwx-1, ay1-6, ST7305_COLOR_BLACK);
        wb_draw_zh(l, bx0+(bwx-wb_zh_w(sy))/2, ay1-30, sy, ST7305_COLOR_WHITE);
        int bx1 = x0+w-10-bwx;
        outline(l, bx1, ay1-34, bx1+bwx-1, ay1-6);
        wb_draw_zh(l, bx1+(bwx-wb_zh_w(no))/2, ay1-30, no, ST7305_COLOR_BLACK);
    }
}

static void wb_loader_touch(ui_ctx_t *ctx) {
    int tx, ty; bool down = input_get_touch_pos(&tx, &ty);
    static bool last = false; static int ox = 0, oy = 0;
    int y0 = LD_Y0 + LD_TITLE_H, rows = (LD_YB - y0) / LD_ROW;
    if (!down) {
        if (last) {
            if (s_delask) {
                const char *tk = "\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe7\x99\xbd\xe6\x9d\xbf?";
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
            ctx->needs_redraw = true;
        }
        last = false; return;
    }
    if (!last) { ox = tx; oy = ty; last = true; }
    else if (!s_delask) {
        uint32_t hold = input_touch_hold_ms();
        if (hold >= 800) {
            int row = (oy - y0) / LD_ROW;
            int shown = s_nfiles < rows ? s_nfiles : rows;
            if (row >= 0 && row < shown) { s_deln = row; s_lsel = row; s_delask = true; ctx->needs_redraw = true; }
        }
    }
}

/* ================= 渲染 ================= */
static int wb_zh_w(const char *s) {
    const uint8_t *p=(const uint8_t*)s; int w=0;
    while(*p){ w+=24; if((*p&0xf0)==0xe0) p+=3; else p++; }
    return w;
}
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

static void wb_draw_exitask(st7305_handle_t *l) {
    const char *t = "\xe9\x80\x80\xe5\x87\xba\xe7\x94\xbb\xe6\x9d\xbf\xef\xbc\x9f"; /* 退出画板？ */
    int lw = wb_zh_w(t);
    int x0 = (WB_W-lw)/2 - 6, x1 = (WB_W+lw)/2 + 6;
    int y0 = 138, y1 = y0 + 24 + 6;
    filr(l, x0, y0, x1, y1, ST7305_COLOR_WHITE);
    outline(l, x0, y0, x1, y1);
    wb_draw_zh(l, (WB_W-lw)/2, y0+3, t, ST7305_COLOR_BLACK);
}

static void wb_draw_topbar(st7305_handle_t *l) {
    static const char *const lab[WB_OP_N] = {"\xe6\x92\xa4\xe9\x94\x80","\xe9\x87\x8d\xe5\x81\x9a","\xe6\xb8\x85\xe7\xa9\xba","\xe4\xbf\x9d\xe5\xad\x98","\xe5\x8a\xa0\xe8\xbd\xbd"}; /* 撤销/重做/清空/保存/加载 */
    filr(l, 0, 0, WB_W-1, WB_TOP_H-1, ST7305_COLOR_WHITE);
    filr(l, 0, 0, WB_W-1, 0, ST7305_COLOR_BLACK);
    filr(l, 0, WB_TOP_H-1, WB_W-1, WB_TOP_H-1, ST7305_COLOR_BLACK);
    int w = (WB_W + WB_OP_N-1)/WB_OP_N;
    for (int i = 0; i < WB_OP_N; i++) {
        int bx = i*w;
        if (i > 0) filr(l, bx, 2, bx, WB_TOP_H-3, ST7305_COLOR_BLACK);
        st7305_color_t c;
        if (s_top_press == i) { filr(l, bx+1, 2, bx+w-2, WB_TOP_H-3, ST7305_COLOR_BLACK); c = ST7305_COLOR_WHITE; }
        else                  c = ST7305_COLOR_BLACK;
        int lw = wb_zh_w(lab[i]);
        wb_draw_zh(l, bx + (w-lw)/2, (WB_TOP_H-24)/2, lab[i], c);
    }
}

static void wb_draw_toolcol(st7305_handle_t *l) {
    filr(l, 0, WB_TOP_H, WB_TB_W-1, WB_H-1, ST7305_COLOR_WHITE);
    for (int i = 0; i <= WB_TOOL_N; i++) {
        int y = WB_TOP_H + i*WB_ROW_H;
        int oy = y + (WB_ROW_H-20)/2;
        if (i < WB_TOOL_N) {
            int sel = (i == s_tool);
            if (sel) filr(l, 0, y, WB_TB_W-1, y+WB_ROW_H-1, ST7305_COLOR_BLACK);
            wb_icon(l, i, (WB_TB_W-20)/2, oy, sel);
        } else {
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

static void wb_icon(st7305_handle_t *l, int n, int ox, int oy, int inv) {
    const uint8_t *p = s_icon[n];
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++) {
            int bit = (p[y*3 + (x>>3)] >> (7-(x&7))) & 1;
            if (inv) px(l, ox+x, oy+y, bit ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
            else     px(l, ox+x, oy+y, bit ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        }
}

static void p_wb_render(ui_ctx_t *ctx) {
    st7305_handle_t *l = ctx->lcd;
    ctx->fullscreen = true;
    st7305_clear(l, ST7305_COLOR_WHITE);
    st7305_blit_1bit(l, 0, 0, WB_W, WB_H, s_canvas);
    wb_draw_topbar(l);
    wb_draw_toolcol(l);
    wb_draw_toast(l);
    if (s_exitask) wb_draw_exitask(l);
    else if (s_load) wb_draw_loader(l);
}

/* ================= 模块接口 ================= */
static void wb_reset(ui_ctx_t *ctx) {
    input_set_swipe_back(false);
    if (!s_inited) {
        memset(s_canvas, 0, WB_BYTES);
        memset(s_undo, 0, sizeof(s_undo));
        memset(s_redo, 0, sizeof(s_redo));
        s_inited = true;
    }
    s_tool = WB_PEN; s_size = 1;
    s_down = false; s_free = false;
    s_undo_n = 0; s_redo_n = 0;
    s_load = false; s_delask = false; s_exitask = false;
    s_cur[0] = 0;
    wb_list();
    if (s_nfiles > 0) {
        strcpy(s_cur, s_paths[0]);
        wb_bmp_read(s_cur);
    } else {
        memset(s_canvas, 0, WB_BYTES);
    }
    ctx->needs_redraw = true;
}

static void wb_exit(ui_ctx_t *ctx) {
    (void)ctx;
    if (!s_cur[0]) wb_new_name(s_cur);
    wb_bmp_write(s_cur);
    input_set_swipe_back(true);
}

static bool p_wb_touch(ui_ctx_t *ctx, int x, int y) {
    (void)ctx; (void)x; (void)y;
    return true;
}

static void p_wb_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_exitask) {
        /* 确认退出浮层: 确认=退出, 返回/取消=关闭 */
        if (a == OS_ACTION_CONFIRM) { s_exitask = false; os_pop(ctx); return; }
        if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { s_exitask = false; ctx->needs_redraw = true; return; }
        return;
    }
    if (s_load) {
        if (s_delask) {
            if (a == OS_ACTION_CONFIRM) { wb_del_entry(s_deln); }
            else if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { s_delask = false; }
        } else {
            switch (a) {
                case OS_ACTION_UP:    if (s_lsel > 0) s_lsel--; break;
                case OS_ACTION_DOWN:  if (s_lsel < s_nfiles-1) s_lsel++; break;
                case OS_ACTION_CONFIRM: wb_do_load(s_lsel); break;
                case OS_ACTION_BACK:  s_load = false; break;
                default: break;
            }
        }
        ctx->needs_redraw = true;
        return;
    }
    switch (a) {
        case OS_ACTION_RIGHT: s_tool = (s_tool+1) % WB_TOOL_N; break;
        case OS_ACTION_LEFT:  s_tool = (s_tool==0) ? WB_TOOL_N-1 : s_tool-1; break;
        case OS_ACTION_UP:    s_size = (s_size >= 6) ? 6 : s_size+1; break;
        case OS_ACTION_DOWN:  s_size = (s_size <= 1) ? 1 : s_size-1; break;
        case OS_ACTION_BACK:  s_exitask = true; break;   /* 返回: 确认退出浮层 */
        default: break;
    }
    ctx->needs_redraw = true;
}

static void p_wb_poll(ui_ctx_t *ctx) {
    int tx, ty;
    if (s_exitask) return;
    if (s_load)    { wb_loader_touch(ctx); return; }
    bool down = input_get_touch_pos(&tx, &ty);

    if (!down) {
        if (s_down && !s_free) { commit_shape(ctx->lcd); ctx->needs_redraw = true; }
        if (s_top_press >= 0) { s_top_press = -1; ctx->needs_redraw = true; }
        s_down = false;
        return;
    }
    if (!s_down) {
        if (in_top_bar(tx, ty)) {
            int o = op_btn(tx);
            s_top_press = o;
            handle_op(o, ctx);
            s_down = true;
            ctx->needs_redraw = true;
            return;
        }
        if (in_tool_col(tx, ty)) {
            int slot = tool_slot_btn(ty);
            if (slot == WB_TOOL_N) { s_size = (s_size >= 6) ? 1 : s_size + 1; }
            else { s_tool = slot; }
            s_down = true;
            ctx->needs_redraw = true;
            return;
        }
        if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) { s_down = false; return; }
        s_down = true;
        s_free = (s_tool==WB_PEN || s_tool==WB_MARK || s_tool==WB_ERAS);
        s_ax = s_lx = tx; s_ay = s_ly = ty;
        if (s_free) { push_undo(); wb_disk(ctx->lcd, tx, ty, tool_radius(), tool_on()); ctx->needs_redraw = true; }
        return;
    }
    if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) return;
    if (s_free) {
        if (tx==s_lx && ty==s_ly) return;
        wb_line(ctx->lcd, s_lx,s_ly, tx,ty, tool_radius(), tool_on());
        s_lx = tx; s_ly = ty;
        ctx->needs_redraw = true;
    } else {
        s_lx = tx; s_ly = ty;
    }
}

static const os_module_t s_mod_wb = {
    .name       = "wb",
    .page_id    = OS_PAGE_WHITEBOARD,
    .on_enter   = wb_reset,
    .on_exit    = wb_exit,
    .render     = p_wb_render,
    .action     = p_wb_action,
    .touch      = p_wb_touch,
    .poll       = p_wb_poll,
    .fullscreen = true,
};

void os_page_wb_register(void) { os_register(&s_mod_wb); }
