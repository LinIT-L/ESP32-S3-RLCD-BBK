/* 内置壁纸程序 (V3.0): 7 个 1bit 动态壁纸
 * 依据用户 2026-08-10 反馈重做:
 *   1 无限楼梯 = 经典彭罗斯菱形回环 (四节楼梯看似一直下坡却无限循环)
 *   2 粒子钟   = 粒子先漂移, 再用 15s 一点一点拼成时间
 *   3 二进制海浪 = 浮世绘和风波浪 (黑底, 浪尖白沫卷爪)
 *   4 生命花园 = 白底低密度苔藓禅意花园
 *   5 雾中巨物 = 克苏鲁从海面迷雾中浮现 (触手+发光眼)
 *   6 电路板   = 真实元器件 (芯片/电阻/电容/三极管/排针) + 走线脉冲
 *   7 风吹麦浪 = 10 米高透视视角 (地平线+天空+麦田波浪)
 * 删除: 板块漂移 / 赫尔曼栅格 / 腐蚀重生.
 * 灰度语言 0..4 (白底黑画 block, 黑底白画 wblock); 大数组 PSRAM. */
#include "wallpapers.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"

#define WP_W ST7305_WIDTH
#define WP_H ST7305_HEIGHT
#define WP_ROW ((WP_W + 7) / 8)          /* 50 */
#define WP_FB_BYTES (WP_ROW * WP_H)

static uint8_t *s_fb = NULL;

