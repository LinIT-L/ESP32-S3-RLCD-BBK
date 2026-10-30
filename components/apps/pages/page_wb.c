/**
 * page_wb.c — 白板 (画图) 页面模块 (P4 迁移: 从 menu/whiteboard.c 改签名为 ui_ctx + 私有 state).
 *
 * 铅笔/马克笔/橡皮/直线/矩形/椭圆 + 粗细, 撤销/重做 (命令日志 + 基线快照),
 * 保存/加载 TF 卡 1-bit BMP, 自动持久化; 全屏绘图 (隐藏状态栏).
 * 私有 state 全部 static 留本文件.
 *
 * ★ 2026-10-30 一次性修复 (A1/A2/A3/A4/A5/A7/A10 + A8/A9 + B1/B2/B4/B7):
 *   - 笔迹点坐标 uint8 → uint16: 原 x>255 / y>255 的点在撤销重放时被截断错位;
 *   - 点池 768 → 2048 点, 单笔点数 uint8 → uint16: 原 >255 点会回绕成 0 → 整笔丢失;
 *   - 所有命令统一走 wb_commit(): 日志满先压基线、新操作一律截断 redo
 *     (原 redo 日志从不清空 → 撤销后新画再重做会重放陈旧命令, 出现幽灵笔迹);
 *   - arena 分配失败 → 硬失败态: 原实现回退到 8 字节 dummy, 随后 15KB 读写直接踩内存;
 *   - 存档目录 mkdir 改对路径 + tmp+rename 原子落盘 + 加载失败保留原画布;
 *   - 顶栏改"抬起才执行"; 形状拖动有橡皮筋预览; 工具栏填充改批量行刷 (B4)。
 */
#include "os.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui_common.h"
#include "input.h"
#include "gesture_back.h"  /* 底部上滑=返回 的开关: gesture_back_enable(false/true) */
#include "sd_scan.h"
#include "font_zh.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define WB_W      ST7305_WIDTH          /* 400 */
#define WB_H      ST7305_HEIGHT         /* 300 */
#define WB_BPR    ((WB_W+7)>>3)
#define WB_BYTES  (WB_H*WB_BPR)

#define WB_TB_W    64
#define WB_TOP_H   32
#define WB_CANV_W  (WB_W - WB_TB_W)     /* 336: 可用画布宽 */
#define WB_CANV_H  (WB_H - WB_TOP_H)    /* 268: 可用画布高 */
#define WB_TOOL_N  6
#define WB_OP_N    5
#define WB_ICON_N  13
#define WB_ROW_H   (WB_CANV_H/(WB_TOOL_N+1))

#define WB_LOG_N   24      /* undo/redo 命令日志条数上限 (满则把画面压成基线快照) */
#define WB_PT_MAX  2048    /* 笔迹点池容量 (点); 每点 2 个 uint16 → 8,192B */
#define WB_PT_TOTAL (WB_PT_MAX * 2)

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

/* ==== 撤销/重做: 命令日志 + 单张基线快照 ====
 * 旧实现用 s_undo/s_redo 各 10 层 × 整幅位图 = 300KB PSRAM.
 * 改为命令日志式: 每步只记录"参数命令"(WB_CMD_*, 工具/笔粗/坐标点),
 * 攒够 WB_LOG_N 步就把当前画布压成唯一基线快照 s_snap(15KB) 并清空日志。
 * 撤销/重做 = 从快照复制 + 重放命令.
 *
 * ★ 2026-10-30 统一容量模型 (A1/A4/A7/A10) —— 画布/快照/日志/点池必须始终自洽:
 *   内存 = 画布 15,000 + 快照 15,000 + 点池 2048×4=8,192 + 双日志 24×16×2=768
 *        = 38,960B (一次 calloc, 全在 PSRAM)
 *   容量检查只有两处, 其余路径一律不管容量:
 *     ① wb_reserve_log(): 命令入日志**之前**调用。日志满 → wb_compress() 把"当前画面"
 *        压成基线并清空日志/点池 —— 时机在画本笔之前, 所以旧命令引用的点不会被覆盖;
 *     ② 笔迹点写入: 点池剩余 <2 个 uint16 时不再追加, 本笔标记 truncated,
 *        抬笔时并入基线(不进撤销) —— 宁可少一步撤销, 也不能出现半截笔迹。
 *   ⚠ wb_commit() 自身**不做**压缩: 压缩会清空点池, 那样 s_pend.pts_idx 立刻失效。 */
#define WB_CMD_CLEAR  0   /* 清屏 */
#define WB_CMD_FREE   1   /* 自由笔/标记/橡皮: 记录笔迹点列 */
#define WB_CMD_SHAPE  2   /* 直线/矩形/椭圆 */

