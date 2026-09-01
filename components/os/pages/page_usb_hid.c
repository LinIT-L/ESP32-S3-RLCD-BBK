/**
 * page_usb_hid.c — USB HID 仿真键鼠 页面模块 (P3 迁移).
 *
 * 从 menu_system.c 的 render_usb_hid/hid_* 迁移, 自包含全屏页:
 *   - 4 种布局: 运维键鼠 / 28键紧凑键盘 / 52键全键盘+触控板 / 纯触控板
 *   - 键盘触摸点键 -> usb_hid_key_tap(); Ctrl/Alt/Shift/Caps/Fn 为开关键
 *   - 触控板: 单点=左键, 按住滑动=移鼠标(随速度加速), 按住不动500ms=右键
 *   - 确认 = 布局切换菜单, 返回 = 退出(停止 HID 恢复串口)
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "usb_hid.h"
#include "font_zh16.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TAG "HIDP"

/* ============ 常量 (与旧 menu 一致) ============ */
#define HID_KW   66
#define HID_KW_LAST 70  /* 最后一列(F6/F12)吃满右侧 4px, 使整宽正好 0..399 无缝隙 */
#define HID_COL0 0
#define HID_COL1 66
#define HID_COL2 132
#define HID_COL3 198
#define HID_COL4 264
#define HID_COL5 330
#define HID_R0_Y 0
#define HID_R1_Y 35
#define HID_R2_Y 70
#define HID_R3_Y 105
#define HID_KH   35
#define HID_ESC_H 70   /* ESC 纵跨 row2+row3 (y70..140) */
#define HID_2W    136  /* 删除/回车: 2 格宽 (x264..399) */

#define HID_TP_Y0 140
#define HID_TP_Y1 299
#define HID_MB_Y  273
#define HID_MB_H  27
#define HID_MB_W  96
#define HID_MB_XL 4     /* L 鼠标键贴最左角落 */
#define HID_MB_XR 300   /* R 鼠标键贴最右角落 */
#define HID_ARROW_Y  273
#define HID_ARROW_H  27
#define HID_ARROW_W  50
#define HID_ARROW_XL 150   /* ← 键左框 */
#define HID_ARROW_XR 203   /* → 键左框 */

/* 运维助手样式底部左右键 (纯触控板 / 52键 布局) */
#define NEW_MB_Y   260
#define NEW_MB_H   (HID_TP_Y1 - NEW_MB_Y + 1)   /* 40, 直铺到底 */
#define NEW_MB_W   100
#define NEW_MB_XL  100
#define NEW_MB_XR  200

/* HID 键盘 usage code */
#define HIDK_A  0x04
#define HIDK_B  0x05
#define HIDK_C  0x06
#define HIDK_D  0x07
#define HIDK_E  0x08
#define HIDK_F  0x09
#define HIDK_G  0x0A
#define HIDK_H  0x0B
#define HIDK_I  0x0C
#define HIDK_J  0x0D
#define HIDK_K  0x0E
#define HIDK_L  0x0F
#define HIDK_M  0x10
#define HIDK_N  0x11
#define HIDK_O  0x12
#define HIDK_P  0x13
#define HIDK_Q  0x14
#define HIDK_R  0x15
#define HIDK_S  0x16
#define HIDK_T  0x17
#define HIDK_U  0x18
#define HIDK_V  0x19
#define HIDK_W  0x1A
#define HIDK_X  0x1B
#define HIDK_Y  0x1C
#define HIDK_Z  0x1D
#define HIDK_1  0x1E
#define HIDK_2  0x1F
#define HIDK_3  0x20
#define HIDK_4  0x21
#define HIDK_5  0x22
#define HIDK_6  0x23
#define HIDK_7  0x24
#define HIDK_8  0x25
#define HIDK_9  0x26
#define HIDK_0  0x27
#define HIDK_ENTER     0x28
#define HIDK_ESC       0x29
#define HIDK_BS        0x2A
#define HIDK_TAB       0x2B
#define HIDK_SPACE     0x2C
#define HIDK_MINUS     0x2D
#define HIDK_EQUAL     0x2E
#define HIDK_LBRACKET  0x2F
#define HIDK_RBRACKET  0x30
#define HIDK_BACKSLASH 0x31
#define HIDK_SEMICOLON 0x33
#define HIDK_QUOTE     0x34
#define HIDK_BACKTICK  0x35
#define HIDK_COMMA     0x36
#define HIDK_PERIOD    0x37
#define HIDK_SLASH     0x38
#define HIDK_CAPS      0x39
#define HIDK_LEFT      0x50
#define HIDK_RIGHT     0x4F
#define HIDK_UP        0x52
#define HIDK_DOWN      0x51
#define HIDK_F1   0x3A
#define HIDK_F2   0x3B
#define HIDK_F3   0x3C
#define HIDK_F4   0x3D
#define HIDK_F5   0x3E
#define HIDK_F6   0x3F
#define HIDK_F7   0x40
#define HIDK_F8   0x41
#define HIDK_F9   0x42
#define HIDK_F10  0x43
#define HIDK_F11  0x44
#define HIDK_F12  0x45
#define HIDK_DELETE 0x4C

/* 修饰键位掩码 (与 TinyUSB / usb_hid 一致) */
#define MOD_LCTRL  (1U << 0)
#define MOD_LSHIFT (1U << 1)
#define MOD_LALT   (1U << 2)

/* 鼠标按键掩码 (USB HID button 位图) */
#define HID_MOUSE_L 0x01
#define HID_MOUSE_R 0x02
#define HID_MOUSE_M 0x04

