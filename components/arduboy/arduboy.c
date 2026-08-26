/**
 * @file arduboy.c
 * @brief Arduboy2 兼容层实现
 *
 * 帧缓冲格式 (Arduboy/SSD1306 一致):
 *   s_buffer[page * 128 + col], page = y/8 (0..7), col = x (0..127)
 *   每字节的 bit 0 = 该列 8 像素段的顶部, bit 7 = 底部
 *   总大小 128 * 8 = 1024 字节
 *
 * 显示缩放: 128x64 -> 384x256 (3x 水平 + 4x 垂直), 居中于 400x300 (offset 8, 22)
 *   转换时先在内存构建 384x256 水平排列 1bpp buffer (12288 字节),
 *   再调用 st7305_blit_1bit 一次批量写入, 保证性能.
 *
 * 按键映射 (仅 2 个物理按键):
 *   KEY  (GPIO18, input idx=0) = ARDUBOY_BTN_A
 *   BOOT (GPIO0,  input idx=1) = ARDUBOY_BTN_B
 *   方向键由游戏内部根据 A/B 的 held/edge 自行解释.
 */
#include "arduboy.h"
#include "input.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/task.h"

/* === 缩放参数 === */
#define ARD_SCALE_X     3
#define ARD_SCALE_Y     4
#define ARD_SCALED_W    (ARDUBOY_WIDTH  * ARD_SCALE_X)   /* 384 */
#define ARD_SCALED_H    (ARDUBOY_HEIGHT * ARD_SCALE_Y)   /* 256 */
#define ARD_OFFSET_X    ((ST7305_WIDTH  - ARD_SCALED_W) / 2)   /* 8  */
#define ARD_OFFSET_Y    ((ST7305_HEIGHT - ARD_SCALED_H) / 2)   /* 22 */
#define ARD_SCALED_BPR  (ARD_SCALED_W / 8)                /* 48 字节/行 */

/* === 128x64 1bpp 帧缓冲 (Arduboy 垂直格式) === */
EXT_RAM_BSS_ATTR static uint8_t s_buffer[(ARDUBOY_WIDTH * ARDUBOY_HEIGHT) / 8];  /* 1024 字节 */

/* === 缩放后的 384x256 1bpp 水平格式 buffer (用于 blit, 动态分配避免 DRAM 溢出) === */
static uint8_t *s_scaled = NULL;   /* 12288 字节, arduboy_init() 时 heap 分配 */

/* === ST7305 句柄 === */
static st7305_handle_t *s_lcd = NULL;

/* === 文字光标 === */
static int s_cursor_x = 0;
static int s_cursor_y = 0;

/* === 按键状态 === */
static uint8_t s_btn_curr = 0;      /* 当前按住 */
static uint8_t s_btn_pressed = 0;   /* 本帧刚按下 (edge) */
static uint8_t s_btn_released = 0;  /* 本帧刚松开 (edge) */

/* === 5x7 ASCII 字体 (32-127, 每字符 5 字节, 列优先, bit0=顶部) === */
static const uint8_t s_font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' '  32 */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!'  */
    {0x00,0x07,0x00,0x07,0x00}, /* '"'  */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#'  */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$'  */
    {0x23,0x13,0x08,0x64,0x62}, /* '%'  */
    {0x36,0x49,0x55,0x22,0x50}, /* '&'  */
    {0x00,0x05,0x03,0x00,0x00}, /* '''  */
    {0x00,0x1C,0x22,0x41,0x00}, /* '('  */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')'  */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* '*'  */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+'  */
    {0x00,0x50,0x30,0x00,0x00}, /* ','  */
    {0x08,0x08,0x08,0x08,0x08}, /* '-'  */
    {0x00,0x60,0x60,0x00,0x00}, /* '.'  */
    {0x20,0x10,0x08,0x04,0x02}, /* '/'  */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0'  48 */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1'  */
    {0x42,0x61,0x51,0x49,0x46}, /* '2'  */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3'  */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4'  */
    {0x27,0x45,0x45,0x45,0x39}, /* '5'  */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6'  */
    {0x01,0x71,0x09,0x05,0x03}, /* '7'  */
    {0x36,0x49,0x49,0x49,0x36}, /* '8'  */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9'  */
    {0x00,0x36,0x36,0x00,0x00}, /* ':'  58 */
    {0x00,0x56,0x36,0x00,0x00}, /* ';'  */
    {0x00,0x08,0x14,0x22,0x41}, /* '<'  */
    {0x14,0x14,0x14,0x14,0x14}, /* '='  */
    {0x41,0x22,0x14,0x08,0x00}, /* '>'  */
    {0x02,0x01,0x51,0x09,0x06}, /* '?'  */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@'  64 */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A'  */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B'  */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C'  */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D'  */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E'  */
    {0x7F,0x09,0x09,0x01,0x01}, /* 'F'  */
    {0x3E,0x41,0x41,0x51,0x32}, /* 'G'  */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H'  */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I'  */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J'  */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K'  */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L'  */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 'M'  */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N'  */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O'  */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P'  */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q'  */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R'  */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S'  */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T'  */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U'  */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V'  */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 'W'  */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X'  */
    {0x03,0x04,0x78,0x04,0x03}, /* 'Y'  */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z'  */
    {0x00,0x7F,0x41,0x41,0x00}, /* '['  91 */
    {0x02,0x04,0x08,0x10,0x20}, /* '\'  */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']'  */
    {0x04,0x02,0x01,0x02,0x04}, /* '^'  */
    {0x40,0x40,0x40,0x40,0x40}, /* '_'  */
    {0x00,0x01,0x02,0x04,0x00}, /* '`'  96 */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a'  */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b'  */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c'  */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd'  */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e'  */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f'  */
    {0x08,0x14,0x54,0x54,0x3C}, /* 'g'  */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h'  */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i'  */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j'  */
    {0x00,0x7F,0x10,0x28,0x44}, /* 'k'  */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l'  */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm'  */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n'  */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o'  */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p'  */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q'  */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r'  */
    {0x48,0x54,0x54,0x54,0x20}, /* 's'  */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't'  */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u'  */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v'  */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w'  */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x'  */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y'  */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z'  */
    {0x00,0x08,0x36,0x41,0x00}, /* '{'  123 */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|'  */
    {0x00,0x41,0x36,0x08,0x00}, /* '}'  */
    {0x02,0x01,0x02,0x04,0x02}, /* '~'  127 */
};

