/* gam4980 模拟器 wrapper
 * 集成 libretro 核心到 ST7305 反射式 LCD
 *
 * 帧缓冲: libretro 输出 159x96 RGB565 (实际只有 0x0000 黑 / 0xd6da 灰白两色)
 *        → 1bpp 缓存 (160*96/8 = 1920 bytes) → 直接 blit 到 ST7305 fb
 */
#include "gam4980_emu.h"
#include "libretro.h"
#include "menu_system.h"
#include "font_zh.h"
#include "audio_player.h"
#include "virtual_keys.h"
#include "web_gamepad.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#include "input.h"
#include "bt_manager.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "GAM4980";

/* 引用全局菜单状态 (获取 game_status_bar 设置) */
extern menu_state_t g_menu;

/* libretro 输出: 159x96 RGB565 (LCD_WIDTH+1 x LCD_HEIGHT = 160x96) */
#define CORE_W 160
#define CORE_H 96
#define CORE_BPR (CORE_W / 8)
#define FB_STRIDE (ST7305_HEIGHT >> 2)        /* 75 */
#define FB_BYTES  ((ST7305_WIDTH / 4) * (ST7305_HEIGHT / 2))  /* 15000 */
#define CORE_FB_BYTES (CORE_BPR * CORE_H) /* 1bpp 缓冲总字节数 = 1920 */

/* 游戏区在 ST7305 屏幕居中: 160x96 * 2 = 320x192 (2x 缩放全屏) */
#define SCALE 2
#define GAME_X ((400 - CORE_W * SCALE) / 2)   /* 40 */
#define GAME_Y ((300 - CORE_H * SCALE) / 2)   /* 54 */

/* 全屏模式: 保持原比例 160:96 = 5:3, 宽度拉满 400, 高度 240, 上下各留 30 */
#define FS_SCALED_W 400
#define FS_SCALED_H ((FS_SCALED_W * CORE_H) / CORE_W)  /* 240 */
#define FS_BAR_H 30
#define FS_GAME_Y FS_BAR_H
#define FS_BPR (FS_SCALED_W / 8)  /* 50 */

/* libretro 回调 */
static st7305_handle_t *g_lcd = NULL;
static volatile bool g_running = false;
static bool s_home_prev = false;  /* 退出到菜单键(F_EXIT)上升沿检测 */
static bool g_core_initialized = false;
static volatile bool g_init_in_progress = false;
static char g_rom_path[128] = {0};

/* 后台初始化任务栈: 动态分配 (一次性任务, 失败可回退; 静态分配会占用过多内部 RAM 导致蓝牙初始化失败) */
#define GAM4980_BG_INIT_STACK_SIZE  (8 * 1024)

/* 后台初始化进度 (供菜单状态栏显示) */
static volatile int  g_bg_progress = 0;
static volatile bool g_bg_active = false;

/* 显示模式 (false=点对点2x缩放, true=全屏400x240) - 默认全屏 (用户要求) */
static display_mode_t g_display_mode = DISP_MODE_FULLSCREEN;

/* 状态栏信息 (供全屏模式使用) */
static uint8_t g_status_battery = 100;
static bool g_status_pad_connected = false;



/* 8x12 ASCII 字体 */
static const uint8_t g_font8x12[][12] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x00,0x18,0x18}, /* '!' */
    {0x00,0x00,0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x00,0x00,0x00,0x3C,0x7E,0x3C,0x3C,0x7E,0x3C,0x00,0x00,0x00}, /* '#' */
    {0x00,0x00,0x18,0x3C,0x18,0x70,0x0E,0x1C,0x3C,0x18,0x00,0x00}, /* '$' */
    {0x00,0x00,0x00,0x63,0x63,0x06,0x0C,0x18,0x30,0x63,0x63,0x00}, /* '%' */
    {0x00,0x00,0x00,0x1C,0x36,0x1C,0x3B,0x6E,0x66,0x3B,0x00,0x00}, /* '&' */
    {0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x00,0x00,0x00,0x0E,0x1C,0x38,0x38,0x38,0x1C,0x0E,0x00,0x00}, /* '(' */
    {0x00,0x00,0x00,0x70,0x38,0x1C,0x1C,0x1C,0x38,0x70,0x00,0x00}, /* ')' */
    {0x00,0x00,0x00,0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00,0x00}, /* '*' */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* ',' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00}, /* '.' */
    {0x00,0x00,0x00,0x06,0x0E,0x1C,0x38,0x70,0x60,0x00,0x00,0x00}, /* '/' */
    {0x00,0x00,0x3C,0x66,0x6E,0x7E,0x76,0x66,0x66,0x3C,0x00,0x00}, /* '0' */
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x7E,0x00,0x00}, /* '1' */
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00}, /* '2' */
    {0x00,0x00,0x7E,0x0C,0x18,0x0C,0x06,0x06,0x66,0x3C,0x00,0x00}, /* '3' */
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x0C,0x00,0x00}, /* '4' */
    {0x00,0x00,0x7E,0x60,0x7C,0x06,0x06,0x06,0x66,0x3C,0x00,0x00}, /* '5' */
    {0x00,0x00,0x3C,0x60,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00,0x00}, /* '6' */
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00}, /* '7' */
    {0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C,0x00,0x00}, /* '8' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C,0x00,0x00}, /* '9' */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* ':' */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30,0x00,0x00}, /* ';' */
    {0x00,0x00,0x00,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00,0x00}, /* '<' */
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00}, /* '=' */
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00,0x00}, /* '>' */
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x18,0x00,0x18,0x00,0x00}, /* '?' */
    {0x00,0x00,0x3C,0x66,0x66,0x6E,0x6E,0x60,0x60,0x3C,0x00,0x00}, /* '@' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x00}, /* 'A' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00}, /* 'B' */
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x60,0x66,0x3C,0x00,0x00}, /* 'C' */
    {0x00,0x00,0x78,0x6C,0x66,0x66,0x66,0x66,0x6C,0x78,0x00,0x00}, /* 'D' */
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x7E,0x00,0x00}, /* 'E' */
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'F' */
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x3C,0x00,0x00}, /* 'G' */
    {0x00,0x00,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'H' */
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'I' */
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00,0x00}, /* 'J' */
    {0x00,0x00,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0x00,0x00}, /* 'K' */
    {0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00}, /* 'L' */
    {0x00,0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0x00,0x00}, /* 'M' */
    {0x00,0x00,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x66,0x00,0x00}, /* 'N' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'O' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'P' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00,0x00}, /* 'Q' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x66,0x00,0x00}, /* 'R' */
    {0x00,0x00,0x3C,0x66,0x60,0x3C,0x06,0x66,0x66,0x3C,0x00,0x00}, /* 'S' */
    {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'T' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'U' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00}, /* 'V' */
    {0x00,0x00,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x63,0x00,0x00}, /* 'W' */
    {0x00,0x00,0x66,0x66,0x3C,0x3C,0x3C,0x3C,0x66,0x66,0x00,0x00}, /* 'X' */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'Y' */
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x60,0x40,0x7E,0x00,0x00}, /* 'Z' */
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00}, /* '[' */
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x03,0x00,0x00,0x00}, /* '\' */
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00}, /* ']' */
    {0x00,0x00,0x18,0x3C,0x66,0x42,0x00,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, /* '_' */
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3E,0x00,0x00}, /* 'a' */
    {0x00,0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00}, /* 'b' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00}, /* 'c' */
    {0x00,0x00,0x00,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00}, /* 'd' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x7E,0x60,0x3C,0x00,0x00}, /* 'e' */
    {0x00,0x00,0x00,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00,0x00}, /* 'f' */
    {0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C}, /* 'g' */
    {0x00,0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'h' */
    {0x00,0x00,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'i' */
    {0x00,0x00,0x00,0x06,0x00,0x06,0x06,0x06,0x06,0x06,0x06,0x3C}, /* 'j' */
    {0x00,0x00,0x00,0x60,0x60,0x6C,0x78,0x78,0x6C,0x66,0x00,0x00}, /* 'k' */
    {0x00,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'l' */
    {0x00,0x00,0x00,0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00,0x00}, /* 'm' */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'n' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'o' */
    {0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60}, /* 'p' */
    {0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06,0x06,0x06}, /* 'q' */
    {0x00,0x00,0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'r' */
    {0x00,0x00,0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00,0x00,0x00}, /* 's' */
    {0x00,0x00,0x00,0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00,0x00}, /* 't' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00}, /* 'u' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00}, /* 'v' */
    {0x00,0x00,0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00,0x00,0x00}, /* 'w' */
    {0x00,0x00,0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00,0x00,0x00}, /* 'x' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C}, /* 'y' */
    {0x00,0x00,0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00}, /* 'z' */
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00}, /* '{' */
    {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* '|' */
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00}, /* '}' */
    {0x00,0x00,0x00,0x00,0x76,0x7E,0x1F,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

/* 绘制游戏名称 - 高性能版: 直接写 fb, 跳过 st7305_draw_pixel 开销.
 * 中文12x12, 英文8x12. 每个像素直接计算 fb 索引并写入. */
static void draw_game_name(st7305_handle_t *lcd, int x, int y, const char *text) {
    if (!lcd || !text) return;
    uint8_t *fb = lcd->fb;
    if (!fb) return;
    int cx = x;
    /* FB_STRIDE = 75, compile time */
    for (const char *p = text; *p; ) {
        if ((uint8_t)*p >= 0x80 && p[1] && p[2]) {
            int idx = font_zh_find_utf8(p);
            if (idx >= 0) {
                const uint8_t *bmp = zh_font_data[idx];
                for (int row = 0; row < 24; row += 2) {
                    int ty = y + row / 2;
                    if ((uint32_t)ty >= (uint32_t)ST7305_HEIGHT) continue;
                    int inv_y = ST7305_HEIGHT - 1 - ty;
                    int y_group = inv_y >> 2;
                    int y_sub = inv_y & 3;
                    uint8_t *fb_row = fb + y_group;
                    for (int col = 0; col < 24; col += 2) {
                        int byte_idx = row * 3 + (col / 8);
                        uint8_t bits = bmp[byte_idx];
                        if (!(bits & (1 << (7 - (col % 8))))) continue;
                        int tx = cx + col / 2;
                        if ((uint32_t)tx >= (uint32_t)ST7305_WIDTH) continue;
                        int x_pair = tx >> 1;
                        uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (tx & 1));
                        fb_row[(uint32_t)x_pair * FB_STRIDE] &= ~(uint8_t)(1u << bit);
                    }
                }
            }
            cx += 12;
            p += 3;
        } else {
            char c = *p;
            if (c >= 32 && c <= 126) {
                const uint8_t *bmp = g_font8x12[(uint8_t)c - 32];
                for (int row = 0; row < 12; row++) {
                    int ty = y + row;
                    if ((uint32_t)ty >= (uint32_t)ST7305_HEIGHT) continue;
                    int inv_y = ST7305_HEIGHT - 1 - ty;
                    int y_group = inv_y >> 2;
                    int y_sub = inv_y & 3;
                    uint8_t *fb_row = fb + y_group;
                    uint8_t bits = bmp[row];
                    if (!bits) continue;
                    for (int col = 0; col < 8; col++) {
                        if (!(bits & (1 << (7 - col)))) continue;
                        int tx = cx + col;
                        if ((uint32_t)tx >= (uint32_t)ST7305_WIDTH) continue;
                        int x_pair = tx >> 1;
                        uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (tx & 1));
                        fb_row[(uint32_t)x_pair * FB_STRIDE] &= ~(uint8_t)(1u << bit);
                    }
                }
            }
            cx += 8;
            p++;
        }
    }
}

/* 计算游戏名称宽度 (中文12px, 英文8px) */
static int calc_game_name_width(const char *text) {
    if (!text) return 0;
    int w = 0;
    for (const char *p = text; *p; ) {
        if ((uint8_t)*p >= 0x80 && p[1] && p[2]) {
            w += 12;
            p += 3;
        } else {
            w += 8;
            p++;
        }
    }
    return w;
}

void gam4980_set_status_info(uint8_t battery, bool pad_connected) {
    g_status_battery = battery;
    g_status_pad_connected = pad_connected;
}

void gam4980_set_display_mode(display_mode_t mode) { g_display_mode = mode; }
display_mode_t gam4980_get_display_mode(void) { return g_display_mode; }



/* 兼容旧接口 */
void gam4980_set_fullscreen(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_display_mode = (display_mode_t)mode;
}
bool gam4980_get_fullscreen(void) { return g_display_mode != DISP_MODE_POINT2POINT; }

/* libretro core callbacks (内部链接) */
void retro_init(void);
void retro_deinit(void);
void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);
bool retro_load_game(const struct retro_game_info *game);
void retro_run(void);
void retro_unload_game(void);

/* 存档持久化: libretro.c 提供的 helper, 用于从 sys_flash 读/写 80KB 存档区 */
void gam4980_flash_read_save(uint8_t *buf, size_t size);
void gam4980_flash_write_save(const uint8_t *buf, size_t size);
/* V1.0.52: 返回游戏数据在 PSRAM 中的目标区指针 */
uint8_t *gam4980_retro_rom_target(void);
/* 游戏请求关机/退出 (halt) 检测: 选「退出游戏」时核心进入 halt 状态 */
bool gam4980_retro_halt_requested(void);
/* V1.0.53: halt 位实时状态 (帧末仍置位才判定关机) */
bool gam4980_retro_halt_live(void);
void gam4980_retro_reset_halt(void);

/* libretro 日志 fallback */
static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[256]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    ESP_LOGI("CORE", "%s", buf);
}

