/**
 * page_wifiprobe.c — 无线探测 (Wi-Fi 扫描/雷达/热力图/探针嗅探).
 *
 * 视图 (方向键切换):
 *   雷达视图  ←LEFT/RIGHT→ 信道热力图 ←→ 探针嗅探
 *   雷达: 暗色科幻雷达盘, 扫描线旋转 + 各 AP 圆球散布 (信号越强越近圆心);
 *         ▲▼ 在圆球之间移动选中圈, CONFIRM/点球 弹出"自适应"详情小窗口, 再按关闭.
 *   UP/DOWN 选择, BACK 关闭详情/返回, 退出即停止监听.
 */
#include "os.h"
#include "os_internal.h"
#include "ui_common.h"
#include "wifi_probe.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "font_zh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* 视图枚举 */
enum {
    WP_VIEW_AP = 0,
    WP_VIEW_HEAT,
    WP_VIEW_PROBE,
};
#define WP_VIEW_COUNT 3

#define WP_TOP 24   /* 顶部状态栏高度 (非全屏, 内容下移避让; 保持白色以便状态栏显示) */

/* 虚拟画布 600×400: 雷达按此大画布绘制, 物理屏(400×300)只显示中间偏下的窗口.
 * 半径 300 填满 600 宽: 平底(直径)在画布 y=300, 映射到屏幕最底边 (y=299);
 * 外圈顶(apex y=0)映射到屏外 → 只显示半圆内部, 不出现外弧边界. */
#define VCW 600
#define VCH 400
#define VW_X0 100        /* 物理屏 x=0 ↔ 虚拟 x=100 (横向居中) */
#define VW_Y0 1          /* 物理屏 y=0 ↔ 虚拟 y=1 (平底线落在屏幕底边) */

/* 前方 180° 扇形雷达 (虚拟坐标, 大尺寸) */
#define VCX   300
#define VBASE 300
#define VRAD  300

/* 水波纹 (扫过 AP 时从球体反射扩散) */
#define WP_RIP_MAX 16
static uint32_t s_rip_born[WP_RIP_MAX];   /* 该 AP 波纹出生时刻(ms) */
static bool     s_rip_on[WP_RIP_MAX];

/* 扫描线后瞬态噪点: 注入时换算成屏幕坐标, 绘制零三角函数; 环形游标取模防越界 */
#define WP_TR_N 1000
typedef struct { int px; int py; uint32_t born; uint32_t life; } wp_tr_t;
EXT_RAM_BSS_ATTR static wp_tr_t  s_tr[WP_TR_N];   /* 雷达轨道数组移 PSRAM, 省核心内部 RAM 16KB */
static uint32_t s_tr_tail = 0;      /* 环形写入游标 (取模) */

/* 真正随机: xorshift32, 每帧可变, 避免规律重影 */
static uint32_t s_rng = 0x1234ABCDu;
static inline uint32_t wp_rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_rng = x;
    return x;
}

static int  s_view = WP_VIEW_AP;
static int  s_sel = 0;          /* 雷达选中圆球 / 列表选中行 */
static int  s_scroll = 0;       /* 列表滚动偏移 (热力图/探针) */
static bool s_detail = false;   /* 雷达详情小窗口是否弹出 */
static bool s_running = false;  /* 是否正在监听 */
static uint32_t s_poll_tick = 0;

/* 停止一切监听 */
static void wp_stop_all(void)
{
    if (wifi_probe_sniff_running()) wifi_probe_sniff_stop();
    s_running = false;
}

static void wp_ensure_sniff(void)
{
    if (s_view == WP_VIEW_HEAT || s_view == WP_VIEW_PROBE) {
        if (!s_running) {
            wifi_probe_sniff_reset();
            if (wifi_probe_sniff_start() == ESP_OK) s_running = true;
        }
    } else {
        wp_stop_all();
    }
}

static void wp_set_view(int v)
{
    s_view = v;
    s_scroll = 0;
    s_sel = 0;
    s_detail = false;
    if (v == WP_VIEW_AP) wifi_probe_scan();
    wp_ensure_sniff();
}