/* 单个按键配置 (绝对网格坐标) */
typedef struct {
    const char *label;   /* 显示文本 (fn_key!=0 时, Fn 开启会动态显示 F 行标签) */
    uint8_t     key;     /* HID usage (Fn 关闭时键码); HIDK_TOGGLE_* 为修饰/开关键 */
    uint8_t     mod;     /* tap 时附加的修饰掩码 */
    uint8_t     fn_key;  /* 非0: Fn 档1(F1-F12) 时转发此 HID 码 (如 HIDK_F1) */
    uint16_t    x, y;    /* 左上角 (可 >255) */
    uint16_t    w, h;    /* 尺寸 (可 >255) */
    const char *sym_label; /* 符号档显示文本 (为 NULL 时符号档维持默认) */
    uint8_t     sym_key;   /* 符号档 HID 码 */
    uint8_t     sym_mod;   /* 符号档附加修饰掩码 */
} hid_key_t;

/* 修饰/开关按键伪键码 (与键盘 HID usage <=0xE7 不冲突) */
#define HIDK_TOGGLE_CAPS  0xF1
#define HIDK_TOGGLE_CTRL  0xF2
#define HIDK_TOGGLE_ALT   0xF3
#define HIDK_TOGGLE_SHIFT 0xF4
#define HIDK_TOGGLE_FN    0xF5

/* 键鼠布局枚举 (与二级菜单顺序一致) */
typedef enum {
    HID_LAYOUT_OP,        /* 0 运维键鼠: F区+光标+ESC/Del+回车, 下方触控板 */
    HID_LAYOUT_28,        /* 1 28键紧凑键盘: 英文字母+空格+回车+删除+大小写(无触控板) */
    HID_LAYOUT_76,        /* 2 52键全键盘+触控板: 标准QWERTY(数字行按Fn切F1-F12), 下方触控板 */
    HID_LAYOUT_TOUCHPAD,  /* 3 纯触控板: 整屏触控板 */
    HID_LAYOUT_MAX
} hid_layout_t;

/* ============ 键位表 ============ */
static const hid_key_t hid_row0[] = {   /* 第1排: F1-F6 */
    { "F1", HIDK_F1,  0, 0, HID_COL0, HID_R0_Y, HID_KW, HID_KH },
    { "F2", HIDK_F2,  0, 0, HID_COL1, HID_R0_Y, HID_KW, HID_KH },
    { "F3", HIDK_F3,  0, 0, HID_COL2, HID_R0_Y, HID_KW, HID_KH },
    { "F4", HIDK_F4,  0, 0, HID_COL3, HID_R0_Y, HID_KW, HID_KH },
    { "F5", HIDK_F5,  0, 0, HID_COL4, HID_R0_Y, HID_KW, HID_KH },
    { "F6", HIDK_F6,  0, 0, HID_COL5, HID_R0_Y, HID_KW_LAST, HID_KH },
};
static const hid_key_t hid_row1[] = {   /* 第2排: F7-F12 */
    { "F7",  HIDK_F7,  0, 0, HID_COL0, HID_R1_Y, HID_KW, HID_KH },
    { "F8",  HIDK_F8,  0, 0, HID_COL1, HID_R1_Y, HID_KW, HID_KH },
    { "F9",  HIDK_F9,  0, 0, HID_COL2, HID_R1_Y, HID_KW, HID_KH },
    { "F10", HIDK_F10, 0, 0, HID_COL3, HID_R1_Y, HID_KW, HID_KH },
    { "F11", HIDK_F11, 0, 0, HID_COL4, HID_R1_Y, HID_KW, HID_KH },
    { "F12", HIDK_F12, 0, 0, HID_COL5, HID_R1_Y, HID_KW_LAST, HID_KH },
};
static const hid_key_t hid_row2[] = {   /* 第3排: + / 上 / - / ESC(纵跨) / 删除 */
    { "+",   HIDK_EQUAL,  0, 0, HID_COL0, HID_R2_Y, HID_KW, HID_KH },
    { "^",   HIDK_UP,     0, 0, HID_COL1, HID_R2_Y, HID_KW, HID_KH },
    { "-",   HIDK_MINUS,  0, 0, HID_COL2, HID_R2_Y, HID_KW, HID_KH },
    { "Esc", HIDK_ESC,    0, 0, HID_COL3, HID_R2_Y, HID_KW, HID_ESC_H },
    { "Del", HIDK_DELETE, 0, 0, HID_COL4, HID_R2_Y, HID_2W, HID_KH },
};
static const hid_key_t hid_row3[] = {   /* 第4排: 左 / 下 / 右 / 回车 */
    { "<-",    HIDK_LEFT,  0, 0, HID_COL0, HID_R3_Y, HID_KW, HID_KH },
    { "v",     HIDK_DOWN,  0, 0, HID_COL1, HID_R3_Y, HID_KW, HID_KH },
    { "->",    HIDK_RIGHT, 0, 0, HID_COL2, HID_R3_Y, HID_KW, HID_KH },
    { "Enter", HIDK_ENTER, 0, 0, HID_COL4, HID_R3_Y, HID_2W, HID_KH },
};
#define HID_R0_N (sizeof(hid_row0)/sizeof(hid_key_t))
#define HID_R1_N (sizeof(hid_row1)/sizeof(hid_key_t))
#define HID_R2_N (sizeof(hid_row2)/sizeof(hid_key_t))
#define HID_R3_N (sizeof(hid_row3)/sizeof(hid_key_t))

#define K28_R0_Y 0
#define K28_R1_Y 72
#define K28_R2_Y 144
#define K28_R3_Y 216
#define K28_KH   72

