/* PSP XMB 风格菜单系统
 * - 主菜单: 横向大图标 (选中 64x64, 其他 32x32)
 * - 子菜单: 垂直列表 (24x24 中文字体)
 * - 3 键导航: 左/右/确认 + 退出
 */
#include "menu_system.h"
#include "wallpapers.h"
#include "esp_timer.h"
#include "favorites.h"
#include "font_zh.h"
#include "sd_scan.h"
#include "gam4980_emu.h"
#include "file_browser.h"
#include "bt_manager.h"
#include "bt_icon_png.h"   /* 状态栏蓝牙图标 (来自桌面图标文件夹 蓝牙.png) */
#if WIFI_SUPPORT
#include "wifi_manager.h"
#include "esp_http_client.h"
#endif
#include "audio_player.h"
#include "web_gamepad.h"
#include "virtual_keys.h"
#include "gb_emu.h"
#include "gbc_emu.h"
#include "nes_emu.h"
#include "arduboy_avr.h"
#include "engine_manager.h"
#include "board_rlcd.h"    /* board_shim: GB/GBC 显示/音频适配层 */
#include "book_reader.h"
#include "tone_player.h"   /* V1.0.68: 方波直驱音调播放器 */
#include "touch_panel.h"   /* V1.0.68: 禁用触摸屏 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

/* 过滤 macOS Finder 生成的 AppleDouble 元数据文件 (._开头) 和 .DS_Store */
#define MENU_IS_APPLE_METANAME(n)  ((n)[0] == '.' && ((n)[1] == '_' || ((n)[1] == 'D' && strcmp((n), ".DS_Store") == 0)))
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_crt_bundle.h"   /* V1.0.68: HTTPS 证书包 (天气时钟) */
#include "driver/ledc.h"      /* V1.0.68: 方波直驱 (GPIO48 PWM 音调) */

/* V1.0.41: input.c 的手柄导航开关 (按键映射期间禁用) */
extern void input_set_gamepad_nav_enabled(bool enabled);
/* GB/GBC joypad 掩码 (低电平有效), 供 GB/GBC 模拟器每帧查询按键状态 */
extern uint8_t input_get_held_gb_joypad(void);
extern menu_action_t input_get_action(void);
/* V1.0.68: 软关机键 GPIO1 长按 2 秒软关机轮询 */
extern bool input_power_should_sleep(void);
/* V1.0.68: 最近一次动作是否来自触摸 (确认框区分上滑确认/物理 BACK 取消) */
extern bool input_touch_last_action(void);
#include "esp_heap_caps.h"
#include "esp_attr.h"

/* 赞助作者图片位图: 文件级 static const, 存于 .rodata (flash), 不占任务栈.
 * 注意: 必须放文件作用域, 绝不能放在函数内 (否则 11KB 数组会占 menu_render 的栈,
 * 撑爆 main_task 14KB 栈 → 踩坏 .bss 日志互斥量 → 开机后台任务断言复位). */
#include "sponsor_img.inc"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "MENU"

#define SCREEN_W ST7305_WIDTH     /* 400 */
#define SCREEN_H ST7305_HEIGHT    /* 300 */

extern menu_state_t g_menu;

/* XMB 图标数据 (由 generate_icons.py 生成) */
#include "icons_data.inc"
/* 状态栏图标 (24x24) */
#include "status_icons_data.inc"

/* ============ 图标全部内置, 不从 SD 卡加载主题 ============ */

/* 状态栏图标索引 (只有电池) */
#define SI_BAT_0    0   /* 空 */
#define SI_BAT_1    1   /* 1/3 */
#define SI_BAT_2    2   /* 2/3 */
#define SI_BAT_3    3   /* 满 */



/* ============ 通用绘图工具 ============ */

static inline void draw_pixel_inv(st7305_handle_t *lcd, int x, int y,
                                   st7305_color_t color, bool inverted) {
    st7305_draw_pixel(lcd, x, y, inverted ? (1 - color) : color);
}

static void fill_rect(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                      st7305_color_t color) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            st7305_draw_pixel(lcd, x, y, color);
}

static void draw_hline(st7305_handle_t *lcd, int x0, int x1, int y,
                       st7305_color_t color) {
    for (int x = x0; x <= x1; x++) st7305_draw_pixel(lcd, x, y, color);
}

/* V1.0.68: 启动失败等提示用迷你小框 (同"已收藏"提示), 不用大方框 */
static void menu_show_fail_hint(menu_state_t *state, const char *msg) {
    snprintf(state->hint_text, sizeof(state->hint_text), "%s", msg);
    state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 1500;
    state->needs_redraw = true;
}

static void draw_vline(st7305_handle_t *lcd, int x, int y0, int y1,
                       st7305_color_t color) {
    for (int y = y0; y <= y1; y++) st7305_draw_pixel(lcd, x, y, color);
}

static void draw_rect_outline(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                              st7305_color_t color) {
    draw_hline(lcd, x0, x1, y0, color);
    draw_hline(lcd, x0, x1, y1, color);
    draw_vline(lcd, x0, y0, y1, color);
    draw_vline(lcd, x1, y0, y1, color);
}

/* ============ 前向声明 (供状态栏/屏保使用) ============ */
static void draw_ascii_small(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
static void draw_ascii_medium(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
static void draw_ascii(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
static void draw_zh(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted, int scale);
static void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx);
static void draw_text_centered(st7305_handle_t *lcd, int y, const char *str, bool inverted);
/* V1.0.33: 紧凑提示弹窗模板 - 前向声明 (定义在文件下方) */
static void draw_notice_popup(st7305_handle_t *lcd, const char *text);
/* 确认弹窗前向声明 (游戏退出确认框使用, 定义在文件下方) */
static void draw_confirm_dialog(menu_state_t *state);

/* ============ 时间功能 (基于 ESP32-S3 RTC) ============ */

void menu_get_time_str(char *buf, int bufsize) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(buf, bufsize, "%02d:%02d", t->tm_hour, t->tm_min);
}

void menu_set_time(int hour, int minute, int second) {
    struct timeval tv;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    t->tm_hour = hour;
    t->tm_min = minute;
    t->tm_sec = second;
    tv.tv_sec = mktime(t);
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "时间已设置: %02d:%02d:%02d", hour, minute, second);
}

/* V1.0.46: 时间持久化到 NVS (断电重启恢复) + 断电默认 2026-08-01 */
#define TIME_NVS_NS "menu_settings"
static void time_save_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    nvs_set_i32(h, "save_year",  t.tm_year + 1900);
    nvs_set_i32(h, "save_month", t.tm_mon + 1);
    nvs_set_i32(h, "save_day",   t.tm_mday);
    nvs_set_i32(h, "save_hour",  t.tm_hour);
    nvs_set_i32(h, "save_min",   t.tm_min);
    nvs_set_i32(h, "save_sec",   t.tm_sec);
    nvs_commit(h);
    nvs_close(h);
}

static void time_load_from_nvs(void) {
    nvs_handle_t h;
    int32_t y = 0, mo = 0, d = 0, ho = 0, mi = 0, se = 0;
    if (nvs_open(TIME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        bool ok = (nvs_get_i32(h, "save_year", &y)  == ESP_OK) &&
                  (nvs_get_i32(h, "save_month", &mo) == ESP_OK) &&
                  (nvs_get_i32(h, "save_day", &d)   == ESP_OK) &&
                  (nvs_get_i32(h, "save_hour", &ho) == ESP_OK) &&
                  (nvs_get_i32(h, "save_min", &mi)  == ESP_OK) &&
                  (nvs_get_i32(h, "save_sec", &se)  == ESP_OK);
        nvs_close(h);
        if (ok && y >= 2020 && y <= 2099) {
            struct tm t = {0};
            t.tm_year = (int)y - 1900;
            t.tm_mon  = (int)mo - 1;
            t.tm_mday = (int)d;
            t.tm_hour = (int)ho;
            t.tm_min  = (int)mi;
            t.tm_sec  = (int)se;
            t.tm_isdst = -1;
            time_t ts = mktime(&t);
            if (ts != (time_t)-1) {
                struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "时间已从 NVS 恢复: %04d-%02d-%02d %02d:%02d:%02d",
                         (int)y, (int)mo, (int)d, (int)ho, (int)mi, (int)se);
            }
            return;
        }
    }
    /* 无有效 NVS 记录: 系统时间无效 (< 2026-01-01) 时设为默认 2026-08-01 */
    time_t now = time(NULL);
    struct tm chk;
    localtime_r(&now, &chk);
    if (chk.tm_year + 1900 < 2026) {
        struct tm t = {0};
        t.tm_year = 2026 - 1900;
        t.tm_mon  = 8 - 1;
        t.tm_mday = 1;
        t.tm_isdst = -1;
        time_t ts = mktime(&t);
        if (ts != (time_t)-1) {
            struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "时间无效, 设为默认 2026-08-01");
        }
    }
}

/* 从 RTC 读取时间到 settings */
static void read_time_from_rtc(menu_settings_t *settings) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    settings->hour = (uint8_t)t->tm_hour;
    settings->minute = (uint8_t)t->tm_min;
    settings->second = (uint8_t)t->tm_sec;
}

/* ============ 苹果风格电池图标 (四态显示) ============ */
/* V1.0.43: 参考电池电压 ADC 读取, 四档离散显示
 * 绘制苹果风格电池图标: 长方形主体 + 右上角小凸起 + 内部四档电量
 * 位置 x,y, 电池宽 28, 高 14, 正极帽 3x6
 * 四档: 0%(空) / 1-33%(低) / 34-66%(中) / 67-100%(满) */
static void draw_apple_battery(st7305_handle_t *lcd, int x, int y, uint8_t percent, bool charging) {
    int bw = 28, bh = 14;
    int cap_w = 3, cap_h = 6;

    /* V1.0.68: 未检测到电池 (255 哨兵) -> 电池图标内画 "?" */
    if (percent == 255) {
        draw_rect_outline(lcd, x, y, x + bw - 1, y + bh - 1, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 2, y + 2, x + bw - 3, y + bh - 3, ST7305_COLOR_WHITE);
        st7305_draw_text(lcd, x + 7, y - 1, "?");
        return;
    }

    /* 正极帽 (右上角小凸起) */
    int cap_x = x + bw;
    int cap_y = y + (bh - cap_h) / 2;
    fill_rect(lcd, cap_x, cap_y, cap_x + cap_w - 1, cap_y + cap_h - 1, ST7305_COLOR_BLACK);

    /* 电池外框 */
    draw_rect_outline(lcd, x, y, x + bw - 1, y + bh - 1, ST7305_COLOR_BLACK);

    /* 内部背景 (白色) */
    fill_rect(lcd, x + 2, y + 2, x + bw - 3, y + bh - 3, ST7305_COLOR_WHITE);

    /* V1.0.43: 四态离散填充 (内部可用宽度 = bw-6 = 22px, 分四档) */
    /* 档位: 0=空(不填), 1=低(1/3≈7px), 2=中(2/3≈14px), 3=满(22px) */
    int level = 0;
    if (percent > 66)      level = 3;
    else if (percent > 33) level = 2;
    else if (percent > 0)  level = 1;

    if (level > 0) {
        int fill_w = (bw - 6) * level / 3;  /* 7, 14, 22 */
        /* 20260812: 电量从左往右填充, 缺少(空)的部分在右边 (与常规电池图标一致).
         * 之前从右往左填充, 空的部分显示在左边. */
        fill_rect(lcd, x + 2, y + 2, x + 2 + fill_w - 1, y + bh - 3, ST7305_COLOR_BLACK);
    }

    /* 充电闪电图标 */
    if (charging) {
        int lx = x + bw / 2 - 3;
        int ly = y + bh / 2 - 4;
        /* 闪电形状 */
        st7305_draw_pixel(lcd, lx + 1, ly, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 1, ly + 1, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly + 1, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx, ly + 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 3, ly + 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 1, ly + 3, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly + 3, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly + 4, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly + 5, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 1, ly + 6, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 2, ly + 6, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, lx + 3, ly + 6, ST7305_COLOR_BLACK);
    }
}

/* ============ 可复用状态栏 ============ */

/* 绘制 2px 宽粗线 (macOS 菜单栏风格) */
static void draw_thick_hline(st7305_handle_t *lcd, int x0, int x1, int y,
                              st7305_color_t c) {
    draw_hline(lcd, x0, x1, y, c);
    draw_hline(lcd, x0, x1, y + 1, c);
}
static void draw_thick_vline(st7305_handle_t *lcd, int x, int y0, int y1,
                              st7305_color_t c) {
    draw_vline(lcd, x, y0, y1, c);
    draw_vline(lcd, x + 1, y0, y1, c);
}

/* 绘制蓝牙图标 (16x16, macOS 菜单栏风格, 2px 线宽)
 * connected=false: 黑色线性图标 (已开启未连接)
 * connected=true:  黑色填充背景 + 白色图标 (已连接) */
static void draw_bt_icon(st7305_handle_t *lcd, int x, int y, bool connected) {
    if (connected) {
        /* 连接成功: 黑色圆角背景 + 白色蓝牙符号 (2px 线宽) */
        fill_rect(lcd, x, y, x + 15, y + 15, ST7305_COLOR_BLACK);
        st7305_color_t c = ST7305_COLOR_WHITE;
        /* 中心 2px 竖线 */
        draw_thick_vline(lcd, x + 7, y + 3, y + 12, c);
        /* 上 V 对角线 (2px 宽, 用两条平行对角线) */
        for (int i = 0; i < 4; i++) {
            st7305_draw_pixel(lcd, x + 7 - i, y + 3 + i, c);
            st7305_draw_pixel(lcd, x + 8 - i, y + 3 + i, c);
            st7305_draw_pixel(lcd, x + 7 + i, y + 3 + i, c);
            st7305_draw_pixel(lcd, x + 8 + i, y + 3 + i, c);
        }
        /* 下 V 对角线 */
        for (int i = 0; i < 4; i++) {
            st7305_draw_pixel(lcd, x + 7 - i, y + 12 - i, c);
            st7305_draw_pixel(lcd, x + 8 - i, y + 12 - i, c);
            st7305_draw_pixel(lcd, x + 7 + i, y + 12 - i, c);
            st7305_draw_pixel(lcd, x + 8 + i, y + 12 - i, c);
        }
        /* 横线连接 (2px) */
        draw_thick_hline(lcd, x + 4, x + 7, y + 6, c);
        draw_thick_hline(lcd, x + 4, x + 7, y + 9, c);
    } else {
        /* 未连接: 黑色线性图标 (2px 线宽) */
        st7305_color_t c = ST7305_COLOR_BLACK;
        /* 中心 2px 竖线 */
        draw_thick_vline(lcd, x + 7, y + 2, y + 13, c);
        /* 上 V 对角线 (2px 宽) */
        for (int i = 0; i < 4; i++) {
            st7305_draw_pixel(lcd, x + 7 - i, y + 2 + i, c);
            st7305_draw_pixel(lcd, x + 8 - i, y + 2 + i, c);
            st7305_draw_pixel(lcd, x + 7 + i, y + 2 + i, c);
            st7305_draw_pixel(lcd, x + 8 + i, y + 2 + i, c);
        }
        /* 下 V 对角线 */
        for (int i = 0; i < 4; i++) {
            st7305_draw_pixel(lcd, x + 7 - i, y + 13 - i, c);
            st7305_draw_pixel(lcd, x + 8 - i, y + 13 - i, c);
            st7305_draw_pixel(lcd, x + 7 + i, y + 13 - i, c);
            st7305_draw_pixel(lcd, x + 8 + i, y + 13 - i, c);
        }
        /* 横线连接 (2px) */
        draw_thick_hline(lcd, x + 4, x + 7, y + 5, c);
        draw_thick_hline(lcd, x + 4, x + 7, y + 10, c);
    }
}

/* 绘制 WiFi 图标 (16x16, macOS 菜单栏风格, 2px 线宽) */
static void draw_wifi_icon(st7305_handle_t *lcd, int x, int y) {
    st7305_color_t c = ST7305_COLOR_BLACK;
    /* 中心点 (2x2 实心) */
    fill_rect(lcd, x + 7, y + 12, x + 8, y + 13, c);
    /* 内弧 (2px 宽) */
    draw_thick_hline(lcd, x + 5, x + 10, y + 9, c);
    st7305_draw_pixel(lcd, x + 4, y + 8, c);
    st7305_draw_pixel(lcd, x + 4, y + 9, c);
    st7305_draw_pixel(lcd, x + 11, y + 8, c);
    st7305_draw_pixel(lcd, x + 11, y + 9, c);
    /* 中弧 (2px 宽) */
    draw_thick_hline(lcd, x + 3, x + 12, y + 5, c);
    st7305_draw_pixel(lcd, x + 2, y + 4, c);
    st7305_draw_pixel(lcd, x + 2, y + 5, c);
    st7305_draw_pixel(lcd, x + 13, y + 4, c);
    st7305_draw_pixel(lcd, x + 13, y + 5, c);
    /* 外弧 (2px 宽) */
    draw_thick_hline(lcd, x + 1, x + 14, y + 1, c);
}

/* 绘制喇叭图标 (16x16, 2px 线宽, macOS 菜单栏风格)
 * 形状: 矩形喇叭体 (左) + 梯形扩散口 (右)
 * - 喇叭体: 4x8 矩形 (左侧)
 * - 扩散口: 4x6 梯形, 向右张开
 * - 声音波: 右侧 2-3 条弧线 (按音量档数显示) */
static void draw_speaker_icon(st7305_handle_t *lcd, int x, int y, int level) {
    st7305_color_t c = ST7305_COLOR_BLACK;
    /* 喇叭体 (左侧矩形 6x8, 居中) */
    int body_x0 = x + 1;
    int body_y0 = y + 5;
    int body_x1 = x + 4;
    int body_y1 = y + 10;
    /* 矩形描边 (2px) */
    draw_thick_hline(lcd, body_x0, body_x1, body_y0, c);
    draw_thick_hline(lcd, body_x0, body_x1, body_y1 - 1, c);
    draw_thick_vline(lcd, body_x0, body_y0, body_y1, c);
    draw_thick_vline(lcd, body_x1 - 1, body_y0, body_y1, c);
    /* 梯形扩散口 (向右张开) */
    /* 上边 */
    draw_thick_hline(lcd, body_x1, x + 8, y + 3, c);
    /* 下边 */
    draw_thick_hline(lcd, body_x1, x + 8, y + 12, c);
    /* 右边 */
    draw_thick_vline(lcd, x + 8, y + 3, y + 13, c);
    /* 声音波 (按 level 数量显示) */
    if (level >= 3) {
        /* 第 1 波 */
        st7305_draw_pixel(lcd, x + 10, y + 4, c);
        st7305_draw_pixel(lcd, x + 10, y + 11, c);
        draw_thick_hline(lcd, x + 10, x + 10, y + 5, c);
        draw_thick_hline(lcd, x + 10, x + 10, y + 10, c);
    }
    if (level >= 6) {
        /* 第 2 波 */
        st7305_draw_pixel(lcd, x + 12, y + 3, c);
        st7305_draw_pixel(lcd, x + 12, y + 12, c);
        draw_thick_hline(lcd, x + 12, x + 12, y + 4, c);
        draw_thick_hline(lcd, x + 12, x + 12, y + 11, c);
    }
    if (level >= 9) {
        /* 第 3 波 (最大音量) */
        st7305_draw_pixel(lcd, x + 14, y + 2, c);
        st7305_draw_pixel(lcd, x + 14, y + 13, c);
        draw_thick_hline(lcd, x + 14, x + 14, y + 3, c);
        draw_thick_hline(lcd, x + 14, x + 14, y + 12, c);
    }
    /* 静音: 喇叭上画 X */
    if (level == 0) {
        draw_thick_hline(lcd, x + 9, x + 14, y + 6, c);
        draw_thick_hline(lcd, x + 9, x + 14, y + 9, c);
    }
}

/* 绘制耳机图标 (16x16, macOS 菜单栏风格, 2px 线宽) */
static void draw_headphone_icon(st7305_handle_t *lcd, int x, int y) {
    st7305_color_t c = ST7305_COLOR_BLACK;
    /* 耳机形状: 两个圆 + 头梁连接 */
    /* 左耳机 (2px 圆环) */
    /* 外圆 */
    for (int i = 0; i < 3; i++) {
        draw_hline(lcd, x + 2 + i, x + 6 + i, y + 3 + i, c);
        draw_hline(lcd, x + 2 + i, x + 6 + i, y + 7 - i, c);
        draw_vline(lcd, x + 2 + i, y + 3 + i, y + 7 - i, c);
        draw_vline(lcd, x + 6 + i, y + 3 + i, y + 7 - i, c);
    }
    /* 右耳机 */
    for (int i = 0; i < 3; i++) {
        draw_hline(lcd, x + 9 + i, x + 13 + i, y + 3 + i, c);
        draw_hline(lcd, x + 9 + i, x + 13 + i, y + 7 - i, c);
        draw_vline(lcd, x + 9 + i, y + 3 + i, y + 7 - i, c);
        draw_vline(lcd, x + 13 + i, y + 3 + i, y + 7 - i, c);
    }
    /* 头梁 (连接两个耳机的弧线) */
    draw_thick_hline(lcd, x + 3, x + 12, y + 2, c);
}

/* 绘制手柄图标 (16x16, macOS 菜单栏风格, 2px 线宽) */
static void draw_gamepad_icon(st7305_handle_t *lcd, int x, int y) {
    st7305_color_t c = ST7305_COLOR_BLACK;
    /* 手柄轮廓 (圆角矩形) */
    /* 左边十字键 */
    draw_thick_hline(lcd, x + 3, x + 6, y + 5, c);
    draw_thick_vline(lcd, x + 4, y + 3, y + 7, c);
    /* 右边 A/B/X/Y 按钮 */
    /* A 按钮 (右下圆形) */
    fill_rect(lcd, x + 10, y + 7, x + 12, y + 9, c);
    /* B 按钮 (右上圆形) */
    fill_rect(lcd, x + 12, y + 5, x + 14, y + 7, c);
    /* 手柄主体 */
    draw_thick_hline(lcd, x + 2, x + 13, y + 3, c);
    draw_thick_hline(lcd, x + 2, x + 13, y + 11, c);
    draw_thick_vline(lcd, x + 2, y + 3, y + 11, c);
    draw_thick_vline(lcd, x + 13, y + 3, y + 11, c);
}

/* 绘制状态栏 (菜单和游戏全屏共用)
 * 布局: 左侧 日期+时间(medium 12px, "M-DD HH:MM" 24小时制) | 音量 | 右侧蓝牙/WiFi/电池
 * V1.0.42: 日期与时间同尺寸, 12小时制带 AM/PM
 * 高度: 24px */
void menu_draw_status_bar(st7305_handle_t *lcd, const menu_settings_t *settings,
                          const char *center_text) {
    int sb_h = 24;
    int sb_y = 0;
    (void)center_text;  /* 状态栏中间文字已弃用 (用户需求: 删除 "MP3" 等中间字) */

    /* V1.0.46: 24小时制 (与设置里一致), 格式 "M-DD HH:MM" */
    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);

    /* 日期 "YYYY-MM-DD" (V1.0.68: 前面加年份) */
    char date_str[48];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    /* 时间 "HH:MM" (24小时制, 补零) */
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t->tm_hour, t->tm_min);

    int text_y = 3;  /* 中字体12px, 垂直居中: (24-12)/2 = 6, 微调到3 */
    int x = 3;

    /* 20260812 状态栏布局:
     *  最左: 日期 "M-DD"
     *  中:   时间 HH:MM 水平居中
     *  右:   电池 -> 蓝牙图标 -> 喇叭+音量 -> WiFi
     *  V1.0.68 fix: 蓝牙连接图标从左侧移到右侧 (电池左边, 音量右边) */
    /* 最左侧: 日期 "M-DD" */
    for (int i = 0; date_str[i]; i++) {
        draw_ascii_medium(lcd, x, text_y, date_str[i], false);
        x += 12;
    }

    /* 正中间: 时间 "HH:MM" 水平居中 */
    int time_w = (int)strlen(time_str) * 12;
    int time_x = (SCREEN_W - time_w) / 2;
    for (int i = 0; time_str[i]; i++) {
        draw_ascii_medium(lcd, time_x, text_y, time_str[i], false);
        time_x += 12;
    }

    /* 后台引擎预加载进度条: 在 render_select_game_two_cols 中专门绘制 (120x6 居中),
     * 这里不再重复画. 只在游戏全屏等没有专门进度条的地方才画小进度条. */

    /* 右侧从右到左: 电池 -> 蓝牙图标 -> 喇叭+音量 -> WiFi */
    int right_x = SCREEN_W - 36;
    int bat_y = sb_y + (sb_h - 14) / 2;
    draw_apple_battery(lcd, right_x, bat_y, settings->battery, false);
    right_x -= 20;

    /* V1.0.68 fix: 蓝牙图标移到电池左边 (连接时显示), 未连接时不占位 */
    if (settings->bt_enabled && settings->bt_connected) {
        st7305_draw_bitmap_1bit(lcd, right_x - 16, sb_y + (sb_h - BT_ICON_H) / 2,
                                BT_ICON_W, BT_ICON_H, bt_icon_png);
        right_x -= 20;   /* 图标 16px + 4px 间距 */
    }

    /* 喇叭图标 + 音量数字 (蓝牙图标左侧), V1.0.67: 统一 0-10 档, 10 显示两位数 */
    int vol_level = settings->mute ? 0 : (int)settings->volume;
    if (vol_level > 10) vol_level = 10;
    int vol_num_x = right_x - 12;          /* 数字最右 */
    int vol_num_w = (vol_level == 10) ? 24 : 12;   /* 两位/一位宽度 */
    int vol_icon_x = vol_num_x - vol_num_w - 2;    /* 图标在数字左边 */
    int vol_icon_y = sb_y + (sb_h - 16) / 2;
    if (vol_level == 10) {
        draw_ascii_medium(lcd, vol_num_x - 12, text_y, '1', false);
        draw_ascii_medium(lcd, vol_num_x, text_y, '0', false);
    } else {
        draw_ascii_medium(lcd, vol_num_x, text_y, (char)('0' + vol_level), false);
    }
    draw_speaker_icon(lcd, vol_icon_x, vol_icon_y, vol_level);
    right_x = vol_icon_x - 6;              /* 继续往左 */

    /* WiFi 图标 (V1.0.46: 连接成功才显示) */
#if WIFI_SUPPORT
    if (settings->wifi_enabled && wifi_manager_is_connected()) {
        draw_wifi_icon(lcd, right_x - 16, sb_y + (sb_h - 16) / 2);
    }
#else
    if (settings->wifi_enabled) {
        draw_wifi_icon(lcd, right_x - 16, sb_y + (sb_h - 16) / 2);
    }
#endif

    /* 状态栏中间文字已删除: 不再绘制 center_text (用户需求) */
}

/* ============ 屏保 ============ */
/* 20260812 临时: 壁纸游戏强制点对点(1x) 测试; 验证后移除. */
static uint32_t s_last_input_ms = 0;
static bool s_screensaver_active = false;
bool s_screensaver_preview = false;  /* 预览模式: 跳过倒计时 */
static bool s_screensaver_test = false;       /* 测试壁纸: 不挂蓝牙, 30s 自动退出 */
static uint32_t s_screensaver_test_ms = 0;    /* 测试壁纸进入时刻 */
static bool s_bt_suspended = false;  /* 屏保期间蓝牙已关闭, 退出时恢复 */

/* 进入屏保: 挂起蓝牙 (断开连接 + 停止扫描, 协议栈保持存活)
 * 不采用完整 disable/enable: BBK 引擎占用内部内存时蓝牙重新初始化
 * 会内存不足崩溃重启 (实测按键唤醒即重启). */
static void screensaver_bt_off(void) {
    if (s_bt_suspended) return;
    s_bt_suspended = true;
    ESP_LOGI(TAG, "屏保: 挂起蓝牙 (断开+停扫)");
    bt_manager_suspend();
}

/* 退出屏保: 恢复蓝牙并自动重连 */
static void screensaver_bt_on(void) {
    if (!s_bt_suspended) return;
    s_bt_suspended = false;
    ESP_LOGI(TAG, "屏保退出: 恢复蓝牙, 自动重连");
    bt_manager_resume();
}

/* 游戏壁纸模式: 任意按键强退 (含手柄全部逻辑键, 上升沿触发) */
static bool s_wp_key_inited = false;
static uint16_t s_wp_prev_keys = 0;

/* ============ 壁纸 (V1.0.64) ============ */
/* 壁纸类型 */
/* TF 动态图: /sdcard/wallpaper 下的 BMP 帧序列, 按文件名排序循环播放 */
#define WALLPAPER_BMP_DIR "/sdcard/wallpaper"
#define WALLPAPER_BMP_MAX  48
EXT_RAM_BSS_ATTR static char s_wp_bmp_files[WALLPAPER_BMP_MAX][64];
static int s_wp_bmp_count = 0;
static int s_wp_bmp_index = 0;
static uint8_t *s_wp_frame = NULL;          /* 400x300 1bpp 行序位图 (15KB, PSRAM) */
static uint32_t s_wp_bmp_last_ms = 0;

/* 退出壁纸时释放壁纸 PSRAM 缓冲 (内置程序 + TF 动态图), 下次进入再分配 */
static void screensaver_release_wallpaper_buffers(void) {
    if (s_wp_frame) {
        free(s_wp_frame);
        s_wp_frame = NULL;
    }
    wp_release_buffers();
}

/* 游戏壁纸列表 (持久化到 /sdcard/system/wallpaper_game.cfg) */
#define WALLPAPER_GAME_MAX 6
#define WALLPAPER_GAME_CFG "/sdcard/system/wallpaper_game.cfg"
/* 引擎编号: 0=GB 1=GBC 2=NES 3=arduboy 4=BBK */
EXT_RAM_BSS_ATTR static char s_wp_games[WALLPAPER_GAME_MAX][128];
static uint8_t s_wp_game_engine[WALLPAPER_GAME_MAX];
static int s_wp_game_count = -1;            /* -1 = 未从文件加载 */

static void wallpaper_games_load(void) {
    if (s_wp_game_count >= 0) return;
    s_wp_game_count = 0;
    FILE *f = fopen(WALLPAPER_GAME_CFG, "r");
    if (!f) return;
    char line[192];
    while (s_wp_game_count < WALLPAPER_GAME_MAX && fgets(line, sizeof(line), f)) {
        char *pipe = strchr(line, '|');
        if (!pipe) continue;
        *pipe = '\0';
        int eng = atoi(line);
        char *p = pipe + 1;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = '\0';
        if (eng < 0 || eng > 4 || n == 0 || n >= 128) continue;
        s_wp_game_engine[s_wp_game_count] = (uint8_t)eng;
        memcpy(s_wp_games[s_wp_game_count], p, n + 1);
        s_wp_game_count++;
    }
    fclose(f);
    ESP_LOGI(TAG, "游戏壁纸列表: %d 个", s_wp_game_count);
}

static void wallpaper_games_save(void) {
    FILE *f = fopen(WALLPAPER_GAME_CFG, "w");
    if (!f) return;
    for (int i = 0; i < s_wp_game_count; i++)
        fprintf(f, "%d|%s\n", s_wp_game_engine[i], s_wp_games[i]);
    fclose(f);
}

static bool wallpaper_game_add(int engine, const char *path) {
    wallpaper_games_load();
    for (int i = 0; i < s_wp_game_count; i++)
        if (s_wp_game_engine[i] == engine && strcmp(s_wp_games[i], path) == 0)
            return false;   /* 已存在 */
    if (s_wp_game_count >= WALLPAPER_GAME_MAX) return false;
    s_wp_game_engine[s_wp_game_count] = (uint8_t)engine;
    snprintf(s_wp_games[s_wp_game_count], 128, "%.127s", path);
    s_wp_game_count++;
    wallpaper_games_save();
    return true;
}

static bool wallpaper_game_remove(int idx) {
    wallpaper_games_load();
    if (idx < 0 || idx >= s_wp_game_count) return false;
    for (int i = idx; i < s_wp_game_count - 1; i++) {
        s_wp_game_engine[i] = s_wp_game_engine[i + 1];
        memcpy(s_wp_games[i], s_wp_games[i + 1], 128);
    }
    s_wp_game_count--;
    wallpaper_games_save();
    return true;
}

/* 启动游戏壁纸 (定义在 game_run_loop 之后) */
static void start_wallpaper_game(menu_state_t *state, int engine, const char *path);
/* 前向声明 (定义在本文件靠后位置) */
static void screensaver_render_stars(st7305_handle_t *lcd, uint32_t now_ms);
static const char *platform_root_dir(int engine);
static void book_pad_name(char *buf, size_t len, int target, const char *name);
static void menu_config_save(void);
static void list_dialog_open(menu_state_t *state, const char *title, int count,
                             void (*on_select)(menu_state_t *, int));
static bool list_dialog_pop_parent(menu_state_t *state);
static bool s_wallpaper_game_mode = false;  /* 游戏壁纸: 任意按键强退 */

/* ============ V1.0.67: 天气时钟壁纸 (时间 + 联网天气) ============ */
static float s_weather_temp = -999.0f;   /* 无效值表示未获取 */
static int   s_weather_code = -1;
static uint32_t s_weather_fetch_ms = 0;
static bool s_weather_fetching = false;

static const char *weather_name(int code) {
    if (code < 0) return "";
    if (code == 0) return "\xe6\x99\xb4";                /* 晴 */
    if (code <= 3) return "\xe5\xa4\x9a\xe4\xba\x91";    /* 多云 */
    if (code <= 48) return "\xe9\x9b\xbe";               /* 雾 */
    if (code <= 67) return "\xe9\x9b\xa8";               /* 雨 */
    if (code <= 77) return "\xe9\x9b\xaa";               /* 雪 */
    if (code <= 82) return "\xe9\x98\xb5\xe9\x9b\xa8";   /* 阵雨 */
    if (code <= 86) return "\xe9\x9b\xaa";               /* 雪 */
    return "\xe9\x9b\xb7\xe9\x9b\xa8";                   /* 雷雨 */
}

/* Open-Meteo 免 key 接口, 手动解析 temperature/weathercode */
static void weather_http_fetch(void) {
    const char *url = "https://api.open-meteo.com/v1/forecast?latitude=39.90&longitude=116.40&current_weather=true";
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 8000, .buffer_size = 1024,
        /* V1.0.68: HTTPS 必须带证书校验 (esp_crt_bundle), 之前无证书 HTTPS 必然失败 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(client);
        if (len > 0) {
            char *buf = malloc((size_t)len + 1);
            if (buf) {
                int r = esp_http_client_read_response(client, buf, len);
                if (r > 0) {
                    buf[r] = '\0';
                    char *p = strstr(buf, "\"temperature\":");
                    if (p) s_weather_temp = (float)atof(p + strlen("\"temperature\":"));
                    p = strstr(buf, "\"weathercode\":");
                    if (p) s_weather_code = atoi(p + strlen("\"weathercode\":"));
                    ESP_LOGI(TAG, "天气: %.1f°C code=%d", (double)s_weather_temp, s_weather_code);
                }
                free(buf);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void weather_fetch_once(void *arg) {
    (void)arg;
    if (wifi_manager_is_connected()) {
        weather_http_fetch();
    }
    s_weather_fetching = false;
    vTaskDelete(NULL);
}

static void weather_maybe_fetch(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool stale = (s_weather_temp < -900) || (now - s_weather_fetch_ms > 30 * 60 * 1000);
    if (!s_weather_fetching && stale && wifi_manager_is_connected()) {
        s_weather_fetching = true;
        s_weather_fetch_ms = now;
        /* esp_http_client + TLS(mbedtls) + 证书校验调用链很深, 用 32KB 栈防溢出 */
        xTaskCreate(weather_fetch_once, "wxfetch", 32768, NULL, 1, NULL);
    }
}

/* 7 段数码管一位数字 (宽 sw, 高 sh, 段厚 th) */
static void draw_seg7(st7305_handle_t *lcd, int x0, int y0, int digit, int th) {
    static const uint8_t SEG[10] = {
        0b00111111, 0b00000110, 0b01011011, 0b01001111, 0b01100110,
        0b01101101, 0b01111101, 0b00000111, 0b01111111, 0b01101111,
    };
    if (digit < 0) digit = 0;
    if (digit > 9) digit = 9;
    uint8_t m = SEG[digit];
    const int sw = th * 5, sh = th * 11;
    /* a 顶横 */ if (m & 0x01) fill_rect(lcd, x0 + th, y0, x0 + sw - th, y0 + th - 1, ST7305_COLOR_BLACK);
    /* b 右上竖 */ if (m & 0x02) fill_rect(lcd, x0 + sw - th, y0 + th, x0 + sw - 1, y0 + sh / 2 - 1, ST7305_COLOR_BLACK);
    /* c 右下竖 */ if (m & 0x04) fill_rect(lcd, x0 + sw - th, y0 + sh / 2 + th, x0 + sw - 1, y0 + sh - th - 1, ST7305_COLOR_BLACK);
    /* d 底横 */ if (m & 0x08) fill_rect(lcd, x0 + th, y0 + sh - th, x0 + sw - th, y0 + sh - 1, ST7305_COLOR_BLACK);
    /* e 左下竖 */ if (m & 0x10) fill_rect(lcd, x0, y0 + sh / 2 + th, x0 + th - 1, y0 + sh - th - 1, ST7305_COLOR_BLACK);
    /* f 左上竖 */ if (m & 0x20) fill_rect(lcd, x0, y0 + th, x0 + th - 1, y0 + sh / 2 - 1, ST7305_COLOR_BLACK);
    /* g 中横 */ if (m & 0x40) fill_rect(lcd, x0 + th, y0 + sh / 2 - th / 2, x0 + sw - th, y0 + sh / 2 + th / 2 - 1, ST7305_COLOR_BLACK);
}

static void weather_clock_render(st7305_handle_t *lcd, uint32_t now_ms) {
    (void)now_ms;
    weather_maybe_fetch();
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* 日期 */
    static const char *wd[] = {"\xe6\x97\xa5","\xe4\xb8\x80","\xe4\xba\x8c","\xe4\xb8\x89",
                               "\xe5\x9b\x9b","\xe4\xba\x94","\xe5\x85\xad"};
    char buf[64];
    snprintf(buf, sizeof(buf), "%d\xe6\x9c\x88%d\xe6\x97\xa5 \xe6\x98\x9f\xe6\x9c\x9f%s",
             t->tm_mon + 1, t->tm_mday, wd[t->tm_wday]);
    draw_text_centered(lcd, 30, buf, false);

    /* 时间: HH:MM 大号 7 段数码管 (两位+冒号+两位), 每字 sw=40(sh=88, th=8), 居中 */
    const int th = 8, sw = th * 5, sh = th * 11;
    int total_w = sw * 4 + th * 4;   /* 4 字 + 3 个冒号间距 */
    int cx = (SCREEN_W - total_w) / 2;
    int cy = 60;
    int h1 = t->tm_hour / 10, h2 = t->tm_hour % 10;
    int m1 = t->tm_min / 10, m2 = t->tm_min % 10;
    draw_seg7(lcd, cx, cy, h1, th);           cx += sw + th;
    draw_seg7(lcd, cx, cy, h2, th);           cx += sw + th;
    /* 冒号两个点 */
    fill_rect(lcd, cx, cy + sh / 4, cx + th - 1, cy + sh / 4 + th - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, cx, cy + sh * 3 / 4, cx + th - 1, cy + sh * 3 / 4 + th - 1, ST7305_COLOR_BLACK);
    cx += th * 2;
    draw_seg7(lcd, cx, cy, m1, th);           cx += sw + th;
    draw_seg7(lcd, cx, cy, m2, th);

    /* 天气/温度 */
    if (s_weather_temp > -900.0f) {
        snprintf(buf, sizeof(buf), "%.0f\xc2\xb0C %s", (double)s_weather_temp, weather_name(s_weather_code));
    } else {
        snprintf(buf, sizeof(buf), "--\xc2\xb0C %s", wifi_manager_is_connected() ? "\xe5\x8a\xa0\xe8\xbd\xbd\xe4\xb8\xad" : "\xe6\x9c\xaa\xe8\x81\x94\xe7\xbd\x91");
        /* 加载中 / 未联网 */
    }
    draw_text_centered(lcd, 190, buf, false);
}

/* 渲染当前选中的内置壁纸程序 (0=星空, 8=天气时钟, 其余旧程序) */
static void screensaver_render_builtin(st7305_handle_t *lcd, uint32_t now_ms) {
    extern menu_state_t g_menu;
    if (g_menu.wallpaper_program == WP_PROG_STARS)
        screensaver_render_stars(lcd, now_ms);
    else if (g_menu.wallpaper_program == WP_PROG_WEATHER)
        weather_clock_render(lcd, now_ms);
    else
        wp_program_render(lcd, (int)g_menu.wallpaper_program, now_ms);
}

/* ==== TF 动态图 (BMP) ==== */
static int wp_bmp_scan(void) {
    if (sd_mount() != ESP_OK) return 0;
    int n = 0;
    DIR *dir = opendir(WALLPAPER_BMP_DIR);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < WALLPAPER_BMP_MAX) {
        if (ent->d_name[0] == '.') continue;
        size_t l = strlen(ent->d_name);
        if (l < 5 || strcasecmp(ent->d_name + l - 4, ".bmp") != 0) continue;
        snprintf(s_wp_bmp_files[n], 64, "%.63s", ent->d_name);
        n++;
    }
    closedir(dir);
    /* 按文件名排序 */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && strcasecmp(s_wp_bmp_files[j - 1], s_wp_bmp_files[j]) > 0; j--) {
            char t[64];
            memcpy(t, s_wp_bmp_files[j - 1], 64);
            memcpy(s_wp_bmp_files[j - 1], s_wp_bmp_files[j], 64);
            memcpy(s_wp_bmp_files[j], t, 64);
        }
    }
    return n;
}

/* 解码 BMP (1/8/24bpp, 支持上下颠倒) 到 400x300 1bpp 行序位图 (位=1 黑), 自动缩放 */
static bool wp_bmp_decode(const char *path, uint8_t *fb) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return false; }
    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11] << 8) |
                        ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
    int32_t w = (int32_t)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24));
    int32_t h = (int32_t)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    uint16_t bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    bool topdown = (h < 0);
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        (bpp != 1 && bpp != 8 && bpp != 24)) { fclose(f); return false; }
    uint8_t pal[1024];
    if (bpp == 8 || bpp == 1) {
        size_t pl = (bpp == 8) ? 1024 : 8;
        fseek(f, 54, SEEK_SET);
        if (fread(pal, 1, pl, f) != pl) { fclose(f); return false; }
    }
    size_t row_bytes = (((size_t)w * bpp + 31) / 32) * 4;
    uint8_t *row = malloc(row_bytes ? row_bytes : 4);
    if (!row) { fclose(f); return false; }
    fseek(f, (long)data_off, SEEK_SET);
    memset(fb, 0, (ST7305_WIDTH * ST7305_HEIGHT + 7) / 8);
    for (int32_t y = 0; y < h; y++) {
        size_t got = fread(row, 1, row_bytes, f);
        if (got < (size_t)((w * bpp + 7) / 8)) break;
        int32_t src_y = topdown ? y : (h - 1 - y);
        int dst_y = (src_y * ST7305_HEIGHT) / h;
        if (dst_y < 0 || dst_y >= ST7305_HEIGHT) continue;
        for (int dst_x = 0; dst_x < ST7305_WIDTH; dst_x++) {
            int src_x = (dst_x * w) / ST7305_WIDTH;
            int lum;
            if (bpp == 24) {
                int bi = src_x * 3;
                lum = (row[bi + 2] * 299 + row[bi + 1] * 587 + row[bi] * 114) / 1000;
            } else if (bpp == 8) {
                uint8_t pi = row[src_x];
                lum = (pal[pi * 4 + 2] * 299 + pal[pi * 4 + 1] * 587 + pal[pi * 4] * 114) / 1000;
            } else {
                int pi = (row[src_x / 8] >> (7 - (src_x % 8))) & 1;
                lum = (pal[pi * 4 + 2] * 299 + pal[pi * 4 + 1] * 587 + pal[pi * 4] * 114) / 1000;
            }
            if (lum < 128)
                fb[dst_y * ((ST7305_WIDTH + 7) / 8) + (dst_x / 8)] |= (uint8_t)(1 << (7 - (dst_x % 8)));
        }
    }
    free(row);
    fclose(f);
    return true;
}

/* 渲染当前 BMP 帧 (按时序切换), 无文件时回退星空 */
static void screensaver_render_bmp(st7305_handle_t *lcd, uint32_t now_ms) {
    extern menu_state_t g_menu;
    static const uint32_t interval_tab[3] = { 200, 120, 80 };  /* 慢/标准/快 */
    uint8_t spd = (g_menu.wallpaper_bmp_fps <= 2) ? g_menu.wallpaper_bmp_fps : 1;
    uint32_t interval_ms = interval_tab[spd];
    if (!s_wp_frame) {
        s_wp_frame = heap_caps_malloc((ST7305_WIDTH * ST7305_HEIGHT + 7) / 8,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wp_frame) { screensaver_render_builtin(lcd, now_ms); return; }
    }
    if (s_wp_bmp_count == 0) {
        s_wp_bmp_count = wp_bmp_scan();
        s_wp_bmp_index = 0;
    }
    if (s_wp_bmp_count == 0) { screensaver_render_builtin(lcd, now_ms); return; }
    if (s_wp_bmp_index >= s_wp_bmp_count) s_wp_bmp_index = 0;
    if (now_ms - s_wp_bmp_last_ms >= interval_ms || s_wp_bmp_last_ms == 0) {
        s_wp_bmp_last_ms = now_ms;
        char path[96];
        snprintf(path, sizeof(path), WALLPAPER_BMP_DIR "/%s", s_wp_bmp_files[s_wp_bmp_index]);
        if (wp_bmp_decode(path, s_wp_frame)) {
            st7305_clear(lcd, ST7305_COLOR_WHITE);
            st7305_blit_1bit(lcd, 0, 0, ST7305_WIDTH, ST7305_HEIGHT, s_wp_frame);
            st7305_flush(lcd);
        }
        s_wp_bmp_index++;
    }
}

/* ==== 游戏壁纸选择器 ==== */
#define WP_PICKER_MAX 96
static int s_wp_picker_engine = -1;          /* -1 = 引擎选择层 */
#define WP_PICKER_SHOW 15   /* list_dialog_items 只有 16 行 (含"返回") */
EXT_RAM_BSS_ATTR static char s_wp_picker_paths[WP_PICKER_MAX][160];
EXT_RAM_BSS_ATTR static char s_wp_picker_names[WP_PICKER_MAX][64];
EXT_RAM_BSS_ATTR static uint8_t s_wp_picker_is_dir[WP_PICKER_MAX];
static char s_wp_picker_path[160];           /* 当前分类目录 (相对引擎根, 空=根目录) */
static int s_wp_picker_count = 0;

static bool wp_ext_match(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n <= e) return false;
    return strcasecmp(name + n - e, ext) == 0;
}

static void wp_picker_add_file(const char *dir, const char *name) {
    if (s_wp_picker_count >= WP_PICKER_MAX) return;
    snprintf(s_wp_picker_paths[s_wp_picker_count], 160, "%.110s/%.48s", dir, name);
    snprintf(s_wp_picker_names[s_wp_picker_count], 64, "%.63s", name);
    char *dot = strrchr(s_wp_picker_names[s_wp_picker_count], '.');
    if (dot) *dot = '\0';
    s_wp_picker_is_dir[s_wp_picker_count] = 0;
    s_wp_picker_count++;
}

static void wp_picker_add_dir(const char *rel) {
    if (s_wp_picker_count >= WP_PICKER_MAX) return;
    snprintf(s_wp_picker_paths[s_wp_picker_count], 160, "%.159s", rel);
    const char *base = strrchr(rel, '/');
    snprintf(s_wp_picker_names[s_wp_picker_count], 64, "%.62s/", base ? base + 1 : rel);
    s_wp_picker_is_dir[s_wp_picker_count] = 1;
    s_wp_picker_count++;
}

/* 扫描当前目录: 分类文件夹在前, 游戏在后, 带 ".. (上级)" */
static void wp_picker_scan(int engine) {
    s_wp_picker_count = 0;
    if (sd_mount() != ESP_OK) return;
    const char *exts[4] = { ".gb", ".gbc", ".nes", ".hex" };
    const char *root = (engine < 4) ? platform_root_dir(engine) : "/sdcard/gam";
    const char *ext = (engine < 4) ? exts[engine] : ".gam";
    char full[300];
    if (s_wp_picker_path[0])
        snprintf(full, sizeof(full), "%s/%.130s", root, s_wp_picker_path);
    else
        snprintf(full, sizeof(full), "%s", root);
    if (s_wp_picker_path[0]) {   /* 上级入口 */
        snprintf(s_wp_picker_names[0], 64, ".. (上级)");
        snprintf(s_wp_picker_paths[0], 160, "%s", s_wp_picker_path);
        s_wp_picker_is_dir[0] = 1;
        s_wp_picker_count = 1;
    }
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_wp_picker_count < WP_PICKER_MAX) {
        if (ent->d_name[0] == '.') continue;
        char fp[300];
        snprintf(fp, sizeof(fp), "%.200s/%.50s", full, ent->d_name);
        struct stat st;
        if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) {
            char rel[160];
            if (s_wp_picker_path[0])
                snprintf(rel, sizeof(rel), "%.110s/%.48s", s_wp_picker_path, ent->d_name);
            else
                snprintf(rel, sizeof(rel), "%.120s", ent->d_name);
            wp_picker_add_dir(rel);
        } else if (wp_ext_match(ent->d_name, ext)) {
            wp_picker_add_file(full, ent->d_name);
        }
    }
    closedir(d);
    /* 稳定排序: 目录在前 (不含 ".."), 游戏在后, 各自按字母序 */
    for (int i = 1; i < s_wp_picker_count; i++) {
        for (int j = i; j > 1; j--) {
            bool a_dir = s_wp_picker_is_dir[j - 1], b_dir = s_wp_picker_is_dir[j];
            int key = (a_dir == b_dir)
                ? strcasecmp(s_wp_picker_names[j - 1], s_wp_picker_names[j])
                : (a_dir ? -1 : 1);
            if (key <= 0) break;
            char t[64], tp[160];
            uint8_t ti;
            memcpy(t, s_wp_picker_names[j - 1], 64);
            memcpy(s_wp_picker_names[j - 1], s_wp_picker_names[j], 64);
            memcpy(s_wp_picker_names[j], t, 64);
            memcpy(tp, s_wp_picker_paths[j - 1], 160);
            memcpy(s_wp_picker_paths[j - 1], s_wp_picker_paths[j], 160);
            memcpy(s_wp_picker_paths[j], tp, 160);
            ti = s_wp_picker_is_dir[j - 1];
            s_wp_picker_is_dir[j - 1] = s_wp_picker_is_dir[j];
            s_wp_picker_is_dir[j] = ti;
        }
    }
}

static bool wp_picker_path_added(int idx) {
    wallpaper_games_load();
    for (int i = 0; i < s_wp_game_count; i++)
        if (strcmp(s_wp_games[i], s_wp_picker_paths[idx]) == 0) return true;
    return false;
}

static void wp_picker_toggle(int idx) {
    if (idx < 0 || idx >= s_wp_picker_count) return;
    wallpaper_games_load();
    for (int i = 0; i < s_wp_game_count; i++) {
        if (strcmp(s_wp_games[i], s_wp_picker_paths[idx]) == 0) {
            wallpaper_game_remove(i);
            snprintf(g_menu.hint_text, sizeof(g_menu.hint_text), "已取消游戏壁纸");
            g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 800;
            g_menu.needs_redraw = true;
            return;
        }
    }
    if (wallpaper_game_add(s_wp_picker_engine, s_wp_picker_paths[idx])) {
        snprintf(g_menu.hint_text, sizeof(g_menu.hint_text), "已添加游戏壁纸");
    } else {
        snprintf(g_menu.hint_text, sizeof(g_menu.hint_text), "列表已满或重复");
    }
    g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 800;
    g_menu.needs_redraw = true;
}

static void wp_picker_rebuild(menu_state_t *state) {
    const char *base = strrchr(s_wp_picker_path, '/');
    const char *disp = base ? base + 1 : s_wp_picker_path;
    snprintf(state->list_dialog_title, sizeof(state->list_dialog_title),
             "选择游戏: %.16s", disp[0] ? disp : "根目录");
    /* 20260812: 移除 15 个上限, 全量填入列表, 由弹窗滚动条滚动 (数组已扩到 100) */
    for (int i = 0; i < s_wp_picker_count; i++) {
        if (s_wp_picker_is_dir[i])
            snprintf(state->list_dialog_items[i], 64, "%s", s_wp_picker_names[i]);
        else if (wp_picker_path_added(i))
            snprintf(state->list_dialog_items[i], 64, "[W] %.55s", s_wp_picker_names[i]);
        else
            snprintf(state->list_dialog_items[i], 64, "%.63s", s_wp_picker_names[i]);
    }
    snprintf(state->list_dialog_items[s_wp_picker_count], 64, "返回");
    state->list_dialog_count = s_wp_picker_count + 1;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    state->needs_redraw = true;
}

/* 游戏壁纸: 选择器 on_select (idx 为游戏序号, 最后一项"返回"由通用分支处理) */
static void wp_picker_on_select(menu_state_t *state, int idx) {
    if (idx < 0 || idx >= s_wp_picker_count) return;
    if (s_wp_picker_is_dir[idx]) {
        if (idx == 0 && s_wp_picker_path[0] != '\0') {   /* ".. (上级)" */
            char *slash = strrchr(s_wp_picker_path, '/');
            if (slash) *slash = '\0';
            else s_wp_picker_path[0] = '\0';
        } else {
            snprintf(s_wp_picker_path, sizeof(s_wp_picker_path), "%.159s", s_wp_picker_paths[idx]);
        }
        wp_picker_scan(s_wp_picker_engine);
        wp_picker_rebuild(state);
        state->list_dialog_selected = 0;
        state->list_dialog_scroll = 0;
        state->needs_redraw = true;
        return;
    }
    wp_picker_toggle(idx);
    wp_picker_rebuild(state);
}

static void wp_engine_dialog_on_select(menu_state_t *state, int idx);
static void open_wallpaper_game_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    s_wp_picker_engine = -1;
    list_dialog_open(state, "游戏壁纸", 6, wp_engine_dialog_on_select);
    snprintf(state->list_dialog_items[0], 64, "GB");
    snprintf(state->list_dialog_items[1], 64, "GBC");
    snprintf(state->list_dialog_items[2], 64, "NES");
    snprintf(state->list_dialog_items[3], 64, "arduboy");
    snprintf(state->list_dialog_items[4], 64, "步步高");
    snprintf(state->list_dialog_items[5], 64, "返回");
    state->list_dialog_count = 6;
}

static void wp_engine_dialog_on_select(menu_state_t *state, int idx) {
    if (idx < 0 || idx > 4) return;
    s_wp_picker_engine = idx;
    s_wp_picker_path[0] = '\0';
    wp_picker_scan(idx);
    /* 嵌套弹窗: 自动压入引擎列表为父弹窗 */
    list_dialog_open(state, "选择游戏 (确认/多功能键添加)", 0, wp_picker_on_select);
    wp_picker_rebuild(state);
}

/* ==== 壁纸设置弹窗 (手柄/存储同款 list_dialog) ==== */
static void wallpaper_dialog_on_select(menu_state_t *state, int idx);
static void wallpaper_type_dialog_on_select(menu_state_t *state, int idx);
static void wallpaper_prog_dialog_on_select(menu_state_t *state, int idx);
static void wallpaper_speed_dialog_on_select(menu_state_t *state, int idx);
static void wallpaper_min_dialog_on_select(menu_state_t *state, int idx);

/* 重建壁纸设置弹窗内容 (值变化后调用, 强制全量重绘) */
static void wallpaper_dialog_rebuild(menu_state_t *state) {
    snprintf(state->list_dialog_items[0], 64, "壁纸类型");
    snprintf(state->list_dialog_items[1], 64, "休眠时间");
    snprintf(state->list_dialog_items[2], 64, "测试壁纸");
    snprintf(state->list_dialog_items[3], 64, "返回");
    state->list_dialog_count = 4;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    state->needs_redraw = true;
}

static void open_wallpaper_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    list_dialog_open(state, "壁纸设置", 4, wallpaper_dialog_on_select);
    wallpaper_dialog_rebuild(state);
}

/* 壁纸设置弹窗: 壁纸类型 / 休眠时间 / 测试壁纸 / 返回 */
static void wallpaper_dialog_on_select(menu_state_t *state, int idx) {
    switch (idx) {
    case 0:  /* 壁纸类型 → 子弹窗 */
        list_dialog_open(state, "壁纸类型", 4, wallpaper_type_dialog_on_select);
        snprintf(state->list_dialog_items[0], 64, "%s%s",
                 state->wallpaper_mode == WALLPAPER_MODE_STARS ? "* " : "", "内置壁纸");
        snprintf(state->list_dialog_items[1], 64, "%s%s",
                 state->wallpaper_mode == WALLPAPER_MODE_BMP ? "* " : "", "TF动态图");
        snprintf(state->list_dialog_items[2], 64, "%s%s",
                 state->wallpaper_mode == WALLPAPER_MODE_GAME ? "* " : "", "游戏壁纸");
        snprintf(state->list_dialog_items[3], 64, "返回");
        state->list_dialog_count = 4;
        return;
    case 1: {  /* 休眠时间 → 子弹窗 */
        static const int mins[] = { 1, 2, 3, 5, 10, 15, 20, 30 };
        int cnt = (int)(sizeof(mins) / sizeof(mins[0]));
        list_dialog_open(state, "休眠时间", cnt + 1, wallpaper_min_dialog_on_select);
        for (int i = 0; i < cnt; i++) {
            bool cur = (state->wallpaper_timeout_min == (uint8_t)mins[i]);
            snprintf(state->list_dialog_items[i], 64, "%s%d分钟",
                     cur ? "* " : "", mins[i]);
        }
        snprintf(state->list_dialog_items[cnt], 64, "返回");
        state->list_dialog_count = cnt + 1;
        return;
    }
    case 2:  /* 测试壁纸: 立即进入 */
        s_screensaver_preview = true;
        s_screensaver_test = true;
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_stack_top = 0;
        state->needs_redraw = true;
        return;
    default:
        return;
    }
}

/* 壁纸类型子弹窗: 选中即设模式, 并按类型进入对应子窗口 */
static void wallpaper_type_dialog_on_select(menu_state_t *state, int idx) {
    switch (idx) {
    case 0: {  /* 内置壁纸 → 程序选择 (V1.0.67: 只保留星空) */
        static const int progs[] = { WP_PROG_STARS, WP_PROG_WEATHER };
        int cnt = (int)(sizeof(progs) / sizeof(progs[0]));
        list_dialog_open(state, "选择壁纸程序", cnt + 1, wallpaper_prog_dialog_on_select);
        for (int i = 0; i < cnt; i++) {
            snprintf(state->list_dialog_items[i], 64, "%s%s",
                     state->wallpaper_program == progs[i] ? "* " : "", wp_prog_name(progs[i]));
        }
        snprintf(state->list_dialog_items[cnt], 64, "返回");
        state->list_dialog_count = cnt + 1;
        return;
    }
    case 1: {  /* TF动态图 → 播放速度子窗口 */
        state->wallpaper_mode = WALLPAPER_MODE_BMP;
        menu_config_save();
        list_dialog_open(state, "TF动态图 播放速度", 4, wallpaper_speed_dialog_on_select);
        snprintf(state->list_dialog_items[0], 64, "慢 (5帧/秒)");
        snprintf(state->list_dialog_items[1], 64, "标准 (8帧/秒)");
        snprintf(state->list_dialog_items[2], 64, "快 (12帧/秒)");
        snprintf(state->list_dialog_items[3], 64, "返回");
        state->list_dialog_count = 4;
        return;
    }
    case 2:  /* 游戏壁纸 → 直接进游戏选择器 */
        state->wallpaper_mode = WALLPAPER_MODE_GAME;
        menu_config_save();
        open_wallpaper_game_dialog(state);
        return;
    default:
        return;
    }
}

static void wallpaper_prog_dialog_on_select(menu_state_t *state, int idx) {
    static const int progs[] = { WP_PROG_STARS, WP_PROG_WEATHER };
    int cnt = (int)(sizeof(progs) / sizeof(progs[0]));
    if (idx < 0 || idx >= cnt) return;
    state->wallpaper_mode = WALLPAPER_MODE_STARS;
    state->wallpaper_program = (uint8_t)progs[idx];
    menu_config_save();
    list_dialog_pop_parent(state);   /* 程序 → 壁纸类型 */
    list_dialog_pop_parent(state);   /* 壁纸类型 → 壁纸设置 */
    wallpaper_dialog_rebuild(state);
}

static void wallpaper_speed_dialog_on_select(menu_state_t *state, int idx) {
    if (idx < 0 || idx > 2) return;
    state->wallpaper_bmp_fps = (uint8_t)idx;
    menu_config_save();
    list_dialog_pop_parent(state);       /* 回到壁纸类型 */
    list_dialog_pop_parent(state);       /* 回到壁纸设置 */
    wallpaper_dialog_rebuild(state);
}

static void wallpaper_min_dialog_on_select(menu_state_t *state, int idx) {
    static const int mins[] = { 1, 2, 3, 5, 10, 15, 20, 30 };
    if (idx < 0 || idx >= (int)(sizeof(mins) / sizeof(mins[0]))) return;
    state->wallpaper_timeout_min = (uint8_t)mins[idx];
    menu_config_save();
    list_dialog_pop_parent(state);
    wallpaper_dialog_rebuild(state);
}

#if 0  /* 旧全屏子页 (已改为弹窗) */
static int wallpaper_build(menu_state_t *state, char buf[][64], int max) {
    wallpaper_games_load();
    char tmp[24];
    int n = 0;
    book_pad_name(tmp, sizeof(tmp), 5, "壁纸类型");
    snprintf(buf[n++], 64, "%s %s", tmp, wallpaper_mode_name(state->wallpaper_mode));
    book_pad_name(tmp, sizeof(tmp), 5, "游戏壁纸");
    snprintf(buf[n++], 64, "%s (%d)", tmp, s_wp_game_count > 0 ? s_wp_game_count : 0);
    book_pad_name(tmp, sizeof(tmp), 5, "休眠时间");
    snprintf(buf[n++], 64, "%s %d分钟", tmp, state->wallpaper_timeout_min);
    book_pad_name(tmp, sizeof(tmp), 5, "测试壁纸");
    snprintf(buf[n++], 64, "%s 立即进入", tmp);
    return n;
}

static bool wallpaper_on_confirm(menu_state_t *state, int idx) {
    switch (idx) {
    case 0:
        state->wallpaper_mode = (uint8_t)((state->wallpaper_mode + 1) % 3);
        menu_config_save();
        state->needs_redraw = true;
        return true;
    case 1:
        open_wallpaper_game_dialog(state);
        return true;
    case 2:
        state->wallpaper_timeout_min = (state->wallpaper_timeout_min >= 30) ? 1
                                     : (uint8_t)(state->wallpaper_timeout_min + 1);
        menu_config_save();
        state->needs_redraw = true;
        return true;
    case 3:
        /* 测试壁纸: 立即进入壁纸状态 */
        s_screensaver_preview = true;
        s_screensaver_test = true;
        state->needs_redraw = true;
        return true;
    default:
        return false;
    }
}
#endif

/* ============ 番茄钟 (V1.0.64) ============ */
static bool     s_pomo_active = false;
static bool     s_pomo_work = true;         /* true=工作 false=休息 */
static uint32_t s_pomo_end_ms = 0;          /* 本阶段结束时刻 (esp_timer ms) */
static uint32_t s_pomo_total_ms = 0;
static bool     s_pomo_edit = false;        /* 弹窗编辑模式: 数值闪烁 + 上下调值 */
static bool     s_pomo_time_is_work = false;/* V1.0.68: 时间列表弹窗当前编辑的是工作时间还是休息时间 */

/* === 完成提醒音: 电子合成和弦 (C5 E5 G5 C6) ===
 * 正弦 + 八度泛音, 指数衰减, 22050Hz 立体声, 主循环逐帧喂 PCM. */
#define POMO_SR        22050
#define POMO_RING_LEN  22050
#define POMO_NOTE_N    4
static bool     s_pomo_ring = false;
static uint32_t s_pomo_ring_pos = 0;
static const float POMO_NOTES[POMO_NOTE_N] = { 523.25f, 659.25f, 783.99f, 1046.50f };
static const float POMO_NOTE_LEN[POMO_NOTE_N] = { 0.16f, 0.16f, 0.16f, 0.55f };

static void pomodoro_ring_start(void) {
    extern menu_state_t g_menu;
    if (!g_menu.pomo_reminder) return;
    s_pomo_ring = true;
    s_pomo_ring_pos = 0;
}

static void pomodoro_ring_update(void) {
    if (!s_pomo_ring) return;
    int16_t buf[512 * 2];
    int guard = 0;
    while (s_pomo_ring && guard < 8) {
        uint32_t chunk = 512;
        if (s_pomo_ring_pos + chunk > POMO_RING_LEN)
            chunk = POMO_RING_LEN - s_pomo_ring_pos;
        uint32_t t0 = s_pomo_ring_pos;
        for (uint32_t i = 0; i < chunk; i++) {
            float tt = (float)(t0 + i) / POMO_SR;
            float acc = 0.0f;
            int note = -1;
            for (int k = 0; k < POMO_NOTE_N; k++) {
                if (tt < acc + POMO_NOTE_LEN[k]) { note = k; break; }
                acc += POMO_NOTE_LEN[k];
            }
            int16_t v = 0;
            if (note >= 0) {
                float lt = tt - acc;
                float f = POMO_NOTES[note];
                float env = expf(-lt * 5.0f / POMO_NOTE_LEN[note]);
                float s = (sinf(6.2831853f * f * tt) + 0.35f * sinf(12.56637f * f * tt)) * env;
                v = (int16_t)(s * 12000.0f);
            }
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        size_t wr = audio_player_feed_pcm(buf, chunk, POMO_SR);
        s_pomo_ring_pos += (uint32_t)wr;
        if (wr < chunk) break;                 /* 环形缓冲满, 下帧继续 */
        if (s_pomo_ring_pos >= POMO_RING_LEN) {
            s_pomo_ring = false;
            audio_player_stop();               /* 释放 PCM 输出任务 */
        }
        guard++;
    }
}

static void pomodoro_start(menu_state_t *state) {
    s_pomo_work = true;
    s_pomo_total_ms = (uint32_t)state->pomo_work_min * 60 * 1000;
    s_pomo_end_ms = (uint32_t)(esp_timer_get_time() / 1000) + s_pomo_total_ms;
    s_pomo_active = true;
}

static void pomodoro_stop(menu_state_t *state) {
    s_pomo_active = false;
    s_pomo_ring = false;
    audio_player_flush_pcm();
    state->needs_redraw = true;
}

/* 阶段切换 (工作↔休息 循环) */
static void pomodoro_tick(void) {
    if (!s_pomo_active) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(s_pomo_end_ms - now) > 0) return;
    extern menu_state_t g_menu;
    s_pomo_work = !s_pomo_work;
    s_pomo_total_ms = (uint32_t)((s_pomo_work ? g_menu.pomo_work_min : g_menu.pomo_rest_min) * 60 * 1000);
    s_pomo_end_ms = now + s_pomo_total_ms;
    ESP_LOGI(TAG, "番茄钟切换到 %s", s_pomo_work ? "工作" : "休息");
    pomodoro_ring_start();
}

static void pomodoro_render(st7305_handle_t *lcd) {
    if (!lcd) return;
    pomodoro_tick();
    pomodoro_ring_update();
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int64_t remain_ms = (int64_t)s_pomo_end_ms - (int64_t)now;
    if (remain_ms < 0) remain_ms = 0;
    int64_t remain_s = (remain_ms + 999) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d分钟", s_pomo_work ? "工作中" : "休息中",
             (int)((s_pomo_total_ms / 60000)));
    draw_text_centered(lcd, 70, buf, false);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(remain_s / 60), (int)(remain_s % 60));
    draw_text_centered(lcd, 130, buf, false);
    /* 进度条 */
    int bar_x = 50, bar_y = 200, bar_w = 300, bar_h = 16;
    for (int yy = 0; yy < bar_h; yy++) {
        st7305_draw_pixel(lcd, bar_x, bar_y + yy, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, bar_x + bar_w - 1, bar_y + yy, ST7305_COLOR_BLACK);
    }
    for (int xx = 0; xx < bar_w; xx++) {
        st7305_draw_pixel(lcd, bar_x + xx, bar_y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, bar_x + xx, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    }
    uint32_t done = (s_pomo_total_ms > remain_ms) ? (s_pomo_total_ms - remain_ms) : 0;
    int fill = (int)((bar_w - 4) * (s_pomo_total_ms ? done / (float)s_pomo_total_ms : 0));
    for (int yy = 2; yy < bar_h - 2; yy++)
        for (int xx = 0; xx < fill; xx++)
            st7305_draw_pixel(lcd, bar_x + 2 + xx, bar_y + yy, ST7305_COLOR_BLACK);
    draw_text_centered(lcd, 240, "按任意键返回", false);
    st7305_flush(lcd);
}

static bool pomodoro_is_active(void) { return s_pomo_active; }

static void pomodoro_dialog_rebuild(menu_state_t *state) {
    snprintf(state->list_dialog_items[0], 64, "工作时间: %d分钟", state->pomo_work_min);
    snprintf(state->list_dialog_items[1], 64, "休息时间: %d分钟", state->pomo_rest_min);
    snprintf(state->list_dialog_items[2], 64, "开始番茄钟");
    snprintf(state->list_dialog_items[3], 64, "提醒设置");
    snprintf(state->list_dialog_items[4], 64, "返回");
    state->list_dialog_count = 5;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    state->needs_redraw = true;
}

/* 编辑模式: UP/DOWN 调值; 非编辑: 默认上下移动.
 * V1.0.68: 工作时间/休息时间改为弹出时间列表弹窗, 编辑模式不再使用
 * (s_pomo_edit 恒为 false, 保留分支以防未来复用) */
static void pomodoro_dialog_on_key(menu_state_t *state, int idx, menu_action_t action) {
    if (!s_pomo_edit || idx > 1) {
        if (action == MENU_ACTION_UP || action == MENU_ACTION_LEFT) {
            state->list_dialog_selected = (state->list_dialog_selected > 0)
                ? state->list_dialog_selected - 1 : state->list_dialog_count - 1;
        } else if (action == MENU_ACTION_DOWN || action == MENU_ACTION_RIGHT) {
            state->list_dialog_selected = (state->list_dialog_selected < state->list_dialog_count - 1)
                ? state->list_dialog_selected + 1 : 0;
        } else {
            return;
        }
        state->list_dialog_local_update = true;
        state->needs_redraw = true;
        return;
    }
    if (action == MENU_ACTION_UP) {
        if (idx == 0 && state->pomo_work_min < 120) state->pomo_work_min++;
        if (idx == 1 && state->pomo_rest_min < 60) state->pomo_rest_min++;
    } else if (action == MENU_ACTION_DOWN) {
        if (idx == 0 && state->pomo_work_min > 1) state->pomo_work_min--;
        if (idx == 1 && state->pomo_rest_min > 1) state->pomo_rest_min--;
    } else {
        return;
    }
    menu_config_save();
    pomodoro_dialog_rebuild(state);   /* V1.0.68: 标准渲染需刷新"工作时间: X分钟"文字 */
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
}

/* V1.0.68: 工作时间/休息时间 时间列表弹窗 (5 分钟间隔, 上下拖动/点选).
 * 选中某时间 → 设置并返回番茄钟弹窗. */
static void pomodoro_time_on_select(menu_state_t *state, int idx) {
    int val = 5 + idx * 5;   /* 5 分钟间隔 */
    if (s_pomo_time_is_work) state->pomo_work_min = val;
    else state->pomo_rest_min = val;
    menu_config_save();
    state->needs_redraw = true;
    list_dialog_pop_parent(state);   /* 返回番茄钟弹窗 */
    pomodoro_dialog_rebuild(state);  /* 刷新父弹窗数值显示 */
}

static void open_pomodoro_time_dialog(menu_state_t *state, bool is_work) {
    s_pomo_time_is_work = is_work;
    int max = is_work ? 120 : 60;
    int cur = is_work ? state->pomo_work_min : state->pomo_rest_min;
    int n = max / 5;   /* 5,10,...,max */
    list_dialog_open(state, is_work ? "\xe5\xb7\xa5\xe4\xbd\x9c\xe6\x97\xb6\xe9\x97\xb4"   /* 工作时间 */
                                       : "\xe4\xbc\x91\xe6\x81\xaf\xe6\x97\xb6\xe9\x97\xb4",  /* 休息时间 */
                     n + 1, pomodoro_time_on_select);
    for (int i = 0; i < n; i++) {
        snprintf(state->list_dialog_items[i], 64, "%d\xe5\x88\x86\xe9\x92\x9f", 5 + i * 5);  /* X分钟 */
    }
    snprintf(state->list_dialog_items[n], 64, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 返回 */
    state->list_dialog_count = n + 1;
    /* 预选当前值, 滚动由 draw_list_dialog 的钳制自动定位 */
    int sel = (cur - 5) / 5;
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    state->list_dialog_selected = sel;
    state->list_dialog_scroll = 0;
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
}

static void pomodoro_remind_dialog_on_select(menu_state_t *state, int idx) {
    extern menu_state_t g_menu;
    if (idx != 0) return;
    g_menu.pomo_reminder = !g_menu.pomo_reminder;
    menu_config_save();
    snprintf(state->list_dialog_items[0], 64, "提醒声音: %s",
             g_menu.pomo_reminder ? "开" : "关");
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    state->needs_redraw = true;
}

static void open_pomodoro_remind_dialog(menu_state_t *state) {
    extern menu_state_t g_menu;
    state->list_dialog_return_page = state->current_page;
    list_dialog_open(state, "提醒设置", 2, pomodoro_remind_dialog_on_select);
    snprintf(state->list_dialog_items[0], 64, "提醒声音: %s",
             g_menu.pomo_reminder ? "开" : "关");
    snprintf(state->list_dialog_items[1], 64, "返回");
    state->list_dialog_count = 2;
}

static void pomodoro_dialog_on_select(menu_state_t *state, int idx) {
    switch (idx) {
    case 0:   /* 工作时间: 弹出时间列表 (5 分钟间隔) */
        open_pomodoro_time_dialog(state, true);
        return;
    case 1:   /* 休息时间 */
        open_pomodoro_time_dialog(state, false);
        return;
    case 2:   /* 开始 */
        s_pomo_edit = false;
        pomodoro_start(state);
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_stack_top = 0;
        state->current_page = MENU_PAGE_MAIN;
        state->needs_redraw = true;
        return;
    case 3:   /* 提醒设置 */
        s_pomo_edit = false;
        open_pomodoro_remind_dialog(state);
        return;
    default:
        return;
    }
}

static void open_pomodoro_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    s_pomo_edit = false;
    list_dialog_open(state, "番茄钟", 5, pomodoro_dialog_on_select);
    pomodoro_dialog_rebuild(state);
    /* V1.0.68: 使用标准列表弹窗渲染 (与手柄菜单一致):
     * 返回项固定在底部 + 上方分隔线; 不再用自定义渲染 */
    state->list_dialog_on_render = NULL;
    state->list_dialog_on_key = pomodoro_dialog_on_key;
    state->list_dialog_content_dirty = true;
}

/* 星空屏保数据 */
#define STAR_COUNT 105
EXT_RAM_BSS_ATTR static int s_star_x[STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_star_y[STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_star_z[STAR_COUNT];
EXT_RAM_BSS_ATTR static float s_star_base_speed[STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_star_prev_px[STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_star_prev_py[STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_star_speed_type[STAR_COUNT]; /* 0=慢, 1=中, 2=快 */

/* 背景小星 (不变大, 始终1px, 速度慢) */
#define BG_STAR_COUNT 75
EXT_RAM_BSS_ATTR static int s_bg_star_x[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_bg_star_y[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static int s_bg_star_z[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static float s_bg_star_speed[BG_STAR_COUNT];

/* === 巨型星/星舰已彻底删除 (用户要求) === */

/* 进度条装饰 */
EXT_RAM_BSS_ATTR static float s_progress_decor;  /* 0-100 循环 */

void screensaver_reset(void) {
    screensaver_bt_on();   /* 任何方式退出屏保都恢复蓝牙 (幂等) */
    s_last_input_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_screensaver_active = false;
    s_screensaver_preview = false;
    s_screensaver_test = false;
    for (int i = 0; i < STAR_COUNT; i++) {
        /* 70%星星分布在左右两侧远处 */
        if (rand() % 10 < 7) {
            int side = (rand() % 2) ? 1 : -1;
            s_star_x[i] = side * (100 + (int)(rand() % (int)(SCREEN_W * 1.2f)));
        } else {
            s_star_x[i] = (rand() % SCREEN_W) - SCREEN_W / 2;
        }
        s_star_y[i] = (rand() % (SCREEN_H * 3)) - SCREEN_H * 3 / 2;
        s_star_z[i] = 500 + rand() % 2000;
        /* 三种速度类型: 慢/中/快 各占1/3 */
        s_star_speed_type[i] = i % 3;
        if (s_star_speed_type[i] == 0) s_star_base_speed[i] = 0.8f + (float)(rand() % 15) * 0.1f;
        else if (s_star_speed_type[i] == 1) s_star_base_speed[i] = 2.5f + (float)(rand() % 20) * 0.1f;
        else s_star_base_speed[i] = 5.0f + (float)(rand() % 35) * 0.1f;
        s_star_prev_px[i] = -1;
        s_star_prev_py[i] = -1;
    }
    /* 背景小星初始化 */
    for (int i = 0; i < BG_STAR_COUNT; i++) {
        if (rand() % 10 < 7) {
            int side = (rand() % 2) ? 1 : -1;
            s_bg_star_x[i] = side * (100 + (int)(rand() % (int)(SCREEN_W * 1.2f)));
        } else {
            s_bg_star_x[i] = (rand() % SCREEN_W) - SCREEN_W / 2;
        }
        s_bg_star_y[i] = (rand() % (SCREEN_H * 3)) - SCREEN_H * 3 / 2;
        s_bg_star_z[i] = 500 + rand() % 2000;
        s_bg_star_speed[i] = 0.8f + (float)(rand() % 25) * 0.1f;
    }
    /* 巨型星/星舰已彻底删除 (用户要求), 无需初始化 */
    s_progress_decor = 0.0f;
}

/* 绘制飞船 (简单符号, 2px线) */
static void draw_spaceship(st7305_handle_t *lcd, int cx, int cy, int dir) {
    /* dir: 0=右, 1=左, 2=下, 3=上 */
    int s = 2;
    if (dir == 0) {
        draw_hline(lcd, cx - 6, cx + 6, cy, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 5, cx + 5, cy - s, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 5, cx + 5, cy + s, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 4, cx + 4, cy - s * 2, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 4, cx + 4, cy + s * 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx + 6, cy, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx + 7, cy, ST7305_COLOR_BLACK);
    } else if (dir == 1) {
        draw_hline(lcd, cx - 6, cx + 6, cy, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 5, cx + 5, cy - s, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 5, cx + 5, cy + s, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 4, cx + 4, cy - s * 2, ST7305_COLOR_BLACK);
        draw_hline(lcd, cx - 4, cx + 4, cy + s * 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx - 6, cy, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx - 7, cy, ST7305_COLOR_BLACK);
    } else if (dir == 2) {
        draw_vline(lcd, cx, cy - 6, cy + 6, ST7305_COLOR_BLACK);
        draw_vline(lcd, cx - s, cy - 5, cy + 5, ST7305_COLOR_BLACK);
        draw_vline(lcd, cx + s, cy - 5, cy + 5, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx, cy + 7, ST7305_COLOR_BLACK);
    } else {
        draw_vline(lcd, cx, cy - 6, cy + 6, ST7305_COLOR_BLACK);
        draw_vline(lcd, cx - s, cy - 5, cy + 5, ST7305_COLOR_BLACK);
        draw_vline(lcd, cx + s, cy - 5, cy + 5, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx, cy - 7, ST7305_COLOR_BLACK);
    }
}

/* 绘制一个数字到面板 (小号字体) */
static void draw_panel_digit(st7305_handle_t *lcd, int x, int y, int digit) {
    if (digit < 0 || digit > 9) return;
    char c = '0' + (char)digit;
    draw_ascii_small(lcd, x, y, c, false);
}

/* 绘制数字字符串到面板 */
static void draw_panel_str(st7305_handle_t *lcd, int x, int y, const char *str) {
    for (int i = 0; str[i]; i++) {
        draw_ascii_small(lcd, x + i * 8, y, str[i], false);
    }
}

/* 绘制填充圆 (优化版: 水平扫描, 减少迭代) */
static void fill_circle_simple(st7305_handle_t *lcd, int cx, int cy, int r) {
    for (int y = -r; y <= r; y++) {
        int hw = (int)sqrtf((float)(r * r - y * y));
        for (int x = -hw; x <= hw; x++)
            st7305_draw_pixel(lcd, cx + x, cy + y, ST7305_COLOR_BLACK);
    }
}

/* 快速水平线填充 (直接写 framebuffer, 跳过 draw_pixel 函数调用)
 * 用于大块区域填充, 减少 PSRAM 访问次数和函数调用开销 */
static void fast_fill_hline(st7305_handle_t *lcd, int x0, int x1, int y) {
    if (!lcd || !lcd->fb) return;
    if (y < 0 || y >= ST7305_HEIGHT) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= ST7305_WIDTH) x1 = ST7305_WIDTH - 1;
    if (x0 > x1) return;

    int inv_y = ST7305_HEIGHT - 1 - y;
    int y_group = inv_y >> 2;
    int y_sub = inv_y & 3;

    int pair_start = x0 >> 1;
    int pair_end = x1 >> 1;

    for (int x_pair = pair_start; x_pair <= pair_end; x_pair++) {
        uint32_t idx = (uint32_t)x_pair * (ST7305_HEIGHT >> 2) + (uint32_t)y_group;
        uint8_t mask = 0;

        int byte_x0 = x_pair * 2;
        int byte_x1 = byte_x0 + 1;

        if (byte_x0 >= x0 && byte_x0 <= x1)
            mask |= (uint8_t)(1u << (7u - (uint8_t)((y_sub << 1) | 0)));
        if (byte_x1 >= x0 && byte_x1 <= x1)
            mask |= (uint8_t)(1u << (7u - (uint8_t)((y_sub << 1) | 1)));

        lcd->fb[idx] &= ~mask;
    }
}

/* 快速填充圆 (使用 fast_fill_hline, 大幅减少函数调用) */
static void fast_fill_circle(st7305_handle_t *lcd, int cx, int cy, int r) {
    for (int y = -r; y <= r; y++) {
        int hw = (int)sqrtf((float)(r * r - y * y));
        fast_fill_hline(lcd, cx - hw, cx + hw, cy + y);
    }
}

/* 科幻星空屏保 - 重写版 */
static void screensaver_render_stars(st7305_handle_t *lcd, uint32_t now_ms) {
    /* 注意: 此函数在 app_main 任务中执行, app_main 没注册到 TWDT,
     * 不能调用 esp_task_wdt_reset(). 否则每秒打印几十次 E (xxx) task_wdt: task not found,
     * 阻塞 UART 输出导致按键响应被饿死, 表现为"按任何键没反应". */
    (void)now_ms;

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int cx = SCREEN_W / 2;
    int cy = 120;
    int max_z = 2500;

    /* 仪表盘位置参数 (供星星消失边界使用) */
    int cb = SCREEN_H - 1, chC = 32;
    int ctC = cb - chC;
    int star_vanish_y = ctC + 5;

    /* === 背景小星 (不变大, 始终1px, 速度慢) === */
    for (int i = 0; i < BG_STAR_COUNT; i++) {
        s_bg_star_z[i] -= (int)(s_bg_star_speed[i] * 1.0f);
        if (s_bg_star_z[i] <= 10) {
            float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
            float dist = 30.0f + (float)(rand() % 60);
            s_bg_star_x[i] = (int)(cos(angle) * dist);
            s_bg_star_y[i] = (int)(sin(angle) * dist);
            s_bg_star_z[i] = max_z - (rand() % 500);
            s_bg_star_speed[i] = 0.8f + (float)(rand() % 25) * 0.1f;
            continue;
        }
        float scale = 500.0f / (float)s_bg_star_z[i];
        int px = cx + (int)(s_bg_star_x[i] * scale);
        int py = cy + (int)(s_bg_star_y[i] * scale);
        if (px < 0 || px >= SCREEN_W || py < 0 || py >= star_vanish_y) {
            s_bg_star_z[i] = max_z - (rand() % 300);
            float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
            float dist = 40.0f + (float)(rand() % 60);
            s_bg_star_x[i] = (int)(cos(angle) * dist);
            s_bg_star_y[i] = (int)(sin(angle) * dist);
            continue;
        }
        st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }

    /* === 星空: 105颗星, 三种速度, 平滑变大 === */
    for (int i = 0; i < STAR_COUNT; i++) {
        /* 三种速度的layer_speed: 慢=1.0, 中=1.8, 快=3.0 */
        float layer_speed;
        if (s_star_speed_type[i] == 0) layer_speed = 1.0f;
        else if (s_star_speed_type[i] == 1) layer_speed = 1.8f;
        else layer_speed = 3.0f;

        float z_ratio = (float)(max_z - s_star_z[i]) / (float)max_z;
        float accel;
        if (z_ratio < 0.3f) {
            accel = 0.5f + z_ratio * 1.3f;
        } else if (z_ratio < 0.7f) {
            accel = 0.9f + (z_ratio - 0.3f) * 1.1f;
        } else {
            accel = 1.34f + (z_ratio - 0.7f) * 0.5f;
        }
        s_star_z[i] -= (int)(s_star_base_speed[i] * layer_speed * accel);

        if (s_star_z[i] <= 10) {
            float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
            float dist = 10.0f + (float)(rand() % 45);
            s_star_x[i] = (int)(cos(angle) * dist);
            s_star_y[i] = (int)(sin(angle) * dist);
            s_star_z[i] = max_z - (rand() % 500);
            if (s_star_speed_type[i] == 0) s_star_base_speed[i] = 0.8f + (float)(rand() % 15) * 0.1f;
            else if (s_star_speed_type[i] == 1) s_star_base_speed[i] = 2.5f + (float)(rand() % 20) * 0.1f;
            else s_star_base_speed[i] = 5.0f + (float)(rand() % 35) * 0.1f;
            s_star_prev_px[i] = -1;
            s_star_prev_py[i] = -1;
            continue;
        }

        float scale = 500.0f / (float)s_star_z[i];
        int px = cx + (int)(s_star_x[i] * scale);
        int py = cy + (int)(s_star_y[i] * scale);

        if (px < -10 || px >= SCREEN_W + 10 || py < -10 || py >= star_vanish_y) {
            s_star_z[i] = max_z - (rand() % 300);
            float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
            float dist = 30.0f + (float)(rand() % 50);
            s_star_x[i] = (int)(cos(angle) * dist);
            s_star_y[i] = (int)(sin(angle) * dist);
            if (s_star_speed_type[i] == 0) s_star_base_speed[i] = 0.8f + (float)(rand() % 15) * 0.1f;
            else if (s_star_speed_type[i] == 1) s_star_base_speed[i] = 2.5f + (float)(rand() % 20) * 0.1f;
            else s_star_base_speed[i] = 5.0f + (float)(rand() % 35) * 0.1f;
            s_star_prev_px[i] = -1;
            s_star_prev_py[i] = -1;
            continue;
        }

        /* 短尾迹 (中速和快速星有拖尾) */
        if (s_star_prev_px[i] >= 0 && s_star_z[i] < 900 && s_star_speed_type[i] >= 1) {
            int tdx = px - s_star_prev_px[i];
            int tdy = py - s_star_prev_py[i];
            int abs_dx = tdx < 0 ? -tdx : tdx;
            int abs_dy = tdy < 0 ? -tdy : tdy;
            int steps = abs_dx > abs_dy ? abs_dx : abs_dy;
            if (steps > 0) {
                int max_trail = 6 + (int)((float)(900 - s_star_z[i]) / 100.0f);
                if (max_trail > 12) max_trail = 12;
                if (steps > max_trail) steps = max_trail;
                for (int s = 0; s <= steps; s++) {
                    int tx = s_star_prev_px[i] + tdx * s / steps;
                    int ty = s_star_prev_py[i] + tdy * s / steps;
                    if (tx < 0 || tx >= SCREEN_W || ty < 0 || ty >= star_vanish_y) continue;
                    if (s > steps * 2 / 3)
                        st7305_draw_pixel(lcd, tx, ty, ST7305_COLOR_BLACK);
                }
            }
        }

        /* 星星大小: 平滑变大 1-4px */
        float size_f;
        if (s_star_z[i] > 2000) {
            size_f = 1.0f;
        } else if (s_star_z[i] > 1500) {
            size_f = 1.0f + (2000.0f - (float)s_star_z[i]) / 500.0f * 0.5f;
        } else if (s_star_z[i] > 1000) {
            size_f = 1.5f + (1500.0f - (float)s_star_z[i]) / 500.0f * 1.0f;
        } else if (s_star_z[i] > 500) {
            size_f = 2.5f + (1000.0f - (float)s_star_z[i]) / 500.0f * 1.0f;
        } else {
            size_f = 3.5f + (500.0f - (float)s_star_z[i]) / 490.0f * 0.5f;
        }
        int star_size = (int)size_f;
        if (star_size < 1) star_size = 1;
        if (star_size > 4) star_size = 4;

        if (star_size == 1) {
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
        } else {
            int half = star_size / 2;
            int r2 = (star_size * star_size) / 4;
            for (int sy = -half; sy <= half; sy++) {
                for (int sx = -half; sx <= half; sx++) {
                    if (sx * sx + sy * sy <= r2) {
                        int dx = px + sx, dy = py + sy;
                        if (dx >= 0 && dx < SCREEN_W && dy >= 0 && dy < star_vanish_y)
                            st7305_draw_pixel(lcd, dx, dy, ST7305_COLOR_BLACK);
                    }
                }
            }
        }
        s_star_prev_px[i] = px;
        s_star_prev_py[i] = py;
    }

    /* === 巨型星/星舰已彻底删除 (用户要求), 保持纯净星空 === */

    /* === 底部驾驶舱 (三级台阶, 1px线, 中台阶45度斜边, 完整外壳) === */
    int chLR = 22, stL = 105, stR = 295;
    int ctLR = cb - chLR, cr = 4;

    /* 左台阶: 圆角左上角+上边+左边+下边 */
    for (int x = 0; x <= stL; x++) {
        int inCorner = (x < cr);
        if (!inCorner || (cr - x) * (cr - x) <= cr * cr)
            st7305_draw_pixel(lcd, x, ctLR, ST7305_COLOR_BLACK);
    }
    for (int y = ctLR; y <= cb; y++) {
        st7305_draw_pixel(lcd, 0, y, ST7305_COLOR_BLACK);
    }

    /* 3D保护罩参数 (提前定义供台阶跳过使用) */
    int dmx = SCREEN_W / 2, dmc = ctC - 1, dmr = 80;
    int shield_r = dmr * 2 / 3;       /* 保护罩半径 53 */

    /* 中台阶: 上边(跳过3D罩区域)+下边 */
    int shield_left = dmx - shield_r - 2;
    int shield_right = dmx + shield_r + 2;
    for (int x = stL + 5; x <= 159; x++) {
        if (x >= shield_left && x <= shield_right) continue;
        st7305_draw_pixel(lcd, x, ctC, ST7305_COLOR_BLACK);
    }
    for (int x = 241; x <= stR - 5; x++) {
        if (x >= shield_left && x <= shield_right) continue;
        st7305_draw_pixel(lcd, x, ctC, ST7305_COLOR_BLACK);
    }

    /* 右台阶: 圆角右上角+上边+右边+下边 */
    for (int x = stR; x < SCREEN_W; x++) {
        int inCorner = (SCREEN_W - 1 - x < cr);
        if (!inCorner || ((SCREEN_W - 1 - x) * (SCREEN_W - 1 - x) <= cr * cr))
            st7305_draw_pixel(lcd, x, ctLR, ST7305_COLOR_BLACK);
    }
    for (int y = ctLR; y <= cb; y++) {
        st7305_draw_pixel(lcd, SCREEN_W - 1, y, ST7305_COLOR_BLACK);
    }

    /* 底部贯通 */
    for (int x = 0; x < SCREEN_W; x++) {
        st7305_draw_pixel(lcd, x, cb, ST7305_COLOR_BLACK);
    }

    /* 45度斜边连接左右台阶到中台阶 */
    for (int y = ctLR; y >= ctC; y--) {
        int off = (int)((float)(ctLR - y) * 0.5f);
        st7305_draw_pixel(lcd, stL + off, y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, stR - off, y, ST7305_COLOR_BLACK);
    }

    /* === 半球 (保护罩缩小三分之一) + 旋臂亮点 (缩小五分之一) === */
    int gx = dmx, gy = dmc - 16;      /* 旋臂中心 */
    float arm_scale = 0.8f;           /* 旋臂亮点缩小五分之一 */

    /* 底座椭圆 (长轴=保护罩半径, 上下端点对齐形成3D球体) */
    {
        int ea = shield_r, eb = 13, ecx = dmx, ecy = dmc;
        int lastPx = -1, lastPy = -1, dashCnt = 0;
        for (int a = 0; a < 360; a++) {
            float rad = (float)a * 3.14159f / 180.0f;
            int pEx = ecx + (int)(ea * cos(rad));
            int pEy = ecy + (int)(eb * sin(rad));
            if (pEx < 0 || pEx >= SCREEN_W || pEy < 0 || pEy >= SCREEN_H) continue;
            if (a < 180) {
                /* 上半实线 (被半球遮住, 画也无妨) */
                st7305_draw_pixel(lcd, pEx, pEy, ST7305_COLOR_BLACK);
            } else {
                /* 下半虚线 (3D球体底沿) */
                if (pEx != lastPx || pEy != lastPy) {
                    dashCnt++;
                    if (dashCnt % 3 == 1)
                        st7305_draw_pixel(lcd, pEx, pEy, ST7305_COLOR_BLACK);
                    lastPx = pEx; lastPy = pEy;
                }
            }
        }
    }

    /* 保护罩上半圆 (半径=shield_r, 与底座椭圆左右端点对齐) */
    for (int a = 0; a <= 180; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int pX = dmx + (int)(shield_r * cos(rad));
        int pY = dmc - (int)(shield_r * sin(rad));
        if (pX >= 0 && pX < SCREEN_W && pY >= 0 && pY < SCREEN_H) {
            st7305_draw_pixel(lcd, pX, pY, ST7305_COLOR_BLACK);
        }
    }

    /* 4臂旋涡星系 (缩小五分之一, 限制在保护罩内) */
    {
        int arm_bound = (int)((float)(shield_r - 4) * arm_scale);
        for (int arm = 0; arm < 4; arm++) {
            float arm_off = (float)arm * 90.0f * 3.14159f / 180.0f;
            for (int r = 5; r < (int)(46.0f); r++) {
                float angle = arm_off + (float)r * 0.15f + (float)now_ms / 4000.0f;
                int px2 = gx + (int)((float)r * cos(angle));
                int py2 = gy + (int)((float)r * 0.45f * sin(angle));
                int ddx = px2 - dmx, ddy = py2 - dmc;
                if (ddx * ddx + ddy * ddy < arm_bound * arm_bound && py2 <= dmc) {
                    if ((r + arm * 5 + (int)(now_ms / 150)) % 2 == 0)
                        st7305_draw_pixel(lcd, px2, py2, ST7305_COLOR_BLACK);
                }
            }
        }
        /* 中心亮点 (缩小五分之一) */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -6; dx <= 6; dx++)
                if (dx * dx * 2 + dy * dy * 3 < 26) {
                    int px2 = gx + dx, py2 = gy + dy;
                    int ddx = px2 - dmx, ddy = py2 - dmc;
                    if (ddx * ddx + ddy * ddy < (shield_r - 3) * (shield_r - 3))
                        st7305_draw_pixel(lcd, px2, py2, ST7305_COLOR_BLACK);
                }
        /* 旋臂亮点 (缩小五分之一) */
        for (int arm = 0; arm < 4; arm++) {
            float arm_off = (float)arm * 90.0f * 3.14159f / 180.0f;
            for (int i = 0; i < 7; i++) {
                int r2 = (int)((14 + i * 6) * arm_scale);
                float angle = arm_off + (float)r2 * 0.15f + (float)now_ms / 4000.0f;
                int px2 = gx + (int)((float)r2 * cos(angle));
                int py2 = gy + (int)((float)r2 * 0.45f * sin(angle));
                int ddx = px2 - dmx, ddy = py2 - dmc;
                if (ddx * ddx + ddy * ddy < arm_bound * arm_bound && py2 <= dmc) {
                    st7305_draw_pixel(lcd, px2, py2, ST7305_COLOR_BLACK);
                    st7305_draw_pixel(lcd, px2 + 1, py2, ST7305_COLOR_BLACK);
                    st7305_draw_pixel(lcd, px2, py2 + 1, ST7305_COLOR_BLACK);
                    st7305_draw_pixel(lcd, px2 + 1, py2 + 1, ST7305_COLOR_BLACK);
                }
            }
        }
    }

    /* 内容: 速度值+单位左 / 时间右 (无英文标签) */
    int speed_val = 8000 + (int)(sin((float)now_ms / 1000.0f) * 200);
    if (speed_val < 0) speed_val = 0;
    if (speed_val > 99999) speed_val = 99999;
    char speed_str[12];
    snprintf(speed_str, sizeof(speed_str), "%05d", speed_val);
    draw_panel_str(lcd, 20, ctLR + 5, speed_str);
    draw_panel_str(lcd, 20 + (int)strlen(speed_str) * 8 + 2, ctLR + 5, "KM/H");

    char time_str[12];
    menu_get_time_str(time_str, sizeof(time_str));
    draw_panel_str(lcd, 320, ctLR + 5, time_str);

    /* 进度条 (距底部12px, 水平居中, 加粗) */
    int prog_y = cb - 12;
    int prog_x0 = stL + 20, prog_x1 = stR - 20;

    /* 进度条装饰循环 (约5分钟到达) */
    s_progress_decor += 0.0055f;
    if (s_progress_decor > 100.0f) s_progress_decor -= 100.0f;

    /* 进度条边框 */
    for (int x = prog_x0; x <= prog_x1; x++) {
        st7305_draw_pixel(lcd, x, prog_y - 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x, prog_y + 1, ST7305_COLOR_BLACK);
    }
    int fill_w = (prog_x1 - prog_x0 - 2) * (int)s_progress_decor / 100;
    for (int x = prog_x0 + 1; x <= prog_x0 + 1 + fill_w; x++) {
        st7305_draw_pixel(lcd, x, prog_y - 1, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x, prog_y, ST7305_COLOR_BLACK);
    }

    /* 注意: 此函数在 app_main 任务中执行, 不能调用 esp_task_wdt_reset() */

    st7305_flush(lcd);
}

/* 简单的描边圆 (使用2px线) */
static void stroke_circle_simple(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            int r_in = r - 1;
            if (r_in < 0) r_in = 0;
            if (r_in * r_in <= d2 && d2 <= r * r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
                    st7305_draw_pixel(lcd, px, py, color);
                }
            }
        }
    }
}

/* 进入壁纸 (按当前配置的模式). 游戏壁纸模式会阻塞运行游戏直到退出, 返回 false 表示壁纸已结束 */
static bool screensaver_enter(st7305_handle_t *lcd, uint32_t now_ms) {
    extern menu_state_t g_menu;
    /* 20260812: 所有壁纸模式(含游戏壁纸)进入即挂起蓝牙, 退出时统一恢复.
     * 常驻的只有音频框架; 蓝牙在壁纸期间断开+停扫 (协议栈存活). */
    screensaver_bt_off();
    if (g_menu.wallpaper_mode == WALLPAPER_MODE_GAME) {
        wallpaper_games_load();
        if (s_wp_game_count <= 0) {
            /* 没有游戏壁纸: 回退内置星空 */
            s_screensaver_active = true;
            screensaver_bt_off();
            s_last_input_ms = now_ms + 500;
            screensaver_render_builtin(lcd, now_ms);
            return true;
        }
        /* 20260812 测试: 从壁纸游戏列表中随机挑一个启动 (验证各引擎可用性) */
        int idx = rand() % s_wp_game_count;
        ESP_LOGI(TAG, "游戏壁纸: 启动 [engine=%d] %s", s_wp_game_engine[idx], s_wp_games[idx]);
        s_wp_key_inited = false;   /* 重进时重新采样按键, 忽略已按住的键 */
        s_screensaver_active = true;
        s_last_input_ms = now_ms + 500;
        start_wallpaper_game(&g_menu, s_wp_game_engine[idx], s_wp_games[idx]);
        /* 游戏退出: 结束本次壁纸, 恢复蓝牙 (测试路径已挂起, 幂等) */
        screensaver_bt_on();
        screensaver_release_wallpaper_buffers();
        s_screensaver_active = false;
        s_last_input_ms = now_ms;
        return false;
    }
    s_screensaver_active = true;
    if (s_screensaver_test) {
        s_screensaver_test_ms = now_ms;   /* 测试壁纸: 保持蓝牙连接 */
    } else {
        screensaver_bt_off();
    }
    /* V1.0.68: 锁屏后任意输入立即退出 (用户需求: 点按软关机键/任意键立马退出休眠) */
    s_last_input_ms = now_ms;
    if (g_menu.wallpaper_mode == WALLPAPER_MODE_BMP)
        screensaver_render_bmp(lcd, now_ms);
    else
        screensaver_render_builtin(lcd, now_ms);
    return true;
}

bool screensaver_check_and_render(st7305_handle_t *lcd, bool has_input) {
    extern menu_state_t g_menu;

    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    /* V1.0.68: 手动锁屏 (点按软关机键) 优先级最高, 任何界面生效
     * (must be checked BEFORE has_input, otherwise the confirm button press
     *  that set s_screensaver_preview=true also triggers has_input reset) */
    if (s_screensaver_preview) {
        s_screensaver_preview = false;
        return screensaver_enter(lcd, now_ms);
    }

    /* MP3 播放器界面 / 音频播放中 / 电子书阅读中始终不启用屏保
     * (仅对"自动进入"生效; 锁屏已激活时任何输入必须能立即退出) */
    if (!s_screensaver_active &&
        (g_menu.current_page == MENU_PAGE_MP3_PLAYER ||
         audio_player_get_state() == AUDIO_STATE_PLAYING ||
         s_pomo_active ||
         (g_menu.current_page == MENU_PAGE_BOOK && book_reader_is_open()))) {
        return false;
    }
    /* 屏保始终启用, 仅在音乐播放时跳过 (见上方) */
    if (has_input) {
        screensaver_bt_on();   /* 输入唤醒: 恢复蓝牙 */
        if (s_screensaver_active)
            screensaver_release_wallpaper_buffers();
        s_last_input_ms = now_ms;
        s_screensaver_active = false;
        s_screensaver_preview = false;
        s_screensaver_test = false;
        return false;
    }
    /* 屏保已激活: 持续渲染, 有输入时检查是否在忽略期内 */
    if (s_screensaver_active) {
        if (s_screensaver_test && now_ms - s_screensaver_test_ms >= 30000) {
            ESP_LOGI(TAG, "测试壁纸: 30 秒到, 自动退出");
            screensaver_bt_on();   /* 自动退出也要恢复蓝牙 */
            screensaver_release_wallpaper_buffers();
            s_screensaver_test = false;
            s_screensaver_active = false;
            s_last_input_ms = now_ms;
            return false;
        }
        if (has_input && now_ms >= s_last_input_ms) {
            /* 输入在忽略期外, 退出屏保 */
            screensaver_release_wallpaper_buffers();
            s_screensaver_active = false;
            s_screensaver_test = false;
            s_last_input_ms = now_ms;
            return false;
        }
        if (g_menu.wallpaper_mode == WALLPAPER_MODE_BMP)
            screensaver_render_bmp(lcd, now_ms);
        else
            screensaver_render_builtin(lcd, now_ms);
        return true;
    }
    if (s_last_input_ms == 0) {
        s_last_input_ms = now_ms;
        return false;
    }
    uint32_t idle_ms = now_ms - s_last_input_ms;
    /* 休眠时间 (可调, 1..30 分钟) */
    uint8_t tm = (g_menu.wallpaper_timeout_min >= 1 && g_menu.wallpaper_timeout_min <= 30)
                 ? g_menu.wallpaper_timeout_min : 3;
    uint32_t timeout_ms = (uint32_t)tm * 60 * 1000;
    if (idle_ms < timeout_ms) return false;
    return screensaver_enter(lcd, now_ms);
}

bool screensaver_is_active(void) {
    return s_screensaver_active;
}

/* V1.0.68: 手动锁屏 (点按软关机键): 置 preview 标志, 下一帧
 * screensaver_check_and_render 立即进入屏保 (任何界面生效). */
void menu_screensaver_activate(void) {
    extern menu_state_t g_menu;
    s_screensaver_preview = true;
    g_menu.needs_redraw = true;
}

void menu_screensaver_enter_test(void) {
    s_screensaver_preview = true;
    s_screensaver_test = true;
}

bool menu_screensaver_is_active(void) {
    return s_screensaver_active;
}

/* 判断菜单是否停在主菜单桌面 (MENU_PAGE_MAIN).
 * 仅在该页面为 true, 用于蓝牙自动重连、屏保等后台逻辑的条件门控. */
bool menu_is_current_page_main(void) {
    return (g_menu.current_page == MENU_PAGE_MAIN);
}

/* === 注: 公开 wrapper (menu_open_list_dialog / menu_open_sd_dialog) 在文件后面定义,
 * 因为它们依赖的 static list_dialog_open / open_sd_dialog 在文件下方. */

/* === 前向声明: 解决函数在使用前未定义的问题 ===
 * list_dialog_open / gamepad_list_on_select / gamepad_history_on_select /
 * gamepad_act_* 都在文件下方定义, 这里先声明. */
static void list_dialog_open(menu_state_t *state, const char *title,
                             int count, void (*on_select)(menu_state_t *, int));
static void gamepad_list_on_select(menu_state_t *state, int idx);
static void gamepad_history_on_select(menu_state_t *state, int idx);
static void gamepad_act_add_device(menu_state_t *state);
static void gamepad_act_key_mapping(menu_state_t *state);
static void gamepad_act_history(menu_state_t *state);
static void gamepad_list_open(menu_state_t *state);

/* ============ 字体渲染 ============ */

/* 8x12 ASCII 字体 (缩放 2x = 16x24, 与中文字体等高) */
static const uint8_t FONT8X12[][12] = {
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

/* 绘制小号 ASCII 字符 (8x12 不缩放, 用于状态栏小数字) */
static void draw_ascii_small(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 7, y + 11, bg);
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            st7305_draw_pixel(lcd, x + col, y + row, color);
        }
    }
}

/* 绘制中号 ASCII 字符 (8x12 缩放 1.5x = 12x18) - 用于状态栏时间 */
static void draw_ascii_medium(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 17, bg);
    /* 1.5x 缩放: 8->12, 12->18, 用反向映射表 */
    static const int col_map[12] = {0,0,1,2,2,3,4,4,5,6,6,7};
    static const int row_map[18] = {0,0,1,2,2,3,4,4,5,6,6,7,8,8,9,10,10,11};
    for (int dy = 0; dy < 18; dy++) {
        uint8_t bits = bmp[row_map[dy]];
        for (int dx = 0; dx < 12; dx++) {
            st7305_color_t color = (bits & (1 << (7 - col_map[dx]))) ? fg : bg;
            st7305_draw_pixel(lcd, x + dx, y + dy, color);
        }
    }
}

/* 绘制 ASCII 字符 (8x12 缩放 2x = 16x24) - 支持 inverted 反色 */
static void draw_ascii(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    /* 背景色 */
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    /* 填充背景方块 (16x24) */
    fill_rect(lcd, x, y, x + 15, y + 23, bg);
    /* 绘制字符 (缩放 2x)
       FONT8X12 位图: MSB(bit7)=最左像素 */
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            /* 2x 缩放 */
            st7305_draw_pixel(lcd, x + col * 2,     y + row * 2,     color);
            st7305_draw_pixel(lcd, x + col * 2 + 1, y + row * 2,     color);
            st7305_draw_pixel(lcd, x + col * 2,     y + row * 2 + 1, color);
            st7305_draw_pixel(lcd, x + col * 2 + 1, y + row * 2 + 1, color);
        }
    }
}

/* 绘制单个中文字符 (24x24, 支持 inverted 反色 + scale 缩放) */
static void draw_zh(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted, int scale) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int sz = 24 * scale;
    /* 填充背景方块 */
    fill_rect(lcd, x, y, x + sz - 1, y + sz - 1, bg);
    if (idx < 0) {
        /* 全角空格 U+3000: 画空白 (用于菜单选项冒号对齐补位), 不画叉 */
        if ((uint8_t)str[0] == 0xE3 && (uint8_t)str[1] == 0x80 && (uint8_t)str[2] == 0x80)
            return;
        /* 找不到字 - 画叉 */
        for (int i = 0; i < sz; i++) {
            st7305_draw_pixel(lcd, x + i, y + i, fg);
            st7305_draw_pixel(lcd, x + i, y + sz - 1 - i, fg);
        }
        return;
    }
    const uint8_t *bmp = zh_font_data[idx];
    int bytes_per_row = (ZH_FONT_W + 7) / 8;  /* 3 */
    for (int row = 0; row < ZH_FONT_H; row++) {
        for (int col = 0; col < ZH_FONT_W; col++) {
            int byte_idx = row * bytes_per_row + (col / 8);
            int bit = 7 - (col % 8);
            st7305_color_t color = (bmp[byte_idx] & (1 << bit)) ? fg : bg;
            if (scale == 1) {
                st7305_draw_pixel(lcd, x + col, y + row, color);
            } else {
                /* scale 倍放大 */
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        st7305_draw_pixel(lcd, x + col * scale + dx, y + row * scale + dy, color);
            }
        }
    }
}

/* 绘制中英文混合字符串 (UTF-8, scale=1 默认 24x24 中文) */
static void draw_text(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            /* ASCII: 16x24 */
            draw_ascii(lcd, cursor_x, y, c, inverted);
            cursor_x += 16;
            str++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 (跳过, 字库不含) */
            cursor_x += 16;
            str += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 中文 */
            draw_zh(lcd, cursor_x, y, str, inverted, 1);
            cursor_x += 24;
            str += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 (跳过) */
            cursor_x += 24;
            str += 4;
        } else {
            str++;
        }
    }
}

/* 绘制标签 (放大 1.5x 中文 + 1.5x ASCII) - 弹字大字号 */
static void draw_label(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    int scale = 1;  /* 1x = 24x24 中文字, 1x = 16x24 ASCII (比之前 2x 小) */
    int ascii_w = 16 * scale;
    int zh_w = 24 * scale;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            /* ASCII */
            char ch = c;
            int idx = (ch >= 0x20 && ch <= 0x7E) ? (ch - 0x20) : ('?' - 0x20);
            const uint8_t *bmp = FONT8X12[idx];
            st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
            st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
            fill_rect(lcd, cursor_x, y, cursor_x + ascii_w - 1, y + ascii_w - 1, bg);
            for (int row = 0; row < 12; row++) {
                uint8_t bits = bmp[row];
                for (int col = 0; col < 8; col++) {
                    st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
                    st7305_draw_pixel(lcd, cursor_x + col * 2,     y + row * 2,     color);
                    st7305_draw_pixel(lcd, cursor_x + col * 2 + 1, y + row * 2,     color);
                    st7305_draw_pixel(lcd, cursor_x + col * 2,     y + row * 2 + 1, color);
                    st7305_draw_pixel(lcd, cursor_x + col * 2 + 1, y + row * 2 + 1, color);
                }
            }
            cursor_x += ascii_w;
            str++;
        } else if ((c & 0xF0) == 0xE0) {
            draw_zh(lcd, cursor_x, y, str, inverted, scale);
            cursor_x += zh_w;
            str += 3;
        } else {
            str += 3;  /* 跳过 */
        }
    }
}

/* 计算字符串渲染宽度 */
static int text_width(const char *str) {
    int w = 0;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) { w += 16; str++; }
        else if ((c & 0xE0) == 0xC0) { w += 16; str += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 24; str += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 24; str += 4; }
        else str++;
    }
    return w;
}

/* 测量 [start, end) 区间的 UTF-8 文本宽度 (用于多行弹窗, 避免依赖 '\0' 结尾) */
static int text_width_bounded(const char *start, const char *end) {
    int w = 0;
    const char *s = start;
    while (s < end) {
        uint8_t c = (uint8_t)*s;
        if (c < 0x80) { w += 16; s++; }
        else if ((c & 0xE0) == 0xC0) { w += 16; s += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 24; s += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 24; s += 4; }
        else s++;
    }
    return w;
}

/* 计算大标签宽度 (scale=1 中文字 24, ASCII 16) */
static int label_width(const char *str) {
    int w = 0;
    int scale = 1;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) { w += 16 * scale; str++; }
        else if ((c & 0xF0) == 0xE0) { w += 24 * scale; str += 3; }
        else str += 3;
    }
    return w;
}

/* 在指定中心点 x 画标签 (用于动画) */
static void draw_label_centered_at(st7305_handle_t *lcd, int cx, int y, const char *str, bool inverted) {
    int w = label_width(str);
    int x = cx - w / 2;
    if (x < 0) x = 0;
    if (x + w > SCREEN_W) x = SCREEN_W - w;
    draw_label(lcd, x, y, str, inverted);
}

static void draw_text_centered(st7305_handle_t *lcd, int y, const char *str, bool inverted) {
    int w = text_width(str);
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    draw_text(lcd, x, y, str, inverted);
}

/* 自动换行绘制: 在 [x0, x1] 宽度内将 text 折行, 从 y0 起逐行绘制 (行高 line_h).
 * align: 0=左对齐, 1=居中. 支持 \n 硬换行; CJK 按字符折行, ASCII 按空格分词
 * (超长单词按字符折), 避免文字超出弹窗边缘. 返回绘制行数. */
static int draw_text_wrapped(st7305_handle_t *lcd, int x0, int x1, int y0,
                             const char *text, bool inverted, int align, int line_h) {
    int max_w = x1 - x0;
    if (max_w < 16) max_w = 16;
    int y = y0;
    int lines = 0;
    const char *p = text;
    char para[128];
    while (*p && lines < 16) {
        int plen = 0;
        while (*p && *p != '\n' && plen < (int)sizeof(para) - 1) {
            para[plen++] = *p++;
        }
        para[plen] = '\0';
        if (*p == '\n') p++;
        int i = 0;
        while (i < plen && lines < 16) {
            int line_w = 0;
            int j = i;
            int last_break = -1;
            while (j < plen) {
                uint8_t c = (uint8_t)para[j];
                int cw;
                if (c < 0x80) cw = 16;
                else if ((c & 0xF0) == 0xE0) cw = 24;
                else if ((c & 0xE0) == 0xC0) cw = 16;
                else if ((c & 0xF8) == 0xF0) cw = 24;
                else cw = 16;
                if (line_w + cw > max_w && j > i) break;
                if (c == ' ') last_break = j;
                line_w += cw;
                j++;
            }
            int line_len = j - i;
            if (j < plen && last_break > i) {
                line_len = last_break - i + 1;
                j = last_break + 1;
            }
            char buf[64];
            int n = 0;
            for (int k = i; k < i + line_len && n < (int)sizeof(buf) - 1; k++) {
                buf[n++] = para[k];
            }
            while (n > 0 && buf[n - 1] == ' ') n--;
            buf[n] = '\0';
            int draw_x;
            if (align == 1) {
                int w = text_width(buf);
                draw_x = x0 + (max_w - w) / 2;
                if (draw_x < x0) draw_x = x0;
            } else {
                draw_x = x0;
            }
            draw_text(lcd, draw_x, y, buf, inverted);
            y += line_h;
            lines++;
            i = j;
        }
    }
    return lines;
}

/* ============ 图标绘制 (使用位图) ============ */

/* 绘制 1-bit 位图图标 (支持宽高不一致) - 先声明内部函数 */
static void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx);
static void draw_icon_bitmap(st7305_handle_t *lcd, int cx, int cy, int size, int icon_idx) {
    draw_icon_bitmap_stretched(lcd, cx, cy, size, size, icon_idx);
}
static void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    if (icon_idx < 0 || icon_idx >= XMB_ICON_COUNT) return;
    const uint8_t *bmp = xmb_icons[icon_idx];
    int x0 = cx - size_w / 2;
    int y0 = cy - size_h / 2;
    for (int dy = 0; dy < size_h; dy++) {
        for (int dx = 0; dx < size_w; dx++) {
            int src_x = (dx * XMB_ICON_W) / size_w;
            int src_y = (dy * XMB_ICON_H) / size_h;
            int byte_idx = src_y * ((XMB_ICON_W + 7) / 8) + (src_x / 8);
            int bit = 7 - (src_x % 8);
            if (bmp[byte_idx] & (1 << bit)) {
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}

/* 主菜单项定义 */
typedef struct {
    const char *title;        /* 标题 (顶部显示) */
    const char *short_label;  /* 短标签 (图标下方) */
    int         icon_idx;     /* 图标索引 */
    menu_page_t sub_page;     /* 进入子页面 */
} main_item_t;

static const main_item_t main_items[] = {
    /* === 固定项: 词典 (游戏改名) === */
    { "BBK",         "BBK",     0,  MENU_PAGE_SELECT_GAME },  /* icon_game */
    /* === 彩蛋隐藏游戏模拟器 (解锁后显示, 紧排在"词典"之后) ===
     * 默认被 menu_main_count() 隐藏, 仅在"请作者喝杯水"图界面连按 5 次确认解锁后
     * 出现在主菜单. 各引擎的二级菜单在进入时由 engine_manager 后台加载.
     * NES 与 FC 使用相同 .nes 文件, 已合并为一个 FC 引擎;
     * GBC/FC/arduboy 引擎尚未实现, 暂统用 GB 图标并复用 GB 游戏页占位. */
    { "GB",          "GB",      8,  MENU_PAGE_GB_GAME      },  /* icon_gb   */
    { "GBC",         "GBC",     8,  MENU_PAGE_GB_GAME      },  /* 统一用 GB 图标 */
    { "NES",         "NES",     8,  MENU_PAGE_GB_GAME      },  /* 统一用 GB 图标 */
    { "arduboy",     "arduboy", 8,  MENU_PAGE_GB_GAME      },  /* 统一用 GB 图标 */
    /* === 固定项: 其余功能 === */
    { "阅读",        "阅读",     9,  MENU_PAGE_BOOK       },  /* icon_book */
    { "手柄",        "手柄",    3,  MENU_PAGE_GAMEPAD      },  /* icon_pad  */
    { "音乐",        "音乐",     7,  MENU_PAGE_MP3_PLAYER   },  /* icon_play */
    { "壁纸",        "壁纸",    10, MENU_PAGE_WALLPAPER    },  /* icon_wall */
    { "番茄钟",      "番茄钟",  11, MENU_PAGE_POMODORO     },  /* icon_pomo */
    /* === 用户需求: 存储管理 (TF卡) 独立到一级菜单栏, 使用 TF 卡图标 (icon_sd=5) ===
     * 原位置在 设置 -> 存储管理 弹窗, 现提升为独立一级菜单项, 选中直接弹出 SD 卡管理. */
    { "存储",        "存储",     5,  MENU_PAGE_SETTINGS_SD  },  /* icon_sd   */
    { "设置",        "设置",    6,  MENU_PAGE_SETTINGS     },  /* icon_info */
};
#define MAIN_ITEM_COUNT (sizeof(main_items) / sizeof(main_items[0]))
/* 固定项数 (词典/阅读/手柄/音乐/壁纸/番茄钟/存储/设置) 与 彩蛋隐藏引擎项数.
 * 隐藏引擎物理索引区间 [MAIN_HIDDEN_FIRST, MAIN_HIDDEN_FIRST+MAIN_HIDDEN_COUNT) = 1..4,
 * 紧跟在"词典"(0) 之后; 固定项在物理索引 0 与 5..11.
 * V1.0.49: NES 已与 FC 合并, 隐藏引擎为 GB/GBC/FC/arduboy 共 4 个. */
#define MAIN_FIXED_COUNT   8
#define MAIN_HIDDEN_COUNT  4
#define MAIN_HIDDEN_FIRST  1
#define MAIN_MP3_PHYS      7   /* "音乐"(MP3) 在 main_items 里的物理索引 */
/* V1.0.68: 禁用音频时隐藏 MP3 菜单项 */
static bool mp3_menu_visible(const menu_state_t *state) {
    return !state->settings.audio_disable;
}
/* 主菜单实际显示项数 (可见数): 固定项 + (解锁后) 隐藏引擎项 - (禁用音频时) MP3 */
static int menu_main_count(const menu_state_t *state) {
    int count = MAIN_FIXED_COUNT
              + (state->show_hidden_menus ? MAIN_HIDDEN_COUNT : 0);
    if (!mp3_menu_visible(state)) count--;
    return count;
}
/* 将"可见索引"映射回 main_items 物理索引.
 * 解锁时可见顺序 == 物理顺序; 隐藏时跳过 1..4 (GB/GBC/NES/arduboy);
 * 禁用音频时再跳过物理索引 7 (MP3). */
static int menu_main_phys(const menu_state_t *state, int vis_idx) {
    int phys;
    if (state->show_hidden_menus) {
        phys = vis_idx;
    } else {
        if (vis_idx == 0) return 0;  /* 词典 */
        phys = vis_idx + MAIN_HIDDEN_COUNT;
    }
    if (!mp3_menu_visible(state) && phys >= MAIN_MP3_PHYS) phys++;
    return phys;
}

/* ============ 子页面定义 ============ */

typedef struct {
    const char *title;
    /* 动态生成项目文本的函数 (写到 buf, 返回项目数); 若为 NULL 则用静态 items */
    int  (*build_items)(menu_state_t *state, char buf[][64], int max);
    /* 确认键处理: 返回 true 表示消费了事件 (不进入子页); 返回 false 进入 default 行为 */
    bool (*on_confirm)(menu_state_t *state, int idx);
    /* 左/右键处理 (编辑模式) */
    bool (*on_left_right)(menu_state_t *state, int idx, bool is_right);
} sub_page_def_t;

/* 子页项目缓存 (每次渲染前重建)
 * 容量: 128 条 (足以显示 100+ 个游戏 + 1 个"返回"项 + 缓冲)
 * 内存: 128 * 64 = 8192 字节 (8 KB), 比之前 200 条 (12.5 KB) 省 4.5 KB */
EXT_RAM_BSS_ATTR static char g_sub_items[128][64];
static int  g_sub_count;

/* === 游戏设置子页的函数前置声明 (供 select_game_build / select_game_on_confirm 调用) ===
 * 用户需求: 把"游戏设置"做成与游戏文件夹同级的左侧项, 选中后右栏同时显示 4 个设置项.
 * 因此 select_game_build 在左侧 idx=0 时, 需要填充右栏为设置项 (调用 game_settings_build);
 * select_game_on_confirm 在右栏确认时, 需要调用 game_settings_on_confirm 触发对应动作. */
static int  game_settings_build(menu_state_t *state, char buf[][64], int max);
static bool game_settings_on_confirm(menu_state_t *state, int idx);
static bool game_settings_on_lr(menu_state_t *state, int idx, bool is_right);

/* === 电子词典分栏布局: 文件夹名缓存 + 选择辅助 === */
/* 这些声明提前到这里, 以便 select_game_on_confirm 也能使用 */
#define MAX_FOLDER_NAMES   32     /* 最多缓存 32 个子文件夹名 */
EXT_RAM_BSS_ATTR static char g_folder_names[MAX_FOLDER_NAMES][64];
static int  g_folder_count = 0;
/* 侧栏索引含义:
 *   0 = "游戏设置" (特殊, 无对应文件夹)
 *   1 = "收藏" (特殊, 无对应文件夹)
 *   2..N+1 = 真实子文件夹 (g_folder_names[idx-2])
 * 返回 NULL 表示"特殊项"或越界 */
static const char *get_selected_folder_name(menu_state_t *state, int idx) {
    if (idx == 2) return "";  /* "全部": 空 = 扫描根目录 */
    if (idx < 3) return NULL;
    int real = idx - 3;
    if (real >= g_folder_count) return NULL;
    return g_folder_names[real];
}

/* 判断是否为侧栏特殊项 (0=游戏设置, 1=收藏, 2=全部) */
static inline bool is_special_folder(int idx) { return idx >= 0 && idx < 3; }

/* ============ 子页动作 (on_confirm) ============ */

/* 默认处理: 倒数第一项为"返回", 其他项默认回到主菜单 */
/* 文曲星游戏确认 - 启动游戏 */
/* GB 游戏运行循环: 每帧轮询按键 → joypad.
 * 退出: 1) 物理 BOOT(GPIO0) 长按 / KEY(GPIO18) 长按 → MENU_ACTION_BACK / MENU_ACTION_LONG_LEFT
 *       2) 手柄 L2=F_EXIT (返回菜单键) 上升沿 → 强制退出到菜单主页
 *       3) 手柄 BACK 长按 500ms → 退出 (短按仍做 GB B 键, 不退出)
 * 手柄导航键禁用 (防止方向/确认键被当菜单动作), 但 F_EXIT 和 BACK 长按仍独立查询. */

/* 公用游戏运行循环所需的引擎操作 (GB/GBC 复用同一套退出确认 + 运行循环).
 * 两引擎的 joypad 掩码/暂停/恢复/停止语义一致, 仅调用目标不同. */
typedef struct {
    void (*pause)(void);
    void (*resume)(void);
    void (*set_joypad)(uint8_t);
    esp_err_t (*stop)(void);
} game_run_engine_ops_t;

static const game_run_engine_ops_t ops_engine_gb = {
    gb_emu_pause, gb_emu_resume, gb_emu_set_joypad, gb_emu_stop,
};
static const game_run_engine_ops_t ops_engine_gbc = {
    gbc_emu_pause, gbc_emu_resume, gbc_emu_set_joypad, gbc_emu_stop,
};
static const game_run_engine_ops_t ops_engine_nes = {
    nes_emu_pause, nes_emu_resume, nes_emu_set_joypad, nes_emu_stop,
};
static const game_run_engine_ops_t ops_engine_arduboy = {
    arduboy_avr_pause, arduboy_avr_resume, arduboy_avr_set_joypad, arduboy_avr_stop,
};

/* === V1.0.52: 统一加载进度条 (GB/GBC/电子词典共用布局) ===
 * 参考电子词典 boot_progress_cb:
 *   文件名/状态文字     y=105 居中
 *   进度条 300x16       (50,140)
 *   百分比              y=165 居中
 * 首次调用全屏绘制框架, 后续按百分比增量填充进度条并更新百分比. */
static int            s_loading_pct = -1;
static char           s_loading_text[64] = {0};
static st7305_handle_t *s_loading_lcd = NULL;

static void loading_screen_draw(int percent, const char *text)
{
    st7305_handle_t *lcd = s_loading_lcd;
    if (!lcd) return;
    const int bar_x = 50, bar_y = 140, bar_w = 300, bar_h = 16;

    if (s_loading_pct < 0) {
        st7305_clear(lcd, ST7305_COLOR_WHITE);
        /* 进度条框 */
        for (int y = 0; y < bar_h; y++) {
            st7305_draw_pixel(lcd, bar_x, bar_y + y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, bar_x + bar_w - 1, bar_y + y, ST7305_COLOR_BLACK);
        }
        for (int x = 0; x < bar_w; x++) {
            st7305_draw_pixel(lcd, bar_x + x, bar_y, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, bar_x + x, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
        }
        /* 文件名 */
        if (text && text[0]) draw_text_centered(lcd, 105, text, false);
        s_loading_pct = 0;
    }

    if (percent > s_loading_pct) {
        int old_fill = (bar_w - 4) * s_loading_pct / 100;
        int new_fill = (bar_w - 4) * percent / 100;
        for (int y = 2; y < bar_h - 2; y++) {
            for (int x = old_fill; x < new_fill; x++) {
                st7305_draw_pixel(lcd, bar_x + 2 + x, bar_y + y, ST7305_COLOR_BLACK);
            }
        }
        s_loading_pct = percent;
        /* 清除旧百分比 (居中 40 宽区域) 并绘制新百分比 */
        for (int dy = 0; dy < 14; dy++) {
            for (int dx = 0; dx < 40; dx++) {
                st7305_draw_pixel(lcd, 180 + dx, 165 + dy, ST7305_COLOR_WHITE);
            }
        }
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        draw_text_centered(lcd, 165, pct, false);
        st7305_flush(lcd);
    }
}

/* 从完整路径提取文件名 (不含目录) */
static void loading_set_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    s_loading_text[0] = 0;
    if (base) {
        snprintf(s_loading_text, sizeof(s_loading_text), "%s", base + 1);
    } else {
        snprintf(s_loading_text, sizeof(s_loading_text), "%s", path);
    }
}

static void gb_load_progress_cb(int percent)  { loading_screen_draw(percent, s_loading_text); }
static void gbc_load_progress_cb(int percent) { loading_screen_draw(percent, s_loading_text); }

static game_exit_result_t game_exit_confirm_dialog(menu_state_t *state, const game_run_engine_ops_t *ops);
static void game_run_loop(menu_state_t *state, const game_run_engine_ops_t *ops);
/* 收藏过滤/游戏扫描按当前引擎扩展名的辅助函数 (定义在 select_game_build 之前) */
static bool is_page_game_file(const char *path, const menu_state_t *state);
static const char *current_gb_ext(const menu_state_t *state);
static const char *platform_root_dir(int engine);
static const char *platform_title(int engine);
/* 各 console 引擎独立灰度 (定义在 config 段, 供游戏启动/设置提前使用) */
static uint8_t engine_gray_get(int engine);
static void engine_gray_set(int engine, uint8_t v);
/* 当前页面/引擎对应的收藏引擎 ID (定义在 select_game_build 之前) */
static fav_engine_t state_fav_engine(const menu_state_t *state);

static bool select_game_on_confirm(menu_state_t *state, int idx) {
    /* === 侧栏特殊项: 在右栏有内容时按下确认, 触发对应动作 === */
    if (state->select_folder_idx == 0 && state->select_focus == 1) {
        /* 游戏设置 + 右栏: 调用 game_settings_on_confirm
         *  - idx 0 (映射按键): 跳转到 KEY_CONFIG 页面
         *  - idx 1/2/3 (全屏/声音/状态栏): 单击立即翻转 */
        return game_settings_on_confirm(state, idx);
    }
    if (idx >= 0 && idx < g_sub_count) {
        /* 拿到游戏完整路径 */
        char path[160];
        const char *p = NULL;
        if (state->select_folder_idx == 1) {
            /* 收藏: 路径直接存放在 favorites_list() (按页面模式过滤, 与右栏列表序号一致) */
            int fav_count = 0;
            const char *const *favs = favorites_list(state_fav_engine(state), &fav_count);
            int matched = 0;
            for (int i = 0; i < fav_count; i++) {
                const char *fp = favs[i];
                if (!fp) continue;
                if (!is_page_game_file(fp, state)) continue;
                if (matched == idx) {
                    strncpy(path, fp, sizeof(path) - 1);
                    path[sizeof(path) - 1] = '\0';
                    p = path;
                    break;
                }
                matched++;
            }
        } else {
            /* 真实子文件夹: 根据 folder + idx 构造路径 (按页面+平台选目录) */
            const char *folder = get_selected_folder_name(state, state->select_folder_idx);
            if (state->select_mode == 1) {
                p = platform_game_path(platform_root_dir(state->select_engine), folder, idx,
                                       current_gb_ext(state), path, sizeof(path));
            } else {
                p = bbk_game_path_in_folder(folder, idx, path, sizeof(path));
            }
        }
        if (p == NULL) {
            ESP_LOGE(TAG, "无法找到游戏路径 (folder_idx=%d, idx=%d)", state->select_folder_idx, idx);
            menu_show_fail_hint(state, "无法定位游戏");
            return true;
        }
        ESP_LOGI(TAG, "启动游戏: %s", p);

        /* V1.0.46: 按页面模式启动 (0=电子词典 gam4980, 1=console 页) */
        if (state->select_mode == 1) {
            /* V1.0.53: NES (nofrendo) — 从 /sdcard/nes 的 .nes 游戏加载并启动 */
            if (state->select_engine == 2) {
                /* V1.0.52: 统一加载进度条 (文件名 + 进度条 + 百分比) */
                s_loading_lcd = state->lcd;
                s_loading_pct = -1;
                loading_set_basename(p);
                nes_emu_set_progress_cb(gb_load_progress_cb);  /* 回调签名一致, 复用 */
                esp_err_t ret = nes_emu_start(p);
                nes_emu_set_progress_cb(NULL);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "NES 启动失败: %s", esp_err_to_name(ret));
                    menu_show_fail_hint(state, "NES 启动失败");
                    return true;
                }
                /* 补满进度条 + 同步设置 */
                loading_screen_draw(100, s_loading_text);
                nes_emu_set_fullscreen(state->game_display_mode > 0);
                /* 同步 NES 引擎独立的模拟灰度开关 (0=关/纯黑白, 1=开/灰度) */
                board_shim_set_gb_gray(engine_gray_get(state->select_engine));
                /* 运行循环 (阻塞直到退出) */
                game_run_loop(state, &ops_engine_nes);
                ESP_LOGI(TAG, "NES 游戏退出, 返回菜单");
                state->needs_redraw = true;
                return true;
            }
            /* V1.0.53: Arduboy 引擎 (simavr ATmega32u4) — 从 AB 目录的 .hex 游戏加载并启动 */
            if (state->select_engine == 3) {
                arduboy_avr_set_fullscreen((int)state->game_display_mode);
                esp_err_t ret = arduboy_avr_start(p);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Arduboy 启动失败: %s", esp_err_to_name(ret));
                    menu_show_fail_hint(state, "Arduboy 启动失败");
                    return true;
                }
                /* 运行循环 (阻塞直到退出) */
                game_run_loop(state, &ops_engine_arduboy);
                ESP_LOGI(TAG, "Arduboy 游戏退出, 返回菜单");
                state->needs_redraw = true;
                return true;
            }
            /* V1.0.47: GBC 引擎 (gnuboy) — 走 gbc_emu_start 直接读 ROM 并启动任务 */
            if (state->select_engine == 1) {
                /* V1.0.52: 统一加载进度条 (文件名 + 进度条 + 百分比) */
                s_loading_lcd = state->lcd;
                s_loading_pct = -1;
                loading_set_basename(p);
                gbc_emu_set_progress_cb(gbc_load_progress_cb);
                esp_err_t ret = gbc_emu_start(p);
                gbc_emu_set_progress_cb(NULL);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "GBC 启动失败: %s", esp_err_to_name(ret));
                    menu_show_fail_hint(state, "GBC 启动失败");
                    return true;
                }
                /* 补满进度条 */
                loading_screen_draw(100, s_loading_text);
                /* 同步 GBC 设置: 全屏 + 模拟灰度 + 音量 */
                gbc_emu_set_fullscreen((int)state->game_display_mode);
                board_shim_set_gb_gray(engine_gray_get(state->select_engine));
                /* 运行循环 (阻塞直到退出) */
                game_run_loop(state, &ops_engine_gbc);
                ESP_LOGI(TAG, "GBC 游戏退出, 返回菜单");
                state->needs_redraw = true;
                return true;
            }
            /* === GB 游戏 === */
            /* V1.0.52: 统一加载进度条 (文件名 + 进度条 + 百分比) */
            s_loading_lcd = state->lcd;
            s_loading_pct = -1;
            loading_set_basename(p);
            gb_emu_set_progress_cb(gb_load_progress_cb);
            loading_screen_draw(5, s_loading_text);
            gb_emu_rom_t rom;
            esp_err_t ret = gb_emu_load_rom(p, &rom);
            gb_emu_set_progress_cb(NULL);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "GB ROM 加载失败: %s", esp_err_to_name(ret));
                menu_show_fail_hint(state, "GB ROM 加载失败");
                return true;
            }
            gb_emu_log_rom_info(&rom);
            ret = gb_emu_start(&rom);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "GB 模拟器启动失败: %s", esp_err_to_name(ret));
                gb_emu_free_rom(&rom);
                menu_show_fail_hint(state, "GB 启动失败");
                return true;
            }
            /* 同步 GB 设置: 全屏 + 模拟灰度 */
            gb_emu_set_fullscreen((int)state->game_display_mode);
            board_shim_set_gb_gray(engine_gray_get(state->select_engine));
            /* V1.0.52: 加载完成, 进度条补满 + 100% */
            loading_screen_draw(100, s_loading_text);
            /* 运行循环 (阻塞直到退出) */
            game_run_loop(state, &ops_engine_gb);
            gb_emu_free_rom(&rom);
            ESP_LOGI(TAG, "GB 游戏退出, 返回菜单");
            state->needs_redraw = true;
            return true;
        }

        /* === 电子词典 (gam4980) === */
        /* V1.0.68 fix: 不再用 static emu_inited 缓存跳过初始化 —
         * gam4980_emu_unload (退出词典页时) 会释放 EPX 缓冲并 retro_deinit,
         * 缓存导致二次进入跳过 init → s_epx_buf 永久 NULL → 抗锯齿消失.
         * gam4980_emu_init 本身幂等: 内部按 g_core_initialized 判断,
         * EPX 缓冲按需重新分配. */
        esp_err_t r = gam4980_emu_init(state->lcd);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "emu 初始化失败: %s", esp_err_to_name(r));
            menu_show_fail_hint(state, "初始化错误");
            return true;
        }

        int rc = gam4980_emu_load(path);
        if (rc != 0) {
            ESP_LOGE(TAG, "游戏加载失败: %d", rc);
            menu_show_fail_hint(state, "缺8.BIN");
            return true;
        }
        ESP_LOGI(TAG, "游戏加载成功, 进入运行循环");

        /* 存档恢复由 gam4980_emu_load() 内部自动处理 (在 retro_load_game 之后调用
         * gam4980_emu_load_state), 这里不需要再调用. V1.0.7 之前版本曾直接调用
         * load_state 但因为存档 80KB 错误覆盖了游戏数据区 (sys_flash[0x8000+]) 导致
         * 游戏 BRK 卡死; V1.0.8 修复后存档只覆盖 32KB 真正存档区 (sys_flash[0..0x7FFF]),
         * 不会再破坏游戏数据, 由 gam4980_emu_load 内部统一管理调用时机. */
        
        /* 传递状态栏信息给游戏 */
        gam4980_set_status_info(state->settings.battery, state->settings.pad_connected);
        /* 同步游戏显示模式: 全屏 or 点对点 */
        gam4980_set_fullscreen(state->game_display_mode > 0);
        /* V1.0.46: 同步画面优化开关 */
        gam4980_set_pic_opt(state->game_pic_opt);
        /* V1.0.68: 进入 BBK 游戏禁用手柄导航键 (与 GB 流程一致), 否则游戏内
         * 按手柄 B (F_BACK, 游戏键) 会被 input_get_action 误判为返回/退出. */
        input_set_gamepad_nav_enabled(false);
        /* 启动游戏循环 (阻塞直到游戏退出) */
        game_exit_result_t exit_result = gam4980_emu_run();
        input_set_gamepad_nav_enabled(true);

        ESP_LOGI(TAG, "游戏退出, 返回菜单");
        /* 确认退出 → 返回对应游戏二级菜单, 方便继续选择游戏;
         * 无操作 10s 超时 → 直接返回桌面. */
        state->current_page = (exit_result == GAME_EXIT_TIMEOUT) ? MENU_PAGE_MAIN : MENU_PAGE_SELECT_GAME;
        state->needs_redraw = true;
        /* 超时直接返回桌面: 卸载引擎释放内存 (确认退出则留在二级菜单, 保留引擎) */
        if (exit_result == GAME_EXIT_TIMEOUT)
            engine_manager_unload_all();
    }
    return true;
}

/* ============ GB/GBC 游戏 (单列列表, 复用 render_sub 渲染) ============ */

/* 游戏退出确认弹窗: 暂停游戏, 在画面上叠加确认框.
 * 返回 true=确认退出, false=取消(恢复游戏继续).
 * 交互: 确认键=退出, BACK/HOME=取消恢复游戏. */
static game_exit_result_t game_exit_confirm_dialog(menu_state_t *state, const game_run_engine_ops_t *ops) {
    ops->pause();  /* 冻结画面, 供叠加确认框 */

    /* 小弹窗 (draw_notice_popup 样式, 同收藏提示): 显示"退出游戏?"
     * 确认键=退出, BACK/HOME=取消恢复游戏, 无按键 10s 超时=自动退出到桌面.
     * 等待循环里每轮重绘弹窗, 保证即便被游戏帧 flush 覆盖一次也会立即恢复,
     * 等效于把弹窗优先级提到最高, 始终停留在最上层. */

    bool back_pressed = false;
    uint32_t back_press_start_ms = 0;
    extern uint32_t esp_log_timestamp(void);
    uint32_t start_ms = (uint32_t)esp_log_timestamp();

    /* 手柄确认/返回键在进入 GB 游戏时被 input_set_gamepad_nav_enabled(false) 禁用,
     * 因此 input_get_action() 读不到 F_CONFIRM / F_BACK. 这里直接查询原始电平,
     * 并做"上升沿"检测(从松开→按下才触发), 避免弹窗循环 16ms 一轮导致按住重复.
     * 初始记录当前原始状态, 防止"请求退出时仍按着的键"被误判为一次新按下. */
    bool confirm_pressed = false;   /* 上一次 F_CONFIRM 原始电平 */
    bool cancel_pressed = false;    /* 上一次 F_BACK 取消使用的原始电平 */
    {
        bool c = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_CONFIRM) : false;
        confirm_pressed = c;
    }
    {
        bool c = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_BACK) : false;
        cancel_pressed = c;
    }

    while (1) {
        /* 每轮强制重绘+刷新, 使弹窗保持可见 (弹窗优先级最高) */
        draw_notice_popup(state->lcd, "\xe9\x80\x80\xe5\x87\xba\xe6\xb8\xb8\xe6\x88\x8f\xef\xbc\x9f"); /* 退出游戏？ */
        st7305_flush(state->lcd);

        menu_action_t action = input_get_action();

        /* 物理键 (KEY 短按=CONFIRM, BOOT 短按=BACK), 不受 gamepad_nav 影响 */
        if (action == MENU_ACTION_CONFIRM) {
            return GAME_EXIT_CONFIRMED;
        }
        if (action == MENU_ACTION_BACK) {
            /* V1.0.68: 触摸底部上滑再次出现 = 确认返回;
             * 物理 BACK 键 = 取消恢复游戏 */
            if (input_touch_last_action()) return GAME_EXIT_CONFIRMED;
            ops->resume();
            return GAME_EXIT_CANCEL;
        }
        if (action == MENU_ACTION_HOME) {
            ops->resume();
            return GAME_EXIT_CANCEL;
        }

        uint32_t now_ms = (uint32_t)esp_log_timestamp();

        /* 手柄 F_CONFIRM 上升沿 → 确认退出.
         * 注意: F_EXIT (返回菜单键) 只负责"请求退出", 不在此确认退出,
         * 否则请求退出的同一按键按下时仍被按住, 进弹窗后立即被当成"确认"而跳过确认. */
        bool now_confirm = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_CONFIRM) : false;
        if (now_confirm && !confirm_pressed) {
            ESP_LOGI(TAG, "[GB] 退出确认: 手柄确认键退出");
            return GAME_EXIT_CONFIRMED;
        }
        confirm_pressed = now_confirm;

        /* 手柄 F_BACK 短按上升沿 → 取消恢复游戏; 长按 500ms → 取消 */
        bool now_back = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_BACK) : false;
        if (now_back && !cancel_pressed) {
            /* 短按: 立即取消 */
            ops->resume();
            return GAME_EXIT_CANCEL;
        }
        cancel_pressed = now_back;
        if (now_back && !back_pressed) {
            back_pressed = true;
            back_press_start_ms = now_ms;
        } else if (!now_back) {
            back_pressed = false;
        } else if (back_pressed && (uint32_t)(now_ms - back_press_start_ms) >= 500) {
            ops->resume();
            return GAME_EXIT_CANCEL;
        }

        /* 超时 (无按键) → 自动退出到桌面 */
        if ((uint32_t)(now_ms - start_ms) >= 10000) {
            return GAME_EXIT_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void game_run_loop(menu_state_t *state, const game_run_engine_ops_t *ops) {
    input_set_gamepad_nav_enabled(false);
    /* 游戏运行期间暂停 SD 扫描/重挂, 避免 SDMMC 重挂与游戏读 SD 并发崩溃 */
    sd_watcher_set_paused(true);
    /* V1.0.68: 游戏内屏幕虚拟按键 (游戏设置里开启) */
    virtual_keys_set_enabled(state->game_virtual_keys);

    /* 处理退出动作: 先暂停游戏并弹出确认框, 用户确认后才真正退出.
     * 用 s_gb_exit_requested 标记"已请求退出", 在主循环统一处理,
     * 避免在按键轮询中途直接 break 导致无法叠加确认框. */
    bool exit_requested = false;
    game_exit_result_t exit_result = GAME_EXIT_CONFIRMED;
    bool prev_exit = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_EXIT) : false;

    while (1) {
        /* V1.0.68: 软关机键 GPIO1 长按 2 秒软关机 */
        if (input_power_should_sleep()) {
            menu_soft_power_off(state->lcd);
        }
        uint8_t j = input_get_held_gb_joypad();
        ops->set_joypad(j);

        /* 游戏壁纸模式: 任意设备按键 → 强制退出 (无确认框).
         * 物理键/手柄导航由 input_get_action 提供 (边沿);
         * 手柄全部 8 个逻辑键用上升沿掩码覆盖, 实现"任意键强退". */
        if (s_wallpaper_game_mode) {
            menu_action_t wp_action = input_get_action();
            uint16_t wp_keys = 0;
            if (bt_manager_is_connected()) {
                const func_t wp_all[] = { F_UP, F_DOWN, F_LEFT, F_RIGHT,
                                          F_CONFIRM, F_BACK, F_EXIT, F_FAV };
                for (int k = 0; k < 8; k++) {
                    if (bt_manager_is_key_pressed(wp_all[k]))
                        wp_keys |= (uint16_t)(1u << k);
                }
            }
            if (!s_wp_key_inited) {
                s_wp_key_inited = true;
                s_wp_prev_keys = wp_keys;   /* 忽略进入时已按住的键 */
            }
            if (wp_action != MENU_ACTION_NONE || (wp_keys & ~s_wp_prev_keys)) {
                exit_result = GAME_EXIT_CONFIRMED;
                break;
            }
            s_wp_prev_keys = wp_keys;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        /* 1) 物理键 action (BOOT 长按 BACK, KEY 长按 LONG_LEFT → 都视为请求退出) */
        menu_action_t action = input_get_action();
        /* V1.0.68: HOME (状态栏长按 3s) / POWER_RELEASE (软关机键0.5s后松手) /
         * POWER_LOCK (软关机键点按) → 立即退出返回主菜单 (无确认框) */
        if (action == MENU_ACTION_HOME || action == MENU_ACTION_POWER_RELEASE ||
            action == MENU_ACTION_POWER_LOCK) {
            exit_result = GAME_EXIT_TIMEOUT;
            break;
        }
        if (action == MENU_ACTION_BACK || action == MENU_ACTION_LONG_LEFT) {
            exit_requested = true;
        }

        /* 2) 手柄 L2=F_EXIT (返回菜单键) 上升沿 → 请求退出
         *    因为 input_set_gamepad_nav_enabled(false), input_get_action 的 nav 循环被跳过,
         *    所以必须在这里独立查询, 否则 L2 完全无效. */
        bool now_exit = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_EXIT) : false;
        if (!prev_exit && now_exit) {
            exit_requested = true;
            prev_exit = true;
        }
        prev_exit = now_exit;

        /* 3) 不再用手柄 F_BACK 长按退出: 返回(B) 是纯游戏键.
         *    退出只用独立特殊键 F_EXIT 或设备物理键 (见上方 1/2). */

        /* 请求退出: 弹确认框, 确认则退出, 取消则恢复游戏继续 */
        if (exit_requested) {
            exit_result = game_exit_confirm_dialog(state, ops);
            if (exit_result == GAME_EXIT_CANCEL) {
                /* 取消: 恢复游戏, 清除退出请求并继续 */
                exit_requested = false;
                prev_exit = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_EXIT) : false;
            } else {
                break;  /* 退出 (确认返回二级菜单, 超时返回桌面) */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }

    ops->stop();

    virtual_keys_set_enabled(false);
    input_set_gamepad_nav_enabled(true);
    sd_watcher_set_paused(false);
    /* 游戏壁纸退出: 一律返回主菜单并卸载引擎 (等效退出二级菜单) */
    if (s_wallpaper_game_mode) {
        s_wallpaper_game_mode = false;
        exit_result = GAME_EXIT_TIMEOUT;
    }
    /* 确认退出 → 返回对应游戏二级菜单, 方便继续选择游戏;
     * 无操作 10s 超时 → 直接返回桌面. */
    state->current_page = (exit_result == GAME_EXIT_TIMEOUT) ? MENU_PAGE_MAIN : MENU_PAGE_GB_GAME;
    st7305_clear(state->lcd, ST7305_COLOR_WHITE);
    state->needs_redraw = true;
    /* 超时直接返回桌面: 卸载引擎释放内存 (确认退出则留在二级菜单, 保留引擎) */
    if (exit_result == GAME_EXIT_TIMEOUT)
        engine_manager_unload_all();
}


/* 启动游戏壁纸: 按引擎启动对应模拟器, 任意设备按键强制退出, 退出后卸载引擎回主菜单 */
static void start_wallpaper_game(menu_state_t *state, int engine, const char *path) {
    if (!path || !path[0]) return;
    s_wallpaper_game_mode = true;
    if (engine >= 0 && engine < 4) {
        if (engine == 2) {  /* NES */
            s_loading_lcd = state->lcd;
            s_loading_pct = -1;
            loading_set_basename(path);
            nes_emu_set_progress_cb(gb_load_progress_cb);
            esp_err_t ret = nes_emu_start(path);
            nes_emu_set_progress_cb(NULL);
            if (ret == ESP_OK) {
                loading_screen_draw(100, s_loading_text);
#if WALLPAPER_TEST_1X
                nes_emu_set_fullscreen(false);
#else
                nes_emu_set_fullscreen(state->game_display_mode > 0);
#endif
                board_shim_set_gb_gray(engine_gray_get(engine));
                game_run_loop(state, &ops_engine_nes);
            }
        } else if (engine == 3) {  /* arduboy */
#if WALLPAPER_TEST_1X
            arduboy_avr_set_fullscreen(0);
#else
            arduboy_avr_set_fullscreen((int)state->game_display_mode);
#endif
            if (arduboy_avr_start(path) == ESP_OK) {
                game_run_loop(state, &ops_engine_arduboy);
            }
        } else if (engine == 1) {  /* GBC */
            s_loading_lcd = state->lcd;
            s_loading_pct = -1;
            loading_set_basename(path);
            gbc_emu_set_progress_cb(gbc_load_progress_cb);
            esp_err_t ret = gbc_emu_start(path);
            gbc_emu_set_progress_cb(NULL);
            if (ret == ESP_OK) {
                loading_screen_draw(100, s_loading_text);
#if WALLPAPER_TEST_1X
                gbc_emu_set_fullscreen(0);
#else
                gbc_emu_set_fullscreen((int)state->game_display_mode);
#endif
                board_shim_set_gb_gray(engine_gray_get(engine));
                game_run_loop(state, &ops_engine_gbc);
            }
        } else {  /* GB */
            s_loading_lcd = state->lcd;
            s_loading_pct = -1;
            loading_set_basename(path);
            gb_emu_set_progress_cb(gb_load_progress_cb);
            loading_screen_draw(5, s_loading_text);
            gb_emu_rom_t rom;
            esp_err_t ret = gb_emu_load_rom(path, &rom);
            gb_emu_set_progress_cb(NULL);
            if (ret == ESP_OK) {
                ret = gb_emu_start(&rom);
                if (ret == ESP_OK) {
#if WALLPAPER_TEST_1X
                    gb_emu_set_fullscreen(0);
#else
                    gb_emu_set_fullscreen((int)state->game_display_mode);
#endif
                    board_shim_set_gb_gray(engine_gray_get(engine));
                    loading_screen_draw(100, s_loading_text);
                    game_run_loop(state, &ops_engine_gb);
                }
                gb_emu_free_rom(&rom);
            }
        }
        s_wallpaper_game_mode = false;   /* game_run_loop 正常路径已清, 兜底 */
    } else {  /* BBK */
        gam4980_emu_init(state->lcd);
        if (gam4980_emu_load(path) == 0) {
            gam4980_set_status_info(state->settings.battery, state->settings.pad_connected);
            gam4980_set_fullscreen(state->game_display_mode > 0);
            gam4980_set_pic_opt(state->game_pic_opt);
            gam4980_set_wallpaper_mode(true);
            gam4980_emu_run();
            gam4980_set_wallpaper_mode(false);
        }
        gam4980_emu_unload();
    }
    /* 退出后: 回主菜单, 卸载引擎释放内存 (等效退出二级菜单) */
    state->current_page = MENU_PAGE_MAIN;
    state->needs_redraw = true;
    st7305_clear(state->lcd, ST7305_COLOR_WHITE);
    engine_manager_unload_all();
}

static void bt_device_found(bt_device_t *results, int count, bool updated);

/* 历史设备匹配查找结果 (用于主动连接模式) */
typedef struct {
    int      idx;        /* 在 bt_devices[] 中的下标 */
    int8_t   rssi;       /* RSSI 值 (负数, 越接近 0 越强) */
} auto_match_t;

/* 在历史记录中查找设备: 匹配 MAC, 或 (MAC 随机化时) 匹配名称.
 * 部分 BLE 手柄每次连接会随机化 MAC (地址变、名称不变), 仅靠 MAC 会"一直连不上". */
static bool bt_history_has_mac(const bt_device_t *dev) {
    extern int bt_manager_get_history_count(void);
    extern const bt_device_t *bt_manager_get_history_at(int index);
    int hn = bt_manager_get_history_count();
    for (int i = 0; i < hn; i++) {
        const bt_device_t *h = bt_manager_get_history_at(i);
        if (!h) continue;
        if (memcmp(h->bd_addr, dev->bd_addr, 6) == 0) return true;
        if (h->has_name && dev->has_name && h->name[0] != '\0'
            && strcmp(h->name, dev->name) == 0) return true;
    }
    return false;
}

static void bt_device_found(bt_device_t *results, int count, bool updated) {
    bool silent_retry = g_menu.bt_retry_scan_active;
    if (!g_menu.bt_scan_active && !silent_retry) return;
    int copy_count = count < 20 ? count : 20;

    /* 只在设备数量变化时触发重绘, 避免频繁闪烁 (静默重连不刷新 UI) */
    bool count_changed = (copy_count != g_menu.bt_device_count);

    if (g_menu.bt_scan_active) {
        for (int i = 0; i < copy_count; i++) {
            memcpy(&g_menu.bt_devices[i], &results[i], sizeof(bt_device_t));
        }
        g_menu.bt_device_count = copy_count;

        /* 新设备加入才重绘; RSSI 更新不重绘 (避免闪烁) */
        if (count_changed) {
            g_menu.needs_redraw = true;
        }
    }

    /* === 后台静默重连: 扫描阶段匹配历史 MAC, 无 UI === */
    if (silent_retry &&
        copy_count > 0 &&
        !bt_manager_is_connected() &&
        bt_manager_is_scanning()) {
        auto_match_t best = { -1, -127 };
        for (int i = 0; i < copy_count; i++) {
            if (bt_history_has_mac(&results[i])) {
                if (results[i].rssi > best.rssi) {
                    best.idx = i;
                    best.rssi = results[i].rssi;
                }
            }
        }
        if (best.idx >= 0) {
            const char *name = bt_manager_get_device_name(&results[best.idx]);
            ESP_LOGI(TAG, "后台静默扫描: 找到历史设备 %s (RSSI=%d), 发起连接",
                     name ? name : "?", best.rssi);
            bt_manager_stop_scan();
            g_menu.bt_retry_scan_active = false;
            g_menu.bt_retry_scan_until_ms = 0;
            g_menu.bt_retry_connect_pending = true;
            bt_device_t copy = results[best.idx];
            bt_manager_connect_device(&copy);
        }
        return;
    }

    if (!g_menu.bt_scan_active) return;

    /* === 主动连接模式: 扫描结果中匹配历史记录 MAC 的设备 ===
     * 流程:
     *   1. 遍历当前扫描结果, 找出在历史记录中存在的设备
     *   2. 选 RSSI 最强的一个 (rssi 越接近 0 越强)
     *   3. 自动调用 bt_manager_connect_device 主动连接
     *   4. 标记当前目标设备名, 后续弹窗显示"正在连接 X"
     * 防止重复: 仅在 bt_auto_connect_target 为空时执行连接 (成功/失败后再清零) */
    if (g_menu.bt_auto_connect_active &&
        g_menu.bt_auto_connect_target[0] == '\0' &&
        copy_count > 0 &&
        !bt_manager_is_connected() &&
        bt_manager_is_scanning()) {  /* 仅在扫描进行中才触发自动连接 */
        auto_match_t best = { -1, -127 };
        int found_count = 0;
        for (int i = 0; i < copy_count; i++) {
            if (bt_history_has_mac(&g_menu.bt_devices[i])) {
                found_count++;
                if (g_menu.bt_devices[i].rssi > best.rssi) {
                    best.idx = i;
                    best.rssi = g_menu.bt_devices[i].rssi;
                }
            }
        }
        g_menu.bt_auto_connect_found = found_count;
        if (best.idx >= 0) {
            const char *name = bt_manager_get_device_name(&g_menu.bt_devices[best.idx]);
            if (!name) name = "";
            snprintf(g_menu.bt_auto_connect_target,
                     sizeof(g_menu.bt_auto_connect_target), "%s", name);
            ESP_LOGI(TAG, "主动连接: 找到历史设备 %s (RSSI=%d), 发起连接",
                     g_menu.bt_auto_connect_target, best.rssi);
            /* 居中小弹窗"正在连接" (用户需求: 连接蓝牙时显示, 居中对齐)
             * 由 bt_connect_callback 切换到"连接成功"或关闭。
             * bt_stop_scan+扫描关闭保留(扫描与 GATTC 并发是首次连接必败根因)。
             * bt_connect_awaiting 用于区分"连接失败"与"正常断开"。 */
            bt_manager_stop_scan();
            g_menu.bt_scan_active = false;
            g_menu.bt_device_count = 0;
            g_menu.selected_index = 0;
            g_menu.scroll_offset = 0;
            g_menu.bt_connect_awaiting = true;
            g_menu.connecting_popup_active = true;
            g_menu.connecting_popup_success = false;
            uint32_t t_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            /* V1.0.40: 5 秒硬超时 (正常 3 秒内连上, 超过 5 秒视为失败) */
            g_menu.connecting_popup_until_ms = t_now + 5000;
            g_menu.connecting_popup_started_at_ms = t_now;
            /* 实际发起连接 (异步, 结果由 bt_connect_callback 更新) */
            bt_manager_connect_device(&g_menu.bt_devices[best.idx]);
            g_menu.needs_redraw = true;
        } else {
            /* 还没找到: 强制重绘以更新 "已发现 N 个" 计数 */
            if (found_count > 0) g_menu.needs_redraw = true;
        }
    }
}

static void draw_bt_scan_dialog(menu_state_t *state) {
    if (!state->bt_scan_active) return;
    st7305_handle_t *lcd = state->lcd;

    /* 主动连接模式: 显示"正在搜索"提示, 实时更新已发现的历史设备数
     * 找到设备后由 bt_device_found 切换到"正在连接 X"提示并关闭扫描弹窗 */
    if (state->bt_auto_connect_active) {
        /* 统一窗口模板: 离屏幕上下左右各 25 像素 (与 list_dialog 一致) */
        int w = SCREEN_W - 50;
        int h = SCREEN_H - 50;
        int x = 25, y = 25;

        /* 白底 */
        for (int dy = 0; dy < h; dy++) {
            for (int dx = 0; dx < w; dx++) {
                st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
            }
        }
        /* 黑色边框 (3 像素) */
        for (int k = 0; k < 3; k++) {
            for (int dx = 0; dx < w; dx++) {
                st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
                st7305_draw_pixel(lcd, x + dx, y + h - 1 - k, ST7305_COLOR_BLACK);
            }
            for (int dy = 0; dy < h; dy++) {
                st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
                st7305_draw_pixel(lcd, x + w - 1 - k, y + dy, ST7305_COLOR_BLACK);
            }
        }

        /* 标题: 搜索主动连接设备 */
        draw_text_centered(lcd, y + 24, "搜索主动连接设备", false);
        draw_hline(lcd, x + 10, x + w - 10, y + 48, ST7305_COLOR_BLACK);

        /* 第一行: 正在扫描附近手柄... (自动换行, 避免超出弹窗) */
        char line1[64];
        snprintf(line1, sizeof(line1), "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f\xe9\x99\x84\xe8\xbf\x91\xe6\x89\x8b\xe6\x9f\x84...");  /* "正在扫描附近手柄..." */
        draw_text_wrapped(lcd, x + 12, x + w - 12, y + 60, line1, false, 1, 24);

        /* 第二行: 已发现 N 个历史设备 (自动换行) */
        char line2[64];
        snprintf(line2, sizeof(line2), "已发现 %d 个历史设备",
                 state->bt_auto_connect_found);
        draw_text_wrapped(lcd, x + 12, x + w - 12, y + 92, line2, false, 1, 24);

        /* 底部: 返回取消 */
        draw_text_centered(lcd, y + h - 22, "\xe8\xbf\x94\xe5\x9b\x9e\xe5\x8f\x96\xe6\xb6\x88", false);
        return;
    }

    /* 先拷贝一份设备数据到本地 (避免 BTC 线程并发修改导致闪烁) */
    bt_device_t local_devs[20];
    int local_count = state->bt_device_count;
    if (local_count > 20) local_count = 20;
    for (int i = 0; i < local_count; i++) {
        memcpy(&local_devs[i], &state->bt_devices[i], sizeof(bt_device_t));
    }
    int local_sel = state->selected_index;
    if (local_sel >= local_count) local_sel = local_count - 1;
    if (local_sel < 0) local_sel = 0;

    /* 统一窗口模板: 离屏幕上下左右各 25 像素 (与 list_dialog / draw_confirm_dialog 一致) */
    int w = SCREEN_W - 50;
    int h = SCREEN_H - 50;
    int x = 25, y = 25;

    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
        }
    }

    for (int k = 0; k < 3; k++) {
        for (int dx = 0; dx < w; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + dx, y + h - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < h; dy++) {
            st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + w - 1 - k, y + dy, ST7305_COLOR_BLACK);
        }
    }

    draw_text_centered(lcd, y + 16, "扫描蓝牙设备", false);
    draw_hline(lcd, x + 10, x + w - 10, y + 40, ST7305_COLOR_BLACK);

    int list_y = y + 50;
    int list_h = h - 50 - 36;
    int line_h = 28;
    int max_visible = list_h / line_h;
    if (max_visible < 1) max_visible = 1;

    int total = local_count;
    int sel = local_sel;

    if (total == 0) {
        draw_text_centered(lcd, list_y + list_h / 2 - 8, "正在扫描蓝牙设备...", false);
    } else {
        if (sel < state->scroll_offset) {
            state->scroll_offset = sel;
        } else if (sel >= state->scroll_offset + max_visible) {
            state->scroll_offset = sel - max_visible + 1;
        }
        if (state->scroll_offset < 0) state->scroll_offset = 0;

        for (int i = 0; i < max_visible && i + state->scroll_offset < total; i++) {
            int item_idx = i + state->scroll_offset;
            int yy = list_y + i * line_h;
            bool is_selected = (item_idx == sel);

            const char *name = bt_manager_get_device_name(&local_devs[item_idx]);
            char line[64];
            /* 截断: 只显示名称, 超长时截断并在末尾加省略号 */
            int name_len = strlen(name);
            if (name_len > 28) {
                snprintf(line, 64, "%.28s...", name);
            } else {
                snprintf(line, 64, "%s", name);
            }

            if (is_selected) {
                fill_rect(lcd, x + 6, yy, x + w - 6 - 1, yy + line_h - 2, ST7305_COLOR_BLACK);
                draw_text_centered(lcd, yy + 4, line, true);
            } else {
                draw_text_centered(lcd, yy + 4, line, false);
            }
        }
    }

}

/* === 手柄菜单 + 按键映射 (list_dialog 单弹窗框架) ===
 * 用户需求 (V1.0.18 重构): 彻底删除之前散落在文件各处的 gamepad_build /
 *   gamepad_on_confirm / gamepad_dialog_on_select / start_key_mapping / 
 *   mapping_finish / mapping_cancel / bt_history_* 等旧实现, 统一到本区块内
 *   重新生成, 全部走 list_dialog 框架.
 *
 * 索引约定 (与 gamepad_list_build 严格对齐, 4 项):
 *   0: 添加设备  - 扫描手柄, 点设备自动连接, 成功自动跳按键映射
 *   1: 按键映射  - 8 键顺序映射 (上下左右 / 确认 / 返回 / 回到菜单 / 多功能键)
 *   2: 连接记录  - 嵌套 list_dialog 列出已配对设备, 按确定直接删除
 *   3: 返回      - 关闭弹窗, 回到主菜单
 *
 * 蓝牙默认开启, 无开关选项; 整个手柄子菜单没有独立全屏页, 全部经 list_dialog
 * 弹窗, 不再经过 sub_pages[MENU_PAGE_GAMEPAD] 全屏入口. */

/* V1.0.68: 10 个键的映射顺序 — 上下左右 / 确认 / 返回 / 返回菜单 / 多功能键 / Start / Select */
static const char *s_map_names[] = {
    "上", "下", "左", "右", "确认", "返回", "返回菜单", "多功能键", "Start", "Select"
};
static const func_t s_map_keys[] = {
    F_UP, F_DOWN, F_LEFT, F_RIGHT,
    F_CONFIRM, F_BACK, F_EXIT, F_FAV,
    F_START, F_SELECT
};
#define MAP_KEY_COUNT (sizeof(s_map_names) / sizeof(s_map_names[0]))

/* === 手柄子菜单 (list_dialog) === */
static int  gamepad_list_build(menu_state_t *state, char buf[][64], int max);
static void gamepad_list_on_select(menu_state_t *state, int idx);
static void gamepad_list_on_key(menu_state_t *state, int idx, menu_action_t action);
static void gamepad_list_open(menu_state_t *state);
static void gamepad_list_back(menu_state_t *state);

/* 动作函数 (由 gamepad_list_on_select 和 list_dialog_close 回调调用) */
static void gamepad_act_add_device(menu_state_t *state);
static void gamepad_act_keymap(menu_state_t *state);
static void gamepad_act_sup_keymap(menu_state_t *state);  /* V1.0.46: 补充按键映射启动 */
static void select_game_invalidate_cache(void);          /* 前向声明 (定义见下方) */
static void gamepad_act_history(menu_state_t *state);
static void gamepad_act_wifi(menu_state_t *state);       /* V1.0.67: WiFi 网页手柄 */

/* 连接记录弹窗回调 */
static void gamepad_history_on_select(menu_state_t *state, int idx);

/* === 列表构建 (唯一文本源) === */
static int gamepad_list_build(menu_state_t *state, char buf[][64], int max) {
    (void)max;
    int n = 0;
    snprintf(buf[n++], 64, "\xe6\xb7\xbb\xe5\x8a\xa0\xe8\xae\xbe\xe5\xa4\x87");  /* 添加设备 */
    snprintf(buf[n++], 64, "\xe6\x8c\x89\xe9\x94\xae\xe6\x98\xa0\xe5\xb0\x84");  /* 按键映射 */
    snprintf(buf[n++], 64, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95");  /* 连接记录 */
    snprintf(buf[n++], 64, "%sWiFi \xe6\x89\x8b\xe6\x9f\x84",                  /* WiFi 手柄 */
             web_gamepad_is_running() ? "* " : "");
    snprintf(buf[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");                            /* 返回 */
    (void)state;
    return n;
}

/* V1.0.39: 手柄配置弹窗方向键回调
 * - LEFT 长按 500ms → 直接触发 gamepad_act_add_device (跳过"按确定进添加设备"两步)
 * - 其他方向键 → 默认 wrap 行为 (与无 on_key 时一致) */
static void gamepad_list_on_key(menu_state_t *state, int idx, menu_action_t action) {
    (void)idx;
    if (action == MENU_ACTION_LONG_LEFT) {
        ESP_LOGI(TAG, "手柄配置: 长按LEFT 500ms → 直接扫描设备");
        /* 关闭弹窗 + 状态机重置 + 回到主菜单 (与 gamepad_list_on_select 一致) */
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_key = NULL;
        state->list_dialog_on_close = NULL;
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = state->main_selected_index;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        /* 触发添加设备 (直接扫描) */
        gamepad_act_add_device(state);
        return;
    }
    /* 默认 wrap: UP/LEFT 向上选, DOWN/RIGHT 向下选 */
    if (action == MENU_ACTION_UP || action == MENU_ACTION_LEFT) {
        if (state->list_dialog_selected > 0) {
            state->list_dialog_selected--;
        } else {
            state->list_dialog_selected = state->list_dialog_count - 1;
        }
    } else {
        if (state->list_dialog_selected < state->list_dialog_count - 1) {
            state->list_dialog_selected++;
        } else {
            state->list_dialog_selected = 0;
        }
    }
    state->needs_redraw = true;
    state->list_dialog_local_update = true;
}

/* === 2. 弹窗打开/关闭 === */
static void gamepad_list_open(menu_state_t *state) {
    int cnt = gamepad_list_build(state, state->list_dialog_items, 16);
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    list_dialog_open(state, "\xe6\x89\x8b\xe6\x9f\x84\xe9\x85\x8d\xe7\xbd\xae", cnt, gamepad_list_on_select);
    /* V1.0.39: 注册 on_key 回调, 用于 LEFT 长按 500ms 直接扫描设备 */
    state->list_dialog_on_key = gamepad_list_on_key;
}

/* === 3. 弹窗选中 (list_dialog 引擎调用) === */
static void gamepad_list_on_select(menu_state_t *state, int idx) {
    /* 关闭弹窗 + 弹窗 prev 状态机重置 + 回到主菜单 + 恢复位置 */
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
    state->current_page = MENU_PAGE_MAIN;
    state->selected_index = state->main_selected_index;
    state->scroll_offset = 0;
    state->needs_redraw = true;

    /* 触发对应动作: idx 0/1/2 = 动作, idx 3 = 返回 (弹窗已关) */
    switch (idx) {
    case 0: gamepad_act_add_device(state);  break;
    case 1: gamepad_act_keymap(state);      break;
    case 2: gamepad_act_history(state);     break;
    case 3: gamepad_act_wifi(state);        break;
    case 4: /* 返回 */                       break;
    default: break;
    }
}

/* === V1.0.67: WiFi 网页手柄 (AP 热点 + 手机浏览器虚拟手柄) === */
static void gamepad_act_wifi(menu_state_t *state) {
    ESP_LOGI(TAG, "点击 WiFi 手柄, 当前运行=%d", web_gamepad_is_running());
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (web_gamepad_is_running()) {
        web_gamepad_stop();
        snprintf(state->hint_text, sizeof(state->hint_text), "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad"); /* WiFi 手柄已关闭 */
    } else {
        if (web_gamepad_start() == ESP_OK) {
            snprintf(state->hint_text, sizeof(state->hint_text), "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf WiFi \xe6\x89\x8b\xe6\x9f\x84"); /* 已开启 WiFi 手柄 */
        } else {
            snprintf(state->hint_text, sizeof(state->hint_text), "WiFi \xe6\x89\x8b\xe6\x9f\x84\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5"); /* WiFi 手柄启动失败 */
        }
    }
    state->hint_until_ms = now + 500;   /* 0.5 秒 */
    state->needs_redraw = true;
}

/* === 4. 动作: 添加设备 (扫描 → 点选 → 自动连接 → 成功自动跳映射) === */
static void gamepad_act_add_device(menu_state_t *state) {
    if (!bt_manager_is_stack_ready()) {
        snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe8\x93\x9d\xe7\x89\x99");
        snprintf(state->confirm_msg, sizeof(state->confirm_msg),
                 "\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe4\xb8\xad\n\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x90\x8e\xe5\x86\x8d\xe8\xaf\x95");
        state->confirm_active = true;
        state->confirm_notice = true;
        state->needs_redraw = true;
        return;
    }
    state->bt_device_count = 0;
    state->bt_scan_active = true;
    state->selected_index = 0;
    state->scroll_offset = 0;
    bt_manager_start_scan_continuous(bt_device_found);
    state->needs_redraw = true;
}

/* === V1.0.41: 按键映射 — 小提示弹窗 (draw_notice_popup) 顺序映射 8 键 ===
 * 流程: gamepad_act_keymap 启动 → menu_poll_gamepad_mapping 每帧轮询物理输入
 *       → 检测到按键后写入 s_map, 显示"已映射: X" 0.5s → 8 键完成保存退出.
 * BACK 键随时取消 (不保存当前进度).
 * 显示文本: "请按下 [功能名] 对应的按键" / "已映射: 物理按键名" */

/* 启动按键映射: 从第 0 个功能(上)开始, 清空待消费边沿 */
static void gamepad_act_keymap(menu_state_t *state) {
    /* 关闭可能残留的弹窗 */
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->confirm_active = false;
    state->connecting_popup_active = false;
    state->current_page = MENU_PAGE_MAIN;
    state->selected_index = state->main_selected_index;
    state->scroll_offset = 0;
    /* 启动映射 */
    state->key_mapping_idx = 0;
    state->key_mapping_phase = false;  /* 等待按键 */
    state->key_mapping_until_ms = 0;
    bt_manager_poll_new_press_reset();
    input_set_gamepad_nav_enabled(false);  /* V1.0.41: 禁用手柄导航键, 防止干扰映射 */
    ESP_LOGI(TAG, "按键映射启动: 请按下 [上] 对应的按键");
    state->needs_redraw = true;
}

/* 每帧轮询: 检测物理按键输入, 推进映射进度 (需在 menu_render 前调用)
 * V1.0.41: 去掉"已映射"显示阶段, 检测到按键直接推进到下一个功能 */
void menu_poll_gamepad_mapping(menu_state_t *state) {
    if (state->key_mapping_idx < 0) return;  /* 未激活 */
    int f = state->key_mapping_idx;
    if (f < 0 || f >= FUNC_MAX) {
        state->key_mapping_idx = -1;
        return;
    }

    /* 等待按键态: 轮询是否有新的物理输入 */
    if (bt_manager_poll_new_press((func_t)f)) {
        ESP_LOGI(TAG, "按键映射: [%s] → %s",
                 bt_manager_func_name((func_t)f),
                 bt_manager_phys_name(bt_manager_get_key_map((func_t)f)));
        state->key_mapping_idx++;
        if (state->key_mapping_idx >= FUNC_MAX) {
            /* 8 键全部完成: 保存并退出 */
            bt_manager_save_key_map();
            ESP_LOGI(TAG, "按键映射完成, 已保存到 NVS");
            state->key_mapping_idx = -1;
            input_set_gamepad_nav_enabled(true);  /* 恢复手柄导航 */
        } else {
            /* 下一个功能: 重置边沿检测 */
            bt_manager_poll_new_press_reset();
            ESP_LOGI(TAG, "按键映射: 下一个 [%s]",
                     bt_manager_func_name((func_t)state->key_mapping_idx));
        }
        state->needs_redraw = true;
    }
}

/* === V1.0.46: 补充按键映射 (4 键: F/G/Shift/空格 → 功能1-4) ===
 * 与 8 键映射相似 (消费 HID 物理边沿), 但带"确认/跳过":
 *   捕获阶段: 消费一个物理边沿; 若该物理键已被 8 键映射或其它补充键占用 → 无效(忽略).
 *   确认阶段: 按 F_CONFIRM(确定) 写入当前功能并进入下一功能;
 *             按 F_BACK(返回) 跳过当前功能(不写入).
 *   4 个完成 → 自动保存并退出. 映射期间禁用手柄导航, 由本模块自行处理确认/跳过. */

/* 启动补充按键映射: 从第 0 个功能(功能1/F)开始 */
static void gamepad_act_sup_keymap(menu_state_t *state) {
    state->list_dialog_active = false;
    state->confirm_active = false;
    state->connecting_popup_active = false;
    state->current_page = MENU_PAGE_MAIN;
    state->selected_index = state->main_selected_index;
    state->scroll_offset = 0;
    state->sup_map_idx = 0;
    state->sup_map_captured = false;
    state->sup_map_pending = PHYS_MAX;
    state->sup_confirm_prev = false;
    state->sup_back_prev = false;
    bt_manager_poll_new_press_reset();
    input_set_gamepad_nav_enabled(false);  /* 禁用手柄导航, 防止干扰映射 */
    ESP_LOGI(TAG, "补充按键映射启动: 请按下 [%s] 对应的按键", bt_manager_sup_func_name(0));
    state->needs_redraw = true;
}

/* 进入下一个功能 / 全部完成则保存退出 */
static void sup_advance(menu_state_t *state) {
    state->sup_map_idx++;
    state->sup_map_captured = false;
    state->sup_map_pending = PHYS_MAX;
    state->sup_confirm_prev = false;
    state->sup_back_prev = false;
    if (state->sup_map_idx >= SUP_MAX) {
        bt_manager_save_sup_map();
        ESP_LOGI(TAG, "补充按键映射完成, 已保存到 NVS");
        state->sup_map_idx = -1;
        input_set_gamepad_nav_enabled(true);  /* 恢复手柄导航 */
        select_game_invalidate_cache();  /* 刷新游戏设置显示(已映射数量) */
    } else {
        bt_manager_poll_new_press_reset();  /* 清空待消费边沿, 等待新按键 */
        ESP_LOGI(TAG, "补充按键映射: 下一步 [%s]", bt_manager_sup_func_name(state->sup_map_idx));
    }
    state->needs_redraw = true;
}

/* 每帧轮询: 捕获物理按键 / 处理确认(确定)/跳过(返回) */
void menu_poll_sup_mapping(menu_state_t *state) {
    if (state->sup_map_idx < 0) return;  /* 未激活 */
    int idx = state->sup_map_idx;
    if (idx < 0 || idx >= SUP_MAX) { state->sup_map_idx = -1; return; }

    if (!state->sup_map_captured) {
        /* 捕获阶段: 消费一个物理边沿 */
        phys_t p;
        if (bt_manager_poll_sup_capture(&p)) {
            if (bt_manager_sup_phys_used(p, idx)) {
                /* 已占用(8键映射或其它补充键) → 无效, 忽略 */
                ESP_LOGI(TAG, "补充按键映射: 按键 %s 已占用, 忽略", bt_manager_phys_name(p));
                snprintf(state->hint_text, sizeof(state->hint_text),
                         "\xe6\x8c\x89\xe9\x94\xae\xe5\xb7\xb2\xe5\x8d\xa0\xe7\x94\xa8");  /* 按键已占用 */
                state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 1000;
            } else {
                state->sup_map_pending = p;
                state->sup_map_captured = true;
                ESP_LOGI(TAG, "补充按键映射: [%s] 捕获 %s",
                         bt_manager_sup_func_name(idx), bt_manager_phys_name(p));
            }
            state->needs_redraw = true;
        }
        return;  /* 等待确认/跳过 */
    }

    /* 确认阶段: 边沿检测 F_CONFIRM(确定). 
     * 用户需求: 取消键 F_BACK 不再跳过当前按键 (按下不生效, 只等待确定). */
    bool cf = bt_manager_is_key_pressed(F_CONFIRM);
    if (cf && !state->sup_confirm_prev) {
        /* 确定: 写入当前功能 → 下一功能 */
        bt_manager_set_sup_map(idx, state->sup_map_pending);
        ESP_LOGI(TAG, "补充按键映射: [%s] 确认 -> %s",
                 bt_manager_sup_func_name(idx), bt_manager_phys_name(state->sup_map_pending));
        sup_advance(state);
    }
    state->sup_confirm_prev = cf;
}

/* === GB 辅助按键映射 (2 键: SELECT/START → 任意手柄物理键) ===
 * 与 BBK 补充按键映射类似, 但独立存储 (s_gb_map, NVS "gbmap"), 只在 GB 游戏
 * 二级菜单及游戏中生效. 流程:
 *   - 进入 GB 游戏菜单且未映射 → gb_aux_prompt=true, 弹"映射辅助键"提示,
 *     按 F_CONFIRM 开始映射, 按 F_BACK 取消提示.
 *   - 映射阶段: 捕获物理边沿 (已在 8 键核心映射或其它 GB 辅助键 → 提示"按键已占用");
 *     确定(F_CONFIRM)写入并下一步, 返回(F_BACK)跳过.
 *   - 2 个完成 → 自动保存退出. 映射期间禁用手柄导航. */

/* 启动 GB 辅助映射: 从 start_idx (0=SELECT, 1=START) 开始 */
static void gamepad_act_gb_auxmap_from(menu_state_t *state, int start_idx) {
    if (start_idx < 0) start_idx = 0;
    if (start_idx >= GB_MAP_MAX) start_idx = GB_MAP_MAX - 1;
    state->gb_aux_prompt = false;
    state->gb_map_idx = start_idx;
    state->gb_map_captured = false;
    state->gb_map_pending = PHYS_MAX;
    state->gb_confirm_prev = false;
    state->gb_back_prev = false;
    bt_manager_poll_new_press_reset();
    input_set_gamepad_nav_enabled(false);  /* 禁用手柄导航, 防止干扰映射 */
    ESP_LOGI(TAG, "GB 辅助按键映射启动: 请按下 [%s] 对应的按键",
             bt_manager_gb_func_name(state->gb_map_idx));
    state->needs_redraw = true;
}

static void gamepad_act_gb_auxmap(menu_state_t *state) {
    gamepad_act_gb_auxmap_from(state, 0);
}

static void gb_aux_advance(menu_state_t *state) {
    state->gb_map_idx++;
    state->gb_map_captured = false;
    state->gb_map_pending = PHYS_MAX;
    state->gb_confirm_prev = false;
    state->gb_back_prev = false;
    if (state->gb_map_idx >= GB_MAP_MAX) {
        bt_manager_save_gb_map();
        ESP_LOGI(TAG, "GB 辅助按键映射完成, 已保存到 NVS");
        state->gb_map_idx = -1;
        input_set_gamepad_nav_enabled(true);  /* 恢复手柄导航 */
        select_game_invalidate_cache();  /* 刷新游戏设置显示(已映射 SELECT/START) */
    } else {
        bt_manager_poll_new_press_reset();  /* 清空待消费边沿, 等待新按键 */
        ESP_LOGI(TAG, "GB 辅助按键映射: 下一步 [%s]",
                 bt_manager_gb_func_name(state->gb_map_idx));
    }
    state->needs_redraw = true;
}

/* 每帧轮询: 处理"映射辅助键"提示确认/取消 + 捕获/确定/跳过 */
void menu_poll_gb_auxmap(menu_state_t *state) {
    /* 提示阶段: 显示"映射辅助键", 按 F_CONFIRM 开始映射, F_BACK 取消 */
    if (state->gb_aux_prompt) {
        bool cf = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_CONFIRM) : false;
        if (cf && !state->gb_confirm_prev) {
            gamepad_act_gb_auxmap(state);
            return;
        }
        state->gb_confirm_prev = cf;
        bool bk = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_BACK) : false;
        if (bk && !state->gb_back_prev) {
            state->gb_aux_prompt = false;  /* 取消映射提示 */
            state->needs_redraw = true;
            return;
        }
        state->gb_back_prev = bk;
        return;
    }

    if (state->gb_map_idx < 0) return;  /* 未激活 */
    int idx = state->gb_map_idx;
    if (idx < 0 || idx >= GB_MAP_MAX) { state->gb_map_idx = -1; return; }

    if (!state->gb_map_captured) {
        /* 捕获阶段: 消费一个物理边沿 */
        phys_t p;
        if (bt_manager_poll_gb_capture(&p)) {
            if (bt_manager_gb_phys_used(p, idx)) {
                /* 已占用(8键映射或其它 GB 辅助键) → 无效, 忽略 */
                ESP_LOGI(TAG, "GB 辅助映射: 按键 %s 已占用, 忽略", bt_manager_phys_name(p));
                snprintf(state->hint_text, sizeof(state->hint_text),
                         "\xe6\x8c\x89\xe9\x94\xae\xe5\xb7\xb2\xe5\x8d\xa0\xe7\x94\xa8");  /* 按键已占用 */
                state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 1000;
            } else {
                state->gb_map_pending = p;
                state->gb_map_captured = true;
                ESP_LOGI(TAG, "GB 辅助映射: [%s] 捕获 %s",
                         bt_manager_gb_func_name(idx), bt_manager_phys_name(p));
            }
            state->needs_redraw = true;
        }
        return;  /* 等待确认/跳过 */
    }

    /* 确认阶段: 边沿检测 F_CONFIRM(确定) / F_BACK(返回) */
    bool cf = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_CONFIRM) : false;
    bool bk = bt_manager_is_connected() ? bt_manager_is_key_pressed(F_BACK) : false;
    if (cf && !state->gb_confirm_prev) {
        bt_manager_set_gb_map(idx, state->gb_map_pending);
        ESP_LOGI(TAG, "GB 辅助映射: [%s] 确认 -> %s",
                 bt_manager_gb_func_name(idx), bt_manager_phys_name(state->gb_map_pending));
        gb_aux_advance(state);
    } else if (bk && !state->gb_back_prev) {
        ESP_LOGI(TAG, "GB 辅助映射: [%s] 跳过", bt_manager_gb_func_name(idx));
        gb_aux_advance(state);
    }
    state->gb_confirm_prev = cf;
    state->gb_back_prev = bk;
}

/* === 5. 动作: 连接记录 (嵌套 list_dialog) === */
static void gamepad_act_history(menu_state_t *state) {
    int hn = bt_manager_get_history_count();
    if (hn <= 0) {
        snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95");
        snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe6\x9a\x82\xe6\x97\xa0\xe8\xae\xb0\xe5\xbd\x95");
        state->confirm_active = true;
        state->confirm_notice = true;
        state->needs_redraw = true;
        return;
    }
    int cnt = 0;
    for (int i = 0; i < hn && cnt < 16; i++) {
        const bt_device_t *d = bt_manager_get_history_at(i);
        if (!d || !d->name[0]) snprintf(state->list_dialog_items[cnt++], 64, "(\xe6\x9c\xaa\xe7\x9f\xa5\xe8\xae\xbe\xe5\xa4\x87)");
        else                     snprintf(state->list_dialog_items[cnt++], 64, "%s", d->name);
    }
    snprintf(state->list_dialog_items[cnt++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");
    list_dialog_open(state, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95", cnt, gamepad_history_on_select);
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
}

/* === 7. 连接记录弹窗: 按确定直接删除 (无需二次确认) === */
static void gamepad_history_on_select(menu_state_t *state, int idx) {
    int hn = bt_manager_get_history_count();
    if (idx < 0 || idx >= hn) return;  /* "返回" 项: list_dialog 通用关闭 */
    const bt_device_t *del = bt_manager_get_history_at(idx);
    bool was_paired = (del && bt_manager_is_paired_device(del->bd_addr));
    if (bt_manager_remove_history_at(idx) && was_paired) {
        bt_manager_clear_paired();
    }
    /* 重建弹窗 (idx 不变 → 指向原下一条) */
    int new_hn = bt_manager_get_history_count();
    if (new_hn <= 0) {
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->current_page = state->list_dialog_return_page;
        state->selected_index = state->main_selected_index;
        state->scroll_offset = 0;
        snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95");
        snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe6\x9a\x82\xe6\x97\xa0\xe8\xae\xb0\xe5\xbd\x95");
        state->confirm_active = true;
        state->confirm_notice = true;
        state->needs_redraw = true;
        return;
    }
    int new_idx = idx;
    if (new_idx >= new_hn) new_idx = new_hn - 1;
    int cnt = 0;
    for (int i = 0; i < new_hn && cnt < 16; i++) {
        const bt_device_t *d = bt_manager_get_history_at(i);
        if (!d || !d->name[0]) snprintf(state->list_dialog_items[cnt++], 64, "(\xe6\x9c\xaa\xe7\x9f\xa5\xe8\xae\xbe\xe5\xa4\x87)");
        else                     snprintf(state->list_dialog_items[cnt++], 64, "%s", d->name);
    }
    snprintf(state->list_dialog_items[cnt++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");
    state->list_dialog_count = cnt;
    state->list_dialog_selected = new_idx;
    state->list_dialog_scroll = 0;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
}

/* menu_poll_gamepad_mapping 实现已移至 V1.0.41 按键映射小弹窗模块 (上方) */

/* === 连接记录 (全屏列表) ===
 * 用户需求: 手柄弹窗 -> 连接记录, 进入后展示已连接过的设备, 选中后弹出
 * "是否删除" 确认, 返回键退出. 渲染时全屏重绘 (覆盖主菜单/子页背景). */
/* 已删除: 全屏连接记录页 draw_bt_history_page (死代码, bt_history_active 从未被置 true).
 * 连接记录统一走 list_dialog 弹窗, 见 gamepad_history_on_select (上面). */

/* === 旧 gamepad 全屏实现已彻底删除 (V1.0.18 重构) ===
 * 删除了以下函数/数组:
 *   - gamepad_build / gamepad_on_confirm (旧 sub_pages 全屏入口)
 *   - gamepad_dialog_on_select (旧 list_dialog 入口, 与新 gamepad_list_on_select 重复)
 *   - gamepad_action_add_device / gamepad_action_key_mapping /
 *     gamepad_action_history / gamepad_action_back (旧动作函数, 与新 gamepad_act_* 重复)
 *   - bt_history_dialog_on_select (旧连接记录弹窗回调, 与新 gamepad_history_on_select 重复)
 *   - open_gamepad_dialog (旧弹窗打开函数, 已由新 gamepad_list_open 取代)
 *   - start_key_mapping (旧启动函数, 已并入 gamepad_mapping_start)
 *   - draw_key_mapping (旧全屏渲染函数, 已并入 menu_render 通用弹窗模板)
 *   - sub_pages[MENU_PAGE_GAMEPAD] (旧全屏子页入口)
 *
 * 现在手柄子菜单只有一套实现 (gamepad_list_*), 全部走 list_dialog 弹窗, 不再有
 * 全屏子页. 主菜单点"手柄"直接调用 gamepad_list_open. 蓝牙连接后自动跳映射,
 * 映射完成后 gamepad_mapping_finish 会重新打开弹窗, 不会再出现"全屏手柄页". */

/* 通用: 打开列表弹窗前的统一初始化 (重置 prev_active, 强制下次全量绘制外框/标题)
 * 解决"退出重新进时弹窗外框/文字不显示"的局部刷新 bug:
 *   弹窗关闭后, list_dialog_prev_active 仍为 true, 再次打开时
 *   draw_list_dialog 误判为可走"局部刷新"路径, 只重绘两行, 外框/标题被跳过. */
/* 嵌套弹窗: 打开子弹窗前, 把当前(父)弹窗状态压栈, 关闭子弹窗后恢复(含原选中位置) */
static void list_dialog_push_parent(menu_state_t *state) {
    if (state->list_dialog_stack_top >= LIST_DIALOG_STACK_DEPTH) return;  /* 栈满, 极少见 */
    list_dialog_saved_t *s = &state->list_dialog_stack[state->list_dialog_stack_top];
    memcpy(s->items, state->list_dialog_items, sizeof(state->list_dialog_items));
    s->count       = state->list_dialog_count;
    s->selected    = state->list_dialog_selected;
    s->scroll      = state->list_dialog_scroll;
    s->return_page = state->list_dialog_return_page;
    s->on_select   = state->list_dialog_on_select;
    s->on_key      = state->list_dialog_on_key;
    s->on_close    = state->list_dialog_on_close;
    s->on_render   = state->list_dialog_on_render;
    s->prev_selected = state->list_dialog_prev_selected;
    s->prev_active   = state->list_dialog_prev_active;
    s->content_dirty = state->list_dialog_content_dirty;
    state->list_dialog_stack_top++;
}

/* 嵌套弹窗: 关闭子弹窗时弹出父弹窗状态并恢复. 返回 true=已恢复父弹窗 */
static bool list_dialog_pop_parent(menu_state_t *state) {
    if (state->list_dialog_stack_top <= 0) return false;
    state->list_dialog_stack_top--;
    list_dialog_saved_t *s = &state->list_dialog_stack[state->list_dialog_stack_top];
    memcpy(state->list_dialog_items, s->items, sizeof(state->list_dialog_items));
    state->list_dialog_count       = s->count;
    state->list_dialog_selected    = s->selected;
    state->list_dialog_scroll      = s->scroll;
    state->list_dialog_return_page = s->return_page;
    state->list_dialog_on_select   = s->on_select;
    state->list_dialog_on_key      = s->on_key;
    state->list_dialog_on_close    = s->on_close;
    state->list_dialog_on_render   = s->on_render;
    /* 强制全量重绘父弹窗, 恢复正确外框/选中位置 (避免局部刷新残留) */
    state->list_dialog_active = true;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    state->list_dialog_content_dirty = s->content_dirty;
    state->needs_redraw = true;
    return true;
}

static void list_dialog_open(menu_state_t *state, const char *title,
                             int count, void (*on_select)(menu_state_t *, int)) {
    /* 嵌套: 若已有弹窗激活, 先压入父弹窗状态, 关闭子弹窗后恢复(含原位置) */
    if (state->list_dialog_active) {
        list_dialog_push_parent(state);
    }
    state->list_dialog_active = true;
    state->list_dialog_selected = 0;
    state->list_dialog_scroll = 0;
    state->list_dialog_on_select = on_select;
    snprintf(state->list_dialog_title, sizeof(state->list_dialog_title), "%s", title);
    state->list_dialog_count = count;
    /* 关键: 强制全量重绘, 防止局部刷新状态机残留导致外框/文字丢失 */
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_local_update = false;
    /* 自定义渲染/内容脏: 默认为空, 由调用方按需设置 */
    state->list_dialog_on_render = NULL;
    state->list_dialog_content_dirty = false;
    /* 关键: 重置方向键与关闭回调, 防止上一个弹窗 (如时间弹窗) 的回调残留
     * 导致新弹窗 (如设置弹窗) 的 UP/DOWN 仍被旧回调拦截, 出现"设置不能上下移动"bug. */
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
    state->needs_redraw = true;
}

/* TF 卡子页 */
static int sd_build(menu_state_t *state, char buf[][64], int max);
static bool sd_on_confirm(menu_state_t *state, int idx);
static void sd_dialog_on_select(menu_state_t *state, int idx);
static void open_sd_dialog(menu_state_t *state);
static void open_sd_info_dialog(menu_state_t *state);
/* 打开 SD 卡管理弹窗, 返回页面 = 当前所在页 (main 菜单或 settings).
 * 之前硬编码为 MENU_PAGE_SETTINGS, 在存储管理提升为一级菜单后, 从主菜单点进来
 * 应该返回主菜单, 从设置点进来仍返回设置. */
static void open_sd_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;  /* 跟随当前页, 弹窗关闭后回到这里 */
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再 build 写入子弹窗 items */
    list_dialog_open(state, "存储管理", 0, sd_dialog_on_select);
    int cnt = sd_build(state, state->list_dialog_items, 16);
    state->list_dialog_count = cnt;
}

/* === 公开 wrapper: 供其它模块 (file_browser.c 等) 间接调用 list_dialog_open / open_sd_dialog === */
void menu_open_list_dialog(menu_state_t *state, const char *title,
                           int count, void (*on_select)(menu_state_t *, int)) {
    /* 复用 static list_dialog_open: 它内部已重置 prev 状态, 强制全量重绘外框/标题 */
    list_dialog_open(state, title, count, on_select);
}

void menu_open_sd_dialog(menu_state_t *state) {
    /* 供 file_browser.c 等通过 on_close 调用, 实现"按返回键回到上一步" */
    open_sd_dialog(state);
}

/* === 存储信息弹窗 (用户需求: 用 list_dialog 弹窗显示, 不用 confirm 小弹窗) ===
 * 弹窗里几行只读信息 + "返回", 关闭时回到 sd_dialog. */
static void sd_info_dialog_on_select(menu_state_t *state, int idx) {
    /* 只读, 选"返回"由 list_dialog 通用分支处理, 不会走到这里 */
    (void)state; (void)idx;
}
static void open_sd_info_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再写入子弹窗 items */
    list_dialog_open(state, "存储信息", 0, sd_info_dialog_on_select);

    uint64_t total = 0, free = 0;
    extern int sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes);
    extern bool sd_is_mounted(void);
    bool was_mounted = sd_is_mounted();
    int rc = sd_get_info(&total, &free);

    /* 用 list_dialog_items 填多行信息, 最后一项为"返回" */
    int n = 0;
    if (rc == 0) {
        uint32_t total_mb = (uint32_t)(total / (1024ULL * 1024ULL));
        uint32_t free_mb  = (uint32_t)(free  / (1024ULL * 1024ULL));
        uint32_t used_mb  = total_mb > free_mb ? (total_mb - free_mb) : 0;
        snprintf(state->list_dialog_items[n++], 64, "总容量: %luMB", (unsigned long)total_mb);
        snprintf(state->list_dialog_items[n++], 64, "已用:  %luMB", (unsigned long)used_mb);
        snprintf(state->list_dialog_items[n++], 64, "剩余:  %luMB", (unsigned long)free_mb);
    } else if (rc == -1) {
        snprintf(state->list_dialog_items[n++], 64, "状态: 未挂载");
        snprintf(state->list_dialog_items[n++], 64, was_mounted ? "原因: 重读失败" : "原因: 请格式化");
    } else {
        snprintf(state->list_dialog_items[n++], 64, "状态: 读卡错误");
    }
    snprintf(state->list_dialog_items[n++], 64, "返回");
    (void)was_mounted;
    state->list_dialog_count = n;
    /* V1.0.41: 不再设 on_close, 关闭时由 list_dialog_pop_parent 恢复父弹窗位置 */
}
static void sd_dialog_on_select(menu_state_t *state, int idx) {
    /* V1.0.41: 不关闭 SD 管理弹窗, 直接调子打开函数.
     * list_dialog_open 会自动压栈 SD 管理弹窗, 子弹窗关闭后通过 list_dialog_pop_parent
     * 恢复 SD 管理弹窗(含原选中位置), 实现"弹窗返回上级保持原位置".
     * idx == count-1 ("返回") 由 list_dialog 通用 CONFIRM 分支处理, 不会走到这里. */
    sd_on_confirm(state, idx);
}
static int sd_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    snprintf(buf[n++], 64, "浏览文件");
    snprintf(buf[n++], 64, "挂载到电脑");
    snprintf(buf[n++], 64, "格式化TF卡");
    snprintf(buf[n++], 64, "存储信息");
    snprintf(buf[n++], 64, "返回");
    (void)state; (void)max;
    return n;
}
/* === 挂载电脑 list_dialog 弹窗 (用户需求: 用 list_dialog 弹窗样式, 不用 confirm 小弹窗)
 * 弹窗项: [确定挂载] [返回]
 *   - 选中确定 -> 执行挂载 (卸载 VFS + 启动 USB MSC)
 *   - 选中返回/按 BACK -> 关闭弹窗, 通过 list_dialog_pop_parent 恢复 SD 管理弹窗
 *   - 结果 (成功/失败) 用 confirm 弹窗显示, 关闭后回到挂载弹窗 */
static void sd_mount_dialog_on_select(menu_state_t *state, int idx);
static void open_sd_mount_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;  /* 跟随 SD 管理所在页 */
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再写入子弹窗 items */
    list_dialog_open(state, "\xe6\x8c\x82\xe8\xbd\xbd\xe5\x88\xb0\xe7\x94\xb5\xe8\x84\x91", 0, sd_mount_dialog_on_select);
    int n = 0;
    snprintf(state->list_dialog_items[n++], 64, "\xe7\xa1\xae\xe5\xae\x9a\xe6\x8c\x82\xe8\xbd\xbd");  /* 确定挂载 */
    snprintf(state->list_dialog_items[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 返回 */
    state->list_dialog_count = n;
    /* V1.0.41: 不再设 on_close, 关闭时由 list_dialog_pop_parent 恢复父弹窗位置 */
}

/* 弹窗选中: 0=确定挂载, 1=返回 */
static void sd_mount_dialog_on_select(menu_state_t *state, int idx) {
    if (idx == 0) {
        /* 确定挂载: 真正执行 */
        extern bool usbh_msc_is_running(void);
        if (usbh_msc_is_running()) {
            /* 已经在挂载, 弹重启提示 (不重开 SD 管理, 即将重启) */
            state->list_dialog_active = false;
            state->list_dialog_prev_active = false;
            state->list_dialog_prev_selected = -1;
            state->list_dialog_on_close = NULL;
            state->confirm_active = true;
            state->confirm_notice = true;
            state->confirm_no_hint = true;
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe9\x80\x80\xe5\x87\xba");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe8\xae\xbe\xe5\xa4\x87\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf...");
            state->needs_redraw = true;
            menu_render(state);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
            return;
        }
        /* 真正执行挂载 */
        extern void sd_watcher_set_paused(bool);
        /* 进入 MSC 前必须暂停 SD watcher: VFS 卸载后 s_mounted=false,
         * watcher 若发现"未挂载"会 deinit SDMMC host 并重挂, 与 PC
         * 正在读写的扇区并发 → 设备挂死 (PC 无法访问磁盘). */
        sd_watcher_set_paused(true);
        extern int sd_unmount_vfs_keep_card(void);
        extern int usbh_msc_start(void);
        int rc1 = sd_unmount_vfs_keep_card();
        if (rc1 != 0) {
            sd_watcher_set_paused(false);  /* 卸载失败: 恢复 watcher */
            /* 失败: 关闭弹窗, 显示失败 confirm, 关闭后回到 SD 管理 */
            state->confirm_active = true;
            state->confirm_notice = true;
            state->confirm_no_hint = true;
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe5\x8d\xb8\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5");
            state->needs_redraw = true;
            return;
        }
        int rc2 = usbh_msc_start();
        if (rc2 != 0) {
            extern int sd_remount_vfs_from_card(void);
            sd_remount_vfs_from_card();
            sd_watcher_set_paused(false);  /* MSC 启动失败: 恢复 watcher */
            state->confirm_active = true;
            state->confirm_notice = true;
            state->confirm_no_hint = true;
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "USB\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5");
            state->needs_redraw = true;
            return;
        }
        /* 成功: 保留 list_dialog_active=true, confirm 覆盖显示"已连接".
         * 关闭 confirm 后会自然回到 SD 管理 list_dialog (弹窗优先级 confirm>list_dialog).
         * 用户需求: 按返回键停留到上一步, 这里结果弹窗任意键关闭后也会回 SD 管理. */
        state->confirm_active = true;
        state->confirm_notice = true;
        state->confirm_no_hint = true;
        snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5");
        snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe7\x94\xb5\xe8\x84\x91\xe5\x8f\xaf\xe8\xaf\xbb\xe5\x86\x99");
        state->needs_redraw = true;
        return;
    }
    /* idx == 1: "返回" - 关闭弹窗, on_close 会重开 SD 管理 */
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->current_page = state->list_dialog_return_page;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    if (state->list_dialog_on_close) {
        void (*cb)(menu_state_t *) = state->list_dialog_on_close;
        state->list_dialog_on_close = NULL;
        cb(state);
    }
}

/* === 格式化TF卡 list_dialog 弹窗 (用户需求: 用 list_dialog 弹窗样式) */
static void sd_format_dialog_on_select(menu_state_t *state, int idx);
static void open_sd_format_dialog(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再写入子弹窗 items */
    list_dialog_open(state, "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96TF\xe5\x8d\xa1", 0, sd_format_dialog_on_select);
    int n = 0;
    snprintf(state->list_dialog_items[n++], 64, "\xe7\xa1\xae\xe5\xae\x9a\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96");  /* 确定格式化 */
    snprintf(state->list_dialog_items[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 返回 */
    state->list_dialog_count = n;
    /* V1.0.41: 不再设 on_close, 关闭时由 list_dialog_pop_parent 恢复父弹窗位置 */
}

/* 弹窗选中: 0=确定格式化, 1=返回 */
static void sd_format_dialog_on_select(menu_state_t *state, int idx) {
    if (idx == 0) {
        /* 确定格式化: 真正执行 (格式化中需全屏显示进度, 故先关闭 list_dialog;
         * 完成后通过重开 SD 管理 + confirm 显示结果的方式, 让结果关闭后回到 SD 管理) */
        ESP_LOGI(TAG, "格式化 TF 卡...");

        /* 关闭弹窗, 全屏显示格式化进度.
         * V1.0.41: 清空嵌套栈 (格式化完成后会重开 SD 管理, 不需要恢复旧父弹窗状态,
         * 否则栈残留会导致返回时多一层). */
        state->list_dialog_stack_top = 0;
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_close = NULL;

        st7305_clear(state->lcd, ST7305_COLOR_WHITE);
        draw_text_centered(state->lcd, 100, "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe4\xb8\xad", false);  /* 格式化中 */
        draw_text_centered(state->lcd, 130, "\xe7\xa8\x8d\xe5\x80\x99", false);  /* 稍候 */
        draw_text_centered(state->lcd, 200, "\xe5\x8b\xbf\xe6\x96\xad\xe7\x94\xb5", false);  /* 勿断电 */
        st7305_flush(state->lcd);

        extern int sd_format_and_create_dirs(void);
        int rc = sd_format_and_create_dirs();

        /* 完成后: 重开 SD 管理 list_dialog, 同时显示结果 confirm.
         * 关闭 confirm 后会自然看到 SD 管理. */
        open_sd_dialog(state);
        state->confirm_active = true;
        state->confirm_notice = true;
        state->confirm_no_hint = true;
        if (rc == 0) {
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xae\x8c\xe6\x88\x90");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe5\xb7\xb2\xe5\xbb\xba\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");
        } else if (rc == -1) {
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "SPI\xe9\x94\x99\xe8\xaf\xaf");
        } else if (rc == -2) {
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe6\x97\xa0TF\xe5\x8d\xa1");
        } else if (rc == -3) {
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe6\x93\xa6\xe9\x99\xa4\xe5\xa4\xb1\xe8\xb4\xa5");
        } else {
            snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe5\xa4\xb1\xe8\xb4\xa5");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "\xe6\x9c\xaa\xe7\x9f\xa5\xe9\x94\x99\xe8\xaf\xaf");
        }
        state->needs_redraw = true;
        return;
    }
    /* idx == 1: "返回" - 关闭弹窗, on_close 会重开 SD 管理 */
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->current_page = state->list_dialog_return_page;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    if (state->list_dialog_on_close) {
        void (*cb)(menu_state_t *) = state->list_dialog_on_close;
        state->list_dialog_on_close = NULL;
        cb(state);
    }
}

static bool sd_on_confirm(menu_state_t *state, int idx) {
    /* confirm_executing=true 表明是"弹窗已 KEY 确认, 现在真正执行" */
    switch (idx) {
        case 0: { /* 浏览文件 - 打开文件浏览器弹窗 (用户需求: 弹窗样式) */
            extern void fb_open_dialog(menu_state_t *state);
            fb_open_dialog(state);
            return true;
        }
        case 1: { /* 挂载电脑 - 改为 list_dialog 弹窗 (用户需求: 弹窗样式)
                   * 第一次进入: 打开 list_dialog 确认弹窗
                   * 用户在 list_dialog 中选"确定" -> sd_mount_dialog_on_select 执行 */
            open_sd_mount_dialog(state);
            return true;
        }
        case 2: { /* 格式化TF卡 - 改为 list_dialog 弹窗 (用户需求: 弹窗样式) */
            open_sd_format_dialog(state);
            return true;
        }
        case 3: {
            /* 存储信息: 改用 list_dialog 弹窗显示 (用户需求: 信息类用原来的弹窗样式,
             * 不要用 confirm 小弹窗, 避免覆盖痕迹影响 list_dialog 重绘) */
            extern void open_sd_info_dialog(menu_state_t *state);
            open_sd_info_dialog(state);
            return true;
        }
        case 5:
        default:
            state->current_page = MENU_PAGE_SETTINGS;
            state->selected_index = 0;
            state->scroll_offset = 0;
            state->needs_redraw = true;
            return true;
    }
}

/* 音量档位 (settings.volume, 0-10 共 11 档) -> 实际 ES8311 百分比 (0-100)
 * V1.0.67: 统一 0-10 档, 0 静音, 10 = 100%, 每档 10% 线性.
 * V1.0.68 fix: 当前硬件 (解码输出/数码调音) 低区偏闷, 旧 1-5 档几乎无声.
 *   新 1 档 = 旧 5 档 (50%), 新 2-10 档在 50%-100% 间平均分布 (每档约 5.6%).
 *   仅 scheme==0 (解码输出) 生效; 切换不同解码/其他方案保持原线性. */
static int volume_step_to_percent(int vol) {
    if (vol <= 0) return 0;
    if (vol >= 10) return 100;
    if (g_menu.settings.audio_scheme == 0) {
        return 50 + (vol - 1) * 50 / 9;
    }
    return vol * 10;
}

/* ============ 配置持久化: TF 卡 -> 系统(NVS) -> 默认 (三级回退) ============
 * 用户需求:
 *  - 当前所有配置默认存一份到 TF 卡 (/sdcard/system/config.cfg)
 *  - 进入桌面时先读 TF 配置; TF 没有则读系统(NVS)配置; 系统也没有则用默认配置
 * 存储格式: 二进制小结构体 (magic + version + 各 bool/uint8 字段),
 *           同时写 TF 文件 + NVS blob (ns="menu_settings", key="all"),
 *           读取优先级 TF > NVS > 默认. */
#define BBK_CFG_PATH       "/sdcard/system/config.cfg"
#define BBK_CFG_MAGIC      0x42424B43u   /* "BBKC" */
#define BBK_CFG_VERSION    16  /* V1.0.68: 隐藏设置 (v16: +audio_scheme +touch_disable) */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint8_t  volume;          /* 0-9 档 */
    uint8_t  mute;            /* 0/1 */
    uint8_t  low_power;       /* 0/1 */
    uint8_t  bt_enabled;      /* 0/1 */
    uint8_t  wifi_enabled;    /* 0/1 */
    uint8_t  game_show_statusbar; /* 0/1 (状态栏设置) */
    uint8_t  game_gray_mode;  /* 0/1 GB 模拟灰度 (gray[0]) */
    uint8_t  game_pic_opt;    /* 0 (保留, 固定关) */
    uint8_t  game_fullscreen; /* 1 (保留, 固定开) */
    /* V1.0.49: 各 console 引擎独立灰度 (复用原 reserved[3], 保持结构体大小不变以兼容旧配置)
     * gray[1]=GBC, gray[2]=FC, gray[3]=arduboy */
    uint8_t  game_gray[3];
    uint8_t  game_key_sound;  /* 0/1 BBK 按键音效 (V1.0.59) */
    uint8_t  game_display[4]; /* 各引擎显示模式: GB/GBC/FC/arduboy (V1.0.60) */
    /* V1.0.62: 电子书设置 */
    uint8_t  book_knock;      /* 0/1 敲击翻页 */
    uint8_t  book_sens;       /* 0=低, 1=中, 2=高 */
    uint8_t  book_night;      /* 0/1 夜间模式 (反色) */
    uint8_t  book_pagenum;    /* 0/1 显示页码 */
    uint8_t  book_rot;        /* 旋转方向: 0上 1下 2左 3右 (V1.0.62) */
    /* V1.0.63: 电子书布局 */
    uint8_t  book_fontsize;   /* 0=20 1=24 2=28 3=32 */
    uint8_t  book_margin;     /* 0=窄 1=中 2=宽 */
    uint8_t  book_lineh;      /* 0=紧凑 1=标准 2=宽松 */
    uint8_t  book_gap;        /* 0=标准 1=宽松 */
    uint8_t  book_font_family;/* 0=黑体 1=宋体 (V1.0.64) */
    uint8_t  wallpaper_mode;  /* 0=内置星空 1=TF动态图 2=游戏壁纸 (V1.0.64) */
    uint8_t  wallpaper_program; /* 内置壁纸程序 0..10 (V1.0.64) */
    uint8_t  wallpaper_timeout_min; /* 休眠分钟 1..30 (V1.0.64) */
    uint8_t  wallpaper_bmp_fps; /* 0=慢 1=标准 2=快 (V1.0.64) */
    uint8_t  pomo_work_min;     /* 番茄钟工作分钟 (V1.0.64) */
    uint8_t  pomo_rest_min;     /* 番茄钟休息分钟 (V1.0.64) */
    uint8_t  pomo_reminder;     /* 番茄钟完成提醒声音 0/1 (V1.0.65) */
    uint8_t  game_vkey;         /* 游戏内虚拟按键 0/1 (V1.0.68) */
    uint8_t  audio_disable;     /* 禁用音频 0/1 (V1.0.68) */
    uint8_t  audio_scheme;      /* 音频方案 0=解码 1=方波PWM 2=禁用 (V1.0.68) */
    uint8_t  touch_disable;     /* 禁用触摸屏 0/1 (V1.0.68) */
    uint8_t  tone_effect_sel;   /* 试听音效选择 0-5 (V1.0.68) */
} bbk_config_t;

/* 确保 /sdcard/system 目录存在 (参考 favorites.c) */
static void config_ensure_dir(void) {
    struct stat st;
    if (stat("/sdcard/system", &st) == 0) return;
    mkdir("/sdcard/system", 0755);
}

/* V1.0.49+: 各 console 引擎独立灰度模式 (索引=select_engine, 0=GB,1=GBC,2=FC,3=arduboy)
 * V1.0.56: 简化为开关两态 = 0=关(纯黑白), 1=开(灰度), 删除散点抖动档 */
static uint8_t s_engine_gray[4] = { 1, 1, 1, 1 };

static uint8_t engine_gray_get(int engine) {
    if (engine < 0 || engine > 3) return 1;
    return s_engine_gray[engine];
}

static void engine_gray_set(int engine, uint8_t v) {
    if (engine < 0 || engine > 3) return;
    if (v > 1) v = 1;
    s_engine_gray[engine] = v;
}

/* V1.0.60: 各引擎独立显示模式 (0=点对点, 1=全屏, 2=拉伸); BBK 单独一份 */
static uint8_t s_engine_display[4] = { 1, 1, 1, 1 };
static uint8_t s_bbk_display = 1;

static uint8_t engine_display_get(int engine) {
    if (engine < 0 || engine > 3) return 1;
    return s_engine_display[engine];
}

static void engine_display_set(int engine, uint8_t v) {
    if (engine < 0 || engine > 3) return;
    if (v > 2) v = 2;
    s_engine_display[engine] = v;
}

/* 灰度开关显示名: 0=关(纯黑白), 1=开(灰度) */
static const char *engine_gray_name(uint8_t mode) {
    switch (mode) {
    case 0:  return "\xe5\x85\xb3";       /* 关 */
    default: return "\xe5\xbc\x80";       /* 开 */
    }
}

/* 把当前 g_menu.settings 序列化为结构体 */
static void config_pack(bbk_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->magic = BBK_CFG_MAGIC;
    c->version = BBK_CFG_VERSION;
    c->volume = g_menu.settings.volume;
    c->mute = g_menu.settings.mute ? 1 : 0;
    c->low_power = g_menu.settings.low_power ? 1 : 0;
    c->bt_enabled = g_menu.settings.bt_enabled ? 1 : 0;
    c->wifi_enabled = g_menu.settings.wifi_enabled ? 1 : 0;
    c->game_show_statusbar = g_menu.game_show_statusbar ? 1 : 0;
    c->game_gray_mode = s_engine_gray[0];  /* GB */
    c->game_gray[0] = s_engine_gray[1];    /* GBC */
    c->game_gray[1] = s_engine_gray[2];    /* FC */
    c->game_gray[2] = s_engine_gray[3];    /* arduboy */
    c->game_pic_opt = (uint8_t)g_menu.game_pic_opt;
    c->game_fullscreen = s_bbk_display;   /* BBK 显示模式 */
    c->game_key_sound = g_menu.game_key_sound ? 1 : 0;
    c->game_display[0] = s_engine_display[0];   /* GB */
    c->game_display[1] = s_engine_display[1];   /* GBC */
    c->game_display[2] = s_engine_display[2];   /* FC/NES */
    c->game_display[3] = s_engine_display[3];   /* arduboy */
    c->book_knock = g_menu.book_knock ? 1 : 0;
    c->book_sens = g_menu.book_sens;
    c->book_night = g_menu.book_night ? 1 : 0;
    c->book_pagenum = g_menu.book_pagenum ? 1 : 0;
    c->book_rot = g_menu.book_rot;
    c->book_fontsize = g_menu.book_fontsize;
    c->book_margin = g_menu.book_margin;
    c->book_lineh = g_menu.book_lineh;
    c->book_gap = g_menu.book_gap;
    c->book_font_family = g_menu.book_font_family;
    c->wallpaper_mode = g_menu.wallpaper_mode;
    c->wallpaper_program = g_menu.wallpaper_program;
    c->wallpaper_timeout_min = g_menu.wallpaper_timeout_min;
    c->wallpaper_bmp_fps = g_menu.wallpaper_bmp_fps;
    c->pomo_work_min = g_menu.pomo_work_min;
    c->pomo_rest_min = g_menu.pomo_rest_min;
    c->pomo_reminder = g_menu.pomo_reminder ? 1 : 0;
    c->game_vkey = g_menu.game_virtual_keys ? 1 : 0;
    c->audio_disable = g_menu.settings.audio_disable ? 1 : 0;
    c->audio_scheme = g_menu.settings.audio_scheme;
    c->touch_disable = g_menu.settings.touch_disable ? 1 : 0;
    c->tone_effect_sel = g_menu.settings.tone_effect_sel;
}

/* 把结构体反序列化应用到 g_menu.settings (只改持久化字段) */
static void config_unpack(const bbk_config_t *c) {
    g_menu.settings.volume = (c->volume > 10) ? 10 : c->volume;
    g_menu.settings.mute = c->mute ? true : false;
    g_menu.settings.low_power = c->low_power ? true : false;
    g_menu.settings.bt_enabled = c->bt_enabled ? true : false;
    g_menu.settings.wifi_enabled = c->wifi_enabled ? true : false;
    g_menu.game_show_statusbar = c->game_show_statusbar ? true : false;
    g_menu.settings.game_status_bar = c->game_show_statusbar ? true : false;  /* 渲染也读此字段, 保持同步 */
    /* 各引擎独立灰度开关 (0/1; 旧配置的 2=抖动 归一到 1=开) */
    s_engine_gray[0] = (c->game_gray_mode > 1) ? 1 : c->game_gray_mode;
    s_engine_gray[1] = (c->game_gray[0]   > 1) ? 1 : c->game_gray[0];
    s_engine_gray[2] = (c->game_gray[1]   > 1) ? 1 : c->game_gray[1];
    s_engine_gray[3] = (c->game_gray[2]   > 1) ? 1 : c->game_gray[2];
    g_menu.game_gray_mode = s_engine_gray[0];  /* 当前激活引擎尚未确定, 先取 GB */
    g_menu.game_pic_opt = (c->game_pic_opt > 1) ? 1 : c->game_pic_opt;  /* 抗锯齿两态: 0=关, 1=EPX */
    /* V1.0.60: 各引擎独立显示模式; 旧配置 (version<5) 全部沿用原共享值 */
    s_bbk_display = (c->game_fullscreen > 2) ? 2 : c->game_fullscreen;
    for (int i = 0; i < 4; i++) {
        uint8_t v = (c->version >= 5) ? c->game_display[i] : s_bbk_display;
        s_engine_display[i] = (v > 2) ? 2 : v;
    }
    g_menu.game_display_mode = s_engine_display[0];  /* 当前引擎进入页面时再同步 */
    /* V1.0.59: 旧配置 (version 3) 无按键音效字段, 默认开 */
    g_menu.game_key_sound = (c->version >= 4) ? (c->game_key_sound ? true : false) : true;
    /* V1.0.62: 电子书设置 (version < 6 用默认: 敲击开/灵敏度中/夜间关/页码开) */
    /* V1.0.62: 电子书设置. 敲击翻页暂时停用: 忽略持久化值默认关闭 */
    g_menu.book_knock = false;
    g_menu.book_sens = (c->version >= 6 && c->book_sens <= 2) ? c->book_sens : 1;
    g_menu.book_night = (c->version >= 6) ? (c->book_night ? true : false) : false;
    /* 页码默认不显示 (用户要求); 忽略持久化值, 需要时可在书设置里临时打开 */
    g_menu.book_pagenum = false;
    g_menu.book_rot = (c->version >= 7 && c->book_rot <= 3) ? c->book_rot : 0;   /* 默认上 */
    /* V1.0.63: 电子书布局 (默认: 中字号/中边距/标准行高/标准字距) */
    g_menu.book_fontsize = (c->version >= 8 && c->book_fontsize <= 3) ? c->book_fontsize : 1;
    g_menu.book_margin   = (c->version >= 8 && c->book_margin <= 2)   ? c->book_margin   : 1;
    g_menu.book_lineh    = (c->version >= 8 && c->book_lineh <= 2)    ? c->book_lineh    : 1;
    g_menu.book_gap      = (c->version >= 8 && c->book_gap <= 1)      ? c->book_gap      : 0;
    g_menu.book_font_family = (c->version >= 9 && c->book_font_family <= 20) ? c->book_font_family : 0;
    g_menu.wallpaper_mode = (c->version >= 10 && c->wallpaper_mode <= 2) ? c->wallpaper_mode : 0;
    g_menu.wallpaper_program = (c->version >= 12 &&
                                (c->wallpaper_program == WP_PROG_STARS ||
                                 c->wallpaper_program == WP_PROG_WEATHER))
                               ? c->wallpaper_program : 0;   /* V1.0.67: 只保留星空 + 天气时钟 */
    g_menu.wallpaper_timeout_min = (c->version >= 10 && c->wallpaper_timeout_min >= 1
                                    && c->wallpaper_timeout_min <= 30) ? c->wallpaper_timeout_min : 3;
    g_menu.wallpaper_bmp_fps = (c->version >= 11 && c->wallpaper_bmp_fps <= 2) ? c->wallpaper_bmp_fps : 1;
    g_menu.pomo_work_min = (c->version >= 12 && c->pomo_work_min >= 1 && c->pomo_work_min <= 120)
                           ? c->pomo_work_min : 25;
    g_menu.pomo_rest_min = (c->version >= 12 && c->pomo_rest_min >= 1 && c->pomo_rest_min <= 60)
                           ? c->pomo_rest_min : 5;
    g_menu.pomo_reminder = (c->version >= 13) ? (c->pomo_reminder != 0) : true;
    g_menu.game_virtual_keys = (c->version >= 14) ? (c->game_vkey != 0) : false;
    g_menu.settings.audio_disable = (c->version >= 15) ? (c->audio_disable != 0) : false;
    /* V1.0.68: 音频方案 (0=解码输出 1=方波直驱 2=禁用); 禁用音频由此方案派生,
     * 忽略旧 v15 的独立 audio_disable 值 (已并入方案). */
    g_menu.settings.audio_scheme = (c->version >= 16 && c->audio_scheme <= 2) ? c->audio_scheme : 0;
    g_menu.settings.touch_disable = (c->version >= 16) ? (c->touch_disable != 0) : false;
    g_menu.settings.tone_effect_sel = (c->version >= 16 && c->tone_effect_sel <= 5) ? c->tone_effect_sel : 0;
    g_menu.settings.audio_disable = (g_menu.settings.audio_scheme == 2);
    g_menu.wallpaper_game_rot = 0;
}

/* 校验结构体合法性 (magic/version 匹配, 字段范围合理) */
static bool config_valid(const bbk_config_t *c) {
    if (c->magic != BBK_CFG_MAGIC) return false;
    if (c->version < 3 || c->version > BBK_CFG_VERSION) return false;
    /* V1.0.62 修复: 音量默认档位=10 (100%), 之前 >9 误判导致整份配置被拒、
     * 每次开机都回默认值并覆盖已保存配置 (旋转方向等全部设置断电丢失的根因) */
    if (c->volume > 10) return false;
    return true;
}

/* 保存: 写 TF 文件 (若已挂载) + 写 NVS blob.
 * 用户需求: 默认把当前配置存一份到 TF 卡. */
static void menu_config_save(void) {
    bbk_config_t c;
    config_pack(&c);
    /* 1) TF 卡: 已挂载才尝试 (无卡则跳过, 不刷错误日志) */
    if (sd_is_mounted()) {
        config_ensure_dir();
        FILE *f = fopen(BBK_CFG_PATH, "wb");
        if (f) {
            size_t w = fwrite(&c, 1, sizeof(c), f);
            fclose(f);
            if (w == sizeof(c)) {
                ESP_LOGI(TAG, "配置已保存到 TF: %s", BBK_CFG_PATH);
            } else {
                ESP_LOGW(TAG, "写入 TF 配置不完整: %u/%u", (unsigned)w, (unsigned)sizeof(c));
            }
        } else {
            ESP_LOGW(TAG, "打开 TF 配置失败 (errno=%d)", errno);
        }
    }
    /* 2) NVS (系统配置): 永远写, 作为 TF 缺失时的回退 */
    nvs_handle_t h;
    esp_err_t err = nvs_open("menu_settings", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "all", &c, sizeof(c));
        if (err == ESP_OK) {
            nvs_commit(h);
        } else {
            ESP_LOGW(TAG, "保存配置到NVS失败: %s", esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "保存配置到NVS失败: nvs_open %s", esp_err_to_name(err));
    }
}

/* 加载: TF -> NVS -> 默认 三级回退.
 * 注意: 此函数不调用 audio_player_set_volume, 因为 audio_player_init 可能尚未完成.
 * 启动流程: menu_init -> menu_config_load 写入 settings -> main.c 调 menu_apply_volume_setting 同步到硬件.
 * 返回: 1=来自TF, 0=来自NVS, -1=默认. */
static int menu_config_load(void) {
    bbk_config_t c;
    bool have = false;
    int src = -1;

    /* 1) TF 卡优先 */
    if (sd_is_mounted()) {
        FILE *f = fopen(BBK_CFG_PATH, "rb");
        if (f) {
            size_t r = fread(&c, 1, sizeof(c), f);
            fclose(f);
            size_t cfg_min = sizeof(c) - 5;   /* 兼容 v3 (比当前结构少 5 字节) */
            if ((r >= cfg_min && r <= sizeof(c)) && config_valid(&c)) {
                have = true; src = 1;
            } else if (r < cfg_min || r > sizeof(c)) {
                ESP_LOGW(TAG, "TF 配置文件大小不符 (%u/%u), 跳过", (unsigned)r, (unsigned)sizeof(c));
            } else {
                ESP_LOGW(TAG, "TF 配置 magic/version 不符, 跳过");
            }
        } else if (errno != ENOENT) {
            ESP_LOGW(TAG, "打开 TF 配置失败 (errno=%d)", errno);
        }
    }

    /* 2) 系统 NVS (TF 没有或无效时) */
    if (!have) {
        nvs_handle_t h;
        esp_err_t err = nvs_open("menu_settings", NVS_READONLY, &h);
        if (err == ESP_OK) {
            bbk_config_t nc;
            size_t len = sizeof(nc);
            if (nvs_get_blob(h, "all", &nc, &len) == ESP_OK &&
                len >= sizeof(nc) - 5 && len <= sizeof(nc) && config_valid(&nc)) {
                c = nc; have = true; src = 0;
            } else {
                /* 兼容旧版: 仅 volume 单独存于 NVS key="volume" (0-10) */
                uint8_t vol = 0;
                if (nvs_get_u8(h, "volume", &vol) == ESP_OK && vol <= 10) {
                    g_menu.settings.volume = vol;
                    ESP_LOGI(TAG, "从旧版 NVS 迁移音量: %d", vol);
                }
            }
            nvs_close(h);
        }
    }

    if (have) {
        config_unpack(&c);
        ESP_LOGI(TAG, "加载配置: 来源=%s", src == 1 ? "TF卡" : "NVS");
    } else {
        ESP_LOGI(TAG, "无保存配置, 使用默认值");
    }
    return src;
}

/* 把当前 g_menu.settings.volume 同步到 audio_player.
 * 在 audio_player_init 完成后调用, 把开机时加载的音量应用到底层硬件. */
void menu_apply_volume_setting(void) {
    audio_player_set_volume(volume_step_to_percent(g_menu.settings.volume));
    /* V1.0.68: 开机时应用"禁用音频" (全局强制静音) */
    audio_player_set_muted(g_menu.settings.audio_disable);
    ESP_LOGI(TAG, "应用音量设置: 档位=%d 百分比=%d%% 禁用音频=%d",
             g_menu.settings.volume, volume_step_to_percent(g_menu.settings.volume),
             g_menu.settings.audio_disable ? 1 : 0);
}

/* 音量: 0-10 共 11 档 (0 静音, 10-100%) */
static int volume_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    snprintf(buf[n++], 64, "音量: %d", (int)state->settings.volume);
    snprintf(buf[n++], 64, "静音: %s", state->settings.mute ? "开" : "关");
    snprintf(buf[n++], 64, "返回");
    (void)max;
    return n;
}
static bool volume_on_lr(menu_state_t *state, int idx, bool is_right) {
    if (idx == 0) {
        /* 0-9 共 10 档, 每次按 LR 走 1 档 */
        int step = is_right ? 1 : -1;
        int vol = (int)state->settings.volume + step;
        if (vol < 0) vol = 0;
        if (vol > 10) vol = 10;
        state->settings.volume = (uint8_t)vol;
        /* 同步到 audio_player: 0->0%, 1-9->11%-100% 线性 */
        audio_player_set_volume(volume_step_to_percent(vol));
        /* 持久化: 下次启动恢复当前音量 */
        menu_config_save();
        state->needs_redraw = true;
        return true;
    }
    if (idx == 1) {
        if (is_right || !state->settings.mute) {
            state->settings.mute = !state->settings.mute;
            menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
            state->needs_redraw = true;
        }
        return true;
    }
    return false;
}
/* 前向声明: open_settings_dialog 在文件后面定义 */
static void open_settings_dialog(menu_state_t *state);
/* V1.0.67: 点击音量/静音项循环切换 (触摸点击 + 确认键都走这里) */
static void volume_on_select(menu_state_t *state, int idx) {
    if (idx == 0) {
        /* 音量: 单击 +1 档 (0-10 循环) */
        int v = (int)state->settings.volume + 1;
        if (v > 10) v = 0;
        state->settings.volume = (uint8_t)v;
        audio_player_set_volume(volume_step_to_percent(v));
        menu_config_save();
    } else if (idx == 1) {
        /* 静音: 单击切换 */
        state->settings.mute = !state->settings.mute;
        menu_config_save();
    } else if (idx == 1) {
        /* 静音: 单击切换 */
        state->settings.mute = !state->settings.mute;
        menu_config_save();
    }
    volume_build(state, state->list_dialog_items, 16);
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
}

static bool volume_on_confirm(menu_state_t *state, int idx) {
    if (idx == 0 || idx == 1) {
        volume_on_select(state, idx);   /* 点击音量/静音也循环切换 */
        return true;
    }
    if (idx == 2) {
        /* 返回: 关闭音量子页, 重开设置弹窗 (与时间设置/系统信息一致).
         * 用户需求: 弹窗间返回保持原菜单位置. */
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        open_settings_dialog(state);
        return true;
    }
    return false;
}

/* 音量调节: 改为统一 list_dialog 弹窗 (与设置/时间一致), 不再用全屏子页.
 * 左右键调音量/静音, 上下键在选项间移动, 实时刷新显示. */
static void volume_dialog_on_key(menu_state_t *state, int idx, menu_action_t action) {
    if (action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT) {
        bool right = (action == MENU_ACTION_RIGHT);
        if (idx == 0) {
            int vol = (int)state->settings.volume + (right ? 1 : -1);
            if (vol < 0) vol = 0;
            if (vol > 10) vol = 10;
            state->settings.volume = (uint8_t)vol;
            audio_player_set_volume(volume_step_to_percent(vol));
            menu_config_save();
        } else if (idx == 1) {
            if (right || !state->settings.mute) {
                state->settings.mute = !state->settings.mute;
                menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
            }
        } else {
            return;
        }
        /* 重建列表文本, 让"音量: X"/"静音: 开"实时更新 */
        volume_build(state, state->list_dialog_items, 16);
        state->list_dialog_content_dirty = true;
        state->needs_redraw = true;
        return;
    }
    /* 上下键: 在 音量 / 静音 / 返回 之间移动选中 */
    if (action == MENU_ACTION_UP) {
        if (state->list_dialog_selected > 0) state->list_dialog_selected--;
        else state->list_dialog_selected = state->list_dialog_count - 1;
    } else if (action == MENU_ACTION_DOWN) {
        if (state->list_dialog_selected < state->list_dialog_count - 1) state->list_dialog_selected++;
        else state->list_dialog_selected = 0;
    } else {
        return;
    }
    state->needs_redraw = true;
}

static void open_volume_dialog(menu_state_t *state) {
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再 build 写入子弹窗 items.
     * 旧代码先 build 再 open, 导致父弹窗 items 被覆盖后才压栈, 恢复时内容错误. */
    list_dialog_open(state, "音量调节", 0, volume_on_select);
    int cnt = volume_build(state, state->list_dialog_items, 16);
    state->list_dialog_count = cnt;
    state->list_dialog_on_key = volume_dialog_on_key;
}

/* 时间设置 - 弹窗 (用户需求: 改成弹窗, 并增加年/月/日) */
static int time_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    /* 从 RTC 读取当前时间 (年/月/日/时/分/秒) */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    state->settings.hour   = (uint8_t)t->tm_hour;
    state->settings.minute = (uint8_t)t->tm_min;
    state->settings.second = (uint8_t)t->tm_sec;
    snprintf(buf[n++], 64, "%d\xe5\xb9\xb4",  t->tm_year + 1900);
    snprintf(buf[n++], 64, "%d\xe6\x9c\x88",  t->tm_mon + 1);
    snprintf(buf[n++], 64, "%d\xe6\x97\xa5",  t->tm_mday);
    snprintf(buf[n++], 64, "%02d\xe6\x97\xb6", t->tm_hour);
    snprintf(buf[n++], 64, "%02d\xe5\x88\x86", t->tm_min);
    snprintf(buf[n++], 64, "%02d\xe7\xa7\x92", t->tm_sec);
    snprintf(buf[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");
    (void)state; (void)max;
    return n;
}

/* === 时间设置弹窗 (用户需求: 统一弹窗交互)
 * 弹窗里 6 项: 年 / 月 / 日 / 时 / 分 / 秒 (无"返回"项)
 *   - LEFT/RIGHT 切换字段 (选中项移动)
 *   - UP/DOWN 增减当前字段值 (仅更新草稿, 不写 RTC)
 *   - CONFIRM 把草稿写 RTC 并关闭弹窗 (返回上一级 settings 弹窗)
 *   - BACK   关闭弹窗并丢弃草稿 (不保存)
 *   顺序固定为 年→月→日→时→分→秒 (按 idx 0..5 排, 与用户需求一致) */
/* 前向声明: open_settings_dialog / open_time_dialog / open_sysinfo_dialog 在文件后面定义, 此处先引用 */
static void open_settings_dialog(menu_state_t *state);
static void open_time_dialog(menu_state_t *state);
static void open_sysinfo_dialog(menu_state_t *state);

/* 时间草稿: 调值时只更新这里, CONFIRM 才写 RTC, BACK 丢弃 */
static struct {
    bool valid;       /* 是否已初始化 */
    bool editing;     /* V1.0.42: 是否处于编辑模式 (确认键进入, 字段闪烁+上下调值) */
    int  year;        /* 实际年份 (e.g. 2026) */
    int  month;       /* 1..12 */
    int  day;         /* 1..31 */
    int  hour;        /* 0..23 */
    int  minute;      /* 0..59 */
    int  second;      /* 0..59 */
    int  field;       /* 当前选中的字段: 0=年 1=月 2=日 3=时 4=分 5=秒 (用于横向单行渲染) */
} s_time_draft;

/* 调值: field 0-5 对应 年/月/日/时/分/秒, delta=+1 / -1 */
static void time_dialog_adjust(menu_state_t *state, int field, int delta);
static void time_dialog_render(menu_state_t *state, st7305_handle_t *lcd,
                               int cx, int cy, int cw, int ch);

static void time_dialog_on_key(menu_state_t *state, int idx, menu_action_t action) {
    (void)idx;  /* 不用 idx, 用 s_time_draft.field */
    /* 兜底: 若草稿未初始化, 先从 RTC 取 */
    if (!s_time_draft.valid) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        s_time_draft.year   = t->tm_year + 1900;
        s_time_draft.month  = t->tm_mon + 1;
        s_time_draft.day    = t->tm_mday;
        s_time_draft.hour   = t->tm_hour;
        s_time_draft.minute = t->tm_min;
        s_time_draft.second = t->tm_sec;
        s_time_draft.field  = 0;
        s_time_draft.valid  = true;
        /* 不重置 editing: 默认保持编辑模式 (V1.0.46) */
    }
    /* V1.0.42: 编辑模式下 UP/DOWN 调值, LEFT/RIGHT 切换字段.
     * 非编辑模式下 LEFT/RIGHT 切换字段, UP/DOWN 忽略.
     * CONFIRM 由 time_dialog_on_select 处理 (非编辑→进编辑, 编辑→保存关闭).
     * BACK 由 list_dialog 通用分支处理 (关闭弹窗). */
    switch (action) {
        case MENU_ACTION_UP:
            if (s_time_draft.editing) {
                time_dialog_adjust(state, s_time_draft.field, +1);
                state->list_dialog_content_dirty = true;
                state->needs_redraw = true;
                state->list_dialog_local_update = false;
            }
            break;
        case MENU_ACTION_DOWN:
            if (s_time_draft.editing) {
                time_dialog_adjust(state, s_time_draft.field, -1);
                state->list_dialog_content_dirty = true;
                state->needs_redraw = true;
                state->list_dialog_local_update = false;
            }
            break;
        case MENU_ACTION_LEFT:
        case MENU_ACTION_RIGHT:
            /* 左右: 切换字段 (wrap), 编辑模式和非编辑模式都支持 */
            if (action == MENU_ACTION_LEFT) {
                if (s_time_draft.field > 0) s_time_draft.field--;
                else s_time_draft.field = 5;
            } else {
                if (s_time_draft.field < 5) s_time_draft.field++;
                else s_time_draft.field = 0;
            }
            /* 字段切换: 高亮位置变化, 需要重绘 */
            state->list_dialog_content_dirty = true;
            state->needs_redraw = true;
            state->list_dialog_local_update = false;
            break;
        default:
            break;
    }
}

/* 关闭弹窗的通用步骤: 优先恢复父弹窗(保持位置), 无父弹窗则关闭并触发 on_close.
 * V1.0.41: 改用 list_dialog_pop_parent 保持父弹窗选中位置. */
static void time_dialog_close(menu_state_t *state) {
    if (list_dialog_pop_parent(state)) {
        return;  /* 已恢复父弹窗, 保持原选中位置 */
    }
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->current_page = state->list_dialog_return_page;
    state->selected_index = state->main_selected_index;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    if (state->list_dialog_on_close) {
        void (*cb)(menu_state_t *) = state->list_dialog_on_close;
        state->list_dialog_on_close = NULL;
        cb(state);
    }
}

static void time_dialog_on_select(menu_state_t *state, int idx) {
    /* V1.0.42: idx==0 单行时间:
     *   - 非编辑模式: 按确认进入编辑模式, 当前字段开始闪烁, UP/DOWN 调值
     *   - 编辑模式: 按确认保存到 RTC 并关闭弹窗
     * idx==1 ("返回"项) 走 list_dialog 通用分支 (count-1), 不会走到这里. */
    (void)idx;
    if (!s_time_draft.editing) {
        /* 进入编辑模式 */
        s_time_draft.editing = true;
        state->list_dialog_content_dirty = true;
        state->needs_redraw = true;
        state->list_dialog_local_update = false;
        return;
    }
    /* 编辑模式: 保存并关闭 */
    if (s_time_draft.valid) {
        struct tm t = {0};
        t.tm_year = s_time_draft.year - 1900;
        t.tm_mon  = s_time_draft.month - 1;
        t.tm_mday = s_time_draft.day;
        t.tm_hour = s_time_draft.hour;
        t.tm_min  = s_time_draft.minute;
        t.tm_sec  = s_time_draft.second;
        t.tm_isdst = -1;
        time_t ts = mktime(&t);
        if (ts != (time_t)-1) {
            struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            state->settings.hour   = (uint8_t)s_time_draft.hour;
            state->settings.minute = (uint8_t)s_time_draft.minute;
            state->settings.second = (uint8_t)s_time_draft.second;
            /* V1.0.46: 持久化到 NVS, 断电重启后恢复 */
            time_save_to_nvs();
            /* V1.0.46: 已保存提示 (0.5s) */
            snprintf(state->hint_text, sizeof(state->hint_text),
                     "\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xe6\x97\xb6\xe9\x97\xb4"); /* 已保存时间 */
            state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 500;
            ESP_LOGI(TAG, "时间已保存: %04d-%02d-%02d %02d:%02d:%02d",
                     s_time_draft.year, s_time_draft.month, s_time_draft.day,
                     s_time_draft.hour, s_time_draft.minute, s_time_draft.second);
        } else {
            ESP_LOGW(TAG, "mktime 失败, 时间未保存");
        }
    }
    s_time_draft.valid = false;
    s_time_draft.editing = false;
    time_dialog_close(state);
}

/* 调值: field 0-5, delta=+1/-1, 仅更新草稿 */
static void time_dialog_adjust(menu_state_t *state, int field, int delta) {
    (void)state;
    switch (field) {
        case 0: {
            int y = s_time_draft.year + delta;
            if (y < 2020) y = 2020;
            if (y > 2099) y = 2099;
            s_time_draft.year = y;
            break;
        }
        case 1: {
            int m = s_time_draft.month + delta;
            while (m < 1) m += 12;
            while (m > 12) m -= 12;
            s_time_draft.month = m;
            break;
        }
        case 2: {
            /* 日: 根据当月天数 clamp (含闰年) */
            static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int m = s_time_draft.month;
            int base_dim = dim[m - 1];
            if (m == 2) {
                int yy = s_time_draft.year;
                bool leap = ((yy % 4 == 0) && (yy % 100 != 0)) || (yy % 400 == 0);
                if (leap) base_dim = 29;
            }
            int d = s_time_draft.day + delta;
            if (d < 1) d = base_dim;
            if (d > base_dim) d = 1;
            s_time_draft.day = d;
            break;
        }
        case 3: {
            int h = s_time_draft.hour + delta;
            if (h < 0) h = 23;
            if (h > 23) h = 0;
            s_time_draft.hour = h;
            break;
        }
        case 4: {
            int mi = s_time_draft.minute + delta;
            if (mi < 0) mi = 59;
            if (mi > 59) mi = 0;
            s_time_draft.minute = mi;
            break;
        }
        case 5: {
            int s = s_time_draft.second + delta;
            if (s < 0) s = 59;
            if (s > 59) s = 0;
            s_time_draft.second = s;
            break;
        }
    }
}

/* 时间设置弹窗自定义渲染: 单行横向显示 "年 月 日 时:分:秒", 当前字段反色高亮.
 * 用户需求: 全部合并成一排, 横向排列, 时间分钟秒用":"分隔, 一排搞定.
 * 高度: 24 像素 (与 list_dialog 行内文本高度一致) */
static void time_dialog_render(menu_state_t *state, st7305_handle_t *lcd,
                               int cx, int cy, int cw, int ch) {
    (void)state;
    if (!s_time_draft.valid) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        s_time_draft.year   = t->tm_year + 1900;
        s_time_draft.month  = t->tm_mon + 1;
        s_time_draft.day    = t->tm_mday;
        s_time_draft.hour   = t->tm_hour;
        s_time_draft.minute = t->tm_min;
        s_time_draft.second = t->tm_sec;
        s_time_draft.field  = 0;
        s_time_draft.valid  = true;
    }

    /* 各字段字符串 */
    char year_s[8], month_s[8], day_s[8], hour_s[8], min_s[8], sec_s[8];
    snprintf(year_s,  sizeof(year_s),  "%d",  s_time_draft.year);
    snprintf(month_s, sizeof(month_s), "%d",  s_time_draft.month);
    snprintf(day_s,   sizeof(day_s),   "%d",  s_time_draft.day);
    snprintf(hour_s,  sizeof(hour_s),  "%02d", s_time_draft.hour);
    snprintf(min_s,   sizeof(min_s),   "%02d", s_time_draft.minute);
    snprintf(sec_s,   sizeof(sec_s),   "%02d", s_time_draft.second);

    /* 各字段实际渲染宽度 (动态: month/day 可能是 1 或 2 位) */
    int year_w  = text_width(year_s);
    int month_w = text_width(month_s);
    int day_w   = text_width(day_s);
    int hour_w  = text_width(hour_s);
    int min_w   = text_width(min_s);
    int sec_w   = text_width(sec_s);

    /* 分隔符宽度 (固定) */
    const int nian_w   = 24;  /* 年 (3-byte UTF-8) */
    const int yue_w    = 24;  /* 月 */
    const int ri_w     = 24;  /* 日 */
    const int space_w  = 16;  /* 空格 */
    const int colon_w  = 16;  /* ":" */

    /* 总宽度 */
    int total_w = year_w + nian_w + month_w + yue_w + day_w + ri_w + space_w +
                  hour_w + colon_w + min_w + colon_w + sec_w;

    /* 居中起始 x; 垂直居中 y (ch/2 - 12) */
    int x = cx + (cw - total_w) / 2;
    if (x < cx) x = cx;
    int y = cy + (ch - 24) / 2;
    if (y < cy) y = cy;

    /* 计算每个字段的 x 位置 (用于高亮反色绘制) */
    int field_x[6], field_w[6];
    int xx = x;
    field_x[0] = xx; field_w[0] = year_w;  xx += year_w + nian_w;
    field_x[1] = xx; field_w[1] = month_w; xx += month_w + yue_w;
    field_x[2] = xx; field_w[2] = day_w;   xx += day_w + ri_w + space_w;
    field_x[3] = xx; field_w[3] = hour_w;  xx += hour_w + colon_w;
    field_x[4] = xx; field_w[4] = min_w;   xx += min_w + colon_w;
    field_x[5] = xx; field_w[5] = sec_w;

    /* === 1. 绘制当前字段的高亮背景 ===
     * V1.0.42: 非编辑模式=反色高亮常亮; 编辑模式=500ms 闪烁 (反色/正常交替) */
    int cur = s_time_draft.field;
    bool highlight_cur = true;  /* 是否高亮当前字段 */
    if (s_time_draft.editing) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        highlight_cur = ((now_ms / 500) % 2) == 0;  /* 500ms 闪一次 */
    }
    if (cur >= 0 && cur <= 5 && highlight_cur) {
        int hx = field_x[cur];
        int hw = field_w[cur];
        fill_rect(lcd, hx - 2, y - 2, hx + hw + 1, y + 24 + 1, ST7305_COLOR_BLACK);
    }

    /* === 2. 绘制完整时间字符串 ===
     * 当前字段: inverted = highlight_cur (高亮时反色, 闪烁隐藏时正常)
     * 其他字段: inverted=false (白底黑字) */
    int cx0 = x;
    draw_text(lcd, cx0, y, year_s,  (cur == 0 && highlight_cur));  cx0 += year_w;
    draw_text(lcd, cx0, y, "\xe5\xb9\xb4", false);  cx0 += nian_w;        /* 年 */
    draw_text(lcd, cx0, y, month_s, (cur == 1 && highlight_cur));  cx0 += month_w;
    draw_text(lcd, cx0, y, "\xe6\x9c\x88", false);  cx0 += yue_w;         /* 月 */
    draw_text(lcd, cx0, y, day_s,   (cur == 2 && highlight_cur));  cx0 += day_w;
    draw_text(lcd, cx0, y, "\xe6\x97\xa5", false);  cx0 += ri_w;          /* 日 */
    draw_text(lcd, cx0, y, " ",      false);  cx0 += space_w;
    draw_text(lcd, cx0, y, hour_s,  (cur == 3 && highlight_cur));  cx0 += hour_w;
    draw_text(lcd, cx0, y, ":",      false);  cx0 += colon_w;
    draw_text(lcd, cx0, y, min_s,   (cur == 4 && highlight_cur));  cx0 += min_w;
    draw_text(lcd, cx0, y, ":",      false);  cx0 += colon_w;
    draw_text(lcd, cx0, y, sec_s,   (cur == 5 && highlight_cur));
}

/* 系统信息 */
static int sysinfo_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    snprintf(buf[n++], 64, "固件: 1.1");
    snprintf(buf[n++], 64, "LCD: ST7305 400x300");
    snprintf(buf[n++], 64, "Flash: 16MB");
    snprintf(buf[n++], 64, "PSRAM: 8MB");
    snprintf(buf[n++], 64, "BY: LinIT");
    snprintf(buf[n++], 64, "返回");
    (void)max;
    return n;
}
static bool sysinfo_on_confirm(menu_state_t *state, int idx) {
    if (idx == 5) {
        state->current_page = MENU_PAGE_SETTINGS;
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;
    }
    return false;
}

/* ---- 文曲星. 游戏子页 build (扫描 TF) ---- */

/* 截断 UTF-8 字符串到 max_chars 个字符 (中文 1 字, ASCII 算 1 字)
 * 超过部分用 "..." 替代
 * 用途: 游戏列表显示 (避免长名字超出列表宽度)
 * 返回写入的字节数 (不含结尾 0) */
static int truncate_name(char *out, size_t out_size, const char *src, int max_chars) {
    if (out_size == 0) return 0;
    if (src == NULL) { out[0] = '\0'; return 0; }

    int char_count = 0;  /* 已复制的字符数 */
    int out_pos = 0;     /* 输出位置 */
    int src_pos = 0;     /* 输入位置 */

    /* 统计有效字符数 (用于判断是否需要省略) */
    int total_chars = 0;
    while (src[src_pos] != '\0') {
        uint8_t c = (uint8_t)src[src_pos];
        int step = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            else { src_pos++; continue; }  /* 跳过非法字节 */
        }
        src_pos += step;
        total_chars++;
    }

    /* 如果总字符数 <= max_chars, 直接复制 */
    if (total_chars <= max_chars) {
        int n = 0;
        for (int i = 0; src[i] && n < (int)out_size - 1; i++) {
            out[n++] = src[i];
        }
        out[n] = '\0';
        return n;
    }

    /* 需要截断: 复制 max_chars 个字符 + "..." */
    src_pos = 0;
    while (char_count < max_chars && src[src_pos] != '\0') {
        uint8_t c = (uint8_t)src[src_pos];
        int step = 1;
        if (c < 0x80) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        else { src_pos++; continue; }

        /* 复制这个字符 (如果空间够) */
        if (out_pos + step < (int)out_size - 4) {  /* 留 "..." 位置 */
            for (int i = 0; i < step; i++) {
                out[out_pos++] = src[src_pos + i];
            }
        }
        src_pos += step;
        char_count++;
    }

    /* 追加 "..." */
    if (out_pos + 3 < (int)out_size) {
        out[out_pos++] = '.';
        out[out_pos++] = '.';
        out[out_pos++] = '.';
    }
    out[out_pos] = '\0';
    return out_pos;
}

/* ============================================================
 * 电子词典: 左右分栏布局 (左文件夹 / 右游戏)
 * ============================================================
 * 注: g_folder_names / g_folder_count / get_selected_folder_name
 *     已在前面 (select_game_on_confirm 之前) 提前声明, 避免 forward decl 问题 */
#define SELECT_LEFT_W      100    /* 左栏宽度 (像素) */
#define SELECT_GAP          6     /* 左右栏间距 */
#define SELECT_RIGHT_X     (SELECT_LEFT_W + SELECT_GAP)  /* 104 */
#define SELECT_RIGHT_W     (SCREEN_W - SELECT_RIGHT_X - 4) /* 292 */
#define SELECT_LIST_Y       30    /* 列表起点 y (状态栏 24px + 6px 间距) */
#define SELECT_LIST_BOTTOM (SCREEN_H - 4)  /* 列表底部 */
#define SELECT_ITEM_H       32    /* 每行高度 */
#define SELECT_MAX_VISIBLE ((SELECT_LIST_BOTTOM - SELECT_LIST_Y) / SELECT_ITEM_H)
#define SELECT_TRUNC_CHARS  12    /* 截断显示字符数 (含省略号) */

/* === 右栏缓存 (移到文件作用域, 以便游戏设置切换时能失效缓存) ===
 * - s_cached_folder_idx: 上次扫描的 folder_idx. -1 表示尚未扫描.
 * - s_cached_game_count: 上次扫描的项数.
 * - s_cache_dirty: 设置项被切换后置 1, 强制重建右栏 (因为显示文本会变化).
 *
 * 之所以需要 dirty 标志: 设置项 (全屏/声音/状态栏) 的开关状态变化时,
 * select_folder_idx 不变, 但 g_sub_items 里的文字 "游戏全屏: 开" -> "关" 需要更新.
 * 真实文件夹扫描是慢操作, 仍保留 folder_idx 缓存避免无谓的 SD 卡读取. */
static int s_cached_folder_idx = -1;
static int s_cached_game_count  = 0;
static bool s_cache_dirty       = true;  /* 启动时强制扫一次 */

/* === 公共失效接口 ===
 * 供 game_settings_on_confirm 等切换设置后调用, 强制 select_game_build 重建右栏. */
static void select_game_invalidate_cache(void) {
    s_cache_dirty = true;
}

/* ========================================================================
 *  V1.0.49: 平台目录/扩展名映射 (console 家族 select_engine)
 *  select_engine: 0=GB, 1=GBC, 2=FC(NES 合并), 3=arduboy
 *  各平台使用独立根目录, 不再共用 /sdcard/gb.
 * ======================================================================== */
static const char *platform_root_dir(int engine) {
    switch (engine) {
        case 1: return "/sdcard/gbc";
        case 2: return "/sdcard/nes";
        case 3: return "/sdcard/AB";
        default: return "/sdcard/gb";
    }
}

static const char *platform_ext(int engine) {
    switch (engine) {
        case 1: return ".gbc";
        case 2: return ".nes";   /* FC 使用 NES 文件 */
        case 3: return ".hex";
        default: return ".gb";
    }
}

/* 二级菜单状态栏标题 (console 家族) */
static const char *platform_title(int engine) {
    switch (engine) {
        case 1: return "GBC \xe6\xb8\xb8\xe6\x88\x8f";                /* GBC 游戏 */
        case 2: return "NES \xe6\xb8\xb8\xe6\x88\x8f";                /* NES 游戏 */
        case 3: return "arduboy \xe6\xb8\xb8\xe6\x88\x8f";            /* arduboy 游戏 */
        default: return "GB \xe6\xb8\xb8\xe6\x88\x8f";                /* GB 游戏 */
    }
}

/* 当前页面/引擎对应的收藏引擎 ID (电子词典=BBK; console 各平台独立).
 * select_engine: 0=GB, 1=GBC, 2=FC(NES 合并), 3=arduboy */
static fav_engine_t state_fav_engine(const menu_state_t *state) {
    if (state->select_mode == 0) return FAV_ENGINE_BBK;
    switch (state->select_engine) {
        case 0: return FAV_ENGINE_GB;
        case 1: return FAV_ENGINE_GBC;
        case 2: return FAV_ENGINE_FC;   /* FC (NES+FC 合并) */
        case 3:
        default: return FAV_ENGINE_AB;
    }
}

/* 判断路径是否属于当前游戏页引擎的文件 (BBK: .gam; 各平台: 对应扩展名).
 * 用于收藏栏过滤, 防止不同平台/引擎的收藏串栏. */
static bool is_page_game_file(const char *path, const menu_state_t *state) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    if (state->select_mode == 1) {
        return strcasecmp(dot, platform_ext(state->select_engine)) == 0;
    }
    return strcasecmp(dot, ".gam") == 0;
}

/* 当前 console 页引擎对应的文件扩展名 (".gb" / ".gbc" / ".nes" / ".hex") */
static const char *current_gb_ext(const menu_state_t *state) {
    return platform_ext(state->select_engine);
}

static int select_game_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;

    /* 兼容性: 原 static 变量已移到文件作用域, 这里留空占位以防其它代码使用 */

    /* 首次进入页面 / 切换回来时, 一次性扫描子文件夹名, 缓存到 g_folder_names */
    if (!state->select_loaded) {
        /* V1.0.48: 按页面+平台扫描文件夹 (平台各自独立目录) */
        g_folder_count = (state->select_mode == 1)
            ? scan_platform_folders(platform_root_dir(state->select_engine), g_folder_names, MAX_FOLDER_NAMES)
            : scan_bbk_folders(g_folder_names, MAX_FOLDER_NAMES);
        state->select_loaded = true;
        /* V1.0.68: 进入页面默认焦点在右栏(内容页), 选中第一个游戏 */
        state->select_focus = 1;
        int max_idx = 2 + g_folder_count - 1;  /* 0=设置, 1=收藏, 2=全部, 3..N+2=文件夹 */
        if (state->select_folder_idx < 0 || state->select_folder_idx > max_idx) {
            state->select_folder_idx = 1;  /* 默认收藏 */
        }
        /* 不再强制把焦点拉回左栏: "游戏设置"项的右栏也有 4 个设置项, 可正常切换焦点 */
        state->select_folder_scroll = 0;
        state->select_game_idx = 0;
        state->select_game_scroll = 0;
        /* 失效缓存, 强制扫描右栏 */
        s_cached_folder_idx = -1;
        s_cached_game_count  = 0;
        s_cache_dirty = true;

        /* 引擎已在开机时后台加载, 无需再触发 */
    }

    /* 性能优化: 仅在 folder 变化 或 设置项被切换 (dirty) 时重新扫描右栏.
     * 真实文件夹扫描是慢操作 (读 SD 卡), 必须有 folder_idx 缓存;
     * 但设置项的开关文字会变, 即使 folder_idx 不变也要重建, 用 s_cache_dirty 标志. */
    if (s_cache_dirty || state->select_folder_idx != s_cached_folder_idx) {
        s_cache_dirty = false;
        /* === 侧栏特殊项处理 === */
        if (state->select_folder_idx == 0) {
            /* 游戏设置: 右栏同时显示 4 个设置项, 与游戏文件夹风格一致
             * (用户需求: 点击左侧"游戏设置"后, 右栏立即出现设置项,
             *  而非跳转新页面或显示空右栏) */
            n = game_settings_build(state, buf, max);
            s_cached_folder_idx = state->select_folder_idx;
            s_cached_game_count  = n;
            return n;
        }
        if (state->select_folder_idx == 1) {
            /* 收藏: 列出所有收藏路径, 取文件名显示 (按页面模式过滤: GB 只看 .gb, 电子词典只看 .gam)
             * 不再添加"返回"项: 左右键切换侧栏即可回到文件夹列表 */
            int fav_count = 0;
            const char *const *favs = favorites_list(state_fav_engine(state), &fav_count);
            for (int i = 0; i < fav_count && n < max; i++) {
                const char *path = favs[i];
                /* V1.0.46: 过滤其他平台的收藏 (防 GB/电子词典收藏串栏) */
                if (!is_page_game_file(path, state)) continue;
                /* 跳过空路径(NVS 损坏时可能出现) */
                if (!path || path[0] == '\0') {
                    continue;
                }
                const char *slash = strrchr(path, '/');
                const char *name = slash ? slash + 1 : path;
                /* 跳过空文件名 */
                if (name[0] == '\0') {
                    continue;
                }
                strncpy(buf[n], name, 63);
                buf[n][63] = '\0';
                /* 去除后缀名 (.BIN / .gam / .nes 等), 用户需求: 收藏栏不显示后缀 */
                char *dot = strrchr(buf[n], '.');
                if (dot && dot != buf[n]) {
                    /* 确保 '.' 出现在最后一个 '/' 之后 (避免误伤目录中的点号) */
                    char *slash_in_name = strrchr(buf[n], '/');
                    if (!slash_in_name || dot > slash_in_name) {
                        *dot = '\0';
                    }
                }
                truncate_name(buf[n], 64, buf[n], SELECT_TRUNC_CHARS);
                n++;
            }
            s_cached_folder_idx = state->select_folder_idx;
            s_cached_game_count  = n;
            return n;
        }
        /* === 真实子文件夹: 正常扫描 === */
        const char *folder = get_selected_folder_name(state, state->select_folder_idx);
        int found = (state->select_mode == 1)
            ? scan_platform_games(platform_root_dir(state->select_engine), folder, current_gb_ext(state), buf + n, max - n)
            : scan_bbk_games_in_folder(folder, buf + n, max - n);
        /* 截断游戏名 (12 字符 + "...") */
        for (int i = 0; i < found; i++) {
            char tmp[64];
            memcpy(tmp, buf[n + i], 64);
            truncate_name(buf[n + i], 64, tmp, SELECT_TRUNC_CHARS);
        }
        n += found;
        s_cached_folder_idx = state->select_folder_idx;
        s_cached_game_count  = n;
        (void)state;
        return n;
    }

    /* 缓存命中: 直接返回上次的游戏数 (g_sub_items 已被缓存内容填充) */
    n = s_cached_game_count;
    (void)state;
    (void)max;
    (void)buf;
    return n;
}

/* 渲染左栏: 特殊项(0=游戏设置, 1=收藏) + 文件夹列表
 * 起点 y=SELECT_LIST_Y, 每行 SELECT_ITEM_H 像素
 *  - 当前选中项由 state->select_folder_idx 决定
 *  - 当前焦点列由 state->select_focus 决定 (0=左)
 *  - 选中项反色 (焦点列=左时); 文字居中显示 */
static void draw_folder_pane(st7305_handle_t *lcd, menu_state_t *state) {
    int x0 = 2;
    int x1 = x0 + SELECT_LEFT_W - 4;  /* 右边 4 像素留给中间分隔线 */
    int y  = SELECT_LIST_Y;

    /* 总数 = 3 特殊项(设置/收藏/全部) + g_folder_count 个真实子文件夹 */
    int total = 3 + g_folder_count;
    int sel = state->select_folder_idx;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;

    /* 滚动 */
    int max_vis = (SELECT_LIST_BOTTOM - y) / SELECT_ITEM_H;
    if (max_vis < 1) max_vis = 1;
    if (sel < state->select_folder_scroll) state->select_folder_scroll = sel;
    if (sel >= state->select_folder_scroll + max_vis)
        state->select_folder_scroll = sel - max_vis + 1;
    int scroll = state->select_folder_scroll;

    /* 绘制可见项 (无图标, 文字居中) */
    for (int i = 0; i < max_vis && (scroll + i) < total; i++) {
        int idx = scroll + i;
        int yy = y + i * SELECT_ITEM_H;
        bool is_sel = (idx == sel);

        /* V1.0.68: 统一选中样式: 下方 2px 横线 (不再整行反色块) */
        char trunc[64];
        if (idx == 0) {
            /* 游戏设置 */
            const char *name = "\xe6\xb8\xb8\xe6\x88\x8f\xe8\xae\xbe\xe7\xbd\xae";  /* 游戏设置 */
            truncate_name(trunc, sizeof(trunc), name, 6);
        } else if (idx == 1) {
            /* 收藏栏 */
            const char *name = "\xe6\x94\xb6\xe8\x97\x8f\xe6\xa0\x8f";  /* 收藏栏 */
            truncate_name(trunc, sizeof(trunc), name, 6);
        } else if (idx == 2) {
            /* 全部 (根目录) */
            const char *name = "\xe5\x85\xa8\xe9\x83\xa8";  /* 全部 */
            truncate_name(trunc, sizeof(trunc), name, 6);
        } else {
            const char *name = g_folder_names[idx - 3];
            truncate_name(trunc, sizeof(trunc), name, 6);
        }
        int tw = text_width(trunc);
        int tx = x0 + (SELECT_LEFT_W - 4 - tw) / 2;
        if (tx < x0) tx = x0;
        draw_text(lcd, tx, yy + 8, trunc, false);

        /* 选中项下方画 2px 横线 (画在文字之后, 贴行底缘) */
        if (is_sel) {
            draw_hline(lcd, x0, x1, yy + SELECT_ITEM_H - 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, x0, x1, yy + SELECT_ITEM_H, ST7305_COLOR_BLACK);
        }
    }
}

/* 渲染右栏: 游戏列表 (无"返回"项, BACK 键返回主菜单)
 *  - 无标题, 无图标, 无分隔线
 *  - 当前选中项由 state->select_game_idx 决定
 *  - 当前焦点列由 state->select_focus 决定 (1=右)
 *  - 内部依赖 g_sub_items / g_sub_count (已由 select_game_build 填充)
 *  - 选中项反色 (焦点列=右时); 文字居中显示
 *  - 空收藏 (select_folder_idx==1 且 g_sub_count==0) 时, 显示提示引导用户添加 */
static void draw_game_pane(st7305_handle_t *lcd, menu_state_t *state) {
    int x0 = SELECT_RIGHT_X;
    int x1 = SCREEN_W - 4;
    int y  = SELECT_LIST_Y;

    int total = g_sub_count;
    int sel = state->select_game_idx;
    if (sel < 0) sel = 0;
    if (sel >= total && total > 0) sel = total - 1;

    int max_vis = (SELECT_LIST_BOTTOM - y) / SELECT_ITEM_H;
    if (max_vis < 1) max_vis = 1;
    /* V1.0.68: 松手固定内容页后, 只做边界钳制; 正常时按选中项保持可见 */
    if (state->select_game_drag_fix) {
        int max_scroll = total - max_vis;
        if (max_scroll < 0) max_scroll = 0;
        if (state->select_game_scroll > max_scroll) state->select_game_scroll = max_scroll;
    } else {
        if (sel < state->select_game_scroll) state->select_game_scroll = sel;
        if (sel >= state->select_game_scroll + max_vis)
            state->select_game_scroll = sel - max_vis + 1;
    }
    int scroll = state->select_game_scroll;
    bool focus_right = (state->select_focus == 1);

    /* === 空收藏提示 ===
     * 用户需求: 收藏栏为空时, 第一个位置显示提示
     *   "在游戏名称按多功能键收藏游戏"
     * 位置: 右栏居中显示, 字体 24px, 颜色黑色 (非反色)
     * 不影响正常列表渲染, 仅 total==0 时触发. */
    if (total == 0 && state->select_folder_idx == 1) {
        /* 使用两行文本居中显示, 上行为"暂无收藏" 下行为操作引导 */
        const char *hint_line1 = "\xe6\x9a\x82\xe6\x97\xa0\xe6\x94\xb6\xe8\x97\x8f";  /* 暂无收藏 */
        const char *hint_line2 = "\xe5\x9c\xa8\xe6\xb8\xb8\xe6\x88\x8f\xe5\x90\x8d\xe7\xa7\xb0\xe6\x8c\x89\xe5\xa4\x9a\xe5\x8a\x9f\xe8\x83\xbd\xe9\x94\xae\xe6\x94\xb6\xe8\x97\x8f";  /* 在游戏名称按多功能键收藏 */
        /* 居中 y: 列表区上下居中, 上行 y = 中心-12, 下行 y = 中心+12 */
        int center_y = (SELECT_LIST_Y + SELECT_LIST_BOTTOM) / 2;
        /* 居中 x: 右栏内水平居中 */
        int rw = x1 - x0;
        int w1 = text_width(hint_line1);
        int w2 = text_width(hint_line2);
        if (w1 > rw - 8) {
            /* 过长, 用截断版本 */
            char tmp[64];
            truncate_name(tmp, sizeof(tmp), hint_line1, 8);
            draw_text(lcd, x0 + (rw - text_width(tmp)) / 2, center_y - 16, tmp, false);
        } else {
            draw_text(lcd, x0 + (rw - w1) / 2, center_y - 16, hint_line1, false);
        }
        if (w2 > rw - 8) {
            char tmp[64];
            truncate_name(tmp, sizeof(tmp), hint_line2, 12);
            draw_text(lcd, x0 + (rw - text_width(tmp)) / 2, center_y + 12, tmp, false);
        } else {
            draw_text(lcd, x0 + (rw - w2) / 2, center_y + 12, hint_line2, false);
        }
        return;
    }

    /* 绘制可见项 (无图标, 文字居中).
     * V1.0.68: 拖动时按偏移量多渲染几行, 避免上滑后底部空白 (内容没提前加载). */
    int drag_off = state->select_game_drag_offset;
    int extra = 0;
    if (drag_off < 0) extra = (-drag_off + SELECT_ITEM_H - 1) / SELECT_ITEM_H + 1;
    if (drag_off > 0) extra = ( drag_off + SELECT_ITEM_H - 1) / SELECT_ITEM_H + 1;
    int idx_start = scroll - extra;
    if (idx_start < 0) idx_start = 0;
    int idx_end = scroll + max_vis - 1 + extra;
    if (idx_end >= total) idx_end = total - 1;

    for (int idx = idx_start; idx <= idx_end; idx++) {
        int yy = y + (idx - scroll) * SELECT_ITEM_H + drag_off;
        /* 整行完全在可视区外则跳过 */
        if (yy + SELECT_ITEM_H <= y || yy >= SELECT_LIST_BOTTOM) continue;
        bool is_sel = (idx == sel);

        /* V1.0.68: 内容页选中 = 整行加黑 (原样式); 侧栏才是横线 */
        if (is_sel && focus_right) {
            fill_rect(lcd, x0, yy, x1, yy + SELECT_ITEM_H - 2, ST7305_COLOR_BLACK);
        }

        /* 文字: 直接使用 g_sub_items[idx], 跳过空字符串/空项 */
        const char *text = g_sub_items[idx];
        if (text == NULL || text[0] == '\0') text = " ";

        /* 截断显示 (右栏宽度更宽, 12 字符) */
        char trunc[64];
        truncate_name(trunc, sizeof(trunc), text, SELECT_TRUNC_CHARS);
        int tw = text_width(trunc);
        int tx;
        if (state->select_folder_idx == 0) {
            if (idx == total - 1) {
                /* 最后一项"还原映射": 居中显示 (按钮样式) */
                tx = x0 + ((x1 - x0) - tw) / 2;
            } else {
                /* 其余设置项: 左对齐 + 右移 (BBK=1.5汉字 36px, 其他=2汉字 48px),
                 * 名称全角空格补位保证所有冒号竖直对齐 */
                int indent = (state->select_mode == 1) ? (8 + 48) : (8 + 48 - 12);
                tx = x0 + indent;
            }
        } else {
            tx = x0 + ((x1 - x0) - tw) / 2;
        }
        if (tx < x0) tx = x0;
        draw_text(lcd, tx, yy + 8, trunc, is_sel && focus_right);
    }
}

/* 渲染电子词典/GB 分栏 UI (顶替原 render_game_cards, 接入 menu_render)
 * 性能优化: 仅在左侧文件夹变化时重新扫描右栏游戏 (静态缓存)
 * 布局: 状态栏显示二级菜单名称; 左右两栏无图标/无内部横线/无标题, 仅中间一条竖直分隔线
 * 提示: 按下多功能键 -> 短时提示 0.5s (如 "已添加收藏栏"), 以屏幕正中弹窗显示
 *       (2px 黑色描边, 白色背景, 固定尺寸 200x60, 离屏幕边缘各 15px 边距). */
static void render_select_game_two_cols(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 状态栏 - 显示二级菜单名称 (V1.0.48: 各平台显示对应名称) */
    const char *title = (state->current_page == MENU_PAGE_GB_GAME)
        ? platform_title(state->select_engine)
        : "\xe7\x94\xb5\xe5\xad\x90\xe8\xaf\x8d\xe5\x85\xb8";  /* 电子词典 */
    menu_draw_status_bar(lcd, &state->settings, title);

    /* 填充分栏数据: select_game_build 会按当前 select_folder_idx 扫描右侧游戏
     * 并设置 g_sub_count
     * 注: select_game_build 内部已做 select_loaded 缓存, 二次调用仅在 folder 变化时重扫右栏 */
    g_sub_count = select_game_build(state, g_sub_items, sizeof(g_sub_items) / sizeof(g_sub_items[0]));

    /* 左栏: 文件夹 */
    draw_folder_pane(lcd, state);

    /* 右栏: 游戏 */
    draw_game_pane(lcd, state);

    /* 左右栏之间的竖直分隔线 (贯穿列表区, 即"文件夹/游戏"分隔) */
    draw_vline(lcd, SELECT_LEFT_W + SELECT_GAP / 2, SELECT_LIST_Y,
               SELECT_LIST_BOTTOM, ST7305_COLOR_BLACK);

    /* === 短时提示 (屏幕中央弹窗) ===
     * V1.0.33: 改为统一紧凑提示弹窗模板 (draw_notice_popup).
     * 边框离字 3px (字四边 3px 内边距), 弹窗尺寸按文本字符数自动计算. */
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (state->hint_text[0] != '\0' && now_ms < state->hint_until_ms) {
        draw_notice_popup(state->lcd, state->hint_text);
    } else if (state->hint_text[0] != '\0' && now_ms >= state->hint_until_ms) {
        /* 提示过期: 清空, 避免下次误显示 */
        state->hint_text[0] = '\0';
    }

    /* 刷新由 menu_render 末尾统一处理 */
}

/* === 电子书双栏渲染 (镜像游戏菜单) ===
 * 注意: book_scan_folders / book_build_right 等定义在文件下方 (电子书 UI 区),
 * 这里先声明供渲染函数使用 */
#define BOOK_DIR "/sdcard/books"
#define BOOK_MAX_FOLDERS 32
/* 电子书二级菜单旋转输出备用缓冲 (15KB PSRAM, 仅旋转时使用) */
EXT_RAM_BSS_ATTR static uint8_t s_book_rot_buf[15000];
EXT_RAM_BSS_ATTR static char g_book_folder_names[BOOK_MAX_FOLDERS][64];
static int g_book_folder_count = 0;
static int g_book_root_count = 0;   /* 根目录 .txt 数量 (决定"临时目录"是否显示) */
/* 书单实际文件名 (带扩展名), 与 g_sub_items 一一对应 */
EXT_RAM_BSS_ATTR static char g_book_files[128][64];

static void book_scan_folders(menu_state_t *state);
static int  book_sidebar_total(void);
static bool book_is_temp_idx(int idx);
static int  book_build_right(menu_state_t *state, char buf[][64], int max);
static int  book_utf8_count(const char *s);

static void draw_book_folder_pane(st7305_handle_t *lcd, menu_state_t *state) {
    int x0 = 2;
    int x1 = x0 + SELECT_LEFT_W - 4;
    int y  = SELECT_LIST_Y;

    int total = book_sidebar_total();
    int sel = state->select_folder_idx;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;

    int max_vis = (SELECT_LIST_BOTTOM - y) / SELECT_ITEM_H;
    if (max_vis < 1) max_vis = 1;
    if (sel < state->select_folder_scroll) state->select_folder_scroll = sel;
    if (sel >= state->select_folder_scroll + max_vis)
        state->select_folder_scroll = sel - max_vis + 1;
    int scroll = state->select_folder_scroll;

    for (int i = 0; i < max_vis && (scroll + i) < total; i++) {
        int idx = scroll + i;
        int yy = y + i * SELECT_ITEM_H;
        bool is_sel = (idx == sel);
        const char *name;
        if (idx == 0) {
            name = "\xe8\xae\xbe\xe7\xbd\xae";  /* 设置 */
        } else if (idx == 1) {
            name = "\xe6\x94\xb6\xe8\x97\x8f\xe4\xb9\xa6\xe6\x9e\xb6";  /* 收藏书架 */
        } else if (book_is_temp_idx(idx)) {
            name = "\xe4\xb8\xb4\xe6\x97\xb6\xe7\x9b\xae\xe5\xbd\x95";  /* 临时目录 */
        } else {
            int fi = idx - 2;
            if (fi < 0 || fi >= g_book_folder_count) continue;
            name = g_book_folder_names[fi];
        }
        char trunc[64];
        truncate_name(trunc, sizeof(trunc), name, 6);
        int tw = text_width(trunc);
        int tx = x0 + ((x1 - x0) - tw) / 2;
        draw_text(lcd, tx, yy + 8, trunc, false);
        /* V1.0.68: 统一选中样式: 下方 2px 横线 (与游戏列表一致) */
        if (is_sel) {
            draw_hline(lcd, x0, x1, yy + SELECT_ITEM_H - 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, x0, x1, yy + SELECT_ITEM_H, ST7305_COLOR_BLACK);
        }
    }
}

static const char *skip_zh_space(const char *s);   /* V1.0.68: 前向声明 (定义在下方, 供 draw_book_game_pane 使用) */

static void draw_book_game_pane(st7305_handle_t *lcd, menu_state_t *state) {
    int x0 = SELECT_RIGHT_X;
    int x1 = SCREEN_W - 4;
    int y  = SELECT_LIST_Y;
    int total = g_sub_count;
    int sel = state->select_game_idx;
    if (sel < 0) sel = 0;
    if (sel >= total && total > 0) sel = total - 1;
    int center_y = (SELECT_LIST_Y + SELECT_LIST_BOTTOM) / 2;

    if (total == 0) {
        const char *line1;
        const char *line2 = NULL;
        if (state->select_folder_idx == 1) {
            line1 = "\xe6\x9a\x82\xe6\x97\xa0\xe6\x94\xb6\xe8\x97\x8f";        /* 暂无收藏 */
            line2 = "\xe5\x9c\xa8\xe4\xb9\xa6\xe7\xb1\x8d\xe5\x90\x8d\xe7\xa7\xb0\xe6\x8c\x89\xe5\xa4\x9a\xe5\x8a\x9f\xe8\x83\xbd\xe9\x94\xae\xe6\x94\xb6\xe8\x97\x8f"; /* 在书籍名称按多功能键收藏 */
        } else if (state->select_folder_idx == 0) {
            line1 = "\xe7\x82\xb9\xe7\xa1\xae\xe8\xae\xa4\xe5\xbe\xaa\xe7\x8e\xaf\xe5\x88\x87\xe6\x8d\xa2";  /* 点确认循环切换 */
        } else {
            line1 = "\xe6\x9a\x82\xe6\x97\xa0\xe4\xb9\xa6\xe7\xb1\x8d";        /* 暂无书籍 */
            line2 = "\xe5\xb0\x86 txt \xe6\x94\xbe\xe5\x85\xa5\xe6\xad\xa4\xe7\x9b\xae\xe5\xbd\x95";  /* 将 txt 放入此目录 */
        }
        char tmp[64];
        truncate_name(tmp, sizeof(tmp), line1, 14);
        int tw = text_width(tmp);
        draw_text(lcd, x0 + ((x1 - x0) - tw) / 2, center_y - 16, tmp, false);
        if (line2) {
            truncate_name(tmp, sizeof(tmp), line2, 18);
            tw = text_width(tmp);
            draw_text(lcd, x0 + ((x1 - x0) - tw) / 2, center_y + 12, tmp, false);
        }
        return;
    }

    int max_vis = (SELECT_LIST_BOTTOM - y) / SELECT_ITEM_H;
    if (max_vis < 1) max_vis = 1;
    if (sel < state->select_game_scroll) state->select_game_scroll = sel;
    if (sel >= state->select_game_scroll + max_vis)
        state->select_game_scroll = sel - max_vis + 1;
    int scroll = state->select_game_scroll;
    bool focus_right = (state->select_focus == 1);

    for (int i = 0; i < max_vis && (scroll + i) < total; i++) {
        int idx = scroll + i;
        int yy = y + i * SELECT_ITEM_H;
        bool is_sel = (idx == sel);
        /* V1.0.68: 内容页选中 = 整行加黑 (原样式) */
        if (is_sel && focus_right) {
            fill_rect(lcd, x0, yy, x1, yy + SELECT_ITEM_H - 2, ST7305_COLOR_BLACK);
        }
        const char *text = g_sub_items[idx];
        if (!text || text[0] == '\0') text = " ";
        if (state->select_folder_idx == 0) {
            /* 设置项: "标签|值" → 标签左对齐, 值右对齐 */
            const char *pipe = strchr(text, '|');
            if (pipe) {
                char label[24];
                int ln = (int)(pipe - text);
                if (ln > 23) ln = 23;
                memcpy(label, text, (size_t)ln);
                label[ln] = '\0';
                draw_text(lcd, x0 + 8 + 48, yy + 8, label, is_sel && focus_right);
                const char *val = pipe + 1;
                int vw = text_width(val);
                int vx = x1 - 4 - vw;
                if (vx < x0 + 8 + 48) vx = x0 + 8 + 48;
                draw_text(lcd, vx, yy + 8, val, is_sel && focus_right);
                continue;
            }
        }
        char trunc[64];
        truncate_name(trunc, sizeof(trunc), text, SELECT_TRUNC_CHARS);
        int tw = text_width(trunc);
        int tx;
        if (state->select_folder_idx == 0) {
            tx = x0 + 8 + 48;   /* 设置项: 左对齐 + 缩进 (冒号对齐) */
        } else {
            tx = x0 + ((x1 - x0) - tw) / 2;
        }
        if (tx < x0) tx = x0;
        draw_text(lcd, tx, yy + 8, trunc, is_sel && focus_right);
    }
}

static void render_book_two_cols_portrait(menu_state_t *state);   /* 前向声明 (定义在下方) */
static void draw_book_game_pane(st7305_handle_t *lcd, menu_state_t *state);

static void render_book_two_cols(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    if (!state->book_loaded) book_scan_folders(state);
    g_sub_count = book_build_right(state, g_sub_items, sizeof(g_sub_items) / sizeof(g_sub_items[0]));
    /* 旋转 左/右: 竖屏布局 + 软件旋转 */
    if (state->book_rot == 2 || state->book_rot == 3) {
        render_book_two_cols_portrait(state);
        return;
    }
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    menu_draw_status_bar(lcd, &state->settings, "\xe7\x94\xb5\xe5\xad\x90\xe4\xb9\xa6");  /* 电子书 */
    draw_book_folder_pane(lcd, state);
    draw_book_game_pane(lcd, state);
    /* 分隔竖线: 相对原始位置左移 1px (用户微调) */
    draw_vline(lcd, SELECT_LEFT_W + SELECT_GAP / 2 - 1, SELECT_LIST_Y, SELECT_LIST_BOTTOM, ST7305_COLOR_BLACK);
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (state->hint_text[0] != '\0' && now_ms < state->hint_until_ms) {
        draw_notice_popup(state->lcd, state->hint_text);
    } else if (state->hint_text[0] != '\0' && now_ms >= state->hint_until_ms) {
        state->hint_text[0] = '\0';
    }
}

/* === 电子书二级菜单: 竖屏布局 (旋转 左/右) ===
 * 左栏 92px (3 字) + 右栏 188px (7 字), 画进逻辑缓冲, 软件旋转映射回横屏帧缓冲 */
#define MPFB_W 300
#define MPFB_H 400
#define MPFB_ROW_BYTES 40
EXT_RAM_BSS_ATTR static uint8_t s_book_menu_pfb[MPFB_ROW_BYTES * MPFB_H];

static inline void mpfb_px(uint8_t *fb, int x, int y, int black) {
    if (x < 0 || x >= MPFB_W || y < 0 || y >= MPFB_H) return;
    uint8_t *p = &fb[(size_t)y * MPFB_ROW_BYTES + (size_t)(x >> 3)];
    if (black) *p |= (uint8_t)(1u << (7 - (x & 7)));
    else       *p &= (uint8_t)~(1u << (7 - (x & 7)));
}

static inline void mpfb_rect(uint8_t *fb, int x0, int y0, int x1, int y1, int black) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            mpfb_px(fb, x, y, black);
}

/* 画 24x24 中文字 (font_zh), 返回像素宽; ASCII 跳过 */
static int mpfb_draw_zh24(uint8_t *fb, int x, int y, const char *str, bool inv) {
    const uint8_t *p = (const uint8_t *)str;
    int cx = x;
    while (*p) {
        int idx = font_zh_find_utf8((const char *)p);
        if (idx < 0) { p++; cx += 24; continue; }
        const uint8_t *bmp = font_zh_get_bitmap_by_index(idx);
        if (bmp) {
            for (int row = 0; row < 24; row++) {
                const uint8_t *src = bmp + row * 3;
                for (int col = 0; col < 24; col++) {
                    int bit = (int)((src[col >> 3] >> (7 - (col & 7))) & 1u);
                    mpfb_px(fb, cx + col, y + row, inv ? !bit : bit);
                }
            }
        }
        cx += 24;
        p += 3;
    }
    return cx - x;
}

/* 画单个 ASCII 字符 8x12 (竖屏菜单), 返回像素宽 */
static int mpfb_draw_ascii8(uint8_t *fb, int x, int y, uint8_t c, bool inv) {
    int idx = (c >= 0x20 && c <= 0x7E) ? (c - 0x20) : ('?' - 0x20);
    const uint8_t *bmp = FONT8X12[idx];
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            bool bit = ((bits >> (7 - col)) & 1u) != 0;
            mpfb_px(fb, x + col, y + row, inv ? !bit : bit);
        }
    }
    return 8;
}

/* 混合文本 (中文 24px + ASCII 8x12), 返回像素宽 (竖屏菜单) */
static int mpfb_draw_text(uint8_t *fb, int x, int y, const char *str, bool inv) {
    int cx = x;
    const uint8_t *p = (const uint8_t *)str;
    while (*p) {
        if (*p < 0x80) {
            cx += mpfb_draw_ascii8(fb, cx, y + 6, *p, inv);
            p++;
        } else if ((*p & 0xF0) == 0xE0) {
            int idx = font_zh_find_utf8((const char *)p);
            if (idx >= 0) {
                const uint8_t *bmp = font_zh_get_bitmap_by_index(idx);
                for (int row = 0; row < 24; row++) {
                    const uint8_t *src = bmp + row * 3;
                    for (int col = 0; col < 24; col++) {
                        int bit = (int)((src[col >> 3] >> (7 - (col & 7))) & 1u);
                        mpfb_px(fb, cx + col, y + row, inv ? !bit : bit);
                    }
                }
            }
            cx += 24;
            p += 3;
        } else {
            p++;
        }
    }
    return cx - x;
}

static int mpfb_text_width(const char *str) {
    int w = 0;
    const uint8_t *p = (const uint8_t *)str;
    while (*p) {
        if (*p < 0x80) { w += 8; p++; }
        else if ((*p & 0xF0) == 0xE0) { w += 24; p += 3; }
        else p++;
    }
    return w;
}

static inline void menu_fb_set_px(uint8_t *fb, int x, int y, int black) {
    int inv_y = ST7305_HEIGHT - 1 - y;
    uint32_t idx = (uint32_t)(x >> 1) * (ST7305_HEIGHT >> 2) + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (x & 1));
    if (black) fb[idx] &= (uint8_t)~(1u << bit);
    else       fb[idx] |= (uint8_t)(1u << bit);
}

/* 复制前 n 个 UTF-8 字符 (无省略号, 竖屏窄列用) */
static int zh_copy_n(char *out, size_t out_sz, const char *src, int n) {
    int cnt = 0, o = 0, i = 0;
    while (src[i] && cnt < n && o < (int)out_sz - 1) {
        uint8_t c = (uint8_t)src[i];
        int step = 1;
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (o + step < (int)out_sz) {
            memcpy(out + o, src + i, (size_t)step);
            o += step;
        }
        i += step;
        cnt++;
    }
    out[o] = '\0';
    return o;
}

/* 跳过开头全角空格 (U+3000, 设置项对齐占位) */
static const char *skip_zh_space(const char *s) {
    while ((uint8_t)s[0] == 0xE3 && (uint8_t)s[1] == 0x80 && (uint8_t)s[2] == 0x80) s += 3;
    return s;
}

static void render_book_two_cols_portrait(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    uint8_t *fb = s_book_menu_pfb;
    memset(fb, 0, MPFB_ROW_BYTES * MPFB_H);   /* 白底 */

    /* 右转时: 侧栏 4 字宽(96px)在分隔线左侧, 内容列加宽在右侧 */
    bool mirror = (state->book_rot == 3);
    int side_x0 = mirror ? 8 : 4;
    int side_x1 = mirror ? 104 : 100;
    int div_x   = mirror ? 108 : 102;
    int right_x = mirror ? 112 : 108;
    int right_w = 184;
    int right_x1 = right_x + right_w;
    int y0 = 4, row_h = 30;
    int max_vis = (MPFB_H - y0 - 6) / row_h;
    if (max_vis < 1) max_vis = 1;
    int total = book_sidebar_total();
    int sel = state->select_folder_idx;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;
    if (sel < state->select_folder_scroll) state->select_folder_scroll = sel;
    if (sel >= state->select_folder_scroll + max_vis)
        state->select_folder_scroll = sel - max_vis + 1;
    int scroll = state->select_folder_scroll;
    for (int i = 0; i < max_vis && (scroll + i) < total; i++) {
        int idx = scroll + i;
        int yy = y0 + i * row_h;
        bool is_sel = (idx == sel);
        const char *name;
        if (idx == 0) {
            name = "\xe8\xae\xbe\xe7\xbd\xae";  /* 设置 */
        } else if (idx == 1) {
            name = "\xe6\x94\xb6\xe8\x97\x8f\xe4\xb9\xa6\xe6\x9e\xb6";  /* 收藏书架 */
        } else if (book_is_temp_idx(idx)) {
            name = "\xe4\xb8\xb4\xe6\x97\xb6\xe7\x9b\xae\xe5\xbd\x95";  /* 临时目录 */
        } else {
            int fi = idx - 2;
            if (fi < 0 || fi >= g_book_folder_count) continue;
            name = g_book_folder_names[fi];
        }
        char trunc[64];
        zh_copy_n(trunc, sizeof(trunc), name, 4);
        int tw = 24 * book_utf8_count(trunc);
        int tx = side_x0 + (side_x1 - side_x0 - tw) / 2;
        if (tx < side_x0) tx = side_x0;
        mpfb_draw_zh24(fb, tx, yy + 3, trunc, false);
        /* V1.0.68: 统一选中样式: 下方 2px 横线 (与游戏列表一致) */
        if (is_sel) {
            for (int xx = side_x0; xx <= side_x1; xx++) {
                mpfb_px(fb, xx, yy + row_h - 1, 1);
                mpfb_px(fb, xx, yy + row_h,     1);
            }
        }
    }
    mpfb_rect(fb, div_x, 4, div_x, MPFB_H - 4, 1);   /* 中缝 */

    /* 右栏 */
    int gtotal = g_sub_count;
    int gsel = state->select_game_idx;
    if (gsel < 0) gsel = 0;
    if (gsel >= gtotal && gtotal > 0) gsel = gtotal - 1;
    if (gsel < state->select_game_scroll) state->select_game_scroll = gsel;
    if (gsel >= state->select_game_scroll + max_vis)
        state->select_game_scroll = gsel - max_vis + 1;
    int gscroll = state->select_game_scroll;
    bool focus_right = (state->select_focus == 1);
    if (gtotal == 0) {
        const char *hint = "\xe6\x9a\x82\xe6\x97\xa0\xe5\x86\x85\xe5\xae\xb9";  /* 暂无内容 */
        char tmp[64];
        zh_copy_n(tmp, sizeof(tmp), hint, right_w / 24);
        int tw = 24 * book_utf8_count(tmp);
        mpfb_draw_zh24(fb, right_x + (right_w - tw) / 2, (MPFB_H - 24) / 2, tmp, false);
    } else {
        for (int i = 0; i < max_vis && (gscroll + i) < gtotal; i++) {
            int idx = gscroll + i;
            int yy = y0 + i * row_h;
            bool is_sel = (idx == gsel);
            /* V1.0.68: 内容页选中 = 整行加黑 (原样式); 侧栏才是横线 */
            if (is_sel && focus_right) {
                mpfb_rect(fb, right_x, yy, right_x1, yy + row_h - 2, 1);
            }
            const char *text = g_sub_items[idx];
            if (!text || text[0] == '\0') text = " ";
            if (state->select_folder_idx == 0) {
                /* 设置项: "标签|值" → 标签靠左, 值靠右 */
                const char *pipe = strchr(text, '|');
                if (pipe) {
                    char label[24];
                    int ln = (int)(pipe - text);
                    if (ln > 23) ln = 23;
                    memcpy(label, text, (size_t)ln);
                    label[ln] = '\0';
                    mpfb_draw_text(fb, right_x + 2, yy + 3, label, is_sel && focus_right);
                    const char *val = pipe + 1;
                    int vw = mpfb_text_width(val);
                    int vx = right_x1 - 2 - vw;
                    if (vx < right_x + 2) vx = right_x + 2;
                    mpfb_draw_text(fb, vx, yy + 3, val, is_sel && focus_right);
                    continue;
                }
            }
            text = skip_zh_space(text);   /* 去掉设置项对齐空格, 靠左显示 */
            char trunc[64];
            zh_copy_n(trunc, sizeof(trunc), text, right_w / 24);
            mpfb_draw_text(fb, right_x + 2, yy + 3, trunc, is_sel && focus_right);
        }
    }

    /* 逻辑竖屏 -> 横屏帧缓冲 (软件旋转) */
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    uint8_t *lfb = lcd->fb;
    for (int Y = 0; Y < MPFB_H; Y++) {
        for (int X = 0; X < MPFB_W; X++) {
            int black = (int)((fb[(size_t)Y * MPFB_ROW_BYTES + (size_t)(X >> 3)] >>
                              (7 - (X & 7))) & 1u);
            int fx, fy;
            if (state->book_rot == 2) {
                fx = ST7305_WIDTH - 1 - Y;    /* 左: 屏幕左转(逆时针)看 */
                fy = X;
            } else {
                fx = Y;                       /* 右: 屏幕右转(顺时针)看 */
                fy = ST7305_HEIGHT - 1 - X;   /* 修复: fy 用高度 300, 之前误用宽度导致越界 */
            }
            menu_fb_set_px(lfb, fx, fy, black);
        }
    }
}

static const sub_page_def_t sub_pages[MENU_PAGE_COUNT];  /* 前向声明 */
static void render_main(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int sel = state->selected_index;
    int total = menu_main_count(state);

    /* === 状态栏 (复用函数) === */
    menu_draw_status_bar(lcd, &state->settings, NULL);

    /* === 主体区域: 大图标横向排列 === */
    int icon_center_y = SCREEN_H / 2;
    int icon_spacing = 100;
    int center_x = SCREEN_W / 2;

    /* === 切换动画 === */
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t anim_duration = 240;
    float t = 1.0f;
    if (state->anim_start_ms > 0 && (now_ms - state->anim_start_ms) < anim_duration) {
        t = (float)(now_ms - state->anim_start_ms) / (float)anim_duration;
    } else if (state->anim_start_ms > 0) {
        state->anim_start_ms = 0;
        state->anim_direction = 0;
        state->prev_selected = sel;
        state->main_drag_offset = 0;   /* 动画结束, 拖动偏移归零 */
    }
    float ease = (t < 0.5f) ? 4.0f * t * t * t : 1.0f - (float)pow(-2.0f * t + 2.0f, 3) / 2.0f;

    int dir = state->anim_direction;
    int prev = state->prev_selected;

    /* 计算每个图标的位置和尺寸 (循环滚动: 取到选中项的最短距离) */
    for (int i = 0; i < total; i++) {
        /* 计算目标偏移 (考虑循环, 取最短路径) */
        int diff_target = i - sel;
        if (diff_target > total / 2) diff_target -= total;
        else if (diff_target < -total / 2) diff_target += total;
        int target_offset = diff_target * icon_spacing;

        /* 计算起始偏移 (动画开始时的位置, 也考虑循环) */
        int diff_start = diff_target;
        if (state->anim_start_ms > 0 && dir != 0) {
            diff_start = i - prev;
            if (diff_start > total / 2) diff_start -= total;
            else if (diff_start < -total / 2) diff_start += total;
            /* V1.0.68: 目标相对起点取最短环绕路径. 原来起点/目标各自独立环绕,
             * 环绕切换时图标会横穿整个屏幕经过中央 (看起来像一堆小图标弹出/重叠),
             * 改为只走最近距离, 松手吸附更平滑. */
            if (diff_target - diff_start > total / 2) diff_target -= total;
            else if (diff_target - diff_start < -total / 2) diff_target += total;
            target_offset = diff_target * icon_spacing;
        }
        int start_offset = diff_start * icon_spacing;

        /* 插值位置 */
        int cx = start_offset + (int)((target_offset - start_offset) * ease);
        cx += center_x;
        /* V1.0.66: 拖动中跟手平移; 松手吸附动画期间把残留偏移衰减到 0 (无缝过渡) */
        if (state->main_drag_active) {
            cx += state->main_drag_offset;
        } else if (state->anim_start_ms > 0) {
            cx += (int)(state->main_drag_offset * (1.0f - ease));
        }

        /* 尺寸: 中央最大, 两边逐渐变小. V1.0.67: 动画期间按起止 distance 插值,
         * 避免松手瞬间按新选中项直接算 size 导致图标突然变小(闪过小图标). */
        int d_start = abs(diff_start);
        int d_target = abs(diff_target);
        int size_start = (d_start == 0) ? 100 : (d_start == 1) ? 70 : (d_start == 2) ? 50 : 40;
        int size_target = (d_target == 0) ? 100 : (d_target == 1) ? 70 : (d_target == 2) ? 50 : 40;
        int size = size_start + (int)((size_target - size_start) * ease);

        /* 只绘制屏幕内的图标 */
        if (cx >= -size && cx <= SCREEN_W + size) {
            draw_icon_bitmap(lcd, cx, icon_center_y, size, main_items[menu_main_phys(state, i)].icon_idx);
        }
    }

    /* 文字标签: 选中图标下方 (V1.0.68: 下移 10px) */
    int label_y = icon_center_y + 68;
    if (label_y > SCREEN_H - 28) label_y = SCREEN_H - 28;
    draw_label_centered_at(lcd, center_x, label_y, main_items[menu_main_phys(state, sel)].short_label, false);

    /* 后台存档状态: 仅在状态栏左上角显示 SD 卡图标, 此处不再画文字提示 */

    /* 刷新由 menu_render 末尾统一处理, 避免弹窗叠加时的双 flush 闪烁 */
}

/* === V1.0.32 紧凑提示弹窗模板 (作为所有短提示的统一规格) ===
 * 用户需求: 弹窗边框离字 3 像素 (字四边 3px 内边距), 让出屏幕大部分空间,
 * 不打扰用户。所有"短提示/通知"类弹窗 (连接中/连接成功/已保存/失败等)
 * 都按此规格。
 *
 * 规格 (3px 内边距 + 3px 边框):
 *   - 文字 24px 高, 上下左右各 3px 内边距
 *   - 3px 边框
 *   - 总高 = 3 + 3 + 24 + 3 + 3 = 36px
 *   - 总宽 = 3 + 3 + 文本宽 + 3 + 3 (按最长字串测量)
 *
 * 用宏: NOTICE_POPUP_W(text_len) / NOTICE_POPUP_H 计算尺寸.
 * - text_len: 字符数 (中英文都按 1 个字符计, 12px 字宽估算时按 12x12 单元).
 *
 * 居中绘制: x = (SCREEN_W - W) / 2, y = (SCREEN_H - H) / 2. */
#define NOTICE_TEXT_W  12   /* 24px 高字体一个字符占 12 像素宽 (12x24, 已含字间距) */
#define NOTICE_PAD     3    /* 文字到边框 3 像素 */
#define NOTICE_BORDER  3    /* 边框 3 像素 */
#define NOTICE_TEXT_H  24   /* 24px 高文字 */
/* 根据文本字符数计算弹窗宽: 边框3 + 内边距3 + 字符*N + 内边距3 + 边框3 = 12 + N*12 */
#define NOTICE_POPUP_W(text_len)  ((NOTICE_BORDER * 2) + (NOTICE_PAD * 2) + (text_len) * NOTICE_TEXT_W)
/* 弹窗高固定: 边框3 + 内边距3 + 文字24 + 内边距3 + 边框3 = 36 */
#define NOTICE_POPUP_H  ((NOTICE_BORDER * 2) + (NOTICE_PAD * 2) + NOTICE_TEXT_H)

/* 通用绘制函数: 在屏幕居中画紧凑提示弹窗, 显示单行文本 (UTF-8).
 * V1.0.41: 弹窗宽度用 text_width() 精确计算 (中文 24px, ASCII 16px),
 *          替代旧的字符数×12 估算 (旧法导致弹窗左右太窄, 文字超出边框). */
static void draw_notice_popup(st7305_handle_t *lcd, const char *text) {
    if (!text || !text[0]) return;
    int tw = text_width(text);
    const int W = (NOTICE_BORDER * 2) + (NOTICE_PAD * 2) + tw;
    const int H = NOTICE_POPUP_H;
    int x = (SCREEN_W - W) / 2;
    int y = (SCREEN_H - H) / 2;
    /* 白底 */
    for (int dy = 0; dy < H; dy++) {
        for (int dx = 0; dx < W; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
        }
    }
    /* 3px 黑边框 */
    for (int k = 0; k < NOTICE_BORDER; k++) {
        for (int dx = 0; dx < W; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + dx, y + H - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < H; dy++) {
            st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + W - 1 - k, y + dy, ST7305_COLOR_BLACK);
        }
    }
    /* 文字: 居中. y 起点 = 边框3 + 内边距3 = y + 6, 文字高 24, 到底部内边距 3+边框 3 = 6, 总 36, 居中正确. */
    draw_text_centered(lcd, y + NOTICE_BORDER + NOTICE_PAD, text, false);
}

/* 公开封装: 供其它组件(如 gam4980)复用同一款提示小弹窗, 保证外观与收藏提示一致. */
void menu_draw_notice_popup(st7305_handle_t *lcd, const char *text) {
    draw_notice_popup(lcd, text);
}

/* V1.0.68: 软关机流程 — 显示"正在关机" → 清空屏幕 → 进入 deep sleep.
 * 反射式 LCD 掉电后会保持最后一帧画面, 所以关机前必须清屏, 否则黑屏/花屏残留.
 * esp_deep_sleep_start() 不会返回; 之后按下电源键(GPIO1)唤醒复位开机. */
void menu_soft_power_off(st7305_handle_t *lcd) {
    if (!lcd) return;
    extern menu_state_t g_menu;
    ESP_LOGI(TAG, "软关机: 显示正在关机 -> 清屏 -> deep sleep");
    /* V1.0.68: 方波直驱方案下播放关机下行音 */
    if (g_menu.settings.audio_scheme == 1 && tone_player_ready()) {
        tone_play_effect(TONE_EFFECT_SHUTDOWN);
    }
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    menu_draw_notice_popup(lcd, "\xe6\xad\xa3\xe5\x9c\xa8\xe5\x85\xb3\xe6\x9c\xba"); /* 正在关机 */
    st7305_flush(lcd);
    vTaskDelay(pdMS_TO_TICKS(900));
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    st7305_flush(lcd);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

/* 多行紧凑提示弹窗 (彩蛋用): 与 draw_notice_popup 同规格 (3px 边框 + 3px 内边距),
 * 但支持 \n 硬换行, 每行 24px 高, 用于"已解锁游戏菜单\n测试功能，可能闪退"这类两行提示. */
static void draw_notice_popup_multiline(st7305_handle_t *lcd, const char *text) {
    if (!text || !text[0]) return;
    /* 计算行数 + 每行宽度 */
    int lines = 1;
    int max_tw = 0;
    const char *p = text;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int tw = text_width_bounded(line_start, p);
        if (tw > max_tw) max_tw = tw;
        if (*p == '\n') { lines++; p++; }
    }
    const int W = (NOTICE_BORDER * 2) + (NOTICE_PAD * 2) + max_tw;
    const int H = (NOTICE_BORDER * 2) + (NOTICE_PAD * 2) + (lines * NOTICE_TEXT_H);
    int x = (SCREEN_W - W) / 2;
    int y = (SCREEN_H - H) / 2;
    /* 白底 */
    for (int dy = 0; dy < H; dy++) {
        for (int dx = 0; dx < W; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
        }
    }
    /* 3px 黑边框 */
    for (int k = 0; k < NOTICE_BORDER; k++) {
        for (int dx = 0; dx < W; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + dx, y + H - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < H; dy++) {
            st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + W - 1 - k, y + dy, ST7305_COLOR_BLACK);
        }
    }
    /* 逐行绘制, 垂直居中 */
    int text_y = y + NOTICE_BORDER + NOTICE_PAD;
    const char *lp = text;
    for (int i = 0; i < lines; i++) {
        const char *ls = lp;
        while (*lp && *lp != '\n') lp++;
        int tw = text_width_bounded(ls, lp);
        int lx = (SCREEN_W - tw) / 2;
        if (lx < 0) lx = 0;
        draw_text(lcd, lx, text_y, ls, false);
        text_y += NOTICE_TEXT_H;
        if (*lp == '\n') lp++;
    }
}

/* 绘制连接小弹窗 (居中对齐, 用户需求: 添加设备连接时显示"正在连接"/
 * "连接成功", 弹窗自动结束, 无其他字)。
 * V1.0.32: 改为 3px 内边距紧凑提示弹窗模板 (draw_notice_popup), 与其他短提示统一. */
static void draw_connecting_popup(menu_state_t *state) {
    if (!state->connecting_popup_active) return;
    const char *txt = state->connecting_popup_success
                          ? "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f"   /* 连接成功 */
                          : "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5";   /* 正在连接 */
    draw_notice_popup(state->lcd, txt);
}

/* 绘制确认弹窗 (覆盖在内容之上) */
static void draw_confirm_dialog(menu_state_t *state) {
    if (!state->confirm_active) return;
    st7305_handle_t *lcd = state->lcd;
    /* 统一窗口模板: 离屏幕上下左右各 25 像素 (与 list_dialog 一致) */
    int w = SCREEN_W - 50;
    int h = SCREEN_H - 50;
    int x = 25, y = 25;
    /* 先清弹窗区域 (白色背景) */
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
        }
    }
    /* 3px 粗黑边框 */
    for (int k = 0; k < 3; k++) {
        for (int dx = 0; dx < w; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + dx, y + h - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < h; dy++) {
            st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + w - 1 - k, y + dy, ST7305_COLOR_BLACK);
        }
    }
    /* 标题 */
    draw_text_centered(lcd, y + 14, state->confirm_title, false);
    /* 分割线 */
    draw_hline(lcd, x + 10, x + w - 10, y + 38, ST7305_COLOR_BLACK);
    /* 消息: 自动换行 (居中), 避免超出弹窗边缘. 区域: 分割线以下到窗口底部. */
    int msg_x0 = x + 12;
    int msg_x1 = x + w - 12;
    int msg_y0 = y + 50;
    draw_text_wrapped(lcd, msg_x0, msg_x1, msg_y0,
                      state->confirm_msg, false, 1, 24);
}

/* 列表选择弹窗 - 内部几何计算, 局部刷新复用
 * 用户需求: 弹窗固定尺寸, 边框离屏幕上下左右各 25 像素 (真正固定, 不随内容自适应).
 * 宽度 = SCREEN_W - 50 = 350
 * 高度 = SCREEN_H - 50 = 250
 * 列表行在弹窗内上下居中显示. */
typedef struct {
    int w, h, x, y;          /* 弹窗外框 */
    int line_h;              /* 行高 (紧凑) */
    int content_y0;          /* 内容区(可滚动)顶部 y */
    int content_h;           /* 内容区高度 */
    int content_visible;     /* 内容区可见行数 */
    int content_count;       /* 内容项数 (不含底部"返回") */
    int footer_y;            /* 底部固定"返回"行顶部 y (-1 = 无返回行) */
    bool has_footer;         /* 是否把最后一项当作固定"返回"行 */
} list_dialog_geom_t;

#define LIST_DIALOG_MARGIN   25                                  /* 弹窗边框离屏幕边距 (上下左右都此值) */
#define LIST_DIALOG_LINE_H   40                                  /* 列表行高 (紧凑, 比原 48 更紧; 单行 24px 字居中, 2 行换行也能容纳) */

static void list_dialog_calc_geom(const menu_state_t *state, list_dialog_geom_t *g) {
    /* 弹窗外框: 真正固定尺寸, 上下左右都是 25 像素边距 */
    g->w = SCREEN_W - LIST_DIALOG_MARGIN * 2;
    g->h = SCREEN_H - LIST_DIALOG_MARGIN * 2;
    g->x = LIST_DIALOG_MARGIN;
    g->y = LIST_DIALOG_MARGIN;
    g->line_h = LIST_DIALOG_LINE_H;

    int count = state->list_dialog_count;
    int inner_pad = 4;   /* 内边距 (边框内侧留白) */

    /* 自定义渲染(如时间弹窗单行多字段): 整窗给回调, 无返回固定行、无滚动条 */
    if (state->list_dialog_on_render) {
        g->has_footer = false;
        g->content_count = count;
        g->content_y0 = g->y + inner_pad;
        g->content_h = g->h - inner_pad * 2;
        g->content_visible = g->content_h / g->line_h;
        if (g->content_visible < 1) g->content_visible = 1;
        g->footer_y = -1;
        return;
    }

    /* 文本列表: 最后一项视为"返回", 固定在弹窗最下方;
     * 其余项在内容区垂直居中 (在"上边框下方"和"返回分隔线"之间居中).
     * V1.0.41: 返回行强制在最底部, 上方加分隔线; 内容区在分隔线上方垂直居中. */
    g->has_footer = (count > 0);
    g->content_count = (count > 1) ? (count - 1) : 0;   /* 不含返回 */
    int footer_h = (g->has_footer && count > 1) ? g->line_h : 0;
    if (count == 1) footer_h = g->line_h;               /* 仅 1 项(就是返回)时也作为固定行 */

    /* 返回行固定在弹窗底部 (紧贴下边框 inner_pad) */
    g->footer_y = (footer_h > 0) ? (g->y + g->h - inner_pad - footer_h) : -1;

    /* 内容区: 上边框下方 inner_pad 到返回分隔线上方, 内容项垂直居中 */
    int sep_h = (count > 1) ? 2 : 0;  /* 返回上方分隔线 */
    int content_area_top = g->y + inner_pad;
    int content_area_bottom = (g->footer_y >= 0) ? (g->footer_y - sep_h) : (g->y + g->h - inner_pad);
    int content_area_h = content_area_bottom - content_area_top;

    int total_content_h = g->content_count * g->line_h;
    int top_pad = (content_area_h - total_content_h) / 2;
    if (top_pad < 0) top_pad = 0;

    g->content_y0 = content_area_top + top_pad;
    g->content_h = content_area_h;
    g->content_visible = (content_area_h) / g->line_h;
    if (g->content_visible < 1) g->content_visible = 1;
}

/* 计算某 item 的绝对 y (返回行用 footer_y; 内容行用 content_y0 + vis*line_h) */
static int list_dialog_item_y(const list_dialog_geom_t *g, int item_idx, int scroll) {
    if (item_idx == g->content_count && g->footer_y >= 0) return g->footer_y; /* 返回行 */
    int vis = item_idx - scroll;
    return g->content_y0 + vis * g->line_h;
}

/* 在弹窗中绘制一行 (含背景 + 居中文本).
 * abs_y: 该行的绝对 y (由调用方用 list_dialog_item_y 计算).
 * selected=true: 黑底白字; selected=false: 白底黑字 */
static void list_dialog_draw_row(st7305_handle_t *lcd, const list_dialog_geom_t *g,
                                  int abs_y, const char *text, bool selected) {
    int inner_x0 = g->x + 6;
    int inner_x1 = g->x + g->w - 6;
    int top = abs_y + (g->line_h - 24) / 2;   /* 单行 24px 字垂直居中 */
    if (selected) {
        fill_rect(lcd, inner_x0, abs_y, inner_x1 - 1, abs_y + g->line_h - 2, ST7305_COLOR_BLACK);
        if (text_width(text) <= (inner_x1 - inner_x0)) {
            draw_text_centered(lcd, top, text, true);  /* 单行 */
        } else {
            draw_text_wrapped(lcd, inner_x0, inner_x1, abs_y + (g->line_h - 36) / 2,
                              text, true, 1, 18);  /* 自动换行(2 行, 行高 18) */
        }
    } else {
        /* 强制白底, 避免上次选中残留导致底色污染 */
        fill_rect(lcd, inner_x0, abs_y, inner_x1 - 1, abs_y + g->line_h - 2, ST7305_COLOR_WHITE);
        if (text_width(text) <= (inner_x1 - inner_x0)) {
            draw_text_centered(lcd, top, text, false);
        } else {
            draw_text_wrapped(lcd, inner_x0, inner_x1, abs_y + (g->line_h - 36) / 2,
                              text, false, 1, 18);
        }
    }
}

/* 完整绘制弹窗: 外框 + 列表 (无标题) */
static void list_dialog_draw_full(menu_state_t *state, const list_dialog_geom_t *g) {
    st7305_handle_t *lcd = state->lcd;
    int count = state->list_dialog_count;

    /* 弹窗整体白底 (覆盖原底层页面) */
    for (int dy = 0; dy < g->h; dy++) {
        for (int dx = 0; dx < g->w; dx++) {
            st7305_draw_pixel(lcd, g->x + dx, g->y + dy, ST7305_COLOR_WHITE);
        }
    }
    /* 2px 粗黑边框 (弹窗四边) */
    for (int k = 0; k < 2; k++) {
        for (int dx = 0; dx < g->w; dx++) {
            st7305_draw_pixel(lcd, g->x + dx, g->y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, g->x + dx, g->y + g->h - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < g->h; dy++) {
            st7305_draw_pixel(lcd, g->x + k, g->y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, g->x + g->w - 1 - k, g->y + dy, ST7305_COLOR_BLACK);
        }
    }
    /* 注: 用户需求 - 弹窗不显示标题, 因此不画 title 文字和分隔线 */

    /* 自定义渲染回调 (用于单行多字段横向显示, 如时间弹窗): 整窗给回调 */
    if (state->list_dialog_on_render) {
        int cx = g->x + 6;
        int cy = g->content_y0;
        int cw = g->w - 12;
        int ch = g->content_h;
        state->list_dialog_on_render(state, lcd, cx, cy, cw, ch);
        return;
    }

    /* 内容区: 绘制可见的内容项 (不含底部"返回").
     * V1.0.68: 拖动时叠加偏移并多渲染边缘几行, 列表跟随手指. */
    int drag_off = state->list_dialog_drag_offset;
    int extra = 0;
    if (drag_off < 0) extra = (-drag_off + g->line_h - 1) / g->line_h + 1;
    if (drag_off > 0) extra = (drag_off + g->line_h - 1) / g->line_h + 1;
    int area_y1 = g->content_y0 + g->content_visible * g->line_h;
    for (int i = -extra; i < g->content_visible + extra; i++) {
        int item_idx = i + state->list_dialog_scroll;
        if (item_idx < 0 || item_idx >= g->content_count) continue;
        int yy = g->content_y0 + i * g->line_h + drag_off;
        if (yy + g->line_h <= g->content_y0 || yy >= area_y1) continue;
        bool is_selected = (item_idx == state->list_dialog_selected);
        list_dialog_draw_row(lcd, g, yy, state->list_dialog_items[item_idx], is_selected);
    }

    /* 底部固定"返回"行 (最后一项, 强制在弹窗最底部) + 上方分隔线.
     * V1.0.41: 返回行在最下方, 上方加分隔线, 内容区在分隔线上方垂直居中. */
    if (g->footer_y >= 0 && count > 0) {
        int fy = g->footer_y;
        if (count > 1) {
            draw_hline(lcd, g->x + 6, g->x + g->w - 6, fy - 1, ST7305_COLOR_BLACK);
        }
        int last_idx = count - 1;
        bool is_selected = (last_idx == state->list_dialog_selected);
        list_dialog_draw_row(lcd, g, fy, state->list_dialog_items[last_idx], is_selected);
    }

    /* 滚动条: 内容超出可见区时, 在内容区右侧画竖向条 + 滑块 */
    if (g->content_count > g->content_visible) {
        int bar_x = g->x + g->w - 3;
        int bar_y0 = g->content_y0;
        int bar_y1 = g->content_y0 + g->content_visible * g->line_h - 1;
        /* 轨道 */
        draw_vline(lcd, bar_x, bar_y0, bar_y1, ST7305_COLOR_BLACK);
        /* 滑块位置/高度 */
        int track_h = bar_y1 - bar_y0 + 1;
        int thumb_h = (track_h * g->content_visible) / g->content_count;
        if (thumb_h < 4) thumb_h = 4;
        int max_scroll = g->content_count - g->content_visible;
        int thumb_y = bar_y0 + (track_h - thumb_h) * state->list_dialog_scroll / max_scroll;
        if (thumb_y < bar_y0) thumb_y = bar_y0;
        if (thumb_y + thumb_h > bar_y1 + 1) thumb_y = bar_y1 + 1 - thumb_h;
        draw_vline(lcd, bar_x - 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, bar_x + 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        /* 滑块填黑 */
        for (int ty = thumb_y; ty < thumb_y + thumb_h; ty++) {
            st7305_draw_pixel(lcd, bar_x, ty, ST7305_COLOR_BLACK);
        }
    }
}

/* 列表选择弹窗: 智能选择全量/局部刷新
 *  - 弹窗刚打开/关闭/滚动/条目变更: 全量重绘
 *  - 仅选中项变化: 只重绘旧/新两行, 大幅减少 SPI 数据量
 *  - 自定义渲染 (on_render) 或内容脏 (content_dirty): 强制全量重绘 */
static void draw_list_dialog(menu_state_t *state) {
    if (!state->list_dialog_active) {
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        return;
    }

    st7305_handle_t *lcd = state->lcd;
    int count = state->list_dialog_count;
    int sel = state->list_dialog_selected;

    list_dialog_geom_t g;
    list_dialog_calc_geom(state, &g);

    /* 滚动越界时调整 scroll (仅对内容项; 底部"返回"是固定行, 不参与滚动) */
    if (sel < g.content_count) {  /* 选中的是内容项 */
        /* V1.0.68: 松手固定内容位置后只做边界钳制, 不因选中项回拉滚动 */
        if (state->list_dialog_drag_fix) {
            int max_scroll = g.content_count - g.content_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (state->list_dialog_scroll > max_scroll) state->list_dialog_scroll = max_scroll;
            if (state->list_dialog_scroll < 0) state->list_dialog_scroll = 0;
        } else {
            if (sel < state->list_dialog_scroll) {
                state->list_dialog_scroll = sel;
            } else if (sel >= state->list_dialog_scroll + g.content_visible) {
                state->list_dialog_scroll = sel - g.content_visible + 1;
            }
            if (state->list_dialog_scroll < 0) state->list_dialog_scroll = 0;
        }
    }
    /* 选中"返回"项: 不调整 scroll */

    /* === 自定义渲染 / 内容脏 / 滚动变化 / 拖动偏移 -> 全量重绘 (刷新滚动条) === */
    if (state->list_dialog_on_render || state->list_dialog_content_dirty ||
        state->list_dialog_scroll != state->list_dialog_prev_scroll ||
        state->list_dialog_drag_offset != 0) {
        list_dialog_draw_full(state, &g);
        state->list_dialog_prev_active = true;
        state->list_dialog_prev_selected = sel;
        state->list_dialog_prev_scroll = state->list_dialog_scroll;
        state->list_dialog_content_dirty = false;
        return;
    }

    /* === 判断是否可以走"局部刷新"路径 ===
     * 必须同时满足: 弹窗上一帧已激活 + 几何未变 + 选中项在视口内 */
    bool can_local =
        state->list_dialog_prev_active &&
        state->list_dialog_prev_selected >= 0 &&
        state->list_dialog_prev_selected < count;

    /* 选中行已出视口(返回项恒在视口) -> 回退全量 */
    if (can_local) {
        int prev = state->list_dialog_prev_selected;
        bool prev_in = (prev == g.content_count) ||
                       ((prev - state->list_dialog_scroll) >= 0 &&
                        (prev - state->list_dialog_scroll) < g.content_visible);
        bool new_in  = (sel == g.content_count) ||
                       ((sel - state->list_dialog_scroll) >= 0 &&
                        (sel - state->list_dialog_scroll) < g.content_visible);
        if (!prev_in || !new_in) can_local = false;
        /* 同一选中: 跳过绘制 (无变化) */
        if (prev == sel) {
            state->list_dialog_prev_active = true;
            state->list_dialog_prev_selected = sel;
            state->list_dialog_prev_scroll = state->list_dialog_scroll;
            return;
        }
    }

    if (!can_local) {
        list_dialog_draw_full(state, &g);
        state->list_dialog_prev_active = true;
        state->list_dialog_prev_selected = sel;
        state->list_dialog_prev_scroll = state->list_dialog_scroll;
        return;
    }

    /* === 局部刷新: 仅重绘旧/新两行 (返回项用 footer_y, 内容项用 content_y0+vis) === */
    int prev = state->list_dialog_prev_selected;
    list_dialog_draw_row(lcd, &g, list_dialog_item_y(&g, prev, state->list_dialog_scroll),
                         state->list_dialog_items[prev], false);
    list_dialog_draw_row(lcd, &g, list_dialog_item_y(&g, sel, state->list_dialog_scroll),
                         state->list_dialog_items[sel], true);

    state->list_dialog_prev_selected = sel;
    state->list_dialog_prev_active = true;
    state->list_dialog_prev_scroll = state->list_dialog_scroll;
}

/* 绘制光盘 (CD) - 黑色盘面 + 白色旋转纹路 */
static void draw_cd(st7305_handle_t *lcd, int cx, int cy, int r, int angle_deg) {
    /* 黑色盘面 (实心填充) */
    for (int y = -r; y <= r; y++) {
        int half_w = (int)sqrtf((float)(r * r - y * y));
        int yy = cy + y;
        if (yy < 0 || yy >= SCREEN_H) continue;
        int x0 = cx - half_w;
        int x1 = cx + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
        for (int x = x0; x <= x1; x++) {
            st7305_draw_pixel(lcd, x, yy, ST7305_COLOR_BLACK);
        }
    }

    /* 内圈白色环 (光盘内环, 中心透明区域) */
    int inner_r = 28;
    for (int y = -inner_r; y <= inner_r; y++) {
        int half_w = (int)sqrtf((float)(inner_r * inner_r - y * y));
        int yy = cy + y;
        if (yy < 0 || yy >= SCREEN_H) continue;
        int x0 = cx - half_w;
        int x1 = cx + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
        for (int x = x0; x <= x1; x++) {
            st7305_draw_pixel(lcd, x, yy, ST7305_COLOR_WHITE);
        }
    }

    /* 中心圆环 (直径约15px, 1px粗线, 主轴孔) */
    int center_ring_r = 14;
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(center_ring_r * cosf(rad));
        int py = cy + (int)(center_ring_r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
        }
    }

    /* 内层中心圆环 (r=10, 1px细线) */
    int inner_ring_r = 10;
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(inner_ring_r * cosf(rad));
        int py = cy + (int)(inner_ring_r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
        }
    }

    /* r=15~27 环形区域散点 (半透明效果, 跟随旋转) */
    for (int a = 0; a < 360; a++) {
        int hash = (a * 73 + 37) % 181;
        if (hash < 50) {
            int dot_r = 15 + (hash % 13);
            float rad = (float)(a + angle_deg) * 3.14159f / 180.0f;
            int px = cx + (int)(dot_r * cosf(rad));
            int py = cy + (int)(dot_r * sinf(rad));
            if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
                st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
            }
        }
    }

    /* === 旋转纹路: 8 条白色辐射线, 随角度旋转 === */
    for (int i = 0; i < 8; i++) {
        int deg = (angle_deg + i * 45) % 360;
        float rad = (float)deg * 3.14159f / 180.0f;
        float c = cosf(rad);
        float s = sinf(rad);
        /* 白色辐射线: 从内圈外侧 (inner_r+10) 到外圈内侧 (r-4) */
        int x1 = cx + (int)((inner_r + 10) * c);
        int y1 = cy + (int)((inner_r + 10) * s);
        int x2 = cx + (int)((r - 4) * c);
        int y2 = cy + (int)((r - 4) * s);
        int steps = abs(x2 - x1) + abs(y2 - y1);
        if (steps < 1) steps = 1;
        for (int s2 = 0; s2 <= steps; s2++) {
            int px = x1 + (x2 - x1) * s2 / steps;
            int py = y1 + (y2 - y1) * s2 / steps;
            /* 画 3px 粗的线 */
            for (int w = -1; w <= 1; w++) {
                int wx = px + (int)(-s * w);
                int wy = py + (int)(c * w);
                if (wx >= 0 && wx < SCREEN_W && wy >= 0 && wy < SCREEN_H) {
                    st7305_draw_pixel(lcd, wx, wy, ST7305_COLOR_WHITE);
                }
            }
        }
    }

    /* 外圈边缘白色高光 (1px 描边) */
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(r * cosf(rad));
        int py = cy + (int)(r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_WHITE);
        }
    }
}

/* MP3 播放界面渲染 (新版: 光盘+名称+进度+左菜单) */
/* 设置页面 (容器)
 * 用户需求: 删除"显示设置", "存储管理"(已提升到一级菜单), "声音设置", "蓝牙设备", "Wi-Fi".
 * 保留: 时间设置, 音量调节, 系统信息, 返回. */
static int settings_build(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    snprintf(buf[n++], 64, "\xe6\x97\xb6\xe9\x97\xb4\xe8\xae\xbe\xe7\xbd\xae"); /* 时间设置 */
    snprintf(buf[n++], 64, "\xe9\x9f\xb3\xe9\x87\x8f\xe8\xb0\x83\xe8\x8a\x82"); /* 音量调节 */
    snprintf(buf[n++], 64, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xbe\xe7\xbd\xae");   /* 连接设置 */
    snprintf(buf[n++], 64, "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf");   /* 系统信息 */
    snprintf(buf[n++], 64, "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe6\xb0\xb4"); /* 请作者喝杯水 */
    snprintf(buf[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");                           /* 返回 */
    (void)state;
    (void)max;
    return n;
}
static bool settings_on_confirm(menu_state_t *state, int idx) {
    if (idx == 5) {
        /* 返回 */
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;
    }
    if (idx == 4) {
        /* 请作者喝杯咖啡: 关闭弹窗 + 全屏 1:1 显示赞助图 (避免弹窗覆盖) */
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_key = NULL;
        state->list_dialog_on_close = NULL;
        state->list_dialog_stack_top = 0;
        state->sponsor_active = true;
        state->sponsor_notice_active = false;
        state->needs_redraw = true;
        return true;
    }
    /* idx 2 = 连接设置: 弹窗 (蓝牙/Wi-Fi 开关) */
    if (idx == 2) {
        extern void open_connection_dialog(menu_state_t *state);
        open_connection_dialog(state);
        return true;
    }
    /* 进入对应子页面: idx 0=时间设置, idx 1=音量设置, idx 3=系统信息 */
    menu_page_t target = MENU_PAGE_MAIN;
    if (idx == 0)      target = MENU_PAGE_SETTINGS_TIME;
    else if (idx == 1) target = MENU_PAGE_VOLUME;
    else if (idx == 3) target = MENU_PAGE_SETTINGS_INFO;
    else return false;
    state->current_page = target;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    return true;
}

/* === 主菜单"设置"入口: 直接弹窗 (与手柄/存储一致) ===
 * 弹窗里复用 settings_build 的 4 项, 选择项后切换到对应子页;
 * 弹窗 BACK 或选最后一项"返回"则回到主菜单. */
static void settings_dialog_on_select(menu_state_t *state, int idx);
/* 前向声明: open_connection_dialog 定义在下方, settings_dialog_on_select 提前引用 */
void open_connection_dialog(menu_state_t *state);
static void open_settings_dialog(menu_state_t *state) {
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    int cnt = settings_build(state, state->list_dialog_items, 16);
    list_dialog_open(state, "设置", cnt, settings_dialog_on_select);
}
/* 弹窗选中: 直接进入对应子弹窗, 不关闭当前设置弹窗 (list_dialog_open 自动压栈,
 * 子弹窗关闭后通过 list_dialog_pop_parent 恢复设置弹窗原选中位置).
 * idx == count-1 ("返回") 由 list_dialog CONFIRM 分支统一处理, 不会走到这里.
 * V1.0.41: 不再关闭父弹窗+on_close 回调, 改用压栈机制保持选中位置. */
static void settings_dialog_on_select(menu_state_t *state, int idx) {
    /* 时间设置: 用户需求改成弹窗 (年/月/日/时/分/秒) */
    if (idx == 0) {
        extern void open_time_dialog(menu_state_t *state);
        open_time_dialog(state);
        return;
    }
    /* idx 1 = 音量调节: list_dialog 弹窗 */
    if (idx == 1) {
        open_volume_dialog(state);
        return;
    }
    /* idx 2 = 连接设置: 蓝牙/Wi-Fi 开关弹窗 */
    if (idx == 2) {
        open_connection_dialog(state);
        return;
    }
    /* idx 3 = 系统信息: 弹窗 */
    if (idx == 3) {
        open_sysinfo_dialog(state);
        return;
    }
    /* idx 4 = 请作者喝杯水: 关闭弹窗 + 全屏显示赞助图 */
    if (idx == 4) {
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_key = NULL;
        state->list_dialog_on_close = NULL;
        state->list_dialog_stack_top = 0;
        state->sponsor_active = true;
        state->sponsor_notice_active = false;
        state->needs_redraw = true;
        return;
    }
}

/* === 时间弹窗入口 ===
 * 用户需求: 全部合并成一排, 横向排列, 时分秒用":"分隔.
 * 必须设置 list_dialog_on_render = time_dialog_render, 否则走通用行渲染,
 * UP/DOWN 调整后 list_dialog_items 不会更新, 表面"上下移动时间名称没刷新".
 * 关键: 自定义渲染从 s_time_draft 实时取值, 任何调值后只需 list_dialog_content_dirty=true
 *       强制全量重绘, 就能立刻看到新值. */
static void open_time_dialog(menu_state_t *state) {
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再写入子弹窗 items */
    list_dialog_open(state, "\xe6\x97\xb6\xe9\x97\xb4", 0, time_dialog_on_select);
    /* 至少 2 项: idx 0 = 时间行 (按确定进编辑/保存), idx count-1 = "返回" */
    int n = 0;
    snprintf(state->list_dialog_items[n++], 64, "时间");
    snprintf(state->list_dialog_items[n++], 64, "返回");
    state->list_dialog_count = n;
    /* V1.0.46: 重置草稿: 从 RTC 读取, 默认进入编辑模式, 光标在"年"字段闪烁 */
    s_time_draft.valid = false;  /* render 时会从 RTC 初始化 */
    s_time_draft.editing = true;
    s_time_draft.field = 0;
    /* 注册自定义渲染: 横向单行 "年 月 日 时:分:秒", 当前字段反色高亮/闪烁 */
    state->list_dialog_on_render = time_dialog_render;
    /* 注册方向键回调 (编辑模式 UP/DOWN 调值, LEFT/RIGHT 切字段).
     * V1.0.41: 不再设 on_close, 关闭时由 list_dialog_pop_parent 恢复父弹窗位置. */
    state->list_dialog_on_key = time_dialog_on_key;
    state->list_dialog_on_close = NULL;
    /* 首次进入: 强制全量重绘, 防止残留 prev 状态机导致自定义渲染没生效 */
    state->list_dialog_content_dirty = true;
}

/* === 隐藏设置弹窗 (V1.0.68): 系统信息 "BY: LinIT" 连点 5 次开启 ===
 * 音频方案: 0=解码输出 1=方波直驱(PWM) 2=禁用音频; 禁用触摸屏. */
static void hidden_settings_on_select(menu_state_t *state, int idx);
static int hidden_settings_build(menu_state_t *state, char buf[][64], int max) {
    static const char *scheme_name[3] = {
        "\xe8\xa7\xa3\xe7\xa0\x81\xe8\xbe\x93\xe5\x87\xba",          /* 解码输出 */
        "\xe6\x96\xb9\xe6\xb3\xa2\xe7\x9b\xb4\xe9\xa9\xb1",          /* 方波直驱 */
        "\xe7\xa6\x81\xe7\x94\xa8\xe9\x9f\xb3\xe9\xa2\x91",          /* 禁用音频 */
    };
    int s = state->settings.audio_scheme;
    if (s > 2) s = 2;
    int n = 0;
    snprintf(buf[n++], 64, "\xe9\x9f\xb3\xe9\xa2\x91\xe8\xae\xbe\xe7\xbd\xae: %s", scheme_name[s]);  /* 音频设置: X */
    if (s == 1) {
        /* 方波直驱模式下增加"试听音效"项 (循环切换常见电子音) */
        static const char *fx_name[6] = {
            "\xe7\xa1\xae\xe8\xae\xa4",  /* 确认 */
            "\xe5\x8f\x96\xe6\xb6\x88",  /* 取消 */
            "\xe9\x94\x99\xe8\xaf\xaf",  /* 错误 */
            "\xe5\xbc\x80\xe6\x9c\xba",  /* 开机 */
            "\xe5\x85\xb3\xe6\x9c\xba",  /* 关机 */
            "\xe9\x97\xb9\xe9\x93\x83",  /* 闹铃 */
        };
        int f = state->settings.tone_effect_sel;
        if (f < 0) f = 0;
        if (f > 5) f = 5;
        snprintf(buf[n++], 64, "\xe8\xaf\x95\xe5\x90\xac\xe9\x9f\xb3\xe6\x95\x88: %s", fx_name[f]);  /* 试听音效: X */
    }
    snprintf(buf[n++], 64, "\xe7\xa6\x81\xe7\x94\xa8\xe8\xa7\xa6\xe6\x91\xb8\xe5\xb1\x8f: %s",
             state->settings.touch_disable ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 禁用触摸屏: 开/关 */
    snprintf(buf[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 返回 */
    (void)max;
    return n;
}

/* 应用音频方案: 2=禁用(含 ES8311 静音 + PWM 静音), 1=方波直驱(PWM 初始化), 0=解码输出 */
static void audio_scheme_apply(menu_state_t *state) {
    uint8_t scheme = state->settings.audio_scheme;
    if (scheme == 2) {
        state->settings.audio_disable = true;
        audio_player_set_muted(true);
        tone_stop();
    } else {
        state->settings.audio_disable = false;
        audio_player_set_muted(false);
        if (scheme == 1) {
            tone_player_init();
            if (tone_player_ready()) {
                tone_play_effect(TONE_EFFECT_BOOT);   /* 切换提示音 */
            }
        } else {
            tone_stop();
        }
    }
    menu_config_save();
    if (state->selected_index >= menu_main_count(state))
        state->selected_index = menu_main_count(state) - 1;
    state->needs_redraw = true;
}

static void touch_disable_apply(menu_state_t *state) {
    if (state->settings.touch_disable) {
        touch_panel_deinit();
        ESP_LOGI(TAG, "触摸屏已禁用 (释放 I2C 驱动内存)");
    } else {
        touch_panel_init();
        ESP_LOGI(TAG, "触摸屏已启用");
    }
    menu_config_save();
    state->needs_redraw = true;
}

static void hidden_settings_on_select(menu_state_t *state, int idx) {
    if (idx == 0) {
        /* 音频方案: 0→1→2 循环 */
        state->settings.audio_scheme = (uint8_t)((state->settings.audio_scheme + 1) % 3);
        audio_scheme_apply(state);
    } else if (state->settings.audio_scheme == 1 && idx == 1) {
        /* 试听音效: 循环切换并播放 */
        state->settings.tone_effect_sel = (uint8_t)((state->settings.tone_effect_sel + 1) % 6);
        menu_config_save();
        tone_play_effect((tone_effect_t)state->settings.tone_effect_sel);
    } else if ((state->settings.audio_scheme == 1 ? idx == 2 : idx == 1)) {
        state->settings.touch_disable = !state->settings.touch_disable;
        touch_disable_apply(state);
    }
    hidden_settings_build(state, state->list_dialog_items, 8);
    state->list_dialog_count = 3 + (state->settings.audio_scheme == 1 ? 1 : 0);
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
}

static void hidden_settings_open(menu_state_t *state) {
    state->list_dialog_return_page = state->current_page;
    list_dialog_open(state, "\xe9\x9a\x90\xe8\x97\x8f\xe8\xae\xbe\xe7\xbd\xae", 0, hidden_settings_on_select);  /* 隐藏设置 */
    hidden_settings_build(state, state->list_dialog_items, 8);
    state->list_dialog_count = 3 + (state->settings.audio_scheme == 1 ? 1 : 0);
    state->list_dialog_content_dirty = true;
}

/* === 系统信息弹窗入口 ===
 * 用户需求: 系统信息改用弹窗, 与时间设置一致, 只读, 关闭时回到 settings 弹窗.
 * 内容: 固件版本、LCD、Flash、PSRAM、作者. 后续可加 SD 卡容量 / 蓝牙状态等. */
static int s_linit_taps = 0;          /* V1.0.68: BY: LinIT 连点计数 */
static uint32_t s_linit_tap_last_ms = 0;
static void sysinfo_dialog_on_select(menu_state_t *state, int idx) {
    /* 只读弹窗, 选"返回"时 list_dialog 通用分支会关闭弹窗, 不会走到这里.
     * V1.0.68: "BY: LinIT" (idx=4) 连点 5 次 → 隐藏设置 (物理键确认也计数) */
    if (idx == 4) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (s_linit_taps > 0 && now - s_linit_tap_last_ms > 3000) s_linit_taps = 0;
        s_linit_taps++;
        s_linit_tap_last_ms = now;
        if (s_linit_taps >= 5) {
            s_linit_taps = 0;
            ESP_LOGI(TAG, "彩蛋: 连点 5 次 LinIT -> 隐藏设置");
            hidden_settings_open(state);
        }
    }
    (void)state;
}
static void open_sysinfo_dialog(menu_state_t *state) {
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    /* V1.0.41: 先 list_dialog_open (压栈保存父弹窗 items), 再 build 写入子弹窗 items */
    list_dialog_open(state, "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf", 0, sysinfo_dialog_on_select);
    int cnt = sysinfo_build(state, state->list_dialog_items, 16);
    state->list_dialog_count = cnt;
    /* V1.0.41: 系统信息只读, 不需要 on_key/on_close; 关闭时由 list_dialog_pop_parent 恢复父弹窗位置 */
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
}

/* V1.0.46: Wi-Fi 网络列表弹窗前向声明 */
static void wifi_net_open(menu_state_t *state);
static void wifi_net_on_select(menu_state_t *state, int idx);
static void wifi_net_poll(menu_state_t *state);

/* === 连接设置弹窗 (蓝牙开关 + Wi-Fi 连网开关) ===
 * V1.0.46: Wi-Fi 为连网功能 (STA), 开启后若已有保存配置则自动连接,
 * 否则弹出虚拟键盘输入 SSID/密码; 弹窗中显示连接状态. */
static void connection_dialog_rebuild(menu_state_t *state) {
    int n = 0;
    /* 蓝牙: 开/关 */
    snprintf(state->list_dialog_items[n++], 64,
             "\xe8\x93\x9d\xe7\x89\x99: %s",  /* 蓝牙: X */
             state->settings.bt_enabled ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 开 / 关 */
    /* V1.0.46: Wi-Fi 只做开关, 不显示状态 */
    snprintf(state->list_dialog_items[n++], 64,
             "Wi-Fi: %s",
             state->settings.wifi_enabled ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    /* 开启后显示"连接网络"项 (选择网络列表, 类似蓝牙选择窗口) */
    if (state->settings.wifi_enabled) {
        snprintf(state->list_dialog_items[n++], 64,
                 "\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 连接网络 */
    }
    snprintf(state->list_dialog_items[n++], 64, "\xe8\xbf\x94\xe5\x9b\x9e"); /* 返回 */
    state->list_dialog_count = n;
    state->list_dialog_content_dirty = true;
    state->list_dialog_prev_active = false;  /* 强制全量重绘 */
    state->needs_redraw = true;
}

static void connection_dialog_on_select(menu_state_t *state, int idx) {
    if (idx == 0) {
        /* 蓝牙开关 */
        state->settings.bt_enabled = !state->settings.bt_enabled;
        if (state->settings.bt_enabled) {
            bt_manager_enable();
            state->bt_auto_connect_on_enable = true;
        } else {
            bt_manager_disable();
        }
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        connection_dialog_rebuild(state);
    } else if (idx == 1) {
        /* V1.0.46: Wi-Fi 只做开关 (连接网络走下一项) */
        state->settings.wifi_enabled = !state->settings.wifi_enabled;
#if WIFI_SUPPORT
        if (state->settings.wifi_enabled) {
            wifi_manager_enable();
        } else {
            wifi_manager_disable();
        }
#endif
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        connection_dialog_rebuild(state);
    } else if (state->settings.wifi_enabled && idx == 2) {
        /* V1.0.46: 连接网络 — 打开网络列表弹窗 (自动压栈保存连接设置弹窗) */
        wifi_net_open(state);
    }
    /* idx == count-1 (返回) 由 list_dialog 通用分支处理 */
}

/* V1.0.46: Wi-Fi 网络列表弹窗 (选择网络, 类似蓝牙选择窗口) */
static bool s_wifi_net_dialog_open = false;  /* 当前弹窗是否为网络列表 */
static bool s_wifi_net_loaded = false;       /* 扫描结果是否已填充列表 */

static void wifi_net_open(menu_state_t *state) {
    s_wifi_net_dialog_open = true;
    s_wifi_net_loaded = false;
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    /* 初始 2 项: [扫描中...] + [返回] (扫描中也能返回退出) */
    list_dialog_open(state, "\xe9\x80\x89\xe6\x8b\xa9\xe7\xbd\x91\xe7\xbb\x9c", 2, wifi_net_on_select); /* 选择网络 */
    snprintf(state->list_dialog_items[0], 64, "\xe6\x89\xab\xe6\x8f\x8f\xe4\xb8\xad..."); /* 扫描中... */
    snprintf(state->list_dialog_items[1], 64, "\xe8\xbf\x94\xe5\x9b\x9e");               /* 返回 */
    state->list_dialog_count = 2;
    state->list_dialog_content_dirty = true;
#if WIFI_SUPPORT
    if (!wifi_manager_scan_start()) {
        snprintf(state->list_dialog_items[0], 64, "\xe6\x89\xab\xe6\x8f\x8f\xe5\xa4\xb1\xe8\xb4\xa5"); /* 扫描失败 */
    }
#endif
    state->needs_redraw = true;
}

/* V1.0.67: 网络列表关闭(返回/BACK)时停止扫描, 避免后台继续扫 */
static void wifi_net_close(menu_state_t *state) {
    (void)state;
    if (s_wifi_net_dialog_open) {
        s_wifi_net_dialog_open = false;
        s_wifi_net_loaded = false;
#if WIFI_SUPPORT
        wifi_manager_scan_stop();
#endif
    }
}

/* 选中网络: 完全关闭所有弹窗, 打开全屏虚拟键盘输入密码 (弹窗不能遮挡全屏) */
static void wifi_net_on_select(menu_state_t *state, int idx) {
    s_wifi_net_dialog_open = false;
#if WIFI_SUPPORT
    wifi_manager_scan_stop();
    if (idx < 0 || idx >= wifi_manager_get_scan_count()) return;
    char ssid[33];
    if (!wifi_manager_get_scan_ssid(idx, ssid, sizeof(ssid))) return;
#else
    (void)idx;
    return;
#endif
    /* 完全关闭所有弹窗 (含压栈的连接设置弹窗) */
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
    state->list_dialog_stack_top = 0;
    /* 打开全屏虚拟键盘: SSID 预填, 直接输入密码 */
    state->wifi_kb_active = true;
    state->wifi_kb_field = 1;
    state->wifi_kb_cur = 0;
    state->wifi_kb_shift = false;
    state->wifi_kb_sym = false;
    strncpy(state->wifi_kb_ssid, ssid, sizeof(state->wifi_kb_ssid) - 1);
    state->wifi_kb_ssid[sizeof(state->wifi_kb_ssid) - 1] = '\0';
    state->wifi_kb_pass[0] = '\0';
    state->wifi_kb_msg[0] = '\0';
    state->needs_redraw = true;
}

/* 菜单轮询: 网络列表弹窗打开且扫描完成时填充列表 */
static void wifi_net_poll(menu_state_t *state) {
    if (!s_wifi_net_dialog_open || s_wifi_net_loaded) return;
#if WIFI_SUPPORT
    if (!wifi_manager_is_scan_done()) return;
    s_wifi_net_loaded = true;
    int cnt = wifi_manager_get_scan_count();
    if (cnt > 14) cnt = 14;
    for (int i = 0; i < cnt; i++) {
        char ssid[33];
        if (wifi_manager_get_scan_ssid(i, ssid, sizeof(ssid))) {
            snprintf(state->list_dialog_items[i], 64, "%s", ssid);
        } else {
            state->list_dialog_items[i][0] = '\0';
        }
    }
    if (cnt == 0) {
        snprintf(state->list_dialog_items[0], 64, "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\xe7\xbd\x91\xe7\xbb\x9c"); /* 未找到网络 */
        cnt = 1;
    }
    snprintf(state->list_dialog_items[cnt], 64, "\xe8\xbf\x94\xe5\x9b\x9e"); /* 返回 */
    state->list_dialog_count = cnt + 1;
    state->list_dialog_selected = 0;
    state->list_dialog_scroll = 0;
    state->list_dialog_content_dirty = true;
    state->needs_redraw = true;
#endif
}

void open_connection_dialog(menu_state_t *state) {
    state->list_dialog_return_page = MENU_PAGE_MAIN;
    list_dialog_open(state, "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xbe\xe7\xbd\xae", 0, connection_dialog_on_select); /* 连接设置 */
    connection_dialog_rebuild(state);
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
}

/* 屏保设置已移除 UI (硬编码 3 分钟 + 星空动画, 音乐播放时禁用) */

/* ============ MP3 播放器 ============ */

#define MP3_DIR "/sdcard/mp3"

#define MAX_MP3_FILES 64

static char (*mp3_files)[128] = NULL;
static int  mp3_count = 0;
static int  mp3_current = 0;
static bool mp3_in_player = false;  /* true=播放界面, false=列表 */

/* 扫描 MP3 文件列表 (带缓存: 只扫一次, 避免每帧 opendir 阻塞) */
static void mp3_scan_files(void) {
    static bool s_mp3_scanned = false;
    if (s_mp3_scanned && mp3_count > 0) return;

    if (!mp3_files) {
        mp3_files = heap_caps_malloc(MAX_MP3_FILES * 128, MALLOC_CAP_SPIRAM);
        if (!mp3_files) mp3_files = malloc(MAX_MP3_FILES * 128);
    }
    mp3_count = 0;
    DIR *dir = opendir(MP3_DIR);
    if (!dir && sd_is_mounted()) {
        /* opendir 失败但 s_mounted=true: SD 卡可能因 NVS flash 写而失效, 重新挂载 */
        ESP_LOGW(TAG, "MP3扫描: opendir失败, 尝试重新挂载SD卡");
        sd_unmount();
        sd_mount();
        dir = opendir(MP3_DIR);
    }
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && mp3_count < MAX_MP3_FILES) {
            if (MENU_IS_APPLE_METANAME(ent->d_name)) continue;
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && (strcasecmp(dot, ".mp3") == 0)) {
                strncpy(mp3_files[mp3_count], ent->d_name, sizeof(mp3_files[0]) - 1);
                mp3_files[mp3_count][sizeof(mp3_files[0]) - 1] = '\0';
                mp3_count++;
            }
        }
        closedir(dir);
    }
    s_mp3_scanned = true;
    ESP_LOGI(TAG, "MP3 文件扫描完成: %d 个文件 (已缓存)", mp3_count);
}

static int mp3_build(menu_state_t *state, char buf[][64], int max) {
    mp3_scan_files();
    int n = 0;
    for (int i = 0; i < mp3_count && n < max; i++) {
        snprintf(buf[n], 64, "%.63s", mp3_files[i]);
        n++;
    }
    if (n == 0) {
        snprintf(buf[n++], 64, "未找到 MP3 文件");
        snprintf(buf[n++], 64, "请放入 %s", MP3_DIR);
    }
    return n;
}

static bool mp3_on_confirm(menu_state_t *state, int idx) {
    if (mp3_count == 0) return true;

    if (!mp3_in_player) {
        /* 列表中选中: 开始播放 */
        mp3_current = idx;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", MP3_DIR, mp3_files[idx]);
        int ret = audio_player_play(path);
        if (ret != 0) {
            ESP_LOGE(TAG, "播放失败: %s", path);
            return true;
        }
        mp3_in_player = true;
        state->needs_redraw = true;
        return true;
    }

    /* 已在播放中: 如果选了不同歌曲则切换, 否则暂停/恢复 */
    if (idx != mp3_current) {
        audio_player_stop();
        mp3_current = idx;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", MP3_DIR, mp3_files[idx]);
        int ret = audio_player_play(path);
        if (ret != 0) {
            ESP_LOGE(TAG, "播放失败: %s", path);
            return true;
        }
        state->needs_redraw = true;
        return true;
    }

    /* 同一首歌: 暂停/恢复 */
    audio_state_t st = audio_player_get_state();
    if (st == AUDIO_STATE_PLAYING) {
        audio_player_pause();
    } else if (st == AUDIO_STATE_PAUSED) {
        audio_player_resume();
    } else if (st == AUDIO_STATE_STOPPED || st == AUDIO_STATE_IDLE) {
        /* 当前歌曲播完了, 下一曲 */
        mp3_current = (mp3_current + 1) % mp3_count;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", MP3_DIR, mp3_files[mp3_current]);
        audio_player_play(path);
    }
    state->needs_redraw = true;
    return true;
}

static bool mp3_on_lr(menu_state_t *state, int idx, bool is_right) {
    if (mp3_count == 0) return true;

    if (mp3_in_player) {
        /* 播放界面: 左右 = 上一曲/下一曲 */
        audio_player_stop();
        if (is_right) {
            mp3_current = (mp3_current + 1) % mp3_count;
        } else {
            mp3_current = (mp3_current - 1 + mp3_count) % mp3_count;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", MP3_DIR, mp3_files[mp3_current]);
        audio_player_play(path);
        state->needs_redraw = true;
        return true;
    }
    return false;
}

/* MP3 播放器退出清理 (从 BACK 返回时调用) */
static void mp3_exit_cleanup(void) {
    if (mp3_in_player) {
        audio_player_stop();
        mp3_in_player = false;
    }
}

/* === 电子书: 双栏菜单 (镜像游戏菜单) ===
 * 左栏: 0=设置, 1=收藏书架, 2..N-1=分类目录, 最后一个=临时目录 (根目录书籍)
 * 右栏: 设置项 / 收藏书籍 / 分类书籍
 * 根目录里的 txt 不物理移动, 在"临时目录"里虚拟归集 (最后一个文件夹) */

#define BOOK_FULL_SPACE "\xe3\x80\x80"

static int book_utf8_count(const char *s) {
    int cnt = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) cnt++;
    return cnt;
}

/* 名称补齐到 target 个中文字符宽度并追加冒号, 使设置项冒号竖直对齐 */
static void book_pad_name(char *buf, size_t len, int target, const char *name) {
    int n = book_utf8_count(name);
    int pad = (n < target) ? target - n : 0;
    int pos = 0;
    for (int i = 0; i < pad && pos < (int)len; i++)
        pos += snprintf(buf + pos, len - (size_t)pos, "%s", BOOK_FULL_SPACE);
    snprintf(buf + pos, len - (size_t)pos, "%s:", name);
}

static bool book_is_book_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".fb2") == 0 ||
        strcasecmp(dot, ".epub") == 0) return true;
    return false;
}

static void book_scan_folders(menu_state_t *state) {
    g_book_folder_count = 0;
    g_book_root_count = 0;
    DIR *dir = opendir(BOOK_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (MENU_IS_APPLE_METANAME(ent->d_name)) continue;
            if (ent->d_type == DT_DIR) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                if (g_book_folder_count < BOOK_MAX_FOLDERS) {
                    snprintf(g_book_folder_names[g_book_folder_count], 64, "%.63s", ent->d_name);
                    g_book_folder_count++;
                }
            } else if (book_is_book_name(ent->d_name)) {
                g_book_root_count++;
            }
        }
        closedir(dir);
    }
    /* 字母序排序文件夹 */
    for (int i = 1; i < g_book_folder_count; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", g_book_folder_names[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(g_book_folder_names[j], tmp) > 0) {
            memcpy(g_book_folder_names[j + 1], g_book_folder_names[j], 64);
            j--;
        }
        memcpy(g_book_folder_names[j + 1], tmp, 64);
    }

    if (!state->book_loaded) {
        state->book_loaded = true;
        state->select_focus = 1;
        state->select_folder_idx = 1;   /* 默认收藏书架 */
        state->select_folder_scroll = 0;
        state->select_game_idx = 0;
        state->select_game_scroll = 0;
    }
}

/* 左栏总数: 设置 + 收藏书架 + 分类目录 + (根目录有书时) 临时目录 */
static int book_sidebar_total(void) {
    return 2 + g_book_folder_count + (g_book_root_count > 0 ? 1 : 0);
}

static bool book_is_temp_idx(int idx) {
    return (g_book_root_count > 0) && (idx == 2 + g_book_folder_count);
}

/* 旋转方向循环顺序: 0°(上) -> 90°(左) -> 180°(下) -> 270°(右), 默认下 */
static const uint8_t s_book_rot_order[4] = { 0, 2, 1, 3 };

static uint8_t book_rot_next(uint8_t r) {
    for (int i = 0; i < 4; i++) {
        if (s_book_rot_order[i] == r) {
            return s_book_rot_order[(i + 1) % 4];
        }
    }
    return 2;   /* 左 */
}

static void book_apply_settings_to_reader(menu_state_t *state) {
    book_reader_set_settings(state->book_knock, (int)state->book_sens,
                             state->book_night, state->book_pagenum,
                             (int)state->book_rot,
                             (int)state->book_fontsize, (int)state->book_margin,
                             (int)state->book_lineh, (int)state->book_gap);
}

/* 构建右栏 (设置项 / 收藏 / 目录书籍) */
static int book_build_right(menu_state_t *state, char buf[][64], int max) {
    int n = 0;
    if (state->select_folder_idx == 0) {
        static const char *rot_name[4] = { "上", "下", "左", "右" };
        static const char *fsize_name[4] = { "20", "24", "28", "32" };
        static const char *margin_name[3] = { "窄", "中", "宽" };
        static const char *lineh_name[3] = { "紧凑", "标准", "宽松" };
        static const char *gap_name[2] = { "标准", "宽松" };
        char tmp[24];
        book_pad_name(tmp, sizeof(tmp), 5, "夜间模式");
        snprintf(buf[n++], 64, "%s|%s", tmp, state->book_night ? "开" : "关");
        book_pad_name(tmp, sizeof(tmp), 5, "显示页码");
        snprintf(buf[n++], 64, "%s|%s", tmp, state->book_pagenum ? "开" : "关");
        book_pad_name(tmp, sizeof(tmp), 5, "旋转方向");
        int r = (state->book_rot > 3) ? 0 : state->book_rot;
        snprintf(buf[n++], 64, "%s|%s", tmp, rot_name[r]);
        book_pad_name(tmp, sizeof(tmp), 5, "字体大小");
        int fs = (state->book_fontsize > 3) ? 1 : state->book_fontsize;
        snprintf(buf[n++], 64, "%s|%s", tmp, fsize_name[fs]);
        book_pad_name(tmp, sizeof(tmp), 5, "边距");
        int mg = (state->book_margin > 2) ? 1 : state->book_margin;
        snprintf(buf[n++], 64, "%s|%s", tmp, margin_name[mg]);
        book_pad_name(tmp, sizeof(tmp), 5, "行高");
        int lh = (state->book_lineh > 2) ? 1 : state->book_lineh;
        snprintf(buf[n++], 64, "%s|%s", tmp, lineh_name[lh]);
        book_pad_name(tmp, sizeof(tmp), 5, "字距");
        int gp = (state->book_gap > 1) ? 0 : state->book_gap;
        snprintf(buf[n++], 64, "%s|%s", tmp, gap_name[gp]);
        return n;
    }

    if (state->select_folder_idx == 1) {
        int cnt = 0;
        const char *const *favs = favorites_list(FAV_ENGINE_BOOK, &cnt);
        for (int i = 0; i < cnt && n < max; i++) {
            const char *path = favs[i];
            if (!path || strncmp(path, "/sdcard/books/", 13) != 0) continue;
            const char *slash = strrchr(path, '/');
            const char *name = slash ? slash + 1 : path;
            snprintf(buf[n], 64, "%.63s", name);
            char *dot = strrchr(buf[n], '.');
            if (dot) *dot = '\0';
            n++;
        }
        return n;
    }

    const char *folder = NULL;
    if (book_is_temp_idx(state->select_folder_idx)) {
        folder = "";
    } else {
        int fi = state->select_folder_idx - 2;
        if (fi < 0 || fi >= g_book_folder_count) return 0;
        folder = g_book_folder_names[fi];
    }
    char dir_path[256];
    if (folder[0]) {
        snprintf(dir_path, sizeof(dir_path), "%s/%s", BOOK_DIR, folder);
    } else {
        snprintf(dir_path, sizeof(dir_path), "%s", BOOK_DIR);
    }
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        if (MENU_IS_APPLE_METANAME(ent->d_name)) continue;
        if (!book_is_book_name(ent->d_name)) continue;
        snprintf(g_book_files[n], 64, "%.63s", ent->d_name);
        snprintf(buf[n], 64, "%.63s", ent->d_name);
        char *dot = strrchr(buf[n], '.');
        if (dot) *dot = '\0';
        n++;
    }
    closedir(dir);
    /* 字母序排序 */
    for (int i = 1; i < n; i++) {
        char tmp[64];
        char tmpf[64];
        snprintf(tmp, sizeof(tmp), "%s", buf[i]);
        snprintf(tmpf, sizeof(tmpf), "%s", g_book_files[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(buf[j], tmp) > 0) {
            memcpy(buf[j + 1], buf[j], 64);
            memcpy(g_book_files[j + 1], g_book_files[j], 64);
            j--;
        }
        memcpy(buf[j + 1], tmp, 64);
        memcpy(g_book_files[j + 1], tmpf, 64);
    }
    return n;
}

/* 根据右栏索引得到完整书籍路径 (收藏用存储路径, 目录用当前选中目录) */
static bool book_get_path_by_index(menu_state_t *state, int idx, char *out, size_t n) {
    if (idx < 0 || idx >= g_sub_count) return false;
    if (state->select_folder_idx == 1) {
        int cnt = 0;
        const char *const *favs = favorites_list(FAV_ENGINE_BOOK, &cnt);
        int k = 0;
        for (int i = 0; i < cnt; i++) {
            const char *path = favs[i];
            if (!path || strncmp(path, "/sdcard/books/", 13) != 0) continue;
            if (k == idx) {
                snprintf(out, n, "%s", path);
                return true;
            }
            k++;
        }
        return false;
    }
    const char *folder = NULL;
    if (book_is_temp_idx(state->select_folder_idx)) {
        folder = "";
    } else {
        int fi = state->select_folder_idx - 2;
        if (fi < 0 || fi >= g_book_folder_count) return false;
        folder = g_book_folder_names[fi];
    }
    if (folder[0]) {
        snprintf(out, n, "%s/%s/%.120s", BOOK_DIR, folder,
                 g_book_files[idx][0] ? g_book_files[idx] : g_sub_items[idx]);
    } else {
        snprintf(out, n, "%s/%.120s", BOOK_DIR,
                 g_book_files[idx][0] ? g_book_files[idx] : g_sub_items[idx]);
    }
    return true;
}

/* 右栏确认: 设置项循环切换 / 打开书籍 */
static bool book_on_confirm_item(menu_state_t *state, int idx) {
    if (state->select_folder_idx == 0) {
        switch (idx) {
        case 0: state->book_night = !state->book_night; break;
        case 1: state->book_pagenum = !state->book_pagenum; break;
        case 2: state->book_rot = book_rot_next(state->book_rot); break;
        case 3: state->book_fontsize = (uint8_t)((state->book_fontsize + 1) % 4); break;
        case 4: state->book_margin = (uint8_t)((state->book_margin + 1) % 3); break;
        case 5: state->book_lineh = (uint8_t)((state->book_lineh + 1) % 3); break;
        case 6: state->book_gap = (uint8_t)((state->book_gap + 1) % 2); break;
        default: return false;
        }
        book_apply_settings_to_reader(state);
        menu_config_save();
        state->needs_redraw = true;
        return true;
    }
    char path[256];
    if (!book_get_path_by_index(state, idx, path, sizeof(path))) return false;
    book_apply_settings_to_reader(state);
    if (!book_reader_open(path)) {
        ESP_LOGE(TAG, "电子书打开失败: %s", path);
        snprintf(state->hint_text, sizeof(state->hint_text), "打开失败");
        state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 1500;
    }
    state->needs_redraw = true;
    return true;
}

/* 当前选中书籍路径 (长按多功能键收藏用) */
static bool book_get_current_path(menu_state_t *state, char *out, size_t n) {
    if (state->select_focus != 1 || state->select_folder_idx == 0) return false;
    return book_get_path_by_index(state, state->select_game_idx, out, n);
}

/* 电子书页面按键处理 (阅读器未打开时) */
static void book_handle_action(menu_state_t *state, menu_action_t action) {
    /* 右栏数据可能滞后, 每次按键重建一次 (书单量小) */
    g_sub_count = book_build_right(state, g_sub_items, sizeof(g_sub_items) / sizeof(g_sub_items[0]));
    int left_total = book_sidebar_total();
    if (left_total < 1) left_total = 1;

    switch (action) {
    case MENU_ACTION_LEFT:
        state->select_focus = 0;
        state->needs_redraw = true;
        return;
    case MENU_ACTION_RIGHT:
        if (g_sub_count > 0) {
            state->select_focus = 1;
            state->select_game_idx = 0;
            state->select_game_scroll = 0;
        }
        state->needs_redraw = true;
        return;
    case MENU_ACTION_UP:
        if (state->select_focus == 0) {
            if (state->select_folder_idx > 0) state->select_folder_idx--;
            else state->select_folder_idx = left_total - 1;
        } else if (g_sub_count > 0) {
            if (state->select_game_idx > 0) state->select_game_idx--;
            else state->select_game_idx = g_sub_count - 1;
        }
        state->needs_redraw = true;
        return;
    case MENU_ACTION_DOWN:
        if (state->select_focus == 0) {
            if (state->select_folder_idx < left_total - 1) state->select_folder_idx++;
            else state->select_folder_idx = 0;
        } else if (g_sub_count > 0) {
            if (state->select_game_idx < g_sub_count - 1) state->select_game_idx++;
            else state->select_game_idx = 0;
        }
        state->needs_redraw = true;
        return;
    case MENU_ACTION_CONFIRM:
        if (state->select_focus == 0) {
            if (g_sub_count > 0) {
                state->select_focus = 1;
                state->select_game_idx = 0;
                state->select_game_scroll = 0;
            }
        } else {
            book_on_confirm_item(state, state->select_game_idx);
        }
        state->needs_redraw = true;
        return;
    case MENU_ACTION_BACK:
    case MENU_ACTION_HOME:
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = state->main_selected_index;
        state->scroll_offset = 0;
        state->book_loaded = false;   /* 下次进入重新扫描书单 */
        state->needs_redraw = true;
        return;
    default:
        return;
    }
}

/* === 游戏设置子页 (电子词典 -> 侧栏"游戏设置") ===
 * GB 模式 (select_mode==1) 子项: 显示模式 / 补充按键(GB辅助) / 虚拟按键 / 模拟灰度 / 音量 / 还原映射
 * 电子词典模式 子项: 状态栏设置 / 显示模式 / 虚拟按键 / 抗锯齿 / 音量
 *   (V1.0.68: 已删除 BBK 补充按键映射 + 还原映射)
 * 选项名用全角空格补位, 使所有冒号竖直对齐, 通过动作映射路由 confirm/lr.
 * 不含"返回"项: 左右键即可切换侧栏(返回上一侧栏), 按 BACK 键回上一级
 * 在两栏布局中, 右栏设置项的单击行为即触发对应动作 (无需进入"编辑模式"再按左右键).
 * 各项直接翻转布尔值/调整数值, 翻转后调用 select_game_invalidate_cache() 强制右栏重绘. */
/* 游戏设置项动作 ID (固定顺序, confirm/lr 据此路由) */
enum {
    GS_VOLUME,      /* 音量 (GB/GBC) */
    GS_GRAY,        /* 模拟灰度 (GB/GBC) */
    GS_STATUSBAR,   /* 状态栏设置 (BBK) */
    GS_AA,          /* 抗锯齿 (BBK) */
    GS_DISPLAY,     /* 显示模式: 全屏/点对点 */
    GS_SUPKEY,      /* 补充按键 */
    GS_VKEY,        /* 虚拟按键 (V1.0.68) */
    GS_RESETMAP,    /* 还原映射 */
};

/* 当前页面固定顺序的动作映射 (供 confirm/lr 使用) */
static int g_gs_action[8];
static int g_gs_count;

/* 统计 UTF-8 字符串的字符数 (按首字节判定, 非字节数) */
static int utf8_charcount(const char *s) {
    int cnt = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) cnt++;
    return cnt;
}

/* 全角空格 (U+3000) UTF-8 编码, 宽 24px, 与中文字符等宽, 用于补齐占位 */
#define GS_FULL_SPACE "\xe3\x80\x80"

/* 将选项名补齐到 target 个中文字符宽度并追加冒号, 使所有选项冒号竖直对齐 */
static void gs_pad_name(char *buf, size_t len, int target, const char *name) {
    int n = utf8_charcount(name);
    int pad = (n < target) ? target - n : 0;
    int pos = 0;
    for (int i = 0; i < pad && pos < (int)len; i++)
        pos += snprintf(buf + pos, len - (size_t)pos, "%s", GS_FULL_SPACE);
    snprintf(buf + pos, len - (size_t)pos, "%s:", name);
}

/* 显示模式名: 0=点对点, 1=全屏, 2=拉伸 */
static const char *display_mode_name(uint8_t m) {
    switch (m) {
    case 0:  return "\xe7\x82\xb9\xe5\xaf\xb9\xe7\x82\xb9";  /* 点对点 */
    case 2:  return "\xe6\x8b\x89\xe4\xbc\xb8";               /* 拉伸 */
    default: return "\xe5\x85\xa8\xe5\xb1\x8f";               /* 全屏 */
    }
}

/* 显示模式档位数-1: BBK/NES 两档 (0/1), GB/GBC/AB 三档 (0/1/2) */
static int display_mode_max(const menu_state_t *state) {
    if (state->select_mode == 0) return 1;      /* BBK */
    if (state->select_engine == 2) return 1;    /* NES */
    return 2;                                   /* GB/GBC/AB */
}

/* 按动作生成设置项 label; 返回 true 表示成功生成 */
static bool game_settings_make_label(const menu_state_t *state, int action,
                                     char *buf, size_t len) {
    /* 各模式下最长选项名(中文字符数): GB/GBC=4, BBK=5. 用全角空格补齐较短名称,
     * 使所有冒号在同一竖直位置对齐 (中文字符与全角空格同为 24px 宽). */
    int pad = (state->select_mode == 1) ? 4 : 5;
    switch (action) {
    case GS_VOLUME:
        gs_pad_name(buf, len, pad, "\xe9\x9f\xb3\xe9\x87\x8f");  /* 音量 */
        snprintf(buf + strlen(buf), len - strlen(buf), " %d",
                 (int)state->settings.volume);
        return true;
    case GS_GRAY:
        gs_pad_name(buf, len, pad, "\xe6\xa8\xa1\xe6\x8b\x9f\xe7\x81\xb0\xe5\xba\xa6");  /* 模拟灰度 */
        snprintf(buf + strlen(buf), len - strlen(buf), " %s",
                 engine_gray_name(engine_gray_get(state->select_engine)));
        return true;
    case GS_STATUSBAR:
        gs_pad_name(buf, len, pad, "\xe7\x8a\xb6\xe6\x80\x81\xe6\xa0\x8f\xe8\xae\xbe\xe7\xbd\xae");  /* 状态栏设置 */
        snprintf(buf + strlen(buf), len - strlen(buf), " %s",
                 state->game_show_statusbar ? "\xe6\x98\xbe\xe7\xa4\xba" : "\xe9\x9a\x90\xe8\x97\x8f"); /* 显示/隐藏 */
        return true;
    case GS_AA: {
        gs_pad_name(buf, len, pad, "\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf");  /* 抗锯齿 */
        static const char *nm[] = {
            "\xe5\x85\xb3",            /* 关 */
            "EPX",
        };
        int p = (int)state->game_pic_opt;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        snprintf(buf + strlen(buf), len - strlen(buf), " %s", nm[p]);
        return true;
    }
    case GS_DISPLAY:
        gs_pad_name(buf, len, pad, "\xe6\x98\xbe\xe7\xa4\xba\xe6\xa8\xa1\xe5\xbc\x8f");  /* 显示模式 */
        snprintf(buf + strlen(buf), len - strlen(buf), " %s",
                 display_mode_name(state->game_display_mode));
        return true;
    case GS_VKEY:
        gs_pad_name(buf, len, pad, "\xe8\x99\x9a\xe6\x8b\x9f\xe6\x8c\x89\xe9\x94\xae");  /* 虚拟按键 */
        snprintf(buf + strlen(buf), len - strlen(buf), " %s",
                 state->game_virtual_keys ? "\xe5\xbc\x80" : "\xe5\x85\xb3"); /* 开/关 */
        return true;
    case GS_RESETMAP:
        snprintf(buf, len, "\xe8\xbf\x98\xe5\x8e\x9f\xe6\x98\xa0\xe5\xb0\x84");  /* 还原映射 */
        return true;
    }
    return false;
}

static int game_settings_build(menu_state_t *state, char buf[][64], int max) {
    /* 固定顺序 (不随文字数量重排, 冒号已通过对齐补齐) */
    int base[8];
    int m = 0;
    if (state->select_mode == 1) {
        /* V1.0.68: GB/GBC 设置: 显示模式 / 虚拟按键 / 模拟灰度 / 音量 / 还原映射
         * (已删除补充按键映射) */
        base[m++] = GS_DISPLAY;
        base[m++] = GS_VKEY;
        base[m++] = GS_GRAY;
        base[m++] = GS_VOLUME;
        base[m++] = GS_RESETMAP;
    } else {
        /* 电子词典/BBK 设置: 状态栏设置 / 显示模式 / 虚拟按键 / 抗锯齿 / 音量
         * (V1.0.68: 已删除 BBK 补充按键映射 + 还原映射, 保留 GB 的辅助键映射) */
        base[m++] = GS_STATUSBAR;
        base[m++] = GS_DISPLAY;
        base[m++] = GS_VKEY;
        base[m++] = GS_AA;
        base[m++] = GS_VOLUME;
    }

    /* 保存动作映射供 confirm/lr 路由 */
    g_gs_count = m;
    for (int i = 0; i < m; i++) g_gs_action[i] = base[i];

    int n = 0;
    for (int i = 0; i < m && n < max; i++) {
        if (game_settings_make_label(state, g_gs_action[i], buf[n], 64))
            n++;
    }
    ESP_LOGI(TAG, "game_settings_build: n=%d, mode=%d", n, state->select_mode);
    return n;
}

static bool game_settings_on_confirm(menu_state_t *state, int idx) {
    if (idx < 0 || idx >= g_gs_count) return false;
    switch (g_gs_action[idx]) {
    case GS_VOLUME: {
        /* 音量: 与系统/音乐统一, 单击 +1 档 (0-10) 循环 */
        int v = (int)state->settings.volume + 1;
        if (v > 10) v = 0;
        state->settings.volume = (uint8_t)v;
        audio_player_set_volume(volume_step_to_percent(v));
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "音量(统一) -> %d", v);
        return true;
    }
    case GS_GRAY: {
        /* 模拟灰度: 单击两态循环 关->开->关 (按当前引擎独立存储) */
        uint8_t ngray = (uint8_t)((engine_gray_get(state->select_engine) + 1) % 2);
        engine_gray_set(state->select_engine, ngray);
        state->game_gray_mode = ngray;
        board_shim_set_gb_gray((int)ngray);
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "模拟灰度[eng=%d] -> %s", state->select_engine, engine_gray_name(ngray));
        return true;
    }
    case GS_STATUSBAR:
        /* 状态栏设置: 单击立即翻转 (电子词典设置) */
        state->game_show_statusbar = !state->game_show_statusbar;
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "状态栏设置 -> %s", state->game_show_statusbar ? "显示" : "隐藏");
        return true;
    case GS_AA:
        /* 抗锯齿: 单击两态循环: 关→EPX→关 (V1.0.58+) */
        state->game_pic_opt = (uint8_t)((state->game_pic_opt + 1) % 2);
        gam4980_set_pic_opt((int)state->game_pic_opt);
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        {
            static const char *nm[] = {"关", "EPX"};
            ESP_LOGI(TAG, "BBK 抗锯齿 -> %s", nm[state->game_pic_opt]);
        }
        return true;
    case GS_DISPLAY:
        /* 显示模式: 点对点 -> 全屏 -> 拉伸 循环 (按引擎档位) */
        state->game_display_mode = (uint8_t)((state->game_display_mode + 1) %
                                             (display_mode_max(state) + 1));
        if (state->select_mode == 1)
            engine_display_set(state->select_engine, state->game_display_mode);
        else
            s_bbk_display = state->game_display_mode;
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "显示模式 -> %s", display_mode_name(state->game_display_mode));
        return true;
    case GS_VKEY:
        /* V1.0.68: 游戏内屏幕虚拟按键开关 */
        state->game_virtual_keys = !state->game_virtual_keys;
        menu_config_save();
        state->editing_index = -1;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "虚拟按键 -> %s", state->game_virtual_keys ? "开" : "关");
        return true;
    case GS_RESETMAP:
        /* V1.0.68: 还原 8 键按键映射 (上下左右/A/B/返回菜单/多功能键) */
        bt_manager_reset_key_map();
        snprintf(state->hint_text, sizeof(state->hint_text),
                 "\xe5\xb7\xb2\xe8\xbf\x98\xe5\x8e\x9f\xe6\x98\xa0\xe5\xb0\x84");  /* 已还原映射 */
        state->hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 1500;
        select_game_invalidate_cache();
        state->needs_redraw = true;
        ESP_LOGI(TAG, "已还原映射(清除)");
        return true;
    }
    return false;
}

static bool game_settings_on_lr(menu_state_t *state, int idx, bool is_right) {
    if (state->editing_index < 0 || state->editing_index != idx) return false;
    if (idx < 0 || idx >= g_gs_count) return false;
    switch (g_gs_action[idx]) {
    case GS_VOLUME: {
        /* GB/GBC 音量: 左右 ±1 档 (与系统/音乐统一) */
        int v = (int)state->settings.volume + (is_right ? 1 : -1);
        if (v < 0) v = 0;
        if (v > 10) v = 10;
        state->settings.volume = (uint8_t)v;
        audio_player_set_volume(volume_step_to_percent(v));
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->needs_redraw = true;
        return true;
    }
    case GS_STATUSBAR:
        state->game_show_statusbar = !state->game_show_statusbar;
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->needs_redraw = true;
        return true;
    case GS_AA:
        /* 抗锯齿: 左右循环切换两档: 关/EPX (V1.0.58+) */
        {
            int v = (int)state->game_pic_opt + (is_right ? 1 : -1);
            if (v < 0) v = 1;
            if (v > 1) v = 0;
            state->game_pic_opt = (uint8_t)v;
            gam4980_set_pic_opt(v);
        }
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->needs_redraw = true;
        return true;
    case GS_VKEY:
        /* V1.0.68: 虚拟按键开关 (左右循环) */
        state->game_virtual_keys = !state->game_virtual_keys;
        menu_config_save();
        state->needs_redraw = true;
        return true;
    case GS_DISPLAY:
        state->game_display_mode = (uint8_t)((state->game_display_mode + 1) %
                                             (display_mode_max(state) + 1));
        if (state->select_mode == 1)
            engine_display_set(state->select_engine, state->game_display_mode);
        else
            s_bbk_display = state->game_display_mode;
        menu_config_save();  /* 配置变更, 持久化 (TF + NVS) */
        state->needs_redraw = true;
        return true;
    }
    return false;
}


static const sub_page_def_t sub_pages[MENU_PAGE_COUNT] = {
    [MENU_PAGE_SELECT_GAME]       = { "电子词典游戏", select_game_build, select_game_on_confirm, NULL },
    /* [MENU_PAGE_GAMEPAD] 已删除: V1.0.18 重构后, 手柄配置不再走全屏子页,
     * 全部走 list_dialog 弹窗 (gamepad_list_open), 由 main_items[MENU_PAGE_GAMEPAD]
     * 在 MENU_ACTION_CONFIRM 时直接调用 gamepad_list_open. */
    [MENU_PAGE_VOLUME]            = { "音量调节",   volume_build,      volume_on_confirm,      volume_on_lr },
    [MENU_PAGE_SETTINGS]          = { "设置",       settings_build,    settings_on_confirm,    NULL },
    [MENU_PAGE_SETTINGS_TIME]     = { "时间设置",   time_build,        NULL,                   NULL },
    [MENU_PAGE_SETTINGS_SD]       = { "存储管理",   sd_build,          sd_on_confirm,          NULL },
    [MENU_PAGE_SETTINGS_INFO]     = { "系统信息",   sysinfo_build,     sysinfo_on_confirm,     NULL },
    [MENU_PAGE_FILE_BROWSER]      = { "文件浏览",   fb_build,          fb_on_confirm,          NULL },  /* 已迁移到 list_dialog 弹窗, 保留条目以兼容 */
    [MENU_PAGE_MP3_PLAYER]        = { "MP3 播放器", mp3_build,         mp3_on_confirm,         NULL },
    [MENU_PAGE_GAME_SETTINGS]     = { "游戏设置",   game_settings_build, game_settings_on_confirm, game_settings_on_lr },
    [MENU_PAGE_GB_GAME]           = { "GB 游戏",    select_game_build, select_game_on_confirm, NULL },
    [MENU_PAGE_BOOK]              = { "电子书",     NULL,              NULL,                   NULL },
};


/* ============ 初始化 ============ */

/* 蓝牙连接成功处理:
 * 用户需求 (新): 不再弹"立即映射/稍后"确认弹窗.
 * 第一次连接的新设备 -> 0.5s 后自动跳转按键映射 (见 bt_connect_callback +
 * menu_check_dialog_timeout 中的 bt_map_jump_at_ms 轮询). */

/* 蓝牙连接状态回调 (从 BT 任务线程调用, 设置 g_menu 的状态标志) */
static void bt_connect_callback(bool connected) {
    g_menu.settings.bt_connected = connected;
    g_menu.needs_redraw = true;

    /* 关闭"正在连接..."弹窗 */
    if (g_menu.confirm_active && g_menu.confirm_notice) {
        g_menu.confirm_active = false;
    }

    if (connected) {
        /* 根据连接的设备名称判断类型 */
        const char *dev_name = bt_manager_get_connected_device_name();
        if (dev_name && dev_name[0]) {
            /* 检测耳机/音箱关键字 (优先, 因为耳机也可能叫 "Audio" 等) */
            if (strstr(dev_name, "Headphone") || strstr(dev_name, "headphone") ||
                strstr(dev_name, "Headset") || strstr(dev_name, "headset") ||
                strstr(dev_name, "Earphone") || strstr(dev_name, "earphone") ||
                strstr(dev_name, "Speaker") || strstr(dev_name, "speaker") ||
                strstr(dev_name, "Audio") || strstr(dev_name, "audio") ||
                strstr(dev_name, "耳机") || strstr(dev_name, "音箱")) {
                g_menu.settings.bt_device_type = BT_DEVICE_HEADPHONE;
            } else {
                /* 游戏模拟器: 除耳机外的所有 HID 设备默认视为手柄
                 * (Q36/ShanWan 等手柄名称不含 "Gamepad" 关键字) */
                g_menu.settings.bt_device_type = BT_DEVICE_GAMEPAD;
            }
            ESP_LOGI(TAG, "蓝牙连接: %s (类型=%d)", dev_name, g_menu.settings.bt_device_type);
        } else {
            g_menu.settings.bt_device_type = BT_DEVICE_GAMEPAD;
            ESP_LOGI(TAG, "蓝牙连接: HID 设备 (默认手柄)");
        }
        /* HID 设备都视为手柄连接 */
        g_menu.settings.pad_connected = true;

        /* 用户需求(V1.0.32 简化):
         *  - 连接蓝牙时: 紧凑小弹窗"正在连接" (connecting_popup_active)
         *  - 连接成功后: 小弹窗切换为"连接成功", 800ms 后自动结束
         *  - 弹窗结束后: 首次连接的新设备 1s 后自动跳转按键映射
         *    (走空闲基线快照, 跳过 INIT, 排除按键干扰)
         *  - 老设备重连不跳转, 弹窗关闭后回主页
         *  - 按键映射完成: 8 键后自动保存并返回主页 (mapping_finish) */
        if (g_menu.settings.bt_device_type == BT_DEVICE_GAMEPAD) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            /* V1.0.32: 直接走按键映射, 去掉中间的 BT 测试模式. */
            bool first_time = bt_manager_last_connect_was_new();
            bool must_map = first_time;  /* V1.0.40: 仅首次连接的新设备才跳转按键映射 */
            /* 连接小弹窗: 切到"连接成功"状态, 800ms 后自动结束 */
            g_menu.connecting_popup_active = true;
            g_menu.connecting_popup_success = true;
            g_menu.connecting_popup_until_ms = now + 500;  /* V1.0.47: 小弹窗 0.5s 自动关闭 */
            g_menu.connecting_popup_started_at_ms = now;  /* V1.0.24: 重置看门狗起点 */
            /* 关闭旧 confirm "已连接" (若残留), 防干扰 */
            g_menu.confirm_active = false;
            g_menu.confirm_notice = false;
            g_menu.confirm_until_ms = 0;
            g_menu.hint_until_ms = 0;
            g_menu.bt_map_notice_active = false;
            /* V1.0.40: 仅首次连接的新设备 1s 后跳转按键映射弹窗, 老设备重连不跳转 */
            ESP_LOGI(TAG, "蓝牙连接成功 (首次=%d)", must_map);
            g_menu.bt_map_jump_at_ms = must_map ? (now + 1000) : 0;
            /* 清理主动连接状态 (成功了, 不再重试) */
            g_menu.bt_auto_connect_active = false;
            g_menu.bt_auto_connect_target[0] = '\0';
            g_menu.bt_auto_connect_found = 0;
            /* 重置 5 秒自动重连排期与状态 */
            g_menu.bt_retry_next_ms = 0;
            g_menu.bt_retry_direct_failed = false;
            g_menu.bt_retry_scan_active = false;
            g_menu.bt_retry_scan_until_ms = 0;
            g_menu.bt_retry_connect_pending = false;
            g_menu.bt_connect_awaiting = false;  /* 连接成功, 清除等待标记 */
        }
        /* 主动连接模式: 连接成功, 弹出"已连接 X"短暂提示 (1.2s 后自动关闭) */
        if (g_menu.bt_auto_connect_active) {
            char success_msg[64];
            snprintf(success_msg, sizeof(success_msg),
                     "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5 %s",
                     g_menu.bt_auto_connect_target[0] ? g_menu.bt_auto_connect_target : "\xe8\xae\xbe\xe5\xa4\x87");
            snprintf(g_menu.confirm_title, sizeof(g_menu.confirm_title), "主动连接");
            snprintf(g_menu.confirm_msg, sizeof(g_menu.confirm_msg), "%s", success_msg);
            g_menu.confirm_active = true;
            g_menu.confirm_notice = true;
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            g_menu.hint_until_ms = now + 1200;
            g_menu.confirm_until_ms = now + 1200;
            g_menu.bt_auto_connect_active = false;
            g_menu.bt_auto_connect_target[0] = '\0';
            g_menu.bt_auto_connect_found = 0;
            g_menu.bt_connect_awaiting = false;  /* 连接成功, 清除等待标记 */
        }
    } else {
        /* 断开连接, 重置类型 */
        g_menu.settings.bt_device_type = BT_DEVICE_NONE;
        g_menu.settings.pad_connected = false;
        g_menu.key_mapping_idx = -1;
        g_menu.sup_map_idx = -1;
        g_menu.gb_aux_prompt = false;
        g_menu.gb_map_idx = -1;
        ESP_LOGI(TAG, "蓝牙已断开");
        /* 连接失败: 关闭"正在连接"小弹窗 (无论后续是否弹失败提示) */
        g_menu.connecting_popup_active = false;
        g_menu.connecting_popup_until_ms = 0;
        g_menu.connecting_popup_started_at_ms = 0;  /* V1.0.24: 清看门狗起点 */
        /* 后台静默重连失败: 下轮改扫描, 不弹错误窗 */
        if (g_menu.bt_retry_connect_pending) {
            g_menu.bt_retry_direct_failed = true;
            g_menu.bt_retry_connect_pending = false;
            if (g_menu.bt_retry_scan_active) {
                bt_manager_stop_scan();
                g_menu.bt_retry_scan_active = false;
                g_menu.bt_retry_scan_until_ms = 0;
            }
        }
        /* 连接失败提示:
         * 自动连接模式 (bt_auto_connect_active) 与后台静默重连 (bt_retry_*) 失败时不弹
         * "连接失败" —— 这些机制会持续每 5 秒重试, 弹窗会与随后的 "已连接" 冲突
         * (用户所见: 先"连接失败"再"连接成功")。同样, 主菜单页面 (MENU_PAGE_MAIN)
         * 的静默重连一直在跑, 任何连接失败提示都会过早。仅在用户显式手动连接
         * (bt_connect_awaiting) 且不在自动重连周期 / 不在主菜单时, 才弹持久失败提示。
         * 失败的最终兜底: "正在连接" 弹窗保留 50s 超时, 由 menu_check_dialog_timeout
         * 改写为 "连接超时"。 */
        bool auto_reconnect = g_menu.bt_auto_connect_active ||
                              g_menu.bt_retry_scan_active ||
                              g_menu.bt_retry_connect_pending ||
                              (g_menu.current_page == MENU_PAGE_MAIN);
        /* V1.0.47: 手动连接失败也用紧凑小弹窗 (0.5s 自动关闭, 与"连接成功"一致) */
        if (g_menu.bt_connect_awaiting && !auto_reconnect) {
            g_menu.confirm_active = false;
            g_menu.confirm_notice = false;
            g_menu.connecting_popup_active = false;
            g_menu.connecting_popup_until_ms = 0;
            g_menu.connecting_popup_started_at_ms = 0;
            snprintf(g_menu.hint_text, sizeof(g_menu.hint_text),
                     "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5");  /* 连接失败 */
            g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 500;
            g_menu.needs_redraw = true;
        } else {
            /* 自动重连 / 主菜单场景: 关闭"正在连接"提示, 让重连在后台继续, 不打断用户
             * (避免"连接失败"闪现后又被"已连接"覆盖的误导现象) */
            g_menu.confirm_active = false;
            g_menu.confirm_until_ms = 0;
        }
        g_menu.bt_connect_awaiting = false;
        /* 不立即清空 auto_connect 状态, 保留以便后续重试 */
    }
}

/* 蓝牙连接进度回调 (从 BT 任务线程调用, 更新弹窗提示) */
static void bt_connect_progress_cb(const char *stage) {
    if (g_menu.confirm_active && g_menu.confirm_notice) {
        snprintf(g_menu.confirm_msg, sizeof(g_menu.confirm_msg),
                 "正在连接...\n%s", stage);
        g_menu.needs_redraw = true;
    }
}

/* 蓝牙连接弹窗 15 秒超时兜底:
 * 若连接既未成功也未失败 (协议栈卡死), 不会触发 bt_connect_callback,
 * 弹窗将永远挂起。此处每帧检查, 超时后把提示改为"连接超时"并等待用户手动关闭,
 * 避免用户误以为死机。 */
/* ============ 多功能键 (短按) 收藏检测 ============ */
/* KEY 按下时: BTN_GPIO_LEFT (GPIO18, 低电平=按下)
 * 手柄兼容: F_CONFIRM 按下 = KEY 按住
 * 多功能键 (F_FAV): 短按添加游戏到收藏栏
 * 不跨组件依赖: 直接调 gpio_get_level + bt_manager_is_key_pressed */
#include "driver/gpio.h"
#define FAV_COOLDOWN_MS    500     /* 触发后 0.5s 冷却, 防止重复 (提示也持续 0.5s) */
static bool is_key_held(void) {
    if (gpio_get_level(GPIO_NUM_18) == 0) return true;
    if (bt_manager_is_connected() && bt_manager_is_key_pressed(F_CONFIRM)) return true;
    return false;
}
/* 多功能键 (收藏) 是否被按住 */
static bool is_space_held(void) {
    if (bt_manager_is_connected() && bt_manager_is_key_pressed(F_FAV)) return true;
    /* 物理 KEY 按钮 (GPIO18) 按下 = 多功能键的兜底支持, 方便无手柄场景 */
    if (gpio_get_level(GPIO_NUM_18) == 0) return true;
    return false;
}

/* 获取右栏当前选中游戏的完整路径. 写入 path, 返回 true 成功. */
static bool get_current_selected_path(menu_state_t *state, char *path, int path_size) {
    int idx = state->select_game_idx;
    if (idx < 0 || idx >= g_sub_count) return false;  /* 越界 (无"返回"项) */
    if (state->select_folder_idx == 1) {
        /* 收藏: 按页面模式过滤 (与右栏列表序号一致) */
        int fav_count = 0;
        const char *const *favs = favorites_list(state_fav_engine(state), &fav_count);
        int matched = 0;
        for (int i = 0; i < fav_count; i++) {
            const char *fp = favs[i];
            if (!fp) continue;
            if (!is_page_game_file(fp, state)) continue;
            if (matched == idx) {
                strncpy(path, fp, path_size - 1);
                path[path_size - 1] = '\0';
                return true;
            }
            matched++;
        }
        return false;
    }
    /* 真实子文件夹 (按页面+平台选目录) */
    const char *folder = get_selected_folder_name(state, state->select_folder_idx);
    const char *p = (state->select_mode == 1)
        ? platform_game_path(platform_root_dir(state->select_engine), folder, idx,
                             current_gb_ext(state), path, path_size)
        : bbk_game_path_in_folder(folder, idx, path, path_size);
    return p != NULL;
}

void menu_poll_long_press(menu_state_t *state) {
    /* 壁纸游戏选择器 (游戏列表层): 多功能键 (F_FAV/KEY) 按下 = 加入/移出游戏壁纸列表 */
    if (state->list_dialog_active && state->list_dialog_on_select == wp_picker_on_select) {
        uint32_t wpnow = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (state->space_cooldown_until_ms > 0 && wpnow < state->space_cooldown_until_ms) {
            state->space_prev_held = is_space_held();
            return;
        }
        bool held = is_space_held();
        if (held && !state->space_prev_held) {
            int idx = state->list_dialog_selected;
            if (idx >= 0 && idx < s_wp_picker_count) {
                wp_picker_toggle(idx);
                wp_picker_rebuild(state);
            }
            state->space_cooldown_until_ms = wpnow + FAV_COOLDOWN_MS;
            state->needs_redraw = true;
        }
        state->space_prev_held = held;
        return;
    }
    /* 电子书: 右栏选中书籍时, 多功能键 (空格/KEY) 收藏/取消收藏 */
    if (state->current_page == MENU_PAGE_BOOK && !book_reader_is_open()) {
        if (state->select_focus != 1 || state->select_folder_idx == 0) {
            state->space_prev_held = false;
            state->space_cooldown_until_ms = 0;
            return;
        }
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (state->space_cooldown_until_ms > 0 && now < state->space_cooldown_until_ms) {
            state->space_prev_held = is_space_held();
            return;
        }
        bool cur_held = is_space_held();
        if (cur_held && !state->space_prev_held) {
            char path[256];
            if (book_get_current_path(state, path, sizeof(path))) {
                bool was_fav = favorites_contains(FAV_ENGINE_BOOK, path);
                if (was_fav) {
                    favorites_remove(FAV_ENGINE_BOOK, path);
                    snprintf(state->hint_text, sizeof(state->hint_text),
                             "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x94\xb6\xe8\x97\x8f\xe4\xb9\xa6\xe6\x9e\xb6");  /* 已取消收藏书架 */
                } else if (favorites_add(FAV_ENGINE_BOOK, path)) {
                    snprintf(state->hint_text, sizeof(state->hint_text),
                             "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f\xe4\xb9\xa6\xe6\x9e\xb6");  /* 已添加收藏书架 */
                }
                state->hint_until_ms = now + FAV_COOLDOWN_MS;
                state->space_cooldown_until_ms = now + FAV_COOLDOWN_MS;
                state->needs_redraw = true;
            }
        }
        state->space_prev_held = cur_held;
        return;
    }

    /* 电子词典/GB 分栏页面, 右栏焦点 + 选中游戏时检测 (V1.0.46: GB 页面也支持收藏) */
    if ((state->current_page != MENU_PAGE_SELECT_GAME && state->current_page != MENU_PAGE_GB_GAME)
        || state->select_focus != 1) {
        state->space_prev_held = false;
        state->space_cooldown_until_ms = 0;
        return;
    }
    /* 右栏为空时不检测 */
    if (g_sub_count <= 0) {
        state->space_prev_held = false;
        state->space_cooldown_until_ms = 0;
        return;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* 冷却中: 等待冷却结束 */
    if (state->space_cooldown_until_ms > 0 && now < state->space_cooldown_until_ms) {
        state->space_prev_held = is_space_held();
        return;
    }

    /* 多功能键 (空格) 短按 = 上升沿触发 (从未按住 -> 按住)
     * 用户需求: 按下多功能键 -> 把当前游戏添加到收藏栏, 弹 "已添加收藏栏" 提示 0.5s.
     * 物理 KEY 按钮 (GPIO18) 复用为多功能键的兜底, 也走上升沿. */
    bool cur_held = is_space_held();
    if (cur_held && !state->space_prev_held) {
        /* 上升沿: 触发收藏 (按路径推断所属引擎, 收藏分引擎独立存储) */
        char path[160];
        if (get_current_selected_path(state, path, sizeof(path))) {
            fav_engine_t fav_e = favorites_engine_for_path(path);
            bool was_fav = favorites_contains(fav_e, path);
            if (was_fav) {
                favorites_remove(fav_e, path);
                ESP_LOGW(TAG, "已取消收藏: %s", path);
                state->needs_redraw = true;
                state->space_cooldown_until_ms = now + FAV_COOLDOWN_MS;
                /* V1.0.33: 用 hint_text 弹"已取消收藏"小弹窗 (走 draw_notice_popup 模板, 3px 边框, 0.5s) */
                snprintf(state->hint_text, sizeof(state->hint_text), "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x94\xb6\xe8\x97\x8f");
                state->hint_until_ms = now + FAV_COOLDOWN_MS;
                select_game_invalidate_cache();
            } else {
                bool ok = favorites_add(fav_e, path);
                if (ok) {
                    ESP_LOGW(TAG, "已添加收藏栏: %s (总数 %d)", path, favorites_count(fav_e));
                    state->needs_redraw = true;
                    state->space_cooldown_until_ms = now + FAV_COOLDOWN_MS;
                    /* V1.0.33: 用 hint_text 弹"已添加收藏栏"小弹窗 (走 draw_notice_popup 模板, 3px 边框, 0.5s) */
                    snprintf(state->hint_text, sizeof(state->hint_text), "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f");
                    state->hint_until_ms = now + FAV_COOLDOWN_MS;
                    /* 强制失效右栏缓存, 无论当前在哪个 folder, 下次 render 都会重建 */
                    select_game_invalidate_cache();
                } else {
                    /* 添加失败 (空路径/已存在/已满), 不弹提示, 也不设冷却 */
                    ESP_LOGW(TAG, "收藏失败 (路径: %s, count=%d)", path, favorites_count(fav_e));
                }
            }
        }
    }
    state->space_prev_held = cur_held;
}

void menu_check_dialog_timeout(void) {
    extern menu_state_t g_menu;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* V1.0.46: Wi-Fi 网络列表扫描完成刷新 */
    wifi_net_poll(&g_menu);

#if WIFI_SUPPORT
    /* V1.0.46: 连接成功 → 0.5s 提示弹窗 */
    if (wifi_manager_consume_connected_event()) {
        snprintf(g_menu.hint_text, sizeof(g_menu.hint_text),
                 "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f"); /* 连接成功 */
        g_menu.hint_until_ms = now + 500;
        g_menu.needs_redraw = true;
    }
#endif

    /* === 首次连接的新设备: 自动跳转按键映射(无需按确认) ===
     * 用户洞察: "按确认键跳转"本身就是首键(上)被错捕的根源——按键瞬间手柄
     * 报告里 A 键可能被锁进基线(INIT 等待松手的窗口里)。改为: 用户不按任何
     * 键, 走 from_key=false + 空闲基线快照路径(连接瞬间手柄真空闲时种下的快照
     * 直接做基线, 跳过 INIT 等待), 从根本上排除按键干扰。
     *
     * 这里等"空闲基线快照就绪"才真正跳转(最多等 2s), 避免基线未稳定时
     * 跳进去走 INIT 兜底; 万一超过 2s 仍未就绪(控制器极慢/一直抖), 也兜底
     * 跳转(走 INIT, 至少能进映射)。 */
    if (g_menu.bt_map_jump_at_ms > 0 && now >= g_menu.bt_map_jump_at_ms) {
        g_menu.bt_map_jump_at_ms = 0;
        if (bt_manager_is_connected()) {
            /* V1.0.40: 首次连接成功后跳转按键映射弹窗 */
            ESP_LOGI(TAG, "首次连接: 跳转按键映射弹窗");
            g_menu.confirm_active = false;
            g_menu.confirm_until_ms = 0;
            g_menu.connecting_popup_active = false;
            g_menu.connecting_popup_until_ms = 0;
            g_menu.connecting_popup_started_at_ms = 0;
            g_menu.current_page = MENU_PAGE_MAIN;
            g_menu.selected_index = g_menu.main_selected_index;
            g_menu.scroll_offset = 0;
            gamepad_act_keymap(&g_menu);
            g_menu.needs_redraw = true;
        }
    }

    /* 连接小弹窗自动关闭: 成功态("连接成功")到点自动结束, 防干扰 */
    if (g_menu.connecting_popup_active && g_menu.connecting_popup_until_ms > 0 &&
        now >= g_menu.connecting_popup_until_ms) {
        g_menu.connecting_popup_active = false;
        g_menu.connecting_popup_until_ms = 0;
        g_menu.connecting_popup_started_at_ms = 0;
        g_menu.needs_redraw = true;
    }

    /* V1.0.40: 连接弹窗看门狗 — 5 秒硬超时, 不再宽限循环.
     * 正常 3 秒内连上, 超过 5 秒视为失败, 强制取消连接并清理状态. */
    if (g_menu.connecting_popup_active && !g_menu.connecting_popup_success &&
        g_menu.connecting_popup_started_at_ms > 0) {
        uint32_t elapsed = now - g_menu.connecting_popup_started_at_ms;
        bool already_connected = bt_manager_is_connected();
        if (already_connected) {
            ESP_LOGW(TAG, "[看门狗] 弹窗仍显示但设备已连接, 强制关闭");
            g_menu.connecting_popup_active = false;
            g_menu.connecting_popup_until_ms = 0;
            g_menu.connecting_popup_started_at_ms = 0;
            g_menu.needs_redraw = true;
        } else if (elapsed > 5000) {
            /* 5 秒超时: 取消连接任务, 清理所有连接状态, 提示失败 */
            ESP_LOGW(TAG, "[5s 超时] 连接弹窗显示 %u ms, 强制取消并清理", (unsigned)elapsed);
            if (bt_manager_is_connecting()) bt_manager_cancel_connect();
            g_menu.bt_connect_awaiting = false;
            g_menu.bt_auto_connect_active = false;
            g_menu.bt_auto_connect_target[0] = '\0';
            g_menu.bt_auto_connect_found = 0;
            g_menu.bt_retry_connect_pending = false;
            g_menu.bt_retry_direct_failed = false;
            g_menu.bt_retry_scan_active = false;
            g_menu.bt_retry_scan_until_ms = 0;
            g_menu.connecting_popup_active = false;
            g_menu.connecting_popup_until_ms = 0;
            g_menu.connecting_popup_started_at_ms = 0;
            /* V1.0.47: 连接超时也用紧凑小弹窗 (0.5s 自动关闭) */
            g_menu.confirm_active = false;
            g_menu.confirm_notice = false;
            snprintf(g_menu.hint_text, sizeof(g_menu.hint_text),
                     "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6");  /* 连接超时 */
            g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 500;
            g_menu.needs_redraw = true;
        }
    }

    if (g_menu.confirm_until_ms == 0 || !g_menu.confirm_active) return;
    if (now < g_menu.confirm_until_ms) return;

    /* V1.0.40: 5 秒硬超时. 之前都是 3 秒内连上, 超过 5 秒一定有问题.
     * 不再宽限循环, 直接取消连接并提示失败. */
    if (bt_manager_is_connected()) {
        g_menu.confirm_active = false;
        g_menu.confirm_notice = false;
        g_menu.confirm_until_ms = 0;
        g_menu.needs_redraw = true;
        ESP_LOGI(TAG, "连接已成功, 关闭弹窗");
        return;
    }
    /* 超时: 取消连接任务, 清理状态, 提示失败 */
    ESP_LOGW(TAG, "[5s 超时] 蓝牙连接超时, 强制取消并清理");
    if (bt_manager_is_connecting()) bt_manager_cancel_connect();
    g_menu.bt_connect_awaiting = false;
    g_menu.bt_auto_connect_active = false;
    g_menu.bt_auto_connect_target[0] = '\0';
    g_menu.bt_auto_connect_found = 0;
    g_menu.bt_retry_connect_pending = false;
    g_menu.bt_retry_direct_failed = false;
    g_menu.bt_retry_scan_active = false;
    g_menu.bt_retry_scan_until_ms = 0;
    g_menu.connecting_popup_active = false;
    g_menu.connecting_popup_until_ms = 0;
    g_menu.connecting_popup_started_at_ms = 0;
    /* V1.0.47: 连接超时也用紧凑小弹窗 (0.5s 自动关闭) */
    g_menu.confirm_active = false;
    g_menu.confirm_notice = false;
    snprintf(g_menu.hint_text, sizeof(g_menu.hint_text),
             "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6");  /* 连接超时 */
    g_menu.hint_until_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 500;
    g_menu.needs_redraw = true;
}

/* 后台轮询: 蓝牙开/重启后, 等 HID Host 就绪, 自动启动"主动连接"扫描
 * 用户场景: 设备不重启, 手柄断电后十几分钟再开, 设备原配置不主动扫,
 *           需手动点"搜索主动连接设备"才能连上. 这里在蓝牙就绪后自动触发,
 *           只要在范围内就会自动连上, 不在范围则扫描几秒后由用户决定.
 * 实现: 在 bt_manager_enable() 后置 bt_auto_connect_on_enable=true,
 *       每帧检查 HID Host 就绪+已配对+未连接, 满足则启动主动连接模式. */
extern int bt_manager_get_history_count(void);
extern const bt_device_t *bt_manager_get_history_at(int index);
extern bool bt_manager_is_ready(void);

/* === 主菜单后台自动重连 ===
 * 用户需求: 主菜单页面任何状态下, 只要没有连接到设备, 自动搜索历史连接记录,
 * 每 5 秒尝试连接一次 (静默: 不弹"正在连接"窗, 失败也不提示; 连上后由
 * bt_connect_callback 弹 1.2s "已连接" 提示)。
 * 多条历史记录时按最近使用顺序轮流尝试。 */
#define BT_RETRY_INTERVAL_MS 5000
#define BT_RETRY_SCAN_DURATION_MS 4000
static void menu_poll_bt_history_retry(menu_state_t *state) {
    if (!state->settings.bt_enabled) return;
    if (bt_manager_is_suspended()) return;   /* 屏保挂起期间不自动重连 */
    /* 用户需求: 所有界面都尝试连接 (不再限制主菜单页面).
     * 弹窗激活时不干扰 (避免扫描/连接中断用户操作). */
    if (state->bt_scan_active) return;                   /* 正在扫描/添加设备, 不干扰 */
    if (state->list_dialog_active) return;               /* 弹窗激活时不干扰 */
    if (state->confirm_active) return;                   /* 确认弹窗激活时不干扰 */
    if (!bt_manager_is_ready()) return;
    if (bt_manager_is_connected() || bt_manager_is_connecting()) return;
    int hn = bt_manager_get_history_count();
    if (hn <= 0) return;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* 静默扫描进行中: 等待匹配或超时 */
    if (state->bt_retry_scan_active) {
        if (now >= state->bt_retry_scan_until_ms) {
            ESP_LOGI(TAG, "后台静默扫描超时, 下轮改直连");
            bt_manager_stop_scan();
            state->bt_retry_scan_active = false;
            state->bt_retry_scan_until_ms = 0;
            state->bt_retry_direct_failed = false;
            state->bt_retry_next_ms = now + BT_RETRY_INTERVAL_MS;
        }
        return;
    }

    if (state->bt_retry_next_ms == 0) {
        /* 首次排期: 5 秒后开始第一次尝试 */
        state->bt_retry_next_ms = now + BT_RETRY_INTERVAL_MS;
        return;
    }
    if (now < state->bt_retry_next_ms) return;
    state->bt_retry_next_ms = now + BT_RETRY_INTERVAL_MS;

    if (state->bt_retry_hist_idx >= hn) state->bt_retry_hist_idx = 0;
    const bt_device_t *dev = bt_manager_get_history_at(state->bt_retry_hist_idx);
    state->bt_retry_hist_idx = (state->bt_retry_hist_idx + 1) % hn;
    if (!dev) return;

    /* V1.0.60: 一律走"扫描到设备再连接", 不再盲连历史 MAC.
     * 手柄休眠/关机后对不可达地址 esp_hidh_dev_open 失败会把 Bluedroid/HID
     * 协议栈搞崩 (LoadProhibited) -> 开机约 5 分钟自动重启循环的根因.
     * 扫描确认设备在线后再连, open 正常成功, 不会触发该崩溃. */
    ESP_LOGI(TAG, "后台自动重连(扫描): 搜索历史设备...");
    state->bt_retry_scan_active = true;
    state->bt_retry_scan_until_ms = now + BT_RETRY_SCAN_DURATION_MS;
    bt_manager_start_scan_continuous(bt_device_found);
}

void menu_poll_bt_auto_connect(menu_state_t *state) {
    if (!state) return;
    /* 主菜单未连接: 每 5 秒静默尝试连接历史设备 (与开机主动连接扫描互不影响) */
    if (!state->bt_auto_connect_active) {
        menu_poll_bt_history_retry(state);
    }
    if (!state->bt_auto_connect_on_enable) return;
    if (!state->settings.bt_enabled) {
        state->bt_auto_connect_on_enable = false;  /* 蓝牙关了, 不再触发 */
        return;
    }
    /* 等待 HID Host 就绪 */
    if (!bt_manager_is_ready()) return;
    /* 已经有设备连上了, 不需要主动连接 */
    if (bt_manager_is_connected()) {
        state->bt_auto_connect_on_enable = false;
        return;
    }
    /* 没有连接记录, 无法主动连接 */
    if (bt_manager_get_history_count() <= 0) {
        state->bt_auto_connect_on_enable = false;
        return;
    }
    /* 已经在扫描了 (用户可能手动点了"搜索"), 跳过 */
    if (state->bt_scan_active) {
        state->bt_auto_connect_on_enable = false;
        return;
    }
    /* 启动主动连接模式 */
    ESP_LOGI(TAG, "蓝牙就绪且已配对, 自动启动主动连接扫描");
    state->bt_auto_connect_active = true;
    state->bt_auto_connect_found = 0;
    state->bt_auto_connect_target[0] = '\0';
    state->bt_device_count = 0;
    state->bt_scan_active = true;
    state->selected_index = 0;
    state->scroll_offset = 0;
    bt_manager_start_scan_continuous(bt_device_found);
    state->bt_auto_connect_on_enable = false;  /* 触发一次后清零, 避免重复 */
    state->needs_redraw = true;
}

void menu_init(menu_state_t *state, st7305_handle_t *lcd) {
    memset(state, 0, sizeof(*state));
    state->lcd = lcd;
    /* GB 模拟器渲染走 board_shim 适配层 (board_rlcd_draw_gb_line_2x 使用 s_lcd),
     * 必须设置 st7305 句柄, 否则 gb_emu_start 报 "RLCD is not initialized" */
    board_shim_set_lcd(lcd);
    state->current_page = MENU_PAGE_MAIN;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    state->editing_index = -1;
    /* 显示设置默认值 (已移除对比度/反色 UI 选项) */
    state->settings.low_power = false;
    state->settings.volume = 10;  /* V1.0.50: 默认 10 档 = 100% */
    state->settings.mute = false;
    state->settings.audio_disable = false;  /* V1.0.68: 禁用音频默认关 */
    state->settings.audio_scheme = 0;       /* V1.0.68: 音频方案默认解码输出 */
    state->settings.touch_disable = false;  /* V1.0.68: 触摸默认启用 */
    state->settings.bt_connected = false;
    state->settings.pad_connected = false;
    /* 蓝牙默认开启 (后台初始化), WiFi 默认关闭 (省电) */
    state->settings.bt_enabled = true;
    state->settings.wifi_enabled = false;
    state->settings.battery = 100;
    state->settings.game_status_bar = true; /* 默认显示游戏状态栏 */
    state->key_mapping_idx = -1;
    state->sup_map_idx = -1;
    state->gb_aux_prompt = false;
    state->gb_map_idx = -1;
    /* 电子词典: 左右分栏布局默认状态.
     * 侧栏索引: 0=游戏设置, 1=收藏 (默认选中), 2..N+1=真实子文件夹 (idx-2). */
    state->select_focus = 1;
    state->select_folder_idx = 1;  /* 默认进入"收藏" */
    state->select_folder_scroll = 0;
    state->select_game_idx = 0;
    state->select_game_scroll = 0;
    state->select_loaded = false;  /* 进入页面时再扫描子文件夹 */
    /* 游戏设置默认: 全屏(1), 状态栏显示开, BBK 按键音效开 */
    state->game_display_mode = 1;
    state->game_key_sound = true;
    state->game_show_statusbar = true;
    /* V1.0.46+: GB 默认 4级点聚灰度 (1); 画面优化 (圆角平滑) 默认关闭 */
    state->game_gray_mode = 1;
    state->game_pic_opt = 1;  /* BBK 抗锯齿默认开启 (V1.0.53) */
    state->game_virtual_keys = false;  /* V1.0.68: 虚拟按键默认关闭 */
    /* 电子书设置默认: 敲击翻页开, 灵敏度中, 夜间模式关, 显示页码开 */
    state->book_loaded = false;
    state->book_knock = false;   /* 暂时停用敲击翻页 */
    state->book_sens = 1;
    state->book_night = false;
    state->book_pagenum = false;   /* 默认不显示右下角页码 */
    state->book_rot = 0;   /* 默认: 上 (0°, 与主菜单一致) */
    state->book_fontsize = 1;   /* 中字号 24 */
    state->book_font_family = 0; /* 黑体 */
    state->book_margin = 1;     /* 中边距 */
    state->book_lineh = 1;      /* 标准行高 */
    state->book_gap = 0;        /* 标准字距 */
    /* 壁纸设置默认: 内置星空, 休眠 3 分钟 */
    state->wallpaper_mode = 0;
    state->wallpaper_program = 0;
    state->wallpaper_timeout_min = 3;
    state->wallpaper_bmp_fps = 1;
    state->wallpaper_game_rot = 0;
    state->pomo_work_min = 25;
    state->pomo_rest_min = 5;
    state->pomo_reminder = true;
    /* 弹窗局部刷新: 初始化为 -1 强制首次全量绘制 */
    state->list_dialog_prev_selected = -1;
    state->list_dialog_prev_active = false;
    state->list_dialog_local_update = false;
    /* 连接记录: 默认关闭, 由手柄弹窗"连接记录"项激活 */
    state->bt_history_active = false;
    state->bt_history_idx = 0;
    state->bt_history_scroll = 0;
    /* 弹窗方向键 / 关闭后回调: 默认 NULL (走通用处理) */
    state->list_dialog_on_key = NULL;
    state->list_dialog_on_close = NULL;
    /* 弹窗自定义渲染 / 内容脏: 默认 NULL/false (走通用行渲染) */
    state->list_dialog_on_render = NULL;
    state->list_dialog_content_dirty = false;
    /* 初始化收藏模块 (从 TF 卡加载) */
    favorites_init();
    /* 配置持久化: 进入桌面先读 TF 配置 -> 系统 NVS -> 默认 (三级回退),
     * 仅写入 settings, 真正的硬件同步由 main.c 在 audio_player_init 完成后
     * 调用 menu_apply_volume_setting() 完成. 之后默认再存一份到 TF 卡. */
    menu_config_load();
    menu_config_save();
    /* V1.0.68: 开机按音频方案初始化方波直驱 (PWM), 并应用触摸禁用设置 */
    if (state->settings.audio_scheme == 1) {
        tone_player_init();
    }
    if (state->settings.touch_disable) {
        touch_panel_deinit();
        ESP_LOGI(TAG, "开机: 触摸屏已禁用 (配置)");
    }
    /* === 彩蛋: 恢复隐藏游戏菜单显示状态 (默认隐藏) === */
    state->sponsor_confirm_count = 0;
    state->sponsor_notice_active = false;
    state->show_hidden_menus = false;
    {
        nvs_handle_t h;
        if (nvs_open("menu_settings", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "show_hidden", &v) == ESP_OK)
                state->show_hidden_menus = (v != 0);
            nvs_close(h);
        }
        if (state->show_hidden_menus)
            ESP_LOGW(TAG, "彩蛋: 上次已解锁隐藏游戏菜单 (测试功能, 可能闪退)");
    }
    /* V1.0.46: 恢复 NVS 保存的时间; 无记录且时间无效时默认 2026-08-01 */
    time_load_from_nvs();
    /* 时间: 从 RTC 读取 */
    read_time_from_rtc(&state->settings);
    /* 屏保: 默认星空动画, 3分钟超时, 默认开启 (无 UI 选项, 硬编码) */
    state->settings.screensaver_type = SCREENSAVER_STARS;
    screensaver_reset();

    /* 用户需求: 图标全部内置, 不从 SD 卡加载主题图标 */

    /* 注册蓝牙连接状态回调 (用于自动更新状态栏图标) */
    bt_manager_set_connect_callback(bt_connect_callback);
    /* 注册蓝牙连接进度回调 (用于显示连接阶段) */
    bt_manager_set_connect_progress_cb(bt_connect_progress_cb);
}

/* ============ 渲染 ============ */

/* PSP XMB 主菜单渲染 */



static void render_mp3_player(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 确保文件列表已扫描 (render_mp3_player 跳过 render_sub, mp3_build 不会被调用) */
    mp3_scan_files();

    /* 状态栏 - 中间显示二级菜单名称 "MP3播放器" (上移到这里) */
    menu_draw_status_bar(lcd, &state->settings, "MP3 \xe6\x92\xad\xe6\x94\xbe\xe5\x99\xa8");

    audio_state_t astate = audio_player_get_state();
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int progress = audio_player_get_progress();

    /* 未播放时用 state->selected_index 高亮, 播放时用 mp3_current */
    int highlight_idx = mp3_in_player ? mp3_current : state->selected_index;
    if (highlight_idx < 0) highlight_idx = 0;

    /* === 左菜单: 播放列表 (使用中文字体, 24x24每行) === */
    int list_x = 0;
    int list_w = 195;
    int list_y = 28;
    int list_h = SCREEN_H - list_y - 5;
    int line_h = 26;
    int max_visible = list_h / line_h;

    (void)list_x; (void)list_w; (void)list_h;

    /* 计算滚动偏移, 确保高亮歌曲可见 */
    int scroll = 0;
    if (highlight_idx >= max_visible - 1) {
        scroll = highlight_idx - max_visible + 2;
    }
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < mp3_count && i < max_visible - 1; i++) {
        int idx = i + scroll;
        if (idx >= mp3_count) break;
        int yy = list_y + 2 + i * line_h;
        const char *name = mp3_files[idx];
        /* 截断文件名(去掉.mp3) */
        char disp[64];
        int nl = strlen(name);
        if (nl > 4 && name[nl-4] == '.') nl -= 4;
        if (nl > 30) nl = 30;
        strncpy(disp, name, nl);
        disp[nl] = '\0';

        int list_content_w = list_w - 8;

        if (idx == highlight_idx) {
            /* 当前歌曲: 【】选中标记 */
            int cur_tw = text_width(disp);
            int tx = list_x + (list_w - cur_tw) / 2;
            if (tx < list_x + 2) tx = list_x + 2;
            int glyph_h = 24;
            int glyph_w = 10;
            int top = yy;
            int bot = yy + glyph_h - 1;
            int left_x = tx - glyph_w - 4;
            int right_x = tx + cur_tw + 4;

            /* 左【 - 2px 粗 */
            for (int d = 0; d < 2; d++) {
                int bx = left_x + d;
                for (int yy2 = top; yy2 <= bot; yy2++)
                    if (bx >= 0) st7305_draw_pixel(lcd, bx, yy2, ST7305_COLOR_BLACK);
                for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++)
                    st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++)
                    st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
            }
            /* 右】 - 2px 粗 */
            for (int d = 0; d < 2; d++) {
                int bx = right_x + glyph_w - 1 - d;
                for (int yy2 = top; yy2 <= bot; yy2++)
                    if (bx < SCREEN_W) st7305_draw_pixel(lcd, bx, yy2, ST7305_COLOR_BLACK);
                for (int xx = bx - 6; xx <= bx; xx++)
                    if (xx >= 0) st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                for (int xx = bx - 6; xx <= bx; xx++)
                    if (xx >= 0) st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
            }
            draw_text(lcd, tx, yy + 2, disp, false);
        } else {
            /* 未播放歌曲: 纯文字居中 */
            int avail = list_content_w - 4;
            char truncated[16];
            int pos = 0, tw = 0;
            for (int si = 0; disp[si] && tw + 16 <= avail && pos < 15; ) {
                uint8_t c = (uint8_t)disp[si];
                if (c < 0x80) { tw += 16; truncated[pos++] = disp[si]; si++; }
                else if ((c & 0xF0) == 0xE0) { tw += 24; if (tw <= avail) { truncated[pos++] = disp[si++]; truncated[pos++] = disp[si++]; truncated[pos++] = disp[si++]; } else break; }
                else { si++; }
            }
            truncated[pos] = '\0';
            int item_text_w = text_width(truncated);
            int item_text_x = list_x + (list_w - item_text_w) / 2;
            draw_text(lcd, item_text_x, yy + 2, truncated, false);
        }
    }

    /* 无文件时显示提示 */
    if (mp3_count == 0) {
        draw_text_centered(lcd, 100, "未找到 MP3 文件", false);
        draw_text_centered(lcd, 130, "请放入 /sdcard/mp3", false);
    }

    /* === 右侧: 光盘 + 进度条 === */
    int right_x = list_x + list_w + 25;
    int right_w = SCREEN_W - right_x - 8;

    /* 旋转光盘 */
    int cd_cx = right_x + right_w / 2 - 10;
    int cd_cy = 130;
    int cd_r = 92;
    int angle = 0;
    if (astate == AUDIO_STATE_PLAYING) {
        angle = (int)((float)now_ms / 30.0f) % 360;
    } else if (astate == AUDIO_STATE_PAUSED) {
        angle = state->last_cd_angle;
    } else {
        /* STOPPED/IDLE: 角度归零 + 使用时间慢速旋转以示静止 */
        angle = (int)((float)now_ms / 300.0f) % 360;
    }
    state->last_cd_angle = angle;

    /* 绘制光盘 */
    draw_cd(lcd, cd_cx, cd_cy, cd_r, angle);

    /* === 进度条 (全屏宽, 实心填充) === */
    int bar_pad_bottom = 10;
    int bar_h = 8;
    int bar_y = SCREEN_H - bar_pad_bottom - bar_h;
    int bar_x0 = 15;
    int bar_x1 = SCREEN_W - 15;
    int bar_w = bar_x1 - bar_x0 + 1;
    int played_w = bar_w * progress / 1000;

    /* 已播放部分: 黑色填充 */
    fill_rect(lcd, bar_x0, bar_y, bar_x0 + played_w - 1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    /* 未播放部分: 白色填充 */
    fill_rect(lcd, bar_x0 + played_w, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_WHITE);
    /* 进度条外框 (1px 黑边) */
    fill_rect(lcd, bar_x0, bar_y, bar_x1, bar_y, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y + bar_h - 1, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y, bar_x0, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x1, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);

    /* 日志: 每30帧打印一次渲染统计 */
    static int render_cnt = 0;
    render_cnt++;
    if (render_cnt % 30 == 0) {
        ESP_LOGI(TAG, "MP3_RENDER: angle=%d progress=%d state=%d",
                 angle, progress, astate);
    }

    /* 所有状态下都持续刷新, 保证 CD 旋转 */
    state->needs_redraw = true;

    /* 刷新由 menu_render 末尾统一处理 */
}

static void render_sub(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    const sub_page_def_t *page = &sub_pages[state->current_page];
    if (!page->title) {
        ESP_LOGW(TAG, "render_sub: current_page=%d has NULL title, bail out", state->current_page);
        return;
    }

    g_sub_count = page->build_items ? page->build_items(state, g_sub_items, sizeof(g_sub_items) / sizeof(g_sub_items[0])) : 0;
    static int s_render_sub_count = 0;
    /* 诊断: 进入游戏设置或设置子项数为 0 时每次都打印, 否则每 30 帧打印一次 */
    bool force_log = (g_sub_count == 0) || (state->current_page == MENU_PAGE_GAME_SETTINGS);
    if (s_render_sub_count % 30 == 0 || force_log) {
        ESP_LOGI(TAG, "render_sub: page=%d title='%s' g_sub_count=%d build_items=%p",
                 state->current_page, page->title ? page->title : "(null)", g_sub_count,
                 (void *)(page->build_items));
    }
    s_render_sub_count++;

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 状态栏 - 中间显示二级菜单名称 (上移到这里) */
    menu_draw_status_bar(lcd, &state->settings, page->title);

    /* 提示信息 (如果有, 仍在有效期内) */
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int start_y = 30;  /* 列表起点, 紧贴状态栏下方 */

    if (state->hint_text[0] != '\0' && now < state->hint_until_ms) {
        draw_text_centered(lcd, 32, state->hint_text, false);
        start_y = 60;
    }

    /* 列表项 - 24px 字体, 行高 32 */
    int line_h = 32;
    int max_visible = (SCREEN_H - start_y - 8) / line_h;
    if (max_visible < 1) max_visible = 1;

    if (state->selected_index < state->scroll_offset) {
        state->scroll_offset = state->selected_index;
    } else if (state->selected_index >= state->scroll_offset + max_visible) {
        state->scroll_offset = state->selected_index - max_visible + 1;
    }
    if (state->scroll_offset < 0) state->scroll_offset = 0;

    int editing = state->editing_index;
    /* === 特定页面不含"返回"项: 游戏设置 (4 个子项都是有效设置, 没有"返回") ===
     * 此处扩展: 需要时可在此添加更多页面. */
    bool no_return = (state->current_page == MENU_PAGE_GAME_SETTINGS);
    for (int i = 0; i < max_visible && i + state->scroll_offset < g_sub_count; i++) {
        int item_idx = i + state->scroll_offset;
        int y = start_y + i * line_h;
        bool is_selected = (item_idx == state->selected_index);
        bool is_editing = (item_idx == editing);
        bool is_return = !no_return && (item_idx == g_sub_count - 1);

        char buf[80];
        if (is_return) {
            snprintf(buf, sizeof(buf), "\xe8\xbf\x94\xe5\x9b\x9e"); /* 返回 */
        } else {
            snprintf(buf, sizeof(buf), "%s", g_sub_items[item_idx]);
        }

        /* 居中显示列表项 */
        int tw = text_width(buf);
        int tx = (SCREEN_W - tw) / 2;
        if (tx < 10) tx = 10;

        /* 选中项: 像素绘制图标 + 文字
         *  - 普通选中: 【】方括号
         *  - 编辑/确认状态: ▶ ◀ 三角箭头 */
        if (is_selected) {
            int tw = text_width(buf);
            int tx = (SCREEN_W - tw) / 2;
            int glyph_h = 24;
            int glyph_w = 10;
            int top = y;
            int bot = y + glyph_h - 1;
            int mid = y + glyph_h / 2;
            int left_x = tx - glyph_w - 4;
            int right_x = tx + tw + 4;

            if (is_editing) {
                /* === 编辑状态: ▶ ◀ 三角箭头 === */
                int arrow_w = 10;
                int arrow_h = 14;
                int arrow_top = y + (glyph_h - arrow_h) / 2;
                int arrow_bot = arrow_top + arrow_h - 1;
                int arrow_mid = arrow_top + arrow_h / 2;

                /* 左 ▶ (右向三角) */
                int lx = left_x;
                for (int yy = 0; yy < arrow_h; yy++) {
                    int yy_abs = arrow_top + yy;
                    int dist_from_mid = abs(yy - arrow_h / 2);
                    int line_w = arrow_w - dist_from_mid * 2;
                    if (line_w < 1) line_w = 1;
                    for (int xx = 0; xx < line_w; xx++) {
                        int px = lx + xx;
                        if (px >= 0 && px < SCREEN_W && yy_abs >= 0)
                            st7305_draw_pixel(lcd, px, yy_abs, ST7305_COLOR_BLACK);
                    }
                }

                /* 右 ◀ (左向三角) */
                int rx = right_x + glyph_w - 1;
                for (int yy = 0; yy < arrow_h; yy++) {
                    int yy_abs = arrow_top + yy;
                    int dist_from_mid = abs(yy - arrow_h / 2);
                    int line_w = arrow_w - dist_from_mid * 2;
                    if (line_w < 1) line_w = 1;
                    for (int xx = 0; xx < line_w; xx++) {
                        int px = rx - xx;
                        if (px >= 0 && px < SCREEN_W && yy_abs >= 0)
                            st7305_draw_pixel(lcd, px, yy_abs, ST7305_COLOR_BLACK);
                    }
                }
            } else {
                /* === 普通选中: 【】方括号 - 2px 粗 === */
                /* 左【 */
                for (int d = 0; d < 2; d++) {
                    int bx = left_x + d;
                    /* 竖线 */
                    for (int yy = top; yy <= bot; yy++) {
                        if (bx >= 0)
                            st7305_draw_pixel(lcd, bx, yy, ST7305_COLOR_BLACK);
                    }
                    /* 上横线 (向右 6px) */
                    for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++) {
                        st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                    }
                    /* 下横线 (向右 6px) */
                    for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++) {
                        st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
                    }
                }

                /* 右】 */
                for (int d = 0; d < 2; d++) {
                    int bx = right_x + glyph_w - 1 - d;
                    /* 竖线 */
                    for (int yy = top; yy <= bot; yy++) {
                        if (bx < SCREEN_W)
                            st7305_draw_pixel(lcd, bx, yy, ST7305_COLOR_BLACK);
                    }
                    /* 上横线 (向左 6px) */
                    for (int xx = bx - 6; xx <= bx; xx++) {
                        if (xx >= 0)
                            st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                    }
                    /* 下横线 (向左 6px) */
                    for (int xx = bx - 6; xx <= bx; xx++) {
                        if (xx >= 0)
                            st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
                    }
                }
            }

            draw_text(lcd, tx, y, buf, false);
        } else {
            draw_text(lcd, tx, y, buf, false);
        }
    }

    if (state->hint_text[0] != '\0' && now >= state->hint_until_ms) {
        state->hint_text[0] = '\0';
    }

    /* 刷新由 menu_render 末尾统一处理 */
}

/* ============ 游戏列表卡片式渲染 ============ */

/* 渲染电子词典游戏列表为卡片形式: 左侧图标 + 右侧名称 */
/* 旧的 render_game_cards (单卡片列表) 已被 render_select_game_two_cols (左右分栏) 替代
 * 现保留旧实现作为备份注释, 防止需要回退时找不到代码
 * 实际渲染流程由 menu_render 中的 else if (MENU_PAGE_SELECT_GAME) 分支调用新函数 */
#if 0
static void render_game_cards(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 状态栏 */
    menu_draw_status_bar(lcd, &state->settings, NULL);

    /* 标题 */
    draw_text_centered(lcd, 34, "电子词典游戏", false);
    draw_hline(lcd, 40, SCREEN_W - 40, 62, ST7305_COLOR_BLACK);

    /* 构建项目列表 (设置 g_sub_count) */
    const sub_page_def_t *page = &sub_pages[MENU_PAGE_SELECT_GAME];
    g_sub_count = page->build_items ? page->build_items(state, g_sub_items, sizeof(g_sub_items) / sizeof(g_sub_items[0])) : 0;

    int total = g_sub_count;
    int sel = state->selected_index;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;

    /* 卡片布局: 每张卡片高 36px, 从 y=70 开始
     * 一屏最多显示 6 张卡片 (36*6=216, 70+216=286) */
    int card_h = 36;
    int card_w = SCREEN_W - 40;  /* 360, 左右各留 20px */
    int card_x = 20;
    int start_y = 70;
    int max_visible = 6;

    /* 滚动逻辑 */
    if (sel < state->scroll_offset) state->scroll_offset = sel;
    if (sel >= state->scroll_offset + max_visible)
        state->scroll_offset = sel - max_visible + 1;
    int scroll = state->scroll_offset;

    /* 绘制可见卡片 */
    for (int i = 0; i < max_visible && (scroll + i) < total; i++) {
        int idx = scroll + i;
        int y = start_y + i * card_h;
        bool is_selected = (idx == sel);
        bool is_return = (idx == total - 1);

        /* 卡片背景: 选中项反色(黑底白字), 非选中白底 */
        if (is_selected) {
            fill_rect(lcd, card_x, y, card_x + card_w - 1, y + card_h - 2, ST7305_COLOR_BLACK);
        } else {
            /* 非选中项画边框 */
            draw_rect_outline(lcd, card_x, y, card_x + card_w - 1, y + card_h - 2, ST7305_COLOR_BLACK);
        }

        int icon_cx = card_x + 22;
        int icon_cy = y + card_h / 2;

        if (is_return) {
            /* 返回项: 画箭头图标 */
            st7305_color_t c = is_selected ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
            /* 左箭头 */
            for (int dy = -6; dy <= 6; dy++) {
                st7305_draw_pixel(lcd, icon_cx - 6 + abs(dy), icon_cy + dy, c);
            }
            draw_hline(lcd, icon_cx - 6, icon_cx + 6, icon_cy, c);
            /* 文字: "返回" */
            int tw = text_width("返回");
            draw_text(lcd, card_x + (card_w - tw) / 2, y + 8, "返回", is_selected);
        } else {
            /* 游戏项: 画游戏机图标 (简化: 方框+十字按键) */
            st7305_color_t c = is_selected ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
            int ico_x = icon_cx - 12;
            int ico_y = icon_cy - 10;
            /* 游戏机外框 */
            draw_rect_outline(lcd, ico_x, ico_y, ico_x + 24, ico_y + 20, c);
            /* 十字按键 */
            draw_hline(lcd, ico_x + 4, ico_x + 10, ico_y + 10, c);
            draw_vline(lcd, ico_x + 7, ico_y + 7, ico_y + 13, c);
            /* 两个按钮 */
            st7305_draw_pixel(lcd, ico_x + 16, ico_y + 8, c);
            st7305_draw_pixel(lcd, ico_x + 16, ico_y + 9, c);
            st7305_draw_pixel(lcd, ico_x + 17, ico_y + 8, c);
            st7305_draw_pixel(lcd, ico_x + 17, ico_y + 9, c);
            st7305_draw_pixel(lcd, ico_x + 19, ico_y + 11, c);
            st7305_draw_pixel(lcd, ico_x + 20, ico_y + 11, c);
            st7305_draw_pixel(lcd, ico_x + 19, ico_y + 12, c);
            st7305_draw_pixel(lcd, ico_x + 20, ico_y + 12, c);

            /* 游戏名称 (居中在卡片右侧区域) */
            if (idx < g_sub_count - 1) {
                int tw = text_width(g_sub_items[idx]);
                int name_x = card_x + 50 + (card_w - 50 - tw) / 2;
                draw_text(lcd, name_x, y + 8, g_sub_items[idx], is_selected);
            }
        }
    }

    /* 滚动指示器 */
    if (total > max_visible) {
        /* 右侧滚动条 */
        int bar_x = SCREEN_W - 6;
        int bar_y_start = start_y;
        int bar_y_end = start_y + max_visible * card_h - 2;
        draw_vline(lcd, bar_x, bar_y_start, bar_y_end, ST7305_COLOR_BLACK);
        /* 指示当前位置 */
        int thumb_y = bar_y_start + (bar_y_end - bar_y_start) * scroll / (total - max_visible);
        int thumb_h = (bar_y_end - bar_y_start) * max_visible / total;
        fill_rect(lcd, bar_x - 1, thumb_y, bar_x + 1, thumb_y + thumb_h, ST7305_COLOR_BLACK);
    }

    st7305_flush(lcd);
}
#endif /* 0 */

static void wifi_kb_render(menu_state_t *state);  /* V1.0.46: 前向声明 */

void menu_render(menu_state_t *state) {
    /* 番茄钟运行中: 全屏倒计时 */
    if (s_pomo_active) {
        pomodoro_render(state->lcd);
        return;
    }
    if (state->current_page == MENU_PAGE_MAIN && state->anim_start_ms > 0) {
        state->needs_redraw = true;
    }
    /* V1.0.42: 时间弹窗编辑模式下, 字段闪烁需要周期性重绘 (每 500ms) */
    if (state->list_dialog_active && s_time_draft.editing) {
        static uint32_t s_last_blink_ms = 0;
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - s_last_blink_ms >= 500) {
            s_last_blink_ms = now_ms;
            state->needs_redraw = true;
            state->list_dialog_content_dirty = true;
            state->list_dialog_local_update = false;
        }
    }
    /* 同步蓝牙连接状态 (用于状态栏图标显示)
     * 即使回调路径遗漏, 这里轮询也能保证状态栏正确 */
    bool bt_now = bt_manager_is_connected();
    if (state->settings.bt_connected != bt_now) {
        state->settings.bt_connected = bt_now;
        state->settings.pad_connected = bt_now;
        if (bt_now) {
            state->settings.bt_device_type = BT_DEVICE_GAMEPAD;
        }
        state->needs_redraw = true;
    }
    /* "映射完成-待确认"展示态: 不再自动返回, 由 menu_handle_action 中
     * 用户按 [确认] 键 (mapping_confirm_done) 才返回手柄页, 见下方输入处理 */
    /* 赞助图彩蛋提示弹窗: hint_text 有效期内持续重绘, 到期自动清空 (与收藏提示一致) */
    if (state->sponsor_active && state->hint_text[0] != '\0') {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms < state->hint_until_ms) {
            state->needs_redraw = true;
        }
    }
    if (!state->needs_redraw) return;
    state->needs_redraw = false;

    /* === 弹窗内仅选项变化: 跳过整页重绘, 仅更新弹窗两行 ===
     * 帧缓冲中仍保留上一帧的"底页+旧弹窗"内容, 这里只重绘变化的行.
     * 配合 draw_list_dialog 的局部刷新, SPI 数据量从全屏 ~15KB 降到 ~300B. */
    bool dialog_local_only =
        state->list_dialog_active &&
        state->list_dialog_local_update &&
        !state->bt_scan_active &&
        !state->confirm_active;
    state->list_dialog_local_update = false;

    if (!dialog_local_only) {
        if (state->wifi_kb_active) {
            /* V1.0.46: Wi-Fi 虚拟键盘全屏覆盖 */
            wifi_kb_render(state);
        } else if (state->current_page == MENU_PAGE_BOOK) {
            if (book_reader_is_open()) {
                /* 电子书阅读器: 全屏渲染 */
                book_reader_render(state->lcd);
            } else {
                /* 电子书双栏菜单 */
                render_book_two_cols(state);
            }
        } else if (state->current_page == MENU_PAGE_MAIN) {
            render_main(state);
        } else if (state->current_page == MENU_PAGE_MP3_PLAYER) {
            render_mp3_player(state);
        } else if (state->current_page == MENU_PAGE_SELECT_GAME) {
            /* 电子词典: 左右分栏布局 (左侧 100px 文件夹, 右侧 292px 游戏) */
            render_select_game_two_cols(state);
        } else if (state->current_page == MENU_PAGE_GB_GAME) {
            /* V1.0.46: GB 复用电子词典分栏模板 (左栏 设置/收藏/文件夹, 右栏游戏) */
            render_select_game_two_cols(state);
        } else {
            render_sub(state);
        }
        /* 底页重绘会 st7305_clear 清空帧缓冲, list_dialog 的黑框/背景随之丢失.
         * 必须强制 list_dialog 走全量重绘 (list_dialog_draw_full), 否则局部刷新
         * 只重绘两行内容, 黑框永远不出现 (用户反馈"看不到黑框"的根因). */
        if (state->list_dialog_active) {
            state->list_dialog_prev_active = false;
            state->list_dialog_prev_selected = -1;
        }
    }
    /* 弹窗按优先级叠加: 蓝牙扫描 → 列表选择 → 确认/通知 → 按键映射小弹窗
     * 所有绘制共用同一帧缓冲, 最后只在 menu_render 末尾调用一次 st7305_flush,
     * 避免以前每条弹窗都 flush 造成的 "全屏先出基页, 再叠上弹窗" 闪烁 */
    if (state->bt_scan_active) {
        draw_bt_scan_dialog(state);
    }
    if (state->list_dialog_active) {
        draw_list_dialog(state);
    }
    if (state->confirm_active) {
        draw_confirm_dialog(state);
    }
    if (state->connecting_popup_active) {
        draw_connecting_popup(state);
    }
    /* V1.0.41: 按键映射小弹窗 (draw_notice_popup 样式) — 最上层, 映射期间常显.
     * 只显示"请按下 X 按键"提示, 不显示"已映射"结果 (用户要求). */
    if (state->key_mapping_idx >= 0) {
        char map_text[48];
        int f = state->key_mapping_idx;
        snprintf(map_text, sizeof(map_text),
                 "\xe8\xaf\xb7\xe6\x8c\x89\xe4\xb8\x8b %s \xe6\x8c\x89\xe9\x94\xae",  /* 请按下 X 按键 */
                 bt_manager_func_name((func_t)f));
        draw_notice_popup(state->lcd, map_text);
    }
    /* V1.0.46: 补充按键映射小弹窗 — 最上层, 映射期间常显 */
    if (state->sup_map_idx >= 0) {
        char map_text[56];
        int idx = state->sup_map_idx;
        if (!state->sup_map_captured) {
            snprintf(map_text, sizeof(map_text),
                     "\xe8\xaf\xb7\xe6\x8c\x89\xe4\xb8\x8b %s \xe6\x8c\x89\xe9\x94\xae",  /* 请按下 X 按键 */
                     bt_manager_sup_func_name(idx));
        } else {
            /* 已捕获后不显示具体按键名, 只提示按确定继续 */
            snprintf(map_text, sizeof(map_text),
                     "\xe5\xb7\xb2\xe6\x8d\x95\xe8\x8e\xb7\xef\xbc\x9a\xe6\x8c\x89\xe7\xa1\xae\xe5\xae\x9a\xe7\xbb\xa7\xe7\xbb\xad");  /* 已捕获：按确定继续 */
        }
        draw_notice_popup(state->lcd, map_text);
    }
    /* GB 辅助按键映射小弹窗 — 最上层, 映射期间常显.
     * 提示阶段: gb_aux_prompt=true 显示"映射辅助键"; 映射阶段显示"请按下 X 键". */
    if (state->gb_aux_prompt || state->gb_map_idx >= 0) {
        char map_text[56];
        if (state->gb_aux_prompt) {
            snprintf(map_text, sizeof(map_text),
                     "\xe6\x98\xa0\xe5\xb0\x84\xe8\xbe\x85\xe5\x8a\xa9\xe9\x94\xae");  /* 映射辅助键 */
        } else {
            int idx = state->gb_map_idx;
            if (idx < 0) idx = 0;
            if (!state->gb_map_captured) {
                snprintf(map_text, sizeof(map_text),
                         "\xe8\xaf\xb7\xe6\x8c\x89\xe4\xb8\x8b %s \xe6\x8c\x89\xe9\x94\xae",  /* 请按下 X 按键 */
                         bt_manager_gb_func_name(idx));
            } else {
                /* 已捕获后不显示具体按键名, 只提示按确定继续 (与补充按键一致) */
                snprintf(map_text, sizeof(map_text),
                         "\xe5\xb7\xb2\xe6\x8d\x95\xe8\x8e\xb7\xef\xbc\x9a\xe6\x8c\x89\xe7\xa1\xae\xe5\xae\x9a\xe7\xbb\xa7\xe7\xbb\xad");  /* 已捕获：按确定继续 */
            }
        }
        draw_notice_popup(state->lcd, map_text);
    }
    /* V1.0.46: 赞助作者全屏图 (1:1 居中 300x300, 按 BACK 退出) */
    if (state->sponsor_active) {
        st7305_clear(state->lcd, ST7305_COLOR_WHITE);
        st7305_draw_bitmap_1bit(state->lcd, 50, 0, 300, 300, s_sponsor_img);
        /* 彩蛋提示弹窗 (解锁/隐藏游戏菜单) 需叠加显示在赞助图上 */
        /* 模态弹窗: sponsor_notice_active=true 时常显, 需确认键关闭 */
        if (state->sponsor_notice_active && state->hint_text[0] != '\0') {
            draw_notice_popup_multiline(state->lcd, state->hint_text);
        } else if (!state->sponsor_notice_active && state->hint_text[0] != '\0') {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms < state->hint_until_ms) {
                draw_notice_popup_multiline(state->lcd, state->hint_text);
            } else {
                state->hint_text[0] = '\0';
                state->needs_redraw = true;  /* 到期清除残留弹窗 */
            }
        }
        st7305_flush(state->lcd);
        return;
    }

    /* V1.0.46: 全局短提示 (连接成功等, 0.5s) — 最上层 */
    if (state->hint_text[0] != '\0') {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms < state->hint_until_ms) {
            draw_notice_popup(state->lcd, state->hint_text);
        } else {
            state->hint_text[0] = '\0';
        }
    }
    /* 统一刷新: 一次 SPI 传输完成基页+弹窗, 避免双 flush 闪烁
     * 电子书二级菜单跟随"旋转方向"设置 (主菜单固定横屏)
     * 0/左/右 已由渲染函数把画面旋转进帧缓冲, 这里普通 flush; 只有 下(180°) 走驱动旋转 */
    if (state->current_page == MENU_PAGE_BOOK && state->book_rot == 1) {
        st7305_flush_rotated(state->lcd, 1, s_book_rot_buf);
    } else {
        st7305_flush(state->lcd);
    }
    /* 后台存档状态已在状态栏 SD 卡图标显示, 此处不画弹窗 */
}

/* ============ 动作处理 ============ */

/* ============ V1.0.46: Wi-Fi 虚拟键盘 (方向键 + 确认输入 SSID/密码) ============
 * V1.0.67 重排: 单大小写字母 + Shift 键切换大写, 字符 5 行 + 动作 1 行(每键 2 格). */
/* ===== Wi-Fi 虚拟键盘 (微软 QWERTY 风格, V1.0.67) ===== */
#define KB_KEY_W    40
#define KB_KEY_H    42
#define KB_ROW_GAP  3
#define KB_TOP_Y    58

/* 按键动作 */
#define KBK_CHAR   0
#define KBK_SHIFT  1
#define KBK_SYM    2
#define KBK_DEL    3
#define KBK_SPACE  4
#define KBK_RET    5
#define KBK_FIELD  6
#define KBK_OK     7

/* 字母层 4 个字符行 (行3 中间 8 键, 左右是 Shift/退格) */
static const char *const kb_abc_line[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."
};
/* 符号层 4 个字符行 */
static const char *const kb_sym_line[4] = {
    "1234567890", "!@#$%^&*()", "-_=+[]{};:'", "\\|/?<>\"~"
};

/* 显示字符: 字母层且 Shift 激活时, 小写字母转大写 */
static char kb_disp(const menu_state_t *state, char c) {
    if (!state->wifi_kb_sym && state->wifi_kb_shift && c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/* 画一个按键 (黑边框 + 居中文本; sel=反色) */
static void kb_draw_key(st7305_handle_t *lcd, int x, int y, int w, const char *label, bool sel) {
    if (sel) {
        fill_rect(lcd, x, y, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
    } else {
        fill_rect(lcd, x, y, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_WHITE);
        /* 边框 */
        draw_hline(lcd, x, x + w - 1, y, ST7305_COLOR_BLACK);
        draw_hline(lcd, x, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x, y, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x + w - 1, y, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
    }
    int tw = text_width(label);
    draw_text(lcd, x + (w - tw) / 2, y + (KB_KEY_H - 24) / 2, label, sel);
}

/* 执行一个按键动作 */
static void wifi_kb_do_key(menu_state_t *state, int kind, int ch) {
    switch (kind) {
    case KBK_CHAR: {
        char c = (char)ch;
        if (state->wifi_kb_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        char *buf = (state->wifi_kb_field == 0) ? state->wifi_kb_ssid : state->wifi_kb_pass;
        size_t max = (state->wifi_kb_field == 0) ? 31 : 63;
        size_t len = strlen(buf);
        if (len < max) { buf[len] = c; buf[len + 1] = '\0'; }
        break;
    }
    case KBK_SPACE: {
        char *buf = (state->wifi_kb_field == 0) ? state->wifi_kb_ssid : state->wifi_kb_pass;
        size_t max = (state->wifi_kb_field == 0) ? 31 : 63;
        size_t len = strlen(buf);
        if (len < max) { buf[len] = ' '; buf[len + 1] = '\0'; }
        break;
    }
    case KBK_SHIFT:
        state->wifi_kb_shift = !state->wifi_kb_shift;
        break;
    case KBK_SYM:
        state->wifi_kb_sym = !state->wifi_kb_sym;
        break;
    case KBK_DEL: {
        char *buf = (state->wifi_kb_field == 0) ? state->wifi_kb_ssid : state->wifi_kb_pass;
        size_t len = strlen(buf);
        if (len > 0) buf[len - 1] = '\0';
        break;
    }
    case KBK_FIELD:
        state->wifi_kb_field = !state->wifi_kb_field;
        break;
    case KBK_RET:
        state->wifi_kb_active = false;
        break;
    case KBK_OK:
        if (state->wifi_kb_ssid[0] == '\0') {
            snprintf(state->wifi_kb_msg, sizeof(state->wifi_kb_msg),
                     "\xe8\xaf\xb7\xe5\x85\x88\xe8\xbe\x93\xe5\x85\xa5 SSID"); /* 请先输入 SSID */
        } else {
            wifi_manager_connect(state->wifi_kb_ssid, state->wifi_kb_pass);
            state->wifi_kb_active = false;
            state->wifi_kb_msg[0] = '\0';
            connection_dialog_rebuild(state);
        }
        break;
    }
    state->needs_redraw = true;
}

static void wifi_kb_render(menu_state_t *state) {
    st7305_handle_t *lcd = state->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    draw_text_centered(lcd, 2, "Wi-Fi \xe8\xbf\x9e\xe6\x8e\xa5", false); /* Wi-Fi 连接 */

    /* 顶部当前输入行 (点击切换字段) */
    char line[80];
    const char *tag = (state->wifi_kb_field == 0) ? "SSID" : "\xe5\xaf\x86\xe7\xa0\x81"; /* SSID / 密码 */
    const char *val = (state->wifi_kb_field == 0) ? state->wifi_kb_ssid : state->wifi_kb_pass;
    snprintf(line, sizeof(line), "%s: %s_", tag, val);
    draw_text(lcd, 8, 30, line, false);

    const char *const *lines = state->wifi_kb_sym ? kb_sym_line : kb_abc_line;
    int y = KB_TOP_Y;

    /* 行 0/1: 10 键 */
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 10; i++) {
            char t[2] = { kb_disp(state, lines[r][i]), 0 };
            kb_draw_key(lcd, i * KB_KEY_W, y, KB_KEY_W, t, false);
        }
        y += KB_KEY_H + KB_ROW_GAP;
    }
    /* 行 2: 9 键居中 */
    int x2 = (SCREEN_W - 9 * KB_KEY_W) / 2;
    for (int i = 0; i < 9; i++) {
        char t[2] = { kb_disp(state, lines[2][i]), 0 };
        kb_draw_key(lcd, x2 + i * KB_KEY_W, y, KB_KEY_W, t, false);
    }
    y += KB_KEY_H + KB_ROW_GAP;

    /* 行 3: 左(Shift/ABC) + 8 键 + 右(退格) */
    kb_draw_key(lcd, 0, y, 52, state->wifi_kb_sym ? "ABC" : (state->wifi_kb_shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"), false); /* 小写/大写 */
    int x3 = 52;
    const int mid_w = (SCREEN_W - 104) / 8;   /* (400-104)/8 = 37 */
    for (int i = 0; i < 8; i++) {
        char t[2] = { kb_disp(state, lines[3][i]), 0 };
        kb_draw_key(lcd, x3 + i * mid_w, y, mid_w, t, false);
    }
    kb_draw_key(lcd, SCREEN_W - 52, y, 52, "\xe9\x80\x80\xe6\xa0\xbc", false); /* 退格 */
    y += KB_KEY_H + KB_ROW_GAP;

    /* 行 4: &123 + 空格 + 删除 + 连接 */
    kb_draw_key(lcd, 0, y, 60, state->wifi_kb_sym ? "ABC" : "&123", false);
    kb_draw_key(lcd, 63, y, 150, "\xe7\xa9\xba\xe6\xa0\xbc", false);           /* 空格 */
    kb_draw_key(lcd, 216, y, 86, "\xe5\x88\xa0\xe9\x99\xa4", false);           /* 删除 */
    kb_draw_key(lcd, 305, y, SCREEN_W - 305, "\xe8\xbf\x9e\xe6\x8e\xa5", false); /* 连接 */

    if (state->wifi_kb_msg[0]) {
        draw_text(lcd, 8, 285, state->wifi_kb_msg, false);
    }
    st7305_flush(lcd);
}

/* 触摸点选: 根据坐标命中的键执行动作. 返回 true=命中, false=未命中 */
static bool wifi_kb_hit(menu_state_t *state, int x, int y) {
    if (y < KB_TOP_Y) {   /* 点击顶部输入行 -> 切换字段 */
        wifi_kb_do_key(state, KBK_FIELD, 0);
        return true;
    }
    int row = (y - KB_TOP_Y) / (KB_KEY_H + KB_ROW_GAP);
    const char *const *lines = state->wifi_kb_sym ? kb_sym_line : kb_abc_line;
    if (row == 0 || row == 1) {
        if (x < 0 || x >= 10 * KB_KEY_W) return false;
        wifi_kb_do_key(state, KBK_CHAR, lines[row][x / KB_KEY_W]);
        return true;
    }
    if (row == 2) {
        int x2 = (SCREEN_W - 9 * KB_KEY_W) / 2;
        if (x < x2 || x >= x2 + 9 * KB_KEY_W) return false;
        wifi_kb_do_key(state, KBK_CHAR, lines[2][(x - x2) / KB_KEY_W]);
        return true;
    }
    if (row == 3) {
        if (x < 52) { wifi_kb_do_key(state, state->wifi_kb_sym ? KBK_SYM : KBK_SHIFT, 0); return true; }
        if (x >= SCREEN_W - 52) { wifi_kb_do_key(state, KBK_DEL, 0); return true; }
        const int mid_w = (SCREEN_W - 104) / 8;
        int mi = (x - 52) / mid_w;
        if (mi >= 0 && mi < 8) wifi_kb_do_key(state, KBK_CHAR, lines[3][mi]);
        return true;
    }
    if (row == 4) {
        if (x < 60) { wifi_kb_do_key(state, KBK_SYM, 0); return true; }
        if (x < 63 + 150) { wifi_kb_do_key(state, KBK_SPACE, 0); return true; }
        if (x < 216 + 86) { wifi_kb_do_key(state, KBK_DEL, 0); return true; }
        wifi_kb_do_key(state, KBK_OK, 0);
        return true;
    }
    return false;
}

static void wifi_kb_confirm(menu_state_t *state) {
    /* 方向键确认: 保留简单导航, 但确认键直接"连接" */
    wifi_kb_do_key(state, KBK_OK, 0);
}

/* V1.0.65: 触摸点击 hit-test (点哪进哪).
 * 仅在"主菜单桌面"或"列表弹窗"两种干净状态下介入, 命中后只负责移动选中,
 * 由 main.c 随后调用 menu_handle_action(CONFIRM) 复用既有进入逻辑.
 * 其余状态(确认弹窗/扫描/连接中/按键映射/自定义渲染弹窗等)一律返回 false,
 * 让 main.c 回退为普通 CONFIRM, 行为等同物理确认键. */
bool menu_handle_touch(menu_state_t *state, int x, int y) {
    if (!state) return false;

    /* V1.0.68: 赞助图全屏页不做命中, 点击直接当确认 (5连击彩蛋计数),
     * 其余模态状态保持点击忽略, 由物理键/手柄操作, 避免误触二次确认弹窗. */
    if (state->sponsor_active) {
        return true;
    }
    if (state->confirm_active || state->bt_scan_active || state->connecting_popup_active ||
        s_pomo_active ||
        state->key_mapping_idx >= 0 || state->sup_map_idx >= 0 || state->gb_map_idx >= 0 ||
        state->gb_aux_prompt) {
        return false;
    }

    /* Wi-Fi 虚拟键盘: 点击按键直接输入 */
    if (state->wifi_kb_active) {
        wifi_kb_hit(state, x, y);
        return true;   /* 消费点击, 不穿透 */
    }

    /* V1.0.68: 电子书阅读器打开时按区域翻页/进设置 (上半=上页, 下半=下页, 中间=设置),
     * 触摸坐标已含旋转映射. 返回 false 避免再触发一次 CONFIRM. */
    if (state->current_page == MENU_PAGE_BOOK && book_reader_is_open()) {
        if (book_reader_handle_touch(x, y)) {
            state->needs_redraw = true;
        }
        return false;
    }

    /* 列表弹窗: 行 hit-test */
    if (state->list_dialog_active) {
        /* V1.0.68: 番茄钟弹窗行布局与标准一致 (content_y0=29, line_h=40),
         * 支持触摸点选; 其他自定义弹窗(时间等单行多字段)不做行定位. */
        if (state->list_dialog_on_render &&
            state->list_dialog_on_key != pomodoro_dialog_on_key) {
            return false;
        }
        list_dialog_geom_t g;
        list_dialog_calc_geom(state, &g);
        if (x < g.x || x >= g.x + g.w || y < g.y || y >= g.y + g.h) {
            return false;   /* 点在弹窗外 */
        }
        int target = -1;
        if (g.footer_y >= 0 && y >= g.footer_y && y < g.footer_y + g.line_h) {
            target = state->list_dialog_count - 1;   /* 底部固定"返回"行 */
        } else {
            int vis = (y - g.content_y0) / g.line_h;
            if (vis >= 0 && vis < g.content_visible) {
                int idx = state->list_dialog_scroll + vis;
                if (idx >= 0 && idx < g.content_count) target = idx;
            }
        }
        if (target >= 0 && target < state->list_dialog_count) {
            state->list_dialog_selected = target;
            state->needs_redraw = true;
            ESP_LOGI(TAG, "触摸点击弹窗行 %d", target);
            return true;
        }
        return false;
    }

    /* 主菜单桌面: 图标 hit-test (与 render_main 布局一致) */
    if (state->current_page == MENU_PAGE_MAIN) {
        int total = menu_main_count(state);
        if (total <= 0) return false;
        int sel = state->selected_index;
        const int spacing = 100;          /* 与 render_main 的 icon_spacing 一致 */
        const int cy = SCREEN_H / 2;      /* icon_center_y */
        int best = -1, best_d = 1 << 30;
        for (int i = 0; i < total; i++) {
            int diff = i - sel;
            if (diff > total / 2) diff -= total;
            else if (diff < -total / 2) diff += total;
            int cx = SCREEN_W / 2 + diff * spacing;
            int ad = diff < 0 ? -diff : diff;
            int size = (ad == 0) ? 100 : (ad == 1) ? 70 : (ad == 2) ? 50 : 40;
            if (cx < -size || cx > SCREEN_W + size) continue;  /* 屏幕外 */
            int dx = x - cx, dy = y - cy;
            int r = size / 2 + 15;        /* 15px 触屏容差 */
            if (dx * dx + dy * dy <= r * r) {
                int d = dx * dx + dy * dy;
                if (best < 0 || d < best_d) { best_d = d; best = i; }
            }
        }
        if (best >= 0) {
            state->selected_index = best;
            state->anim_start_ms = 0;     /* 直接定位, 不播滑动动画(马上进入子页) */
            state->needs_redraw = true;
            ESP_LOGI(TAG, "触摸点击主菜单图标 %d", best);
            return true;
        }
        return false;
    }

    /* === 双栏页面: 电子词典 / GB / 电子书 (SELECT_GAME / GB_GAME / BOOK) ===
     * 布局与 draw_folder_pane / draw_game_pane 一致: 左栏 2..96, 右栏 104..396,
     * 行高 32, 起点 y=30. 左栏命中=切焦点到右栏, 右栏命中=触发对应项 (启动/设置). */
    if (state->current_page == MENU_PAGE_SELECT_GAME ||
        state->current_page == MENU_PAGE_GB_GAME ||
        (state->current_page == MENU_PAGE_BOOK && !book_reader_is_open())) {
        /* V1.0.68: 电子书竖屏二级菜单 (选文件夹/选小说) 触摸命中, 物理坐标旋转到逻辑竖屏 */
        if (state->current_page == MENU_PAGE_BOOK &&
            (state->book_rot == 2 || state->book_rot == 3)) {
            int lx, ly;
            if (state->book_rot == 2) { lx = y; ly = ST7305_WIDTH - 1 - x; }
            else                     { lx = ST7305_HEIGHT - 1 - y; ly = x; }
            bool mirror = (state->book_rot == 3);
            int side_x0 = mirror ? 8 : 4, side_x1 = mirror ? 104 : 100;
            int right_x = mirror ? 112 : 108, right_x1 = right_x + 184;
            const int y0 = 4, row_h = 30;
            int max_vis = (MPFB_H - y0 - 6) / row_h;
            if (max_vis < 1) max_vis = 1;
            int i = (ly - y0) / row_h;
            if (i >= 0 && i < max_vis) {
                int total = book_sidebar_total();
                if (lx >= side_x0 && lx <= side_x1 && total > 0) {
                    int idx = state->select_folder_scroll + i;
                    if (idx >= 0 && idx < total) {
                        state->select_focus = 0;
                        state->select_folder_idx = idx;
                        state->select_game_idx = 0;
                        state->select_game_scroll = 0;
                        state->needs_redraw = true;
                        ESP_LOGI(TAG, "竖屏触摸点击左栏 %d", idx);
                        return true;
                    }
                } else if (lx >= right_x && lx <= right_x1 && g_sub_count > 0) {
                    int idx = state->select_game_scroll + i;
                    if (idx >= 0 && idx < g_sub_count) {
                        state->select_focus = 1;
                        state->select_game_idx = idx;
                        state->needs_redraw = true;
                        ESP_LOGI(TAG, "竖屏触摸点击右栏 %d", idx);
                        return true;
                    }
                }
            }
            return false;
        }
        int left_total = (state->current_page == MENU_PAGE_BOOK)
                             ? book_sidebar_total()
                             : (3 + g_folder_count);   /* 设置/收藏/全部 + 文件夹 */
        int right_total = g_sub_count;

        /* V1.0.68 fix: 电子书横屏 rot=1(下) 时菜单渲染走 flush_rotated 180° 翻转,
         * 触摸命中必须同样 180° 旋转, 否则点击上下左右颠倒 (竖屏 2/3 已在上面手动旋转). */
        if (state->current_page == MENU_PAGE_BOOK && state->book_rot == 1) {
            int ox = x, oy = y;
            x = ST7305_WIDTH - 1 - ox;
            y = ST7305_HEIGHT - 1 - oy;
        }

        const int left_x0 = 2, left_x1 = SELECT_LEFT_W - 4;
        const int right_x0 = SELECT_RIGHT_X, right_x1 = SCREEN_W - 4;
        const int list_y = SELECT_LIST_Y;

        if (x >= left_x0 && x <= left_x1 && left_total > 0) {
            int i = (y - list_y) / SELECT_ITEM_H;
            if (i >= 0 && i < SELECT_MAX_VISIBLE) {
                int idx = state->select_folder_scroll + i;
                if (idx >= 0 && idx < left_total) {
                    state->select_focus = 0;
                    state->select_folder_idx = idx;
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                    state->needs_redraw = true;
                    ESP_LOGI(TAG, "触摸点击左栏 %d", idx);
                    return true;
                }
            }
        } else if (x >= right_x0 && x <= right_x1 && right_total > 0) {
            int i = (y - list_y) / SELECT_ITEM_H;
            if (i >= 0 && i < SELECT_MAX_VISIBLE) {
                int idx = state->select_game_scroll + i;
                if (idx >= 0 && idx < right_total) {
                    state->select_focus = 1;
                    state->select_game_idx = idx;
                    state->needs_redraw = true;
                    ESP_LOGI(TAG, "触摸点击右栏 %d", idx);
                    return true;
                }
            }
        }
        return false;
    }

    /* === MP3 播放器: 左菜单列表 (布局与 render_mp3_player 一致) === */
    if (state->current_page == MENU_PAGE_MP3_PLAYER) {
        if (mp3_count <= 0) return false;
        const int list_y = 28, line_h = 26, list_w = 195;
        if (x >= 0 && x < list_w) {
            int highlight_idx = mp3_in_player ? mp3_current : state->selected_index;
            if (highlight_idx < 0) highlight_idx = 0;
            int max_visible = (SCREEN_H - list_y - 5) / line_h;
            int scroll = 0;
            if (highlight_idx >= max_visible - 1) scroll = highlight_idx - max_visible + 2;
            if (scroll < 0) scroll = 0;
            int i = (y - list_y - 2) / line_h;
            if (i >= 0 && i < max_visible - 1) {
                int idx = scroll + i;
                if (idx >= 0 && idx < mp3_count) {
                    state->selected_index = idx;
                    state->needs_redraw = true;
                    ESP_LOGI(TAG, "触摸点击 MP3 列表 %d", idx);
                    return true;
                }
            }
        }
        return false;
    }

    /* === 全屏居中列表页 (render_sub): 音量/游戏设置/系统信息等 ===
     * 布局与 render_sub 一致: 行高 32, 起点 y=30 (有提示时 60), 全宽整行可点.
     * 仅处理真正走 render_sub 渲染的页面 (sub_pages 有 title), 防止残留 page 误命中. */
    {
        if (state->current_page < 0 || state->current_page >= MENU_PAGE_COUNT ||
            !sub_pages[state->current_page].title) {
            return false;
        }
        int start_y = 30;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (state->hint_text[0] != '\0' && now < state->hint_until_ms) start_y = 60;
        const int line_h = 32;
        int max_visible = (SCREEN_H - start_y - 8) / line_h;
        if (max_visible < 1) max_visible = 1;
        if (g_sub_count > 0) {
            int i = (y - start_y) / line_h;
            if (i >= 0 && i < max_visible) {
                int idx = state->scroll_offset + i;
                if (idx >= 0 && idx < g_sub_count) {
                    state->selected_index = idx;
                    state->needs_redraw = true;
                    ESP_LOGI(TAG, "触摸点击列表项 %d", idx);
                    return true;
                }
            }
        }
    }

    return false;
}

/* V1.0.66: 主菜单拖动松手吸附 (支持跨多格). */
void menu_drag_settle(menu_state_t *state, int steps) {
    if (!state || steps == 0) return;
    int total = menu_main_count(state);
    if (total <= 0) return;
    int prev = state->selected_index;
    int sel = ((prev + steps) % total + total) % total;   /* 循环 wrap */
    if (sel == prev) return;   /* 整圈回到原位, 不切换 */
    state->prev_selected = prev;
    state->anim_direction = (steps > 0) ? -1 : 1;   /* 与按键 RIGHT/LEFT 动画方向一致 */
    state->anim_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    state->selected_index = sel;
    state->needs_redraw = true;
    ESP_LOGI(TAG, "拖动吸附 steps=%d %d->%d", steps, prev, sel);
}

/* V1.0.68: 游戏二级菜单右栏拖动松手吸附. steps 正=选中下移, 负=选中上移.
 * 游戏列表非循环, 边界钳制; 吸附后拖动偏移归零, 由 draw_game_pane 重新居中. */
void menu_select_game_settle(menu_state_t *state, int steps) {
    if (!state || steps == 0) return;
    int total = g_sub_count;
    if (total <= 0) return;
    int sel = state->select_game_idx + steps;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;
    state->select_game_idx = sel;
    state->select_game_drag_offset = 0;
    state->needs_redraw = true;
    ESP_LOGI(TAG, "游戏列表吸附 steps=%d idx=%d", steps, sel);
}

/* V1.0.68: 游戏列表拖动/按下实时选中: 选中手指所在行的游戏.
 * ty = 手指物理 y, off = 拖动偏移 (当前恒为 0: 列表不随偏移滚动, 避免松手回弹).
 * 不做可见窗口钳制: 手指超出窗口时由渲染的 scroll 钳制自动滚动跟进. */
void menu_select_game_touch_track(menu_state_t *state, int ty, int off) {
    if (!state || g_sub_count <= 0) return;
    int i = (ty - SELECT_LIST_Y - off) / SELECT_ITEM_H;
    int idx = state->select_game_scroll + i;
    if (idx < 0) idx = 0;
    if (idx >= g_sub_count) idx = g_sub_count - 1;
    if (idx != state->select_game_idx) {
        state->select_game_idx = idx;
        state->needs_redraw = true;
    }
}

/* V1.0.68: 触摸长按 (游戏列表右栏) → 选中该行并加入/取消收藏.
 * x/y 为物理屏幕坐标 (游戏页触摸未旋转). */
/* V1.0.68: 列表弹窗跟手拖动: 选中手指所在行 (超出窗口时由 draw_list_dialog
 * 的 scroll 钳制自动滚动). 自定义渲染弹窗(时间等单行多字段)不做行定位. */
void menu_list_dialog_touch_track(menu_state_t *state, int ty) {
    if (!state || !state->list_dialog_active || state->list_dialog_count <= 0) return;
    if (state->list_dialog_on_render) return;
    list_dialog_geom_t g;
    list_dialog_calc_geom(state, &g);
    int sel = -1;
    if (g.footer_y >= 0 && ty >= g.footer_y && ty < g.footer_y + g.line_h) {
        sel = state->list_dialog_count - 1;   /* 底部固定"返回"行 */
    } else {
        int vis = (ty - g.content_y0) / g.line_h;
        if (vis < 0) vis = 0;
        sel = state->list_dialog_scroll + vis;
        if (sel >= g.content_count && g.content_count > 0) sel = g.content_count - 1;
    }
    if (sel >= 0 && sel < state->list_dialog_count && sel != state->list_dialog_selected) {
        state->list_dialog_selected = sel;
        state->needs_redraw = true;
    }
}

/* V1.0.68: 列表弹窗松手: 固定内容位置 (scroll 补偿拖动偏移, 不回弹不吸附).
 * 与游戏内容页拖动一致: 整列跟手, 松手停住. */
void menu_list_dialog_release(menu_state_t *state) {
    if (!state) return;
    int off = state->list_dialog_drag_offset;
    if (off > 0) state->list_dialog_scroll -= (off + LIST_DIALOG_LINE_H / 2) / LIST_DIALOG_LINE_H;
    else if (off < 0) state->list_dialog_scroll += ((-off) + LIST_DIALOG_LINE_H / 2) / LIST_DIALOG_LINE_H;
    if (state->list_dialog_scroll < 0) state->list_dialog_scroll = 0;
    state->list_dialog_drag_fix = true;
    state->list_dialog_drag_offset = 0;
    state->needs_redraw = true;
}

void menu_touch_long_press(menu_state_t *state, int x, int y) {
    if (!state) return;
    if (state->current_page != MENU_PAGE_SELECT_GAME && state->current_page != MENU_PAGE_GB_GAME) return;
    if (x < SELECT_RIGHT_X || x > SCREEN_W - 4) return;
    if (g_sub_count <= 0) return;
    int i = (y - SELECT_LIST_Y) / SELECT_ITEM_H;
    if (i < 0 || i >= SELECT_MAX_VISIBLE) return;
    int idx = state->select_game_scroll + i;
    if (idx < 0 || idx >= g_sub_count) return;
    state->select_focus = 1;
    state->select_game_idx = idx;
    state->needs_redraw = true;
    char path[160];
    if (get_current_selected_path(state, path, sizeof(path))) {
        fav_engine_t fav_e = favorites_engine_for_path(path);
        bool was_fav = favorites_contains(fav_e, path);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (was_fav) {
            favorites_remove(fav_e, path);
            snprintf(state->hint_text, sizeof(state->hint_text),
                     "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x94\xb6\xe8\x97\x8f");  /* 已取消收藏 */
            ESP_LOGI(TAG, "触摸长按: 已取消收藏 %s", path);
        } else if (favorites_add(fav_e, path)) {
            snprintf(state->hint_text, sizeof(state->hint_text),
                     "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe6\x94\xb6\xe8\x97\x8f");  /* 已添加收藏栏 */
            ESP_LOGI(TAG, "触摸长按: 已添加收藏栏 %s", path);
        }
        state->hint_until_ms = now + FAV_COOLDOWN_MS;
        state->space_cooldown_until_ms = now + FAV_COOLDOWN_MS;
        state->needs_redraw = true;
        select_game_invalidate_cache();
    }
}

/* V1.0.68: 是否有模态弹窗/全屏覆盖激活. 弹窗期间 main.c 禁止背景拖动/滑动,
 * 触摸只能控制弹窗本身. 番茄钟倒计时(全屏)也算模态. */
bool menu_modal_active(const menu_state_t *state) {
    if (!state) return false;
    if (state->list_dialog_active || state->confirm_active || state->bt_scan_active ||
        state->connecting_popup_active || state->sponsor_active || state->wifi_kb_active ||
        state->key_mapping_idx >= 0 || state->sup_map_idx >= 0 || state->gb_map_idx >= 0 ||
        state->gb_aux_prompt) {
        return true;
    }
    return s_pomo_active;   /* 番茄钟倒计时全屏覆盖 */
}

void menu_handle_action(menu_state_t *state, menu_action_t action) {
    /* 番茄钟运行中: 任意按键返回 */
    if (s_pomo_active) {
        if (action != MENU_ACTION_NONE) {
            pomodoro_stop(state);
            state->current_page = MENU_PAGE_MAIN;
            state->needs_redraw = true;
        }
        return;
    }
    /* 按键反馈音选项已移除 (无蜂鸣声); 菜单导航本就不响 */
    /* V1.0.46: 全屏赞助图期间, 按 BACK 退出 */
    if (state->sponsor_active) {
        /* 彩蛋提示为模态弹窗: 弹出期间仅确认/返回可关闭, 其余按键不计数 */
        if (state->sponsor_notice_active) {
            if (action == MENU_ACTION_CONFIRM || action == MENU_ACTION_BACK || action == MENU_ACTION_HOME) {
                state->sponsor_notice_active = false;
                state->hint_text[0] = '\0';
                state->needs_redraw = true;
            }
            return;
        }
        if (action == MENU_ACTION_BACK || action == MENU_ACTION_HOME) {
            state->sponsor_active = false;
            state->sponsor_confirm_count = 0;
            state->needs_redraw = true;
        } else if (action == MENU_ACTION_CONFIRM) {
            /* === 彩蛋: 连续按确认键 5 次切换隐藏游戏菜单显示 === */
            state->sponsor_confirm_count++;
            if (state->sponsor_confirm_count >= 5) {
                state->sponsor_confirm_count = 0;
                state->show_hidden_menus = !state->show_hidden_menus;
                /* 持久化到 NVS */
                nvs_handle_t h;
                if (nvs_open("menu_settings", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "show_hidden", (uint8_t)state->show_hidden_menus);
                    nvs_commit(h);
                    nvs_close(h);
                }
                /* 切换后主菜单项数变化, 收敛选中项避免越界 */
                if (state->selected_index >= menu_main_count(state))
                    state->selected_index = menu_main_count(state) - 1;
                if (state->show_hidden_menus) {
                    ESP_LOGW(TAG, "彩蛋: 已解锁测试游戏引擎");
                    snprintf(state->hint_text, sizeof(state->hint_text),
                             "\xe5\xb7\xb2\xe8\xa7\xa3\xe9\x94\x81\xe6\xb5\x8b\xe8\xaf\x95\xe6\xb8\xb8\xe6\x88\x8f\xe5\xbc\x95\xe6\x93\x8e");
                    /* 已解锁测试游戏引擎 */
                } else {
                    ESP_LOGI(TAG, "彩蛋: 已关闭测试游戏引擎");
                    snprintf(state->hint_text, sizeof(state->hint_text),
                             "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad\xe6\xb5\x8b\xe8\xaf\x95\xe6\xb8\xb8\xe6\x88\x8f\xe5\xbc\x95\xe6\x93\x8e");
                    /* 已关闭测试游戏引擎 */
                }
                state->sponsor_notice_active = true;
                state->needs_redraw = true;
            }
        }
        return;
    }
    /* === 电子书页面: 阅读器打开时模态, 否则双栏菜单 === */
    if (state->current_page == MENU_PAGE_BOOK) {
        /* V1.0.68: HOME (软关机键长按0.5s / 状态栏长按3s) 从书籍任何界面
         * (阅读中/二级菜单) 直接返回主菜单, 不经过退出确认. */
        if (action == MENU_ACTION_HOME) {
            if (book_reader_is_open()) book_reader_close();
            state->current_page = MENU_PAGE_MAIN;
            state->selected_index = state->main_selected_index;
            state->scroll_offset = 0;
            state->book_loaded = false;   /* 下次进入重新扫描书单 */
            state->needs_redraw = true;
            return;
        }
        if (book_reader_is_open()) {
            if (book_reader_handle_action(action)) {
                state->needs_redraw = true;
            }
        } else {
            book_handle_action(state, action);
        }
        return;
    }
    /* V1.0.46: Wi-Fi 虚拟键盘激活期间优先处理确认/取消 (触摸点选为主, 方向键不再导航) */
    if (state->wifi_kb_active) {
        switch (action) {
            case MENU_ACTION_CONFIRM: wifi_kb_confirm(state); break;
            case MENU_ACTION_BACK:
            case MENU_ACTION_HOME:  state->wifi_kb_active = false; state->needs_redraw = true; break;
            default: break;
        }
        return;
    }
    /* V1.0.41: 按键映射小弹窗激活期间, 仅 BACK/HOME 可退出, 其他动作忽略
     * (防止手柄方向键/确认键干扰映射流程) */
    if (state->key_mapping_idx >= 0) {
        if (action == MENU_ACTION_BACK || action == MENU_ACTION_HOME) {
            ESP_LOGI(TAG, "按键映射: 用户取消");
            state->key_mapping_idx = -1;
            state->key_mapping_phase = false;
            state->key_mapping_until_ms = 0;
            input_set_gamepad_nav_enabled(true);  /* 恢复手柄导航 */
            state->needs_redraw = true;
        }
        return;
    }
    /* V1.0.46: 补充按键映射期间: 吞噬所有 action,
     * 由 menu_poll_sup_mapping 自行处理"确定=下一键 / 返回=跳过" */
    if (state->sup_map_idx >= 0) {
        return;
    }
    /* 退出到菜单: HOME 键直接返回主菜单 (V1.0.68: 状态栏长按 3s 也走这里) */
    if (action == MENU_ACTION_HOME) {
        if (state->bt_scan_active) {
            bt_manager_stop_scan();
            state->bt_scan_active = false;
        }
        if (bt_manager_is_connecting()) {
            bt_manager_cancel_connect();
            state->bt_auto_connect_active = false;
            state->bt_auto_connect_target[0] = '\0';
            state->bt_auto_connect_found = 0;
            state->bt_connect_awaiting = false;
        }
        /* 关闭所有弹窗/模态状态, 确保回到主菜单后无残留 */
        state->confirm_active = false;
        state->confirm_notice = false;
        state->confirm_no_hint = false;
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_key = NULL;
        state->list_dialog_on_close = NULL;
        state->connecting_popup_active = false;
        state->connecting_popup_until_ms = 0;
        state->connecting_popup_started_at_ms = 0;
        state->hint_text[0] = '\0';
        state->hint_until_ms = 0;
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return;
    }

    /* V1.0.39: 长按左键 (设备物理键 / 手柄 F_LEFT) → 全局快捷键: 直接搜索蓝牙设备.
     * 无论当前在哪个页面/弹窗, 都先清理状态再触发扫描. */
    if (action == MENU_ACTION_LONG_LEFT) {
        ESP_LOGI(TAG, "长按LEFT → 全局快捷键: 直接搜索蓝牙设备");
        /* 停止正在进行的扫描 */
        if (state->bt_scan_active) {
            bt_manager_stop_scan();
            state->bt_scan_active = false;
        }
        /* 取消在途连接 */
        if (bt_manager_is_connecting()) {
            bt_manager_cancel_connect();
            state->bt_auto_connect_active = false;
            state->bt_auto_connect_target[0] = '\0';
            state->bt_auto_connect_found = 0;
            state->bt_connect_awaiting = false;
        }
        /* 关闭所有弹窗, 回到主菜单 */
        state->confirm_active = false;
        state->confirm_notice = false;
        state->confirm_no_hint = false;
        state->list_dialog_active = false;
        state->list_dialog_prev_active = false;
        state->list_dialog_prev_selected = -1;
        state->list_dialog_on_key = NULL;
        state->list_dialog_on_close = NULL;
        state->connecting_popup_active = false;
        state->connecting_popup_until_ms = 0;
        state->connecting_popup_started_at_ms = 0;
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = state->main_selected_index;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        /* 触发添加设备 (直接扫描) */
        gamepad_act_add_device(state);
        return;
    }

    /* V1.0.47: 紧凑小提示弹窗 (连接失败/连接超时/收藏等) 显示期间, BACK 可直接取消 */
    if (state->hint_text[0] != '\0' &&
        (action == MENU_ACTION_BACK || action == MENU_ACTION_HOME)) {
        state->hint_text[0] = '\0';
        state->hint_until_ms = 0;
        state->needs_redraw = true;
        return;
    }

    /* 连接小弹窗激活: 仅 BACK 可取消在途连接并关闭弹窗, 其余动作忽略 */
    if (state->connecting_popup_active) {
        if (action == MENU_ACTION_BACK) {
            state->connecting_popup_active = false;
            state->connecting_popup_until_ms = 0;
            state->connecting_popup_started_at_ms = 0;  /* V1.0.24: 清看门狗起点 */
            if (bt_manager_is_connecting()) {
                bt_manager_cancel_connect();
                state->bt_auto_connect_active = false;
                state->bt_auto_connect_target[0] = '\0';
                state->bt_auto_connect_found = 0;
                state->bt_connect_awaiting = false;
            }
            state->needs_redraw = true;
        }
        return;
    }

    /* 确认/通知弹窗激活 (弹窗绝对优先):
     * 必须在所有背景主菜单/子页处理之前, 也必须在下层弹窗 (bt_scan/list_dialog) 之前,
     * 因为 menu_render 的叠加顺序是 bt_scan → list_dialog → confirm, confirm 是最后
     * 画在最上层的一层。否则弹窗显示时方向键/BACK 会被背景吞掉,
     * 出现"弹窗还在、却控制背景、无法返回"的 bug。
     * 注意: 删除失败等场景会 confirm_active 与 list_dialog_active 同时为真 (confirm 在上层),
     * 因此 confirm 必须最先判断。 */
    if (state->confirm_active) {
        if (action == MENU_ACTION_BACK) {
            state->confirm_active = false;
            state->confirm_notice = false;
            state->confirm_no_hint = false;
            state->list_dialog_prev_active = false;
            state->list_dialog_prev_selected = -1;
            /* 退出"正在连接"窗口: 取消在途连接, 并清理主动连接/等待标记 */
            if (bt_manager_is_connecting()) {
                bt_manager_cancel_connect();
                state->bt_auto_connect_active = false;
                state->bt_auto_connect_target[0] = '\0';
                state->bt_auto_connect_found = 0;
                state->bt_connect_awaiting = false;
            }
            state->needs_redraw = true;
            return;
        }
        if (action == MENU_ACTION_CONFIRM) {
            if (state->confirm_notice) {
                /* 普通通知 (如"正在连接..."/"删除失败"/"暂无记录"): 直接关闭 */
                if (bt_manager_is_connecting()) {
                    bt_manager_cancel_connect();
                    state->bt_auto_connect_active = false;
                    state->bt_auto_connect_target[0] = '\0';
                    state->bt_auto_connect_found = 0;
                    state->bt_connect_awaiting = false;
                }
                state->confirm_active = false;
                state->confirm_notice = false;
                state->confirm_no_hint = false;
            } else {
                /* 二次确认弹窗 (如删除/退出到菜单): 执行 on_confirm 回调 */
                state->confirm_active = false;
                state->confirm_executing = true;
                const sub_page_def_t *page = &sub_pages[state->current_page];
                if (page->on_confirm) {
                    page->on_confirm(state, state->confirm_idx);
                }
                state->confirm_executing = false;
            }
            /* 关闭确认弹窗后, 强制下层 list_dialog 下次全量重绘, 防止外框/文字残留 */
            state->list_dialog_prev_active = false;
            state->list_dialog_prev_selected = -1;
            state->needs_redraw = true;
            return;
        }
        /* 其它动作 (上下左右方向键) 在确认/通知弹窗激活期间一律忽略,
         * 不穿透到背景主菜单/子页, 实现"弹窗优先"。 */
        return;
    }

    /* 已删除: 连接记录全屏页输入处理 (死代码, bt_history_active 从未被置 true).
     * 连接记录统一走 list_dialog 弹窗, 见 gamepad_history_on_select. */
    /* 蓝牙扫描弹窗 */
    if (state->bt_scan_active) {
        switch (action) {
            case MENU_ACTION_UP:
            case MENU_ACTION_LEFT:
                if (state->bt_device_count > 0) {
                    if (state->selected_index > 0) state->selected_index--;
                    else state->selected_index = state->bt_device_count - 1;
                    state->needs_redraw = true;
                }
                break;
            case MENU_ACTION_DOWN:
            case MENU_ACTION_RIGHT:
                if (state->bt_device_count > 0) {
                    if (state->selected_index < state->bt_device_count - 1) state->selected_index++;
                    else state->selected_index = 0;
                    state->needs_redraw = true;
                }
                break;
            case MENU_ACTION_CONFIRM:
                if (state->bt_device_count > 0) {
                    bt_manager_stop_scan();
                    bool connected = bt_manager_connect_device(&state->bt_devices[state->selected_index]);
                    state->bt_scan_active = false;
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                    if (connected) {
                        /* 连接任务已启动, 显示居中小弹窗"正在连接" (用户需求).
                         * 由 bt_connect_callback 统一更新为"连接成功"或关闭弹窗。
                         * bt_connect_awaiting 用于区分"连接失败"与"正常断开"。 */
                        g_menu.bt_connect_awaiting = true;
                        g_menu.connecting_popup_active = true;
                        g_menu.connecting_popup_success = false;
                        uint32_t t_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        /* === 关键修复 V1.0.24: 缩短到 25s === */
                        g_menu.connecting_popup_until_ms = t_now + 25000;
                        g_menu.connecting_popup_started_at_ms = t_now;
                    } else {
                        /* 同步返回 false: 区分"已有连接在途"与硬错误。
                         * - 仍在连接中: 同样显示"正在连接"小弹窗, 等待回调。
                         * - 硬错误(已连接/未就绪/任务异常): 弹具体失败原因。 */
                        if (bt_manager_is_connecting()) {
                            g_menu.bt_connect_awaiting = true;
                            g_menu.connecting_popup_active = true;
                            g_menu.connecting_popup_success = false;
                            uint32_t t_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                            g_menu.connecting_popup_until_ms = t_now + 25000;
                            g_menu.connecting_popup_started_at_ms = t_now;
                        } else {
                            /* 连接失败, 弹窗提示具体原因 */
                            snprintf(state->confirm_title, sizeof(state->confirm_title), "失败");
                            snprintf(state->confirm_msg, sizeof(state->confirm_msg),
                                     "%s", bt_manager_get_connect_error());
                            state->confirm_active = true;
                            state->confirm_notice = true;
                        }
                    }
                }
                state->needs_redraw = true;
                break;
            case MENU_ACTION_BACK:
                bt_manager_stop_scan();
                state->bt_scan_active = false;
                state->bt_auto_connect_active = false;  /* 主动连接模式被取消 */
                state->bt_auto_connect_found = 0;
                state->bt_auto_connect_target[0] = '\0';
                state->bt_device_count = 0;
                state->selected_index = 0;
                state->scroll_offset = 0;
                state->needs_redraw = true;
                break;
            default:
                break;
        }
        return;
    }
    
    /* 列表选择弹窗 */
    if (state->list_dialog_active) {
        switch (action) {
            case MENU_ACTION_UP:
            case MENU_ACTION_LEFT:
            case MENU_ACTION_DOWN:
            case MENU_ACTION_RIGHT:
            case MENU_ACTION_LONG_LEFT:
                /* 用户需求: 时间弹窗等自定义弹窗用 on_key 接管方向键.
                 * 回调中可以修改 selected 或对应字段, 自己控制 UI 更新.
                 * V1.0.39: LONG_LEFT 也交给 on_key 处理 (手柄配置弹窗用此快捷键直接扫描). */
                if (state->list_dialog_on_key) {
                    state->list_dialog_on_key(state, state->list_dialog_selected, action);
                    /* 回调通常会设 needs_redraw=true; 若没设, 兜底设一下. */
                    if (state->needs_redraw) {
                        state->list_dialog_local_update = true;
                    }
                } else {
                    /* 默认: 上下移动选中项 (wrap). */
                    if (action == MENU_ACTION_UP || action == MENU_ACTION_LEFT) {
                        if (state->list_dialog_selected > 0) {
                            state->list_dialog_selected--;
                        } else {
                            state->list_dialog_selected = state->list_dialog_count - 1;
                        }
                    } else {
                        if (state->list_dialog_selected < state->list_dialog_count - 1) {
                            state->list_dialog_selected++;
                        } else {
                            state->list_dialog_selected = 0;
                        }
                    }
                    state->needs_redraw = true;
                    state->list_dialog_local_update = true;
                }
                break;
            case MENU_ACTION_CONFIRM: {
                int idx = state->list_dialog_selected;
                if (idx == state->list_dialog_count - 1) {
                    /* "返回"项: 若处于嵌套弹窗, 恢复父弹窗(保持原选中位置);
                     * 否则关闭弹窗回到 list_dialog_return_page.
                     * 用户需求: 弹窗返回上级弹窗需停留在原来位置. */
                    if (list_dialog_pop_parent(state)) {
                        break;
                    }
                    state->list_dialog_active = false;
                    state->list_dialog_prev_active = false;
                    state->list_dialog_prev_selected = -1;
                    state->current_page = state->list_dialog_return_page;
                    if (state->list_dialog_return_page == MENU_PAGE_MAIN) {
                        state->selected_index = state->main_selected_index;
                    } else {
                        state->selected_index = 0;
                    }
                    state->scroll_offset = 0;
                    state->needs_redraw = true;
                    /* 弹窗关闭后回调 (用于: 时间弹窗关闭后回到设置弹窗) */
                    if (state->list_dialog_on_close) {
                        void (*cb)(menu_state_t *) = state->list_dialog_on_close;
                        state->list_dialog_on_close = NULL;  /* 防止递归: 关闭 settings 时不会再触发 */
                        cb(state);
                    }
                } else if (state->list_dialog_on_select) {
                    state->list_dialog_on_select(state, idx);
                }
                break;
            }
            case MENU_ACTION_BACK:
                /* BACK: 若处于嵌套弹窗, 恢复父弹窗(保持原位置);
                 * 否则关闭弹窗, 保持调用者页面原选中位置 (主菜单恢复 main_selected_index) */
                if (list_dialog_pop_parent(state)) {
                    break;
                }
                state->list_dialog_active = false;
                state->list_dialog_prev_active = false;
                state->list_dialog_prev_selected = -1;
                state->current_page = state->list_dialog_return_page;
                if (state->list_dialog_return_page == MENU_PAGE_MAIN) {
                    state->selected_index = state->main_selected_index;
                } else {
                    state->selected_index = 0;
                }
                state->scroll_offset = 0;
                state->needs_redraw = true;
                /* BACK 也触发关闭回调 (让时间弹窗按 BACK 时也能回到设置弹窗) */
                if (state->list_dialog_on_close) {
                    void (*cb)(menu_state_t *) = state->list_dialog_on_close;
                    state->list_dialog_on_close = NULL;
                    cb(state);
                }
                break;
            default:
                break;
        }
        return;
    }

    /* 编辑模式优先 */
    if (state->editing_index >= 0) {
        const sub_page_def_t *page = &sub_pages[state->current_page];
        switch (action) {
            case MENU_ACTION_LEFT:
                if (page->on_left_right) {
                    page->on_left_right(state, state->editing_index, false);
                }
                state->needs_redraw = true;
                return;
            case MENU_ACTION_RIGHT:
                if (page->on_left_right) {
                    page->on_left_right(state, state->editing_index, true);
                }
                state->needs_redraw = true;
                return;
            case MENU_ACTION_CONFIRM:
            case MENU_ACTION_BACK:
                state->editing_index = -1;
                state->needs_redraw = true;
                return;
            default:
                return;
        }
    }

    if (state->current_page == MENU_PAGE_MAIN) {
        /* 主菜单: 左/右导航
         * 方向定义 (用户偏好):
         *   按右键 → 旧图标往左退, 新图标从右边进  (dir=-1)
         *   按左键 → 旧图标往右退, 新图标从左边进  (dir=+1)
         * Wrap 情况 (5→0 或 0→5) 走 cover-flow 动画, 旧选中从中央缩到邻居, 新选中从对侧进入 */
        switch (action) {
            case MENU_ACTION_LEFT:
                if (state->selected_index > 0) {
                    state->prev_selected = state->selected_index;
                    state->anim_direction = 1;  /* 旧图标向右退 */
                    state->anim_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    state->selected_index--;
                } else {
                    /* wrap: 0 -> total-1 (按左键从 0 到末尾)
                     * 走 cover-flow 动画: dir=+1 让 dock 向右滑
                     * 但因为是 wrap, prev=0 (旧选中) sel=total-1 (新选中)
                     * 视觉: 0 从中央缩到左邻居, total-1 从右进入中央 */
                    state->prev_selected = state->selected_index;  /* 0 (旧) */
                    state->anim_direction = 1;
                    state->anim_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    state->selected_index = menu_main_count(state) - 1;
                }
                state->needs_redraw = true;
                break;
            case MENU_ACTION_RIGHT:
                if (state->selected_index < menu_main_count(state) - 1) {
                    state->prev_selected = state->selected_index;
                    state->anim_direction = -1;  /* 旧图标向左退 (用户偏好) */
                    state->anim_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    state->selected_index++;
                } else {
                    /* wrap: total-1 -> 0 (按右键从末尾到 0)
                     * 走 cover-flow 动画: dir=-1 让 dock 向左滑
                     * 视觉: total-1 从中央缩到右邻居, 0 从左进入中央 */
                    state->prev_selected = state->selected_index;  /* total-1 (旧) */
                    state->anim_direction = -1;
                    state->anim_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    state->selected_index = 0;
                }
                state->needs_redraw = true;
                break;
            case MENU_ACTION_BACK:
                /* MSC 模式中, BACK 也走优雅重启 */
                {
                    extern bool usbh_msc_is_running(void);
                    if (usbh_msc_is_running()) {
                        state->confirm_active = true;
                        state->confirm_notice = true;
                        state->confirm_no_hint = true;
                        snprintf(state->confirm_title, sizeof(state->confirm_title), "正在退出");
                        snprintf(state->confirm_msg, sizeof(state->confirm_msg),
                                 "设备将重启...");
                        state->needs_redraw = true;
                        menu_render(state);
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        esp_restart();
                        return;
                    }
                }
                break;
            case MENU_ACTION_CONFIRM: {
                int idx = state->selected_index;
                int phys = menu_main_phys(state, idx);
                menu_page_t target = main_items[phys].sub_page;
                if (target == MENU_PAGE_RETURN_GAME) {
                    /* TODO: 返回游戏 */
                    return;
                }
                /* === 主菜单"手柄": 直接弹出配置弹窗 (与存储管理一致) ===
                 * 用户偏好"所有需要选择的二级菜单尽量使用弹窗形式":
                 * 在桌面选中手柄图标即弹出"手柄配置"弹窗 (添加设备/映射按键/连接记录/返回).
                 * 弹窗 BACK 或选"返回"项则回到主菜单, 选择具体动作则进入对应处理流程.
                 * V1.0.18 重构后: 统一走新 gamepad_list_open, 删除 sub_pages 全屏入口. */
                if (target == MENU_PAGE_GAMEPAD) {
                    state->main_selected_index = idx;
                    gamepad_list_open(state);
                    break;
                }
                /* === 主菜单"设置": 弹出设置弹窗 (时间/系统信息/返回) ===
                 * 与手柄/存储管理一致: 主菜单点击"设置"直接弹窗, 选具体项再进入子页.
                 * 弹窗 BACK 或选"返回"项则回到主菜单. */
                if (target == MENU_PAGE_SETTINGS) {
                    state->main_selected_index = idx;
                    open_settings_dialog(state);
                    break;
                }
                /* === 存储管理: 用户需求"独立到一级菜单栏, 使用 TF 卡图标" ===
                 * 选中后不进入 SD 子页 (那个页面基本是空壳), 而是直接打开 SD 卡管理弹窗,
                 * 弹窗关闭后通过 list_dialog_return_page = current_page (即主菜单) 返回. */
                if (target == MENU_PAGE_SETTINGS_SD) {
                    state->main_selected_index = idx;
                    open_sd_dialog(state);
                    break;
                }
                /* === 壁纸: 弹窗式设置 (手柄/存储同款) === */
                if (target == MENU_PAGE_WALLPAPER) {
                    state->main_selected_index = idx;
                    open_wallpaper_dialog(state);
                    break;
                }
                /* === 番茄钟: 弹窗式设置 === */
                if (target == MENU_PAGE_POMODORO) {
                    state->main_selected_index = idx;
                    open_pomodoro_dialog(state);
                    break;
                }
                /* 保存当前主菜单位置, 二级菜单返回时恢复 */
                state->main_selected_index = idx;
                state->current_page = target;
                state->selected_index = 0;
                state->scroll_offset = 0;
                state->needs_redraw = true;
                if (target == MENU_PAGE_BOOK) {
                    state->book_loaded = false;   /* 每次进入重新扫描书单 */
                }

                /* 进入对应二级菜单后立马后台加载该菜单的引擎核心 (按需加载, 释放主菜单时的内存压力)
                 * - 电子词典: 触发 gam4980 后台初始化 (libretro core + ROM 预读)
                 * - GB:       预分配 instance (含 struct gb_s ~17KB) 到内部 RAM
                 * 引擎后台加载为幂等操作, 重复进入不会重复加载. */
                if (target == MENU_PAGE_SELECT_GAME) {
                    state->select_mode = 0;  /* 电子词典 */
                    state->game_display_mode = s_bbk_display;
                    engine_manager_load(ENGINE_GAM4980, state->lcd);
                } else if (target == MENU_PAGE_GB_GAME) {
                    state->select_mode = 1;  /* V1.0.46: console 使用电子词典分栏菜单 */
                    /* V1.0.49: 平台按主菜单物理索引映射 (1=GB,2=GBC,3=FC,4=arduboy)
                     * select_engine: 0=GB, 1=GBC, 2=FC(NES 合并), 3=arduboy */
                    state->select_engine = phys - 1;
                    /* V1.0.49: 同步当前引擎的独立灰度设置到 state->game_gray_mode */
                    state->game_gray_mode = engine_gray_get(state->select_engine);
                    /* V1.0.60: 同步当前引擎的独立显示模式 */
                    state->game_display_mode = engine_display_get(state->select_engine);
                    if (state->select_engine == 1) engine_manager_load(ENGINE_GBC, state->lcd);
                    else if (state->select_engine == 0) engine_manager_load(ENGINE_GB, state->lcd);
                    else if (state->select_engine == 2) engine_manager_load(ENGINE_NES, state->lcd);
                    else if (state->select_engine == 3) engine_manager_load(ENGINE_ARDUBOY, state->lcd);
                    /* V1.0.67: 进入 GB 游戏菜单不再自动弹辅助键映射提示/自动映射,
                     * 用户需要时自行到手柄设置里映射 (见 gamepad_act_gb_auxmap 入口). */
                }
                /* V1.0.46: 切换页面必须重置分栏缓存,
                 * 否则 GB/电子词典共用 select_loaded + 文件夹/游戏缓存导致内容串页 */
                state->select_loaded = false;
                state->select_focus = 1;        /* V1.0.68: 内容页默认选中第一个 */
                state->select_folder_idx = 1;   /* 默认收藏 */
                state->select_folder_scroll = 0;
                state->select_game_idx = 0;
                state->select_game_scroll = 0;
                s_cached_folder_idx = -1;
                s_cached_game_count = 0;
                s_cache_dirty = true;
                break;
            }
            default:
                break;
        }
        return;
    }

    /* 子页处理 */
    const sub_page_def_t *page = &sub_pages[state->current_page];
    /* 诊断: GB 页面按键到达子页处理 */
    if (state->current_page == MENU_PAGE_GB_GAME) {
        ESP_LOGI(TAG, "[GB] action=%d page=%d focus=%d sub_count=%d game_idx=%d",
                 (int)action, state->current_page, state->select_focus,
                 g_sub_count, state->select_game_idx);
    }

    /* 已删除: 原 confirm 弹窗处理已上移至 list_dialog 分支之后 (弹窗绝对优先) */

    switch (action) {
        case MENU_ACTION_UP:
            /* 电子词典: 左右分栏, 上下键在当前焦点列内选择 */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                if (state->select_focus == 0) {
                    int max_left = 2 + g_folder_count;
                    if (state->select_folder_idx > 0) state->select_folder_idx--;
                    else state->select_folder_idx = max_left;  /* 循环到末尾 */
                    /* 切换文件夹后重置右栏选中与滚动, 避免旧文件夹的偏移导致新列表只显示底部 */
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                } else {
                    /* 右栏: 游戏 (无末尾"返回"项, 用 BACK 键返回主菜单) */
                    if (state->select_game_idx > 0) state->select_game_idx--;
                    else if (g_sub_count > 0) state->select_game_idx = g_sub_count - 1;
                }
                state->needs_redraw = true;
                break;
            }
            /* GB: 分栏导航, 上下键在当前焦点列内选择
             * V1.0.46: 左栏可移动 (修复游戏设置无法打开) */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                if (state->select_focus == 0) {
                    /* 左栏: 按页面模式 */
                    int max_left = 2 + g_folder_count;
                    if (state->select_folder_idx > 0) state->select_folder_idx--;
                    else state->select_folder_idx = max_left;
                    /* 切换文件夹后重置右栏选中与滚动 */
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                } else if (g_sub_count > 0) {
                    if (state->select_game_idx > 0) state->select_game_idx--;
                    else state->select_game_idx = g_sub_count - 1;
                }
                state->needs_redraw = true;
                break;
            }
            if (state->selected_index > 0) state->selected_index--;
            else state->selected_index = g_sub_count - 1;
            state->needs_redraw = true;
            break;
        case MENU_ACTION_DOWN:
            /* 电子词典: 左右分栏, 上下键在当前焦点列内选择 */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                if (state->select_focus == 0) {
                    int max_left = 2 + g_folder_count;
                    if (state->select_folder_idx < max_left) state->select_folder_idx++;
                    else state->select_folder_idx = 0;
                    /* 切换文件夹后重置右栏选中与滚动 */
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                } else {
                    /* 右栏: 游戏 (无末尾"返回"项) */
                    if (state->select_game_idx < g_sub_count - 1) state->select_game_idx++;
                    else state->select_game_idx = 0;
                }
                state->needs_redraw = true;
                break;
            }
            /* GB: 分栏导航, 上下键在当前焦点列内选择 */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                if (state->select_focus == 0) {
                    /* 左栏: 按页面模式 */
                    int max_left = 2 + g_folder_count;
                    if (state->select_folder_idx < max_left) state->select_folder_idx++;
                    else state->select_folder_idx = 0;
                    /* 切换文件夹后重置右栏选中与滚动 */
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                } else if (g_sub_count > 0) {
                    if (state->select_game_idx < g_sub_count - 1) state->select_game_idx++;
                    else state->select_game_idx = 0;
                }
                state->needs_redraw = true;
                break;
            }
            if (state->selected_index < g_sub_count - 1) state->selected_index++;
            else state->selected_index = 0;
            state->needs_redraw = true;
            break;
        case MENU_ACTION_LEFT: {
            /* 编辑模式下: 左右键修改当前项 */
            if (state->editing_index >= 0) {
                if (page->on_left_right) {
                    page->on_left_right(state, state->editing_index, false);
                    state->needs_redraw = true;
                }
                break;
            }
            /* MP3 界面: 左右键调整音量 (0-9 共 10 档) */
            if (state->current_page == MENU_PAGE_MP3_PLAYER) {
                int vol = (int)state->settings.volume - 1;
                if (vol < 0) vol = 0;
                state->settings.volume = (uint8_t)vol;
                audio_player_set_volume(volume_step_to_percent(vol));
                /* 持久化: 下次启动恢复当前音量 */
                menu_config_save();
                state->needs_redraw = true;
                break;
            }
            /* 电子词典: 左右键切换分栏焦点 (左=文件夹, 右=游戏) */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                state->select_focus = 0;
                state->needs_redraw = true;
                break;
            }
            /* GB/GBC: 左键切到左栏 */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                state->select_focus = 0;
                state->needs_redraw = true;
                break;
            }
            /* 非编辑模式: 左右键 = 上下导航 */
            if (state->selected_index > 0) state->selected_index--;
            else state->selected_index = g_sub_count - 1;
            state->needs_redraw = true;
            break;
        }
        case MENU_ACTION_RIGHT: {
            /* 编辑模式下: 左右键修改当前项 */
            if (state->editing_index >= 0) {
                if (page->on_left_right) {
                    page->on_left_right(state, state->editing_index, true);
                    state->needs_redraw = true;
                }
                break;
            }
            /* MP3 界面: 左右键调整音量 (0-10 共 11 档) */
            if (state->current_page == MENU_PAGE_MP3_PLAYER) {
                int vol = (int)state->settings.volume + 1;
                if (vol > 10) vol = 10;
                state->settings.volume = (uint8_t)vol;
                audio_player_set_volume(volume_step_to_percent(vol));
                /* 持久化: 下次启动恢复当前音量 */
                menu_config_save();
                state->needs_redraw = true;
                break;
            }
            /* 电子词典: 左右键切换分栏焦点 (左=文件夹, 右=游戏) */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                /* 右栏没游戏时不允许切过去 (避免空列表) */
                if (g_sub_count > 0) {
                    state->select_focus = 1;
                    /* 切到右栏时, 把游戏选中项重置到第一行 (idx=0),
                     * 与 CONFIRM 路径行为一致, 避免停在之前浏览过的位置. */
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                }
                state->needs_redraw = true;
                break;
            }
            /* GB/GBC: 右键切到右栏 */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                if (g_sub_count > 0) {
                    state->select_focus = 1;
                    state->select_game_idx = 0;
                    state->select_game_scroll = 0;
                }
                state->needs_redraw = true;
                break;
            }
            /* 非编辑模式: 左右键 = 上下导航 */
            if (state->selected_index < g_sub_count - 1) state->selected_index++;
            else state->selected_index = 0;
            state->needs_redraw = true;
            break;
        }
        case MENU_ACTION_CONFIRM: {
            int idx = state->selected_index;
            /* 编辑模式下: 确认键退出编辑 */
            if (state->editing_index >= 0) {
                state->editing_index = -1;
                state->needs_redraw = true;
                break;
            }
            /* 电子词典: 按当前焦点列发送对应索引到 on_confirm */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                /* 左栏: 单击切换焦点到右栏 (右栏均有内容: 设置/收藏/文件夹游戏)
                 * 与"游戏文件夹"风格一致: 点左即显右, 不再跳转独立子页 */
                if (state->select_focus == 0) {
                    if (g_sub_count > 0) {
                        state->select_focus = 1;
                        state->select_game_idx = 0;
                        state->select_game_scroll = 0;
                    } else {
                        /* 右栏无内容 (理论上不会发生, 但作为防御) */
                        ESP_LOGW(TAG, "右栏无内容, 无法切换焦点 (folder_idx=%d)",
                                 state->select_folder_idx);
                    }
                    state->needs_redraw = true;
                    break;
                }
                /* 右栏: 触发对应动作 (游戏设置/启动游戏/启动收藏游戏) */
                int gidx = state->select_game_idx;
                /* 先让页面处理确认 (select_game_on_confirm)
                 *  - select_folder_idx == 0: 转发到 game_settings_on_confirm
                 *  - select_folder_idx == 1: 启动收藏游戏
                 *  - select_folder_idx >= 2: 启动文件夹游戏 */
                if (page->on_confirm && page->on_confirm(state, gidx)) {
                    state->needs_redraw = true;
                }
                break;
            }
            /* GB/GBC: 分栏确认 (左栏→切到右栏, 右栏→启动游戏) */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                if (state->select_focus == 0) {
                    /* 左栏: 切换到右栏 */
                    if (g_sub_count > 0) {
                        state->select_focus = 1;
                        state->select_game_idx = 0;
                        state->select_game_scroll = 0;
                    }
                    state->needs_redraw = true;
                    break;
                }
                /* 右栏: 启动游戏 */
                int gidx = state->select_game_idx;
                if (page->on_confirm && page->on_confirm(state, gidx)) {
                    state->needs_redraw = true;
                }
                break;
            }
            /* MP3 页面: 直接播放, 最后一个文件不是返回 */
            if (state->current_page == MENU_PAGE_MP3_PLAYER) {
                if (page->on_confirm && page->on_confirm(state, idx)) {
                    state->needs_redraw = true;
                }
                break;
            }
            /* 游戏设置: 没有"返回"项, 直接交给 on_confirm (idx 0..3 都是有效子项) */
            if (state->current_page == MENU_PAGE_GAME_SETTINGS) {
                if (page->on_confirm && page->on_confirm(state, idx)) {
                    state->needs_redraw = true;
                }
                break;
            }
            /* 返回项: 直接返回主菜单 */
            if (idx == g_sub_count - 1) {
                state->current_page = MENU_PAGE_MAIN;
                state->selected_index = state->main_selected_index;
                state->scroll_offset = 0;
                state->needs_redraw = true;
                break;
            }
            /* 有左右切换的项: 确认键进入编辑模式 */
            if (page->on_left_right) {
                state->editing_index = idx;
                state->needs_redraw = true;
                break;
            }
            /* 先让页面处理确认 */
            if (page->on_confirm && page->on_confirm(state, idx)) {
                state->needs_redraw = true;
                break;
            }
            break;
        }
        case MENU_ACTION_BACK:
            /* 编辑模式下: BACK 退出编辑 */
            if (state->editing_index >= 0) {
                state->editing_index = -1;
                state->needs_redraw = true;
                break;
            }
            /* 电子词典: 在分栏中按 BACK 回到主菜单, 并重置分栏状态 */
            if (state->current_page == MENU_PAGE_SELECT_GAME) {
                state->current_page = MENU_PAGE_MAIN;
                state->selected_index = state->main_selected_index;
                state->scroll_offset = 0;
                state->select_loaded = false;
                state->select_focus = 1;
                state->select_folder_idx = 1;  /* 默认下次进入显示"收藏" */
                state->select_folder_scroll = 0;
                state->select_game_idx = 0;
                state->select_game_scroll = 0;
                state->needs_redraw = true;
                /* 返回主菜单: 统一卸载全部引擎, 释放其占用的内存 (PSRAM 等) */
                engine_manager_unload_all();
                break;
            }
            /* GB/GBC: BACK 返回主菜单, 重置分栏状态, 释放引擎核心内存 */
            if (state->current_page == MENU_PAGE_GB_GAME) {
                bool was_gb = (state->current_page == MENU_PAGE_GB_GAME);
                state->current_page = MENU_PAGE_MAIN;
                state->selected_index = state->main_selected_index;
                state->scroll_offset = 0;
                state->select_focus = 0;
                state->select_game_idx = 0;
                state->select_game_scroll = 0;
                state->needs_redraw = true;
                /* 返回主菜单: 统一卸载全部引擎, 释放其占用的内存 */
                engine_manager_unload_all();
                break;
            }
            /* 文件浏览器: 已改为 list_dialog 弹窗, BACK 走 list_dialog 通用处理 */
            /* MP3 播放器: 退出时停止音频 */
            if (state->current_page == MENU_PAGE_MP3_PLAYER) {
                mp3_exit_cleanup();
            }
            /* 返回主菜单, 恢复之前的主菜单位置 */
            state->current_page = MENU_PAGE_MAIN;
            state->selected_index = state->main_selected_index;
            state->scroll_offset = 0;
            state->needs_redraw = true;
            break;
        default:
            break;
    }
}