/* ================= 绘图原语 (限界避免画出状态栏区/屏外) ================= */
static inline void wp_pix(st7305_handle_t *l, int x, int y, int c)
{
    if (x >= 0 && x < UI_SCREEN_W && y >= WP_TOP && y < UI_SCREEN_H)
        st7305_draw_pixel(l, x, y, (st7305_color_t)c);
}

static void wp_line(st7305_handle_t *l, int x0, int y0, int x1, int y1, int c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        wp_pix(l, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 虚拟画布像素: (vx,vy) → 物理屏 (vx-VW_X0, vy-VW_Y0). 越界/状态栏区自动裁剪 */
static inline void wp_vp(st7305_handle_t *l, int vx, int vy, int c)
{
    wp_pix(l, vx - VW_X0, vy - VW_Y0, c);
}

static void wp_circle(st7305_handle_t *l, int cx, int cy, int r, int c)
{
    if (r < 0) return;
    int x = r, y = 0, p = 1 - r;
    wp_pix(l, cx + x, cy, c); wp_pix(l, cx - x, cy, c);
    wp_pix(l, cx, cy + x, c); wp_pix(l, cx, cy - x, c);
    while (x > y) {
        y++;
        if (p <= 0) p += 2 * y + 1;
        else { x--; p += 2 * (y - x) + 1; }
        wp_pix(l, cx + x, cy + y, c); wp_pix(l, cx - x, cy + y, c);
        wp_pix(l, cx + x, cy - y, c); wp_pix(l, cx - x, cy - y, c);
        wp_pix(l, cx + y, cy + x, c); wp_pix(l, cx - y, cy + x, c);
        wp_pix(l, cx + y, cy - x, c); wp_pix(l, cx - y, cy - x, c);
    }
}

/* 实心圆 (信号球) */
static void wp_dot(st7305_handle_t *l, int cx, int cy, int r, int c)
{
    if (r < 1) { wp_pix(l, cx, cy, c); return; }
    for (int yy = -r; yy <= r; yy++) {
        int hz = (int)sqrtf((float)(r * r - yy * yy)) + 0;   /* 半宽 */
        fill_rect(l, cx - hz, cy + yy, cx + hz, cy + yy, (st7305_color_t)c);
    }
}

/* AP 极坐标 (虚拟): 返回方位角(rad, 0..π), rr=离基线径向距离(虚拟像素).
 * 信号越强 fr越接近1 → 越贴基线(距离近). */
static float wp_ap_polar(const wifi_ap_record_t *ap, float *rrout)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < (int)sizeof(ap->ssid) && ap->ssid[i]; i++) {
        h ^= ap->ssid[i]; h *= 16777619u;
    }
    h ^= ap->bssid[0] | (ap->bssid[1] << 3); h *= 16777619u;
    float ang = ((float)(h % 1800) / 10.0f) * (float)M_PI / 180.0f;   /* 0..180° */
    float fr  = (float)(ap->rssi - (-90)) / 60.0f;   /* -90..-30 -> 0..1 */
    if (fr < 0) fr = 0; else if (fr > 1) fr = 1;
    *rrout = VRAD * (0.14f + 0.78f * (1.0f - fr));
    return ang;
}

static void wp_ap_pos(const wifi_ap_record_t *ap, int *px, int *py)
{
    float rr;
    float ang = wp_ap_polar(ap, &rr);
    *px = VCX + (int)(rr * cos(ang) + 0.5f);
    *py = VBASE - (int)(rr * sin(ang) + 0.5f);
}

/* 实线前半扇环弧 (雷达圈, 虚拟坐标) */
static void wp_arc(st7305_handle_t *l, int r, int c)
{
    for (int a = 0; a <= 180; a += 2) {
        float th = (float)a * (float)M_PI / 180.0f;
        wp_vp(l, VCX + (int)(r * cos(th)), VBASE - (int)(r * sin(th)), c);
    }
}

/* 虚线前半扇环弧 (内部网格线, 不画外边界) */
static void wp_arc_dash(st7305_handle_t *l, int r, int c)
{
    for (int a = 0; a <= 180; a += 2) {
        if ((a / 4) & 1) continue;                      /* 隔段跳点成虚线 */
        float th = (float)a * (float)M_PI / 180.0f;
        wp_vp(l, VCX + (int)(r * cos(th)), VBASE - (int)(r * sin(th)), c);
    }
}