static const hid_key_t k28_row0[] = {   /* 第1排: Q W E R T Y U I O P */
    { "Q", HIDK_Q, 0, 0, 0,   K28_R0_Y, 40, K28_KH },
    { "W", HIDK_W, 0, 0, 40,  K28_R0_Y, 40, K28_KH },
    { "E", HIDK_E, 0, 0, 80,  K28_R0_Y, 40, K28_KH },
    { "R", HIDK_R, 0, 0, 120, K28_R0_Y, 40, K28_KH },
    { "T", HIDK_T, 0, 0, 160, K28_R0_Y, 40, K28_KH },
    { "Y", HIDK_Y, 0, 0, 200, K28_R0_Y, 40, K28_KH },
    { "U", HIDK_U, 0, 0, 240, K28_R0_Y, 40, K28_KH },
    { "I", HIDK_I, 0, 0, 280, K28_R0_Y, 40, K28_KH },
    { "O", HIDK_O, 0, 0, 320, K28_R0_Y, 40, K28_KH },
    { "P", HIDK_P, 0, 0, 360, K28_R0_Y, 40, K28_KH },
};
static const hid_key_t k28_row1[] = {   /* 第2排: A S D F G H J K L */
    { "A", HIDK_A, 0, 0, 0,   K28_R1_Y, 44, K28_KH },
    { "S", HIDK_S, 0, 0, 44,  K28_R1_Y, 44, K28_KH },
    { "D", HIDK_D, 0, 0, 88,  K28_R1_Y, 44, K28_KH },
    { "F", HIDK_F, 0, 0, 132, K28_R1_Y, 44, K28_KH },
    { "G", HIDK_G, 0, 0, 176, K28_R1_Y, 44, K28_KH },
    { "H", HIDK_H, 0, 0, 220, K28_R1_Y, 44, K28_KH },
    { "J", HIDK_J, 0, 0, 264, K28_R1_Y, 44, K28_KH },
    { "K", HIDK_K, 0, 0, 308, K28_R1_Y, 44, K28_KH },
    { "L", HIDK_L, 0, 0, 352, K28_R1_Y, 48, K28_KH },
};
static const hid_key_t k28_row2[] = {   /* 第3排: Z X C V B N M Del(最后一个) */
    { "Z",   HIDK_Z,      0, 0, 0,   K28_R2_Y, 50, K28_KH },
    { "X",   HIDK_X,      0, 0, 50,  K28_R2_Y, 50, K28_KH },
    { "C",   HIDK_C,      0, 0, 100, K28_R2_Y, 50, K28_KH },
    { "V",   HIDK_V,      0, 0, 150, K28_R2_Y, 50, K28_KH },
    { "B",   HIDK_B,      0, 0, 200, K28_R2_Y, 50, K28_KH },
    { "N",   HIDK_N,      0, 0, 250, K28_R2_Y, 50, K28_KH },
    { "M",   HIDK_M,      0, 0, 300, K28_R2_Y, 50, K28_KH },
    { "Del", HIDK_DELETE, 0, 0, 350, K28_R2_Y, 50, K28_KH },
};
static const hid_key_t k28_row3[] = {   /* 第4排: 大小写 | 空格 | 回车 */
    { "Caps",  HIDK_TOGGLE_CAPS, 0, 0, 0,   K28_R3_Y, 100, K28_KH },
    { "Space", HIDK_SPACE,       0, 0, 100, K28_R3_Y, 150, K28_KH },
    { "Enter", HIDK_ENTER,       0, 0, 250, K28_R3_Y, 150, K28_KH },
};
#define K28_R0_N (sizeof(k28_row0)/sizeof(hid_key_t))
#define K28_R1_N (sizeof(k28_row1)/sizeof(hid_key_t))
#define K28_R2_N (sizeof(k28_row2)/sizeof(hid_key_t))
#define K28_R3_N (sizeof(k28_row3)/sizeof(hid_key_t))

#define K76_KH   30
#define K76_R0_Y 0
#define K76_R1_Y 30
#define K76_R2_Y 60
#define K76_R3_Y 90
#define K76_R4_Y 120