/* ===================== 公共 API ===================== */

void arduboy_init(st7305_handle_t *lcd) {
    s_lcd = lcd;
    memset(s_buffer, 0, sizeof(s_buffer));
    if (s_scaled == NULL) {
        s_scaled = heap_caps_malloc(ARD_SCALED_H * ARD_SCALED_BPR, MALLOC_CAP_SPIRAM);
        if (s_scaled == NULL) {
            s_scaled = malloc(ARD_SCALED_H * ARD_SCALED_BPR);
        }
    }
    s_cursor_x = 0;
    s_cursor_y = 0;
    s_btn_curr = 0;
    s_btn_pressed = 0;
    s_btn_released = 0;
}

void arduboy_clear(void) {
    memset(s_buffer, 0, sizeof(s_buffer));
}

uint8_t *arduboy_get_buffer(void) {
    return s_buffer;
}

void arduboy_draw_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= ARDUBOY_WIDTH || y < 0 || y >= ARDUBOY_HEIGHT) return;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    uint8_t *p = &s_buffer[(y >> 3) * ARDUBOY_WIDTH + x];
    if (color) *p |= mask;
    else       *p &= (uint8_t)~mask;
}

uint8_t arduboy_get_pixel(int x, int y) {
    if (x < 0 || x >= ARDUBOY_WIDTH || y < 0 || y >= ARDUBOY_HEIGHT) return 0;
    return (uint8_t)((s_buffer[(y >> 3) * ARDUBOY_WIDTH + x] >> (y & 7)) & 1);
}

void arduboy_draw_hline(int x, int y, int w, uint8_t color) {
    for (int i = 0; i < w; i++) arduboy_draw_pixel(x + i, y, color);
}

void arduboy_draw_vline(int x, int y, int h, uint8_t color) {
    for (int i = 0; i < h; i++) arduboy_draw_pixel(x, y + i, color);
}

void arduboy_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    arduboy_draw_hline(x, y, w, color);
    arduboy_draw_hline(x, y + h - 1, w, color);
    arduboy_draw_vline(x, y, h, color);
    arduboy_draw_vline(x + w - 1, y, h, color);
}

void arduboy_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++) arduboy_draw_hline(x, y + j, w, color);
}

/* Bresenham 画圆 (空心) */
void arduboy_draw_circle(int x0, int y0, int r, uint8_t color) {
    if (r <= 0) return;
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;
    arduboy_draw_pixel(x0, y0 + r, color);
    arduboy_draw_pixel(x0, y0 - r, color);
    arduboy_draw_pixel(x0 + r, y0, color);
    arduboy_draw_pixel(x0 - r, y0, color);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        arduboy_draw_pixel(x0 + x, y0 + y, color);
        arduboy_draw_pixel(x0 - x, y0 + y, color);
        arduboy_draw_pixel(x0 + x, y0 - y, color);
        arduboy_draw_pixel(x0 - x, y0 - y, color);
        arduboy_draw_pixel(x0 + y, y0 + x, color);
        arduboy_draw_pixel(x0 - y, y0 + x, color);
        arduboy_draw_pixel(x0 + y, y0 - x, color);
        arduboy_draw_pixel(x0 - y, y0 - x, color);
    }
}

/* Bresenham 画圆 (实心) */
void arduboy_fill_circle(int x0, int y0, int r, uint8_t color) {
    if (r <= 0) return;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                arduboy_draw_pixel(x0 + x, y0 + y, color);
            }
        }
    }
}

/* 1bpp bitmap, Arduboy 垂直格式: 每字节 8 个垂直像素, 列优先
 *  byte[col * pages + page], pages = ceil(h/8), bit0=顶部 */