/* 16px 文本宽度 (中文16, ASCII8) — 供居中计算 */
static int wp_txt_w(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        if ((*p & 0xE0) == 0xE0) { w += 16; p += 3; } else { w += 8; p++; }
    }
    return w;
}
static void wp_txt(st7305_handle_t *l, int x, int y, const char *s, bool inv);  /* 前向声明 */
/* 16px 文本水平居中 (cx 为屏幕中心 x) */
static void wp_txt_c(st7305_handle_t *l, int cx, int y, const char *s, bool inv)
{
    wp_txt(l, cx - wp_txt_w(s) / 2, y, s, inv);
}

/* 细字文本: 中文用 draw_zh_sb(16×16), ASCII 用 draw_ascii_small(8×12) 居中在 16px 高 */
static void wp_txt(st7305_handle_t *l, int x, int y, const char *s, bool inv)
{
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xE0 && (unsigned char)s[1] && (unsigned char)s[2]) {
                char g[4] = { s[0], s[1], s[2], 0 };
                draw_zh_sb(l, x, y, g, inv);
                x += 16; s += 3;
            } else s++;
        } else {
            draw_ascii_small(l, x, y + 2, (char)c, inv);   /* 8×12 居中在 16px 行 */
            x += 8; s++;
        }
    }
}

/* 细字文本 (限定右边界, 超宽截断) */
static void wp_txt_cap(st7305_handle_t *l, int x, int y, const char *s, bool inv, int xmax)
{
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xE0 && (unsigned char)s[1] && (unsigned char)s[2]) {
                if (x + 16 > xmax) break;
                char g[4] = { s[0], s[1], s[2], 0 };
                draw_zh_sb(l, x, y, g, inv);
                x += 16; s += 3;
            } else s++;
        } else {
            if (x + 8 > xmax) break;
            draw_ascii_small(l, x, y + 2, (char)c, inv);
            x += 8; s++;
        }
    }
}