static const hid_key_t k76_row0[] = {   /* 第1排: Esc 1 2 3 4 5 6 7 8 9 0 - + Bksp */
    { .label="Esc",  .key=HIDK_ESC,     .x=0,   .y=K76_R0_Y, .w=28, .h=K76_KH },
    { .label="1",    .key=HIDK_1,       .fn_key=HIDK_F1,  .sym_label="`", .sym_key=HIDK_BACKTICK,  .sym_mod=0,          .x=28,  .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="2",    .key=HIDK_2,       .fn_key=HIDK_F2,  .sym_label="~", .sym_key=HIDK_BACKTICK,  .sym_mod=MOD_LSHIFT, .x=59,  .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="3",    .key=HIDK_3,       .fn_key=HIDK_F3,  .sym_label="[", .sym_key=HIDK_LBRACKET,  .sym_mod=0,          .x=90,  .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="4",    .key=HIDK_4,       .fn_key=HIDK_F4,  .sym_label="]", .sym_key=HIDK_RBRACKET,  .sym_mod=0,          .x=121, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="5",    .key=HIDK_5,       .fn_key=HIDK_F5,  .sym_label="{", .sym_key=HIDK_LBRACKET,  .sym_mod=MOD_LSHIFT, .x=152, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="6",    .key=HIDK_6,       .fn_key=HIDK_F6,  .sym_label="}", .sym_key=HIDK_RBRACKET,  .sym_mod=MOD_LSHIFT, .x=183, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="7",    .key=HIDK_7,       .fn_key=HIDK_F7,  .sym_label=";", .sym_key=HIDK_SEMICOLON, .sym_mod=0,          .x=214, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="8",    .key=HIDK_8,       .fn_key=HIDK_F8,  .sym_label="'", .sym_key=HIDK_QUOTE,     .sym_mod=0,          .x=245, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="9",    .key=HIDK_9,       .fn_key=HIDK_F9,  .sym_label=",", .sym_key=HIDK_COMMA,     .sym_mod=0,          .x=276, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="0",    .key=HIDK_0,       .fn_key=HIDK_F10, .sym_label=".", .sym_key=HIDK_PERIOD,    .sym_mod=0,          .x=307, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="-",    .key=HIDK_MINUS,   .fn_key=HIDK_F11, .sym_label="/", .sym_key=HIDK_SLASH,     .sym_mod=0,          .x=338, .y=K76_R0_Y, .w=31, .h=K76_KH },
    { .label="+",    .key=HIDK_EQUAL,   .mod=MOD_LSHIFT,  .fn_key=HIDK_F12, .sym_label="?", .sym_key=HIDK_SLASH, .sym_mod=MOD_LSHIFT, .x=369, .y=K76_R0_Y, .w=31, .h=K76_KH },
};
static const hid_key_t k76_row1[] = {   /* 第2排: Tab Q W E R T Y U I O P Bksp */
    { .label="Tab", .key=HIDK_TAB, .x=0,   .y=K76_R1_Y, .w=34, .h=K76_KH },
    { .label="Q",   .key=HIDK_Q,   .x=34,  .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="W",   .key=HIDK_W,   .x=67,  .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="E",   .key=HIDK_E,   .x=100, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="R",   .key=HIDK_R,   .x=133, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="T",   .key=HIDK_T,   .x=166, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="Y",   .key=HIDK_Y,   .x=199, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="U",   .key=HIDK_U,   .x=232, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="I",   .key=HIDK_I,   .x=265, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="O",   .key=HIDK_O,   .x=298, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="P",   .key=HIDK_P,   .x=331, .y=K76_R1_Y, .w=33, .h=K76_KH },
    { .label="Bksp",.key=HIDK_BS,  .x=364, .y=K76_R1_Y, .w=36, .h=K76_KH },
};
static const hid_key_t k76_row2[] = {   /* 第3排: Caps A S D F G H J K L Enter */
    { .label="Caps",  .key=HIDK_TOGGLE_CAPS, .x=0,   .y=K76_R2_Y, .w=54, .h=K76_KH },
    { .label="A", .key=HIDK_A, .x=54,  .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="S", .key=HIDK_S, .x=87,  .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="D", .key=HIDK_D, .x=120, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="F", .key=HIDK_F, .x=153, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="G", .key=HIDK_G, .x=186, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="H", .key=HIDK_H, .x=219, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="J", .key=HIDK_J, .x=252, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="K", .key=HIDK_K, .x=285, .y=K76_R2_Y, .w=33, .h=K76_KH },
    { .label="L", .key=HIDK_L, .x=318, .y=K76_R2_Y, .w=32, .h=K76_KH },
    { .label="Enter", .key=HIDK_ENTER, .x=350, .y=K76_R2_Y, .w=50, .h=K76_KH * 2 },
};
static const hid_key_t k76_row3[] = {   /* 第4排: Shift Z X C V B N M ↑ */
    { .label="Shift", .key=HIDK_TOGGLE_SHIFT, .x=0,   .y=K76_R3_Y, .w=70, .h=K76_KH },
    { .label="Z", .key=HIDK_Z, .x=70,  .y=K76_R3_Y, .w=34, .h=K76_KH },
    { .label="X", .key=HIDK_X, .x=104, .y=K76_R3_Y, .w=34, .h=K76_KH },
    { .label="C", .key=HIDK_C, .x=138, .y=K76_R3_Y, .w=34, .h=K76_KH },
    { .label="V", .key=HIDK_V, .x=172, .y=K76_R3_Y, .w=34, .h=K76_KH },
    { .label="B", .key=HIDK_B, .x=206, .y=K76_R3_Y, .w=34, .h=K76_KH },
    { .label="N", .key=HIDK_N, .x=240, .y=K76_R3_Y, .w=33, .h=K76_KH },
    { .label="M", .key=HIDK_M, .x=273, .y=K76_R3_Y, .w=32, .h=K76_KH },
    { .label="↑", .key=HIDK_UP, .x=305, .y=K76_R3_Y, .w=45, .h=K76_KH },
};
static const hid_key_t k76_row4[] = {   /* 第5排: Fn Ctrl Alt Space ← ↓ → */
    { .label="Fn",   .key=HIDK_TOGGLE_FN,   .x=0,   .y=K76_R4_Y, .w=50,  .h=K76_KH },
    { .label="Ctrl", .key=HIDK_TOGGLE_CTRL, .x=50,  .y=K76_R4_Y, .w=50,  .h=K76_KH },
    { .label="Alt",  .key=HIDK_TOGGLE_ALT,  .x=100, .y=K76_R4_Y, .w=50,  .h=K76_KH },
    { .label="Space",.key=HIDK_SPACE,       .x=150, .y=K76_R4_Y, .w=110, .h=K76_KH },
    { .label="←",    .key=HIDK_LEFT,        .x=260, .y=K76_R4_Y, .w=45,  .h=K76_KH },
    { .label="↓",    .key=HIDK_DOWN,        .x=305, .y=K76_R4_Y, .w=45,  .h=K76_KH },
    { .label="→",    .key=HIDK_RIGHT,       .x=350, .y=K76_R4_Y, .w=50,  .h=K76_KH },
};
#define K76_R0_N (sizeof(k76_row0)/sizeof(hid_key_t))
#define K76_R1_N (sizeof(k76_row1)/sizeof(hid_key_t))
#define K76_R2_N (sizeof(k76_row2)/sizeof(hid_key_t))
#define K76_R3_N (sizeof(k76_row3)/sizeof(hid_key_t))
#define K76_R4_N (sizeof(k76_row4)/sizeof(hid_key_t))

/* 行集合: 供渲染与命中复用的通用结构 */
typedef struct { const hid_key_t *keys; int count; } hid_rowarr_t;