static void wp_ensure_fb(void) {
    if (!s_fb)
        s_fb = heap_caps_malloc(WP_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void px(int x, int y) {
    if (x >= 0 && x < WP_W && y >= 0 && y < WP_H)
        s_fb[(size_t)y * WP_ROW + (size_t)(x >> 3)] |= (uint8_t)(1u << (7 - (x & 7)));
}

static inline void clr_px(int x, int y) {
    if (x >= 0 && x < WP_W && y >= 0 && y < WP_H)
        s_fb[(size_t)y * WP_ROW + (size_t)(x >> 3)] &= (uint8_t)~(1u << (7 - (x & 7)));
}

static void fb_white(void) { memset(s_fb, 0, WP_FB_BYTES); }
static void fb_black(void) { memset(s_fb, 0xFF, WP_FB_BYTES); }

static const uint8_t PAT[5][4] = {
    {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 1}, {1, 1, 0, 1}, {1, 1, 1, 1}
};
static const uint8_t ROW0[5] = {0, 2, 2, 3, 3};
static const uint8_t ROW1[5] = {0, 0, 1, 1, 3};

/* 白底黑画 2x2 密度块 (x,y 偶数且在界内走快路径) */
static inline void block(int x, int y, int d) {
    if (x < 0 || y < 0 || x >= WP_W - 1 || y >= WP_H - 1 || (x & 1) || (y & 1)) {
        if (x < 0 || y < 0 || x >= WP_W || y >= WP_H) return;
        if (d > 4) d = 4;
        clr_px(x, y); clr_px(x + 1, y); clr_px(x, y + 1); clr_px(x + 1, y + 1);
        if (d > 0) {
            const uint8_t *p = PAT[d];
            if (p[0]) px(x, y);
            if (p[1]) px(x + 1, y);
            if (p[2]) px(x, y + 1);
            if (p[3]) px(x + 1, y + 1);
        }
        return;
    }
    if (d > 4) d = 4;
    int xb = x >> 3, sh = 6 - (x & 7);
    uint8_t m = (uint8_t)(3u << sh);
    uint8_t *p0 = s_fb + (size_t)y * WP_ROW + xb;
    uint8_t *p1 = p0 + WP_ROW;
    if (d <= 0) { p0[0] &= (uint8_t)~m; p1[0] &= (uint8_t)~m; return; }
    p0[0] = (uint8_t)((p0[0] & (uint8_t)~m) | (uint8_t)(ROW0[d] << sh));
    p1[0] = (uint8_t)((p1[0] & (uint8_t)~m) | (uint8_t)(ROW1[d] << sh));
}

/* 黑底白画 2x2 密度块 (d=0 全黑, d=4 全白) */
static inline void wblock(int x, int y, int d) {
    if (x < 0 || y < 0 || x >= WP_W - 1 || y >= WP_H - 1 || (x & 1) || (y & 1)) {
        if (x < 0 || y < 0 || x >= WP_W || y >= WP_H) return;
        if (d < 0) d = 0;
        if (d > 4) d = 4;
        px(x, y); px(x + 1, y); px(x, y + 1); px(x + 1, y + 1);
        const uint8_t *p = PAT[d];
        if (p[0]) clr_px(x, y);
        if (p[1]) clr_px(x + 1, y);
        if (p[2]) clr_px(x, y + 1);
        if (p[3]) clr_px(x + 1, y + 1);
        return;
    }
    if (d < 0) d = 0;
    if (d > 4) d = 4;
    int xb = x >> 3, sh = 6 - (x & 7);
    uint8_t m = (uint8_t)(3u << sh);
    uint8_t *p0 = s_fb + (size_t)y * WP_ROW + xb;
    uint8_t *p1 = p0 + WP_ROW;
    if (d <= 0) { p0[0] |= m; p1[0] |= m; return; }
    p0[0] = (uint8_t)((p0[0] | m) & (uint8_t)~(uint8_t)(ROW0[d] << sh));
    p1[0] = (uint8_t)((p1[0] | m) & (uint8_t)~(uint8_t)(ROW1[d] << sh));
}

static uint32_t s_rng = 0x2F6E2B51u;
static void rng_seed(uint32_t s) { s_rng = s ? s : 0x2F6E2B51u; }
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float frnd(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }

static inline float ss(float k) { return k * k * (3.0f - 2.0f * k); }
static inline float clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}

/* 黑色 1px 直线 (白底) */
static void line_b(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 白色 1px 直线 (黑底) */
static void line_w(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        clr_px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline bool in_quad(float px_, float py_,
                           float ax, float ay, float bx, float by,
                           float cx, float cy, float dx, float dy) {
    float s1 = (bx - ax) * (py_ - ay) - (by - ay) * (px_ - ax);
    float s2 = (cx - bx) * (py_ - by) - (cy - by) * (px_ - bx);
    float s3 = (dx - cx) * (py_ - cy) - (dy - cy) * (px_ - cx);
    float s4 = (ax - dx) * (py_ - dy) - (ay - dy) * (px_ - dx);
    return (s1 >= 0 && s2 >= 0 && s3 >= 0 && s4 >= 0) ||
           (s1 <= 0 && s2 <= 0 && s3 <= 0 && s4 <= 0);
}

static void flush(st7305_handle_t *lcd) {
    if (!lcd || !s_fb) return;
    /* 关键: st7305_blit_1bit 只清黑位, 必须先把 LCD fb 整屏复位为白 */
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    st7305_blit_1bit(lcd, 0, 0, WP_W, WP_H, s_fb);
    st7305_flush(lcd);
}

const char *wp_prog_name(int id) {
    switch (id) {
    case WP_PROG_STARS:   return "星空";
    case WP_PROG_STAIRS:  return "无限楼梯";
    case WP_PROG_CLOCK:   return "粒子钟";
    case WP_PROG_BINARY:  return "二进制海浪";
    case WP_PROG_LIFE:    return "生命花园";
    case WP_PROG_CTHULHU: return "雾中巨物";
    case WP_PROG_PCB:     return "电路板信号";
    case WP_PROG_WHEAT:   return "风吹麦浪";
    case WP_PROG_WEATHER: return "天气时钟";
    default: return "星空";
    }
}

/* ================= 1. 无限递归的楼梯 (彭罗斯回环) =================
 * 经典菱形回环: 4 条直边 x 10 级台阶, 每条边都是一段"看似一直下坡"的楼梯,
 * 四段首尾相接形成无限循环. 台阶由平行四边形梯面 (深浅交替) + 前缘竖面组成,
 * 外/内轮廓勾勒回环, 攀爬者永远沿环"下坡"行走, 16s 一轮扫光. */
static void wp_stairs(uint32_t now) {
    fb_white();
    /* 边缘晕影 */
    for (int y = 0; y < WP_H; y += 2) {
        block(0, y, 1); block(2, y, 1);
        block(WP_W - 4, y, 1); block(WP_W - 2, y, 1);
    }
    for (int x = 4; x < WP_W - 4; x += 2) {
        block(x, 0, 1); block(x, 2, 1);
        block(x, WP_H - 4, 1); block(x, WP_H - 2, 1);
    }
    const int NS = 6;
    const float Ro = 142.0f, Ri = 116.0f;
    int cx = WP_W / 2, cy = WP_H / 2;
    float ox[4], oy[4], ix[4], iy[4];
    for (int s = 0; s < 4; s++) {
        float a = (float)s * 1.5707963f + 0.7853982f;
        ox[s] = cx + cosf(a) * Ro;
        oy[s] = cy + sinf(a) * Ro * 0.72f;
        ix[s] = cx + cosf(a) * Ri;
        iy[s] = cy + sinf(a) * Ri * 0.72f;
    }
    float scan = fmodf((float)now / 16000.0f, 1.0f) * 24.0f;
    float climb = fmodf((float)now / 34500.0f, 1.0f) * 24.0f;
    int cidx = (int)climb;
    float cu = climb - (float)cidx;
    float hop = -3.0f * 4.0f * cu * (1.0f - cu);
    float cpx = 0.0f, cpy = 0.0f;
    bool climber_done = false;

    for (int s = 0; s < 4; s++) {
        int s2 = (s + 1) & 3;
        for (int k = 0; k < NS; k++) {
            int idx = s * NS + k;
            float u0 = (float)k / NS, u1 = (float)(k + 1) / NS;
            float ax = ox[s] + (ox[s2] - ox[s]) * u0;
            float ay = oy[s] + (oy[s2] - oy[s]) * u0;
            float bx = ox[s] + (ox[s2] - ox[s]) * u1;
            float by = oy[s] + (oy[s2] - oy[s]) * u1;
            float cxx = ix[s] + (ix[s2] - ix[s]) * u0;
            float cyy = iy[s] + (iy[s2] - iy[s]) * u0;
            float dxx = ix[s] + (ix[s2] - ix[s]) * u1;
            float dyy = iy[s] + (iy[s2] - iy[s]) * u1;
            /* 梯面密度: 深浅交替 + 扫光提亮 */
            float dbase = (idx & 1) ? 3.0f : 1.0f;
            float sd = fabsf((float)idx + 0.5f - scan);
            if (sd < 3.0f) dbase -= cosf(3.14159265f * sd / 3.0f);
            if (dbase < 0.0f) dbase = 0.0f;
            int d = (int)(dbase + 0.5f);
            if (d < 1) d = 1;
            if (d > 3) d = 3;
            float qx[4] = { ax, bx, dxx, cxx };
            float qy[4] = { ay, by, dyy, cyy };
            float minx = ax, maxx = ax, miny = ay, maxy = ay;
            for (int q = 1; q < 4; q++) {
                if (qx[q] < minx) minx = qx[q];
                if (qx[q] > maxx) maxx = qx[q];
                if (qy[q] < miny) miny = qy[q];
                if (qy[q] > maxy) maxy = qy[q];
            }
            int x0 = ((int)minx) & ~1, x1 = ((int)maxx) & ~1;
            int y0 = ((int)miny) & ~1, y1 = ((int)maxy) & ~1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > WP_W - 2) x1 = WP_W - 2;
            if (y1 > WP_H - 2) y1 = WP_H - 2;
            for (int yy = y0; yy <= y1; yy += 2)
                for (int xx = x0; xx <= x1; xx += 2)
                    if (in_quad((float)xx + 1.0f, (float)yy + 1.0f,
                                qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], qx[3], qy[3]))
                        block(xx, yy, d);
            /* 前缘竖面 (下坡方向): u1 边 2px 深色 */
            for (int q = 0; q <= 12; q++) {
                float f = (float)q / 12.0f;
                int lx = ((int)(bx + (dxx - bx) * f)) & ~1;
                int ly = ((int)(by + (dyy - by) * f)) & ~1;
                block(lx, ly, 4);
            }
            if (idx == cidx && !climber_done) {
                float u = (float)k / NS + cu / NS;
                float px0 = ox[s] + (ox[s2] - ox[s]) * u;
                float py0 = oy[s] + (oy[s2] - oy[s]) * u;
                float px1 = ix[s] + (ix[s2] - ix[s]) * u;
                float py1 = iy[s] + (iy[s2] - iy[s]) * u;
                cpx = (px0 + px1) * 0.5f;
                cpy = (py0 + py1) * 0.5f + hop;
                climber_done = true;
            }
        }
    }
    /* 外/内轮廓 */
    for (int s = 0; s < 4; s++) {
        line_b((int)ox[s], (int)oy[s], (int)ox[(s + 1) & 3], (int)oy[(s + 1) & 3]);
        line_b((int)ix[s], (int)iy[s], (int)ix[(s + 1) & 3], (int)iy[(s + 1) & 3]);
    }
    /* 攀爬者: 黑环 + 白心 */
    if (climber_done) {
        int X = (int)cpx, Y = (int)cpy;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int r2 = dx * dx + dy * dy;
                if (r2 >= 5 && r2 <= 11) px(X + dx, Y + dy);
                else if (r2 <= 2) clr_px(X + dx, Y + dy);
            }
    }
}