typedef struct {
    uint8_t  type;     /* WB_CMD_* */
    uint8_t  tool;     /* WB_* 工具 */
    uint8_t  size;     /* 笔粗/尺寸 (1..6) */
    uint8_t  pad;      /* 对齐填充, 恒 0 */
    uint16_t n;        /* FREE: 总点数 (0=无点); 原 uint8 超 255 会回绕成 0 致整笔丢失 */
    uint16_t pts_idx;  /* FREE: 笔迹点在 pts[] 中的起始下标 (uint16 元素, 非字节) */
    int16_t  x0, y0;   /* FREE: 首个点; SHAPE: 起点 */
    int16_t  x1, y1;   /* FREE: 末个点; SHAPE: 终点 */
} wb_cmd_t;            /* = 16B */

typedef struct {
    uint8_t  canvas[WB_BYTES];
    uint8_t  snap[WB_BYTES];
    uint16_t pts[WB_PT_TOTAL];      /* x,y 逐点存 uint16: 原 uint8 会截断 x>255 / y>255 */
    wb_cmd_t ulog[WB_LOG_N];
    wb_cmd_t rlog[WB_LOG_N];
} wb_arena_t;

/* ★★★ 2026-10-25 内存优化 (linit): 画布+快照+笔迹点+撤销/重做日志 原来静态常驻 PSRAM,
 *   只有打开"画板"才用得上 ⇒ 首次访问时一次性分配。写法 (s_canvas[i] 等) 保持不变。 */
static wb_arena_t *s_ar;
static bool s_alloc_failed;

static bool wb_alloc(void) {
    if (s_ar) return true;
    if (s_alloc_failed) return false;
    s_ar = (wb_arena_t *)heap_caps_calloc(1, sizeof(wb_arena_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ar) {
        /* A2: 硬失败。原实现返回 8 字节 dummy, 随后 memset(…,15000) / blit(…,15000B)
         * 会越界踩穿相邻静态内存 —— 那种"看起来还能用"的兜底比直接失败危险得多。 */
        s_alloc_failed = true;
        ESP_LOGE("WB", "白板缓冲分配失败 (%u B) → 本次进入禁用绘图/存盘",
                 (unsigned)sizeof(wb_arena_t));
        return false;
    }
    ESP_LOGI("WB", "白板缓冲 %u B (PSRAM)", (unsigned)sizeof(wb_arena_t));
    return true;
}
#define s_canvas (s_ar->canvas)
#define s_snap   (s_ar->snap)
#define s_pts    (s_ar->pts)
#define s_ulog   (s_ar->ulog)
#define s_rlog   (s_ar->rlog)

static int  s_ulog_n = 0, s_rlog_n = 0, s_pts_n = 0;

static int  s_tool  = WB_PEN;
static int  s_size  = 1;
static bool s_down  = false;
static bool s_free  = false;
static bool s_drawing = false; /* 本次按下是否落在画布上 (顶栏/工具列按下不算, 避免误落笔) */
static int  s_lx, s_ly;
static int  s_ax, s_ay;
static bool s_pv    = false;   /* 预览态: 只画屏不写画布 (形状拖动橡皮筋) */

static int   s_top_press = -1; /* 顶栏按下的按钮号 (-1=无); B2: 抬起才执行 */
static int   s_top_px = 0, s_top_py = 0;

/* 正在绘制的自由笔命令 (落笔到抬笔之间攒点, 抬笔时才入日志) */
static wb_cmd_t s_pend;
static bool     s_pend_trunc;  /* 点池不足 → 本笔不完整, 抬笔时并入基线 */

#define WB_FILE_MAX 40
EXT_RAM_BSS_ATTR static int    s_nfiles;
EXT_RAM_BSS_ATTR static char   s_names[WB_FILE_MAX][36];
EXT_RAM_BSS_ATTR static char   s_paths[WB_FILE_MAX][64];
EXT_RAM_BSS_ATTR static char   s_cur[64];

/* ================= 通用绘制 ================= */
static void px(st7305_handle_t *l, int x, int y, st7305_color_t c) {
    if (x < 0 || x >= WB_W || y < 0 || y >= WB_H) return;
    st7305_draw_pixel(l, x, y, c);
}
/* B4: 实心矩形批量刷。原实现逐像素调 st7305_draw_pixel (顶栏+工具列 ≈ 3 万次/帧),
 * 现按行用整行 0xFF 瓦片走 blit_1bit / blit_1bit_white (每行 1 次调用, 每像素只做位运算)。
 * 输出像素与原 filr 完全等价 (只有全黑/全白两种, 无灰阶), 因此画面不变。 */
static const uint8_t s_fill_ff[WB_BPR] = { [0 ... WB_BPR - 1] = 0xFF };
static void filr(st7305_handle_t *l, int x0, int y0, int x1, int y1, st7305_color_t c) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= WB_W) x1 = WB_W - 1;
    if (y1 >= WB_H) y1 = WB_H - 1;
    int w = x1 - x0 + 1;
    if (w <= 0 || y1 < y0) return;
    bool black = (c == ST7305_COLOR_BLACK);
    for (int y = y0; y <= y1; y++) {
        if (black) st7305_blit_1bit(l, x0, y, w, 1, s_fill_ff);
        else       st7305_blit_1bit_white(l, x0, y, w, 1, s_fill_ff);
    }
}