/* 方向箭头字形 (8x12) */
static const uint8_t ARROW_UP[12] = {
    0x18,0x3C,0x7E,0x7E,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,
};
static const uint8_t ARROW_DOWN[12] = {
    0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x7E,0x7E,0x3C,0x18,0x18,
};
static const uint8_t ARROW_LEFT[12] = {
    0x00,0x00,0x18,0x18,0x38,0xF8,0xF8,0x38,0x18,0x18,0x00,0x00,
};
static const uint8_t ARROW_RIGHT[12] = {
    0x00,0x00,0x18,0x18,0x1C,0x1F,0x1F,0x1C,0x18,0x18,0x00,0x00,
};

/* ============ 私有状态 ============ */
static hid_layout_t s_hid_layout = HID_LAYOUT_OP;   /* 当前布局 */
static int  s_tp_top       = HID_TP_Y0;             /* 触控板起始 y (随布局切换) */
static bool s_show_mbtn    = true;                  /* 是否显示底部左右鼠标键 */
static bool s_hid_shift, s_hid_caps, s_hid_ctrl, s_hid_alt;
static uint8_t s_hid_fn_mode = 0;                   /* Fn 三档 0=数字 1=F1-F12 2=符号 */
static bool s_tp_active;
static int  s_tp_start_x, s_tp_start_y;
static int  s_tp_last_x, s_tp_last_y;
static int  s_tp_accum_x, s_tp_accum_y;
static uint32_t s_tp_down_ms;
static int  s_tp_moved;
static bool s_tp_right_sent;

#define PRESS_FLASH_MS  160
static bool     s_press_active;
static int      s_press_x0, s_press_y0, s_press_x1, s_press_y1;
static const char *s_press_label;
static const hid_key_t *s_press_key;
static uint32_t s_press_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ============ 基础功能 ============ */
static void hid_ui_reset(void) {
    switch (s_hid_layout) {
        case HID_LAYOUT_28:       s_tp_top = HID_TP_Y1 + 1;  s_show_mbtn = false; break;
        case HID_LAYOUT_76:       s_tp_top = 156;            s_show_mbtn = true;  break;
        case HID_LAYOUT_TOUCHPAD: s_tp_top = 0;              s_show_mbtn = true;  break;
        case HID_LAYOUT_OP:
        default:                  s_tp_top = HID_TP_Y0;      s_show_mbtn = true;  break;
    }
    s_hid_shift = s_hid_caps = s_hid_ctrl = s_hid_alt = false;
    s_hid_fn_mode = 0;
    usb_hid_key_release();
    usb_hid_mouse_release(HID_MOUSE_L | HID_MOUSE_R | HID_MOUSE_M);
    s_tp_active = false;
    s_tp_moved = 0;
    s_tp_accum_x = s_tp_accum_y = 0;
    s_tp_last_x = s_tp_last_y = 0;
    s_tp_down_ms = 0;
    s_tp_right_sent = false;
}

/* 在 [x, x+w-1] x [y, y+h-1] 内居中画单行文本 (中号 12x18).
 * 支持 UTF-8 方向箭头 ↑↓←→ (E2 86 90-93) 按单个显示字符渲染. */
static void hid_draw_label_centered(st7305_handle_t *lcd, int x, int w,
                                    int y, int h, const char *s, bool inverted) {
    int n = 0;
    const char *p = s;
    while (*p != '\0' && n < 8) {
        if ((unsigned char)*p >= 0x80) p += 3; else p++;
        n++;
    }
    int tw = n * 12;
    if (tw > w) tw = w;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 18) / 2;
    if (ty < 0) ty = 0;
    const char *q = s;
    for (int i = 0; i < n; i++) {
        const uint8_t *bmp = NULL;
        int adv = 1;
        unsigned char c0 = (unsigned char)q[0];
        if (c0 >= 0x80 && q[1] != '\0' && q[2] != '\0' &&
            (unsigned char)q[1] == 0x86 && (unsigned char)q[2] >= 0x90 && (unsigned char)q[2] <= 0x93) {
            adv = 3;
            switch (q[2]) {
                case 0x90: bmp = ARROW_LEFT;  break;
                case 0x91: bmp = ARROW_UP;    break;
                case 0x92: bmp = ARROW_RIGHT; break;
                case 0x93: bmp = ARROW_DOWN;  break;
            }
        }
        if (bmp) {
            draw_ascii_medium_bmp(lcd, tx + i * 12, ty, bmp, inverted);
        } else {
            draw_ascii_medium(lcd, tx + i * 12, ty, q[0], inverted);
        }
        q += adv;
    }
}

/* 16px 中文居中 (Font 16, 同运维助手) */
static void hid_draw_cn_label(st7305_handle_t *lcd, int x, int w, int y, int h,
                              const char *s, bool inv) {
    int n = (int)(strlen(s) / 3);
    int tw = n * 16;
    if (tw > w) tw = w;
    int tx = x + (w - tw) / 2, ty = y + (h - 16) / 2;
    if (ty < 0) ty = 0;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    for (int i = 0; i < n; i++) {
        int idx = font_zh16_find_utf8(s + i * 3);
        if (idx < 0) continue;
        const uint8_t *bmp = zh16_font_data[idx];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                int byte = bmp[r * 2 + (c / 8)];
                st7305_draw_pixel(lcd, tx + i * 16 + c, ty + r,
                                  (byte & (1 << (7 - (c % 8)))) ? fg : bg);
            }
    }
}

