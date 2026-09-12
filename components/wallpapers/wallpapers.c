/* 内置壁纸程序 (V4): 星空（含底部驾驶舱）.
 * 从 menu_system 屏保完整移植到本库, 供 os/桌面 与 屏保 共用.
 * 依赖: st7305(clear/pixel) + ui_common(ascii 小字). */
#include "wallpapers.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_common.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"  /* 屏保监视任务到点强制软关机 */
#include "esp_wifi.h"   /* 待机深睡前停 WiFi, 避免 modem sleep 白耗电 */
#include "driver/gpio.h" /* GPIO_NUM_*: 深睡 ext1 唤醒掩码 */
#include "os.h"         /* os_current_page: 深睡前保存原页面, 唤醒后回原位 */
#include "audio_player.h" /* 进入壁纸暂停音乐, 唤醒恢复 */
#include "bt_manager.h"   /* 屏保省电: 锁屏关蓝牙 / 唤醒开蓝牙+自动回连 */
#include "board_rlcd.h"   /* 屏保独占 LCD: 冻结游戏帧刷新, 壁纸恒在最上层 */
#include "input.h"        /* 统一"最近输入"时钟: 独立监视据此判定空闲 */

#define WP_W ST7305_WIDTH     /* 400 */
#define WP_H ST7305_HEIGHT    /* 300 */
#define WP_ROW ((WP_W + 7) / 8)          /* 50 */
#define WP_FB_BYTES (WP_ROW * WP_H)

static uint8_t *s_fb = NULL;

