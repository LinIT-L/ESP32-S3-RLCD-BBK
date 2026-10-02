/**
 * keyboard.h — 标准 52 键(五排)虚拟键盘模块 (键盘内核 = 输入, 与 UI 拆分).
 *
 * 本模块拆成两部分:
 *   - 键盘内核(输入): keyboard.c —— 键位布局表(kbd_std/kbd_edit) + 按键状态(kbd_t)
 *     + 索引→字符/动作映射(kbd_press/kbd_char/kbd_label/kbd_layer_on) + 屏幕坐标→键
 *     索引的几何命中(kbd_hit*). 不依赖任何绘制, 属系统级输入能力(os 已 REQUIRES 本组件).
 *   - 默认 UI: kbd_ui.c —— kbd_draw* 系列渲染器, 把内核模型画出来.
 *
 * 程序两种用法任选其一:
 *   (a) 直接用模块自带 UI: kbd_draw(...) 绘制 + kbd_hit(...) 命中, 开箱即用;
 *   (b) 自己画 UI(任意风格/布局), 再调内核输入 API 完成输入:
 *       触摸坐标 → kbd_hit* 拿键索引 → kbd_char / kbd_press 出字符/动作.
 *       kbd_t / kbd_key_t 类型公开, 也可自定义布局实例后用内核 API 驱动.
 *   如此适配不同情况, 不必为每种场景重造键盘.
 *
 * 布局与虚拟键鼠 k76 全键盘完全一致; 自带 Fn 三档(数字/F1-F12/符号) + Caps/Shift 大小写.
 */
#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdbool.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_ROW   29            /* 每排键高 */
#define KBD_ROWS  5
#define KBD_H     (KBD_ROWS*KBD_ROW)  /* 五排总高 = 145 */

/* 按键动作码 */
enum {
    KBD_ACT_CHAR  = 0,   /* 普通字符输入 */
    KBD_ACT_ENTER = 1,
    KBD_ACT_BKSP  = 2,
    KBD_ACT_SPACE = 3,
    KBD_ACT_ESC   = 4,
    KBD_ACT_TAB   = 5,
    KBD_ACT_UP    = 6,
    KBD_ACT_DOWN  = 7,
    KBD_ACT_LEFT  = 8,
    KBD_ACT_RIGHT = 9,
    KBD_ACT_CAPS  = 10,
    KBD_ACT_SHIFT = 11,
    KBD_ACT_FN    = 12,
    KBD_ACT_CTRL  = 13,
    KBD_ACT_ALT   = 14
};

typedef struct {
    const char* label;   /* 特殊键显示文本 */
    char        ch;      /* 基准字符(小写字母/数字/符号) */
    int         act;     /* 动作码 */
    int         x, w;    /* 列偏移, 宽 */
    int         y;       /* 所在排 0..4 */
    const char* fn;      /* Fn 档1(F1-F12) 标签 */
    const char* sym;     /* Fn 档2(符号) 文本 */
} kbd_key_t;

typedef struct {
    const kbd_key_t* keys;   /* 布局表(只读, 共享) */
    int   n;                 /* 键数 */
    int   w;                 /* 键盘宽度(屏宽) */
    bool  caps;              /* Caps 锁定 */
    bool  shift;             /* Shift 临时 */
    int   fn_mode;           /* 0=数字 1=F1-F12 2=符号 */
} kbd_t;

/* 标准 52 键布局(全屏宽)实例 */
extern const kbd_t kbd_std;

/* 编辑键盘布局(全屏宽, 收藏夹/编辑场景用):
 * 整体行高压低 1/3(KBD_EDIT_ROW), 删除并入退格键, 原删除位改为空格, 回车改名.
 * 用于需要"编辑输入"但空间受限的界面. */
#define KBD_EDIT_ROW 19            /* 压低 1/3 (原 29) */
extern const kbd_t kbd_edit;
/* 编辑键盘排高: 绘制/命中用 KBD_EDIT_ROW 而非 KBD_ROW */
void kbd_draw_edit(st7305_handle_t* l, int base_y, const kbd_t* k, int press);
int  kbd_hit_edit(const kbd_t* k, int base_y, int sx, int sy);

/* 弹窗缩放绘制: 满宽(k->w)布局缩放并平移到 [base_x, base_x+width-1] 区域,
 * 排高 row_h. 供 WiFi 密码等"键盘放入标准弹窗"场景缩放适配弹窗宽度. */
void kbd_draw_edit_at(st7305_handle_t* l, int base_x, int base_y, int width, int row_h, const kbd_t* k, int press);
int  kbd_hit_edit_at(const kbd_t* k, int base_x, int base_y, int width, int row_h, int sx, int sy);