/* 单角圆角框: top_left=true 圆左上角(左键), false 圆右上角(右键), 其余直角 */
static void hid_draw_btn_corner(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                                int r, bool top_left) {
    if (r < 1 || r * 2 >= x1 - x0 + 1 || r * 2 >= y1 - y0 + 1) {
        draw_rect_outline(lcd, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        return;
    }
    if (top_left) {
        draw_vline(lcd, x1, y0, y1, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + r, x1, y0, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0, x1, y1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0, y0 + r, y1, ST7305_COLOR_BLACK);
        for (int px = x0; px <= x0 + r; px++) {
            int dx = px - (x0 + r), dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(lcd, px, y0 + dy, ST7305_COLOR_BLACK);
        }
    } else {
        draw_vline(lcd, x0, y0, y1, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0, x1 - r, y0, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0, x1, y1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x1, y0 + r, y1, ST7305_COLOR_BLACK);
        for (int px = x1 - r; px <= x1; px++) {
            int dx = px - (x1 - r), dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(lcd, px, y0 + dy, ST7305_COLOR_BLACK);
        }
    }
}

/* Fn 三档时动态标签: 档1(F1-F12)=显示 "F<序号>", 档2(符号)=显示 sym_label */
static const char *hid_key_label(const hid_key_t *k) {
    if (s_hid_fn_mode == 1 && k->fn_key) {
        static char buf[8];
        int n = (int)k->fn_key - (int)HIDK_F1 + 1;
        snprintf(buf, sizeof buf, "F%d", n);
        return buf;
    }
    if (s_hid_fn_mode == 2 && k->sym_label) {
        return k->sym_label;
    }
    /* Caps 决定字母显示大小写. 默认小写, Caps 开=大写.
     * label 存的是大写字母, 故转小写用 +32. */
    if (k->key >= HIDK_A && k->key <= HIDK_Z) {
        static char c[2];
        c[0] = s_hid_caps ? (char)k->label[0] : (char)(k->label[0] + 32);
        c[1] = '\0';
        return c;
    }
    return k->label;
}

/* 该键是否为激活状态的开关按键 (高亮显示) */
static bool hid_key_toggled_on(const hid_key_t *k) {
    switch (k->key) {
        case HIDK_TOGGLE_CAPS:  return s_hid_caps;
        case HIDK_TOGGLE_CTRL:  return s_hid_ctrl;
        case HIDK_TOGGLE_ALT:   return s_hid_alt;
        case HIDK_TOGGLE_SHIFT: return s_hid_shift;
        case HIDK_TOGGLE_FN:    return s_hid_fn_mode != 0;
        default:                return false;
    }
}

/* 画一个按键, on=true 键反白 (开关键激活态) */
static void hid_draw_key(st7305_handle_t *lcd, const hid_key_t *k, bool on) {
    int w = k->w, h = k->h;
    if (on) {
        fill_rect(lcd, k->x, k->y, k->x + w - 1, k->y + h - 1, ST7305_COLOR_BLACK);
    } else {
        draw_rect_outline(lcd, k->x, k->y, k->x + w - 1, k->y + h - 1, ST7305_COLOR_BLACK);
    }
    hid_draw_label_centered(lcd, k->x, w, k->y, h, hid_key_label(k), on);
}

static void hid_draw_rows(st7305_handle_t *lcd, const hid_rowarr_t *rows, int n) {
    for (int r = 0; r < n; r++) {
        for (int i = 0; i < rows[r].count; i++) {
            const hid_key_t *k = &rows[r].keys[i];
            hid_draw_key(lcd, k, hid_key_toggled_on(k));
        }
    }
}

static int hid_layout_rows(hid_rowarr_t *out, int max) {
    int n = 0;
    switch (s_hid_layout) {
        case HID_LAYOUT_28:
            { const hid_rowarr_t r[4] = {
                { k28_row0, K28_R0_N }, { k28_row1, K28_R1_N },
                { k28_row2, K28_R2_N }, { k28_row3, K28_R3_N } };
              for (int i = 0; i < 4 && n < max; i++) out[n++] = r[i]; }
            break;
        case HID_LAYOUT_76:
            { const hid_rowarr_t r[5] = {
                { k76_row0, K76_R0_N }, { k76_row1, K76_R1_N },
                { k76_row2, K76_R2_N }, { k76_row3, K76_R3_N },
                { k76_row4, K76_R4_N } };
              for (int i = 0; i < 5 && n < max; i++) out[n++] = r[i]; }
            break;
        case HID_LAYOUT_TOUCHPAD:
            break;
        default: {  /* OP */
            const hid_rowarr_t r[4] = {
                { hid_row0, HID_R0_N }, { hid_row1, HID_R1_N },
                { hid_row2, HID_R2_N }, { hid_row3, HID_R3_N } };
            for (int i = 0; i < 4 && n < max; i++) out[n++] = r[i];
            break;
        }
    }
    return n;
}

/* 画触控板底部左右键. OP / 52键 / 纯触控板 三种带触控板布局统一为"运维助手样式":
 * 无外框直铺到底, 底部中央并排 左键(左上圆)/右键(右上圆). 用户要求 OP 与 52键/纯触控板一致. */
static void hid_draw_touchpad(st7305_handle_t *lcd) {
    if (s_show_mbtn) {
        hid_draw_btn_corner(lcd, NEW_MB_XL, NEW_MB_Y, NEW_MB_XL + NEW_MB_W - 1,
                            NEW_MB_Y + NEW_MB_H - 1, 8, true);
        hid_draw_btn_corner(lcd, NEW_MB_XR, NEW_MB_Y, NEW_MB_XR + NEW_MB_W - 1,
                            NEW_MB_Y + NEW_MB_H - 1, 8, false);
        hid_draw_cn_label(lcd, NEW_MB_XL, NEW_MB_W, NEW_MB_Y, NEW_MB_H, "\xe5\xb7\xa6\xe9\x94\xae", false); /* 左键 */
        hid_draw_cn_label(lcd, NEW_MB_XR, NEW_MB_W, NEW_MB_Y, NEW_MB_H, "\xe5\x8f\xb3\xe9\x94\xae", false); /* 右键 */
    }
}

/* ============ 按键执行 ============ */
static void hid_key_press_exec(const hid_key_t *k) {
    switch (k->key) {
        case HIDK_TOGGLE_CAPS:  s_hid_caps  = !s_hid_caps;  return;
        case HIDK_TOGGLE_CTRL:  s_hid_ctrl  = !s_hid_ctrl;  return;
        case HIDK_TOGGLE_ALT:   s_hid_alt   = !s_hid_alt;   return;
        case HIDK_TOGGLE_SHIFT: s_hid_shift = !s_hid_shift; return;
        case HIDK_TOGGLE_FN:    s_hid_fn_mode = (s_hid_fn_mode + 1) % 3; return;
        default: break;
    }
    uint8_t mod = k->mod;
    if (s_hid_ctrl)  mod |= MOD_LCTRL;
    if (s_hid_alt)   mod |= MOD_LALT;
    if (s_hid_shift || s_hid_caps) mod |= MOD_LSHIFT;
    uint8_t code = k->key;
    if (s_hid_fn_mode == 1 && k->fn_key) {
        code = k->fn_key;
    } else if (s_hid_fn_mode == 2 && k->sym_key) {
        code = k->sym_key;
        mod |= k->sym_mod;
    }
    usb_hid_key_tap(mod, code);
}

/* 键盘/鼠标键点击命中测试: 返回 true 表示点到某个键并已执行 HID 动作.
 * 触控板滑动区不在此处理 (由 hid_trackpad_poll 处理), 返回 false. */
static bool hid_ui_hit(ui_ctx_t *ctx, int x, int y) {
    hid_rowarr_t rows[5];
    int rn = hid_layout_rows(rows, 5);
    for (int r = 0; r < rn; r++) {
        for (int i = 0; i < rows[r].count; i++) {
            const hid_key_t *k = &rows[r].keys[i];
            if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h) {
                s_press_x0 = k->x; s_press_y0 = k->y;
                s_press_x1 = k->x + k->w - 1; s_press_y1 = k->y + k->h - 1;
                s_press_label = hid_key_label(k);
                s_press_key = k;
                s_press_ms = now_ms();
                s_press_active = true;
                hid_key_press_exec(k);
                ctx->needs_redraw = true;
                return true;
            }
        }
    }
    /* OP / 52键 / 纯触控板 统一: 底部中央 左键/右键 命中检测 (运维助手样式) */
    if (s_show_mbtn && y >= NEW_MB_Y && y < NEW_MB_Y + NEW_MB_H) {
        if (x >= NEW_MB_XL && x < NEW_MB_XL + NEW_MB_W) { usb_hid_mouse_click(HID_MOUSE_L); return true; }
        if (x >= NEW_MB_XR && x < NEW_MB_XR + NEW_MB_W) { usb_hid_mouse_click(HID_MOUSE_R); return true; }
    }
    return false;
}