static void wp_ensure_fb(void) {
    if (!s_fb)
        s_fb = heap_caps_malloc(WP_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

const char *wp_prog_name(int id) {
    (void)id;
    return "\xe6\x98\x9f\xe7\xa9\xba";   /* 星空 */
}

/* ==== 星空屏保 (从 menu_system.c screensaver_render_stars 完整移植) ==== */
#define STAR_COUNT 105
EXT_RAM_BSS_ATTR static int   s_star_x[STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_star_y[STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_star_z[STAR_COUNT];
EXT_RAM_BSS_ATTR static float s_star_base_speed[STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_star_prev_px[STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_star_prev_py[STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_star_speed_type[STAR_COUNT]; /* 0=慢, 1=中, 2=快 */

#define BG_STAR_COUNT 75
EXT_RAM_BSS_ATTR static int   s_bg_star_x[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_bg_star_y[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static int   s_bg_star_z[BG_STAR_COUNT];
EXT_RAM_BSS_ATTR static float s_bg_star_speed[BG_STAR_COUNT];

static bool s_stars_ready = false;

/* 底部进度条装饰 (与原版一致) */
EXT_RAM_BSS_ATTR static float s_progress_decor;

static void stars_init(void) {
    for (int i = 0; i < STAR_COUNT; i++) {
        if (rand() % 10 < 7) {
            int side = (rand() % 2) ? 1 : -1;
            s_star_x[i] = side * (100 + (int)(rand() % (int)(WP_W * 1.2f)));
        } else {
            s_star_x[i] = (rand() % WP_W) - WP_W / 2;
        }
        s_star_y[i] = (rand() % (WP_H * 3)) - WP_H * 3 / 2;
        s_star_z[i] = 500 + rand() % 2000;
        s_star_speed_type[i] = i % 3;
        if (s_star_speed_type[i] == 0) s_star_base_speed[i] = 0.8f + (float)(rand() % 15) * 0.1f;
        else if (s_star_speed_type[i] == 1) s_star_base_speed[i] = 2.5f + (float)(rand() % 20) * 0.1f;
        else s_star_base_speed[i] = 5.0f + (float)(rand() % 35) * 0.1f;
        s_star_prev_px[i] = -1;
        s_star_prev_py[i] = -1;
    }
    for (int i = 0; i < BG_STAR_COUNT; i++) {
        if (rand() % 10 < 7) {
            int side = (rand() % 2) ? 1 : -1;
            s_bg_star_x[i] = side * (100 + (int)(rand() % (int)(WP_W * 1.2f)));
        } else {
            s_bg_star_x[i] = (rand() % WP_W) - WP_W / 2;
        }
        s_bg_star_y[i] = (rand() % (WP_H * 3)) - WP_H * 3 / 2;
        s_bg_star_z[i] = 500 + rand() % 2000;
        s_bg_star_speed[i] = 0.8f + (float)(rand() % 25) * 0.1f;
    }
    s_progress_decor = 0.0f;
    s_stars_ready = true;
}

void wp_release_buffers(void) {
    if (s_fb) {
        free(s_fb);
        s_fb = NULL;
    }
    s_stars_ready = false;   /* 下次渲染重新初始化 */
}

/* 面板小号数字串 (draw_ascii_small) */
static void wp_draw_panel_str(st7305_handle_t *lcd, int x, int y, const char *str) {
    for (int i = 0; str[i]; i++)
        draw_ascii_small(lcd, x + i * 8, y, str[i], false);
}

/* 当前时间 HH:MM (RTC) */
static void wp_time_str(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm ti;
    localtime_r(&t, &ti);
    snprintf(out, n, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

static void render_stars(st7305_handle_t *lcd, uint32_t now_ms) {
    if (!s_stars_ready) stars_init();

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int cx = WP_W / 2;
    int cy = 120;
    int max_z = 2500;

    /* 底部驾驶舱边界: 星星在其上方消失 */
    int cb = WP_H - 1, chC = 32;
    int ctC = cb - chC;
    int star_vanish_y = ctC + 5;
    int chLR = 22, stL = 105, stR = 295;
    int ctLR = cb - chLR;

    /* === 背景小星 (不变大, 始终 1px, 速度慢) === */
    for (int i = 0; i < BG_STAR_COUNT; i++) {
        s_bg_star_z[i] -= (int)(s_bg_star_speed[i]);
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
        if (px < 0 || px >= WP_W || py < 0 || py >= star_vanish_y) {
            s_bg_star_z[i] = max_z - (rand() % 300);
            float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
            float dist = 40.0f + (float)(rand() % 60);
            s_bg_star_x[i] = (int)(cos(angle) * dist);
            s_bg_star_y[i] = (int)(sin(angle) * dist);
            continue;
        }
        st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }

    /* === 主星: 105 颗, 三种速度 === */
    for (int i = 0; i < STAR_COUNT; i++) {
        float layer_speed;
        if (s_star_speed_type[i] == 0) layer_speed = 1.0f;
        else if (s_star_speed_type[i] == 1) layer_speed = 1.8f;
        else layer_speed = 3.0f;

        float z_ratio = (float)(max_z - s_star_z[i]) / (float)max_z;
        float accel;
        if (z_ratio < 0.3f) accel = 0.5f + z_ratio * 1.3f;
        else if (z_ratio < 0.7f) accel = 0.9f + (z_ratio - 0.3f) * 1.1f;
        else accel = 1.34f + (z_ratio - 0.7f) * 0.5f;
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

        if (px < -10 || px >= WP_W + 10 || py < -10 || py >= star_vanish_y) {
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

        /* 短尾迹 (中/快速度星) */
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
                    if (tx < 0 || tx >= WP_W || ty < 0 || ty >= star_vanish_y) continue;
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
                        if (dx >= 0 && dx < WP_W && dy >= 0 && dy < star_vanish_y)
                            st7305_draw_pixel(lcd, dx, dy, ST7305_COLOR_BLACK);
                    }
                }
            }
        }
        s_star_prev_px[i] = px;
        s_star_prev_py[i] = py;
    }

    /* === 底部驾驶舱 (原版: 三级台阶 + 保护罩 + 旋臂星系 + 速度/进度条) === */
    int cr = 4;
    /* 左台阶 */
    for (int x = 0; x <= stL; x++) {
        int inCorner = (x < cr);
        if (!inCorner || (cr - x) * (cr - x) <= cr * cr)
            st7305_draw_pixel(lcd, x, ctLR, ST7305_COLOR_BLACK);
    }
    for (int y = ctLR; y <= cb; y++)
        st7305_draw_pixel(lcd, 0, y, ST7305_COLOR_BLACK);

    int dmx = WP_W / 2, dmc = ctC - 1, dmr = 80;
    int shield_r = dmr * 2 / 3;       /* 保护罩半径 53 */
    int shield_left = dmx - shield_r - 2;
    int shield_right = dmx + shield_r + 2;

    /* 中台阶 (跳过3D罩区域) */
    for (int x = stL + 5; x <= 159; x++) {
        if (x >= shield_left && x <= shield_right) continue;
        st7305_draw_pixel(lcd, x, ctC, ST7305_COLOR_BLACK);
    }
    for (int x = 241; x <= stR - 5; x++) {
        if (x >= shield_left && x <= shield_right) continue;
        st7305_draw_pixel(lcd, x, ctC, ST7305_COLOR_BLACK);
    }
    /* 右台阶 */
    for (int x = stR; x < WP_W; x++) {
        int inCorner = (WP_W - 1 - x < cr);
        if (!inCorner || ((WP_W - 1 - x) * (WP_W - 1 - x) <= cr * cr))
            st7305_draw_pixel(lcd, x, ctLR, ST7305_COLOR_BLACK);
    }
    for (int y = ctLR; y <= cb; y++)
        st7305_draw_pixel(lcd, WP_W - 1, y, ST7305_COLOR_BLACK);
    /* 底部贯通 */
    for (int x = 0; x < WP_W; x++)
        st7305_draw_pixel(lcd, x, cb, ST7305_COLOR_BLACK);
    /* 45度斜边连接左右台阶到中台阶 */
    for (int y = ctLR; y >= ctC; y--) {
        int off = (int)((float)(ctLR - y) * 0.5f);
        st7305_draw_pixel(lcd, stL + off, y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, stR - off, y, ST7305_COLOR_BLACK);
    }

    /* === 半球 (保护罩缩小三分之一) + 旋臂亮点 (缩小五分之一) === */
    int gx = dmx, gy = dmc - 16;
    float arm_scale = 0.8f;

    /* 底座椭圆 */
    {
        int ea = shield_r, eb = 13, ecx = dmx, ecy = dmc;
        int lastPx = -1, lastPy = -1, dashCnt = 0;
        for (int a = 0; a < 360; a++) {
            float rad = (float)a * 3.14159f / 180.0f;
            int pEx = ecx + (int)(ea * cos(rad));
            int pEy = ecy + (int)(eb * sin(rad));
            if (pEx < 0 || pEx >= WP_W || pEy < 0 || pEy >= WP_H) continue;
            if (a < 180) {
                st7305_draw_pixel(lcd, pEx, pEy, ST7305_COLOR_BLACK);
            } else {
                if (pEx != lastPx || pEy != lastPy) {
                    dashCnt++;
                    if (dashCnt % 3 == 1)
                        st7305_draw_pixel(lcd, pEx, pEy, ST7305_COLOR_BLACK);
                    lastPx = pEx; lastPy = pEy;
                }
            }
        }
    }
    /* 保护罩上半圆 */
    for (int a = 0; a <= 180; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int pX = dmx + (int)(shield_r * cos(rad));
        int pY = dmc - (int)(shield_r * sin(rad));
        if (pX >= 0 && pX < WP_W && pY >= 0 && pY < WP_H)
            st7305_draw_pixel(lcd, pX, pY, ST7305_COLOR_BLACK);
    }
    /* 4臂旋涡星系 */
    {
        int arm_bound = (int)((float)(shield_r - 4) * arm_scale);
        for (int arm = 0; arm < 4; arm++) {
            float arm_off = (float)arm * 90.0f * 3.14159f / 180.0f;
            for (int r = 5; r < 46; r++) {
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
        /* 中心亮点 */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -6; dx <= 6; dx++)
                if (dx * dx * 2 + dy * dy * 3 < 26) {
                    int px2 = gx + dx, py2 = gy + dy;
                    int ddx = px2 - dmx, ddy = py2 - dmc;
                    if (ddx * ddx + ddy * ddy < (shield_r - 3) * (shield_r - 3))
                        st7305_draw_pixel(lcd, px2, py2, ST7305_COLOR_BLACK);
                }
        /* 旋臂亮点 */
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

    /* 内容: 速度值+单位左 / 时间右 */
    int speed_val = 8000 + (int)(sin((float)now_ms / 1000.0f) * 200);
    if (speed_val < 0) speed_val = 0;
    if (speed_val > 99999) speed_val = 99999;
    char speed_str[12];
    snprintf(speed_str, sizeof(speed_str), "%05d", speed_val);
    wp_draw_panel_str(lcd, 20, ctLR + 5, speed_str);
    wp_draw_panel_str(lcd, 20 + (int)strlen(speed_str) * 8 + 2, ctLR + 5, "KM/H");

    char time_str[12];
    wp_time_str(time_str, sizeof(time_str));
    wp_draw_panel_str(lcd, 320, ctLR + 5, time_str);

    /* 进度条 (约5分钟到达) */
    int prog_y = cb - 12;
    int prog_x0 = stL + 20, prog_x1 = stR - 20;
    s_progress_decor += 0.0055f;
    if (s_progress_decor > 100.0f) s_progress_decor -= 100.0f;
    for (int x = prog_x0; x <= prog_x1; x++) {
        st7305_draw_pixel(lcd, x, prog_y - 2, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x, prog_y + 1, ST7305_COLOR_BLACK);
    }
    int fill_w = (prog_x1 - prog_x0 - 2) * (int)s_progress_decor / 100;
    for (int x = prog_x0 + 1; x <= prog_x0 + 1 + fill_w; x++) {
        st7305_draw_pixel(lcd, x, prog_y - 1, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, x, prog_y, ST7305_COLOR_BLACK);
    }
}

void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms) {
    if (prog == WP_PROG_STARS) { render_stars(lcd, now_ms); return; }
    wp_ensure_fb();
}
/* ==== 屏保状态机 (从 os_mgr 迁移归位: 屏保职责归壁纸模块) ==== */
#define WP_TAG "WPSS"
static bool s_ss_active = false;
static bool s_ss_manual = false;   /* 锁屏键点按强制进入, 不自动复位 */
/* V1.5.x 壁纸(屏保)运行计时与软关机请求 (声明须在使用之前) */
static uint32_t s_ss_run_ms = 0;     /* 缓存; 0=尚未从 NVS 加载 */
static uint32_t s_ss_enter_ms = 0;   /* 进入屏保的时刻 (壁纸运行计时起点) */
static bool    s_ss_shutdown = false;/* 壁纸超时, 请求软关机 */
/* 进入壁纸暂停一切: 记录是否因屏保而暂停了音乐, 唤醒时恢复 */
/* 进入壁纸暂停一切: 强制全局静音 — 停 MP3(不再自动续播) + 屏蔽引擎 PCM + 编解码器音量0.
 * s_ss_muted_audio 标记是否由本屏保静音, 唤醒时只还原我们静音的那一次(不覆盖用户设置态). */
static bool s_ss_muted_audio = false;
static void ss_mute_audio(void) { if (s_ss_muted_audio) return; audio_player_set_muted(true); s_ss_muted_audio = true; }
static void ss_unmute_audio(void) { if (!s_ss_muted_audio) return; audio_player_set_muted(false); s_ss_muted_audio = false; }

bool os_screensaver_active(void) { return s_ss_active; }
void os_screensaver_reset(void)
{
    bool was = s_ss_active;
    if (s_ss_active) ESP_LOGI(WP_TAG, "屏保退出 (输入唤醒)");
    s_ss_active = false;
    s_ss_manual = false;
    s_ss_shutdown = false;   /* 唤醒即取消软关机请求 */
    /* 屏保退出: 解除 LCD 独占 + 解除刷屏门禁, 后台程序/引擎可恢复 */
    board_rlcd_screensaver_set(false);
    st7305_set_flush_blocked(false);
    input_set_touch_blocked(false);   /* 唤醒: 恢复触屏 -- 休眠期间禁触屏, 仅物理键/手柄唤醒 */
    /* 唤醒: 还原由本屏保静音的音频 (MP3 保持停止, 用户需手动重播) */
    ss_unmute_audio();
    /* 从屏保唤醒: 重新开工业蓝牙, 自动回连任务会接上次手柄 */
    if (was) bt_manager_enable();
}
void os_screensaver_force_enter(void)
{
    s_ss_active = true;
    s_ss_manual = true;
    s_ss_enter_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_ss_shutdown = false;
    /* 屏保独占 LCD + 关闭刷屏门禁 → 所有程序/引擎(含无暂停接口的步步高/文曲星)
     * 都无法再往屏上写, 视觉上"全部暂停", 壁纸恒在最上层且不再与后台互闪 */
    board_rlcd_screensaver_set(true);
    st7305_set_flush_blocked(true);
    /* 触摸屏蔽随"触摸唤醒"设置: 开=不禁触屏, 允许触摸长按1秒唤醒(走 os_core);
     * 关=完全禁触屏, 触摸不响应也不刷新时钟, 仅物理键/手柄可唤醒. */
    input_set_touch_blocked(!wp_screensaver_touch_wake_enabled());
    /* 进入壁纸暂停一切: 强制全局静音(停 MP3 + 屏蔽引擎 PCM) */
    ss_mute_audio();
    bt_manager_disable();   /* 锁屏: 彻底关蓝牙(最省电) */
    ESP_LOGI(WP_TAG, "锁屏键点按: 进入壁纸屏保 (触摸唤醒%s)", 
             wp_screensaver_touch_wake_enabled() ? "开" : "关");
}
/* 息屏(星空屏保)延时: 读壁纸 NVS "os_wp"/"min" (分钟).
 *   min<=0 = 自动不息眠 (返回 UINT32_MAX, 空闲差永远到不了);
 *   未在 NVS 设置时默认 3 分钟. */
static uint32_t s_ss_delay_ms = 0;   /* 缓存; 0=尚未从 NVS 加载 */
static uint32_t wp_ss_delay_ms(void)
{
    if (s_ss_delay_ms) return s_ss_delay_ms;
    int min = 3;
    nvs_handle_t h;
    if (nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "min", &t) == ESP_OK) min = (int)t;
        nvs_close(h);
    }
    s_ss_delay_ms = (min <= 0) ? UINT32_MAX : ((uint32_t)min * 60000UL);
    return s_ss_delay_ms;
}
void wp_screensaver_set_delay_ms(uint32_t ms) { s_ss_delay_ms = ms; }
uint32_t wp_screensaver_delay_ms(void) { return wp_ss_delay_ms(); }   /* 供阻塞引擎循环在内部做空闲息屏 */
/* 壁纸(屏保)运行时长: 读壁纸 NVS "os_wp"/"wp_run" (分钟).
 *   未在 NVS 设置时默认 10 分钟; 运行满时长后请求软关机 (任意按键唤醒). */
static uint32_t wp_ss_run_ms(void)
{
    if (s_ss_run_ms) return s_ss_run_ms;
    int min = 10;
    nvs_handle_t h;
    if (nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "wp_run", &t) == ESP_OK) min = (int)t;
        nvs_close(h);
    }
    s_ss_run_ms = (min <= 0) ? UINT32_MAX : ((uint32_t)min * 60000UL);
    return s_ss_run_ms;
}
void wp_screensaver_set_run_ms(uint32_t ms) { s_ss_run_ms = ms; }
/* 关机时间(休眠一直没唤醒→软关机): 读 NVS "os_wp"/"wp_shut"(分钟); 0/未设=不自动关机.
 * 深睡定时器 = 休眠多久后无任何唤醒(无人按 PWR)则自动转"彻底关机"(由 main 定时器唤醒分支处理). */
static uint32_t s_ss_shut_ms = 0;   /* 缓存的关机时间 ms; 0=等价于不自动关机 */
static uint32_t wp_ss_shut_ms(void)
{
    if (s_ss_shut_ms) return s_ss_shut_ms;
    int min = 0;
    nvs_handle_t h;
    if (nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "wp_shut", &t) == ESP_OK) min = (int)t;
        nvs_close(h);
    }
    s_ss_shut_ms = (min <= 0) ? 0 : ((uint32_t)min * 60000UL);
    return s_ss_shut_ms;
}
void wp_screensaver_set_shutdown_ms(uint32_t ms) { s_ss_shut_ms = ms; }
bool wp_screensaver_shutdown_requested(void) { return s_ss_shutdown; }
/* V1.5.x: 触摸唤醒屏保开关 — 默认关 (触摸不唤醒壁纸, 仅物理键/手柄唤醒).
 * NVS "os_wp"/"wp_touch"; -1=尚未从 NVS 加载. */
static int s_touch_wake = -1;
bool wp_screensaver_touch_wake_enabled(void)
{
    if (s_touch_wake < 0) {
        int v = 0;
        nvs_handle_t h;
        if (nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
            int32_t t = 0;
            if (nvs_get_i32(h, "wp_touch", &t) == ESP_OK) v = (int)t;
            nvs_close(h);
        }
        s_touch_wake = v;
    }
    return s_touch_wake != 0;
}
void wp_screensaver_set_touch_wake(bool en)
{
    s_touch_wake = en ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open("os_wp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "wp_touch", s_touch_wake);
        nvs_commit(h);
        nvs_close(h);
    }
}
void wp_screensaver_poll(uint32_t now_ms, uint32_t last_input_ms, bool in_dialog)
{
    if (s_ss_active) {
        /* 手动锁屏(force_enter)未记录起点时补记 */
        if (s_ss_enter_ms == 0) s_ss_enter_ms = now_ms;
        /* 软关机: 从壁纸出现算起, 播放满"壁纸运行时长"(wp_ss_run_ms) → 请求软关机.
         * 注意: 用 wp_ss_run_ms (壁纸运行时长) 而非 wp_ss_delay_ms (息屏延时). */
        if (!s_ss_shutdown && now_ms - s_ss_enter_ms >= wp_ss_run_ms()) {
            s_ss_shutdown = true;
            ESP_LOGI(WP_TAG, "壁纸播放满运行时长, 请求软关机");
        }
        return;
    }
    if (s_ss_manual) return;   /* 手动锁屏未激活时不自动进入 */
    bool idle_long = (last_input_ms != 0 && now_ms - last_input_ms >= wp_ss_delay_ms());
    /* 本函数只负责"进入"判定. 退出完全由 os_core 输入唤醒路径 (长按1秒/按键/POWER_LOCK)
     * 驱动: 这里不再按空闲状态自动复位, 否则任何触摸(含轻触一下)更新 last_input_ms
     * 都会把 idle_long 置否而立刻退出屏保 —— 正是"壁纸碰一下就退出"的根因.
     * V9.1: 去掉 on_main 门控 —— 任何界面 (含游戏) 超过设置延时无输入都进入休眠,
     * 游戏输入经 input_mark_activity 刷新 last_input_ms, 故"正在玩不睡, 放下手柄睡". */
    if (!in_dialog && idle_long && !s_ss_active) {
        s_ss_active = true;
        s_ss_enter_ms = now_ms;
        s_ss_shutdown = false;
        /* 屏保独占 LCD + 关闭刷屏门禁: 冻结所有程序/引擎, 壁纸恒在最上层 */
        board_rlcd_screensaver_set(true);
        st7305_set_flush_blocked(true);
        /* 进入壁纸暂停一切: 强制全局静音(停 MP3 + 屏蔽引擎 PCM) */
        ss_mute_audio();
        bt_manager_disable();   /* 空闲息屏: 同样关蓝牙省电 */
        ESP_LOGI(WP_TAG, "进入屏保 (星空)");
    }
}

/* ============ 独立屏保监视任务 (休眠为最高优先级) ============
 * 目标: "强制进入壁纸、壁纸覆盖一切、到达睡眠时间强制软关机".
 * 传统做法把休眠判定放主循环/页面内, 会被游戏(阻塞引擎循环)堵死, 导致
 * "游戏内无法休眠、退出游戏才休眠". 本任务独立于所有页面/游戏运行, 无论
 * 前台/后台在干什么, 只要统一输入时钟停摆超过设定延时就强制进入壁纸并
 * 独占渲染; 壁纸播放满"休眠时间"即强制深度休眠软关机. 主循环与游戏循环
 * 都不再自己判定进入, 统交本监视任务 → 口径唯一、绝不遗漏. */
#define SS_SUPER_STACK  8192   /* words(→32KB)：全屏星空渲染 + SPI 独占刷屏 + 时间格式化栈占用大，
                                * 8KB(xTaskCreate)曾触发 task sse 栈溢出→进屏保即重启。用 PSRAM 静态栈充盈 */
EXT_RAM_BSS_ATTR static StackType_t s_ss_stack[SS_SUPER_STACK];  /* 静态 PSRAM 栈(同 board_shim 视频任务模式) */
static StaticTask_t s_ss_tcb;
static st7305_handle_t *s_ss_lcd = NULL;

/* ==== 睡眠执行器 (内部RAM小栈) ====
 * ESP-IDF 规定: esp_*_sleep_start() 必须在"任务栈位于内部RAM"的任务中调用 —
 * 睡眠/待机瞬间会禁用 Flash/PSRAM 缓存, 若当前任务栈在 PSRAM, 被禁缓存后立即
 * assert 崩 (实测: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127
 *   esp_task_stack_is_sane_cache_disabled() 断言 → reset_reason=4, 入睡即重启循环).
 * 因此: 渲染用的大栈监督任务(PSRAM)只负责屏保/计时, 真正的入睡动作交给本内部RAM
 * 小栈执行器任务. 官方文档明确: "tickless idle 自动浅睡由内部RAM idle 任务发起才安全",
 * 本设计即对齐该约束. */
#define SS_SLEEP_STACK 1536   /* words=6KB 内部RAM: 睡眠循环调用栈足够 */
static StackType_t s_sleep_stack[SS_SLEEP_STACK];   /* 内部RAM(非 EXT_RAM_BSS) */
static StaticTask_t s_sleep_tcb;
static TaskHandle_t s_sleep_task_h = NULL;
static volatile bool s_sleep_run_srv = false;  /* 监督任务请求入睡 */
static volatile bool s_sleep_wake_key = false; /* 按键唤醒(返回主界面) */
static uint32_t s_sleep_shut_ms = 0;           /* 关机时间(到点无人按键→转深睡软关机) */
static uint32_t s_sleep_start_ms = 0;

/* 睡眠执行器: 只在内部RAM栈上调用 esp_ light/deep sleep, 规避缓存禁用断言. */
static void ss_sleep_runner(void *arg) {
    (void)arg;
    const uint64_t wake_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_18);
    for (;;) {
        if (!s_sleep_run_srv) { vTaskSuspend(NULL); continue; }   /* 闲置等派活 */
        s_sleep_wake_key = false;
        for (;;) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            /* 关机时间到 → 真软关机(深睡, 仅 PWR), 不返回 */
            if (s_sleep_shut_ms > 0 && now - s_sleep_start_ms >= s_sleep_shut_ms) {
                ESP_LOGW(WP_TAG, "关机时间到, 转软关机(深睡), 仅 PWR 键开机");
                esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_1), ESP_EXT1_WAKEUP_ANY_LOW);
                esp_deep_sleep_start();   /* 不返回 */
            }
            if (input_last_any_ms() > s_ss_enter_ms) { s_sleep_wake_key = true; break; } /* 按键唤醒 */
            esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
            if (s_sleep_shut_ms > 0) {
                uint32_t remain = s_sleep_shut_ms - (now - s_sleep_start_ms);
                esp_sleep_enable_timer_wakeup((uint64_t)remain * 1000ULL);
            }
            esp_light_sleep_start();   /* 阻塞至唤醒后返回 (内部RAM栈, 安全) */
            ESP_LOGW(WP_TAG, "浅睡唤醒: wakeup_cause=%d", (int)esp_sleep_get_wakeup_cause());
            vTaskDelay(pdMS_TO_TICKS(150));   /* 给 input 任务读键刷新统一输入时钟 */
        }
        s_sleep_run_srv = false;   /* 干完, 放监督任务回主界面 */
    }
}