/* 绘制: base_y 为键盘顶部屏幕纵坐标; press 为按下键索引(-1 无). 不依赖调用方布局, 可复用于任意底坐标 */
void kbd_draw(st7305_handle_t* l, int base_y, const kbd_t* k, int press);

/* V: 只绘制前 rows 排 (去掉"Fn/方向键/空格"底排时用) */
void kbd_draw_rows(st7305_handle_t* l, int base_y, const kbd_t* k, int press, int rows);

/* 命中: 屏幕坐标 → 键索引(-1 未命中) */
int  kbd_hit(const kbd_t* k, int base_y, int sx, int sy);

/* V: 仅前 rows 排内命中 */
int  kbd_hit_rows(const kbd_t* k, int base_y, int sx, int sy, int rows);

/* 按下逻辑: 对 Caps/Shift/Fn 翻转内部状态, 其余返回动作码 */
int  kbd_press(kbd_t* k, int idx);

/* 读取当前状态下的可输入字符 (仅 KBD_ACT_CHAR; Fn 功能档或特殊键返回 0) */
char kbd_char(const kbd_t* k, int idx);

/* 键当前应显示标签 (Fn 档/Caps/Shift 联动) */
const char* kbd_label(const kbd_t* k, int idx);

/* 键是否处于激活层 (Caps/Shift/Fn 开启时对应键整键反黑) */
bool kbd_layer_on(const kbd_t* k, int idx);

/* ============ 统一入口: 样式 + 几何 (新代码一律用这套) ============
 * 背景: 本模块历史上对外暴露了 4 种外观变体(std/edit/rows/dialog), 但 draw 与 hit
 *       各是 4 个独立函数, 调用方必须自己配对 —— 一旦"用 kbd_draw_edit 画、却调
 *       kbd_hit 判", 几何就不一致, 命中全错而且不报错。
 * 统一入口把"样式"与"几何"合成一个 kbd_geom_t, draw/hit 共用同一个它,
 * 从结构上消除配错的可能。旧 API 保留(不动在跑的页面), 新代码/桥一律走这里。
 *
 * 四种样式对应关系:
 *   KBD_STYLE_STD    → kbd_draw      / kbd_hit        (5 排全高 145px)
 *   KBD_STYLE_EDIT   → kbd_draw_edit / kbd_hit_edit   (排高 19px, 压低 1/3)
 *   KBD_STYLE_ROWS   → kbd_draw_rows / kbd_hit_rows   (只前 rows 排)
 *   KBD_STYLE_DIALOG → kbd_draw_edit_at / kbd_hit_edit_at (缩放平移到 [x,x+w), 指定 row_h)
 */
typedef enum {
    KBD_STYLE_STD = 0,   /* 终端 / 网络检测 / 密钥管理器 */
    KBD_STYLE_EDIT,      /* 收藏夹 / 编辑场景 */
    KBD_STYLE_ROWS,      /* 摩斯码(只用前几排) */
    KBD_STYLE_DIALOG,    /* 弹窗内缩放(WiFi 密码等) */
    KBD_STYLE_COUNT
} kbd_style_t;

typedef struct {
    kbd_style_t style;
    int x;       /* 左边界     (仅 DIALOG 用; 其余 = 0) */
    int y;       /* 顶边屏幕纵坐标 (全部样式都用) */
    int w;       /* 绘制宽     (仅 DIALOG 用; 其余 = 屏宽) */
    int row_h;   /* 排高       (仅 DIALOG 用; 其余按样式: STD/ROWS=KBD_ROW, EDIT=KBD_EDIT_ROW) */
    int rows;    /* 排数       (仅 ROWS 用; <=0 = 全部) */
} kbd_geom_t;

const char* kbd_style_name(kbd_style_t s);                     /* → "std"/"edit"/"rows"/"dialog" */
int         kbd_style_by_name(const char* name);               /* → kbd_style_t; 未知返回 -1 */
void        kbd_geom_default(kbd_geom_t* g, kbd_style_t s, int y); /* 按样式填默认几何 */
int         kbd_style_height(kbd_style_t s, int rows, int row_h);  /* 该样式占用的像素高 */

void kbd_draw_style(st7305_handle_t* l, const kbd_geom_t* g, const kbd_t* k, int press);
int  kbd_hit_style(const kbd_t* k, const kbd_geom_t* g, int sx, int sy);

#ifdef __cplusplus
}
#endif
#endif /* _KEYBOARD_H */