/* 触控板: 每帧轮询 input_get_touch_pos 并上报鼠标. 也响应长按右击. */
static void hid_trackpad_poll(ui_ctx_t *ctx) {
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);

    if (s_hid_layout == HID_LAYOUT_28) return;
    uint32_t now = now_ms();
    bool in_tp = down && tx >= 0 && tx < UI_SCREEN_W &&
                 ty >= s_tp_top && ty <= HID_TP_Y1;
    if (in_tp && s_show_mbtn) {
        /* OP / 52键 / 纯触控板 统一: 底部中央 L/R 按钮区不计入鼠标移动 */
        bool on_btn = (ty >= NEW_MB_Y) &&
                     ((tx >= NEW_MB_XL && tx < NEW_MB_XL + NEW_MB_W) ||
                      (tx >= NEW_MB_XR && tx < NEW_MB_XR + NEW_MB_W));
        if (on_btn) in_tp = false;
    }
    if (down && !s_tp_active) {
        if (in_tp) {
            s_tp_active = true;
            s_tp_start_x = s_tp_last_x = tx;
            s_tp_start_y = s_tp_last_y = ty;
            s_tp_down_ms = now;
            s_tp_moved = 0;
            s_tp_accum_x = s_tp_accum_y = 0;
            s_tp_right_sent = false;
        }
        return;
    }
    if (!s_tp_active) return;
    if (down) {
        if (in_tp) {
            int dx = tx - s_tp_last_x;
            int dy = ty - s_tp_last_y;
            s_tp_last_x = tx; s_tp_last_y = ty;
            if (dx != 0 || dy != 0) {
                int sp = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                s_tp_moved += sp;
                /* 速度加速: 拖得越快, 手指位移到鼠标的倍数越大 (低速1:1平稳, 快速大跨越).
                 * scale = 1 + 0.25*speed, 上限 6x. */
                float g = 1.0f + 0.25f * (float)sp;
                if (g > 6.0f) g = 6.0f;
                s_tp_accum_x += (int)(dx * g);
                s_tp_accum_y += (int)(dy * g);
                if (s_tp_accum_x || s_tp_accum_y) {
                    int mx = s_tp_accum_x, my = s_tp_accum_y;
                    if (mx > 127) mx = 127; else if (mx < -128) mx = -128;
                    if (my > 127) my = 127; else if (my < -128) my = -128;
                    usb_hid_mouse_move((int8_t)mx, (int8_t)my);
                    s_tp_accum_x = 0; s_tp_accum_y = 0;
                }
            }
            /* 长按不动 -> 右键 */
            if (!s_tp_right_sent && s_tp_moved < 12 && now - s_tp_down_ms >= 500) {
                usb_hid_mouse_click(HID_MOUSE_R);
                s_tp_right_sent = true;
            }
        }
    } else {
        /* 松手: 短按且未拖动 -> 左键 */
        if (s_tp_moved < 12 && !s_tp_right_sent) {
            usb_hid_mouse_click(HID_MOUSE_L);
        }
        s_tp_active = false;
        s_tp_moved = 0;
    }
    (void)ctx;
}