/* 待机(浅睡): 屏幕关, 任意物理键(18/0/1)唤醒 → 秒醒回原位(不重启, 页面状态保留).
 * 设了"关机时间"则在执行器里由 RTC 定时器到点升级为深睡软关机.
 * 注意: ESP32-S3 浅睡会关闭 USB-Serial-JTAG → 串口/USB 掉线, 需重插恢复(用户已确认可接受). */
static void ss_soft_shutdown(void) {
    ESP_LOGW(WP_TAG, "待机: 进入浅睡 (任意物理键秒醒回原位; USB 会掉线, 重插恢复)");
    if (s_ss_lcd) { st7305_clear(s_ss_lcd, ST7305_COLOR_WHITE); st7305_flush_override(s_ss_lcd); }
    esp_wifi_stop();   /* 浅睡前停 WiFi: modem sleep 也 ~10-20mA, 停掉才真正低功耗 */
    s_sleep_shut_ms  = wp_ss_shut_ms();
    s_sleep_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_sleep_run_srv  = true;
    vTaskResume(s_sleep_task_h);             /* 唤起床睡眠执行器 */
    while (s_sleep_run_srv) vTaskDelay(pdMS_TO_TICKS(20));   /* 等执行器干完(睡/醒循环) */
    ESP_LOGW(WP_TAG, "浅睡结束: 按键唤醒回原界面 (未重启)");
}

