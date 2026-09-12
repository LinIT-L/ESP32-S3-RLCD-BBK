/**
 * page_morse.c — 摩斯密码 页面模块.
 *
 * 嵌入标准 52 键虚拟键盘 (kbd_std), 点击字母/数字即按该字符的摩斯码发声 + 同步震动.
 * 快速 O 功能: 一颗"播放"键 (键盘 Tab 键或顶部 ▶ 按钮) 把已输入内容从头到尾重播一遍.
 * 布局: 顶部显示区(输入文本 + 摩斯串 + 播放/清空按钮) + 底部 52 键键盘.
 *   Freq=700Hz 单音; 摩斯节奏: 点=1 单位  划=3 单位  符号间隔=1  字符间隔=3  词间隔=7 (unit=60ms).
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "keyboard.h"
#include "tone_player.h"
#include "vibrator.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define S_W 400
#define S_H 300

#define MORSE_FREQ  700          /* 播放频率 Hz */
#define MORSE_UNIT  60           /* 单位时间 ms */
#define VIB_STRENGTH 55          /* 震动强度 0~100 */

/* 键盘只显示上 4 排 (去掉最下方无用的 Fn/Ctrl/Alt/空格/方向键 排), 贴底 */
#define KBD_SHOW_ROWS (KBD_ROWS - 1)      /* 4 排 */
#define KBD_BASE_Y    (S_H - KBD_SHOW_ROWS * KBD_ROW)   /* 300-116=184 */

#define TEXT_MAX  40              /* 已输入文本上限 */

/* ---- 按钮区 (播放靠最左贴边, 清空靠最右贴边; 高42 宽=2×42=84) ---- */
#define BTN_W    84
#define BTN_Y1  (KBD_BASE_Y)                    /* 底部贴住键盘顶 */
#define BTN_Y0  (BTN_Y1 - 42 + 1)               /* 高 42 */
#define BTN_L0  0                               /* 贴左边缘 */
#define BTN_L1  (BTN_L0 + BTN_W - 1)
#define BTN_R1  (S_W - 1)                       /* 贴右边缘 */
#define BTN_R0  (BTN_R1 - BTN_W + 1)

/* ---- 摩斯表: 仅支持这些字符, 其余忽略 ---- */
static const char *morse_lookup(char ch) {
    ch = tolower((unsigned char)ch);
    switch (ch) {
        case 'a': return ".-";       case 'b': return "-...";   case 'c': return "-.-.";
        case 'd': return "-..";      case 'e': return ".";      case 'f': return "..-.";
        case 'g': return "--.";      case 'h': return "....";   case 'i': return "..";
        case 'j': return ".---";     case 'k': return "-.-";    case 'l': return ".-..";
        case 'm': return "--";       case 'n': return "-.";     case 'o': return "---";
        case 'p': return ".--.";     case 'q': return "--.-";   case 'r': return ".-.";
        case 's': return "...";      case 't': return "-";      case 'u': return "..-";
        case 'v': return "...-";     case 'w': return ".--";    case 'x': return "-..-";
        case 'y': return "-.--";     case 'z': return "--..";
        case '0': return "-----";    case '1': return ".----";  case '2': return "..---";
        case '3': return "...--";    case '4': return "....-";  case '5': return ".....";
        case '6': return "-....";    case '7': return "--...";  case '8': return "---..";
        case '9': return "----.";
        case '.': return ".-.-.-";   case ',': return "--..--"; case '?': return "..--..";
        case '/': return "-..-.";    default:  return NULL;
    }
}

/* ---- 状态 ---- */
static char    s_text[TEXT_MAX + 1];
static int     s_len;
static bool    s_playing;          /* 后台任务是否正在播放(显示用) */
static int     s_press_k = -1;     /* 键盘按下键索引高亮 */
static QueueHandle_t s_q;          /* 待播放字符队列 */
static bool    s_q_ready;

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }

/* 组装当前文本的完整摩斯串 (供显示) */
static void build_morse_str(char *out, int max) {
    int o = 0;
    for (int i = 0; i < s_len; i++) {
        if (s_text[i] == ' ' || s_text[i] == '\0') {          /* 词间隔: 显示为宽间隔 */
            if (o && o < max - 3) { out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; }
            continue;
        }
        const char *m = morse_lookup(s_text[i]);
        if (!m) continue;
        if (o && o < max - 4) out[o++] = ' ';
        if (o + (int)strlen(m) >= max - 1) break;
        strcpy(&out[o], m); o += (int)strlen(m);
    }
    out[o] = 0;
}

/* ---- 后台播放任务: 队列逐字符播放 声音+震动 同步 ---- */
static void morse_play_one(char ch) {
    if (ch == ' ' || ch == '\0') { vTaskDelay(pdMS_TO_TICKS(7 * MORSE_UNIT)); return; }
    const char *m = morse_lookup(ch);
    if (!m) return;
    for (const char *p = m; *p; p++) {
        int d = (*p == '.') ? MORSE_UNIT : 3 * MORSE_UNIT;
        vibrator_tap((uint16_t)d, VIB_STRENGTH);
        tone_beep(MORSE_FREQ, d);                 /* 阻塞 d ms 发同频短音 */
        vTaskDelay(pdMS_TO_TICKS(MORSE_UNIT));    /* 符号间隔 */
    }
    vTaskDelay(pdMS_TO_TICKS(2 * MORSE_UNIT));    /* 字符间隔(补足 3) */
}

static void morse_task(void *arg) {
    char c;
    while (xQueueReceive(s_q, &c, portMAX_DELAY)) {
        morse_play_one(c);
    }
    vTaskDelete(NULL);
}