/* ================= 雷达视图 (默认, 前方 180° 扇扫, 按 600×400 虚拟画布绘制) ================= */
static void wp_render_radar(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    int W = UI_SCREEN_W, H = UI_SCREEN_H;
    fill_rect(lcd, 0, 0, W - 1, H - 1, ST7305_COLOR_WHITE);

    /* 虚拟→屏幕: 平底线(VRAD 圆半径300)落在屏幕最底边 */
    int scx = VCX - VW_X0;   /* 200 */
    int scy = VBASE - VW_Y0; /* 299 (屏幕底边) */

    /* --- 全屏暗色雷达区 (状态栏之下) --- */
    fill_rect(lcd, 0, WP_TOP, W - 1, H - 1, ST7305_COLOR_BLACK);
    /* 内部虚线网格半圆 (不画外边界) */
    wp_arc_dash(lcd, VRAD * 3 / 4, 1);
    wp_arc_dash(lcd, VRAD / 2,     1);
    wp_arc_dash(lcd, VRAD / 4,     1);
    /* 平底线在屏幕最底边 */
    draw_hline(lcd, 0, W - 1, scy, ST7305_COLOR_WHITE);

    /* --- 单条扫描线: 右→左单程, 到头停顿 3s 再从右侧重新扫 (不往返) --- */
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t SW   = 8000;      /* 单程扫描时长 ms */
    const uint32_t HOLD = 3000;      /* 到头停顿 ms */
    uint32_t ph = t % (SW + HOLD);
    const bool sweep = (ph < SW);
    float fr = sweep ? (float)ph / (float)SW : 1.0f;
    float eased = fr * fr * (3.0f - 2.0f * fr);      /* 缓入缓出, 细腻 */
    float ang = (float)M_PI * eased;                 /* 0..π = 右→左 */
    if (sweep) {
        int lx = scx + (int)(VRAD * cos(ang));
        int ly = scy - (int)(VRAD * sin(ang));
        wp_line(lcd, scx, scy, lx, ly, 1);
    }

    /* --- 扫描线后噪点: 每帧随机注入线后, 每个点寿命/位置各不相同, 原地闪烁较久 --- */
    if (sweep) {
        const uint32_t INJ = 150;                      /* 每帧注入 (密集但省性能) */
        for (uint32_t k = 0; k < INJ; k++) {
            float f1 = (float)(wp_rnd() & 0xFFFF) / 65535.0f;
            float f2 = (float)(wp_rnd() & 0xFFFF) / 65535.0f;
            float f3 = (float)(wp_rnd() & 0xFFFF) / 65535.0f;
            float lag = 0.02f + 0.32f * f1;            /* 线后散开 */
            float a = ang - lag; if (a < 0) a = 0;
            float rr = VRAD * (0.08f + 0.86f * f2);
            uint32_t slot = s_tr_tail++ % WP_TR_N;     /* 取模防越界 */
            s_tr[slot].px   = VCX + (int)(rr * cosf(a)) - VW_X0;
            s_tr[slot].py   = VBASE - (int)(rr * sinf(a)) - VW_Y0;
            s_tr[slot].born = t;
            s_tr[slot].life = 200u + (uint32_t)(f3 * 560u);   /* 0.2~0.76s, 各异, 更久 */
        }
    }
    /* 绘制存活噪点 (直接像素, 零三角函数) */
    for (uint32_t i = 0; i < WP_TR_N; i++) {
        uint32_t age = t - s_tr[i].born;
        if (age > s_tr[i].life) continue;
        wp_pix(lcd, s_tr[i].px, s_tr[i].py, 1);
    }

    /* --- AP: 极坐标 (波纹出生检测 + 圆球) --- */
    int n = wifi_probe_ap_count();
    int nn = n; if (nn > WP_RIP_MAX) nn = WP_RIP_MAX;

    /* 扫描线扫过 → 从该球反射一道水波 (仅扫描过程中触发一次) */
    for (int i = 0; i < nn; i++) {
        const wifi_ap_record_t *ap = wifi_probe_ap_record(i);
        if (!ap) continue;
        float rr;
        float a2 = wp_ap_polar(ap, &rr);
        if (sweep && fabs(ang - a2) < 0.13f && !s_rip_on[i]) {
            s_rip_on[i] = true;
            s_rip_born[i] = t;
        }
    }
    /* 水波纹: 几个速度不同的圆环, 非常缓慢地向外扩张, 越久越稀疏消散 */
    for (int i = 0; i < nn; i++) {
        if (!s_rip_on[i]) continue;
        float T = (float)(t - s_rip_born[i]);            /* 已存活 ms */
        float spd[3] = { 0.0040f, 0.0060f, 0.0080f };    /* px/ms — 慢速 */
        const wifi_ap_record_t *ap = wifi_probe_ap_record(i);
        if (!ap) continue;
        int vx = VCX, vy = VBASE;
        wp_ap_pos(ap, &vx, &vy);
        int px = vx - VW_X0, py = vy - VW_Y0;
        if (T * spd[2] > 48.0f) { s_rip_on[i] = false; continue; }   /* ~6s 淡出, 不太持久 */
        int step = 4 + (int)(T / 900.0f);               /* 越久越稀疏 */
        for (int k = 0; k < 3; k++) {
            float R = T * spd[k];
            if (R < 5.0f) continue;
            for (int a = 0; a <= 360; a += (step + k)) {
                float th = (float)a * (float)M_PI / 180.0f;
                wp_pix(lcd, px + (int)(R * cosf(th)), py - (int)(R * sinf(th)), 1);
            }
        }
    }

    /* 圆球: 大小随信号(越强越大), 选中标信道数字 */
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t *ap = wifi_probe_ap_record(i);
        if (!ap) continue;
        int vx = VCX, vy = VBASE;
        wp_ap_pos(ap, &vx, &vy);
        int px = vx - VW_X0, py = vy - VW_Y0;
        int dotR = 2 * (1 + ((ap->rssi + 90) / 10));   /* 放大一倍: -90→2 .. -30→8 */
        if (dotR > 8) dotR = 8;
        if (dotR < 2) dotR = 2;
        if (i == s_sel) {
            wp_circle(lcd, px, py, dotR + 2, 1);
            wp_dot(lcd, px, py, dotR, 1);
            char cc[4];
            snprintf(cc, sizeof(cc), "%u", ap->primary);
            wp_txt(lcd, px + dotR + 3, py - 8, cc, true);
        } else {
            wp_dot(lcd, px, py, dotR, 1);
        }
    }
    if (n == 0) {
        wp_txt(lcd, 150, scy - 10,
            wifi_probe_is_scan_done() ?
                "\xe6\x9c\xaa\xe6\x89\xab\xe5\x88\xb0\xe7\xbd\x91\xe7\xbb\x9c" :  /* 未扫到网络 */
                "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f\xe2\x80\xa6", true); /* 正在扫描… */
    }

    /* --- 左上 HUD 简讯 (细字) --- */
    int bestr = -127, besti = -1;
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t *ap = wifi_probe_ap_record(i);
        if (ap && ap->rssi > bestr) { bestr = ap->rssi; besti = i; }
    }
    char buf[56];
    int hy = WP_TOP + 6;
    snprintf(buf, sizeof(buf), "%s %d", "\xe7\xbd\x91\xe7\xbb\x9c", n);                  /* 网络 N */
    wp_txt(lcd, 6, hy, buf, true);                   hy += 18;
    snprintf(buf, sizeof(buf), "%s %d", "\xe6\x9c\x80\xe5\xbc\xba", (int)bestr);        /* 最强 dBm */
    wp_txt(lcd, 6, hy, buf, true);                   hy += 18;
    if (besti >= 0) {
        const wifi_ap_record_t *b = wifi_probe_ap_record(besti);
        snprintf(buf, sizeof(buf), "%s %d", "\xe4\xbf\xa1\xe9\x81\x93", b->primary);    /* 信道 */
        wp_txt(lcd, 6, hy, buf, true);               hy += 18;
    }

    /* --- 详情小窗口 (细字, 叠在雷达中央; 自适应高度) --- */
    if (s_detail && n > 0) {
        const wifi_ap_record_t *ap = wifi_probe_ap_record(s_sel);
        if (ap) {
            static const char *aname[] = { "\xe5\xbc\x80\xe6\x94\xbe", "WEP", "WPA", "WPA2",
                                           "WPA/WPA2", "\xe4\xbc\x81\xe4\xb8\x9a", "WPA3",
                                           "WPA2/3", "WAPI" };
            int lh = 16, pad = 8, w = 214;
            int x0 = scx - w / 2, x1 = x0 + w - 1;
            /* 行内容 */
            char mac[20], chbuf[40], au[24], rg[24];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
            snprintf(chbuf, sizeof(chbuf), "%s %d  %dMHz", "\xe4\xbf\xa1\xe9\x81\x93", ap->primary,
                     2412 + 5 * (ap->primary - 1));
            int ai = ap->authmode;
            snprintf(au, sizeof(au), "%s%s", "\xe5\x8a\xa0\xe5\xaf\x86",
                     (ai >= 0 && ai < (int)(sizeof(aname) / sizeof(aname[0]))) ? aname[ai] : "");
            snprintf(rg, sizeof(rg), "%s %ddBm", "\xe4\xbf\xa1\xe5\x8f\xb7", (int)ap->rssi);
            int nline = 5 + (!ap->ssid[0]);
            int h = pad + nline * lh + pad + (ap->ssid[0] ? lh : 0);
            int y0 = WP_TOP + ((H - WP_TOP) - h) / 2, y1 = y0 + h - 1;
            fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_WHITE);
            draw_rect_outline(lcd, x0, y0, x1, y1, ST7305_COLOR_BLACK);
            int y = y0 + pad;
            wp_txt_cap(lcd, x0 + 6, y, ap->ssid[0] ? (const char *)ap->ssid : "(\xe9\x9a\x90\xe8\x97\x8f)", false, x0 + w - 6); y += lh;
            wp_txt_cap(lcd, x0 + 6, y, mac, false, x0 + w - 6);                     y += lh;
            wp_txt_cap(lcd, x0 + 6, y, chbuf, false, x0 + w - 6);                  y += lh;
            wp_txt_cap(lcd, x0 + 6, y, au, false, x0 + w - 6);                     y += lh;
            wp_txt_cap(lcd, x0 + 6, y, rg, false, x0 + w - 6);                     y += lh;
            if (!ap->ssid[0]) { wp_txt_cap(lcd, x0 + 6, y, "\xe9\x9a\x90\xe8\x97\x8f SSID", false, x0 + w - 6); y += lh; }
            /* 信号条 */
            if (ap->ssid[0]) {
                int bx = x0 + 6, by = y + 2, bars = (ap->rssi + 100 > 0) ? (ap->rssi + 100) / 10 : 0;
                if (bars > 8) bars = 8;
                for (int b = 0; b < 8; b++) {
                    int bdb = (b < bars) ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
                    fill_rect(lcd, bx + b * 12, by, bx + b * 12 + 8, by + 10, bdb);
                    draw_rect_outline(lcd, bx + b * 12, by, bx + b * 12 + 8, by + 10, ST7305_COLOR_BLACK);
                }
            }
        }
    }
}