/* 帧渲染: libretro RGB565 (黑白) → ST7305 */

/* 预计算查找表,避免渲染时的除法运算 */
static uint8_t g_fs_row_map[FS_SCALED_H];      /* 全屏模式: dy -> sy */
static uint16_t g_fs_col_map[FS_SCALED_W];     /* 全屏模式: dx -> sx */
/* V1.0.53+: 全屏覆盖率抗锯齿需要源像素 -> 输出列/行区间 (含端点) */
static uint16_t g_fs_col_start[CORE_W];        /* 源列 sx -> 输出起始列 */
static uint16_t g_fs_col_end[CORE_W];          /* 源列 sx -> 输出结束列 */
static uint8_t  g_fs_row_start[CORE_H];        /* 源行 sy -> 输出起始行 */
static uint8_t  g_fs_row_end[CORE_H];          /* 源行 sy -> 输出结束行 */
static bool g_lookup_initialized = false;

/* V1.0.53: BBK 抗锯齿开关 (0=关 [纯最近邻放大], >0=开 [覆盖率抗锯齿, 圆滑拐角]) */
static int g_pic_opt = 1;
static bool s_wallpaper_mode = false;  /* 游戏壁纸: 任意按键强制退出 (V1.0.64) */

/* V1.0.58+: pic_opt 档位 (与菜单"抗锯齿"两态循环对应):
 *   0 = 关 (2x 最近邻放大, 全实心)
 *   1 = EPX (Scale2x / AdvMAME2x, 仅处理斜线/拐角, 像素风格边缘平滑)
 * 全屏模式(非 2x 整数倍)下 EPX 档走覆盖率抗锯齿作平滑. */
void gam4980_set_wallpaper_mode(bool on) { s_wallpaper_mode = on; }

void gam4980_set_pic_opt(int level) {
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    g_pic_opt = level;
    const char *nm[] = {"\xe5\x85\xb3", "EPX"};  /* 关 / EPX */
    ESP_LOGI(TAG, "BBK 抗锯齿模式: %s", nm[level]);
}

static void init_lookup_tables(void) {
    if (g_lookup_initialized) return;

    /* 全屏模式行映射 */
    for (int dy = 0; dy < FS_SCALED_H; dy++) {
        g_fs_row_map[dy] = (uint8_t)((dy * CORE_H) / FS_SCALED_H);
    }
    /* 全屏模式列映射 */
    for (int dx = 0; dx < FS_SCALED_W; dx++) {
        g_fs_col_map[dx] = (uint16_t)((dx * CORE_W) / FS_SCALED_W);
    }

    /* 反查: 源列/源行 -> 输出区间 (含端点), 供覆盖率抗锯齿使用 */
    for (int sx = 0; sx < CORE_W; sx++) { g_fs_col_start[sx] = FS_SCALED_W; g_fs_col_end[sx] = 0; }
    for (int dx = 0; dx < FS_SCALED_W; dx++) {
        uint16_t sx = g_fs_col_map[dx];
        if (dx < g_fs_col_start[sx]) g_fs_col_start[sx] = (uint16_t)dx;
        if (dx > g_fs_col_end[sx])   g_fs_col_end[sx]   = (uint16_t)dx;
    }
    for (int sy = 0; sy < CORE_H; sy++) { g_fs_row_start[sy] = FS_SCALED_H; g_fs_row_end[sy] = 0; }
    for (int dy = 0; dy < FS_SCALED_H; dy++) {
        uint8_t sy = g_fs_row_map[dy];
        if (dy < g_fs_row_start[sy]) g_fs_row_start[sy] = (uint8_t)dy;
        if (dy > g_fs_row_end[sy])   g_fs_row_end[sy]   = (uint8_t)dy;
    }

    g_lookup_initialized = true;
}

/* 在 ST7305 fb 中清除一个物理点 (置黑). x,y 为屏坐标 (从 RGB 原点算).
 * fb[idx] 的位 = 7 - ((y_sub<<1) | (x&1)), y_sub = ((ST7305_HEIGHT-1-y) & 3) */
static inline void IRAM_ATTR fb_clear_pixel(uint8_t *fb, int x, int y) {
    int inv_y = ST7305_HEIGHT - 1 - y;
    int y_group = inv_y >> 2;
    int y_sub = inv_y & 3;
    uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (x & 1));
    fb[(uint32_t)(x >> 1) * FB_STRIDE + (uint32_t)y_group] &= ~(uint8_t)(1u << bit);
}

/* 在 ST7305 fb 中设置一个物理点 (置白). 供抗锯齿圆角用: 去掉外凸的黑角像素 */
static inline void IRAM_ATTR fb_set_pixel_white(uint8_t *fb, int x, int y) {
    int inv_y = ST7305_HEIGHT - 1 - y;
    int y_group = inv_y >> 2;
    int y_sub = inv_y & 3;
    uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (x & 1));
    fb[(uint32_t)(x >> 1) * FB_STRIDE + (uint32_t)y_group] |= (uint8_t)(1u << bit);
}

/* ============================================================
 * EPX / Scale2x / AdvMAME2x 1-bit 像素放大算法 (2x)
 * ------------------------------------------------------------
 * 索引编码 (邻域 8bit LUT, 中心 E 固定为白=1 / 黑=0):
 *   bit 0  bit 1  bit 2   对应: A(左上) B(上) C(右上)
 *   bit 3         bit 4         D(左)  E   F(右)
 *   bit 5  bit 6  bit 7         G(左下) H(下) I(右下)
 * 输出 nibble:  bit3=E0(左上)  bit2=E1(右上)  bit1=E2(左下)  bit0=E3(右下)
 * EPX 核心规则:
 *   若 上==下 或 左==右, 则 E0/E1/E2/E3 全部 = E, 即保持实心 (2x2 同色),
 *   否则按 "邻居成对相等" 依次替换对应子像素 (不成对则回退为 E).
 * 这里两套 LUT 预计算: s2x_lut_E1 (中心为白) 与 s2x_lut_E0 (中心为黑)
 *
 * 说明: ESP32-S3 是统一寻址 (Flash 可直接按字节读取), 不再需要 AVR 风格的 PROGMEM,
 *       直接用 const 就会被链接器放到 .rodata (Flash), 不占内部 RAM.
 * ============================================================ */
#define S2X_E(LUT,A,B,C,D,F,G,H,I) \
    LUT[ (uint8_t)(((A)<<0)|((B)<<1)|((C)<<2)|((D)<<3)| \
                   ((F)<<4)|((G)<<5)|((H)<<6)|((I)<<7)) ]

static const uint8_t s2x_lut_E1[256] = {
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E, 0x0E, 0x0F, 0x0F, 0x0E, 0x0E,
    0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E, 0x0E, 0x0F, 0x0F, 0x0E, 0x0E,
    0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F,
    0x07, 0x07, 0x0F, 0x0F, 0x07, 0x07, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F,
    0x07, 0x07, 0x0F, 0x0F, 0x07, 0x07, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E, 0x0E, 0x0F, 0x0F, 0x0E, 0x0E,
    0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E, 0x0E, 0x0F, 0x0F, 0x0E, 0x0E,
    0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0D, 0x0D, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F,
    0x07, 0x07, 0x0F, 0x0F, 0x07, 0x07, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F, 0x0B, 0x0B, 0x0F, 0x0F,
    0x07, 0x07, 0x0F, 0x0F, 0x07, 0x07, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
};

/* s2x_lut_E0 = 中心为黑=0 时的 4-子像素映射.
 * 推导: EPX 规则对颜色对称, 只要把输入与输出同时取反即可.
 * 即 LUT0[idx] = (~LUT1[~idx]) & 0x0F, 这里已预计算. */