static inline void wb_bit(int x, int y, int on) {
    uint8_t *b = &s_canvas[y*WB_BPR + (x>>3)];
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (on) *b |= m; else *b &= ~m;
}
static void wb_ppx(st7305_handle_t *l, int x, int y, int on) {
    if (x < WB_TB_W || x >= WB_W || y < WB_TOP_H || y >= WB_H) return;
    if (!s_pv) wb_bit(x, y, on);   /* 预览态: 只画屏, 不污染画布 (B1) */
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

/* ================= 撤销/重做 (命令日志 + 单张基线快照) ================= */

/* 各工具的等效半径 —— 实时绘制与撤销重放必须用**同一套** (A5)。
 * 原重放取 r=size, 而实时橡皮是 size+4 ⇒ 撤销/重做后橡皮痕迹会变窄, 画面不一致。 */
static int wb_radius(int tool, int size) {
    if (size < 1) size = 1;
    switch (tool) {
        case WB_PEN:  return size;
        case WB_MARK: return size * 2;
        case WB_ERAS: return size + 4;
        default:      return 1;
    }
}
static int tool_radius(void) { return wb_radius(s_tool, s_size); }
static int tool_on(void) { return (s_tool == WB_ERAS) ? 0 : 1; }

/* 形状几何: 实时落笔 与 撤销重放 共用一份, 避免两处算法漂移 */
static void wb_draw_shape(st7305_handle_t *l, int tool, int x0, int y0, int x1, int y1) {
    switch (tool) {
        case WB_LINE: wb_line(l, x0, y0, x1, y1, 1, 1); break;
        case WB_RECT: {
            int lx = x0 < x1 ? x0 : x1, rx = x0 < x1 ? x1 : x0;
            int ty = y0 < y1 ? y0 : y1, by = y0 < y1 ? y1 : y0;
            wb_line(l, lx, ty, rx, ty, 1, 1); wb_line(l, lx, by, rx, by, 1, 1);
            wb_line(l, lx, ty, lx, by, 1, 1); wb_line(l, rx, ty, rx, by, 1, 1);
            break;
        }
        case WB_OVAL: {
            int dx = x1 - x0, dy = y1 - y0;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            int rx = dx/2 + 1, ry = dy/2 + 1;
            int cx = (x0 + x1)/2, cy = (y0 + y1)/2;
            if (rx < 1) rx = 1;
            if (ry < 1) ry = 1;
            wb_outline_ellipse(l, cx, cy, rx, ry);
            break;
        }
        default: break;
    }
}

/* 重放一条命令到 (内存) 画布. FREE 逐段连线; SHAPE 走同一份几何 */
static void wb_replay_cmd(st7305_handle_t *l, const wb_cmd_t *c) {
    if (!c) return;
    if (c->type == WB_CMD_CLEAR) { memset(s_canvas, 0, WB_BYTES); return; }
    if (c->type == WB_CMD_FREE) {
        if (c->n == 0) return;                                        /* 无点 = 无法重放 */
        if ((uint32_t)c->pts_idx + (uint32_t)c->n * 2u > WB_PT_TOTAL) return;  /* 越界保护 */
        int on = (c->tool == WB_ERAS) ? 0 : 1;
        int r  = wb_radius(c->tool, c->size);
        int x  = s_pts[c->pts_idx];
        int y  = s_pts[c->pts_idx + 1];
        wb_disk(l, x, y, r, on);
        for (uint16_t i = 1; i < c->n; i++) {
            int nx = s_pts[(uint32_t)c->pts_idx + (uint32_t)i * 2u];
            int ny = s_pts[(uint32_t)c->pts_idx + (uint32_t)i * 2u + 1u];
            wb_line(l, x, y, nx, ny, r, on);
            x = nx; y = ny;
        }
        return;
    }
    wb_draw_shape(l, c->tool, c->x0, c->y0, c->x1, c->y1);
}

/* 把画布从 [snap + 前 n 条命令] 重放重建 */
static void wb_rebuild(st7305_handle_t *l, int n) {
    memcpy(s_canvas, s_snap, WB_BYTES);
    if (n > s_ulog_n) n = s_ulog_n;          /* 边界复核: 绝不重放越界条目 */
    for (int i = 0; i < n; i++) wb_replay_cmd(l, &s_ulog[i]);
}

/* 压缩: 当前画面 → 基线快照, 清空日志/redo/点池 (三者必须同生共死) */
static void wb_compress(void) {
    memcpy(s_snap, s_canvas, WB_BYTES);
    s_ulog_n = 0;
    s_rlog_n = 0;
    s_pts_n  = 0;
}

/* 命令入日志**之前**调用: 满了就先把当前画面压成基线 (A4) */
static void wb_reserve_log(void) {
    if (s_ulog_n >= WB_LOG_N) wb_compress();
}

/* 提交一条命令。前提: 调用方已 wb_reserve_log(), FREE 的点已写进点池。
 * ⚠ 本函数不做压缩 —— 压缩会清空点池, 那样 s_pend.pts_idx 立刻失效。 */
static void wb_commit(const wb_cmd_t *c) {
    if (!c || s_ulog_n >= WB_LOG_N) return;  /* 兜底: 宁可不记也不越界 */
    s_ulog[s_ulog_n++] = *c;
    s_rlog_n = 0;                            /* A3: 新操作截断 redo 分支 */
}

static void wb_undo(ui_ctx_t *ctx) {
    if (!s_ar || s_ulog_n <= 0 || s_rlog_n >= WB_LOG_N) return;
    s_rlog[s_rlog_n++] = s_ulog[--s_ulog_n];  /* 被撤销命令转入 redo 日志 */
    wb_rebuild(ctx->lcd, s_ulog_n);
    s_down = false; s_free = false; s_drawing = false;
    ctx->needs_redraw = true;
}
static void wb_redo(ui_ctx_t *ctx) {
    if (!s_ar || s_rlog_n <= 0 || s_ulog_n >= WB_LOG_N) return;
    s_ulog[s_ulog_n++] = s_rlog[--s_rlog_n];  /* 命令放回撤销日志 */
    wb_replay_cmd(ctx->lcd, &s_ulog[s_ulog_n - 1]);
    s_down = false; s_free = false; s_drawing = false;
    ctx->needs_redraw = true;
}

static int in_top_bar(int x, int y) { return (y < WB_TOP_H && x >= 0 && x < WB_W); }
static int in_tool_col(int x, int y) { return (x < WB_TB_W && y >= WB_TOP_H && y < WB_H); }
static int op_btn(int x) { int w = (WB_W + WB_OP_N-1)/WB_OP_N; int i = x / w; return (i >= WB_OP_N) ? WB_OP_N-1 : i; }
static int tool_slot_btn(int y) { int i = (y - WB_TOP_H) / WB_ROW_H; return (i >= WB_TOOL_N+1) ? WB_TOOL_N : i; }

static void wb_open_loader(ui_ctx_t *ctx);
static bool wb_bmp_write(const char *path);
static void wb_new_name(char *out);
static int  wb_zh_w(const char *s);
static int  wb_draw_zh(st7305_handle_t *l, int x, int y, const char *str, st7305_color_t c);
static void wb_icon(st7305_handle_t *l, int n, int ox, int oy, int inv);

static void handle_op(int op, ui_ctx_t *ctx) {
    if (!s_ar) return;                      /* 分配失败: 一切操作 no-op */
    switch (op) {
        case 0: wb_undo(ctx); return;
        case 1: wb_redo(ctx); return;
        case 2: {   /* 清屏: 记录 CLEAR 命令 (可撤销) */
            /* A4/A10: 日志满就先压基线 —— 压的是"清屏前"的画面, 所以 CLEAR 仍可撤销。
             * 点池不动: 日志还活着, 清点池会让旧命令引用的点错位。 */
            wb_reserve_log();
            wb_cmd_t c; memset(&c, 0, sizeof(c));
            c.type = WB_CMD_CLEAR;
            wb_commit(&c);
            memset(s_canvas, 0, WB_BYTES);
            break;
        }
        case 3: {   /* 保存: 每次生成新存档 (多白板管理属 C4, 未实施) */
            wb_new_name(s_cur);
            if (!sd_is_mounted())
                os_dialog_toast(ctx, "\xe6\x97\xa0TF\xe5\x8d\xa1");                        /* 无TF卡 */
            else if (wb_bmp_write(s_cur))
                os_dialog_toast(ctx, "\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98");            /* 已保存 */
            else
                os_dialog_toast(ctx, "\xe4\xbf\x9d\xe5\xad\x98\xe5\xa4\xb1\xe8\xb4\xa5");/* 保存失败 */
            break;
        }
        case 4: wb_open_loader(ctx); return;
        default: break;
    }
    ctx->needs_redraw = true;
}

/* ================= 保存/加载 (1-bit BMP) ================= */
#define WB_DIR   "/sdcard/apps/data/\xe5\x9b\xbe\xe7\x89\x87"   /* 画布目录: apps/data/图片 */
static const int s_bmp_off = 0x3E;

/* 递归建目录 (WB_DIR 是两级: /sdcard/apps/data/图片) */
static bool wb_mkdir_p(const char *path) {
    char tmp[80];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool wb_bmp_write(const char *path) {
    if (!s_ar || !path || !path[0]) return false;
    if (!sd_is_mounted()) return false;
    /* A8: 按 WB_DIR 建目录 —— 原来 mkdir 的是已废弃的 /sdcard/App/图片,
     * 目录一旦缺失 (换卡/被删) 保存就永久失败。 */
    if (!wb_mkdir_p(WB_DIR)) { ESP_LOGW("WB", "建目录失败: %s", WB_DIR); return false; }

    /* A9: 先写 .tmp 再 rename —— 掉电/拔卡不会把有效存档写成半截 */
    char tmp[96];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGW("WB", "打开失败: %s", tmp); return false; }

    int stride = ((WB_W + 31) / 32) * 4;
    uint32_t imgsz = (uint32_t)stride * WB_H;
    uint32_t fsz = (uint32_t)s_bmp_off + imgsz;
    uint8_t h[54]; memset(h, 0, sizeof(h));
    h[0]='B'; h[1]='M';
    h[2]=fsz; h[3]=fsz>>8; h[4]=fsz>>16; h[5]=fsz>>24;
    h[10]=s_bmp_off;
    h[14]=40;
    /* 显式 (uint8_t): BMP 头是逐字节写小端, 取低位是有意的 ——
     * 不写转换 GCC 会报 -Woverflow (400→144), 属误报。 */
    h[18]=(uint8_t)WB_W; h[19]=WB_W>>8; h[20]=WB_W>>16; h[21]=WB_W>>24;
    h[22]=(uint8_t)WB_H; h[23]=WB_H>>8; h[24]=WB_H>>16; h[25]=WB_H>>24;
    h[26]=1; h[28]=1;
    h[34]=imgsz; h[35]=imgsz>>8; h[36]=imgsz>>16; h[37]=imgsz>>24;
    h[46]=2;

    bool ok = true;
    if (fwrite(h, 1, sizeof(h), f) != sizeof(h)) ok = false;
    uint8_t pal[8] = {0,0,0,0, 255,255,255,0};
    if (ok && fwrite(pal, 1, sizeof(pal), f) != sizeof(pal)) ok = false;
    uint8_t line[52];
    for (int y = 0; ok && y < WB_H; y++) {
        int fy = WB_H - 1 - y;
        if (fseek(f, s_bmp_off + (long)fy*stride, SEEK_SET) != 0) { ok = false; break; }
        memset(line, 0, stride);
        const uint8_t *row = &s_canvas[y*WB_BPR];
        for (int x = 0; x < WB_W; x++)
            if (!((row[x>>3] >> (7-(x&7))) & 1)) line[x>>3] |= (uint8_t)(0x80u >> (x&7));
        if (fwrite(line, 1, stride, f) != (size_t)stride) ok = false;
    }
    if (ok && fflush(f) != 0) ok = false;
    if (ok) fsync(fileno(f));               /* 尽力而为: rename 前把数据刷到卡 */
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        ESP_LOGW("WB", "写盘失败, 已丢弃临时文件: %s", tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        ESP_LOGW("WB", "rename 失败: %s", path);
        return false;
    }
    return true;
}

static bool wb_bmp_read(const char *path) {
    if (!s_ar || !path || !path[0]) return false;
    if (!sd_is_mounted()) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    const int stride = ((WB_W + 31) / 32) * 4;
    const long need_img = (long)stride * WB_H;
    uint8_t *tmp = NULL;
    uint8_t hdr[62];
    bool ok = false;

    do {
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) break;
        if (hdr[0] != 'B' || hdr[1] != 'M') break;
        int w  = hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|((int)hdr[21]<<24);
        int ht = hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|((int)hdr[25]<<24);
        /* 只认本机写的格式: 1bpp 400×300 (既有存档全部满足, 兼容性不变) */
        if (hdr[28] != 1 || w != WB_W || ht != WB_H) break;
        long off = (long)(hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((long)hdr[13]<<24));
        if (off < 62) break;
        struct stat st;
        if (fstat(fileno(f), &st) != 0) break;
        if ((long)st.st_size < off + need_img) {
            ESP_LOGW("WB", "存档不完整: %s (size=%ld, 需 %ld)",
                     path, (long)st.st_size, off + need_img);
            break;
        }
        tmp = (uint8_t *)heap_caps_malloc(WB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) { ESP_LOGW("WB", "加载缓冲分配失败"); break; }
        memset(tmp, 0, WB_BYTES);
        bool rows_ok = true;
        uint8_t line[52];
        for (int y = 0; y < WB_H; y++) {
            int fy = WB_H - 1 - y;
            if (fseek(f, off + (long)fy*stride, SEEK_SET) != 0) { rows_ok = false; break; }
            if (fread(line, 1, stride, f) != (size_t)stride)   { rows_ok = false; break; }
            uint8_t *row = &tmp[y*WB_BPR];
            for (int x = 0; x < WB_W; x++)
                if (!((line[x>>3] >> (7-(x&7))) & 1)) row[x>>3] |= (uint8_t)(0x80u >> (x&7));
        }
        if (!rows_ok) break;
        /* A9: 整幅都读成功才覆盖 —— 失败时保留当前画面, 不清空 */
        memcpy(s_canvas, tmp, WB_BYTES);
        memcpy(s_snap, s_canvas, WB_BYTES);
        s_ulog_n = 0; s_rlog_n = 0; s_pts_n = 0;
        memset(&s_pend, 0, sizeof(s_pend));
        s_pend_trunc = false;
        ok = true;
    } while (0);

    if (tmp) heap_caps_free(tmp);
    fclose(f);
    return ok;
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
        /* WB_DIR 是编译期常量 → 文件名可用长度 = sizeof(tmp[n]) - sizeof(WB_DIR) - 1
         * (原写 %.50s, 实际可用只有 45, 第 46 字符起被静默吃掉 → 长名会变成另一个名字) */
        snprintf(tmp[n], sizeof(tmp[n]), "%.*s/%.*s",
                 (int)sizeof(WB_DIR) - 1, WB_DIR,
                 (int)sizeof(tmp[n]) - (int)sizeof(WB_DIR) - 2, e->d_name);
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

/* ================= 数据操作 ================= */
static void wb_do_load(int idx) {
    if (wb_bmp_read(s_paths[idx])) strcpy(s_cur, s_paths[idx]);
}
static void wb_del_entry(int idx) {
    if (idx < 0 || idx >= s_nfiles) return;
    unlink(s_paths[idx]);
    if (strcmp(s_paths[idx], s_cur) == 0) s_cur[0] = 0;
    wb_list();
}

/* ================= 加载列表 (统一列表模板, 长按1秒删除) ================= */
static void wb_load_refresh(ui_ctx_t *ctx);
static void wb_del_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    int idx = (int)(intptr_t)ud;
    os_dialog_pop(ctx);                 /* 关确认弹窗 → 回加载列表 */
    if (result == 0 && idx >= 0 && idx < s_nfiles) wb_del_entry(idx);
    wb_load_refresh(ctx);               /* 覆盖顶层(加载列表)刷新 */
}
static bool wb_load_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a == OS_ACTION_LONG_PRESS) {
        /* 长按/点击删除× → 立即弹出小巧自适应确认弹窗: 删除该白板 / 取消
         * (长按已由 dlg 每帧即时检测, 手指按住期间即弹出, 不须松手.) */
        if (d->sel >= 0 && d->sel < s_nfiles) {
            os_dialog_confirm_ex(ctx, "\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe7\x99\xbd\xe6\x9d\xbf?", /* 删除该白板? */
                                 0, 0, wb_del_confirm_cb, (void *)(intptr_t)d->sel);
        }
        return true;
    }
    return false;
}
static void wb_load_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;          /* 返回 → 保持白板 */
    if (result < s_nfiles) { wb_do_load(result); ctx->needs_redraw = true; }  /* 载入选中存档 */
}
static void wb_load_refresh(ui_ctx_t *ctx) {
    wb_list();
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    for (int i = 0; i < s_nfiles; i++)
            snprintf(dlg.items[i], sizeof(dlg.items[i]), "%.*s", (int)sizeof(dlg.items[i]) - 1, s_names[i]);
    dlg.count = s_nfiles;
    dlg.sel = 0;
    dlg.on_key = wb_load_key;
    dlg.cb = wb_load_cb;
    dlg.row_x = true;   /* 每行最右侧显示删除×, 点击× 弹删除确认; 长按行即时删除确认 */
    if (os_dialog_depth() > 0) os_dialog_pop(ctx);   /* 覆盖当前层(加载列表)时先弹栈, 层级不加深 */
    os_dialog_push(ctx, &dlg);                       /* 栈空自动转 push, 打开加载列表 */
}
static void wb_open_loader(ui_ctx_t *ctx) {
    wb_list();
    if (s_nfiles == 0) {
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe5\xad\x98\xe6\xa1\xa3"); /* 无存档 */
        ctx->needs_redraw = true;
        return;
    }
    wb_load_refresh(ctx);   /* 列表模板: 列出已保存白板, 点选加载, 长按删除, 底部返回 */
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
    if (!s_ar) {                        /* A2: 分配失败 → 只提示, 不碰任何缓冲 */
        st7305_clear(l, ST7305_COLOR_WHITE);
        wb_draw_zh(l, 8, 8, "\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3", /* 内存不足 */
                   ST7305_COLOR_BLACK);
        return;
    }
    st7305_clear(l, ST7305_COLOR_WHITE);
    st7305_blit_1bit(l, 0, 0, WB_W, WB_H, s_canvas);
    wb_draw_topbar(l);
    wb_draw_toolcol(l);
    /* B1: 形状拖动预览 —— 橡皮筋只画在屏上 (s_pv), 松手才落进画布 */
    if (s_drawing && !s_free) {
        s_pv = true;
        wb_draw_shape(l, s_tool, s_ax, s_ay, s_lx, s_ly);
        s_pv = false;
    }
}