/* ================= 信道热力图 ================= */
static void wp_render_heat(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    fill_rect(lcd, 0, 0, UI_SCREEN_W - 1, UI_SCREEN_H - 1, ST7305_COLOR_WHITE);
    wp_txt_c(lcd, 200, WP_TOP + 4, "\xe4\xbf\xa1\xe9\x81\x93\xe7\x83\xad\xe5\x8a\x9b\xe5\x9b\xbe", false);
    draw_hline(lcd, 0, UI_SCREEN_W - 1, WP_TOP + 22, ST7305_COLOR_BLACK);
    char buf[64];
    const wp_ch_stat_t *ch = wifi_probe_ch_stat();
    int bx0 = 12, by0 = UI_SCREEN_H - 30, bar_h = by0 - (WP_TOP + 34);
    int bar_w = (UI_SCREEN_W - 24) / WP_CH_COUNT;
    uint32_t maxv = 1;
    for (int c = 0; c < WP_CH_COUNT; c++) {
        uint32_t v = ch[c].beacon + ch[c].probe;
        if (v > maxv) maxv = v;
    }
    for (int c = 0; c < WP_CH_COUNT; c++) {
        uint32_t v = ch[c].beacon + ch[c].probe;
        int bh = (int)((uint32_t)v * bar_h / maxv);
        int x0 = bx0 + c * bar_w;
        if (bh > 0) fill_rect(lcd, x0, by0 - bh, x0 + bar_w - 2, by0 - 1, ST7305_COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", c + 1);
        wp_txt(lcd, x0, by0 + 2, buf, false);
    }
    uint32_t tb = 0, tp = 0;
    for (int c = 0; c < WP_CH_COUNT; c++) { tb += ch[c].beacon; tp += ch[c].probe; }
    snprintf(buf, sizeof(buf), "B%d P%d", (int)tb, (int)tp);
    wp_txt(lcd, 4, WP_TOP + 28, buf, false);
}

/* ================= 探针嗅探 ================= */
static void wp_render_probe(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    fill_rect(lcd, 0, 0, UI_SCREEN_W - 1, UI_SCREEN_H - 1, ST7305_COLOR_WHITE);
    wp_txt_c(lcd, 200, WP_TOP + 4, "\xe6\x8e\xa2\xe9\x92\x88\xe6\xb0\x94\xe5\x91\xb3\x28\xe9\x9a\x90\xe8\x97\x8fSSID\x29", false);
    draw_hline(lcd, 0, UI_SCREEN_W - 1, WP_TOP + 22, ST7305_COLOR_BLACK);
    char buf[64];
    int n = wifi_probe_probe_count();
    snprintf(buf, sizeof(buf), "%s %d", "\xe5\x8f\x91\xe7\x8e\xb0", n);
    wp_txt(lcd, 4, WP_TOP + 28, buf, false);
    int rows = (UI_SCREEN_H - (WP_TOP + 46)) / 20;
    for (int i = 0; i < rows; i++) {
        int idx = s_scroll + i;
        if (idx >= n) break;
        const wp_probe_t *pr = wifi_probe_probe_get(idx);
        int y = WP_TOP + 46 + i * 20;
        bool inv = (i == s_sel);
        wp_txt_cap(lcd, 4, y, pr->ssid, inv, 200);
        snprintf(buf, sizeof(buf), "x%lu CH%d", (unsigned long)pr->count, pr->channel);
        wp_txt(lcd, 230, y, buf, inv);
    }
    if (n == 0)
        wp_txt_c(lcd, 200, 140, "\xe5\x9c\xa8\xe7\xad\x89\xe6\x8e\xa2\xe9\x92\x88\xe6\xb0\x94\xe5\x91\xb3\xe2\x80\xa6", false);
}

/* ================= 页面契约 ================= */
static void wp_enter(ui_ctx_t *ctx)
{
    wifi_probe_init();
    os_statusbar_set_invert(true);     /* 本页状态栏反白 (退出还原) */
    s_view = WP_VIEW_AP;
    s_scroll = 0; s_sel = 0; s_detail = false; s_running = false;
    wp_set_view(WP_VIEW_AP);
    ctx->needs_redraw = true;
}

static void wp_exit(ui_ctx_t *ctx)
{
    wp_stop_all();
    wifi_probe_deinit();
    os_statusbar_set_invert(false);    /* 状态栏还原普通颜色 */
    ctx->needs_redraw = true;
}

static void wp_render(ui_ctx_t *ctx)
{
    switch (s_view) {
    case WP_VIEW_AP:    wp_render_radar(ctx); break;   /* 原雷达视图(还原) */
    case WP_VIEW_HEAT:  wp_render_heat(ctx);  break;
    case WP_VIEW_PROBE: wp_render_probe(ctx); break;
    }
}

static void wp_action(ui_ctx_t *ctx, os_action_t a)
{
    int n = (s_view == WP_VIEW_AP)   ? wifi_probe_ap_count()
          : (s_view == WP_VIEW_PROBE)? wifi_probe_probe_count() : 0;
    switch (a) {
    case OS_ACTION_UP:
        if (n > 0) { s_sel = (s_sel - 1 + n) % n; ctx->needs_redraw = true; }
        break;
    case OS_ACTION_DOWN:
        if (n > 0) { s_sel = (s_sel + 1) % n; ctx->needs_redraw = true; }
        break;
    case OS_ACTION_LEFT:
        wp_set_view((s_view + WP_VIEW_COUNT - 1) % WP_VIEW_COUNT);
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_RIGHT:
        wp_set_view((s_view + 1) % WP_VIEW_COUNT);
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_view == WP_VIEW_AP) {
            if (n > 0) { s_detail = !s_detail; ctx->needs_redraw = true; }
        } else {
            wifi_probe_sniff_reset(); ctx->needs_redraw = true;
        }
        break;
    case OS_ACTION_BACK:
        if (s_view == WP_VIEW_AP && s_detail) { s_detail = false; ctx->needs_redraw = true; }
        else os_pop(ctx);
        break;
    default:
        break;
    }
}