/* ============ 布局切换 (统一 os_dialog, 4 布局 + 底部固定返回) ============ */
static const char *const s_layout_names[4] = {
    "\xe8\xbf\x90\xe7\xbb\xb4\xe9\x94\xae\xe9\xbc\xa0",            /* 运维键鼠 */
    "28\xe9\x94\xae\xe7\xb4\xa7\xe5\x87\x91\xe9\x94\xae\xe7\x9b\x98", /* 28键紧凑键盘 */
    "52\xe9\x94\xae\xe5\x85\xa8\xe9\x94\xae\xe7\x9b\x98+\xe8\xa7\xa6\xe6\x8e\xa7\xe6\x9d\xbf", /* 52键全键盘+触控板 */
    "\xe7\xba\xaf\xe8\xa7\xa6\xe6\x8e\xa7\xe6\x9d\xbf",            /* 纯触控板 */
};

/* 选择菜单"返回"/底部"退出"/弹窗态按 BACK → 直接退出键鼠页 (不弹确认框).
 * 用户需求: 点下方退出直接退出, 硬件返回键按一次直接退出, 不需要退出确认. */
static void hid_layout_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) {
        os_dialog_clear_all(ctx);      /* 关选择菜单 (若开着) */
        os_pop(ctx);                   /* 退出 HID 页 (触发 p_hid_exit 恢复串口/上滑返回) */
        return;
    }
    if (result < HID_LAYOUT_MAX) {
        s_hid_layout = (hid_layout_t)result;
        hid_ui_reset();
        ctx->needs_redraw = true;
    }
}

/* ============ 模块接口 ============ */
static void p_hid_enter(ui_ctx_t *ctx) {
    esp_err_t ret = usb_hid_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB HID 启动失败: %s", esp_err_to_name(ret));
        os_pop(ctx);
        return;
    }
    /* 键鼠操作区整屏占用, 屏蔽底部上滑退出 (仅物理返回键/长按强制退出) */
    input_set_swipe_back(false);
    hid_ui_reset();
    ESP_LOGI(TAG, "USB HID 已启动");
    /* 3.3 交互: 进入即弹出"键鼠布局"列表弹窗选择布局; 底部固定"返回"退出键鼠页 */
    os_dialog_list(ctx, "\xe9\x80\x89\xe6\x8b\xa9\xe9\x94\xae\xe7\x9b\x98",  /* 选择键盘 */ s_layout_names, 4, s_hid_layout, hid_layout_cb, NULL);
}

static void p_hid_exit(ui_ctx_t *ctx) {
    (void)ctx;
    input_set_swipe_back(true);   /* 恢复全局上滑返回 */
    usb_hid_key_release();
    usb_hid_mouse_release(HID_MOUSE_L | HID_MOUSE_R | HID_MOUSE_M);
    usb_hid_stop();
    s_tp_active = false;
}

static void p_hid_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    ctx->fullscreen = true;

    /* 按击反馈超时还原 */
    if (s_press_active && now_ms() - s_press_ms > PRESS_FLASH_MS) {
        s_press_active = false;
    }

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    hid_rowarr_t rows[5];
    int rn = hid_layout_rows(rows, 5);
    hid_draw_rows(lcd, rows, rn);
    if (s_show_mbtn || s_hid_layout != HID_LAYOUT_28) {
        hid_draw_touchpad(lcd);
    }
    /* 普通按键按击反馈 - 按下后短暂反黑 */
    if (s_press_active && s_press_key &&
        s_press_key->key != HIDK_TOGGLE_CAPS && s_press_key->key != HIDK_TOGGLE_SHIFT) {
        fill_rect(lcd, s_press_x0, s_press_y0, s_press_x1, s_press_y1, ST7305_COLOR_BLACK);
        int w = s_press_x1 - s_press_x0 + 1, h = s_press_y1 - s_press_y0 + 1;
        hid_draw_label_centered(lcd, s_press_x0, w, s_press_y0, h, s_press_label, true);
    }
}

static void p_hid_action(ui_ctx_t *ctx, os_action_t a) {
    switch (a) {
    case OS_ACTION_CONFIRM:
        /* 确认键 → 弹出"选择键盘"列表菜单 (切换布局); 菜单内按返回/底部"退出"直接退出 */
        os_dialog_list(ctx, "\xe9\x80\x89\xe6\x8b\xa9\xe9\x94\xae\xe7\x9b\x98",  /* 选择键盘 */
                       s_layout_names, 4, s_hid_layout, hid_layout_cb, NULL);
        break;
    case OS_ACTION_BACK:
        /* 硬件/手柄返回键 → 直接退出键鼠页 (不弹菜单/确认框); 切换布局改由确认键弹菜单 */
        os_dialog_clear_all(ctx);
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_hid_touch(ui_ctx_t *ctx, int x, int y) {
    return hid_ui_hit(ctx, x, y);
}

static void p_hid_poll(ui_ctx_t *ctx) {
    if (os_modal_active(ctx)) return;   /* 弹窗打开时暂停触控板 */
    hid_trackpad_poll(ctx);
}

static const os_module_t s_mod_usb_hid = {
    .name       = "usb_hid",
    .page_id    = OS_PAGE_USB_HID,
    .on_enter   = p_hid_enter,
    .on_exit    = p_hid_exit,
    .render     = p_hid_render,
    .action     = p_hid_action,
    .touch      = p_hid_touch,
    .poll       = p_hid_poll,
    .fullscreen = true,
};

void os_page_usb_hid_register(void) { os_register(&s_mod_usb_hid); }