/* ================= 2. 单像素粒子钟 =================
 * 时间轴: 0-15s 错峰聚合 (每 33ms 出发一批, 15s 内一点一点拼成时间)
 *         15-19s 停留呼吸, 19-22s 炸开, 22-60s 漂浮.
 * 分钟变化时只有目标变化的粒子重新飞行, 其余字形保持原位 (时钟始终可读). */
#define CLK_PART 700
EXT_RAM_BSS_ATTR static float s_cx[CLK_PART], s_cy[CLK_PART];
EXT_RAM_BSS_ATTR static float s_ox[CLK_PART], s_oy[CLK_PART];
EXT_RAM_BSS_ATTR static int s_tx[CLK_PART], s_ty[CLK_PART];
EXT_RAM_BSS_ATTR static int s_otx[CLK_PART], s_oty[CLK_PART];
EXT_RAM_BSS_ATTR static uint8_t s_chg[CLK_PART];
static uint32_t s_clock_min = 0xFFFFFFFF;
static bool s_clock_init = false;
static int s_glcount = 0;

static const uint8_t DIG[11][7] = {
    {0x7E,0x81,0x81,0x81,0x81,0x81,0x7E},  /* 0 */
    {0x08,0x18,0x08,0x08,0x08,0x08,0x1C},  /* 1 */
    {0x7E,0x81,0x01,0x06,0x18,0x20,0xFF},  /* 2 */
    {0x7E,0x81,0x01,0x3E,0x01,0x81,0x7E},  /* 3 */
    {0x0C,0x14,0x24,0x44,0xFF,0x04,0x04},  /* 4 */
    {0xFF,0x80,0x80,0xFE,0x01,0x01,0xFE},  /* 5 */
    {0x3E,0x40,0x80,0xFE,0x81,0x81,0x7E},  /* 6 */
    {0xFF,0x01,0x02,0x04,0x08,0x10,0x10},  /* 7 */
    {0x7E,0x81,0x81,0x7E,0x81,0x81,0x7E},  /* 8 */
    {0x7E,0x81,0x81,0x7F,0x01,0x02,0x7C},  /* 9 */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},  /* : */
};

static void clock_build_targets(uint32_t minute) {
    int hh = (int)(minute / 60) % 24, mm = (int)(minute % 60);
    int glyphs[5] = { hh / 10, hh % 10, 10, mm / 10, mm % 10 };
    int gi = 0;
    for (int g = 0; g < 5 && gi < CLK_PART; g++) {
        int ox = 90 + g * 48;
        int oy = 130;
        if (g == 2) ox = 90 + 2 * 48 - 12;
        for (int r = 0; r < 7 && gi < CLK_PART; r++)
            for (int c = 0; c < 5 && gi < CLK_PART; c++)
                if (DIG[glyphs[g]][r] & (1u << (4 - c))) {
                    /* 每个字形格 4 粒 -> 实心数字 */
                    s_tx[gi] = ox + c * 4;     s_ty[gi] = oy + r * 6 + 1; gi++;
                    s_tx[gi] = ox + c * 4 + 2; s_ty[gi] = oy + r * 6 + 1; gi++;
                    s_tx[gi] = ox + c * 4;     s_ty[gi] = oy + r * 6 + 3; gi++;
                    s_tx[gi] = ox + c * 4 + 2; s_ty[gi] = oy + r * 6 + 3; gi++;
                }
    }
    s_glcount = gi;
    while (gi < CLK_PART) {
        s_tx[gi] = 60 + (int)(frnd() * 280.0f);
        s_ty[gi] = 70 + (int)(frnd() * 160.0f);
        gi++;
    }
}

static void wp_clock(uint32_t now) {
    wp_ensure_fb();
    if (!s_clock_init) {
        s_clock_init = true;
        rng_seed(0xC10Cu);
        for (int i = 0; i < CLK_PART; i++) {
            s_cx[i] = frnd() * WP_W;
            s_cy[i] = frnd() * WP_H;
            s_otx[i] = -1; s_oty[i] = -1;
        }
    }
    uint32_t minute = now / 60000;
    uint32_t phase = now % 60000;
    if (minute != s_clock_min) {
        s_clock_min = minute;
        clock_build_targets(minute);
        for (int i = 0; i < CLK_PART; i++) {
            if (s_tx[i] != s_otx[i] || s_ty[i] != s_oty[i]) {
                s_chg[i] = 1;
                s_ox[i] = s_cx[i]; s_oy[i] = s_cy[i];
            } else {
                s_chg[i] = 0;
            }
        }
        memcpy(s_otx, s_tx, sizeof(s_tx));
        memcpy(s_oty, s_ty, sizeof(s_ty));
    }
    fb_black();
    float t = (float)now / 1000.0f;
    int mm = (int)(minute % 60);
    bool full_flash = (mm == 0 && phase < 1200);
    for (int i = 0; i < CLK_PART; i++) {
        float x = s_cx[i], y = s_cy[i];
        int d = 4;
        bool big = false;
        if (full_flash) {
            big = (i < s_glcount);
        } else if (!s_chg[i]) {
            /* 未变化的字形粒子: 停在目标, 呼吸 */
            x = (float)s_tx[i] + sinf(t * 0.5f + (float)i) * 0.5f;
            y = (float)s_ty[i] + cosf(t * 0.43f + (float)i * 0.7f) * 0.5f;
            d = (sinf(t * 0.35f + (float)i) > 0.3f) ? 3 : 4;
            big = true;
        } else if (phase < 15000) {
            float start = (float)(i % CLK_PART) * 19.0f;   /* 错峰出发 (~13.3s 内) */
            if (phase < start) {   /* 还没出发: 继续漂浮 */
                x += sinf(y * 0.011f + t * 0.31f + (float)i) * 0.35f;
                y += cosf(x * 0.013f + t * 0.27f + (float)i * 0.7f) * 0.35f;
                if (x < 0) x += WP_W;
                if (x >= WP_W) x -= WP_W;
                if (y < 0) y += WP_H;
                if (y >= WP_H) y -= WP_H;
                float bl = sinf(t * 0.125f + (float)i * 1.7f);
                if (bl < -0.2f) continue;
                d = (bl > 0.2f) ? 4 : 2;
            } else {
                float k = ss(clampf((float)(phase - start) / 1200.0f, 0.0f, 1.0f));
                float ox = s_ox[i], oy = s_oy[i];
                float tx = (float)s_tx[i], ty = (float)s_ty[i];
                float dx = tx - ox, dy = ty - oy;
                float len = sqrtf(dx * dx + dy * dy);
                if (len < 1.0f) len = 1.0f;
                float nxv = -dy / len, nyv = dx / len;
                float off = (float)((i * 73 + (int)(minute * 13)) % 61) - 30.0f;
                if (len < 40.0f) off *= len / 40.0f;
                float mcx = (ox + tx) * 0.5f + nxv * off;
                float mcy = (oy + ty) * 0.5f + nyv * off;
                float ik = 1.0f - k;
                x = ik * ik * ox + 2.0f * ik * k * mcx + k * k * tx;
                y = ik * ik * oy + 2.0f * ik * k * mcy + k * k * ty;
                big = (k > 0.8f) && (i < s_glcount);
            }
        } else if (phase < 19000) {
            x = (float)s_tx[i] + sinf(t * 0.5f + (float)i) * 0.5f;
            y = (float)s_ty[i] + cosf(t * 0.43f + (float)i * 0.7f) * 0.5f;
            float bh = (sinf(t * 0.35f + (float)i) > 0.3f) ? 3 : 4;
            d = (i < s_glcount) ? (int)bh : (int)bh - 1;
            big = (i < s_glcount);
        } else if (phase < 22000) {
            float u = (float)(phase - 19000) / 3000.0f;
            float v = u * u;
            float tx = (float)s_tx[i], ty = (float)s_ty[i];
            float gdx = tx - 210.0f, gdy = ty - 155.0f;
            float gl = sqrtf(gdx * gdx + gdy * gdy);
            if (gl < 1.0f) gl = 1.0f;
            x = tx + (gdx / gl) * (10.0f + gl * 0.25f) * v;
            y = ty + (gdy / gl) * (10.0f + gl * 0.25f) * v;
            if (u > 0.85f) continue;
            d = (u > 0.6f) ? 2 : 4;
            big = (u < 0.3f) && (i < s_glcount);
        } else {
            x += sinf(y * 0.011f + t * 0.31f + (float)i) * 0.35f;
            y += cosf(x * 0.013f + t * 0.27f + (float)i * 0.7f) * 0.35f;
            if (x < 0) x += WP_W;
            if (x >= WP_W) x -= WP_W;
            if (y < 0) y += WP_H;
            if (y >= WP_H) y -= WP_H;
            float bl = sinf(t * 0.125f + (float)i * 1.7f);
            if (bl < -0.2f) continue;
            d = (bl > 0.2f) ? 4 : 2;
        }
        s_cx[i] = x; s_cy[i] = y;
        int gx = ((int)x) & ~1, gy = ((int)y) & ~1;
        if (big) {
            wblock(gx, gy, d); wblock(gx + 2, gy, d);
            wblock(gx, gy + 2, d); wblock(gx + 2, gy + 2, d);
        } else {
            wblock(gx, gy, d);
        }
    }
}