static void wp_poll(ui_ctx_t *ctx)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() / portTICK_PERIOD_MS);
    uint32_t iv = (s_view == WP_VIEW_AP) ? 120 : 500;   /* 雷达动画需较快帧 */
    if (now - s_poll_tick > iv) {
        s_poll_tick = now;
        ctx->needs_redraw = true;   /* 主动扫描/监听均为异步, 持续刷新 */
    }
}

static bool wp_touch(ui_ctx_t *ctx, int x, int y)
{
    if (s_view != WP_VIEW_AP || y < WP_TOP) return false;
    int n = wifi_probe_ap_count();
    int best = -1, bd2 = 14 * 14;
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t *rec = wifi_probe_ap_record(i);
        if (!rec) continue;
        int vx = VCX, vy = VBASE;
        wp_ap_pos(rec, &vx, &vy);
        int px = vx - VW_X0, py = vy - VW_Y0;
        int ddx = px - x, ddy = py - y;
        int d2 = ddx * ddx + ddy * ddy;
        if (d2 < bd2) { bd2 = d2; best = i; }
    }
    if (best >= 0) {
        s_sel = best; s_detail = true; ctx->needs_redraw = true; return true;
    }
    if (s_detail) { s_detail = false; ctx->needs_redraw = true; return true; }
    return false;
}

static const os_module_t s_wifiprobe = {
    .name       = "wifiprobe",
    .page_id    = OS_PAGE_WIFIPROBE,
    .on_enter   = wp_enter,
    .on_exit    = wp_exit,
    .render     = wp_render,
    .action     = wp_action,
    .touch      = wp_touch,
    .poll       = wp_poll,
    .fullscreen = false,   /* 显示系统状态栏; 雷达区已下移, 顶部保持白底 */
};

void os_page_wifiprobe_register(void)
{
    os_register(&s_wifiprobe);
}