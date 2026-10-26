/* wp_module_stars.c — 星空壁纸模块 (独立模块样板).
 * 通过 wp_register_module() 自注册到壁纸注册表, 供 wallpapers.c 分发调用.
 * 以后新壁纸模块可照此样板实现, 放到商店下载.
 * 依赖: st7305(clear/pixel) + ui_common(ascii 小字). */
#include "wallpapers.h"
#include "ui_common.h"
#include "esp_attr.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

#define WP_W ST7305_WIDTH     /* 400 */
#define WP_H ST7305_HEIGHT    /* 300 */

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

static int render_stars(st7305_handle_t *lcd, uint32_t now_ms) {
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
    return 1;      /* 动态壁纸: 每帧都有变化, 需要 flush */
}


/* ==== æ¨¡åæ³¨åè¡¨æ³¨å ====
 * ãä¿®å¤ãä¸ç¨ __attribute__((constructor)) èªæ³¨å:
 * ESP-IDF é¾æ¥ --gc-sections åæ¶ä¸,è¯¥èªæ³¨åå½æ°æ å¤é¨ç¬¦å·å¼ç¨æ¶å¯è¢«é¾æ¥å¨ä¸¢å¼,
 * .init_array ä¸æ§è¡ â æ³¨åè¡¨ä¸ºç©º â å±ä¿æ²¡æç»é¢.
 * æ¹ä¸ºç± wallpapers.c å¯å¨æ¶æ¾å¼è°ç¨(å¹ç­).
 */
static bool s_stars_registered = false;
void wp_stars_register_module(void) {
    if (s_stars_registered) return;
    s_stars_registered = true;
    static const wp_module_t mod = {
        .id = WP_PROG_STARS,
        .name = "\xe6\x98\x9f\xe7\xa9\xba",   /* æç©º */
        .render = render_stars,
    };
    wp_register_module(&mod);
}