/* ================= 3. 二进制海浪 (浮世绘和风) =================
 * 三层由近到远的和风浪, 黑底. 浪脊 = 连续白沫 + 北斋卷爪,
 * 浪身 = 白->灰密度渐变 + 叠印 0/1 字符, 波峰高亮. */
static void wp_binary(uint32_t now) {
    fb_black();
    float t = (float)now / 1000.0f;
    for (int i = 0; i < 3; i++) {
        float base = 54.0f + (float)i * 82.0f;
        float A = 8.0f + (float)i * 4.0f;
        float w = 0.55f + (float)i * 0.2f;
        float k = 0.028f;
        float ph0 = (float)i * 2.2f;
        int thick = 26 + i * 8;
        /* 浪脊 + 白沫 + 卷爪 + 浪身渐变 */
        for (int x = 0; x < WP_W; x += 2) {
            float sv = sinf((float)x * k + t * w + ph0);
            int cy = (int)(base + sv * A);
            if (cy < 2 || cy >= WP_H - 1) continue;
            /* 连续白沫 (少量缺口, 像碎浪) */
            if (((x / 3 + i * 4 + (int)(t * w * 5)) & 5) != 0) {
                clr_px(x, cy);
                clr_px(x, cy + 1);
            }
            /* 北斋卷爪: 波峰处向左卷起的浪花 */
            if (fabsf(sv) > 0.86f) {
                int dirx = (sv > 0) ? -1 : 1;
                for (int q = 1; q <= 6; q++) {
                    float qa = (float)q * 0.55f;
                    int qx = x + (int)(cosf(qa) * (float)q * 1.8f) * dirx;
                    int qy = cy - (int)(sinf(qa) * (float)q * 1.5f);
                    clr_px(qx, qy);
                    clr_px(qx + 1, qy);
                    if (q >= 4) clr_px(qx, qy + 1);
                }
            }
            /* 浪身: 白->灰渐变 */
            for (int dy = 2; dy < thick; dy += 2) {
                int y = cy + dy;
                if (y >= WP_H) break;
                float depth = (float)dy / (float)thick;
                int b = 1 + (int)((1.0f - depth) * 2.4f);
                wblock(x, y, b);
            }
        }
        /* 0/1 字符叠在浪身上半 */
        for (int gy = (int)base - 6; gy < (int)base + A + thick / 2; gy += 6) {
            if (gy < 0 || gy >= WP_H) continue;
            for (int gx = 0; gx < WP_W; gx += 6) {
                float sv = sinf((float)gx * k + t * w + ph0);
                int cy = (int)(base + sv * A);
                float depth = ((float)gy - (float)cy) / (float)thick;
                if (depth < 0.05f || depth > 0.6f) continue;
                int which = ((gx + gy) & 1);
                if (((gx * 31 + gy * 13 + (int)(t * 2)) % 97) == 0) which ^= 1;
                if (which) {
                    clr_px(gx + 1, gy); clr_px(gx + 1, gy + 1);
                    clr_px(gx + 1, gy + 2); clr_px(gx + 1, gy + 3);
                    clr_px(gx + 1, gy + 4);
                } else {
                    clr_px(gx, gy); clr_px(gx + 2, gy);
                    clr_px(gx, gy + 1); clr_px(gx + 2, gy + 1);
                    clr_px(gx, gy + 2); clr_px(gx + 2, gy + 2);
                    clr_px(gx, gy + 3); clr_px(gx + 2, gy + 3);
                    clr_px(gx, gy + 4); clr_px(gx + 2, gy + 4);
                }
            }
        }
    }
}

/* ================= 4. 生命花园 (白底苔藓禅意) =================
 * 白底, 低密度 7% 起始, B3/S23 + 稀有播种, 种群上限 4%,
 * 年龄 0..3 -> 密度 4..1 (新生深色种子, 渐淡凋零), 每 3 帧一代.
 * 像墨点苔藓在白纸上缓慢生长收缩. */
#define LIFE_W 200
#define LIFE_H 150
EXT_RAM_BSS_ATTR static uint8_t s_life[LIFE_W * LIFE_H];
EXT_RAM_BSS_ATTR static uint8_t s_life2[LIFE_W * LIFE_H];
static bool s_life_init = false;
static uint32_t s_life_frame = 0;
static uint32_t s_life_seed_t = 0;