/* ================= 模块接口 ================= */
static void wb_reset(ui_ctx_t *ctx) {
    gesture_back_enable(false);
    s_tool = WB_PEN; s_size = 1;
    s_down = false; s_free = false; s_pv = false; s_drawing = false;
    s_top_press = -1; s_top_px = s_top_py = 0;
    s_ulog_n = 0; s_rlog_n = 0; s_pts_n = 0;
    memset(&s_pend, 0, sizeof(s_pend));
    s_pend_trunc = false;
    s_cur[0] = 0;

    if (!wb_alloc()) {                  /* A2: 分配失败 → 不绘图/不存盘, 只提示 */
        os_dialog_toast(ctx, "\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); /* 内存不足 */
        ctx->needs_redraw = true;
        return;
    }
    memset(s_canvas, 0, WB_BYTES);
    memcpy(s_snap, s_canvas, WB_BYTES);      /* 基线快照 = 空画布 */
    wb_list();
    if (s_nfiles > 0) {
        strcpy(s_cur, s_paths[0]);           /* 自动接着上次那张 (序号最大的) */
        if (!wb_bmp_read(s_cur)) {           /* 读失败保留空画布 (不清空已经空白的画布) */
            memset(s_canvas, 0, WB_BYTES);
            memcpy(s_snap, s_canvas, WB_BYTES);
            ESP_LOGW("WB", "自动加载失败: %s", s_cur);
        }
    }
    ctx->needs_redraw = true;
}

static void wb_exit(ui_ctx_t *ctx) {
    (void)ctx;
    gesture_back_enable(true);               /* 无论后面保存成败, 全局手势必须恢复 */
    if (!s_ar) return;                       /* A2: 绝不拿"没分配成功的画布"覆盖存档 */
    if (!s_cur[0]) { wb_new_name(s_cur); if (!s_cur[0]) return; }
    /* 固定产品决策: 每次退出都自动保存 (含未修改) */
    if (!wb_bmp_write(s_cur)) ESP_LOGW("WB", "退出自动保存失败: %s", s_cur);
}

static bool p_wb_touch(ui_ctx_t *ctx, int x, int y) {
    (void)ctx; (void)x; (void)y;
    return true;   /* 全屏绘图页: 触摸一律消费 (内核据此决定是否震动) */
}

static void p_wb_action(ui_ctx_t *ctx, os_action_t a) {
    /* 白板是应用页 (非引擎): 硬件返回键由内核按 .exit_confirm=true 统一拦成
     * "退出程序?"确认 (os_core.c ②), 确认后才 os_pop → wb_exit 自动保存。
     * 所以这里**不再**处理 OS_ACTION_BACK (原 os_pop 分支是死代码, 且注释与实际行为相反)。
     * 加载列表/删除确认/toast 都走 os_dialog, 此处只处理绘图按键。 */
    switch (a) {
        case OS_ACTION_RIGHT: s_tool = (s_tool+1) % WB_TOOL_N; break;
        case OS_ACTION_LEFT:  s_tool = (s_tool==0) ? WB_TOOL_N-1 : s_tool-1; break;
        case OS_ACTION_UP:    s_size = (s_size >= 6) ? 6 : s_size+1; break;
        case OS_ACTION_DOWN:  s_size = (s_size <= 1) ? 1 : s_size-1; break;
        default: break;
    }
    ctx->needs_redraw = true;
}

static void p_wb_poll(ui_ctx_t *ctx) {
    if (!s_ar) return;                       /* 分配失败: 不响应任何触摸 */

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);

    /* ---------- 抬笔帧 ---------- */
    if (!down) {
        /* B2: 顶栏按钮"抬起且仍在同一按钮内"才执行, 按下只高亮; 滑出=取消 */
        if (s_top_press >= 0) {
            int op = s_top_press;
            bool inside = in_top_bar(s_top_px, s_top_py) && (op_btn(s_top_px) == op);
            s_top_press = -1;
            s_down = false; s_free = false; s_drawing = false;
            ctx->needs_redraw = true;
            if (inside) handle_op(op, ctx);   /* 可能已打开加载列表 */
            return;
        }
        /* 形状: 松手落笔 + 记 SHAPE 命令 */
        if (s_drawing && !s_free) {
            wb_reserve_log();                 /* A4: 日志满先压基线 (压的是本形状之前) */
            wb_draw_shape(ctx->lcd, s_tool, s_ax, s_ay, s_lx, s_ly);
            wb_cmd_t c;
            memset(&c, 0, sizeof(c));
            c.type = WB_CMD_SHAPE; c.tool = (uint8_t)s_tool; c.size = (uint8_t)s_size;
            c.x0 = (int16_t)s_ax; c.y0 = (int16_t)s_ay;
            c.x1 = (int16_t)s_lx; c.y1 = (int16_t)s_ly;
            wb_commit(&c);
            ctx->needs_redraw = true;
        }
        /* 自由笔: 提交本条 FREE 命令 */
        if (s_drawing && s_free) {
            if (!s_pend_trunc && s_pend.n > 0) {
                wb_commit(&s_pend);
            } else if (s_pend_trunc) {
                /* 点池不够记完整条笔迹 → 并入基线 (画面不丢, 只是这一步不可撤销) */
                wb_compress();
                ESP_LOGW("WB", "笔迹点池已满 (%d 点), 该笔并入基线", WB_PT_MAX);
            }
            memset(&s_pend, 0, sizeof(s_pend));
            s_pend_trunc = false;
            ctx->needs_redraw = true;
        }
        s_down = false;
        s_free = false;
        s_drawing = false;
        return;
    }

    /* ---------- 落笔帧 ---------- */
    if (!s_down) {
        if (in_top_bar(tx, ty)) {
            s_top_press = op_btn(tx);
            s_top_px = tx; s_top_py = ty;
            s_down = true;
            ctx->needs_redraw = true;         /* 只画按下高亮 */
            return;
        }
        if (in_tool_col(tx, ty)) {
            int slot = tool_slot_btn(ty);
            if (slot == WB_TOOL_N) { s_size = (s_size >= 6) ? 1 : s_size + 1; }
            else { s_tool = slot; }
            s_down = true; s_free = false; s_drawing = false;   /* 工具列按下 ≠ 绘图 */
            ctx->needs_redraw = true;
            return;
        }
        if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) { s_down = false; return; }
        s_down = true;
        s_drawing = true;
        s_free = (s_tool==WB_PEN || s_tool==WB_MARK || s_tool==WB_ERAS);
        s_ax = s_lx = tx; s_ay = s_ly = ty;
        if (s_free) {
            /* A4: 先把日志腾出空间, 再开始攒点 —— 顺序反了会让 pts_idx 失效 */
            wb_reserve_log();
            memset(&s_pend, 0, sizeof(s_pend));
            s_pend.type = WB_CMD_FREE;
            s_pend.tool = (uint8_t)s_tool;
            s_pend.size = (uint8_t)s_size;
            s_pend.pts_idx = (uint16_t)s_pts_n;
            s_pend.x0 = s_pend.x1 = (int16_t)tx;
            s_pend.y0 = s_pend.y1 = (int16_t)ty;
            s_pend_trunc = false;
            if (s_pts_n + 2 <= WB_PT_TOTAL) {
                s_pts[s_pts_n++] = (uint16_t)tx;   /* A1: uint16, 不再截断 399→143 */
                s_pts[s_pts_n++] = (uint16_t)ty;
                s_pend.n = 1;
            } else {
                s_pend_trunc = true;
            }
            wb_disk(ctx->lcd, tx, ty, tool_radius(), tool_on());
            ctx->needs_redraw = true;
        }
        return;
    }

    /* ---------- 拖动帧 ---------- */
    if (s_top_press >= 0) {                   /* 顶栏按住: 只更新落点, 便于抬起判定/滑出取消 */
        s_top_px = tx; s_top_py = ty;
        ctx->needs_redraw = true;
        return;
    }
    if (tx < WB_TB_W || ty < WB_TOP_H || ty >= WB_H) return;
    if (s_free) {
        if (tx == s_lx && ty == s_ly) return;
        wb_line(ctx->lcd, s_lx, s_ly, tx, ty, tool_radius(), tool_on());
        if (!s_pend_trunc) {
            if (s_pts_n + 2 <= WB_PT_TOTAL) {
                s_pts[s_pts_n++] = (uint16_t)tx;
                s_pts[s_pts_n++] = (uint16_t)ty;
                s_pend.n++;                        /* A7: uint16, 超 255 不再回绕成 0 */
                s_pend.x1 = (int16_t)tx;
                s_pend.y1 = (int16_t)ty;
            } else {
                s_pend_trunc = true;               /* 只停止记点, 画面照画 */
            }
        }
        s_lx = tx; s_ly = ty;
        ctx->needs_redraw = true;
    } else {
        s_lx = tx; s_ly = ty;
        ctx->needs_redraw = true;                  /* B1: 预览需要每帧重绘 */
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
    .exit_confirm = true,        /* 返回键先弹"退出程序?"确认 (2026-10-14 全局规范) */
    .swipe_back_disabled = true, /* 全屏绘图页屏蔽上滑退出 (内核统一设置 gesture_back) */
    .no_bottom_shield = true,    /* A6: 底部拖动屏蔽带会让画布下沿断笔, 本页放行 */
};

void os_page_wb_register(void) { os_register(&s_mod_wb); }