static void ss_supervisor_task(void *arg) {
    (void)arg;
    /* [诊断] 开机复位原因: 6=esp_restart软复位 / 7=CPU看门狗(panic) / 其它=上电/深睡 等.
     *   用于区分浅睡唤醒后的"重启回桌面"是软复位还是异常panic. */
    ESP_LOGW(WP_TAG, "开机: reset_reason=%d", (int)esp_reset_reason());
    while (1) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (os_screensaver_active()) {
            /* 屏保中: 独占渲染星空 (主循环可能正被游戏阻塞, 不能等它) */
            if (s_ss_lcd) {
                uint32_t t = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                wp_program_render(s_ss_lcd, WP_PROG_STARS, t);
                st7305_flush_override(s_ss_lcd);   /* 屏保期间唯一允许写屏的通道 */
            }
            /* 从壁纸出现起算, 播满"壁纸运行时长"(wp_ss_run_ms) → 进入休眠(浅睡).
             * 注意: 用 wp_ss_run_ms (壁纸运行时长) 而非 wp_ss_delay_ms (息屏延时). */
            if (now - s_ss_enter_ms >= wp_ss_run_ms()) {
                ss_soft_shutdown();     /* 浅睡待机: 按键唤醒→返回, 关机时间到→不返回(深睡) */
                os_screensaver_reset(); /* 按键唤醒 → 退出屏保回桌面 */
                continue;
            }
            /* 统一唤醒判定: 进入屏保(s_ss_enter_ms)之后, 任何新的输入(时间戳>进入时刻)才唤醒.
             * 以"进入时刻"为分界而非记录某次输入值(wake_base), 补齐了走锁屏键/电源键
             * force_enter 时 wake_base 未同步而"进屏保即刻被自身触发输入唤醒"的漏洞 —
             * 即"按关机键, 立刻休眠又立刻唤醒"闪烁. 触发进入的那次输入与 s_ss_enter_ms
             * 通常同ms(不 >), 不算新输入不会误唤醒; idle 进入时旧输入更不会误算. */
            if (input_last_any_ms() > s_ss_enter_ms) {
                os_screensaver_reset();
            }
            /* 流畅优先: 星空 ~16fps (1000/62≈16帧), 画面流畅为主, 省电后续再做. */
            vTaskDelay(pdMS_TO_TICKS(62));   /* ~16fps */
        } else {
            /* 空闲检测 (低频省电) */
            uint32_t last_in = input_last_any_ms();
            if (last_in != 0 && now - last_in >= wp_ss_delay_ms()) {
                ESP_LOGW(WP_TAG, "独立监视: 空闲超时, 强制进入壁纸");
                os_screensaver_force_enter();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

/* 创建独立屏保监视任务 (由 os_init 传入 LCD 调用一次) */
void wp_screensaver_supervisor_start(st7305_handle_t *lcd) {
    s_ss_lcd = lcd;
    static bool started = false;
    if (started) return;
    started = true;
    /* 静态 PSRAM 栈 (同 board_shim 视频任务): 32KB 保证星空渲染/刷屏/日志不溢出 */
    xTaskCreateStatic(ss_supervisor_task, "sse", SS_SUPER_STACK, NULL, tskIDLE_PRIORITY + 2,
                      s_ss_stack, &s_ss_tcb);
    /* 睡眠执行器: 内部RAM栈(6KB), 专门执行 esp_*_sleep_start (缓存禁用期间任务栈须在
     * 内部RAM, PSRAM 栈会 assert — 官方约束; 入睡由内部RAM idle 类任务发起才安全). */
    xTaskCreateStatic(ss_sleep_runner, "sslp", SS_SLEEP_STACK, NULL, tskIDLE_PRIORITY + 2,
                      s_sleep_stack, &s_sleep_tcb);
    s_sleep_task_h = (TaskHandle_t)&s_sleep_tcb;   /* 静态任务句柄即 StaticTask_t 指针 */
    ESP_LOGI(WP_TAG, "屏保独立监视任务已启动 (PSRAM 栈 %u words; 睡眠执行器内部RAM栈 %u words)",
             (unsigned)SS_SUPER_STACK, (unsigned)SS_SLEEP_STACK);
}