static const uint8_t s2x_lut_E0[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x02, 0x02,
    0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x02, 0x02,
    0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x02, 0x02,
    0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x02, 0x02,
    0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 通用 1-bpp MSB-first Scale2x:
 * src:        1-bpp 单色位图 (1=白 0=黑), 每行 src_w/8 字节
 * src_w/src_h:源宽高 (像素, src_w 必须是 8 的倍数; 否则请调用方自行 pad)
 * dst:        输出 1-bpp 位图, 宽高各 2 倍
 * dst_pitch:  输出每行字节数 (像素*2 向上取 8 对齐后再除 8)
 *
 * 边界处理: 超出画面的邻域像素取中心像素同色, 避免产生边缘伪影. */
void scale2x_1bit(const uint8_t *src, int src_w, int src_h,
                  uint8_t *dst, int dst_pitch) {
    (void)src_w; /* 宽度通过 src_w/8 的循环边界隐含使用, 这里防止 unused 警告 */
    uint8_t *out = dst;
    const int src_bpr = src_w / 8;
    for (int y = 0; y < src_h; y++) {
        const int ym = (y > 0)           ? y - 1 : y;
        const int yp = (y < src_h - 1)   ? y + 1 : y;
        const uint8_t *row_m = src + (long)ym * src_bpr;
        const uint8_t *row_0 = src + (long)y  * src_bpr;
        const uint8_t *row_p = src + (long)yp * src_bpr;

        /* 每字节 8 个源像素, 一次处理 8 列, 对应输出 2 行 x 16 像素 = 2 行 x 2 字节 */
        uint8_t d0[2] = {0}, d1[2] = {0}; /* dst 当前两行像素的位缓冲 */
        for (int xb = 0; xb < src_bpr; xb++) {
            uint8_t rmu = row_m[xb], rc = row_0[xb], rdo = row_p[xb];
            /* 左边的 8 列 (xb-1), 越界用当前列代替 (同色) */
            uint8_t lmu = (xb > 0) ? row_m[xb - 1] : rmu;
            uint8_t lc  = (xb > 0) ? row_0[xb - 1] : rc;
            uint8_t ldo = (xb > 0) ? row_p[xb - 1] : rdo;
            /* 右边的 8 列 (xb+1), 越界用当前列代替 (同色) */
            uint8_t rmu2 = (xb < src_bpr - 1) ? row_m[xb + 1] : rmu;
            uint8_t rc2  = (xb < src_bpr - 1) ? row_0[xb + 1] : rc;
            uint8_t rdo2 = (xb < src_bpr - 1) ? row_p[xb + 1] : rdo;

            uint16_t mu16  = ((uint16_t)lmu << 8) | (uint16_t)rmu;
            uint16_t c16   = ((uint16_t)lc  << 8) | (uint16_t)rc;
            uint16_t do16  = ((uint16_t)ldo << 8) | (uint16_t)rdo;
            uint16_t mu16r = ((uint16_t)rmu << 8) | (uint16_t)rmu2;
            uint16_t c16r  = ((uint16_t)rc  << 8) | (uint16_t)rc2;
            uint16_t do16r = ((uint16_t)rdo << 8) | (uint16_t)rdo2;

            uint8_t d0p = 0, d1p = 0; /* 当前 8 列对应的高/低 8 位输出 */
            for (int i = 0; i < 8; i++) {
                const uint8_t A = (mu16  >> (15 - i))       & 1u;
                const uint8_t B = (mu16  >> (15 - (i + 1))) & 1u;
                const uint8_t C = (mu16r >> (15 - (i + 2))) & 1u;
                const uint8_t D = (c16   >> (15 - i))       & 1u;
                const uint8_t E = (c16   >> (15 - (i + 1))) & 1u;
                const uint8_t F = (c16r  >> (15 - (i + 2))) & 1u;
                const uint8_t G = (do16  >> (15 - i))       & 1u;
                const uint8_t H = (do16  >> (15 - (i + 1))) & 1u;
                const uint8_t I = (do16r >> (15 - (i + 2))) & 1u;
                const uint8_t idx = (uint8_t)(
                    (A << 0) | (B << 1) | (C << 2) | (D << 3) |
                    (F << 4) | (G << 5) | (H << 6) | (I << 7)
                );
                const uint8_t pat = E ? s2x_lut_E1[idx] : s2x_lut_E0[idx];
                /* pat bit3=E0(左上) bit2=E1(右上) bit1=E2(左下) bit0=E3(右下)
                 * 写到 MSB 优先的 d0p/d1p: 源第 i 列 (0=MSB 侧) 对应 第 2i,2i+1 位 */
                const int ob = 6 - i * 2;   /* 目标字节内上两子像素 MSB 起始位 */
                d0p |= (uint8_t)(((pat >> 3) & 1u) << (ob + 1));
                d0p |= (uint8_t)(((pat >> 2) & 1u) << (ob + 0));
                d1p |= (uint8_t)(((pat >> 1) & 1u) << (ob + 1));
                d1p |= (uint8_t)(((pat >> 0) & 1u) << (ob + 0));
            }
            /* 写入 2 行 x 2 字节 */
            d0[xb & 1] = d0p;
            d1[xb & 1] = d1p;
            if ((xb & 1) == 1 || xb == src_bpr - 1) {
                const long row0 = (long)(y * 2)     * dst_pitch;
                const long row1 = (long)(y * 2 + 1) * dst_pitch;
                const int base = (xb & ~1);
                if (dst_pitch >= 2) {
                    out[row0 + (base / 2) * 2]     = d0[0];
                    out[row0 + (base / 2) * 2 + 1] = d0[1];
                    out[row1 + (base / 2) * 2]     = d1[0];
                    out[row1 + (base / 2) * 2 + 1] = d1[1];
                } else {
                    out[row0 + (base / 2)] = d0[0];
                    out[row1 + (base / 2)] = d1[0];
                }
                d0[0] = d0[1] = d1[0] = d1[1] = 0;
            }
        }
    }
}

/* EPX 直接把 4-bit pattern (位3=左上, 位2=右上, 位1=左下, 位0=右下, 黑=1 需清除)
 * 按 2x2 写进 ST7305 fb (GAME_X/GAME_Y 起始的居中区域). 前提: fb 已经 memset 0xFF=白. */
static inline void IRAM_ATTR fb_fill_epx_block(uint8_t *fb, int ox, int oy,
                                                uint8_t pattern) {
    for (int yy = 0; yy < 2; yy++) {
        for (int xx = 0; xx < 2; xx++) {
            /* pattern 布局 = [E0 E1; E2 E3], 对应位 3,2,1,0 */
            const uint8_t bitidx = (uint8_t)(3 - (yy * 2 + xx));
            if (pattern & (1u << bitidx))
                fb_clear_pixel(fb, ox + xx, oy + yy);
        }
    }
}

/* === V1.0.56: 覆盖率抗锯齿 (coverage AA) 填充一个源像素的输出块 ===
 * 目的: 让抗锯齿开启/关闭有**可见**差异 (用户反馈之前几乎看不出效果).
 * 原理: 不再"整块实心再抠角"(那种在 2x2 只能去掉 1 个角, 视觉上几乎无差别),
 *       而是按源像素的邻域分类, 决定块内黑点分布, 让斜线/拐角真正变圆滑:
 *   mode 0 (实心): 直线/填充内部 → 整块涂满, 笔画不因抗锯齿变细.
 *   mode 1 (\):    主对角线像素 (lu&&rd) → 只涂主对角线, 形成 1 点宽平滑斜线.
 *   mode 2 (/):    反对角线像素 (ru&&ld) → 只涂反对角线, 形成 1 点宽平滑斜线.
 *   mode 3 (去外角): 拐角/端点 → 涂满 3 个角, 去掉白色一侧的外角, 圆滑拐角.
 *   mode 4 (孤立点): 无任何相邻黑 → 只涂中心一点.
 * cut: mode 3 时指定要去除外角 (0=左上,1=右上,2=左下,3=右下).
 */
static inline void IRAM_ATTR fb_fill_block_aa(uint8_t *fb, int ox, int oy,
                                              int bw, int bh, int mode, int cut) {
    for (int yy = 0; yy < bh; yy++) {
        for (int xx = 0; xx < bw; xx++) {
            bool black;
            switch (mode) {
            case 0:  black = true; break;                       /* 实心 */
            case 1:  black = (xx == yy); break;                 /* 主对角线 \ */
            case 2:  black = (xx + yy == bw - 1); break;        /* 反对角线 / */
            case 3: {  /* 去掉指定外角 */
                bool is_cut = (cut == 0 && xx == 0     && yy == 0)     ||
                              (cut == 1 && xx == bw - 1 && yy == 0)     ||
                              (cut == 2 && xx == 0     && yy == bh - 1) ||
                              (cut == 3 && xx == bw - 1 && yy == bh - 1);
                black = !is_cut;
                break;
            }
            default: black = (xx == bw / 2 && yy == bh / 2); break; /* 孤立点 */
            }
            if (black) fb_clear_pixel(fb, ox + xx, oy + yy);
        }
    }
}

/* 根据源像素 4 邻域 (u/d/l/r) 与 4 对角邻域 (lu/ld/ru/rd) 分类, 返回 AA 模式.
 * 边界: u/d/l/r 越界按黑 (保持边缘直线实心), 对角越界按白.
 * V1.0.57: 斜线/孤立点一律实心 (不删像素, 避免笔画变细);
 * 仅拐角/端点去外角圆滑, 保证"保粗"优先. */
static inline int IRAM_ATTR fb_classify_aa(bool u, bool d, bool l, bool r,
                                           bool lu, bool ld, bool ru, bool rd,
                                           int *cut) {
    (void)lu; (void)ld; (void)ru; (void)rd;   /* 斜线不再用于删像素 */
    if ((u && d) || (l && r)) return 0;        /* 直线/填充内部 → 实心 */
    /* 拐角: 去掉白色一侧的外角 (圆滑, 对粗细影响小) */
    if (u && l)      { *cut = 3; return 3; }    /* 黑上+左 → 去右下角 */
    else if (u && r) { *cut = 2; return 3; }    /* 黑上+右 → 去左下角 */
    else if (d && l) { *cut = 1; return 3; }    /* 黑下+左 → 去右上角 */
    else if (d && r) { *cut = 0; return 3; }    /* 黑下+右 → 去左上角 */
    /* 端点/斜线/孤立点: 一律实心, 保持笔画粗细不变 */
    return 0;
}

static void IRAM_ATTR render_frame(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!g_lcd || !data) return;
    const uint16_t *src = (const uint16_t *)data;
    init_lookup_tables();

    /* === 性能优化: 直接操作 fb, 跳过 st7305_draw_pixel 的函数调用+边界检查 ===
     * ST7305 fb 布局: 每字节 = 2列 x 4行, 共 (400/2)*(300/4)=200*75=15000 字节
     * fb[idx] 的位 = 7 - ((y_sub<<1) | (x&1)), 其中 y_sub = (inv_y & 3)
     * 黑色 = 清除对应位, 白色 = 设置对应位 */

    if (g_display_mode != DISP_MODE_POINT2POINT) {
        /* 全屏(1) / 拉伸(2) 共用此分支: 源画面拉伸到全屏 400 像素宽
         * (160x96 -> 400x240 与源比例一致, 拉伸/全屏渲染归一复用) */
        memset(g_lcd->fb, 0xFF, FB_BYTES);
        uint8_t *fb = g_lcd->fb;

        if (g_pic_opt >= 1) {
            /* === V1.0.58: 全屏覆盖率抗锯齿 (保粗AA / EPX 档都走这个, 因为非 2x 整数倍) ===
             * 每个源像素映射到输出块 (2/3 列 x 2/3 行), 按邻域分类决定块内黑点:
             * 直线实心, 拐角/端点去外角 → 保持笔画不变粗, 拐角圆滑. */
            for (int sy = 0; sy < CORE_H; sy++) {
                int ry0 = g_fs_row_start[sy], ry1 = g_fs_row_end[sy];
                int y0 = FS_GAME_Y + ry0;
                int rowH = ry1 - ry0 + 1;
                const uint16_t *row  = src + sy * (pitch / 2);
                const uint16_t *row_u = (sy > 0)          ? src + (sy - 1) * (pitch / 2) : row;
                const uint16_t *row_d = (sy < CORE_H - 1) ? src + (sy + 1) * (pitch / 2) : row;
                for (int sx = 0; sx < CORE_W; sx++) {
                    if (row[sx] != 0) continue;  /* 跳过白色(0xd6da), 只处理黑色像素 (黑=0x0000) */
                    bool l = (sx > 0)          ? (row[sx - 1] == 0) : true;
                    bool r = (sx < CORE_W - 1) ? (row[sx + 1] == 0) : true;
                    bool u = (row_u[sx] == 0);
                    bool d = (row_d[sx] == 0);
                    bool lu = (sx > 0 && sy > 0)          ? (row_u[sx - 1] == 0) : false;
                    bool ld = (sx > 0 && sy < CORE_H - 1) ? (row_d[sx - 1] == 0) : false;
                    bool ru = (sx < CORE_W - 1 && sy > 0) ? (row_u[sx + 1] == 0) : false;
                    bool rd = (sx < CORE_W - 1 && sy < CORE_H - 1) ? (row_d[sx + 1] == 0) : false;
                    int cx0 = g_fs_col_start[sx];   /* 全屏横向铺满, 从 x=0 开始 */
                    int colW = g_fs_col_end[sx] - g_fs_col_start[sx] + 1;
                    int cut = 3;
                    int mode = fb_classify_aa(u, d, l, r, lu, ld, ru, rd, &cut);
                    fb_fill_block_aa(fb, cx0, y0, colW, rowH, mode, cut);
                }
            }
        } else {
            /* 纯最近邻放大 (关抗锯齿): 原逻辑 */
            for (int dy = 0; dy < FS_SCALED_H; dy++) {
                int sy = g_fs_row_map[dy];
                const uint16_t *row = src + sy * (pitch / 2);
                int y = FS_GAME_Y + dy;
                int inv_y = ST7305_HEIGHT - 1 - y;
                int y_group = inv_y >> 2;
                int y_sub = inv_y & 3;
                uint8_t *fb_row = fb + (uint32_t)y_group;
                for (int dx = 0; dx < FS_SCALED_W; dx += 2) {
                    int sx0 = g_fs_col_map[dx];
                    int sx1 = g_fs_col_map[dx + 1];
                    uint8_t bits = 0;
                    if (row[sx0] == 0) bits |= (uint8_t)(1u << (7u - (y_sub << 1)));
                    if (row[sx1] == 0) bits |= (uint8_t)(1u << (7u - ((y_sub << 1) | 1)));
                    if (bits != 0) {
                        int x_pair = dx >> 1;
                        fb_row[(uint32_t)x_pair * FB_STRIDE] &= ~bits;
                    }
                }
            }
        }

        if (g_menu.settings.game_status_bar) {
            menu_settings_t fake_settings = {0};
            fake_settings.battery = g_status_battery;
            fake_settings.pad_connected = g_status_pad_connected;
            menu_draw_status_bar(g_lcd, &fake_settings, NULL);
        }

        /* V1.0.68+: 虚拟按键启用时不画底部装饰栏与游戏名称
         * (会与底部 Select/Start 按键区域重叠碍眼) */
        if (!virtual_keys_is_enabled()) {
            int bottom_y = FS_GAME_Y + FS_SCALED_H;
            /* 左侧装饰三角形 - 直接写 fb 跳过 st7305_draw_pixel */
            {
                uint8_t *fb = g_lcd->fb;
                /* FB_STRIDE = 75, compile time */
                for (int k = 0; k < 5; k++) {
                    int start_x = 64 + k * 12;
                    for (int i = 0; i < 60; i++) {
                        int px = start_x - i;
                        int py = bottom_y + k * 4 + i % 8;
                        if (py >= bottom_y + 24 || px < 0) continue;
                        int inv_y = ST7305_HEIGHT - 1 - py;
                        int y_group = inv_y >> 2;
                        int y_sub = inv_y & 3;
                        uint32_t idx = (uint32_t)(px >> 1) * FB_STRIDE + (uint32_t)y_group;
                        uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (px & 1));
                        fb[idx] &= ~(uint8_t)(1u << bit);
                    }
                }
            }
            /* 右侧装饰三角形 - 直接写 fb */
            {
                uint8_t *fb = g_lcd->fb;
                /* FB_STRIDE = 75, compile time */
                for (int k = 0; k < 5; k++) {
                    int start_x = ST7305_WIDTH - 64 - k * 12;
                    for (int i = 0; i < 60; i++) {
                        int px = start_x + i;
                        int py = bottom_y + k * 4 + i % 8;
                        if (py >= bottom_y + 24 || px >= ST7305_WIDTH) continue;
                        int inv_y = ST7305_HEIGHT - 1 - py;
                        int y_group = inv_y >> 2;
                        int y_sub = inv_y & 3;
                        uint32_t idx = (uint32_t)(px >> 1) * FB_STRIDE + (uint32_t)y_group;
                        uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (px & 1));
                        fb[idx] &= ~(uint8_t)(1u << bit);
                    }
                }
            }
            {
                const char *name = g_rom_path;
                const char *p = strrchr(g_rom_path, '/');
                if (p) name = p + 1;
                char display_name[64];
                strncpy(display_name, name, sizeof(display_name) - 1);
                display_name[sizeof(display_name) - 1] = 0;
                char *dot = strrchr(display_name, '.');
                if (dot) *dot = 0;
                int text_w = calc_game_name_width(display_name);
                int text_x = (ST7305_WIDTH - text_w) / 2;
                draw_game_name(g_lcd, text_x, bottom_y + 6, display_name);
            }
        }
        virtual_keys_draw(g_lcd);   /* V1.0.68 fix: 补画屏幕虚拟按键 */
        st7305_flush(g_lcd);
    } else {
        /* 点对点模式: 清空整个 fb 为白色 */
        memset(g_lcd->fb, 0xFF, FB_BYTES);

        /* 性能优化: 直接写 fb, 跳过 st7305_draw_pixel
         * 160x96 -> 320x192 (2x缩放), 每个源像素映射到 2x2 目标块
         * 直接计算 fb 索引, 避免函数调用+边界检查开销 (快 3-5x) */
        /* fb_stride = FB_STRIDE = 75 (compile time) */
        int src_stride = pitch / 2;
        if (g_pic_opt == 1) {
            /* === V1.0.58: EPX (Scale2x / AdvMAME2x) 边缘平滑模式 ===
             * 按中心像素颜色取 8 邻域 LUT 得到 2x2 子像素分布.
             * 像素风格保留硬朗, 斜线/拐角 自动圆滑, 比 coverage AA 更正统像素风. */
            for (int y = 0; y < CORE_H; y++) {
                const uint16_t *row  = src + y * src_stride;
                const uint16_t *row_u = (y > 0)          ? src + (y - 1) * src_stride : row;
                const uint16_t *row_d = (y < CORE_H - 1) ? src + (y + 1) * src_stride : row;
                for (int x = 0; x < CORE_W; x++) {
                    uint16_t e = row[x];
                    int xm = (x > 0) ? x - 1 : x;
                    int xp = (x < CORE_W - 1) ? x + 1 : x;
                    /* 取各邻域 (越界退化为中心像素, 避免边缘伪影) */
                    uint8_t E_bit = (e == 0) ? 1 : 0;  /* 0=白 1=黑, 与 LUT 对应 */
                    uint8_t A = ((row_u[xm] == 0) ? 1 : 0);
                    uint8_t B = ((row_u[x]  == 0) ? 1 : 0);
                    uint8_t C = ((row_u[xp] == 0) ? 1 : 0);
                    uint8_t D = ((row[xm]    == 0) ? 1 : 0);
                    uint8_t F = ((row[xp]    == 0) ? 1 : 0);
                    uint8_t G = ((row_d[xm]  == 0) ? 1 : 0);
                    uint8_t H = ((row_d[x]   == 0) ? 1 : 0);
                    uint8_t I = ((row_d[xp]  == 0) ? 1 : 0);
                    const uint8_t idx = (uint8_t)(
                        (A << 0) | (B << 1) | (C << 2) | (D << 3) |
                        (F << 4) | (G << 5) | (H << 6) | (I << 7)
                    );
                    /* EPX LUT 白/黑对称切换: 中心黑=E_bit=1 → 用 E0 表; 中心白 → 用 E1 表.
                     * 注意 s2x_lut_Ex 的 1=白 0=黑, 我们反色后再写 fb: fb_clear_pixel 需要黑=1. */
                    const uint8_t raw = E_bit ? s2x_lut_E0[idx] : s2x_lut_E1[idx];
                    /* 把 raw(1=白 0=黑) 按 EPX 黑白对称反转成黑=1 写入 fb */
                    const uint8_t pat = (uint8_t)((~raw) & 0x0F);
                    int tx = GAME_X + x * SCALE;
                    int ty = GAME_Y + y * SCALE;
                    fb_fill_epx_block(g_lcd->fb, tx, ty, pat);
                    (void)E_bit;
                }
            }
        } else {
            /* 纯 2x 最近邻放大 (关抗锯齿) */
            for (int y = 0; y < CORE_H; y++) {
                const uint16_t *row = src + y * src_stride;
                for (int x = 0; x < CORE_W; x++) {
                    if (row[x] != 0) continue;
                    int tx = GAME_X + x * SCALE;
                    int ty = GAME_Y + y * SCALE;
                    /* 2x2 实心: 直接按 y_sub 组合位 */
                    for (int py = 0; py < SCALE; py++) {
                        int tty = ty + py;
                        int inv_y = ST7305_HEIGHT - 1 - tty;
                        int y_group = inv_y >> 2;
                        int y_sub = inv_y & 3;
                        uint8_t *fb_row = g_lcd->fb + (uint32_t)y_group;
                        for (int px = 0; px < SCALE; px++) {
                            int txx = tx + px;
                            int x_pair = txx >> 1;
                            uint8_t bit = 7u - (uint8_t)((y_sub << 1) | (txx & 1));
                            fb_row[(uint32_t)x_pair * FB_STRIDE] &= ~(uint8_t)(1u << bit);
                        }
                    }
                }
            }
        }
        if (g_menu.settings.game_status_bar) {
            menu_settings_t fake_settings = {0};
            fake_settings.battery = g_status_battery;
            fake_settings.pad_connected = g_status_pad_connected;
            menu_draw_status_bar(g_lcd, &fake_settings, NULL);
        }
        virtual_keys_draw(g_lcd);   /* V1.0.68 fix: 补画屏幕虚拟按键 */
        st7305_flush(g_lcd);
    }
}

