/**
 * keyboard.h — 标准 52 键(五排)虚拟键盘模块.
 *
 * 布局与虚拟键鼠 k76 全键盘完全一致; 自带 Fn 三档(数字/F1-F12/符号) + Caps/Shift 大小写.
 * 供终端/密码输入等任意界面复用: 只需给定绘制纵坐标与触摸命中, 即可绘制与交互.
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

/* 绘制: base_y 为键盘顶部屏幕纵坐标; press 为按下键索引(-1 无). 不依赖调用方布局, 可复用于任意底坐标 */
void kbd_draw(st7305_handle_t* l, int base_y, const kbd_t* k, int press);

/* 命中: 屏幕坐标 → 键索引(-1 未命中) */
int  kbd_hit(const kbd_t* k, int base_y, int sx, int sy);

/* 按下逻辑: 对 Caps/Shift/Fn 翻转内部状态, 其余返回动作码 */
int  kbd_press(kbd_t* k, int idx);

/* 读取当前状态下的可输入字符 (仅 KBD_ACT_CHAR; Fn 功能档或特殊键返回 0) */
char kbd_char(const kbd_t* k, int idx);

/* 键当前应显示标签 (Fn 档/Caps/Shift 联动) */
const char* kbd_label(const kbd_t* k, int idx);

/* 键是否处于激活层 (Caps/Shift/Fn 开启时对应键整键反黑) */
bool kbd_layer_on(const kbd_t* k, int idx);

#ifdef __cplusplus
}
#endif
#endif /* _KEYBOARD_H */