static inline bool life_alive(uint8_t c) { return (c & 0x80) != 0; }
static inline uint8_t life_age(uint8_t c) { return c & 0x03; }

static void wp_life(uint32_t now) {
    wp_ensure_fb();
    if (!s_life_init) {
        s_life_init = true;
        rng_seed(0x11FEu);
        for (int i = 0; i < LIFE_W * LIFE_H; i++)
            s_life[i] = (frnd() < 0.10f) ? 0x80 : 0;
        s_life_seed_t = now + 15000;
    }
    if (((s_life_frame++ & 2) == 0)) {   /* 每 3 帧一代 */
        int pop = 0;
        for (int y = 0; y < LIFE_H; y++) {
            for (int x = 0; x < LIFE_W; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int xx = (x + dx + LIFE_W) % LIFE_W;
                        int yy = (y + dy + LIFE_H) % LIFE_H;
                        if (life_alive(s_life[yy * LIFE_W + xx])) n++;
                    }
                uint8_t c = s_life[y * LIFE_W + x];
                uint8_t nxt = 0;
                if (life_alive(c)) {
                    if (n == 2 || n == 3) {
                        uint8_t a = life_age(c) + 1;
                        if (a > 3) a = 3;
                        nxt = (uint8_t)(0x80 | a);
                    }
                } else if (n == 3) {
                    nxt = 0x80;
                } else if (n <= 1 && (rnd() % 10000) == 0) {
                    nxt = 0x80;
                }
                s_life2[y * LIFE_W + x] = nxt;
                if (life_alive(nxt)) pop++;
            }
        }
        if (pop > 1500) {   /* 种群上限 5% */
            int kill = pop - 1500, guard = 0;
            while (kill > 0 && guard < 200000) {
                int i = (int)(rnd() % (LIFE_W * LIFE_H));
                if (life_alive(s_life2[i])) { s_life2[i] = 0; kill--; }
                guard++;
            }
        }
        if (now >= s_life_seed_t) {   /* 稀有种子雨 1-2 粒 */
            int cnt = 1 + (int)(rnd() % 2);
            for (int k = 0; k < cnt; k++)
                s_life2[(int)(rnd() % (LIFE_W * LIFE_H))] = 0x80;
            s_life_seed_t = now + 15000 + (rnd() % 10000);
        }
        memcpy(s_life, s_life2, LIFE_W * LIFE_H);
    }
    fb_white();
    for (int y = 0; y < LIFE_H; y++) {
        for (int x = 0; x < LIFE_W; x++) {
            uint8_t c = s_life[y * LIFE_W + x];
            if (!life_alive(c)) continue;
            int a = life_age(c);
            int d = (a == 0) ? 4 : (a == 1) ? 3 : (a == 2) ? 2 : 1;
            if (a == 3 && ((s_life_frame & 3) == 0)) d = 2;
            block(x * 2, y * 2, d);
        }
    }
}

/* ================= 5. 雾中巨物 (克苏鲁从海雾浮现) =================
 * 36s 周期: 10s 浮出 -> 10s 停留 -> 10s 沉下 -> 6s 隐藏.
 * 黑底星空, 海面波浪, 雾霭作背景, 暗色巨物剪影 (兜帽头+发光眼+触手+翼膜),
 * 前景雾带部分遮蔽, 海沫在浪尖闪烁. */
static void wp_cthulhu(uint32_t now) {
    float t = (float)now / 1000.0f;
    uint32_t ph = now % 36000;
    float e;
    if (ph < 10000) e = ss((float)ph / 10000.0f);
    else if (ph < 20000) e = 1.0f;
    else if (ph < 30000) e = 1.0f - ss((float)(ph - 20000) / 10000.0f);
    else e = 0.0f;
    fb_black();
    /* 星空 */
    for (int i = 0; i < 24; i++) {
        int sx = (i * 97 + 13) % 400;
        int sy = (i * 53 + 7) % 86;
        if (sinf(t * 0.3f + (float)i * 1.3f) > 0.15f) clr_px(sx, sy);
    }
    const int WL = 224;   /* 海平线 */
    /* 背景雾 (2px 全覆盖, 亮雾给黑色剪影做底) */
    for (int y = 70; y < 250; y += 2) {
        for (int x = 0; x < WP_W; x += 2) {
            float f = 2.4f + 0.8f * sinf((float)x * 0.013f - t * 0.12f + (float)y * 0.05f)
                    + 0.5f * sinf((float)y * 0.021f + t * 0.07f);
            f = clampf(f, 1.2f, 3.8f);
            wblock(x, y, (int)(f + 0.5f));
        }
    }
    /* 巨物剪影 (暗色盖在雾上) */
    float head_y = 128.0f - (1.0f - e) * 88.0f;   /* 沉下时头降到水面以下 */
    float wing_e = e;
    if (e > 0.05f) {
        /* 翼膜: 两侧大三角暗影 */
        int wy0 = (int)(head_y + 12);
        for (int s = 1; s <= 5; s++) {
            float f = (float)s / 5.0f;
            int wingx = (int)(60.0f + f * 78.0f);
            int wingy = (int)(wy0 + f * 62.0f * wing_e);
            int wingh = (int)(26.0f * (1.0f - f * 0.5f));
            for (int dy = -wingh; dy <= wingh; dy += 2)
                for (int dx = -10; dx <= 10; dx += 2)
                    wblock(wingx + dx, wingy + dy, 0);
            /* 翼缘亮点 (膜边) */
            clr_px(wingx - 11, wingy); clr_px(wingx - 10, wingy + 2);
            clr_px(wingx + 11, wingy); clr_px(wingx + 10, wingy + 2);
        }
        /* 兜帽头: 大椭圆暗影 */
        int hcx = 200, hcy = (int)head_y;
        for (int dy = -42; dy <= 42; dy += 2)
            for (int dx = -52; dx <= 52; dx += 2) {
                float r2 = (float)(dx * dx) / (52.0f * 52.0f) + (float)(dy * dy) / (42.0f * 42.0f);
                if (r2 <= 1.0f) wblock(hcx + dx, hcy + dy, 0);
            }
        /* 头冠/褶皱: 两道浅色脊 */
        line_w(hcx - 16, hcy - 18, hcx - 4, hcy - 8);
        line_w(hcx + 16, hcy - 18, hcx + 4, hcy - 8);
    }
    /* 触手: 从海面升起 */
    static const float TK_X[7] = { 120, 152, 184, 214, 246, 280, 316 };
    static const float TK_L[7] = { 58, 92, 120, 140, 118, 86, 52 };
    for (int i = 0; i < 7; i++) {
        float L = TK_L[i] * e;
        if (L < 6.0f) continue;
        float bx = TK_X[i];
        float wv = 0.7f + (float)i * 0.13f;
        int segs = 18;
        for (int s = 1; s <= segs; s++) {
            float f = (float)s / (float)segs;
            float xx = bx + sinf(f * 2.6f + t * wv + (float)i * 1.4f) * 9.0f * f;
            float yy = WL - L * f;
            int r = 4 + (int)(4.0f * (1.0f - f));
            for (int dy = -r; dy <= r; dy += 2)
                for (int dx = -r; dx <= r; dx += 2)
                    if (dx * dx + dy * dy <= r * r + 2) wblock((int)xx + dx, (int)yy + dy, 0);
        }
        /* 触手尖端小卷 */
        if (L > 40.0f) {
            float tipx = bx + sinf(2.6f + t * wv + (float)i * 1.4f) * 9.0f;
            float tipy = WL - L;
            wblock((int)tipx - 4, (int)tipy + 2, 0);
            wblock((int)tipx + 2, (int)tipy + 4, 0);
        }
    }
    /* 海面主体暗带 + 浪花 */
    for (int y = WL; y < WP_H; y += 2)
        for (int x = 0; x < WP_W; x += 2)
            wblock(x, y, 1);
    for (int x = 0; x < WP_W; x += 2) {
        int wy = WL + 3 + (int)(sinf((float)x * 0.06f + t * 1.4f) * 3.0f);
        if (((x / 4 + (int)(t * 4)) & 3) == 0) clr_px(x, wy);
        int wy2 = WL + 18 + (int)(sinf((float)x * 0.045f - t * 1.1f + 1.0f) * 4.0f);
        if (wy2 < WP_H && ((x / 3 + (int)(t * 3)) & 3) == 0) clr_px(x, wy2);
    }
    /* 前景雾带: 部分遮蔽巨物下半身 */
    for (int y = WL - 30; y < WL + 24; y += 2) {
        for (int x = 0; x < WP_W; x += 2) {
            float f = 1.0f + 1.1f * sinf((float)x * 0.02f - t * 0.16f + (float)y * 0.07f);
            f = clampf(f, 0.0f, 2.6f);
            wblock(x, y, (int)(f + 0.5f));
        }
    }
    /* 发光眼最后画: 永远在最上层 (不被触手/雾盖掉) */
    if (e > 0.4f) {
        int hcy = (int)head_y;
        int ey = hcy + 2;
        for (int side = -1; side <= 1; side += 2) {
            int ex = 200 + side * 34;
            wblock(ex - 3, ey - 3, 2); wblock(ex + 1, ey - 3, 2);
            wblock(ex - 3, ey + 1, 2); wblock(ex + 1, ey + 1, 2);
            wblock(ex - 1, ey - 1, 4); wblock(ex + 1, ey - 1, 4);
            wblock(ex - 1, ey + 1, 4); wblock(ex + 1, ey + 1, 4);
        }
    }
}