/* === libretro 视频回调: 在这里把核心输出到 ST7305 === */
static void IRAM_ATTR video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (g_running) {
        uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
        render_frame(data, w, h, pitch);
        uint32_t t1 = xTaskGetTickCount() * portTICK_PERIOD_MS;
        static uint32_t last_log_ms = 0;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log_ms >= 1000) {
            ESP_LOGI(TAG, "Render time: %lu ms", (unsigned long)(t1 - t0));
            last_log_ms = now;
        }
    }
}

/* === libretro 音频回调 === */
/* V1.0.55: 恢复步步高 4980 定时器方波音效 (按键/游戏蜂鸣滴声).
 * 之前因 audio_out 任务栈溢出关闭, 现在该任务栈已加大到 24KB;
 * 这里改成按帧批量喂送 (735 样本/帧一次), 避免逐样本 feed_pcm 的互斥开销. */
#define GAM4980_ENABLE_AUDIO 1
#define GAM4980_FRAME_SAMPLES 735   /* 44100/60 */
static int g_audio_sample_rate = 44100;
EXT_RAM_BSS_ATTR static int16_t s_bbk_audio_buf[GAM4980_FRAME_SAMPLES * 2];
static int s_bbk_audio_cnt = 0;

static void audio_cb(int16_t left, int16_t right) {
#if GAM4980_ENABLE_AUDIO
    s_bbk_audio_buf[s_bbk_audio_cnt * 2] = left;
    s_bbk_audio_buf[s_bbk_audio_cnt * 2 + 1] = right;
    if (s_bbk_audio_cnt < GAM4980_FRAME_SAMPLES) s_bbk_audio_cnt++;
    /* 不在这里喂送: 每帧由 bbk_audio_frame_finalize() 混入按键音后统一喂一次,
     * 避免多次喂送导致环形缓冲溢出丢片段 (按键音断续/连滴的根因). */
#else
    (void)left; (void)right;
#endif
}
static size_t audio_batch_cb(const int16_t *data, size_t frames) {
#if GAM4980_ENABLE_AUDIO
    return audio_player_feed_pcm(data, frames, g_audio_sample_rate);
#else
    (void)data;
    return frames;   /* 全部消费, 核心不阻塞 */
#endif
}