static void morse_ensure_worker(void) {
    if (s_q_ready) return;
    s_q = xQueueCreate(16, 1);
    s_q_ready = (s_q != NULL);
    if (s_q_ready) xTaskCreate(morse_task, "morse", 4096, NULL, 5, NULL);
}

/* 输入一个字符: 追加 + 立即排入播放队列 (反馈这一字符) */
static void morse_append(ui_ctx_t *ctx, char c) {
    if (s_len >= TEXT_MAX) return;
    if (!morse_lookup(c) && c != ' ') return;     /* 不支持的字符忽略 */
    s_text[s_len++] = c;
    s_text[s_len] = 0;
    if (s_q_ready) xQueueSend(s_q, &c, 0);
    redraw(ctx);
}

static void morse_bksp(ui_ctx_t *ctx) {
    if (s_len > 0) { s_len--; s_text[s_len] = 0; redraw(ctx); }
}

/* 从头重播全部输入: 清空队列再整体入队 */
static void morse_play_all(ui_ctx_t *ctx) {
    if (s_len == 0 || !s_q_ready) return;
    xQueueReset(s_q);
    for (int i = 0; i < s_len; i++) xQueueSend(s_q, &s_text[i], 0);
    s_playing = true;
    redraw(ctx);
}

static void morse_clear(ui_ctx_t *ctx) {
    s_len = 0; s_text[0] = 0;
    if (s_q_ready) xQueueReset(s_q);
    s_playing = false;
    redraw(ctx);
}

/* ---- 渲染 ---- */
static void morse_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 输入文本 */
    char line1[TEXT_MAX + 8];
    snprintf(line1, sizeof(line1), "%s", s_text);
    int lw1 = text_width(line1);
    if (lw1 > S_W - 16) lw1 = S_W - 16;
    draw_text(lcd, 8, 30, line1, false);

    /* 对应摩斯串 */
    char line2[TEXT_MAX * 6];
    build_morse_str(line2, (int)sizeof(line2));
    int lw2 = text_width(line2);
    if (lw2 > S_W - 16) lw2 = S_W - 16;
    draw_text(lcd, 8, 62, line2, false);

    /* 播放(最左) / 清空(最右) 按钮 */
    draw_rect_outline(lcd, BTN_L0, BTN_Y0, BTN_L1, BTN_Y1, ST7305_COLOR_BLACK);
    draw_text(lcd, BTN_L0 + (BTN_W - text_width("播放")) / 2, BTN_Y0 + 9, "播放", false);
    draw_rect_outline(lcd, BTN_R0, BTN_Y0, BTN_R1, BTN_Y1, ST7305_COLOR_BLACK);
    draw_text(lcd, BTN_R0 + (BTN_W - text_width("清空")) / 2, BTN_Y0 + 9, "清空", false);

    /* 52 键虚拟键盘 (仅前 4 排) */
    kbd_draw_rows(lcd, KBD_BASE_Y, &kbd_std, s_press_k, KBD_SHOW_ROWS);
}

/* ---- 触摸: 先按钮后键盘 ---- */
static bool morse_touch(ui_ctx_t *ctx, int x, int y) {
    if (x < 0 || y < 0) return false;
    /* 顶排按钮 */
    if (y >= BTN_Y0 && y <= BTN_Y1) {
        if (x >= BTN_L0 && x <= BTN_L1) morse_play_all(ctx);
        else if (x >= BTN_R0 && x <= BTN_R1) morse_clear(ctx);
        return true;
    }
    if (y < KBD_BASE_Y || y >= S_H) return false;
    int idx = kbd_hit_rows(&kbd_std, KBD_BASE_Y, x, y, KBD_SHOW_ROWS);
    if (idx < 0) return false;
    s_press_k = idx;
    /* 需可变实例以追踪 Caps/Shift (底层 kbd_press 会改状态) */
    static kbd_t k; k = kbd_std;
    int act = kbd_press(&k, idx);
    if (act == KBD_ACT_CHAR) {
        char c = kbd_char(&k, idx);
        morse_append(ctx, c);
    } else if (act == KBD_ACT_BKSP) {
        morse_bksp(ctx);
    } else if (act == KBD_ACT_SPACE) {
        morse_append(ctx, ' ');
    } else if (act == KBD_ACT_TAB || act == KBD_ACT_ENTER) {
        morse_play_all(ctx);              /* Tab/Enter = 从头播放 */
    }
    return true;
}

/* 松手清除按下高亮 */
static void morse_poll(ui_ctx_t *ctx) {
    static bool prev_down = false;
    bool down = input_get_touch_pos(NULL, NULL);
    if (!down && prev_down) {
        if (s_press_k >= 0) { s_press_k = -1; s_playing = false; redraw(ctx); }
    }
    prev_down = down;
}

/* ---- 按键 (方向键导航无键盘时的兜底入口; 触发播放全部) ---- */
static void morse_action(ui_ctx_t *ctx, os_action_t a) {
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { os_pop(ctx); return; }
    if (a == OS_ACTION_CONFIRM) morse_play_all(ctx);
}

/* ---- 生命周期 ---- */
static void morse_enter(ui_ctx_t *ctx) {
    tone_player_init();
    vibrator_init();
    s_len = 0; s_text[0] = 0; s_playing = false;
    morse_ensure_worker();
    ctx->needs_redraw = true;
}
static void morse_exit(ui_ctx_t *ctx) {
    tone_stop();
    vibrator_stop();
}

static const os_module_t s_mod_morse = {
    .name      = "morse",
    .page_id   = OS_PAGE_MORSE,
    .on_enter  = morse_enter,
    .on_exit   = morse_exit,
    .render    = morse_render,
    .action    = morse_action,
    .touch     = morse_touch,
    .poll      = morse_poll,
    .fullscreen = true,
};

void os_page_morse_register(void) { os_register(&s_mod_morse); }