/* ================= 6. 电路板·信号风暴 (元器件版) =================
 * 白底 PCB: 中央 CPU + 2 片 DIP 芯片 + 4 个电阻 + 2 圆片电容 +
 * 2 电解电容 + 2 三极管 + 晶振 + 排针连接器, 元器件之间走线 (45°/直角),
 * 信号脉冲沿线流动, 到达引脚时闪亮. */
#define PCB_PULSES 7
typedef struct {
    uint8_t path, seg, dir, speed;
    float t;
} pcb_pulse_t;
EXT_RAM_BSS_ATTR static pcb_pulse_t s_pcb[PCB_PULSES];
static bool s_pcb_init = false;
static float s_flash_x[3], s_flash_y[3];
static int s_flash_t[3];

/* 走线路径 (各 4-8 个路点, 最后重复用于占位) */
static const int16_t PCB_PATH[PCB_PULSES][8][2] = {
    { {216,136},{216,84},{132,84},{132,40},{132,40},{132,40},{132,40},{132,40} },
    { {178,144},{100,144},{100,122},{100,122},{100,122},{100,122},{100,122},{100,122} },
    { {220,150},{282,150},{282,122},{298,122},{298,122},{298,122},{298,122},{298,122} },
    { {216,164},{216,222},{176,222},{176,228},{176,228},{176,228},{176,228},{176,228} },
    { {35,54},{54,54},{54,168},{60,168},{60,168},{60,168},{60,168},{60,168} },
    { {297,214},{260,214},{260,102},{250,100},{250,100},{250,100},{250,100},{250,100} },
    { {200,284},{200,260},{282,260},{282,52},{282,52},{282,52},{282,52},{282,52} }
};
static const uint8_t PCB_PATH_N[PCB_PULSES] = {4, 3, 4, 4, 4, 4, 4};

static void pcb_pos(const pcb_pulse_t *p, float *px, float *py, float *dvx, float *dvy) {
    const int16_t (*wp)[2] = PCB_PATH[p->path];
    int n = PCB_PATH_N[p->path];
    int seg = p->seg;
    if (seg >= n - 1) seg = n - 2;
    float x0 = (float)wp[seg][0], y0 = (float)wp[seg][1];
    float x1 = (float)wp[seg + 1][0], y1 = (float)wp[seg + 1][1];
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) len = 1.0f;
    *px = x0 + dx * p->t;
    *py = y0 + dy * p->t;
    *dvx = dx / len;
    *dvy = dy / len;
}

/* 画 2px 走线 (d2) */
static void trace_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        block(x0 & ~1, y0 & ~1, 2);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 实心圆盘 (白底) */
static void disc(int cx, int cy, int r, int fill, int rim) {
    for (int dy = -r; dy <= r; dy += 2)
        for (int dx = -r; dx <= r; dx += 2) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r - 2) block(cx + dx, cy + dy, fill);
            else if (d2 <= (r + 2) * (r + 2)) block(cx + dx, cy + dy, rim);
        }
}