void arduboy_draw_bitmap(int x, int y, const uint8_t *bitmap, int w, int h, uint8_t color) {
    if (!bitmap || w <= 0 || h <= 0) return;
    int pages = (h + 7) / 8;
    for (int col = 0; col < w; col++) {
        for (int page = 0; page < pages; page++) {
            uint8_t bits = bitmap[col * pages + page];
            for (int bit = 0; bit < 8; bit++) {
                int py = page * 8 + bit;
                if (py >= h) break;
                if (bits & (1u << bit)) {
                    arduboy_draw_pixel(x + col, y + py, color);
                }
            }
        }
    }
}

void arduboy_draw_compressed(int x, int y, const uint8_t *bitmap, int w, int h, uint8_t color) {
    (void)x; (void)y; (void)bitmap; (void)w; (void)h; (void)color;
    /* 暂未实现 */
}

void arduboy_set_cursor(int x, int y) {
    s_cursor_x = x;
    s_cursor_y = y;
}

void arduboy_print_char(char c) {
    if (c < 32 || c > 127) c = ' ';
    const uint8_t *glyph = s_font5x7[(uint8_t)c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                arduboy_draw_pixel(s_cursor_x + col, s_cursor_y + row, ARDUBOY_WHITE);
            }
        }
    }
    s_cursor_x += 6;  /* 5 像素字宽 + 1 像素间隔 */
}

void arduboy_print(const char *str) {
    if (!str) return;
    while (*str) {
        arduboy_print_char(*str);
        str++;
    }
}

/* === 按键轮询: 每帧调用一次, 采样 GPIO 并计算 edge ===
 * KEY (idx=0) -> BTN_A, BOOT (idx=1) -> BTN_B
 */
void arduboy_poll_input(void) {
    uint8_t now = 0;
    if (input_is_held(0)) now |= ARDUBOY_BTN_A;
    if (input_is_held(1)) now |= ARDUBOY_BTN_B;
    s_btn_pressed  = (uint8_t)(now & ~s_btn_curr);   /* 本帧新按下 */
    s_btn_released = (uint8_t)(s_btn_curr & ~now);   /* 本帧新松开 */
    s_btn_curr = now;
}

uint8_t arduboy_buttons_state(void) { return s_btn_curr; }
uint8_t arduboy_buttons_pressed(void) { return s_btn_pressed; }
uint8_t arduboy_buttons_released(void) { return s_btn_released; }

/* === 帧刷新: 128x64 垂直 fb -> 384x256 水平 fb -> ST7305 blit ===
 * 优化: 逐源行计算一行 48 字节的水平 pattern, 再复制到 4 个目标行.
 */
void arduboy_display(void) {
    if (!s_lcd) return;

    /* 清空缩放 buffer (全 0 = 全白背景) */
    memset(s_scaled, 0, sizeof(s_scaled));

    uint8_t row_pat[ARD_SCALED_BPR];
    for (int y = 0; y < ARDUBOY_HEIGHT; y++) {
        memset(row_pat, 0, ARD_SCALED_BPR);
        int page = y >> 3;
        int bit  = y & 7;
        uint8_t mask = (uint8_t)(1u << bit);
        const uint8_t *src_row = &s_buffer[page * ARDUBOY_WIDTH];
        for (int x = 0; x < ARDUBOY_WIDTH; x++) {
            if (src_row[x] & mask) {
                /* 该源像素亮 -> 在水平 pattern 中置 3 个连续位 */
                int dx0 = x * ARD_SCALE_X;
                row_pat[dx0 >> 3] |= (uint8_t)(0x80u >> (dx0 & 7));
                row_pat[(dx0 + 1) >> 3] |= (uint8_t)(0x80u >> ((dx0 + 1) & 7));
                row_pat[(dx0 + 2) >> 3] |= (uint8_t)(0x80u >> ((dx0 + 2) & 7));
            }
        }
        /* 复制 pattern 到 SCALE_Y 个目标行 */
        int dy0 = y * ARD_SCALE_Y;
        uint8_t *dst0 = &s_scaled[dy0 * ARD_SCALED_BPR];
        for (int dy = 0; dy < ARD_SCALE_Y; dy++) {
            memcpy(dst0 + dy * ARD_SCALED_BPR, row_pat, ARD_SCALED_BPR);
        }
    }

    /* 清屏为白底, 再 blit 缩放后的位图 (bit=1 画黑点) */
    st7305_clear(s_lcd, ST7305_COLOR_WHITE);
    st7305_blit_1bit(s_lcd, ARD_OFFSET_X, ARD_OFFSET_Y,
                     ARD_SCALED_W, ARD_SCALED_H, s_scaled);
    st7305_flush(s_lcd);
}

void arduboy_idle(void) {
    /* 帧率控制由模拟器主循环负责, 此处为兼容空实现 */
}

void arduboy_tunes_play(uint16_t freq, uint16_t duration) {
    (void)freq; (void)duration;  /* 无扬声器 */
}

void arduboy_eeprom_write(int addr, uint8_t val) {
    (void)addr; (void)val;  /* 不持久化 */
}

uint8_t arduboy_eeprom_read(int addr) {
    (void)addr;
    return 0;
}

uint32_t arduboy_millis(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