/* === libretro 输入回调 === */
/* 游戏内按键映射:
 *   物理键: KEY(GPIO18)=A(确认), BOOT(GPIO0)=B(退出)
 *   手柄:   通过 bt_manager 的按键映射读取 (上/下/左/右/A/B/X/Y/L/R/返回主页)
 *   手柄映射由用户在"按键映射"流程中捕获, 适配任意手柄的 HID 报告布局 */
static void input_poll_cb(void) {}

/* 返回某 RetroPad 键状态: 手柄映射键 OR 对应物理键(后备) */
static int16_t joypad_state(unsigned id) {
    bool gp = false;
    bool phys = false;
    /* V1.0.68 fix: 屏幕虚拟按键状态 (bit 清 0 = 按下), 与手柄/物理键并联合入 */
    uint8_t vk = virtual_keys_is_enabled() ? virtual_keys_get_joypad() : 0xFF;
    /* V1.0.xx: 网页手柄 (WiFi AP) 掩码也与 GB joypad 一致 (bit 清 0 = 按下):
     * bit0=A bit1=B bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下.
     * 开启 WiFi 手柄时蓝牙会关闭, 为避免引擎内再按没反应, 并入各键判定. */
    uint8_t wg = web_gamepad_is_running() ? web_gamepad_get_joypad_state() : 0xFF;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_A:
            gp = bt_manager_is_key_pressed(F_CONFIRM) || !(vk & 0x01) || !(wg & 0x01);
            phys = input_is_held(0);
            break;
        case RETRO_DEVICE_ID_JOYPAD_B:
            gp = bt_manager_is_key_pressed(F_BACK) || !(vk & 0x02) || !(wg & 0x02);
            phys = input_is_held(1);
            break;
        case RETRO_DEVICE_ID_JOYPAD_UP:    gp = bt_manager_is_key_pressed(F_UP)    || !(vk & 0x40) || !(wg & 0x40); break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:  gp = bt_manager_is_key_pressed(F_DOWN)  || !(vk & 0x80) || !(wg & 0x80); break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:  gp = bt_manager_is_key_pressed(F_LEFT)  || !(vk & 0x20) || !(wg & 0x20); break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: gp = bt_manager_is_key_pressed(F_RIGHT) || !(vk & 0x10) || !(wg & 0x10); break;
        /* V1.0.38: 8 功能键只有 4 个, X/Y/L/R/SELECT/START 全部映射到 F_FAV (多功能键).
         * 屏幕虚拟按键的 Select/Start 也归到 F_FAV. */
        case RETRO_DEVICE_ID_JOYPAD_X:
        case RETRO_DEVICE_ID_JOYPAD_Y:
        case RETRO_DEVICE_ID_JOYPAD_L:
        case RETRO_DEVICE_ID_JOYPAD_R:
        case RETRO_DEVICE_ID_JOYPAD_SELECT:
        case RETRO_DEVICE_ID_JOYPAD_START:
            gp = bt_manager_is_key_pressed(F_FAV) || !(vk & 0x04) || !(vk & 0x08)
                 || !(wg & 0x04) || !(wg & 0x08);   /* 网页 Select/Start */
            break;
        /* V1.0.46: 补充按键 (F/G/Shift/空格 → BBK 功能1-4)
         * 复用 RetroPad 扩展 ID 16..19, 由 libretro 的 _joyk 映射到对应 BBK 键. */
        case 16: gp = bt_manager_is_sup_pressed(0); break;  /* 功能1(F) */
        case 17: gp = bt_manager_is_sup_pressed(1); break;  /* 功能2(G) */
        case 18: gp = bt_manager_is_sup_pressed(2); break;  /* 功能3(Shift) */
        case 19: gp = bt_manager_is_sup_pressed(3); break;  /* 功能4(空格) */
        default: break;
    }
    return (gp || phys) ? 1 : 0;
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port; (void)device; (void)index;
    return joypad_state(id);
}

/* === BBK 按键音效: 660Hz 方波, 按下起音/长按连续/松开断音 === */
static volatile bool s_key_sound_enabled = true;
static bool s_tone_active = false;
static uint32_t s_tone_start_ms = 0;    /* 本次按下开始时刻 */
static uint32_t s_tone_release_ms = 0;  /* 上次释放时刻 (防抖) */

#define BBK_KEY_TONE_FREQ   660
#define BBK_KEY_TONE_AMP    5000
#define BBK_KEY_SR          44100
#define BBK_KEY_MIN_MS      60      /* 短按最小音长: 释放后仍响到 60ms */
#define BBK_KEY_LONG_MS     200     /* 按住超过 200ms 视为长按, 松开时立即断音 */
#define BBK_KEY_DEBOUNCE_MS 60      /* 释放后 60ms 内不重新触发, 防连滴 */
static uint32_t s_tone_phase = 0;
static int64_t s_last_audio_us = 0;   /* 音频按真实时间喂送的时间基准 */
static int64_t s_audio_acc = 0;       /* 亚样本累加器: 平均每帧多出的零点几个样本,
                                       * 保证产量恒等于消耗, 长按音不会缓慢见底 */

static bool bbk_any_key_pressed(void)
{
    static const unsigned ids[] = {
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        16, 17, 18, 19
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (joypad_state(ids[i])) return true;
    }
    return false;
}

/* 每帧最后: 把按键音混入已累加的游戏音频, 统一喂一次 (避免多次喂送溢出丢片段) */
static void bbk_audio_frame_finalize(void)
{
#if GAM4980_ENABLE_AUDIO
    /* 按真实经过时间决定本次喂送样本数: want = 44100 × dt.
     * 这样无论游戏循环是 59 还是 60fps, 产量恒等于扬声器消耗,
     * 环形缓冲水位稳定, 长按音不会因缓冲见底出现空隙. */
    int64_t now = esp_timer_get_time();
    if (s_last_audio_us == 0) s_last_audio_us = now;
    int64_t dt_us = now - s_last_audio_us;
    s_last_audio_us = now;
    if (dt_us < 1000) dt_us = 1000;
    if (dt_us > 100000) dt_us = 100000;
    s_audio_acc += (int64_t)BBK_KEY_SR * dt_us;
    int want = (int)(s_audio_acc / 1000000);
    s_audio_acc %= 1000000;
    if (want < 256) want = 256;
    if (want > 2048) want = 2048;

    int game_frames = s_bbk_audio_cnt;
    if (game_frames > want) game_frames = want;
    static EXT_RAM_BSS_ATTR int16_t s_frame_buf[2048 * 2];
    for (int i = 0; i < game_frames; i++) {
        s_frame_buf[i * 2] = s_bbk_audio_buf[i * 2];
        s_frame_buf[i * 2 + 1] = s_bbk_audio_buf[i * 2 + 1];
    }
    for (int i = game_frames; i < want; i++) {
        s_frame_buf[i * 2] = 0;
        s_frame_buf[i * 2 + 1] = 0;
    }

    int half = BBK_KEY_SR / (BBK_KEY_TONE_FREQ * 2);
    if (s_tone_active) {
        for (int i = 0; i < want; i++) {
            int16_t v = ((s_tone_phase % (half * 2)) < half)
                        ? BBK_KEY_TONE_AMP : -BBK_KEY_TONE_AMP;
            s_tone_phase++;
            int32_t m = (int32_t)s_frame_buf[i * 2] + v;
            if (m > 32767) m = 32767;
            if (m < -32768) m = -32768;
            s_frame_buf[i * 2] = (int16_t)m;
            s_frame_buf[i * 2 + 1] = (int16_t)m;
        }
    }
    audio_player_feed_pcm(s_frame_buf, (size_t)want, BBK_KEY_SR);
    s_bbk_audio_cnt = 0;
#endif
}

/* 每帧调用 (retro_run 之后): 按键状态机 */
static void bbk_key_sound_tick(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (!s_key_sound_enabled) {
        if (s_tone_active) { s_tone_active = false; audio_player_flush_pcm(); }
        s_tone_release_ms = now;
        return;
    }
    bool any = bbk_any_key_pressed();
    if (any) {
        if (!s_tone_active && (uint32_t)(now - s_tone_release_ms) >= BBK_KEY_DEBOUNCE_MS) {
            s_tone_active = true;
            s_tone_start_ms = now;
        }
        /* 按住: 持续音由 finalize 每帧混入, 连续无空隙 */
    } else {
        s_tone_release_ms = now;
        if (s_tone_active) {
            uint32_t held = (uint32_t)(now - s_tone_start_ms);
            if (held >= BBK_KEY_LONG_MS) {
                /* 长按松开: 立即断音 */
                s_tone_active = false;
                audio_player_flush_pcm();
            } else if (held >= BBK_KEY_MIN_MS) {
                /* 短按: 自然播放完最小音长, 不 flush (避免清空后静音延迟) */
                s_tone_active = false;
            }
            /* held < MIN: 继续响到最小音长 */
        }
    }
}

void gam4980_set_key_sound(bool on)
{
    s_key_sound_enabled = on;
    if (!on) {
        s_tone_active = false;
        audio_player_flush_pcm();
    }
}

/* === 核心帧率回调 (用于RTC计时) === */
static retro_frame_time_callback_t g_core_frame_cb = NULL;
static retro_usec_t g_frame_reference = 16667; /* 1000000/60 */

/* === libretro 环境回调 === */
static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback *cb = (struct retro_log_callback *)data;
            cb->log = fallback_log;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            static const char *sysdir = "/sdcard/system";
            *(const char **)data = sysdir;
            ESP_LOGI(TAG, "libretro 询问 system 目录 -> %s", sysdir);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
            struct retro_frame_time_callback *frame = (struct retro_frame_time_callback *)data;
            g_core_frame_cb = frame->callback;
            g_frame_reference = frame->reference;
            ESP_LOGI(TAG, "核心帧率回调已保存, reference=%lu us", (unsigned long)g_frame_reference);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            /* core 列出可用变量 (含 ghosting=1, lcd_color=grey).
             * 这样 apply_variables 会调 GET_VARIABLE 拿默认值.
             * 我们在 GET_VARIABLE 提供"关闭 ghosting"的稳定值. */
            static const struct retro_variable vars[] = {
                { "gam4980_lcd_ghosting",                  "Ghosting;0|1|2|5|10|20" },
                { "gam4980_lcd_color",                     "Color;grey|green|blue|yellow|random" },
                { "gam4980_cpu_rate",                      "CPU Rate;1.0" },
                { "gam4980_timer_rate",                    "Timer Rate;1.0" },
                { "gam4980_key_pressed_input_min_interval","Key Min Interval (ms);0" },
                { NULL, NULL },
            };
            *(const struct retro_variable **)data = (void *)vars;
            ESP_LOGI(TAG, "core 询问 SET_VARIABLES, 已提供默认值 (ghosting=1)");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            if (strcmp(var->key, "gam4980_lcd_ghosting") == 0) {
                var->value = "0";  /* 完全关闭余辉, 否则 blend_frame 会覆盖画面 */
            } else if (strcmp(var->key, "gam4980_lcd_color") == 0) {
                var->value = "grey";  /* 灰白色 (lcd_bg=0xd6da, lcd_fg=0x0000) */
            } else if (strcmp(var->key, "gam4980_cpu_rate") == 0) {
        /* V1.0.69: 恢复 8.0 (与昨天版本一致, 用户习惯的"八倍速"手感):
         * cpu_rate 同时放大 tstep 与每帧周期数, 每帧模拟 8 倍工作量,
         * 游戏逻辑 8 倍速 (画面 ~15fps 但动作飞快). 1.0 = 真实 4980 速度. */
        var->value = "8.0";
            } else if (strcmp(var->key, "gam4980_timer_rate") == 0) {
                var->value = "1.0";
            } else if (strcmp(var->key, "gam4980_key_pressed_input_min_interval") == 0) {
                var->value = "0";
            } else {
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}