static void pcb_draw_components(void) {
    /* 中央 CPU 40x30 + 引脚 */
    for (int y = 136; y < 164; y += 2)
        for (int x = 180; x < 220; x += 2)
            block(x, y, 3);
    for (int x = 178; x <= 222; x += 2) {
        block(x, 134, 4); block(x, 164, 4);
    }
    for (int y = 134; y <= 164; y += 2) {
        block(178, y, 4); block(220, y, 4);
    }
    block(182, 138, 0);   /* pin1 白点 */
    /* DIP A (37,40)-(67,60) */
    for (int y = 40; y < 60; y += 2)
        for (int x = 37; x < 67; x += 2)
            block(x, y, 3);
    for (int y = 40; y <= 58; y += 2) { block(35, y, 4); block(69, y, 4); }
    block(37, 42, 0);
    /* DIP B (297,200)-(327,220) */
    for (int y = 200; y < 220; y += 2)
        for (int x = 297; x < 327; x += 2)
            block(x, y, 3);
    for (int y = 200; y <= 218; y += 2) { block(295, y, 4); block(329, y, 4); }
    block(299, 202, 0);
    /* 电阻: 体 16x7 + 3 道色环 + 引线 */
    static const int16_t RS[4][2] = { {124,33}, {54,167}, {292,123}, {218,255} };
    for (int r = 0; r < 4; r++) {
        int bx = RS[r][0], by = RS[r][1];
        for (int y = by; y < by + 8; y += 2)
            for (int x = bx; x < bx + 16; x += 2)
                block(x, y, 3);
        for (int x = bx; x <= bx + 16; x += 2) { block(x, by - 2, 4); block(x, by + 8, 4); }
        for (int y = by; y <= by + 8; y += 2) { block(bx - 2, y, 4); block(bx + 16, y, 4); }
        block(bx + 4, by, 0); block(bx + 6, by, 0); block(bx + 8, by, 0);
        block(bx + 4, by + 2, 0); block(bx + 6, by + 2, 0); block(bx + 8, by + 2, 0);
        block(bx + 10, by, 0); block(bx + 12, by, 0);
        block(bx + 10, by + 2, 0); block(bx + 12, by + 2, 0);
        trace_line(bx - 2, by + 4, bx - 12, by + 4);
        trace_line(bx + 16, by + 4, bx + 26, by + 4);
    }
    /* 圆片电容 (d3 体 + d2 缘 + 引线) */
    disc(96, 118, 7, 3, 2);
    trace_line(96, 126, 96, 140);
    disc(318, 86, 7, 3, 2);
    trace_line(318, 94, 318, 108);
    /* 电解电容 (d4 缘 + d2 体 + 极性白楔 + 粗短引线) */
    disc(120, 272, 10, 2, 4);
    for (int y = 266; y < 278; y += 2)   /* 极性白楔 */
        for (int x = 114; x < 124; x += 2)
            if (x + y < 242) block(x, y, 0);
    trace_line(116, 282, 116, 290);
    trace_line(124, 282, 124, 290);
    disc(282, 46, 10, 2, 4);
    for (int y = 40; y < 52; y += 2)
        for (int x = 276; x < 286; x += 2)
            if (x + y < 328) block(x, y, 0);
    trace_line(278, 56, 278, 66);
    trace_line(286, 56, 286, 66);
    /* 三极管 TO-92 */
    for (int y = 220; y < 232; y += 2)
        for (int x = 171; x < 181; x += 2)
            block(x, y, 3);
    for (int x = 169; x <= 183; x += 2) { block(x, 218, 4); block(x, 232, 4); }
    trace_line(173, 232, 173, 244);
    trace_line(176, 232, 176, 244);
    trace_line(179, 232, 179, 244);
    for (int y = 90; y < 102; y += 2)
        for (int x = 239; x < 249; x += 2)
            block(x, y, 3);
    for (int x = 237; x <= 251; x += 2) { block(x, 88, 4); block(x, 102, 4); }
    trace_line(241, 102, 241, 114);
    trace_line(244, 102, 244, 114);
    trace_line(247, 102, 247, 114);
    /* 晶振 18x8 */
    for (int y = 242; y < 250; y += 2)
        for (int x = 57; x < 75; x += 2)
            block(x, y, 3);
    for (int x = 55; x <= 77; x += 2) { block(x, 240, 4); block(x, 250, 4); }
    trace_line(60, 250, 60, 262);
    trace_line(72, 250, 72, 262);
    /* 排针连接器 */
    for (int i = 0; i < 8; i++) {
        int px = 132 + i * 24;
        for (int y = 284; y < 290; y += 2)
            for (int x = px - 3; x < px + 3; x += 2)
                block(x, y, 2);
        block(px - 4, 282, 4); block(px + 2, 282, 4);
    }
}

static void wp_pcb(uint32_t now) {
    (void)now;
    wp_ensure_fb();
    if (!s_pcb_init) {
        s_pcb_init = true;
        rng_seed(0x5CBu);
        for (int i = 0; i < PCB_PULSES; i++) {
            s_pcb[i].path = (uint8_t)i;
            s_pcb[i].seg = (uint8_t)(rnd() % (PCB_PATH_N[i] - 1));
            s_pcb[i].t = frnd();
            s_pcb[i].dir = (rnd() & 1) ? 1 : 0;
            s_pcb[i].speed = (uint8_t)(2 + rnd() % 3);
        }
        for (int i = 0; i < 3; i++) s_flash_t[i] = 0;
    }
    fb_white();
    /* 板面淡格 */
    for (int x = 0; x < WP_W; x += 32)
        for (int y = 0; y < WP_H; y += 4)
            block(x, y, 1);
    for (int y = 0; y < WP_H; y += 32)
        for (int x = 0; x < WP_W; x += 4)
            block(x, y, 1);
    /* 静态走线 */
    for (int i = 0; i < PCB_PULSES; i++)
        for (int s = 0; s + 1 < PCB_PATH_N[i]; s++)
            trace_line(PCB_PATH[i][s][0], PCB_PATH[i][s][1],
                       PCB_PATH[i][s + 1][0], PCB_PATH[i][s + 1][1]);
    pcb_draw_components();
    /* 脉冲推进 */
    for (int i = 0; i < PCB_PULSES; i++) {
        pcb_pulse_t *p = &s_pcb[i];
        int n = PCB_PATH_N[p->path];
        float hx, hy, dvx, dvy;
        pcb_pos(p, &hx, &hy, &dvx, &dvy);
        int seglen = 1;
        {
            const int16_t (*wp)[2] = PCB_PATH[p->path];
            int s = p->seg;
            if (s >= n - 1) s = n - 2;
            int dx = wp[s + 1][0] - wp[s][0], dy = wp[s + 1][1] - wp[s][1];
            seglen = (int)sqrtf((float)(dx * dx + dy * dy));
            if (seglen < 1) seglen = 1;
        }
        p->t += (float)p->speed / (float)seglen;
        if (p->t >= 1.0f) {
            p->t = 0.0f;
            if (p->dir) {
                p->seg++;
                if (p->seg >= n - 1) {
                    p->seg = (uint8_t)(n - 2);
                    p->dir = 0;
                    /* 到达端点: 引脚闪亮 */
                    for (int k = 0; k < 3; k++)
                        if (s_flash_t[k] <= 0) {
                            s_flash_x[k] = (float)PCB_PATH[p->path][n - 1][0];
                            s_flash_y[k] = (float)PCB_PATH[p->path][n - 1][1];
                            s_flash_t[k] = 4;
                            break;
                        }
                }
            } else {
                if (p->seg == 0) p->dir = 1;
                else p->seg--;
            }
        }
        for (int k = 0; k < 10; k++) {   /* 拖尾 */
            int tx = (int)(hx - dvx * (float)k * 2.0f) & ~1;
            int ty = (int)(hy - dvy * (float)k * 2.0f) & ~1;
            block(tx, ty, 3 - k / 4);
        }
        int hxx = ((int)hx) & ~1, hyy = ((int)hy) & ~1;   /* 脉冲头 4x4 d4 */
        block(hxx - 1, hyy - 1, 4); block(hxx + 1, hyy - 1, 4);
        block(hxx - 1, hyy + 1, 4); block(hxx + 1, hyy + 1, 4);
    }
    for (int k = 0; k < 3; k++)
        if (s_flash_t[k] > 0) {
            s_flash_t[k]--;
            int fx = ((int)s_flash_x[k]) & ~1, fy = ((int)s_flash_y[k]) & ~1;
            block(fx - 2, fy - 2, 4); block(fx, fy - 2, 4); block(fx + 2, fy - 2, 4);
            block(fx - 2, fy, 4);     block(fx, fy, 4);     block(fx + 2, fy, 4);
            block(fx - 2, fy + 2, 4); block(fx, fy + 2, 4); block(fx + 2, fy + 2, 4);
        }
}

