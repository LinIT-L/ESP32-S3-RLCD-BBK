/**
 * kbd_c42.h — 经典 42 键紧凑键盘 (5 排 10/10/9/10/3, 无 Fn/方向键).
 *
 * 背景 (2026-11-24 键盘精简):
 *   这套键盘原来有**两份独立实现** —— 设置页 WiFi 密码弹窗 (WKB, 350 宽/排高 25/
 *   满宽横分割线/16×24 字) 与网络检测页 (nd_kbd, 373 宽/排高 26/每键右+下分割线/
 *   16px 混排字)。两者键位表字符串逐字节相同、动作语义相同, 只是几何与字体不同。
 *   现在: 布局/标签/字符/命中/绘制**一份**, 几何与字体由 kbd_c42_geo_t 参数化,
 *   两个场景各自的观感原样保留（不为统一改样子）。
 *
 * 用法:
 *   kbd_c42_geo_t g;  kbd_c42_geo_popup(&g, x0, y0);      // 弹窗版
 *                     kbd_c42_geo_page (&g, x0, y0, w);   // 页内版
 *   kbd_c42_state_t st = {0};
 *   kbd_c42_draw(lcd, &g, &st, press);                    // 画
 *   int id = kbd_c42_hit(&g, x, y);                       // 命中
 *   switch (kbd_c42_act(id)) { ... kbd_c42_char(&st, id) ... }
 *
 * ⚠ 状态 (sym/shift) 由调用方持有: kbd_c42_act() 只把 id 翻成动作码, **不改状态**。
 *   这是刻意的 —— 两份历史实现里"符号层按 Shift 键"的处理并不一致 (弹窗版退出符号层,
 *   页内版只翻 shift), 保留原行为比强行统一更安全。
 */
#ifndef _KBD_C42_H
#define _KBD_C42_H

#include <stdbool.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 键 id (与两份历史实现逐键对齐, 动它等于改动作表) */
enum {
    KBD_C42_NONE    = -1,
    KBD_C42_SHIFT   = 29,   /* 行3 首键: 大写/小写 (符号层时显示 ABC) */
    KBD_C42_LETTER0 = 30,   /* 行3 的 8 个字母: 30..37 */
    KBD_C42_DEL     = 38,   /* 行3 末键: 删除 */
    KBD_C42_SYM     = 39,   /* 行4 首键: &123 / ABC */
    KBD_C42_SPACE   = 40,
    KBD_C42_ENTER   = 41,
    KBD_C42_COUNT   = 42
};

/* 动作码 (kbd_c42_act) */
enum {
    KBD_C42_ACT_CHAR  = 0,
    KBD_C42_ACT_SHIFT = 1,
    KBD_C42_ACT_BKSP  = 2,
    KBD_C42_ACT_SYM   = 3,
    KBD_C42_ACT_SPACE = 4,
    KBD_C42_ACT_ENTER = 5
};

/* 键帽字体 */
typedef enum {
    KBD_C42_FONT_16X24 = 0,  /* 16×24 ASCII (弹窗版原样; 中文标签走同族 16×24) */
    KBD_C42_FONT_16MIX       /* 16px 混排: ASCII 8×16 + 汉字 16×16 (页内版原样) */
} kbd_c42_font_t;

/* 几何: 全部是**像素**值, 与渲染/命中同一份数据 */
typedef struct {
    int  x0, y0;      /* 键盘左上角 */
    int  row_h;       /* 排高 (行距) */
    int  key_h;       /* 键高 (分割线与文字居中的基准, 可小于 row_h) */
    int  kw_a;        /* 行0/1 键宽 (10 键) */
    int  kw_b;        /* 行2 键宽 (9 键) */
    int  off_b;       /* 行2 起点相对 x0 的偏移 (0=左对齐, >0=居中) */
    int  kw_shift;    /* 行3 首键 (Shift/ABC) 宽 */
    int  kw_c;        /* 行3 字母宽 (8 键) */
    int  kw_del;      /* 行3 删除宽 */
    int  kw_sym;      /* 行4 &123 宽 */
    int  kw_space;    /* 行4 空格宽 */
    int  kw_enter;    /* 行4 回车宽 */
    bool div_full;    /* true=行间满宽横线 (弹窗版) / false=每键右+下分割线 (页内版) */
    kbd_c42_font_t font;
} kbd_c42_geo_t;

/* 输入层状态 (调用方持有) */
typedef struct { bool sym, shift; } kbd_c42_state_t;

/* 预设①: 设置页 WiFi 密码弹窗 (350 宽内, 排高 25, 键高 25, 满宽横线, 16×24) */
void kbd_c42_geo_popup(kbd_c42_geo_t *g, int x0, int y0);
/* 预设②: 网络检测页页内 (按容器宽度自适应, 排高 26, 键高 24, 每键分割线, 16px 混排).
 * width = 行0/1 十个键应占的总宽 (历史实现传 373), 键宽 = (width-8)/10。 */
void kbd_c42_geo_page(kbd_c42_geo_t *g, int x0, int y0, int width);

/* 键盘总高 = 5 * row_h */
int  kbd_c42_height(const kbd_c42_geo_t *g);

/* 绘制; press = 按下键 id (-1 无)。不改 st。 */
void kbd_c42_draw(st7305_handle_t *l, const kbd_c42_geo_t *g,
                  const kbd_c42_state_t *st, int press);

/* 命中: 屏幕坐标 → 键 id (-1 未命中) */
int  kbd_c42_hit(const kbd_c42_geo_t *g, int x, int y);

/* 单键矩形 (供调用方做额外高亮/调试; 越界返回 w=0) */
void kbd_c42_key_rect(const kbd_c42_geo_t *g, int id, int *x, int *y, int *w);

/* 键当前应显示的文本 (受 sym/shift 影响; 返回常量或内部静态缓冲) */
const char *kbd_c42_label(const kbd_c42_state_t *st, int id);

/* 键在当前状态下输入什么字符 (非字符键返回 0) */
char kbd_c42_char(const kbd_c42_state_t *st, int id);

/* 键 → 动作码 (不改状态) */
int  kbd_c42_act(int id);

#ifdef __cplusplus
}
#endif
#endif /* _KBD_C42_H */