/* === 后台初始化: 系统启动时在菜单后台自动加载引擎 === */
/* 后台进度回调: 只更新变量, 不绘制屏幕 (菜单在渲染) */
static void bg_progress_cb(int percent, const char *msg) {
    g_bg_progress = percent;
    (void)msg;
}

static void background_init_task(void *arg) {
    ESP_LOGI(TAG, "后台引擎初始化开始...");
    g_init_in_progress = true;
    g_bg_active = true;
    g_bg_progress = 0;
    gam4980_set_boot_progress_cb(bg_progress_cb);
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
    g_core_initialized = true;
    g_init_in_progress = false;
    g_bg_progress = 100;
    g_bg_active = false;
    gam4980_set_boot_progress_cb(NULL);
    ESP_LOGI(TAG, "后台引擎初始化完成!");
    ESP_LOGI(TAG, "[MEM] 引擎加载后: 内部空闲=%u 内部最大块=%u PSRAM空闲=%u PSRAM最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    vTaskDelete(NULL);
}

void gam4980_emu_background_init(st7305_handle_t *lcd) {
    if (g_core_initialized || g_init_in_progress) {
        ESP_LOGI(TAG, "background_init 跳过: core=%d init=%d", g_core_initialized, g_init_in_progress);
        return;
    }
    g_lcd = lcd;
    /* 动态分配任务栈 (一次性任务), 减小到 8KB 避免占用过多内部 RAM.
     * V1.0.46: pin 到 CPU1 + 优先级降到 1, 避免抢占 CPU0 上的菜单 UI 渲染.
     * 之前 (优先级 5, CPU0) 与菜单循环同核且更高优先, OS 启动循环 (~2s)
     * 会冻结菜单界面 → 电子词典打开要等 2 秒才显示. */
    /* ⚠️ 此任务必须用内部栈: sys_init 通过 esp_partition_read 读取 8.BIN/E.BIN
     * (flash 操作期间 cache 禁用, PSRAM 栈会触发断言崩溃, V1.0.53 实测).
     * 仅在进入电子词典菜单时短暂存在, 初始化完成后自删. */
    BaseType_t ret = xTaskCreatePinnedToCore(background_init_task, "emu_init",
                                              GAM4980_BG_INIT_STACK_SIZE, NULL, 1, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "background_init 任务创建失败: 内部空闲=%u 块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    ESP_LOGI(TAG, "background_init 任务已启动 (栈%u bytes, 内部空闲=%u)",
             (unsigned)GAM4980_BG_INIT_STACK_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool gam4980_emu_is_bg_active(void) { return g_bg_active; }
int  gam4980_emu_get_bg_progress(void) { return g_bg_progress; }

bool gam4980_emu_is_ready(void) {
    return g_core_initialized;
}

/* === 公共 API === */
/* === 游戏加载进度条 (中文单行, 增量更新) === */
static int g_last_progress_pct = -1;
static char g_last_progress_msg[32] = {0};
static uint32_t g_last_flush_ms = 0;

static void draw_progress_zh(st7305_handle_t *lcd, int x, int y, const char *str) {
    int cx = x;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            /* ASCII: 用 st7305_draw_text 逐字符 */
            char tmp[2] = {c, 0};
            st7305_draw_text(lcd, cx, y, tmp);
            cx += 12;
            str++;
        } else if ((c & 0xF0) == 0xE0) {
            /* 中文 3-byte UTF-8 */
            int idx = font_zh_find_utf8(str);
            if (idx >= 0) {
                const uint8_t *bmp = zh_font_data[idx];
                for (int row = 0; row < 24; row++) {
                    for (int col = 0; col < 24; col++) {
                        int byte_idx = row * 3 + (col / 8);
                        int bit = 7 - (col % 8);
                        if (bmp[byte_idx] & (1 << bit)) {
                            st7305_draw_pixel(lcd, cx + col, y + row, ST7305_COLOR_BLACK);
                        }
                    }
                }
            }
            cx += 24;
            str += 3;
        } else {
            str++;
        }
    }
}

/* 前向声明 */
static void boot_progress_cb(int percent, const char *msg);

/* 映射回调: 将 libretro 内部 0-100% 映射到 85-95% */
static void boot_progress_cb_mapped(int percent, const char *msg) {
    (void)msg;
    int mapped = 85 + percent * 10 / 100;
    if (mapped < 85) mapped = 85;
    if (mapped > 95) mapped = 95;
    boot_progress_cb(mapped, "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe5\xbc\x95\xe6\x93\x8e"); /* 初始化引擎 */
}

static void boot_progress_cb(int percent, const char *msg) {
    if (!g_lcd) return;

    /* 增量更新: 只在百分比或消息变化时重绘 */
    bool pct_changed = (percent != g_last_progress_pct);
    bool msg_changed = (msg && strcmp(msg, g_last_progress_msg) != 0);

    if (!pct_changed && !msg_changed) return;

    /* 首次或消息变化时全屏重绘 */
    if (g_last_progress_pct < 0 || msg_changed) {
        st7305_clear(g_lcd, ST7305_COLOR_WHITE);

        /* 进度条框: 300x16, 居中 */
        int bar_x = 50, bar_y = 140, bar_w = 300, bar_h = 16;
        for (int y = 0; y < bar_h; y++) {
            st7305_draw_pixel(g_lcd, bar_x, bar_y + y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(g_lcd, bar_x + bar_w - 1, bar_y + y, ST7305_COLOR_BLACK);
        }
        for (int x = 0; x < bar_w; x++) {
            st7305_draw_pixel(g_lcd, bar_x + x, bar_y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(g_lcd, bar_x + x, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
        }

        /* 中文消息 (单行, 在进度条上方) */
        if (msg) {
            int msg_w = 0;
            for (const char *p = msg; *p; ) {
                if ((uint8_t)*p >= 0x80) { msg_w += 24; p += 3; }
                else { msg_w += 12; p++; }
            }
            int msg_x = (400 - msg_w) / 2;
            if (msg_x < 0) msg_x = 0;
            draw_progress_zh(g_lcd, msg_x, 105, msg);
        }

        g_last_progress_pct = -1; /* 强制重绘进度条 */
    }

    /* 增量更新进度条填充 */
    int bar_x = 50, bar_y = 140, bar_w = 300, bar_h = 16;
    int old_fill = (g_last_progress_pct >= 0) ? (bar_w - 4) * g_last_progress_pct / 100 : 0;
    int new_fill = (bar_w - 4) * percent / 100;

    if (new_fill > old_fill) {
        for (int y = 2; y < bar_h - 2; y++) {
            for (int x = old_fill; x < new_fill; x++) {
                st7305_draw_pixel(g_lcd, bar_x + 2 + x, bar_y + y, ST7305_COLOR_BLACK);
            }
        }
    }

    /* 百分比文字 */
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    int pct_w = strlen(pct) * 12;
    /* 清除旧百分比 */
    for (int dy = 0; dy < 14; dy++) {
        for (int dx = 0; dx < 40; dx++) {
            st7305_draw_pixel(g_lcd, 180 + dx, 165 + dy, ST7305_COLOR_WHITE);
        }
    }
    st7305_draw_text(g_lcd, (400 - pct_w) / 2, 165, pct);

    /* 限制 flush 频率: 每 80ms 最多一次, 100% 时总是 flush */
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (percent >= 100 || (now_ms - g_last_flush_ms) >= 80) {
        st7305_flush(g_lcd);
        g_last_flush_ms = now_ms;
    }

    g_last_progress_pct = percent;
    if (msg) strncpy(g_last_progress_msg, msg, sizeof(g_last_progress_msg) - 1);
}

esp_err_t gam4980_emu_init(st7305_handle_t *lcd) {
    g_lcd = lcd;
    /* 如果后台初始化正在进行, 显示进度条等待完成 */
    if (g_init_in_progress) {
        ESP_LOGI(TAG, "等待后台引擎初始化完成...");
        int wait_pct = 50;
        while (g_init_in_progress) {
            boot_progress_cb(wait_pct, "Loading engine");
            if (wait_pct < 95) wait_pct++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    /* 如果还未初始化 (后台任务未启动), 现在前台初始化 */
    if (!g_core_initialized) {
        gam4980_set_boot_progress_cb(boot_progress_cb);
        retro_set_environment(env_cb);
        retro_set_video_refresh(video_cb);
        retro_set_audio_sample(audio_cb);
        retro_set_audio_sample_batch(audio_batch_cb);
        retro_set_input_poll(input_poll_cb);
        retro_set_input_state(input_state_cb);
        retro_init();
        g_core_initialized = true;
        gam4980_set_boot_progress_cb(NULL);
        ESP_LOGI(TAG, "libretro 核心初始化完成 (前台)");
    } else {
        ESP_LOGI(TAG, "libretro 核心已就绪 (后台初始化完成)");
    }
    return ESP_OK;
}

int gam4980_emu_load(const char *path) {
    if (!path) return -1;
    /* 重置进度状态 */
    g_last_progress_pct = -1;
    g_last_progress_msg[0] = 0;
    if (g_lcd) boot_progress_cb(5, "\xe5\x87\x86\xe5\xa4\x87\xe4\xb8\xad"); /* 准备中 */

    gam4980_set_boot_progress_cb(boot_progress_cb);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "文件大小异常: %ld", sz);
        fclose(f);
        return -1;
    }

    /* V1.0.52: 不复用大缓冲分配 (1MB 会超过剩余 PSRAM 最大连续块),
     * 直接复用 sys_flash 目标区, 由 fread 流式读入. */
    uint8_t *buf = gam4980_retro_rom_target();
    if (!buf) {
        ESP_LOGE(TAG, "目标区不可用");
        fclose(f);
        return -1;
    }

    /* 读取文件: 10-80% (文件读取是主要耗时部分) */
    if (g_lcd) boot_progress_cb(10, "\xe8\xaf\xbb\xe5\x8f\x96\xe6\x96\x87\xe4\xbb\xb6"); /* 读取文件 */
    size_t total_read = 0;
    const size_t chunk = 0x10000; /* 64KB */
    int last_update_pct = 10;
    while (total_read < (size_t)sz) {
        size_t to_read = (sz - total_read < chunk) ? (sz - total_read) : chunk;
        size_t n = fread(buf + total_read, 1, to_read, f);
        if (n != to_read) break;
        total_read += n;
        int pct = 10 + (int)(total_read * 70 / sz);
        if (g_lcd && pct > last_update_pct + 3) {
            boot_progress_cb(pct, "\xe8\xaf\xbb\xe5\x8f\x96\xe6\x96\x87\xe4\xbb\xb6"); /* 读取文件 */
            last_update_pct = pct;
        }
    }
    fclose(f);

    if (total_read != (size_t)sz) {
        ESP_LOGE(TAG, "读取不完整: %u/%ld", (unsigned)total_read, sz);
        return -1;
    }
    ESP_LOGI(TAG, "读取 .gam: %s (%ld 字节)", path, sz);

    if (g_lcd) boot_progress_cb(82, "\xe5\x8a\xa0\xe8\xbd\xbd\xe6\xb8\xb8\xe6\x88\x8f"); /* 加载游戏 */

    struct retro_game_info game = {
        .path = path,
        .data = buf,
        .size = (size_t)sz,
        .meta = "",
    };

    /* 设置映射回调: 将 libretro 内部 0-100% 映射到 85-95% */
    gam4980_set_boot_progress_cb(boot_progress_cb_mapped);
    if (g_lcd) boot_progress_cb(85, "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe5\xbc\x95\xe6\x93\x8e"); /* 初始化引擎 */
    if (!retro_load_game(&game)) {
        ESP_LOGE(TAG, "加载失败: %s", path);
        gam4980_set_boot_progress_cb(NULL);
        return -1;
    }

    /* 恢复正常回调 */
    gam4980_set_boot_progress_cb(boot_progress_cb);
    if (g_lcd) boot_progress_cb(96, "\xe5\x87\x86\xe5\xa4\x87\xe5\xb0\xb1\xe7\xbb\xaa"); /* 准备就绪 */

    /* V1.0.53 诊断: 0=禁用存档恢复, 排查"到选择存档界面就闪退"是否由存档恢复引起.
     * 确认原因后恢复为 1 (保存功能始终开启, 只影响进入时是否恢复). */
#define GAM4980_RESTORE_SAVE 1
#if GAM4980_RESTORE_SAVE
    /* 加载存档 (必须在 retro_load_game 之后, 此时 sys_flash[0x8000..0x1F7FFF] 已被
     * sys_load 写入游戏数据, 我们只覆盖存档区, 不会破坏游戏). */
    gam4980_emu_load_state(path);
#else
    ESP_LOGI(TAG, "load_state: 存档恢复已禁用 (诊断模式)");
#endif

    if (g_lcd) boot_progress_cb(100, "\xe5\xae\x8c\xe6\x88\x90"); /* 完成 */

    gam4980_set_boot_progress_cb(NULL);
    strncpy(g_rom_path, path, sizeof(g_rom_path) - 1);
    ESP_LOGI(TAG, "加载成功: %s", path);
    return 0;
}

/* 退出确认弹窗: 冻结游戏画面, 叠加"收藏提示"同款小弹窗.
 * 返回: GAME_EXIT_CONFIRMED=确认退出, GAME_EXIT_CANCEL=取消(恢复游戏继续),
 *       GAME_EXIT_TIMEOUT=无操作超时退出.
 * 交互: CONFIRM=退出, BACK/HOME=取消, 手柄 F_BACK 长按=取消,
 *       无按键 10s 超时=自动退出到桌面. */
static game_exit_result_t gam4980_exit_confirm_dialog(void) {
    bool back_pressed = false;
    uint32_t back_press_start_ms = 0;
    extern uint32_t esp_log_timestamp(void);
    uint32_t start_ms = (uint32_t)esp_log_timestamp();

    /* === 关键修复: 返回主菜单键(F_EXIT)与返回键(F_BACK)是不同按键.
     * 但输入层把 F_EXIT 也映射成了 HOME/返回动作, 弹窗打开时 F_EXIT 若仍被按住,
     * 防抖之后会把弹窗当成"取消"而闪退、游戏也不暂停.
     * 因此这里取消动作只认返回键 (MENU_ACTION_BACK / F_BACK 长按), 不认 HOME. */
    bool back_trigger_held = bt_manager_is_key_pressed(F_BACK); /* F_BACK 长按触发时是否仍按着 */

    /* V1.0.68 fix: 游戏内禁用了手柄导航键, input_get_action() 收不到 BT F_CONFIRM,
     * 玩家按确认无效只能等 10s 超时 (日志实锤 233702 请求 → 234182 确认无反应 → 243710 超时).
     * 这里直接轮询 F_CONFIRM 上升沿 (与 GB 退出弹窗一致). */
    bool confirm_prev = bt_manager_is_key_pressed(F_CONFIRM);

    while (1) {
        /* 每轮强制重绘+刷新, 使弹窗保持可见 (弹窗优先级最高),
         * 避免被游戏帧 flush 覆盖后一闪而过. */
        menu_draw_notice_popup(g_lcd, "\xe9\x80\x80\xe5\x87\xba\xe6\xb8\xb8\xe6\x88\x8f\xef\xbc\x9f"); /* 退出游戏？ */
        st7305_flush(g_lcd);

        /* 触发键已松开 → 之后 F_BACK 长按才当作真正的取消 */
        if (back_trigger_held && !bt_manager_is_key_pressed(F_BACK)) back_trigger_held = false;

        menu_action_t action = input_get_action();

        /* V1.0.xx: 触摸 CONFIRM 区分点击区域 — 点击弹窗外=取消, 内=确认 */
        if (action == MENU_ACTION_CONFIRM) {
            int tx, ty;
            if (input_consume_tap(&tx, &ty)) {
                /* 计算弹窗区域 (与 draw_notice_popup 一致) */
                const int NOTICE_B = 3, NOTICE_P = 3, NOTICE_H = 3+3+3+3+24;
                int tw = 5 * 12; /* "退出游戏？" 5字 * 12px */
                int W = NOTICE_B*2 + NOTICE_P*2 + tw;
                int H = NOTICE_H;
                int px = (ST7305_WIDTH - W) / 2;
                int py = (ST7305_HEIGHT - H) / 2;
                if (tx >= px && tx < px + W && ty >= py && ty < py + H) {
                    return GAME_EXIT_CONFIRMED;  /* 点击弹窗内 → 确认 */
                }
                return GAME_EXIT_CANCEL;          /* 点击弹窗外 → 取消 */
            }
            return GAME_EXIT_CONFIRMED;  /* 物理键 CONFIRM → 确认 */
        }
        /* V1.0.68 fix: 手柄确认键 (F_CONFIRM) 上升沿 → 立即退出 */
        bool now_confirm = bt_manager_is_key_pressed(F_CONFIRM);
        if (now_confirm && !confirm_prev) {
            ESP_LOGI(TAG, "退出确认: 手柄确认键退出");
            return GAME_EXIT_CONFIRMED;
        }
        confirm_prev = now_confirm;

        if (action == MENU_ACTION_BACK) {
            return GAME_EXIT_CANCEL;
        }
        /* MENU_ACTION_HOME(=F_EXIT 返回主菜单键) 不再作为取消, 只负责请求退出. */
        /* 手柄 F_BACK 长按 500ms → 取消恢复游戏 (仅当 F_BACK 非本次触发键时有效). */
        bool now_back = bt_manager_is_key_pressed(F_BACK);
        uint32_t now_ms = (uint32_t)esp_log_timestamp();
        if (now_back && !back_pressed) {
            back_pressed = true;
            back_press_start_ms = now_ms;
        } else if (!now_back) {
            back_pressed = false;
        } else if (!back_trigger_held && back_pressed && (uint32_t)(now_ms - back_press_start_ms) >= 500) {
            return GAME_EXIT_CANCEL;
        }

        /* 超时 (无按键) → 自动退出到桌面 */
        if ((uint32_t)(now_ms - start_ms) >= 10000) {
            return GAME_EXIT_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

game_exit_result_t gam4980_emu_run(void) {
    g_running = true;
    /* V1.0.68 fix: BBK 游戏不走通用 game_run_loop, 必须在此启用屏幕虚拟按键 (V1.0.94: 三态) */
    virtual_keys_set_enabled(menu_vkey_effective(&g_menu));
    s_home_prev = false;  /* 每次进入游戏重置退出键边沿状态 */
    game_exit_result_t exit_result = GAME_EXIT_CONFIRMED;
    ESP_LOGI(TAG, "开始运行游戏循环 (长按 BOOT 1秒 或 按 退出到菜单 键 退出, 60fps)");
    int boot_hold_count = 0;
    uint32_t frame_count = 0;
    uint32_t fps_last_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    /* V1.0.59: 精确 60fps 节流 (esp_timer, 最后 2ms 忙等).
     * 之前 vTaskDelayUntil(16ms)=62.5fps, 音频按 735 样本/帧喂送 (44100/60)
     * 产量比消耗快 ~4%, 环形缓冲灌满后每帧末尾丢片段 -> 长按音频繁空隙. */
    int64_t next_frame_us = esp_timer_get_time();
    bool exit_requested = false;  /* V1.0.47: 退出前先弹确认框 */
        while (g_running) {
            /* 游戏壁纸模式: 任意设备按键 → 强制退出 (无确认框) */
            if (s_wallpaper_mode) {
                if (input_get_action() != MENU_ACTION_NONE) {
                    ESP_LOGI(TAG, "游戏壁纸: 按键退出");
                    exit_result = GAME_EXIT_CONFIRMED;
                    break;
                }
            }
            uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
            retro_run();
            bbk_key_sound_tick();
            bbk_audio_frame_finalize();
        uint32_t t1 = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* V1.0.53: 移除 halt 自动退出检测.
         * 步步高游戏在"选择存档/等按键"等界面会置 halt 位待机 (等中断唤醒),
         * 原检测会把待机误判成"选退出游戏", 导致进游戏秒退回菜单.
         * 现在一律手动退出: 按返回菜单键 (F_EXIT) 或长按 BOOT. */

        /* 调用核心的帧率回调,让核心内部RTC正确计时 */
        if (g_core_frame_cb) {
            retro_usec_t usec = (t1 - t0) * 1000;
            if (usec == 0) usec = g_frame_reference;
            g_core_frame_cb(usec);
        }

        frame_count++;
        if (input_is_held(1)) {
            boot_hold_count++;
            if (boot_hold_count > 60) {
                ESP_LOGI(TAG, "长按 BOOT, 请求退出游戏");
                exit_requested = true;
            }
        } else {
            boot_hold_count = 0;
        }
        /* 退出到菜单: 手柄映射的第 7 键 (F_EXIT) 按下即退出游戏返回菜单
         * 上升沿检测, 单击一次即触发, 避免长按/抖动误触 */
        bool home_now = bt_manager_is_key_pressed(F_EXIT);
        if (home_now && !s_home_prev) {
            ESP_LOGI(TAG, "按下 退出到菜单 键, 请求退出游戏");
            exit_requested = true;
        }
        s_home_prev = home_now;
        /* 用户需求: 取消"长按 返回(F_BACK) 键退出游戏返回菜单"行为.
         * 短按保留给 BBK "跳出" 键 (joypad B = KEY_EXIT), 长按不再退出 (与 GB 流程一致). */

        /* V1.0.68: 每帧轮询设备物理键 + 触摸手势 (手柄导航键在游戏内已禁用):
         *  - HOME / POWER_RELEASE (软关机键0.5s后松手 / 状态栏长按3s) → 立即返回主菜单
         *  - POWER_LOCK (软关机键点按) → 退出游戏回主菜单
         *  - BACK (BOOT 长按 / 底部中间上滑) → 弹确认框退出 */
        menu_action_t action = input_get_action();
        if (action == MENU_ACTION_HOME || action == MENU_ACTION_POWER_RELEASE ||
            action == MENU_ACTION_POWER_LOCK) {
            ESP_LOGI(TAG, "HOME/电源键 -> 返回主菜单");
            exit_result = GAME_EXIT_TIMEOUT;
            break;
        }
        if (action == MENU_ACTION_BACK || action == MENU_ACTION_LONG_LEFT) {
            ESP_LOGI(TAG, "BACK -> 请求退出游戏");
            exit_requested = true;
        }
        if (virtual_keys_is_enabled()) {
            int tx = 0, ty = 0;
            bool down = input_get_touch_pos(&tx, &ty);
            virtual_keys_poll(tx, ty, down);
        }

        /* V1.0.47: 请求退出 → 弹确认框, 确认才退出, 取消则恢复游戏继续 */
        if (exit_requested) {
            exit_result = gam4980_exit_confirm_dialog();
            if (exit_result == GAME_EXIT_CANCEL) {
                /* 取消: 恢复游戏, 清除退出请求并重置各退出键状态 */
                exit_requested = false;
                boot_hold_count = 0;
                s_home_prev = bt_manager_is_key_pressed(F_EXIT);
            } else {
                break;
            }
        }

        /* 喂 TWDT: 已移除. 游戏循环运行在 app_main 任务, 而 app_main 未注册到 TWDT,
         * 调用 esp_task_wdt_reset() 会每秒刷屏 E (xxx) task_wdt: task not found,
         * 阻塞 UART 输出 → 按键响应被饿死 → 游戏卡死并触发重启. 与屏保逻辑一致
         * (menu_system.c 中 app_main 任务同样不喂狗, 靠每帧让出 CPU 保持系统看门狗正常). */

        /* === 精确 60fps 节流 === */
        next_frame_us += 16667;
        if (esp_timer_get_time() < next_frame_us) {
            int64_t left = next_frame_us - esp_timer_get_time();
            if (left > 2000) {
                vTaskDelay((TickType_t)((left - 2000) / 1000));
            }
            /* 最后 2ms: 忙等保证 60.0fps */
            while (esp_timer_get_time() < next_frame_us) {
                /* spin */
            }
        } else {
            next_frame_us = esp_timer_get_time();
            if ((frame_count % 30) == 0) vTaskDelay(1);
            else taskYIELD();
        }

        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - fps_last_ms >= 1000) {
            ESP_LOGI(TAG, "Game FPS=%lu, retro_run=%lu ms", (unsigned long)frame_count, (unsigned long)(t1 - t0));
            frame_count = 0;
            fps_last_ms = now_ms;
        }
    }
    gam4980_emu_stop();
    virtual_keys_set_enabled(false);
    return exit_result;
}

void gam4980_emu_stop(void) {
    g_running = false;
    /* 先把 PSRAM 中的存档写回 SD 卡 (必须在 retro_unload_game 之前,
     * 避免后续 menu 流程误清空 sys_flash). */
    if (g_rom_path[0]) {
        gam4980_emu_save_state(g_rom_path);
    }
    retro_unload_game();
    /* 不调用 retro_deinit: 退出游戏后仍停留在二级菜单, 引擎保留, 便于选下一个游戏 */
}

/* === 卸载引擎: 退出电子词典回主菜单时调用 ===
 * 真正释放引擎动态分配的 6MB PSRAM (sys_flash + sys_rom_8 + sys_rom_e).
 * 下次进入电子词典时会重新触发 background_init, 从 Flash 重新加载. */
void gam4980_emu_unload(void) {
    if (!g_core_initialized && !g_init_in_progress) return;
    ESP_LOGI(TAG, "卸载引擎: 释放 PSRAM");
    g_running = false;
    if (g_init_in_progress) {
        /* 如果后台正在初始化, 等待完成 (不能中途打断) */
        ESP_LOGI(TAG, "等待后台初始化完成后再卸载...");
        while (g_init_in_progress) vTaskDelay(pdMS_TO_TICKS(50));
    }
    retro_unload_game();
    retro_deinit();          /* 释放 sys_flash/sys_rom_8/sys_rom_e (6MB) */
    g_core_initialized = false;
    g_bg_progress = 0;
    g_bg_active = false;
    ESP_LOGI(TAG, "引擎已卸载 (PSRAM 已释放)");
    ESP_LOGI(TAG, "[MEM] 引擎卸载后: 内部空闲=%u 内部最大块=%u PSRAM空闲=%u PSRAM最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* === 游戏存档持久化 (SD 卡) ===
 * 解决用户反馈: "游戏退出后再进入能看到进度, 但重启后丢失"
 * 原因: 之前完全没把 PSRAM 的 sys_flash 存档区写回 SD 卡, 重启 PSRAM 数据丢失.
 * 修复: 退出游戏时把真正的存档区 (32KB) 写入 /sdcard/dict/<romname>.sav,
 *       进入游戏时 (retro_load_game 之后) 从该文件恢复.
 * 大小: 0x8000 = 32768 字节 = sys_flash[0x0000..0x7FFF] (游戏视角 0x1F8000-0x1FFFFF
 *       旋转后区域, libretro 注释为 "last 32 KiB for save file").
 *       大部分是 0xFF 未写入区域, 真实有效数据通常 < 4KB.
 *
 * 历史问题 (V1.0.7 及之前): 曾用 0x14000 (80KB) 3 段拼接, 但 libretro 视角的"非存档
 *   段 2/段 3"实际对应 sys_flash[0x8000+] 游戏数据区, 保存/恢复时会破坏游戏数据,
 *   导致重启后游戏卡死 (BRK) 且进度看似丢失. 修复: 缩小到真正只有存档的 32KB. */
#define SAVE_DIR          "/sdcard/dict"
#define SAVE_SIZE         0x8000      /* 32KB, 真正的用户存档区 (修正: 之前 0x14000 有 bug) */
#define SAVE_MAGIC        0x53415645  /* 'SAVE' 魔数, 验证文件类型 */

/* 保存文件格式 (小端):
 *   [0..3]   uint32_t magic = 'SAVE' (0x53415645)
 *   [4..7]   uint32_t size  = 0x8000 (32KB, 真正存档区大小)
 *   [8..8+size] raw save data (sys_flash[0..0x7FFF] 32KB)
 *
 * 兼容旧版: V1.0.7 及之前 .sav 文件大小 0x14008 (8 头 + 80KB), load_state 会自动
 *   检测并仅提取前 32KB 加载, 旧段 2/段 3 丢弃. */
static void make_save_path(const char *rom_path, char *out, size_t out_size) {
    /* 从 rom_path 提取文件名 (去掉目录与扩展名), 生成 <name>.sav */
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(name, '.');
    int name_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (name_len <= 0 || name_len >= 64) {
        /* 兜底: 截断到 32 字节 */
        name_len = strlen(name) > 32 ? 32 : (int)strlen(name);
        dot = NULL;
    }
    snprintf(out, out_size, "%s/%.*s.sav", SAVE_DIR, name_len, name);
}

void gam4980_emu_save_state(const char *rom_path) {
    if (!rom_path) return;
    if (!g_core_initialized) {
        ESP_LOGW(TAG, "save_state: 引擎未初始化, 跳过");
        return;
    }
    char path[160];
    make_save_path(rom_path, path, sizeof(path));

    /* 申请 PSRAM 缓冲 (32KB 优先用 PSRAM, 避免占内部 SRAM) */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(SAVE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(SAVE_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "save_state: 分配 %d 字节失败", SAVE_SIZE);
        return;
    }

    /* 从 sys_flash[0..0x7FFF] 读出真正存档区 (32KB) */
    gam4980_flash_read_save(buf, SAVE_SIZE);

    /* 写入文件: 8 字节头 (magic + size) + 32KB 数据 = 0x8008 字节 */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "save_state: 打开失败 %s (errno=%d)", path, errno);
        free(buf);
        return;
    }
    uint32_t magic = SAVE_MAGIC;
    uint32_t size  = SAVE_SIZE;
    fwrite(&magic, 1, 4, f);
    fwrite(&size,  1, 4, f);
    size_t written = fwrite(buf, 1, SAVE_SIZE, f);
    fclose(f);
    free(buf);

    if (written == SAVE_SIZE) {
        ESP_LOGI(TAG, "save_state: 存档已保存 %s (%d 字节)", path, (int)(SAVE_SIZE + 8));
    } else {
        ESP_LOGE(TAG, "save_state: 写入不完整 %u/%u", (unsigned)written, (unsigned)SAVE_SIZE);
    }
}

void gam4980_emu_load_state(const char *rom_path) {
    if (!rom_path) return;
    if (!g_core_initialized) {
        ESP_LOGW(TAG, "load_state: 引擎未初始化, 跳过");
        return;
    }
    char path[160];
    make_save_path(rom_path, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGI(TAG, "load_state: 无存档 %s", path);
        return;
    }
    /* 兼容旧版 80KB 存档 (V1.0.7 及之前) + 新版 32KB 存档.
     *   新版: 8 (头) + 0x8000 (32KB 数据) = 0x8008
     *   旧版: 8 (头) + 0x14000 (80KB 数据, 3 段拼接) = 0x14008
     * 旧版前 32KB 即为真正的存档区 (段 1), 提取后正常加载. */
    const off_t NEW_SIZE = SAVE_SIZE + 8;            /* 0x8008 = 32808 */
    const off_t OLD_SIZE = 0x14000 + 8;              /* 0x14008 = 81928 */
    const size_t ARCHIVE_BYTES = 0x8000;             /* 真正需要的存档数据 = 32KB */
    if (st.st_size != NEW_SIZE && st.st_size != OLD_SIZE) {
        ESP_LOGW(TAG, "load_state: 文件大小异常 %lld, 期望 %d 或 %d",
                 (long long)st.st_size, (int)NEW_SIZE, (int)OLD_SIZE);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "load_state: 打开失败 %s", path);
        return;
    }
    uint32_t magic = 0, size = 0;
    if (fread(&magic, 1, 4, f) != 4 || fread(&size, 1, 4, f) != 4 ||
        magic != SAVE_MAGIC) {
        ESP_LOGE(TAG, "load_state: 文件头 magic 不匹配 magic=0x%08x", magic);
        fclose(f);
        return;
    }
    /* 旧版 size 字段可能是 0x14000, 新版是 0x8000, 这里不强校验, 兼容读取 */
    bool from_old = (st.st_size == OLD_SIZE);
    if (from_old) {
        ESP_LOGW(TAG, "load_state: 检测到旧版 80KB 存档, 提取前 32KB 加载");
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(ARCHIVE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(ARCHIVE_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "load_state: 分配 %d 字节失败", ARCHIVE_BYTES);
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, ARCHIVE_BYTES, f);
    fclose(f);
    if (rd != ARCHIVE_BYTES) {
        ESP_LOGE(TAG, "load_state: 读取不完整 %u/%u", (unsigned)rd, (unsigned)ARCHIVE_BYTES);
        free(buf);
        return;
    }

    /* V1.0.53 诊断: 打印存档内容统计, 排查"选择存档界面闪退"是否由存档数据引起.
     * 0xFF = flash 擦除态 (空); 有效数据多说明存档非空. */
    {
        uint32_t non_ff = 0;
        for (uint32_t i = 0; i < ARCHIVE_BYTES; i++) if (buf[i] != 0xFF) non_ff++;
        ESP_LOGI(TAG, "load_state: 存档统计 non_0xFF=%u/32768 头16=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 (unsigned)non_ff,
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                 buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
    }

    /* 仅写入 sys_flash[0..0x7FFF] 真正的存档区 (32KB), 不触碰游戏数据区.
     * 旧版 80KB 存档的段 2/段 3 (对应游戏数据区) 已经被丢弃, 避免破坏游戏. */
    gam4980_flash_write_save(buf, ARCHIVE_BYTES);
    free(buf);
    ESP_LOGI(TAG, "load_state: 存档已恢复 %s", path);
}