/* ================= 7. 风吹麦浪 (10 米透视视角) =================
 * 白底: 天空 + 云 + 飞鸟 + 地平线, 下方麦田透视行向远处收拢,
 * 风从左往右扫过, 麦穗成排倒伏, 明暗波带在田间流动. */
static uint32_t s_wheat_next = 0;
static uint32_t s_wheat_start = 0;
static bool s_wheat_gust = false;

static void wp_wheat(uint32_t now) {
    fb_white();
    if (s_wheat_next == 0) s_wheat_next = 8000 + (uint32_t)(rnd() % 6000);
    float t = (float)now / 1000.0f;
    float gust = 0.0f, spd = 0.0f;
    if (now >= s_wheat_next) {
        s_wheat_start = now;
        s_wheat_gust = true;
        s_wheat_next = now + 12000 + (uint32_t)(rnd() % 8000);
    }
    if (s_wheat_gust) {
        uint32_t du = now - s_wheat_start;
        if (du < 4000) {
            float u = (float)du / 4000.0f;
            gust = sinf(3.14159265f * u);
            spd = 1.6f * gust;
        } else {
            s_wheat_gust = false;
        }
    }
    const int HOR = 108;
    /* 云 */
    for (int i = 0; i < 3; i++) {
        float cx = fmodf((float)(i * 190 + 30) + t * (3.0f + (float)i), 500.0f) - 50.0f;
        int cy = 26 + i * 26;
        for (int yy = -9; yy <= 9; yy += 2)
            for (int xx = -22; xx <= 22; xx += 2) {
                float r2 = (float)(xx * xx) / (22.0f * 22.0f) + (float)(yy * yy) / (9.0f * 9.0f);
                if (r2 < 1.0f) block((int)cx + xx, cy + yy, 1);
            }
    }
    /* 飞鸟 */
    for (int i = 0; i < 3; i++) {
        float bx = fmodf((float)(i * 300) + t * (18.0f + (float)i * 9.0f), 460.0f) - 30.0f;
        int by = 46 + (i % 2) * 18;
        px((int)bx, by); px((int)bx + 3, by);
        px((int)bx + 1, by - 2); px((int)bx + 2, by - 2);
    }
    /* 地平线 + 远山淡影 */
    for (int x = 0; x < WP_W; x++) px(x, HOR);
    for (int x = 0; x < WP_W; x += 2) block(x, HOR - 6, 1);
    /* 地平线霾带: 远处田野淡白, 近处变深 (深度感) */
    for (int y = HOR + 2; y < HOR + 12; y += 2)
        for (int x = 0; x < WP_W; x += 2)
            block(x, y, 1);
    /* 麦田透视行: 行向远处收拢, 近处麦秆高/深/疏, 远处矮/淡/密 */
    const int ROWS = 18;
    const int HF = WP_H - HOR;
    for (int n = 0; n < ROWS; n++) {
        float v = (float)n / (float)(ROWS - 1);           /* 0 远 -> 1 近 */
        int y = HOR + (int)(HF * v * v);
        if (y >= WP_H) continue;
        float persp = 0.5f + 1.7f * v;                     /* 近处波速/频率放大 */
        float sp = 5.0f + 17.0f * v;                       /* 近处株距宽 */
        int hgt = 3 + (int)(12.0f * v);                    /* 近处麦秆高 */
        int base_d = 1 + (int)(2.2f * v);                  /* 远处霾淡, 近处深 */
        for (int x = 0; x < WP_W; x += (int)sp) {
            float ph = (float)x * 0.02f * persp - (t + spd) * (0.8f + 1.8f * v) + (float)n * 0.7f;
            float wv = sinf(ph);
            int lean = (int)(wv * 2.0f * (0.5f + v));
            int bxx = x + lean;
            if (bxx < 0 || bxx >= WP_W) continue;
            int d = base_d;
            if (wv > 0.35f) d++;
            else if (wv < -0.35f) d--;
            if (d < 1) d = 1;
            if (d > 4) d = 4;
            /* 麦穗头 + 麦秆 */
            block(bxx & ~1, (y - hgt) & ~1, d);
            for (int hh = 2; hh < hgt; hh += 4)
                block(bxx & ~1, (y - hh) & ~1, d);
        }
    }
}

/* 程序切换时重置各程序状态 */
static int s_last_prog = -1;
static void wp_reset_program(int prog) {
    (void)prog;
    s_clock_init = false;
    s_clock_min = 0xFFFFFFFF;
    s_life_init = false;
    s_pcb_init = false;
    s_wheat_next = 0;
    s_wheat_gust = false;
}

void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms) {
    wp_ensure_fb();
    if (!s_fb) return;
    if (prog != s_last_prog) {
        s_last_prog = prog;
        wp_reset_program(prog);
    }
    switch (prog) {
    case WP_PROG_STAIRS:  wp_stairs(now_ms); break;
    case WP_PROG_CLOCK:   wp_clock(now_ms); break;
    case WP_PROG_BINARY:  wp_binary(now_ms); break;
    case WP_PROG_LIFE:    wp_life(now_ms); break;
    case WP_PROG_CTHULHU: wp_cthulhu(now_ms); break;
    case WP_PROG_PCB:     wp_pcb(now_ms); break;
    case WP_PROG_WHEAT:   wp_wheat(now_ms); break;
    default: return;
    }
    flush(lcd);
}

void wp_release_buffers(void) {
    if (s_fb) {
        free(s_fb);
        s_fb = NULL;
    }
}
