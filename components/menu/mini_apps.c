/**
 * @file mini_apps.c
 * @brief 一批独立小应用 (工具 + 游戏), 完全自包含渲染 (内置 ASCII 字体 + st7305).
 *
 * 本文件所有渲染仅依赖 st7305 驱动与自身字体, 不读取 SD/网络/字体文件,
 * 保证任何情况下都能稳定工作. 应用内状态全部用静态变量, 无动态分配.
 */
#include "mini_apps.h"
#include "menu_system.h"
#include "input.h"
#include "st7305.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "font_zh.h"   /* NETTOOL: 分类/工具中文名 */
#include "wifi_manager.h"  /* NETTOOL: 联网/扫描/状态 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define SW 400   /* ST7305_WIDTH */
#define SH 300   /* ST7305_HEIGHT */

/* ================= 通用绘制/字体 (自包含) ================= */
static inline void setp(st7305_handle_t *l, int x, int y) {
    if (x < 0 || x >= SW || y < 0 || y >= SH) return;
    st7305_draw_pixel(l, x, y, ST7305_COLOR_BLACK);
}
static inline void clrp(st7305_handle_t *l, int x, int y) {
    if (x < 0 || x >= SW || y < 0 || y >= SH) return;
    st7305_draw_pixel(l, x, y, ST7305_COLOR_WHITE);
}
static void filr(st7305_handle_t *l, int x0, int y0, int x1, int y1, int col) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            st7305_draw_pixel(l, x, y, (st7305_color_t)col);
}
static void outline(st7305_handle_t *l, int x0, int y0, int x1, int y1) {
    filr(l, x0, y0, x1, y0, 1);
    filr(l, x0, y1, x1, y1, 1);
    filr(l, x0, y0, x0, y1, 1);
    filr(l, x1, y0, x1, y1, 1);
}

/* 内置 5x7 ASCII 字体, 每字符 5 列 (MSB=最上一行). 索引 = ASCII - 0x20. */
static const uint8_t gfont[][5] = {
    {0,0,0,0,0},{0,0,0x4F,0,0},{0,0x07,0,0x07,0},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
    {0,0x05,0x03,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x14,0x08,0x3E,0x08,0x14},
    {0x08,0x08,0x3E,0x08,0x08},{0,0x50,0x30,0,0},{0x08,0x08,0x08,0x08,0x08},{0,0x60,0x60,0,0},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0},{0,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x59,0x51,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0,0x41,0x7F,0x41,0},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x0F,0x30,0x40,0x30,0x0F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};
static int glyph_idx(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    int i = (c - 0x20);
    if (i < 0 || i > 58) return -1;
    return i;
}
/* 绘制 ASCII 字符串, 2x 放大 (字高 14, 字宽 10, 间距 2 => 12px/char). 返回新 x. */
static int txt(st7305_handle_t *l, int x, int y, const char *s) {
    while (*s) {
        int gi = glyph_idx(*s);
        if (gi >= 0) {
            const uint8_t *g = gfont[gi];
            for (int cx = 0; cx < 5; cx++)
                for (int cy = 0; cy < 7; cy++)
                    if (g[cx] & (1 << cy)) { setp(l, x + cx * 2, y + cy * 2); setp(l, x + cx * 2 + 1, y + cy * 2); setp(l, x + cx * 2, y + cy * 2 + 1); setp(l, x + cx * 2 + 1, y + cy * 2 + 1); }
        }
        x += 12;
        s++;
    }
    return x;
}
static int txtw(const char *s) { return (int)strlen(s) * 12; }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ================= 应用注册表 ================= */
typedef void (*mini_render_t)(st7305_handle_t *l);
typedef void (*mini_action_t)(menu_state_t *st, menu_action_t a);
typedef bool (*mini_poll_t)(menu_state_t *st);
typedef void (*mini_touch_t)(menu_state_t *st, int x, int y);
typedef void (*mini_init_t)(void);

typedef struct {
    const char *title;
    mini_init_t    init;
    mini_render_t  render;
    mini_action_t  action;
    mini_poll_t    poll;
    mini_touch_t   touch;
} mini_app_t;

static const mini_app_t s_apps[];
#define ALEN (sizeof(s_apps)/sizeof(s_apps[0]))

static int      s_view = 0;        /* 0=未打开, 1=已打开应用 (启动器已删除) */
static int      s_app  = -1;       /* 当前应用索引 */

/* ================= 计算器 ================= */
static char s_calc_expr[40];
static int  s_calc_len;
static bool s_calc_err;
static int  s_calc_sel;            /* 选中按键索引 */
static const char *s_calc_keys = "C<\376\367/789*456-123+0.=X";
/* 按键网格: 4 列 x 5 行; 字符映射: \376=C98/243, \367=退格 */ 

static double eval_expr(const char *s, bool *ok) {
    double nums[32]; char ops[32]; int nc = 0, oc = 0; bool have = false; double cur = 0; bool unary = true;
    *ok = true;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == ' ') continue;
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); have = true; unary = false; }
        else if (c == '.' && have && !unary && ((int)p==0)) { /* keep simple */ }
        else if ((c=='.') && !(nums[nc])) { cur *= 1; } /* noop */
        else {
            /* operator */
            if (c == '-' && unary) { /* negative literal */ cur = -cur; }
            if (have) { nums[nc++] = cur; cur = 0; have = false; unary = true; }
            ops[oc++] = c;
        }
    }
    if (have) nums[nc++] = cur;
    if (nc == 0) { *ok = false; return 0; }
    /* pass1: * and / */
    for (int i = 0; i < oc; ) {
        if (ops[i] == '*' || ops[i] == '/' || ops[i] == 0x00) {
            if (ops[i] == '/' && nums[i+1] == 0) { *ok = false; return 0; }
            double v = (ops[i] == '*') ? (nums[i] * nums[i+1]) : (nums[i] / nums[i+1]);
            for (int j = i; j < oc - 1; j++) nums[j+1] = nums[j+2];
            nums[i] = v;
            for (int j = i; j < oc - 1; j++) ops[j] = ops[j+1];
            oc--;
        } else i++;
    }
    /* pass2: + and - */
    double r = nums[0];
    for (int i = 0; i < oc; i++)
        if (ops[i] == '+') r += nums[i+1];
        else if (ops[i] == '-') r -= nums[i+1];
    return r;
}
static void calc_format(double v, char *out, int cap) {
    if (isnan(v) || isinf(v)) { out[0]='E'; out[1]=0; return; }
    if (fabs(v) > 99999999.0 || fabs(v) < 0.0000001) { snprintf(out, cap, "%g", v); return; }
    snprintf(out, cap, "%.6f", v);
    size_t n = strlen(out);
    while (n > 0 && out[n-1] == '0') { out[n-1] = 0; n--; }
    if (n > 0 && out[n-1] == '.') out[n-1] = 0;
}
static void calc_init(void) { s_calc_len = 0; s_calc_expr[0] = 0; s_calc_err = false; s_calc_sel = 0; }
static void calc_render(st7305_handle_t *l) {
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 4, "CALCULATOR");
    outline(l, 8, 8 + 12 + 6, SW - 8, 62);
    char disp[48];
    if (s_calc_err) { disp[0]='E'; disp[1]=0; }
    else if (s_calc_len == 0) { disp[0]='0'; disp[1]=0; }
    else { memcpy(disp, s_calc_expr, s_calc_len); disp[s_calc_len]=0; }
    int dw = txtw(disp); int dx = SW - 8 - 6 - dw; if (dx < 12) dx = 12;
    txt(l, dx, 34, disp);
    /* 按键 4x5 */
    int x0 = 40, y0 = 74, cw = (SW - 80 - 3 * 6) / 4, ch = (SH - 74 - 10 - 3 * 6) / 5;
    if (cw > 60) cw = 60;
    if (ch > 34) ch = 34;
    int gx = (SW - (cw * 4 + 6 * 3)) / 2;
    for (int i = 0; i < 20; i++) {
        int r = i / 4, c = i % 4;
        int bx = gx + c * (cw + 6), by = y0 + r * (ch + 6);
        outline(l, bx, by, bx + cw, by + ch);
        if (i == s_calc_sel) filr(l, bx + 1, by + 1, bx + cw - 1, by + ch - 1, 1);
        char lab[2]; lab[0] = s_calc_keys[i]; lab[1] = 0;
        int lx = bx + (cw - txtw(lab)) / 2;
        int ly = by + (ch - 14) / 2;
        txt(l, lx, ly, lab);
    }
}
static void calc_key(int i) {
    char k = s_calc_keys[i];
    if (k == 'C') { s_calc_len = 0; s_calc_expr[0]=0; s_calc_err=false; return; }
    if (k == '<') { if (s_calc_len > 0) { s_calc_len--; s_calc_expr[s_calc_len]=0; } return; }
    if (k == 'X') { s_calc_err = false; s_calc_len = 0;
        bool ok; double v = eval_expr(s_calc_expr, &ok);
        if (!ok) { s_calc_expr[0]='E'; s_calc_len=1; s_calc_err=true; return; }
        char tmp[40]; calc_format(v, tmp, sizeof(tmp));
        int n = (int)strlen(tmp); if (n > 38) n = 38;
        memcpy(s_calc_expr, tmp, n); s_calc_expr[n]=0; s_calc_len = n;
        return; }
    if (s_calc_len < 38) { s_calc_expr[s_calc_len++] = k; s_calc_expr[s_calc_len]=0; }
}
static void calc_action(menu_state_t *st, menu_action_t a) {
    (void)st;
    switch (a) {
        case MENU_ACTION_RIGHT: if (s_calc_sel % 4 < 3) s_calc_sel++; break;
        case MENU_ACTION_LEFT:  if (s_calc_sel % 4 > 0) s_calc_sel--; break;
        case MENU_ACTION_DOWN:  if (s_calc_sel < 16) s_calc_sel += 4; break;
        case MENU_ACTION_UP:    if (s_calc_sel >= 4) s_calc_sel -= 4; break;
        case MENU_ACTION_CONFIRM: calc_key(s_calc_sel); break;
        default: break;
    }
}
static void calc_touch(menu_state_t *st, int x, int y) {
    (void)st;
    int x0 = 40, y0 = 74, cw = 60, ch = 34; int gx = 200 - (cw * 4 + 18) / 2;
    for (int i = 0; i < 20; i++) {
        int r = i/4, c = i%4; int bx = gx + c*(cw+6), by = y0 + r*(ch+6);
        if (x >= bx && x <= bx+cw && y >= by && y <= by+ch) { calc_key(i); return; }
    }
}

/* ================= 秒表 ================= */
static bool s_sw_run; static uint32_t s_sw_accum, s_sw_last, s_sw_lap; static bool s_sw_lapset; static int s_sw_laps;
static void sw_init(void){ s_sw_run=false; s_sw_accum=0; s_sw_lap=0; s_sw_lapset=false; s_sw_laps=0; }
static void sw_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 6, "STOPWATCH");
    char b[32];
    uint32_t t = s_sw_accum; uint32_t cs = t%100; uint32_t ss=(t/100)%60; uint32_t mm=t/6000;
    snprintf(b,sizeof(b),"%02u:%02u.%02u",(unsigned)mm,(unsigned)ss,(unsigned)cs);
    int x = (SW - txtw(b))/2; txt(l, x, 90, b);
    if (s_sw_lapset){ char b2[32]; snprintf(b2,sizeof(b2),"LAP %4u",(unsigned)s_sw_laps); txt(l,(SW-txtw(b2))/2,140,b2); }
    txt(l, 30, SH-40, s_sw_run?"*RUN*":"[STOP]");
    txt(l, SW-8-txtw("CONF:START"), SH-40, "CONF:START");
    txt(l, 30, SH-22, "UP:RESET");
    txt(l, SW-8-txtw("DOWN:LAP"), SH-22, "DOWN:LAP");
}
static void sw_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_CONFIRM:
            if (s_sw_run){ s_sw_accum += (now_ms()-s_sw_last); s_sw_run=false; }
            else { s_sw_last = now_ms(); s_sw_run = true; s_sw_lap = s_sw_accum; }
            break;
        case MENU_ACTION_UP: s_sw_accum=0; s_sw_run=false; s_sw_lapset=false; s_sw_laps=0; break;
        case MENU_ACTION_DOWN:
            if (s_sw_run) { s_sw_lap = s_sw_accum; s_sw_laps++; s_sw_lapset=true; }
            break;
        default: break;
    }
}
static bool sw_poll(menu_state_t *st){ 
    (void)st;
    if(!s_sw_run) return false;
    uint32_t nn = now_ms();
    uint32_t dt = nn - s_sw_last;
    uint32_t old = s_sw_accum;
    s_sw_accum += dt / 10;   /* 累加百分之一秒 */
    s_sw_last = nn;
    return (s_sw_accum != old);
}

/* 秒表 poll 修正: 需要逐帧累计 */
/* ================= 倒计时 ================= */
static int  s_cd_sec; static bool s_cd_run; static uint32_t s_cd_last, s_cd_rem; static bool s_cd_ring;
static void cd_init(void){ s_cd_sec=300; s_cd_run=false; s_cd_rem=0; s_cd_ring=false; }
static void cd_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 6, "COUNTDOWN");
    char b[40];
    if (s_cd_ring){ snprintf(b,sizeof(b),"TIME!"); }
    else { uint32_t t = s_cd_run?s_cd_rem:(uint32_t)s_cd_sec; snprintf(b,sizeof(b),"%02u:%02u",(unsigned)(t/60),(unsigned)(t%60)); }
    txt(l,(SW-txtw(b))/2,100,b);
    txt(l, 20, 160, "CONF:START");
    txt(l, SW-8-txtw("CONF:PAUSE"),160,"CONF:PAUSE");
    txt(l, 20, 185, "UP/DOWN:SET");
    txt(l, SW-8-txtw("CLEAR:UP+LONG"),185,"CLEAR:UP+LONG");
    txt(l, (SW-txtw("TIME!=\xF1" ))/2, 220, "TIME! = FLASH"); 
    (void)s_cd_run;
}
/* 补: 上面模板备注 REVISE */
static void cd_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_CONFIRM:
            if (s_cd_ring) { s_cd_ring=false; }
            if (s_cd_run){ s_cd_rem = s_cd_rem - (now_ms()-s_cd_last); if ((int)s_cd_rem<0) s_cd_rem=0; s_cd_run=false; }
            else { s_cd_rem = (uint32_t)s_cd_sec; s_cd_last = now_ms(); s_cd_run = true; }
            break;
        case MENU_ACTION_UP:   if(!s_cd_run){ if(s_cd_sec<99*60) s_cd_sec+=60; } break;
        case MENU_ACTION_DOWN: if(!s_cd_run){ if(s_cd_sec>60) s_cd_sec-=60; } break;
        default: break;
    }
}
static bool cd_poll(menu_state_t *st){
    (void)st;
    if (!s_cd_run) return false;
    uint32_t now = now_ms();
    if (now - s_cd_last >= 1000) {
        s_cd_last += 1000;
        if (s_cd_rem > 0) { s_cd_rem--; }
        if (s_cd_rem == 0) { s_cd_run = false; s_cd_ring = true; }
        return true;
    }
    return false;
}

/* ================= 日历 ================= */
static int s_cal_off; /* 月偏移 (0=本月) */
static void cal_init(void){ s_cal_off = 0; }
static void cal_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    t.tm_mon += s_cal_off;
    if (t.tm_mon < 0) { t.tm_mon += 12; t.tm_year--; }
    if (t.tm_mon > 11) { t.tm_mon -= 12; t.tm_year++; }
    t.tm_mday = 1; t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
    time_t tm0 = mktime(&t);
    struct tm m0 = *localtime(&tm0);
    int y = m0.tm_year + 1900, mo = m0.tm_mon + 1;
    int first_dow = m0.tm_wday;
    int nday = 31; 
    if (mo == 4 || mo == 6 || mo == 9 || mo == 11) nday = 30;
    else if (mo == 2) nday = ((y%4==0 && y%100!=0) || y%400==0) ? 29 : 28;
    char head[32]; snprintf(head,sizeof(head),"%04d-%02d",y,mo);
    int hw = txtw(head); txt(l,(SW-hw)/2,8,head);
    const char *wd = "S M T W T F S";
    txt(l, (SW-txtw(wd))/2, 30, wd);
    int gx = 30, gy = 54, cw = (SW-60)/7, chh = 30;
    for (int d = 1; d <= nday; d++) {
        int idx = (first_dow + d - 1);
        int r = idx / 7, c = idx % 7;
        char dn[4]; snprintf(dn,sizeof(dn),"%02d",d);
        int x = gx + c*cw; int y = gy + r*chh;
        bool is_today = (s_cal_off==0 && d==(localtime(&now)->tm_mday));
        if (is_today) filr(l, x+2, y+2, x+cw-3, y+13, 1);
        int twgtxt = txtw(dn); txt(l, x + (cw-twgtxt)/2, y+3, dn);
    }
    txt(l, 8, SH-24, "<<MONTH>>");
    txt(l, SW-8-txtw("RESET:CONF"), SH-24, "RESET:CONF");
}
static void cal_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_LEFT:  s_cal_off--; break;
        case MENU_ACTION_RIGHT: s_cal_off++; break;
        case MENU_ACTION_UP:    s_cal_off-=12; break;
        case MENU_ACTION_DOWN:  s_cal_off+=12; break;
        case MENU_ACTION_CONFIRM: s_cal_off=0; break;
        default: break;
    }
}

/* ================= 白板 ================= */
static int s_wb_lastx, s_wb_lasty; static bool s_wb_had;
static void wb_init(void){ s_wb_had = false; }
static void wb_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 4, "WHITEBOARD");
    txt(l, SW-8-txtw("CONF:CLEAR"),4,"CONF:CLEAR");
    outline(l, 4, 24, SW-4, SH-4);   /* 可绘画区 */
}
static bool wb_poll(menu_state_t *st){
    (void)st;
    int tx, ty;
    if (!input_get_touch_pos(&tx, &ty)) { if (s_wb_had) s_wb_had=false; return false; }
    if (tx<5||tx>SW-5||ty<25||ty>SH-5) return false;
    if (!s_wb_had) { setp(st->lcd, tx, ty); s_wb_had=true; s_wb_lastx=tx; s_wb_lasty=ty; return true; }
    int x0=s_wb_lastx, y0=s_wb_lasty, x1=tx, y1=ty;
    int dx = abs(x1-x0), sx = x0<x1?1:-1, sy = y0<y1?1:-1, err = dx-abs(y1-y0);
    for (;;) { setp(st->lcd,x0,y0); if(x0==x1&&y0==y1) break; int e2=2*err; if(e2>-abs(y1-y0)){err-=abs(y1-y0);x0+=sx;} if(e2<dx){err+=dx;y0+=sy;} }
    s_wb_lastx=tx; s_wb_lasty=ty;
    return true;
}
static void wb_action(menu_state_t *st, menu_action_t a){
    if (a==MENU_ACTION_CONFIRM){ st7305_clear(st->lcd, ST7305_COLOR_WHITE); }
}

/* ================= 骰子 ================= */
static int s_die[3]; static int s_die_roll;
static void dice_init(void){ s_die[0]=1; s_die[1]=1; s_die[2]=1; }
static void dice_draw_die(st7305_handle_t *l,int cx,int cy,int val){
    filr(l,cx-30,cy-30,cx+30,cy+30,1);
    outline(l,cx-30,cy-30,cx+30,cy+30);
    int dp = 8;
    /* dot positions by value */
    switch(val){
        case 1: setp(l,cx,cy); break;
        case 2: setp(l,cx-dp,cy-dp); setp(l,cx+dp,cy+dp); break;
        case 3: setp(l,cx-dp,cy-dp); setp(l,cx,cy); setp(l,cx+dp,cy+dp); break;
        case 4: setp(l,cx-dp,cy-dp); setp(l,cx+dp,cy-dp); setp(l,cx-dp,cy+dp); setp(l,cx+dp,cy+dp); break;
        case 5: setp(l,cx-dp,cy-dp); setp(l,cx+dp,cy-dp); setp(l,cx,cy); setp(l,cx-dp,cy+dp); setp(l,cx+dp,cy+dp); break;
        case 6: { for(int k=0;k<2;k++){ int py = cy-dp + k*2*dp; setp(l,cx-dp,py); setp(l,cx,py); setp(l,cx+dp,py);} } break;
    }
}
static void dice_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l,(SW-txtw("DICE"))/2,8,"DICE");
    for (int i=0;i<3;i++) dice_draw_die(l, SW/2 + (i-1)*96, 130, s_die[i]);
    txt(l,(SW-txtw("CONF:ROLL"))/2, SH-40, "CONF:ROLL");
}
static void dice_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (a==MENU_ACTION_CONFIRM){ s_die[0]=rand()%6+1; s_die[1]=rand()%6+1; s_die[2]=rand()%6+1; }
}

/* ================= 单位换算 ================= */
static const struct { const char *name; double fac; } s_units[] = {
    {"MM",1.0},{"CM",10.0},{"M",1000.0},{"KM",1000000.0},
    {"IN",25.4},{"FT",304.8},{"YD",914.4},{"MI",1609344.0}
};
#define UN_COUNT (sizeof(s_units)/sizeof(s_units[0]))
static double s_un_val; static int s_un_from, s_un_to, s_un_sel; static int s_un_focus; /*0=val,1=from,2=to*/
static void un_init(void){ s_un_val=1; s_un_from=0; s_un_to=2; s_un_sel=0; s_un_focus=0; }
static void un_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 6, "UNIT CONVERT (LENGTH)");
    char b[32];
    snprintf(b,sizeof(b),"%.6g %s",s_un_val,s_units[s_un_from].name);
    txt(l, 20, 40, "VALUE:");
    txt(l, 20+txtw("VALUE:"), 40, b);
    outline(l, SW-8-txtw("RES:MIN"),30, SW-8,54);
    /* from */
    txt(l, 20, 90, "FROM:");
    txt(l, 20+txtw("FROM:"), 90, s_units[s_un_from].name);
    /* to */
    txt(l, 20, 130, "TO:");
    txt(l, 20+txtw("TO:"), 130, s_units[s_un_to].name);
    double v = s_un_val * s_units[s_un_from].fac / s_units[s_un_to].fac;
    char r[32]; snprintf(r,sizeof(r),"=");
    txt(l, 20, 190, "RESULT:");
    char rr[32]; snprintf(rr,sizeof(rr),"%.6g %s", v, s_units[s_un_to].name);
    txt(l, 20+txtw("RESULT:"), 190, rr);
    if (s_un_focus==0) outline(l,16,32,SW-16,58);
    if (s_un_focus==1) outline(l,16,84,SW-16,118);
    if (s_un_focus==2) outline(l,16,124,SW-16,158);
    txt(l, 8, SH-20, "UP/DN:VAL  L/R:SEL");
}
static void un_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_LEFT:
            if (s_un_focus==1) { s_un_from=(s_un_from-1+UN_COUNT)%UN_COUNT; }
            else if (s_un_focus==2) { s_un_to=(s_un_to-1+UN_COUNT)%UN_COUNT; }
            break;
        case MENU_ACTION_RIGHT:
            if (s_un_focus==1) s_un_from=(s_un_from+1)%UN_COUNT;
            else if (s_un_focus==2) s_un_to=(s_un_to+1)%UN_COUNT;
            break;
        case MENU_ACTION_UP:
            if (s_un_focus==0){ s_un_val*=10; if(s_un_val>1e8) s_un_val=1e8; }
            else if (s_un_focus==3) break;
            break;
        case MENU_ACTION_DOWN:
            if (s_un_focus==0){ s_un_val/=10; if(s_un_val<1e-6) s_un_val=1e-6; }
            break;
        case MENU_ACTION_CONFIRM: s_un_focus = (s_un_focus+1)%3; break;
        case MENU_ACTION_HOME: break;
        default: break;
    }
}
static void un_touch(menu_state_t *st, int x, int y){
    (void)st;
    if (y>=30 && y<=58) s_un_focus=0;
    else if (y>=84 && y<=118) s_un_focus=1;
    else if (y>=124 && y<=158) s_un_focus=2;
}
/* 简单位换算 val 通过 UP/DOWN 以 10x 步进, RES:MIN 提示 */

/* ================= 井字棋 ================= */
static int s_tt[9]; /* 0=空,1=X,2=O */ static int s_tt_turn, s_tt_cx, s_tt_cy, s_tt_over, s_tt_winner;
static void tt_init(void){ memset(s_tt,0,9*sizeof(int)); s_tt_turn=1; s_tt_cx=1; s_tt_cy=1; s_tt_over=0; s_tt_winner=0; }
static int tt_check(void){
    for (int i=0;i<3;i++){ if(s_tt[i*3]&&s_tt[i*3]==s_tt[i*3+1]&&s_tt[i*3+1]==s_tt[i*3+2]) return s_tt[i*3]; }
    for (int j=0;j<3;j++){ if(s_tt[j]&&s_tt[j]==s_tt[3+j]&&s_tt[3+j]==s_tt[6+j]) return s_tt[j]; }
    if(s_tt[0]&&s_tt[0]==s_tt[4]&&s_tt[4]==s_tt[8]) return s_tt[0];
    if(s_tt[2]&&s_tt[2]==s_tt[4]&&s_tt[4]==s_tt[6]) return s_tt[2];
    int full=1; for(int i=0;i<9;i++) if(!s_tt[i]) full=0;
    if(full) return 3;
    return 0;
}
static void tt_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 8, "TIC TAC TOE");
    char h[24]; 
    if (s_tt_over) snprintf(h,sizeof(h),"WIN:%c", s_tt_winner==3?'-':(s_tt_winner==1?'X':'O'));
    else snprintf(h,sizeof(h),"TURN:%c", s_tt_turn==1?'X':'O');
    txt(l,(SW-txtw(h))/2, 8, h);
    int gx=80, gy=70, cw=80, chh=70;
    for(int i=0;i<3;i++){ filr(l,gx+i*(cw+10)-2,0,gx+i*(cw+10)+4,SH,1); (void)(""); }
    /* 画网格线 */
    for(int i=1;i<3;i++){ int x=gx+i*(cw+10)-(cw+10)/2; filr(l,x-2,gy,x+3,gy+3*chh+(3*10),1); }
    (void)chh;
    for (int r=0;r<3;r++) for(int c=0;c<3;c++){ int x=gx+c*(cw+10), y=gy+r*(chh+10); if(s_tt[r*3+c]){ char sym[2]; sym[0]=s_tt[r*3+c]==1?'X':'O'; sym[1]=0; txt(l,x+(cw-txtw(sym))/2,y+(chh-14)/2,sym); } }
    if(!s_tt_over){ outline(l,gx+s_tt_cx*(cw+10)-2,gy+s_tt_cy*(chh+10)-4,gx+s_tt_cx*(cw+10)+cw+2,gy+s_tt_cy*(chh+10)+chh+2); }
    txt(l, 8, SH-22, "ARROWS:MOVE CONF:PUT");
}
static void tt_action(menu_state_t *st, menu_action_t a){
    if (s_tt_over){ if(a==MENU_ACTION_CONFIRM) tt_init(); return; }
    switch(a){
        case MENU_ACTION_LEFT:  if(s_tt_cx>0)s_tt_cx--; break;
        case MENU_ACTION_RIGHT: if(s_tt_cx<2)s_tt_cx++; break;
        case MENU_ACTION_UP:    if(s_tt_cy>0)s_tt_cy--; break;
        case MENU_ACTION_DOWN:  if(s_tt_cy<2)s_tt_cy++; break;
        case MENU_ACTION_CONFIRM:
            if(s_tt[s_tt_cy*3+s_tt_cx]==0){ s_tt[s_tt_cy*3+s_tt_cx]=s_tt_turn; int w=tt_check(); if(w){s_tt_over=1;s_tt_winner=w;} else s_tt_turn=(s_tt_turn==1)?2:1; }
            break;
        default: break;
    }
}
static void tt_touch(menu_state_t *st, int x, int y){
    (void)st;
    int gx=80, gy=70, cw=80, chh=70;
    int c=(x-gx)/(cw+10), r=(y-gy)/(chh+10);
    if(c>=0&&c<3&&r>=0&&r<3){ s_tt_cx=c; s_tt_cy=r;
       if(s_tt[r*3+c]==0){ s_tt[r*3+c]=s_tt_turn; int w=tt_check(); if(w){s_tt_over=1;s_tt_winner=w;} else s_tt_turn=(s_tt_turn==1)?2:1; } }
}

/* ================= 记忆翻牌 ================= */
#define M_R 4
#define M_C 3
#define M_N (M_R*M_C/2)
static int s_mem[ M_R*M_C ]; static bool s_mem_face[M_R*M_C]; static bool s_mem_match[M_R*M_C];
static int s_mem_cx, s_mem_cy, s_mem_f1, s_mem_f2, s_mem_flips, s_mem_done;
static void mm_init(void){
    int a[M_R*M_C]; for(int i=0;i<M_R*M_C;i++) a[i]=i%(M_R*M_C/2);
    /* shuffle */
    for(int i=M_R*M_C-1;i>0;i--){ int j=rand()%(i+1); int t=a[i]; a[i]=a[j]; a[j]=t; }
    for(int i=0;i<M_R*M_C;i++){ s_mem[i]=a[i]; s_mem_face[i]=false; s_mem_match[i]=false; }
    s_mem_cx=0; s_mem_cy=0; s_mem_f1=-1; s_mem_f2=-1; s_mem_flips=0; s_mem_done=0;
}
static void mm_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    char h[40]; snprintf(h,sizeof(h),"MEMORY FLIPS:%d",s_mem_flips); txt(l,8,6,h);
    if (s_mem_done){ txt(l,(SW-txtw("DONE! CONF:NEW"))/2, 30, "DONE! CONF:NEW"); }
    int gx=60, gy=60, cw=90, chh=55;
    for(int r=0;r<M_R;r++) for(int c=0;c<M_C;c++){
        int x=gx+c*(cw+10), y=gy+r*(chh+10); int id=r*M_C+c;
        outline(l,x,y,x+cw,y+chh);
        if (s_mem_face[id] || s_mem_match[id]){
            char sym[2]; sym[0]='A'+s_mem[id]; sym[1]=0; txt(l,x+(cw-txtw(sym))/2,y+(chh-14)/2,sym);
        } else { /* 背面成块 */ filr(l,x+1,y+1,x+cw-1,y+chh-1,1); }
        if (id==(s_mem_cy*M_C+s_mem_cx) && !s_mem_done) outline(l,x-3,y-3,x+cw+3,y+chh+3);
    }
    txt(l,(SW-txtw("CONF:FLIP"))/2, SH-22, "CONF:FLIP");
}
static void mm_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_mem_done){ if(a==MENU_ACTION_CONFIRM) mm_init(); return; }
    switch(a){
        case MENU_ACTION_LEFT:  if(s_mem_cx>0)s_mem_cx--; break;
        case MENU_ACTION_RIGHT: if(s_mem_cx<M_C-1)s_mem_cx++; break;
        case MENU_ACTION_DOWN:  if(s_mem_cy<M_R-1)s_mem_cy++; break;
        case MENU_ACTION_UP:    if(s_mem_cy>0)s_mem_cy--; break;
        case MENU_ACTION_CONFIRM: {
            int id=s_mem_cy*M_C+s_mem_cx;
            if (s_mem_match[id]||s_mem_face[id]) break;
            if (s_mem_f1<0){ s_mem_face[id]=true; s_mem_f1=id; }
            else if (s_mem_f2<0 && id!=s_mem_f1){ s_mem_face[id]=true; s_mem_f2=id; s_mem_flips++; }
            if (s_mem_f1>=0&&s_mem_f2>=0){
                if (s_mem[s_mem_f1]==s_mem[s_mem_f2]){ s_mem_match[s_mem_f1]=true; s_mem_match[s_mem_f2]=true; s_mem_f1=-1; s_mem_f2=-1;
                    int done=0; for(int i=0;i<M_R*M_C;i++) if(s_mem_match[i]) done++; if(done==M_R*M_C) s_mem_done=1; }
                else { s_mem_face[s_mem_f1]=false; s_mem_face[s_mem_f2]=false; s_mem_f1=-1; s_mem_f2=-1; }
            }
        } break;
        default: break;
    }
}
static void mm_touch(menu_state_t *st, int x, int y){
    (void)st;
    int gx=60, gy=60, cw=90, chh=55;
    int c=(x-gx)/(cw+10), r=(y-gy)/(chh+10);
    if(c>=0&&c<M_C&&r>=0&&r<M_R){ s_mem_cx=c; s_mem_cy=r; menu_action_t a=MENU_ACTION_CONFIRM; (void)a; }
}

/* ================= 猜数字 ================= */
static int s_gs_ans, s_gs_guess, s_gs_try, s_gs_state; /* 0=play,1=won */
static void gs_init(void){ s_gs_ans=rand()%100; s_gs_guess=50; s_gs_try=0; s_gs_state=0; }
static void gs_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 8, "GUESS 0-99");
    char b[40]; snprintf(b,sizeof(b),"GUESS:%02d",s_gs_guess); txt(l,(SW-txtw(b))/2,70,b);
    txt(l, 20, 120, "UP:+1  DOWN:-1");
    txt(l, 20, 150, "LEFT:-10 RIGHT:+10");
    if (s_gs_state==1){ txt(l,(SW-txtw("YOU WIN! CONF:NEW"))/2,190,"YOU WIN! CONF:NEW"); }
    txt(l, 20, 230, "TRIES:"); char tc[4]; snprintf(tc,sizeof(tc),"%d",(int)s_gs_try); txt(l,20+txtw("TRIES:"),230,tc);
    txt(l, 20, 254, "CONF:GUESS");
}
static void gs_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_gs_state==1){ if(a==MENU_ACTION_CONFIRM) gs_init(); return; }
    switch(a){
        case MENU_ACTION_UP:   if(s_gs_guess<99)s_gs_guess++; break;
        case MENU_ACTION_DOWN: if(s_gs_guess>0)s_gs_guess--; break;
        case MENU_ACTION_LEFT: if(s_gs_guess>=10)s_gs_guess-=10; else s_gs_guess=0; break;
        case MENU_ACTION_RIGHT:if(s_gs_guess<=89)s_gs_guess+=10; else s_gs_guess=99; break;
        case MENU_ACTION_CONFIRM: if(s_gs_guess==s_gs_ans) s_gs_state=1; else s_gs_try++; break;
        default: break;
    }
}

/* ================= 2048 ================= */
static int s_204[16]; static int s_204_score, s_204_done; 
static void tw_add(void);
static void tw_init(void){ memset(s_204,0,sizeof(s_204)); s_204_score=0; s_204_done=0; tw_add(); tw_add(); }
static void tw_add(void){
    int empty[16], n=0; for(int i=0;i<16;i++) if(!s_204[i]) empty[n++]=i;
    if(n==0) return;
    int i = empty[rand()%n]; s_204[i] = ((rand()%10)==0)?4:2;
}
static int tw_line_mv(int *a){
    /* a: 4 items, move left, merge; returns score & moved */
    int out[4]={0,0,0,0}, oi=0, score=0, moved=0, last=0, lastset=0;
    for(int i=0;i<4;i++){ if(!a[i]) continue; if(lastset && last==a[i]){ out[oi-1]=last*2; score+=last*2; lastset=0; moved=1; } else { out[oi++]=a[i]; last=a[i]; lastset=1; } }
    for(int i=0;i<4;i++) if(out[i]!=a[i]) moved=1;
    for(int i=0;i<4;i++) a[i]=out[i];
    return score;
}
static void tw_slide(int dir){
    /* dir 0 L 1 R 2 U 3 D */
    int before[16]; for(int i=0;i<16;i++) before[i]=s_204[i];
    int g=0;
    if (dir==0 || dir==1){
        for(int r=0;r<4;r++){
            int a[4]; for(int c=0;c<4;c++) a[c]=s_204[r*4+c];
            if(dir==1){ int t[4]; for(int c=0;c<4;c++) t[3-c]=a[c]; for(int c=0;c<4;c++) a[c]=t[3-c]; }
            int sc=tw_line_mv(a);
            if(dir==1){ int t[4]; for(int c=0;c<4;c++) t[3-c]=a[c]; for(int c=0;c<4;c++) a[c]=t[3-c]; }
            for(int c=0;c<4;c++) s_204[r*4+c]=a[c];
            g+=sc;
        }
    } else {
        for(int col=0;col<4;col++){
            int a[4]; for(int r=0;r<4;r++) a[r]=s_204[r*4+col];
            if(dir==3){ int t[4]; for(int r=0;r<4;r++) t[3-r]=a[r]; for(int r=0;r<4;r++) a[r]=t[3-r]; }
            int sc=tw_line_mv(a);
            if(dir==3){ int t[4]; for(int r=0;r<4;r++) t[3-r]=a[r]; for(int r=0;r<4;r++) a[r]=t[3-r]; }
            for(int r=0;r<4;r++) s_204[r*4+col]=a[r];
            g+=sc;
        }
    }
    int moved=0; for(int i=0;i<16;i++) if(s_204[i]!=before[i]) moved=1;
    s_204_score+=g;
    if (moved) tw_add();
    int any=0; for(int i=0;i<16;i++) if(s_204[i]==2048) any=1;
    if (any) s_204_done=1;
}
static void tw_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    char h[32]; snprintf(h,sizeof(h),"2048 SCORE:%d",s_204_score); txt(l,8,8,h);
    if (s_204_done) txt(l,(SW-txtw("WIN! CONF:NEW"))/2, SH-20, "WIN! CONF:NEW");
    int gx=80, gy=70, cw=70, chh=70;
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        int x=gx+c*(cw+12), y=gy+r*(chh+12);
        outline(l,x,y,x+cw,y+chh);
        if (s_204[r*4+c]){ char b[4]; snprintf(b,sizeof(b),"%d",s_204[r*4+c]); txt(l,x+(cw-txtw(b))/2,y+(chh-14)/2,b); }
    }
    txt(l, 8, SH-20, "ARROWS:MOVE");
}
static void tw_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_204_done){ if(a==MENU_ACTION_CONFIRM) tw_init(); return; }
    switch(a){
        case MENU_ACTION_LEFT: tw_slide(0); break;
        case MENU_ACTION_RIGHT: tw_slide(1); break;
        case MENU_ACTION_UP: tw_slide(2); break;
        case MENU_ACTION_DOWN: tw_slide(3); break;
        case MENU_ACTION_CONFIRM: tw_init(); break;
        default: break;
    }
}

/* ================= 弹球 (单人 vs 墙) ================= */
static int s_pp_y, s_pp_byx, s_pp_byy, s_pp_vx, s_pp_vy, s_pp_score, s_pp_over; static uint32_t s_pp_last;
static void pp_init(void){ s_pp_y=120; s_pp_byx=200; s_pp_byy=140; s_pp_vx=2; s_pp_vy=1; s_pp_score=0; s_pp_over=0; }
static void pp_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    char h[32]; snprintf(h,sizeof(h),"PONG SCORE:%d",s_pp_score); txt(l,8,6,h);
    if (s_pp_over){ txt(l,(SW-txtw("LOST CONF:NEW"))/2,140,"LOST CONF:NEW"); }
    /* 左墙 */
    filr(l, 6, 20, 8, SH-20, 1);
    /* 右拍 */
    filr(l, SW-12, s_pp_y, SW-8, s_pp_y+50, 1);
    /* 球 */
    filr(l, s_pp_byx, s_pp_byy, s_pp_byx+4, s_pp_byy+4, 1);
    txt(l, 110, SH-20, "UP/DOWN:MOVE");
    txt(l, SW-8-txtw("CONF:NEW"), SH-20, "CONF:NEW");
}
static bool pp_poll(menu_state_t *st){
    (void)st;
    if (s_pp_over) return false;
    uint32_t now = now_ms();
    if (now - s_pp_last < 12) return false;
    s_pp_last = now;
    s_pp_byx += s_pp_vx; s_pp_byy += s_pp_vy;
    if (s_pp_byy <= 18 || s_pp_byy >= SH-26) s_pp_vy = -s_pp_vy;
    /* 左墙反弹 */
    if (s_pp_byx <= 10){ s_pp_vx = 2; }
    /* 右拍接球 */
    if (s_pp_byx >= SW-20){
        int pc = s_pp_byy + 2;
        if (pc >= s_pp_y && pc <= s_pp_y + 50){
            s_pp_vx = -2;
            s_pp_score++;
            /* 按击球点微调垂直速度, 增加可玩性 */
            int mid = s_pp_y + 25;
            s_pp_vy = (s_pp_byy < mid) ? -((mid - s_pp_byy) / 12 + 1) : ((s_pp_byy - mid) / 12 + 1);
            if (s_pp_vy > 3) s_pp_vy = 3;
            if (s_pp_vy < -3) s_pp_vy = -3;
        } else {
            s_pp_over = 1;
        }
    }
    return true;
}
static void pp_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_pp_over){ if(a==MENU_ACTION_CONFIRM) pp_init(); return; }
    switch(a){
        case MENU_ACTION_UP: if(s_pp_y>22) s_pp_y-=6; break;
        case MENU_ACTION_DOWN: if(s_pp_y<SH-74) s_pp_y+=6; break;
        default: break;
    }
}

/* ================= 扫雷 MINESWEEPER ================= */
#define MW 9
#define MH 10
static int  s_mw_map[MW*MH], s_mw_open[MW*MH], s_mw_flg[MW*MH];
static int  s_mw_fx, s_mw_fy;
static int  s_mw_first, s_mw_over, s_mw_won, s_mw_mines;
static void mw_gen(void){
    for (int i=0;i<MW*MH;i++){ s_mw_map[i]=0; s_mw_open[i]=0; s_mw_flg[i]=0; }
    s_mw_mines=10; int placed=0; int guard=0;
    while (placed<s_mw_mines && guard<10000){
        guard++;
        int n=rand()%(MW*MH);
        if (s_mw_map[n]==9) continue;
        s_mw_map[n]=9; placed++;
    }
    for (int r=0;r<MH;r++)for(int c=0;c<MW;c++){
        if (s_mw_map[r*MW+c]==9) continue;
        int cnt=0;
        for (int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++){
            int rr=r+dr,cc=c+dc;
            if (rr>=0&&rr<MH&&cc>=0&&cc<MW && s_mw_map[rr*MW+cc]==9) cnt++;
        }
        s_mw_map[r*MW+c]=cnt;
    }
    s_mw_over=0; s_mw_won=0; s_mw_first=1;
}
static void mw_open_cell(int c,int r){
    if (r<0||r>=MH||c<0||c>=MW) return;
    int i=r*MW+c;
    if (s_mw_open[i]||s_mw_flg[i]) return;
    s_mw_open[i]=1;
    if (s_mw_map[i]==9){ s_mw_over=1; return; }
    if (s_mw_map[i]==0)
        for (int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++)
            mw_open_cell(c+dc,r+dr);
}
static void mw_init(void){ srand(now_ms()); s_mw_fx=MW/2; s_mw_fy=MH/2; mw_gen(); }
static void mw_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    int cw=27, x0=(SW-MW*cw)/2, y0=34; char b[16];
    for (int r=0;r<MH;r++)for(int c=0;c<MW;c++){
        int i=r*MW+c, xx=x0+c*cw, yy=y0+r*cw;
        outline(l,xx,yy,xx+cw-1,yy+cw-1);
        if (s_mw_open[i]){
            if (s_mw_map[i]==9){ filr(l,xx,yy,xx+cw-1,yy+cw-1,1); }
            else if (s_mw_map[i]>0){ snprintf(b,16,"%d",s_mw_map[i]); txt(l,xx+(cw-10)/2,yy+(cw-14)/2,b); }
        } else {
            if (s_mw_flg[i]) txt(l,xx+(cw-10)/2,yy+(cw-14)/2,"X");
            if (c==s_mw_fx&&r==s_mw_fy){ filr(l,xx,yy,xx+cw-1,yy,1); filr(l,xx,yy+cw-1,xx+cw-1,yy+cw-1,1);
                filr(l,xx,yy,xx,yy+cw-1,1); filr(l,xx+cw-1,yy,xx+cw-1,yy+cw-1,1); }
        }
    }
    int open=0; for (int r=0;r<MH;r++)for(int c=0;c<MW;c++) if (s_mw_open[r*MW+c]) open++;
    if (s_mw_over){ txt(l,8,SH-16,"BOOM! [OK]RETRY"); }
    else if (open==MW*MH - s_mw_mines){ s_mw_won=1; txt(l,8,SH-16,"WIN!  [OK]RETRY"); }
    else { snprintf(b,16,"%02dM",s_mw_mines); txt(l,8,SH-16,b); txt(l,SW-56,SH-16,"MINESWEEP"); }
}
static void mw_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_mw_over||s_mw_won){ if (a==MENU_ACTION_CONFIRM) mw_init(); return; }
    switch(a){
        case MENU_ACTION_LEFT:  if (s_mw_fx>0)         s_mw_fx--; break;
        case MENU_ACTION_RIGHT: if (s_mw_fx<MW-1)      s_mw_fx++; break;
        case MENU_ACTION_UP:    if (s_mw_fy>0)         s_mw_fy--; break;
        case MENU_ACTION_DOWN:  if (s_mw_fy<MH-1)      s_mw_fy++; break;
        case MENU_ACTION_CONFIRM: {
            int c=s_mw_fx,r=s_mw_fy;
            if (s_mw_first && s_mw_map[r*MW+c]==9){
                for (int k=0;k<MW*MH;k++) if (k!=r*MW+c && s_mw_map[k]!=9){ s_mw_map[k]=9; s_mw_map[r*MW+c]=0; break; }
            }
            s_mw_first=0; mw_open_cell(c,r);
        } break;
        default: break;
    }
}

/* ================= 俄罗斯方块 TETRIS ================= */
#define TBW 10
#define TBH 20
static int s_te_b[TBW*TBH];
static int s_te_type, s_te_rot, s_te_px, s_te_py;
static int s_te_score, s_te_over, s_te_pause;
static uint32_t s_te_last;
static const int s_te_shapes[7][4][2]={
    {{0,1},{1,1},{2,1},{3,1}},   /* I */
    {{1,0},{2,0},{1,1},{2,1}},   /* O */
    {{1,0},{0,1},{1,1},{2,1}},   /* T */
    {{1,0},{2,0},{0,1},{1,1}},   /* S */
    {{0,0},{1,0},{1,1},{2,1}},   /* Z */
    {{0,0},{0,1},{1,1},{2,1}},   /* J */
    {{2,0},{0,1},{1,1},{2,1}},   /* L */
};
static void te_cells(int *xs,int *ys,int type,int rot,int px,int py){
    const int(*b)[2]=s_te_shapes[type];
    for (int i=0;i<4;i++){
        int x=b[i][0],y=b[i][1];
        for (int r=0;r<rot;r++){ int t=x; x=1-(y-1); y=1+(t-1); }
        xs[i]=x+px; ys[i]=y+py;
    }
}
static int te_collide(int px,int py,int rot){
    int xs[4],ys[4]; te_cells(xs,ys,s_te_type,rot,px,py);
    for (int i=0;i<4;i++){
        int x=xs[i],y=ys[i];
        if (x<0||x>=TBW) return 1;
        if (y>=TBH) return 1;
        if (y>=0 && s_te_b[y*TBW+x]) return 1;
    }
    return 0;
}
static void te_new(void){ s_te_type=rand()%7; s_te_rot=0; s_te_px=3; s_te_py=-1; }
static void te_lock(void){
    int xs[4],ys[4]; te_cells(xs,ys,s_te_type,s_te_rot,s_te_px,s_te_py);
    for (int i=0;i<4;i++){ int r=ys[i],c=xs[i]; if (r>=0&&r<TBH&&c>=0&&c<TBW) s_te_b[r*TBW+c]=1; }
    for (int r=TBH-1;r>=0;r--){
        int full=1; for (int c=0;c<TBW;c++) if (!s_te_b[r*TBW+c]){full=0;break;}
        if (full){
            for (int rr=r;rr>0;rr--) for (int c=0;c<TBW;c++) s_te_b[rr*TBW+c]=s_te_b[(rr-1)*TBW+c];
            for (int c=0;c<TBW;c++) s_te_b[c]=0;
            s_te_score+=10; r++;
        }
    }
    te_new();
    if (te_collide(s_te_px,s_te_py,s_te_rot)) s_te_over=1;
}
static void te_step(void){
    if (s_te_over||s_te_pause) return;
    if (!te_collide(s_te_px,s_te_py+1,s_te_rot)) s_te_py++; else te_lock();
}
static void te_init(void){ srand(now_ms()); memset(s_te_b,0,sizeof(s_te_b)); s_te_over=0;s_te_score=0;s_te_pause=0; te_new(); s_te_last=now_ms(); }
static bool te_poll(menu_state_t *st){ (void)st;
    if (s_te_over||s_te_pause) return false;
    uint32_t n=now_ms();
    if (n-s_te_last>500){ s_te_last=n; te_step(); return true; }
    return false;
}
static void te_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_te_over){ if (a==MENU_ACTION_CONFIRM) te_init(); s_te_last=now_ms(); return; }
    if (a==MENU_ACTION_CONFIRM){ s_te_pause=!s_te_pause; s_te_last=now_ms(); return; }
    if (s_te_pause) return;
    int nx=s_te_px, ny=s_te_py, nr=s_te_rot;
    switch(a){
        case MENU_ACTION_LEFT:  nx--; break;
        case MENU_ACTION_RIGHT: nx++; break;
        case MENU_ACTION_DOWN:  if (!te_collide(s_te_px,ny+1,s_te_rot)){ s_te_py++; } s_te_last=now_ms(); break;
        case MENU_ACTION_UP:    nr=(s_te_rot+1)%4; break;
        default: break;
    }
    if ((a==MENU_ACTION_LEFT||a==MENU_ACTION_RIGHT||a==MENU_ACTION_UP)){
        if (nr!=s_te_rot){ if (!te_collide(nx,ny,nr)){ s_te_px=nx; s_te_rot=nr; } }
        else if (!te_collide(nx,ny,s_te_rot)){ s_te_px=nx; }
        s_te_last=now_ms();
    }
}
static void te_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    int c=18, x0=(SW-TBW*c)/2, y0=10; char b[24];
    for (int r=0;r<TBH;r++)for(int col=0;col<TBW;col++){
        if (s_te_b[r*TBW+col]) filr(l,x0+col*c,y0+r*c,x0+col*c+c-1,y0+r*c+c-1,1);
        else outline(l,x0+col*c,y0+r*c,x0+col*c+c-1,y0+r*c+c-1);
    }
    int xs[4],ys[4]; te_cells(xs,ys,s_te_type,s_te_rot,s_te_px,s_te_py);
    for (int i=0;i<4;i++){ int r=ys[i],col=xs[i];
        if (r>=0&&r<TBH&&col>=0&&col<TBW) filr(l,x0+col*c,y0+r*c,x0+col*c+c-2,y0+r*c+c-2,1); }
    snprintf(b,24,"SCORE %d",s_te_score);
    txt(l,8,SH-16,b);
    if (s_te_over) txt(l,8,3,"GAMEOVER [OK]RETRY");
    else if (s_te_pause) txt(l,8,3,"PAUSE [OK]");
}

/* ================= 打砖块 BREAKOUT ================= */
#define BKW 9
#define BKH 5
static int s_bk_brick[BKW*BKH];
static float s_bk_px,s_bk_bx,s_bk_by,s_bk_bvx,s_bk_bvy;
static int s_bk_over,s_bk_score;
static uint32_t s_bk_last;
static void bk_init(void){
    srand(now_ms()); for (int i=0;i<BKW*BKH;i++) s_bk_brick[i]=1;
    s_bk_px=172; s_bk_bx=200; s_bk_by=180; s_bk_bvx=2.2f; s_bk_bvy=2.2f;
    s_bk_over=0; s_bk_score=0; s_bk_last=now_ms();
}
static void bk_step(void){
    if (s_bk_over) return;
    s_bk_bx+=s_bk_bvx; s_bk_by+=s_bk_bvy;
    if (s_bk_bx<0){ s_bk_bx=0; s_bk_bvx=-s_bk_bvx; }
    if (s_bk_bx+6>SW){ s_bk_bx=SW-6; s_bk_bvx=-s_bk_bvx; }
    if (s_bk_by<0){ s_bk_by=0; s_bk_bvy=-s_bk_bvy; }
    if (s_bk_by+6>=272 && s_bk_by+6<=282 && s_bk_bx+6>s_bk_px && s_bk_bx<s_bk_px+56){
        s_bk_by=272-6; s_bk_bvy=-s_bk_bvy;
        float rel=(s_bk_bx+3)-(s_bk_px+28); s_bk_bvx=rel*0.06f;
    }
    if (s_bk_by>290){ s_bk_over=1; return; }
    int cw=38,ch=14,x0=(SW-BKW*cw)/2,y0=16;
    for (int r=0;r<BKH;r++)for(int c=0;c<BKW;c++){
        if (!s_bk_brick[r*BKW+c]) continue;
        int bx=x0+c*cw, by=y0+r*ch;
        if (s_bk_bx+6>bx && s_bk_bx<bx+cw && s_bk_by+6>by && s_bk_by<by+ch){
            s_bk_brick[r*BKW+c]=0; s_bk_score+=5; s_bk_bvy=-s_bk_bvy; break;
        }
    }
}
static bool bk_poll(menu_state_t *st){ (void)st;
    if (now_ms()-s_bk_last<20) return false;
    s_bk_last=now_ms(); bk_step(); return true;
}
static void bk_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_bk_over){ if (a==MENU_ACTION_CONFIRM) bk_init(); return; }
    switch(a){
        case MENU_ACTION_LEFT:  if (s_bk_px>4) s_bk_px-=10; break;
        case MENU_ACTION_RIGHT: if (s_bk_px<SW-60) s_bk_px+=10; break;
        default: break;
    }
}
static void bk_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    int cw=38,ch=14,x0=(SW-BKW*cw)/2,y0=16; char b[24];
    for (int r=0;r<BKH;r++)for(int c=0;c<BKW;c++)
        if (s_bk_brick[r*BKW+c]) filr(l,x0+c*cw,y0+r*ch,x0+c*cw+cw-1,y0+r*ch+ch-1,1);
    filr(l,(int)s_bk_px,272,(int)s_bk_px+56,279,1);
    filr(l,(int)s_bk_bx,(int)s_bk_by,(int)s_bk_bx+6,(int)s_bk_by+6,1);
    snprintf(b,24,"SCORE %d",s_bk_score);
    txt(l,8,SH-16,b);
    if (s_bk_over) txt(l,8,3,"MISS [OK]RETRY");
}

/* ================= 21 点 BLACKJACK ================= */
#define BJ_MX 12
static int s_bj_deck[52], s_bj_dc;
static int s_bj_ph[BJ_MX], s_bj_pn, s_bj_dh[BJ_MX], s_bj_dn;
static int s_bj_menu, s_bj_over, s_bj_cash;
static int bj_val(int c){ int r=c%13+1; return (r==1)?11:(r>10?10:r); }
static int bj_sum(int *h,int n,int *ace){ int s=0,a=0;
    for (int i=0;i<n;i++){ int v=bj_val(h[i]); s+=v; if (v==11) a++; }
    while (s>21&&a){ s-=10; a--; }
    *ace=(a>0)?1:0; return s;
}
static int bj_draw(void){ return (s_bj_dc<52)? s_bj_deck[s_bj_dc++] : 0; }
static void bj_deal(void){
    s_bj_pn=s_bj_dn=0;
    s_bj_ph[s_bj_pn++]=bj_draw(); s_bj_dh[s_bj_dn++]=bj_draw();
    s_bj_ph[s_bj_pn++]=bj_draw(); s_bj_dh[s_bj_dn++]=bj_draw();
}
static void bj_init(void){
    srand(now_ms());
    for (int i=0;i<52;i++) s_bj_deck[i]=i;
    for (int i=51;i>0;i--){ int j=rand()%(i+1); int t=s_bj_deck[i]; s_bj_deck[i]=s_bj_deck[j]; s_bj_deck[j]=t; }
    s_bj_dc=0; s_bj_menu=0; s_bj_over=0; s_bj_cash=100; bj_deal();
}
static char bj_rank(int c){ int r=c%13+1; return (r==1)?'A':(r<=10?('0'+r):(r==11?'J':(r==12?'Q':'K'))); }
static void bj_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[32]; char rb[2];
    snprintf(b,32,"CASH %d", s_bj_cash); txt(l,8,6,b);
    txt(l,8,24,"DEALER:");
    for (int i=0;i<s_bj_dn;i++){ filr(l,60+i*24,24,60+i*24+20,38,1); rb[0]=bj_rank(s_bj_dh[i]); rb[1]=0; txt(l,64+i*24,28,rb); }
    if (!s_bj_over && s_bj_dn>1) txt(l,60+24,28,"?");
    txt(l,8,52,"PLAYER:"); int pa=0;
    for (int i=0;i<s_bj_pn;i++){ filr(l,60+i*24,52,60+i*24+20,66,1); rb[0]=bj_rank(s_bj_ph[i]); rb[1]=0; txt(l,64+i*24,56,rb); }
    pa=bj_sum(s_bj_ph,s_bj_pn,&(int){0});
    snprintf(b,32,"SUM %d", pa); txt(l,8,82,b);
    if (s_bj_over){
        int p=bj_sum(s_bj_ph,s_bj_pn,&(int){0}), d=bj_sum(s_bj_dh,s_bj_dn,&(int){0});
        snprintf(b,32,"D %d  P %d", d,p); txt(l,8,104,b);
        txt(l,8,130,(p>21||(d<=21&&d>=p))?"LOSE  [OK]AGAIN":(p==21?"BLACKJACK [OK]AGAIN":"WIN  [OK]AGAIN"));
    } else {
        txt(l,8,130,(s_bj_menu==0)?"> HIT   STAND":"  HIT > STAND");
        txt(l,8,SH-16,"UP/DN SELECT  OK DO");
    }
}
static void bj_exec(void){
    if (s_bj_menu==0){ s_bj_ph[s_bj_pn++]=bj_draw(); }
    else { int da=0; while (bj_sum(s_bj_dh,s_bj_dn,&da)<17 && s_bj_dn<BJ_MX) s_bj_dh[s_bj_dn++]=bj_draw(); s_bj_over=1; }
    int p=bj_sum(s_bj_ph,s_bj_pn,&(int){0}), d=bj_sum(s_bj_dh,s_bj_dn,&(int){0});
    if (p>21){ s_bj_over=1; s_bj_cash-=10; }
    else if (s_bj_menu==0 && p==21){ int da=0; while (bj_sum(s_bj_dh,s_bj_dn,&da)<17 && s_bj_dn<BJ_MX) s_bj_dh[s_bj_dn++]=bj_draw(); s_bj_over=1; }
    if (s_bj_over && p<=21 && (p>d||d>21)) s_bj_cash+=15;
}
static void bj_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_bj_over){ if (a==MENU_ACTION_CONFIRM) bj_init(); return; }
    switch(a){
        case MENU_ACTION_UP:   s_bj_menu=0; break;
        case MENU_ACTION_DOWN: s_bj_menu=1; break;
        case MENU_ACTION_CONFIRM: bj_exec(); break;
        default: break;
    }
}

/* ================= 钓鱼 FISHING ================= */
static int s_fs_x, s_fs_dir, s_fs_score, s_fs_cd, s_fs_hooked;
static uint32_t s_fs_last;
static void fs_init(void){ srand(now_ms()); s_fs_x=200; s_fs_dir=1; s_fs_score=0; s_fs_hooked=0; s_fs_cd=0; s_fs_last=now_ms(); }
static bool fs_poll(menu_state_t *st){ (void)st;
    if (now_ms()-s_fs_last<28) return false;
    s_fs_last=now_ms();
    s_fs_x+=s_fs_dir*(6+(rand()%4));
    if (s_fs_x<20){ s_fs_x=20; s_fs_dir=1; }
    if (s_fs_x>SW-20){ s_fs_x=SW-20; s_fs_dir=-1; }
    if (s_fs_hooked){ s_fs_cd--; if (s_fs_cd<=0) s_fs_hooked=0; }
    return true;
}
static void fs_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (a!=MENU_ACTION_CONFIRM || s_fs_hooked) return;
    if (abs(s_fs_x-SW/2)<30){ s_fs_score++; s_fs_hooked=1; s_fs_cd=12; }
    else if (s_fs_score>0) s_fs_score--;
}
static void fs_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[32];
    filr(l,0,140,SW,141,1); filr(l,0,60,SW,61,1);
    /* hook + pole at center */
    filr(l,SW/2-1,80,SW/2+1,100,1); filr(l,SW/2-9,98,SW/2+9,100,1);
    /* fish */
    filr(l,s_fs_x-8,156,s_fs_x+8,172,1);
    filr(l,s_fs_x-8,156,s_fs_x+8,156,0);
    snprintf(b,32,"CATCH %d", s_fs_score); txt(l,8,8,b);
    if (s_fs_hooked) txt(l,8,100,"GOT IT!");
    txt(l,8,SH-16,"HIT OK WHEN FISH ALIGNED");
}

/* ================= 时钟闹钟 ALARMCLK ================= */
static long s_al_base; static int s_al_sel, s_al_h, s_al_m, s_al_arm, s_al_ring;
static int al_secs(void){ return (int)(((now_ms()-(uint32_t)s_al_base))/1000)%86400; }
static void al_init(void){ s_al_base=(long)now_ms(); s_al_sel=0; s_al_h=6; s_al_m=30; s_al_arm=0; s_al_ring=0; }
static bool al_poll(menu_state_t *st){ (void)st;
    int t=al_secs(), h=t/3600, m=(t/60)%60;
    if (s_al_arm && h==s_al_h && m==s_al_m) s_al_ring=1;
    return false;
}
static void al_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[16];
    int t=al_secs(), h=t/3600, m=(t/60)%60, s=t%60;
    snprintf(b,16,"%02d:%02d:%02d", h,m,s); txt(l,(SW-txtw(b))/2,40,b);
    txt(l,(SW-txtw("SET ALARM"))/2,12,"SET ALARM");
    snprintf(b,16,"ALARM %02d:%02d", s_al_h, s_al_m);
    if (s_al_sel==0){ filr(l,0,68,80,86,1); txt(l,10,72,b); } else txt(l,68,72,b);
    snprintf(b,16,"%02d:%02d", s_al_h, s_al_m); txt(l,44,120,b);
    txt(l,8,SH-16,s_al_arm?(s_al_ring?"RING!!ARMED":"ARMED  [OK]CANCEL"):"NOT SET  [OK]ARM");
}
static void al_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_LEFT: if (s_al_sel>0) s_al_sel--; break;
        case MENU_ACTION_RIGHT: if (s_al_sel<1) s_al_sel++; break;
        case MENU_ACTION_UP:
            if (s_al_sel==0){ if (++s_al_h>23) s_al_h=0; } else { if (++s_al_m>59) s_al_m=0; } s_al_ring=0; break;
        case MENU_ACTION_DOWN:
            if (s_al_sel==0){ if (--s_al_h<0) s_al_h=23; } else { if (--s_al_m<0) s_al_m=59; } s_al_ring=0; break;
        case MENU_ACTION_CONFIRM: s_al_arm=!s_al_arm; if (!s_al_arm) s_al_ring=0; break;
        default: break;
    }
}

/* ================= 猜单词 HANGMAN ================= */
static const char *s_hm_words[]={ "BBK","ESP32","GAMEB","ARDUINO","TANK","RADIO","TFT","PIXEL","RETRO","MESH","EMU","BEEP" };
#define HM_WMAX 16
static int s_hm_widx; static char s_hm_word[HM_WMAX]; static int s_hm_len;
static char s_hm_guess[32]; static int s_hm_try, s_hm_cell, s_hm_over, s_hm_won;
static void hm_init(void){ srand(now_ms()); s_hm_widx=rand()%((int)(sizeof(s_hm_words)/sizeof(s_hm_words[0]))); strcpy(s_hm_word,s_hm_words[s_hm_widx]); s_hm_len=(int)strlen(s_hm_word); memset(s_hm_guess,0,32); s_hm_try=6; s_hm_cell=0; s_hm_over=0; s_hm_won=0; }
static int hm_revealed(void){ for (int i=0;i<s_hm_len;i++) if (!s_hm_guess[(int)(s_hm_word[i]-'A')]) return 0; return 1; }
static void hm_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[24];
    /* gallows */
    filr(l,10,120,30,122,1); filr(l,20,60,22,120,1); filr(l,20,60,90,62,1); filr(l,90,60,92,92,1);
    if (s_hm_try<6) filr(l,74,92,106,94,1);          /* head */
    if (s_hm_try<5) filr(l,88,94,90,130,1);          /* body */
    if (s_hm_try<4) filr(l,74,120,88,122,1);         /* arm L */
    if (s_hm_try<3) filr(l,88,120,102,122,1);        /* arm R */
    if (s_hm_try<2) filr(l,74,142,88,144,1);         /* leg L */
    if (s_hm_try<1) filr(l,88,142,102,144,1);        /* leg R */
    /* word cells */
    int cw=26; for (int i=0;i<s_hm_len;i++){
        int xx=140+i*cw, yy=120;
        outline(l,xx,yy,xx+cw-1,yy+cw-1);
        if (s_hm_guess[(int)(s_hm_word[i]-'A')]){ b[0]=s_hm_word[i]; b[1]=0; txt(l,xx+(cw-10)/2,yy+(cw-14)/2,b); }
    }
    /* letter grid A-Z 7x4 */
    for (int i=0;i<26;i++){
        int gx=140+(i%7)*38, gy=180+(i/7)*26;
        if (i==s_hm_cell) filr(l,gx,gy,gx+36,gy+24,1);
        else outline(l,gx,gy,gx+36,gy+24);
        if (!s_hm_guess['A'+i]){ b[0]='A'+i; b[1]=0; txt(l,gx+14,gy+5,b); }
    }
    snprintf(b,24,"TRY %d [OK]GUESS", s_hm_try);
    txt(l,8,SH-16,b);
    if (s_hm_over||s_hm_won) txt(l,8,SH-34,s_hm_won?"YOU WIN [OK]AGAIN":"HANGED [OK]AGAIN");
}
static void hm_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_hm_over||s_hm_won){ if (a==MENU_ACTION_CONFIRM) hm_init(); return; }
    switch(a){
        case MENU_ACTION_UP:   if (s_hm_cell>=7) s_hm_cell-=7; break;
        case MENU_ACTION_DOWN: if (s_hm_cell<19) s_hm_cell+=7; break;
        case MENU_ACTION_LEFT: if (s_hm_cell%7>0) s_hm_cell--; break;
        case MENU_ACTION_RIGHT: if (s_hm_cell%7<6 && s_hm_cell<25) s_hm_cell++; break;
        case MENU_ACTION_CONFIRM: {
            char let='A'+s_hm_cell;
            if (s_hm_guess[(int)(let-'A')]) break;
            s_hm_guess[(int)(let-'A')]=1;
            int found=0; for (int i=0;i<s_hm_len;i++) if (s_hm_word[i]==let){found=1;break;}
            if (!found) s_hm_try--;
            if (hm_revealed()) s_hm_won=1;
            if (s_hm_try<=0) s_hm_over=1;
        } break;
        default: break;
    }
}

/* ================= 海底收集 DIVER ================= */
#define DV_OBJS 6
static int s_dv_x, s_dv_y, s_dv_life, s_dv_score, s_dv_over;
static int s_dv_ox[DV_OBJS], s_dv_oy[DV_OBJS], s_dv_ok[DV_OBJS];
static uint32_t s_dv_last;
static void dv_spawn(int i){ s_dv_ox[i]=rand()%360; s_dv_oy[i]=-(rand()%80); s_dv_ok[i]=(rand()%4==0)?0:1; }
static void dv_init(void){ srand(now_ms()); s_dv_x=SW/2; s_dv_y=250; s_dv_life=3; s_dv_score=0; s_dv_over=0; for (int i=0;i<DV_OBJS;i++) dv_spawn(i); s_dv_last=now_ms(); }
static bool dv_poll(menu_state_t *st){ (void)st;
    if (s_dv_over) return false;
    if (now_ms()-s_dv_last<60) return false;
    s_dv_last=now_ms();
    for (int i=0;i<DV_OBJS;i++){
        s_dv_oy[i]+=2;
        if (s_dv_oy[i]>SH+10){ dv_spawn(i); continue; }
        if (s_dv_ox[i]<s_dv_x+12 && s_dv_ox[i]+12>s_dv_x && s_dv_oy[i]<s_dv_y+12 && s_dv_oy[i]+12>s_dv_y){
            if (s_dv_ok[i]) s_dv_score++; else { s_dv_life--; if (s_dv_life<=0) s_dv_over=1; }
            dv_spawn(i);
        }
    }
    return true;
}
static void dv_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_dv_over){ if (a==MENU_ACTION_CONFIRM) dv_init(); return; }
    switch(a){
        case MENU_ACTION_UP:   if (s_dv_y>20)  s_dv_y-=6; break;
        case MENU_ACTION_DOWN: if (s_dv_y<270) s_dv_y+=6; break;
        case MENU_ACTION_LEFT: if (s_dv_x>12)  s_dv_x-=6; break;
        case MENU_ACTION_RIGHT:if (s_dv_x<SW-22) s_dv_x+=6; break;
        default: break;
    }
}
static void dv_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[24];
    for (int i=0;i<DV_OBJS;i++){
        if (s_dv_ok[i]) outline(l,s_dv_ox[i],s_dv_oy[i],s_dv_ox[i]+12,s_dv_oy[i]+12);
        else filr(l,s_dv_ox[i],s_dv_oy[i],s_dv_ox[i]+12,s_dv_oy[i]+12,1);
    }
    filr(l,s_dv_x,s_dv_y,s_dv_x+12,s_dv_y+12,1);
    snprintf(b,24,"SCORE %d  LIFE %d", s_dv_score, s_dv_life);
    txt(l,8,SH-16,b);
    if (s_dv_over) txt(l,8,3,"GAME OVER [OK]RETRY");
    else txt(l,8,3,"COLLECT BOX AVOID DARK");
}

/* ================= 进制转换 BINARY ================= */
static int s_bn_v;
static void bn_init(void){ s_bn_v=0; }
static void bn_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[40]; int v=s_bn_v;
    txt(l,64,20,"DEC");
    snprintf(b,40,"%d", v); txt(l,(SW-txtw(b))/2,40,b);
    txt(l,64,96,"BIN"); b[0]=0;
    for (int i=7;i>=0;i--) { char c='0'+((v>>i)&1); char t[2]={c,0}; strcat(b,t); }
    txt(l,(SW-txtw(b))/2,116,b);
    txt(l,64,172,"HEX"); snprintf(b,40,"%X", v); txt(l,(SW-txtw(b))/2,192,b);
    txt(l,8,SH-16,"UP/DOWN 0-255 :CONV");
}
static void bn_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (a==MENU_ACTION_UP && s_bn_v<255) s_bn_v++;
    else if (a==MENU_ACTION_DOWN && s_bn_v>0) s_bn_v--;
}

/* ================= 反应测试 REACT ================= */
static int s_rr_phase; static uint32_t s_rr_arm, s_rr_result;
static void rr_init(void){ srand(now_ms()); s_rr_phase=0; s_rr_result=0; }
static bool rr_poll(menu_state_t *st){ (void)st;
    if (s_rr_phase==1 && now_ms()>=s_rr_arm){ s_rr_phase=2; return true; }
    return false;
}
static void rr_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[24];
    if (s_rr_phase==0){ txt(l,(SW-txtw("REACTION TEST"))/2,80,"REACTION TEST"); txt(l,(SW-txtw("PRESS OK TO START"))/2,140,"PRESS OK TO START"); }
    else if (s_rr_phase==1){ filr(l,0,0,SW-1,SH-1,1); txt(l,(SW-txtw("GET READY..."))/2,80,"GET READY..."); txt(l,8,SH-16,"BACK TO EXIT"); }
    else if (s_rr_phase==2){ filr(l,0,0,SW-1,SH-1,1); txt(l,(SW-txtw("GO!!! PRESS NOW!"))/2,80,"GO!!! PRESS NOW!"); }
    else { snprintf(b,24,"%d ms", (int)s_rr_result); txt(l,(SW-txtw(b))/2,80,b); txt(l,(SW-txtw("PRESS OK AGAIN"))/2,140,"PRESS OK AGAIN"); }
}
static void rr_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (a!=MENU_ACTION_CONFIRM) return;
    if (s_rr_phase==0){ s_rr_phase=1; s_rr_arm=now_ms()+250+(rand()%1500); }
    else if (s_rr_phase==2){ s_rr_result=now_ms()-s_rr_arm; s_rr_phase=3; }
    else if (s_rr_phase==3){ s_rr_phase=0; s_rr_result=0; }
}

/* ================= 石头剪刀布 MORA ================= */
static int s_mr_sel, s_mr_cpu, s_mr_res, s_mr_played;
static const char *s_mr_opts[3]={"ROCK","PAPER","SCIS"};
static void mr_init(void){ srand(now_ms()); s_mr_sel=0; s_mr_played=0; s_mr_res=0; }
static void mr_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[40];
    txt(l,(SW-txtw("ROCK PAPER SCISSOR"))/2,12,"ROCK PAPER SCISSOR");
    for (int i=0;i<3;i++){
        int cx=40+i*110;
        if (i==s_mr_sel) filr(l,cx,70,cx+100,96,1);
        else outline(l,cx,70,cx+100,96);
        txt(l,cx+(100-txtw(s_mr_opts[i]))/2,78,s_mr_opts[i]);
    }
    if (s_mr_played){
        snprintf(b,40,"YOU %s   CPU %s", s_mr_opts[s_mr_sel], s_mr_opts[s_mr_cpu]);
        txt(l,(SW-txtw(b))/2,150,b);
        snprintf(b,40,"%s", s_mr_res==0?"DRAW":(s_mr_res==1?"YOU WIN!":"YOU LOSE"));
        txt(l,(SW-txtw(b))/2,190,b);
    }
    txt(l,8,SH-16,s_mr_played?"ARROW CHOSE  OK PLAY  OK AGAIN":"ARROW CHOSE  OK PLAY");
}
static void mr_action(menu_state_t *st, menu_action_t a){
    (void)st;
    switch(a){
        case MENU_ACTION_LEFT: if (s_mr_sel>0) s_mr_sel--; break;
        case MENU_ACTION_RIGHT: if (s_mr_sel<2) s_mr_sel++; break;
        case MENU_ACTION_CONFIRM: {
            s_mr_cpu=rand()%3; s_mr_played=1;
            if (s_mr_sel==s_mr_cpu) s_mr_res=0;
            else if ((s_mr_sel==0&&s_mr_cpu==2)||(s_mr_sel==1&&s_mr_cpu==0)||(s_mr_sel==2&&s_mr_cpu==1)) s_mr_res=1;
            else s_mr_res=2;
        } break;
        default: break;
    }
}

/* ================= 躲避车 RACE ================= */
static int s_rc_x, s_rc_score, s_rc_over;
static int s_rc_ox[3], s_rc_oy[3];
static uint32_t s_rc_last;
static void rc_init(void){ srand(now_ms()); s_rc_x=SW/2; s_rc_score=0; s_rc_over=0;
    for (int i=0;i<3;i++){ s_rc_ox[i]=rand()%372; s_rc_oy[i]=-(i*90+rand()%60); }
    s_rc_last=now_ms();
}
static bool rc_poll(menu_state_t *st){ (void)st;
    if (s_rc_over) return false;
    if (now_ms()-s_rc_last<50) return false;
    s_rc_last=now_ms();
    for (int i=0;i<3;i++){
        s_rc_oy[i]+=3+ (rand()%3);
        if (s_rc_oy[i]>SH){ s_rc_ox[i]=rand()%372; s_rc_oy[i]=-(rand()%80); s_rc_score++; }
        if (s_rc_ox[i]<s_rc_x+22 && s_rc_ox[i]+28>s_rc_x && s_rc_oy[i]<278 && s_rc_oy[i]+16>270) { s_rc_over=1; }
    }
    return true;
}
static void rc_action(menu_state_t *st, menu_action_t a){
    (void)st;
    if (s_rc_over){ if (a==MENU_ACTION_CONFIRM) rc_init(); return; }
    if (a==MENU_ACTION_LEFT && s_rc_x>10) s_rc_x-=10;
    else if (a==MENU_ACTION_RIGHT && s_rc_x<SW-32) s_rc_x+=10;
}
static void rc_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE); char b[24];
    for (int i=0;i<3;i++) filr(l,s_rc_ox[i],s_rc_oy[i],s_rc_ox[i]+28,s_rc_oy[i]+16,1);
    filr(l,s_rc_x,270,s_rc_x+22,286,1);
    snprintf(b,24,"SCORE %d", s_rc_score);
    txt(l,8,SH-16,b);
    if (s_rc_over) txt(l,8,3,"CRASH [OK]RETRY");
}

/* ================= NETSCAN: OpenVAS 式局域网安全扫描 =================
 * 复用 menu 已链接的 esp_wifi / esp_netif / lwip. 引擎放后台 FreeRTOS 任务,
 * 分两阶段: ① ping 扫描本网段存活主机 ② 对存活主机做常见 TCP 端口扫描.
 * 结果: 每台主机已开放端口位图. UI 仅读全局状态, 不阻塞.
 */
#define NS_MAXHOST 24
typedef struct { uint32_t ip; int rtt; uint16_t openmask; } ns_host_t;
#define NS_NPORTS 12
static const struct { uint16_t p; uint16_t bit; const char *n; } s_ns_ports[NS_NPORTS] = {
    { 21, 0x0001, "FTP" },{ 22, 0x0002, "SSH" },{ 23, 0x0004, "TELNET" },
    { 25, 0x0008, "SMTP" },{ 53, 0x0010, "DNS" },{ 80, 0x0020, "HTTP" },
    { 443,0x0040, "HTTPS" },{ 445,0x0080, "SMB" },{ 3389,0x0100, "RDP" },
    { 3306,0x0200, "MYSQL" },{ 8080,0x0400, "HTTPALT" },{ 161,0x0800, "SNMP" },
};
static ns_host_t s_ns_hosts[NS_MAXHOST];
static int  s_ns_nhost = 0;
static bool s_ns_running = false;
static int  s_ns_phase = 0;   /* 0=空闲 1=主机扫描 2=端口扫描 3=完成 4=无网络 */
static int  s_ns_progress = 0;
static int  s_ns_sel = 0;
static int  s_ns_lastprog = -1, s_ns_lastphase = -1;
static uint32_t s_ns_local = 0, s_ns_gw = 0, s_ns_mask = 0;
static int  s_ns_rssi = 0;

static uint16_t ns_icmp_checksum(const void *d, int len){
    const uint16_t *w = (const uint16_t *)d; uint32_t sum = 0; int i;
    for (i = 0; i + 1 < len; i += 2) sum += w[i/2];
    if (len & 1) sum += ((const uint8_t *)d)[len-1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static void ns_get_iface(void){
    s_ns_local = s_ns_gw = s_ns_mask = 0; s_ns_rssi = 0;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(n, &ip) == ESP_OK) {
            s_ns_local = ip.ip.addr; s_ns_gw = ip.gw.addr; s_ns_mask = ip.netmask.addr;
        }
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_ns_rssi = ap.rssi;
}
/* 阶段①: 本网段 ICMP 存活扫描 (单原始 socket, 群发后统一回收) */
static void ns_ping_sweep(void){
    int sd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sd < 0) { s_ns_phase = 4; s_ns_running = false; return; }
    struct timeval tv = { 0, 450000 };
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint32_t net = s_ns_local & s_ns_mask;
    uint32_t hbmask = ~s_ns_mask & 0xFFFFF;
    uint64_t ts[256]; uint8_t sent[256];
    memset(sent, 0, sizeof(sent));
    s_ns_nhost = 0;
    for (int last = 1; last <= 254; last++) {
        if (!s_ns_running) break;
        uint32_t ip = net | ((uint32_t)last & hbmask);
        if ((ip & hbmask) != (uint32_t)last) continue;
        if (ip == s_ns_local || ip == s_ns_gw) continue;
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip);
        unsigned char pkt[40]; memset(pkt, 0, 40);
        pkt[0] = 8; pkt[1] = 0;                 /* ICMP echo */
        pkt[4] = 0xB1; pkt[5] = 0xB1;           /* id */
        pkt[6] = (unsigned char)(last >> 8); pkt[7] = (unsigned char)last;
        for (int k = 8; k < 40; k++) pkt[k] = (unsigned char)(k & 0xFF);
        uint16_t c = ns_icmp_checksum(pkt, 40);
        pkt[2] = (unsigned char)(c >> 8); pkt[3] = (unsigned char)c;
        if (sendto(sd, pkt, 40, 0, (struct sockaddr *)&a, sizeof(a)) > 0) { sent[last] = 1; ts[last] = now_ms(); }
    }
    for (;;) {
        unsigned char buf[64]; struct sockaddr_in from; socklen_t fl = sizeof(from);
        int r = recvfrom(sd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) break;
        if ((buf[0] & 0xff) != 0) continue;     /* 非 echo reply */
        if ((buf[4] << 8 | buf[5]) != 0xB1B1) continue;
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        uint32_t hb = ip & hbmask;
        if (hb < 1 || hb > 254 || !sent[hb]) continue;
        int h = -1; for (int q = 0; q < s_ns_nhost; q++) if (s_ns_hosts[q].ip == ip) { h = q; break; }
        if (h < 0 && s_ns_nhost < NS_MAXHOST) {
            h = s_ns_nhost++; s_ns_hosts[h].ip = ip; s_ns_hosts[h].openmask = 0;
            s_ns_hosts[h].rtt = (int)(now_ms() - ts[hb]);
        }
        if (h >= 0) { int rt = (int)(now_ms() - ts[hb]); if (s_ns_hosts[h].rtt <= 0 || rt < s_ns_hosts[h].rtt) s_ns_hosts[h].rtt = rt; }
    }
    close(sd);
}
/* 阶段②: 对存活主机做常见端口 TCP connect 扫描 (非阻塞+select) */
static void ns_port_scan(void){
    int total = s_ns_nhost > 0 ? s_ns_nhost : 1;
    for (int h = 0; h < s_ns_nhost && s_ns_running; h++) {
        s_ns_progress = h * 100 / total;
        for (int p = 0; p < NS_NPORTS; p++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            int on = 1; fcntl(fd, F_SETFL, O_NONBLOCK);
            struct sockaddr_in a; memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET; a.sin_port = htons(s_ns_ports[p].p);
            a.sin_addr.s_addr = htonl(s_ns_hosts[h].ip);
            int cr = connect(fd, (struct sockaddr *)&a, sizeof(a));
            if (cr == 0) { s_ns_hosts[h].openmask |= s_ns_ports[p].bit; }
            else if (cr < 0) {
                fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
                struct timeval to = { 0, 120000 };
                int sr = select(fd + 1, NULL, &wf, NULL, &to);
                if (sr > 0 && FD_ISSET(fd, &wf)) {
                    int err = 0; socklen_t el = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                    if (err == 0) s_ns_hosts[h].openmask |= s_ns_ports[p].bit;
                }
            }
            close(fd);
        }
    }
    if (s_ns_running) s_ns_progress = 100;
}
static void ns_scan_task(void *arg){
    (void)arg;
    ns_get_iface();
    if (s_ns_local == 0) { s_ns_phase = 4; s_ns_running = false; vTaskDelete(NULL); return; }
    s_ns_phase = 1; s_ns_progress = 5;
    ns_ping_sweep();
    if (!s_ns_running) { s_ns_phase = 4; vTaskDelete(NULL); return; }
    s_ns_phase = 2; s_ns_progress = 0;
    ns_port_scan();
    s_ns_phase = 3; s_ns_progress = 100; s_ns_running = false;
    vTaskDelete(NULL);
}
static void ns_start(void){
    if (s_ns_running) return;
    s_ns_running = true; s_ns_phase = 0; s_ns_progress = 0; s_ns_sel = 0; s_ns_nhost = 0;
    xTaskCreate(ns_scan_task, "netscan", 6144, NULL, 1, NULL);
}
static void ns_init(void){
    s_ns_running = false; s_ns_nhost = 0; s_ns_phase = 0; s_ns_progress = 0;
    s_ns_sel = 0; s_ns_lastprog = -1; s_ns_lastphase = -1;
    ns_get_iface();
}
static bool ns_poll(menu_state_t *st){
    (void)st;
    bool chg = (s_ns_lastprog != s_ns_progress) || (s_ns_lastphase != s_ns_phase);
    s_ns_lastprog = s_ns_progress; s_ns_lastphase = s_ns_phase;
    return chg;
}
static void ns_action(menu_state_t *st, menu_action_t a){
    switch (a) {
        case MENU_ACTION_CONFIRM: ns_start(); break;
        case MENU_ACTION_UP:   if (s_ns_sel > 0) s_ns_sel--; break;
        case MENU_ACTION_DOWN: if (s_ns_sel < s_ns_nhost - 1) s_ns_sel++; break;
        default: break;
    }
    st->needs_redraw = true;
}
static void ns_render(st7305_handle_t *l){
    st7305_clear(l, ST7305_COLOR_WHITE);
    txt(l, 8, 6, "NETSCAN");
    txt(l, SW - 8 - txtw("CONF:SCAN"), 6, "CONF:SCAN");
    char b[48];
    snprintf(b, sizeof(b), "IP %u.%u.%u.%u GW %u.%u.%u.%u",
        (unsigned)(s_ns_local>>0)&255,(unsigned)(s_ns_local>>8)&255,(unsigned)(s_ns_local>>16)&255,(unsigned)(s_ns_local>>24)&255,
        (unsigned)(s_ns_gw>>0)&255,(unsigned)(s_ns_gw>>8)&255,(unsigned)(s_ns_gw>>16)&255,(unsigned)(s_ns_gw>>24)&255);
    txt(l, 8, 28, b);
    snprintf(b, sizeof(b), "RSSI %d dB", s_ns_rssi);
    txt(l, 8, 46, b);
    if (s_ns_phase == 4) { txt(l, 8, 70, "NO NETWORK / NOT CONNECTED"); return; }
    if (s_ns_running && s_ns_phase == 1) { snprintf(b,48,"PING SWEEP %d%%",s_ns_progress); txt(l,8,70,b); return; }
    if (s_ns_running && s_ns_phase == 2) { snprintf(b,48,"PORT SCAN %d%%",s_ns_progress); txt(l,8,70,b); return; }
    if (s_ns_phase == 0) { txt(l, 8, 70, "CONF TO START SWEEP"); return; }
    /* 结果展示 */
    snprintf(b, sizeof(b), "%d HOST(S) UP", s_ns_nhost);
    txt(l, 8, 70, b);
    int y = 90;
    for (int i = 0; i < s_ns_nhost && y < SH - 46; i++) {
        if (i == s_ns_sel) outline(l, 2, y, SW - 2, y + 14);
        snprintf(b, sizeof(b), "%u.%u.%u.%u",(unsigned)(s_ns_hosts[i].ip>>0)&255,(unsigned)(s_ns_hosts[i].ip>>8)&255,(unsigned)(s_ns_hosts[i].ip>>16)&255,(unsigned)(s_ns_hosts[i].ip>>24)&255);
        txt(l, 8, y + 1, b);
        int n = 0; uint16_t m = s_ns_hosts[i].openmask;
        for (int p = 0; p < NS_NPORTS; p++) if (m & s_ns_ports[p].bit) n++;
        snprintf(b, sizeof(b), "%d open", n);
        txt(l, SW - 8 - txtw(b), y + 1, b);
        y += 15;
    }
    if (s_ns_sel >= 0 && s_ns_sel < s_ns_nhost) {
        txt(l, 8, SH - 44, "SEL >>>");
        int cx = 8, yy = SH - 28;
        uint16_t m = s_ns_hosts[s_ns_sel].openmask;
        for (int p = 0; p < NS_NPORTS; p++) if (m & s_ns_ports[p].bit) {
            snprintf(b, sizeof(b), "%s%d", s_ns_ports[p].n, s_ns_ports[p].p);
            if (cx + txtw(b) > SW - 4) { cx = 8; yy += 15; }
            if (yy < SH) txt(l, cx, yy, b);
            cx += txtw(b) + 6;
        }
        if (cx == 8 && yy == SH - 28) txt(l, 8, yy, "no open port scanned");
    }
}

/* ============================================================
 * NETTOOL: 网络工具
 *  全屏左栏导航 (Wi-Fi / 安全 / 测试 / 其他), 无状态栏.
 *  - Wi-Fi 页: esp_wifi 扫描 (含隐藏/无名称), 名称前 200px + 加密锁/信号图标.
 *  - 点某个网络 -> 全屏输密码页 (网络信息 + 密码框 + 52 键键盘, 无触控板).
 *  - 安全/测试/其他: 右侧工具网格 (安全==局域网扫描==复用 NETSCAN).
 *  本应用完全自包含, 只依赖 esp_wifi / wifi_manager.
 * ============================================================ */
#define ND_MAX 26
/* WiFi 扫描记录表放 PSRAM, 不占内部 RAM (~3.9KB) */
EXT_RAM_BSS_ATTR static wifi_ap_record_t  s_nt_ap[ND_MAX];
static int  s_nt_n = 0;
static int  s_nt_nav = 0;        /* 0=Wi-Fi,1=安全,2=测试,3=其他 */
static int  s_nt_sel = 0;        /* 右侧列表/网格焦点 */
static int  s_nt_scroll = 0;
static bool s_nt_scanning = false;
static bool s_nt_scan_done = false;
static char s_nt_msg[56];
static bool s_nt_pass = false;   /* 输密码页是否打开 */
static int  s_nt_apsel = -1;     /* 密码页对应的网络 */
static char s_nt_pw[64];
static int  s_nt_pwlen = 0;
static bool s_nt_shift = false, s_nt_caps = false, s_nt_fn = false;
static int  s_nt_krow = 0, s_nt_kcol = 0;      /* 键盘方向键焦点 */
/* 键盘几何 (渲染/触摸共用): rows x [x,w] */
static short s_nt_kx[5][14], s_nt_kw[5][14];
static uint8_t s_nt_kc[5];

/* 中文绘制 (font_zh 24x24, 压缩 XIP 字库) */
static void txtzh(st7305_handle_t *l, int x, int y, const char *s) {
    while (*s) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            int idx = font_zh_find_utf8(s);
            if (idx >= 0) {
                const uint8_t *bmp = zh_font_data[idx];
                int bpr = (ZH_FONT_W + 7) / 8; /* 3 */
                for (int r = 0; r < 24; r++)
                    for (int cc = 0; cc < 24; cc++)
                        if (bmp[r * bpr + (cc / 8)] & (1 << (7 - (cc % 8))))
                            setp(l, x + cc, y + r);
            }
            s += 3; x += 24;
        } else { /* ASCII 走小字 */
            int gi = glyph_idx(*s);
            const uint8_t *g = (gi >= 0) ? gfont[gi] : NULL;
            if (g) {
                for (int cx = 0; cx < 5; cx++)
                    for (int cy = 0; cy < 7; cy++)
                        if (g[cx] & (1 << cy)) setp(l, x + cx * 2, y + cy * 2);
            }
            s += 1; x += 12;
        }
    }
}
static int txtzh_w(const char *s) {
    int w = 0;
    while (*s) { w += ((unsigned char)*s >= 0x80) ? 24 : 12; s++; }
    return w;
}

/* ---- 左栏小图标 (1-bit 像素画, 40 高内) ---- */
static void ntic_wifi(st7305_handle_t *l, int cx, int cy) {
    setp(l, cx, cy + 8); /* 中心圆点(天线) */
    for (int rad = 5; rad <= 13; rad += 4) {
        for (int a = -95; a <= 95; a += 3) {
            double r = a * 3.14159 / 180.0;
            int x = (int)(rad * 0.8 * cos(r) + 0.5);
            int y = (int)(rad * sin(r) + 0.5);
            setp(l, cx + x, cy + 8 - y);
        }
    }
}
static void ntic_shield(st7305_handle_t *l, int cx, int cy) {
    int y = cy - 12;
    for (; y <= cy - 8; y++) { /* 顶部 */
        int dx = (y == cy - 12) ? 3 : 0;
        for (int x = cx - 6 + dx; x <= cx + 6 - dx; x++) setp(l, x, y);
    }
    for (int x = cx - 6; x <= cx + 6; x++) setp(l, x, cy + 6); /* 肩线 */
    for (int k = 0; k <= 5; k++) { setp(l, cx - 8 + k, cy - 8 + k); setp(l, cx + 8 - k, cy - 8 + k); }
    for (int k = 0; k <= 4; k++) { for (int x = cx - 5 + k; x <= cx + 5 - k; x++) setp(l, x, cy + 7 + k); }
    for (int x = cx - 3; x <= cx + 3; x++) setp(l, x, cy + 12); /* 底尖 */
    /* 内部对勾 */
    setp(l, cx - 3, cy - 3); setp(l, cx - 2, cy - 4);
    for (int k = 0; k < 4; k++) { setp(l, cx - 2 + k, cy - 3 + k); }
    for (int k = 0; k <= 4; k++) { setp(l, cx + 2 + k, cy + 1 + k); setp(l, cx + 1 + k, cy + 1 + k); }
}
static void ntic_pulse(st7305_handle_t *l, int cx, int cy) {
    /* 心电图/诊断 */
    for (int x = cx - 11; x <= cx + 11; x++) setp(l, x, cy + 8);
    setp(l, cx - 8, cy + 8); setp(l, cx - 7, cy + 7); setp(l, cx - 6, cy + 6);
    for (int k = 0; k < 3; k++) setp(l, cx - 6 + k, cy + 6 - k);
    setp(l, cx - 3, cy + 4); setp(l, cx - 3, cy + 3); setp(l, cx - 3, cy + 2);
    setp(l, cx - 2, cy + 1); for (int k = 0; k < 4; k++) setp(l, cx + k, cy + 1 - k);
    setp(l, cx + 4, cy - 3); setp(l, cx + 4, cy - 2); setp(l, cx + 5, cy - 1); setp(l, cx + 6, cy);
    setp(l, cx + 7, cy + 1); setp(l, cx + 8, cy + 4); setp(l, cx + 8, cy + 5); setp(l, cx + 8, cy + 6);
    for (int x = cx + 8; x <= cx + 11; x++) setp(l, x, cy + 8);
}
static void ntic_sliders(st7305_handle_t *l, int cx, int cy) {
    for (int k = 0; k < 3; k++) {
        int xs = cx - 7 + k * 7;
        for (int y = cy - 9; y <= cy + 9; y++) setp(l, xs, y);
        int hn = cy - 9 + ((k == 0) ? 4 : (k == 1) ? 10 : 2);
        for (int x = xs - 2; x <= xs + 2; x++) setp(l, x, hn);
    }
}

static const char *s_nt_nav_zh[4] = {
    NULL,                              /* 0=Wi-Fi (ASCII) */
    "\xe5\xae\x89\xe5\x85\xa8",        /* 安全 */
    "\xe6\xb5\x8b\xe8\xaf\x95",        /* 测试 */
    "\xe5\x85\xb6\xe4\xbb\x96",        /* 其他 */
};

/* ---- 右侧列表辅助图标 ---- */
static void nt_bars(st7305_handle_t *l, int x, int y, int rssi) {
    int bars = (rssi >= -50) ? 4 : (rssi >= -60) ? 3 : (rssi >= -70) ? 2 : (rssi >= -80) ? 1 : 0;
    int bw = 5;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 4;
        if (i < bars) filr(l, x, y - h, x + bw, y - 1, 1);
        else { for (int yy = y - h; yy < y; yy++) setp(l, x, yy), setp(l, x + bw, yy); }
        x += bw + 2;
    }
}
static void nt_lock(st7305_handle_t *l, int x, int y) {
    outline(l, x, y, x + 8, y + 4);          /* 锁体 */
    filr(l, x + 8, y + 2, x + 9, y + 2, 1);
    for (int k = 0; k < 2; k++) {            /* 锁环 */
        setp(l, x + 2, y); setp(l, x + 3 - k, y - 2); setp(l, x + 4 - k, y - 3);
        setp(l, x + 5 + k, y - 3); setp(l, x + 6, y - 2); setp(l, x + 7, y);
    }
    setp(l, x + 4, y + 2);                   /* 锁孔 */
}

/* 空 SSID 时显示隐藏标识 + 末两字节 bssid */
static void nt_ap_name(const wifi_ap_record_t *r, char *out, size_t n) {
    if (r->ssid[0]) { snprintf(out, n, "%s", r->ssid); return; }
    snprintf(out, n, "(hidden)..%02x%02x", r->bssid[4], r->bssid[5]);
}
static bool nt_ap_locked(const wifi_ap_record_t *r) {
    return r->authmode != WIFI_AUTH_OPEN;
}
static const char *nt_ap_authstr(const wifi_ap_record_t *r) {
    switch (r->authmode) {
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
        case WIFI_AUTH_OPEN: return "OPEN";
        default: return "?";
    }
}

/* 扫描记录取回 + 信号强度降序 */
static void nt_fetch(void) {
    uint16_t num = ND_MAX;
    s_nt_n = 0;
    if (esp_wifi_scan_get_ap_records(&num, s_nt_ap) == ESP_OK) s_nt_n = num;
    for (int i = 1; i < s_nt_n; i++) {
        wifi_ap_record_t t = s_nt_ap[i]; int j = i - 1;
        while (j >= 0 && s_nt_ap[j].rssi < t.rssi) { s_nt_ap[j + 1] = s_nt_ap[j]; j--; }
        s_nt_ap[j + 1] = t;
    }
}

static void nt_init(void) {
    s_nt_nav = 0; s_nt_sel = 0; s_nt_scroll = 0;
    s_nt_pass = false; s_nt_apsel = -1;
    s_nt_pw[0] = 0; s_nt_pwlen = 0;
    s_nt_scanning = false; s_nt_scan_done = false; s_nt_n = 0;
    s_nt_shift = false; s_nt_caps = false; s_nt_fn = false;
    s_nt_krow = 0; s_nt_kcol = 0;
    s_nt_msg[0] = 0;
    if (!wifi_manager_is_enabled()) wifi_manager_enable();
}

/* 启动扫描 (Wi-Fi 页进入时由 poll 调用以复用 wifi_manager 的 enable/scan) */
static void nt_start_scan(void) {
    if (wifi_manager_scan_start()) { s_nt_scanning = true; }
    else { s_nt_scanning = false; s_nt_scan_done = true; s_nt_n = 0; snprintf(s_nt_msg, sizeof(s_nt_msg), "SCAN FAILED"); }
}
static bool nt_poll(menu_state_t *st) {
    (void)st;
    bool chg = false;
    if (!s_nt_pass && s_nt_nav == 0) { /* 仅 Wi-Fi 列表页轮询扫描 */
        if (!s_nt_scanning && !s_nt_scan_done) {
            nt_start_scan();
            chg = true;
        } else if (s_nt_scanning) {
            if (wifi_manager_is_scan_done()) {
                s_nt_scanning = false; s_nt_scan_done = true;
                nt_fetch();
                snprintf(s_nt_msg, sizeof(s_nt_msg), "%d NETWORKS", s_nt_n);
                chg = true;
            } else if (!s_nt_scan_done) { chg = true; }
        }
    }
    return chg;
}

/* 连接某网络 */
static void nt_connect(int idx) {
    if (idx < 0 || idx >= s_nt_n) return;
    wifi_ap_record_t *r = &s_nt_ap[idx];
    if (nt_ap_locked(r)) {
        s_nt_apsel = idx; s_nt_pass = true;
        s_nt_pw[0] = 0; s_nt_pwlen = 0; s_nt_krow = 0; s_nt_kcol = 0;
        s_nt_shift = s_nt_caps = s_nt_fn = false;
    } else {
        wifi_manager_connect((const char *)r->ssid, "");
        snprintf(s_nt_msg, sizeof(s_nt_msg), "CONNECTING..");
    }
}
static void nt_do_connect(void) {
    if (s_nt_apsel < 0 || s_nt_apsel >= s_nt_n) { s_nt_pass = false; return; }
    wifi_ap_record_t *r = &s_nt_ap[s_nt_apsel];
    const char *ssid = (const char *)r->ssid;
    if (ssid[0] == '\0') {
        snprintf(s_nt_msg, sizeof(s_nt_msg), "HIDDEN SSID NEED SSID");
    } else {
        wifi_manager_connect(ssid, s_nt_pw);
        snprintf(s_nt_msg, sizeof(s_nt_msg), "CONNECTING..");
    }
    s_nt_pass = false;
}

/* ================= NETTOOL 键盘 (52 键, 无触控板) ================= */
enum { NTK_CH = 1, NTK_BS, NTK_ENT, NTK_SP, NTK_CAPS, NTK_SHIFT, NTK_FN, NTK_IGN };
typedef struct { const char *lab; uint8_t kind; char c; char sym; uint8_t w; } ntkey_t;
static const ntkey_t ntk_row0[] = {
    {"Esc",NTK_IGN,0,0,28},
    {"1",NTK_CH,'1','!',31},{"2",NTK_CH,'2','@',31},{"3",NTK_CH,'3','#',31},
    {"4",NTK_CH,'4','$',31},{"5",NTK_CH,'5','%',31},{"6",NTK_CH,'6','^',31},
    {"7",NTK_CH,'7','&',31},{"8",NTK_CH,'8','*',31},{"9",NTK_CH,'9','(',31},
    {"0",NTK_CH,'0',')',31},{"-",NTK_CH,'-','_',31},{"+",NTK_CH,'+','=',31},
};
static const ntkey_t ntk_row1[] = {
    {"Tab",NTK_IGN,0,0,34},
    {"Q",NTK_CH,'q',0,33},{"W",NTK_CH,'w',0,33},{"E",NTK_CH,'e',0,33},
    {"R",NTK_CH,'r',0,33},{"T",NTK_CH,'t',0,33},{"Y",NTK_CH,'y',0,33},
    {"U",NTK_CH,'u',0,33},{"I",NTK_CH,'i',0,33},{"O",NTK_CH,'o',0,33},
    {"P",NTK_CH,'p',0,33},{"DEL",NTK_BS,0,0,36},
};
static const ntkey_t ntk_row2[] = {
    {"CA",NTK_CAPS,0,0,60},
    {"A",NTK_CH,'a',0,32},{"S",NTK_CH,'s',0,32},{"D",NTK_CH,'d',0,32},
    {"F",NTK_CH,'f',0,32},{"G",NTK_CH,'g',0,32},{"H",NTK_CH,'h',0,32},
    {"J",NTK_CH,'j',0,32},{"K",NTK_CH,'k',0,32},{"L",NTK_CH,'l',0,32},
    {"OK",NTK_ENT,0,0,52},
};
static const ntkey_t ntk_row3[] = {
    {"Sh",NTK_SHIFT,0,0,70},
    {"Z",NTK_CH,'z',0,40},{"X",NTK_CH,'x',0,40},{"C",NTK_CH,'c',0,40},
    {"V",NTK_CH,'v',0,40},{"B",NTK_CH,'b',0,40},{"N",NTK_CH,'n',0,40},
    {"M",NTK_CH,'m',0,40},{NULL,NTK_IGN,0,0,50},
};
static const ntkey_t ntk_row4[] = {
    {"FN",NTK_FN,0,0,50},{"Ctl",NTK_IGN,0,0,50},{"Alt",NTK_IGN,0,0,50},
    {"SPC",NTK_SP,0,0,100},{NULL,NTK_IGN,0,0,50},
    {NULL,NTK_IGN,0,0,50},{NULL,NTK_IGN,0,0,50},
};
/* 键盘行数组 + 每行键数 */
static const ntkey_t *ntk_rows[5] = { ntk_row0, ntk_row1, ntk_row2, ntk_row3, ntk_row4 };
static const int ntk_nkeys[5] = { 14, 12, 11, 9, 7 };
#define NTKB_TOP  58
#define NTKB_HROW 46

/* 布局: 逐行从左到右铺开 (与 render 一致), 填充全局几何 */
static void nt_kb_layout(void) {
    for (int r = 0; r < 5; r++) {
        int x = 0;
        for (int c = 0; c < ntk_nkeys[r]; c++) {
            s_nt_kx[r][c] = x;
            s_nt_kw[r][c] = ntk_rows[r][c].w;
            s_nt_kc[r] = c;
            x += ntk_rows[r][c].w;
        }
    }
}
static void nt_kb_press(int r, int c) {
    const ntkey_t *k = &ntk_rows[r][c];
    char out = 0;
    switch (k->kind) {
        case NTK_CH:
            if (s_nt_fn && k->sym) out = k->sym;
            else out = ((s_nt_shift || s_nt_caps) && (k->c >= 'a' && k->c <= 'z')) ? (k->c - 32) : k->c;
            break;
        case NTK_BS:
            if (s_nt_pwlen > 0) s_nt_pw[--s_nt_pwlen] = 0;
            return;
        case NTK_SP:
            out = ' ';
            break;
        case NTK_ENT:
            nt_do_connect();
            return;
        case NTK_CAPS: s_nt_caps = !s_nt_caps; return;
        case NTK_SHIFT: s_nt_shift = !s_nt_shift; return;
        case NTK_FN: s_nt_fn = !s_nt_fn; return;
        default: return;
    }
    if (out && s_nt_pwlen < (int)sizeof(s_nt_pw) - 1) { s_nt_pw[s_nt_pwlen++] = out; s_nt_pw[s_nt_pwlen] = 0; }
}

/* 绘制单个键 (含焦点/激活高亮). x0,y0=box 左下 */
static void nt_kb_key(st7305_handle_t *l, int x0, int y0, int w, int h,
                      const char *lab, bool active, uint8_t kind) {
    outline(l, x0, y0, x0 + w - 1, y0 + h - 1);
    if (active) filr(l, x0 + 1, y0 + 1, x0 + w - 2, y0 + h - 2, 1); /* 反白 */
    /* 方向键 (lab 为 NULL) 画小三角 */
    if (lab == NULL) {
        int cx = x0 + w / 2, cy = y0 + h / 2;
        int dim = (kind == NTK_IGN) ? 3 : 3;
        /* 用 r 区分方向: 这里仅画右侧三角占位 → 由 nt_kb_render 细分 */
        for (int i = 0; i < dim; i++) {
            for (int j = 0; j <= i; j++) {
                int px, py;
                px = cx - dim / 2 + i; py = cy + j;
                setp(l, px, py); px = cx - dim / 2 + i; py = cy - j; setp(l, px, py);
            }
        }
        return;
    }
    int tw = txtw(lab);
    int tx = x0 + (w - tw) / 2;
    int ty = y0 + (h - 14) / 2;
    if (active) { /* 反白: 画白字 */
        for (int i = 0; lab[i]; i++) {
            int gi = glyph_idx(lab[i]);
            const uint8_t *g = (gi >= 0) ? gfont[gi] : NULL;
            if (!g) { tx += 12; continue; }
            for (int cx = 0; cx < 5; cx++)
                for (int cy = 0; cy < 7; cy++)
                    if (g[cx] & (1 << cy)) clrp(l, tx + cx * 2, ty + cy * 2);
            tx += 12;
        }
    } else {
        txt(l, tx, ty, lab);
    }
}

/* 键盘区渲染 (从 NTKB_TOP 往下铺 5 行) */
static void nt_kb_render(st7305_handle_t *l) {
    nt_kb_layout();
    for (int r = 0; r < 5; r++) {
        int y0 = NTKB_TOP + r * NTKB_HROW, h = NTKB_HROW;
        for (int c = 0; c < ntk_nkeys[r]; c++) {
            const ntkey_t *k = &ntk_rows[r][c];
            bool active = false;
            if (k->kind == NTK_CAPS) active = s_nt_caps;
            else if (k->kind == NTK_SHIFT) active = s_nt_shift;
            else if (k->kind == NTK_FN) active = s_nt_fn;
            bool fct = (r == s_nt_krow && c == s_nt_kcol); /* 方向键焦点 */
            if (fct) { filr(l, s_nt_kx[r][c], y0, s_nt_kx[r][c] + s_nt_kw[r][c] - 1, y0 + h - 1, 0); }
            nt_kb_key(l, s_nt_kx[r][c], y0, s_nt_kw[r][c], h, k->lab, active, k->kind);
        }
    }
}

/* 键盘触摸命中 */
static bool nt_kb_touch(st7305_handle_t *l, int x, int y) {
    (void)l;
    nt_kb_layout();
    if (y < NTKB_TOP) return false;
    int r = (y - NTKB_TOP) / NTKB_HROW;
    if (r < 0 || r >= 5) return false;
    for (int c = 0; c < ntk_nkeys[r]; c++) {
        int x0 = s_nt_kx[r][c], w = s_nt_kw[r][c];
        if (x >= x0 && x < x0 + w) { s_nt_krow = r; s_nt_kcol = c; nt_kb_press(r, c); return true; }
    }
    return false;
}

/* ---- NETTOOL 渲染 ---- */
#define NT_NAV_W 72
#define NT_NAV_Y0 14
#define NT_NAV_STEP 66

static void nt_draw_nav(st7305_handle_t *l) {
    for (int i = 0; i < 4; i++) {
        int cy = NT_NAV_Y0 + i * NT_NAV_STEP;
        if (i == s_nt_nav) outline(l, 4, cy - 22, NT_NAV_W - 4, cy + 26);
        switch (i) {
            case 0: ntic_wifi(l, NT_NAV_W / 2, cy); break;
            case 1: ntic_shield(l, NT_NAV_W / 2, cy); break;
            case 2: ntic_pulse(l, NT_NAV_W / 2, cy); break;
            case 3: ntic_sliders(l, NT_NAV_W / 2, cy); break;
        }
        /* 标签 */
        const char *lab = s_nt_nav_zh[i];
        if (lab) {
            int lw = txtzh_w(lab);
            txtzh(l, NT_NAV_W / 2 - lw / 2, cy + 16, lab);
        } else {
            txt(l, NT_NAV_W / 2 - txtw("WiFi") / 2, cy + 22, "WiFi");
        }
        if (i != 3) filr(l, 6, cy + 30, NT_NAV_W - 6, cy + 30, 1);
    }
}
static int nt_right_x(void) { return NT_NAV_W + 8; }

/* Wi-Fi 列表渲染 */
static void nt_render_wifi(st7305_handle_t *l) {
    int x0 = nt_right_x();
    if (s_nt_msg[0]) txt(l, x0, 4, s_nt_msg);
    if (!s_nt_scan_done) { txt(l, x0, 26, "SCANNING.."); return; }
    if (s_nt_n == 0) { txt(l, x0, 26, "NO NETWORK FOUND"); return; }
    int y = 28, rh = 22;
    /* 可见范围 */
    int vis = (SH - 28) / rh;
    if (s_nt_sel < s_nt_scroll) s_nt_scroll = s_nt_sel;
    if (s_nt_sel >= s_nt_scroll + vis) s_nt_scroll = s_nt_sel - vis + 1;
    if (s_nt_scroll < 0) s_nt_scroll = 0;
    for (int i = s_nt_scroll; i < s_nt_scroll + vis && i < s_nt_n; i++) {
        if (i == s_nt_sel) outline(l, x0, y, SW - 4, y + rh - 3);
        char nm[48]; nt_ap_name(&s_nt_ap[i], nm, sizeof(nm));
        /* 名称限到 200px (~16 字符) */
        char df[40]; int pos = 0;
        for (int k = 0; k < (int)strlen(nm) && pos < 15; k++) df[pos++] = nm[k];
        df[pos] = 0;
        if ((int)strlen(nm) > pos) { if (pos > 1) pos--; df[pos] = 0; }
        txt(l, x0 + 2, y + 3, df);
        /* 加密锁 + 信号 */
        int iy = y + rh / 2;
        int ix = x0 + 208;
        if (nt_ap_locked(&s_nt_ap[i])) { nt_lock(l, ix, iy - 4); ix += 16; }
        nt_bars(l, ix, iy + 2, s_nt_ap[i].rssi);
        y += rh;
    }
    /* 底部: 选中网络摘要 + 按键提示 */
    char b[64];
    if (s_nt_sel >= 0 && s_nt_sel < s_nt_n) {
        snprintf(b, sizeof(b), "[%d/%d] %s RSSI %d CH %d CONF:CONNECT",
                 s_nt_sel + 1, s_nt_n,
                 nt_ap_authstr(&s_nt_ap[s_nt_sel]), (int)s_nt_ap[s_nt_sel].rssi, (int)s_nt_ap[s_nt_sel].primary);
        txt(l, x0, SH - 18, b);
    }
}

/* 分类工具网格 */
#define NT_TTOOLS 1
static const struct { uint8_t nav; const char *name; uint8_t icon; } nt_tools[NT_TTOOLS] = {
    { 1, "\xe5\xb1\x80\xe5\x9f\x9f\xe7\xbd\x91\xe6\x89\xab\xe6\x8f\x8f", 0 }, /* 局域网扫描 */
};
static void nt_render_cat(st7305_handle_t *l) {
    int x0 = nt_right_x();
    const char *ttl = s_nt_nav_zh[s_nt_nav];
    txtzh(l, x0, 4, ttl);
    int cnt = 0;
    for (int i = 0; i < NT_TTOOLS; i++) if (nt_tools[i].nav == s_nt_nav) cnt++;
    if (cnt == 0) {
        int sw = txtzh_w("\xe6\x9a\x82\xe6\x97\xa0\xe5\xb7\xa5\xe5\x85\xb7"); /* 暂无工具 */
        txtzh(l, x0 + (SW - NT_NAV_W - sw) / 2, 90, "\xe6\x9a\x82\xe6\x97\xa0\xe5\xb7\xa5\xe5\x85\xb7");
        return;
    }
    int cell = 82, gy = 34;
    int i = 0;
    for (int k = 0; k < NT_TTOOLS; k++) {
        if (nt_tools[k].nav != s_nt_nav) continue;
        if (i == s_nt_sel) outline(l, x0 + 8, gy, x0 + 8 + cell - 4, gy + 72);
        /* 工具图标 */
        switch (nt_tools[k].icon) {
            case 0: ntic_shield(l, x0 + 8 + (cell - 4) / 2, gy + 28); break;
            default: ntic_wifi(l, x0 + 8 + (cell - 4) / 2, gy + 28); break;
        }
        int tw = txtzh_w(nt_tools[k].name);
        txtzh(l, x0 + (cell - tw) / 2, gy + 46, nt_tools[k].name);
        i++;
    }
}
static void nt_launch(const char *title); /* 前向声明: 实体在 s_apps 定义之后 */

/* 密码页渲染 */
static void nt_render_pass(st7305_handle_t *l) {
    st7305_clear(l, ST7305_COLOR_WHITE);
    if (s_nt_apsel < 0 || s_nt_apsel >= s_nt_n) { txt(l, 8, 40, "ERR"); return; }
    wifi_ap_record_t *r = &s_nt_ap[s_nt_apsel];
    char lb[40];
    if (r->ssid[0]) snprintf(lb, sizeof(lb), "%s", r->ssid);
    else snprintf(lb, sizeof(lb), "(hidden)..%02x%02x", r->bssid[4], r->bssid[5]);
    /* 顶部: 网络名 + 探测信息一排 */
    int xx = 6;
    /* 名称截断到约 160px */
    char dn[40]; int pos = 0;
    for (int k = 0; k < (int)strlen(lb) && pos < 13; k++) dn[pos++] = lb[k];
    dn[pos] = 0;
    txt(l, xx, 6, dn);
    xx += txtw(dn);
    /* 信息: RSSI / 加密 / CH */
    char info[48];
    snprintf(info, sizeof(info), "%s %ddB CH%d", nt_ap_authstr(r), (int)r->rssi, (int)r->primary);
    if (xx + txtw(info) < SW - 6) { xx += 4; txt(l, xx, 6, info); }
    else txt(l, SW - 6 - txtw(info), 6, info);
    /* 第二排: 密码输入框 */
    outline(l, 6, 30, SW - 6, 50);
    char pw[70]; int i;
    for (i = 0; i < s_nt_pwlen && i < 60; i++) pw[i] = '*';
    pw[i] = 0;
    txt(l, 10, 35, pw);
    /* 光标 (闪烁以当前秒) */
    time_t nowt = time(NULL);
    struct tm *tm_ = localtime(&nowt);
    if ((tm_->tm_sec & 1) == 0) { int cx = 10 + s_nt_pwlen * 12; if (cx < SW - 12) { setp(l, cx, 35); setp(l, cx, 36); setp(l, cx, 37); setp(l, cx, 42); setp(l, cx, 43); setp(l, cx, 44); setp(l, cx, 45); setp(l, cx, 46); setp(l, cx, 47); setp(l, cx, 48); } }
    /* 键盘 (无触控板) */
    nt_kb_render(l);
}

/* NETTOOL 渲染入口 */
static void nt_render(st7305_handle_t *l) {
    st7305_clear(l, ST7305_COLOR_WHITE);   /* 无状态栏 */
    if (s_nt_pass) { nt_render_pass(l); return; }
    nt_draw_nav(l);
    switch (s_nt_nav) {
        case 0: nt_render_wifi(l); break;
        default: nt_render_cat(l); break;
    }
}

/* 返回上级; 返回 true 表示已消费 (密码页->列表) */
static bool nt_back_consumed(void) {
    if (s_nt_pass) { s_nt_pass = false; s_nt_pw[0] = 0; s_nt_pwlen = 0; return true; }
    return false;
}

/* 按键分发 */
static void nt_action(menu_state_t *st, menu_action_t a) {
    if (s_nt_pass) {
        switch (a) {
            case MENU_ACTION_LEFT: if (s_nt_kcol > 0) s_nt_kcol--; break;
            case MENU_ACTION_RIGHT: if (s_nt_kcol < ntk_nkeys[s_nt_krow] - 1) s_nt_kcol++; break;
            case MENU_ACTION_UP: if (s_nt_krow > 0) { s_nt_krow--; if (s_nt_kcol >= ntk_nkeys[s_nt_krow]) s_nt_kcol = ntk_nkeys[s_nt_krow] - 1; } break;
            case MENU_ACTION_DOWN: if (s_nt_krow < 4) { s_nt_krow++; if (s_nt_kcol >= ntk_nkeys[s_nt_krow]) s_nt_kcol = ntk_nkeys[s_nt_krow] - 1; } break;
            case MENU_ACTION_CONFIRM: nt_kb_press(s_nt_krow, s_nt_kcol); break;
            case MENU_ACTION_BACK:
            case MENU_ACTION_HOME: nt_back_consumed(); break;
            default: break;
        }
        st->needs_redraw = true;
        return;
    }
    switch (a) {
        case MENU_ACTION_UP:
            if (s_nt_sel > 0) s_nt_sel--;
            break;
        case MENU_ACTION_DOWN: {
            int mx = (s_nt_nav == 0) ? s_nt_n : 0;
            if (s_nt_nav != 0) for (int i2 = 0; i2 < NT_TTOOLS; i2++) if (nt_tools[i2].nav == s_nt_nav) mx++;
            if (s_nt_sel < mx - 1) s_nt_sel++;
            break;
        }
        case MENU_ACTION_RIGHT:
            if (s_nt_nav < 3) { s_nt_nav++; s_nt_sel = 0; }
            break;
        case MENU_ACTION_LEFT:
            if (s_nt_nav > 0) { s_nt_nav--; s_nt_sel = 0; }
            break;
        case MENU_ACTION_CONFIRM:
            if (s_nt_nav == 0) nt_connect(s_nt_sel);
            else {
                int k = -1, c2 = 0;
                for (int i2 = 0; i2 < NT_TTOOLS; i2++) if (nt_tools[i2].nav == s_nt_nav) { if (c2 == s_nt_sel) { k = i2; break; } c2++; }
                if (k >= 0) nt_launch(nt_tools[k].name);
            }
            break;
        case MENU_ACTION_BACK:
        case MENU_ACTION_HOME:
            /* 网络工具已独立为主菜单页: 返回主菜单(从应用管理进入则回应用管理) */
            st->current_page = (st->module_return_page == MENU_PAGE_MAIN) ? MENU_PAGE_MAIN : st->module_return_page;
            st->module_return_page = MENU_PAGE_MAIN;   /* 一次性 */
            if (st->current_page == MENU_PAGE_MAIN) {
                st->selected_index = st->main_selected_index;
            } else {
                st->selected_index = 0;
            }
            st->scroll_offset = 0;
            break;
        default: break;
    }
    st->needs_redraw = true;
}

/* 触摸 */
static void nt_touch(menu_state_t *st, int x, int y) {
    if (s_nt_pass) {
        if (nt_kb_touch(st->lcd, x, y)) { st->needs_redraw = true; return; }
        /* 除键盘外的点击: 返回列表 */
        nt_back_consumed();
        st->needs_redraw = true;
        return;
    }
    /* 左栏切页 */
    if (x < NT_NAV_W) {
        int idx = (y - NT_NAV_Y0) / NT_NAV_STEP;
        if (idx >= 0 && idx < 4) { s_nt_nav = idx; s_nt_sel = 0; st->needs_redraw = true; }
        return;
    }
    /* Wi-Fi 列表 / 分类网格 */
    if (s_nt_nav == 0) {
        int row = (y - 28) / 22;
        if (row >= 0 && row + s_nt_scroll < s_nt_n) { s_nt_sel = row + s_nt_scroll; nt_connect(s_nt_sel); }
    } else {
        int cnt = 0;
        for (int i2 = 0; i2 < NT_TTOOLS; i2++) if (nt_tools[i2].nav == s_nt_nav) cnt++;
        if (cnt > 0 && y >= 34 && y < 106) {
            s_nt_sel = 0;
            nt_launch("\xe5\xb1\x80\xe5\x9f\x9f\xe7\xbd\x91\xe6\x89\xab\xe6\x8f\x8f");
        }
    }
    st->needs_redraw = true;
}

/* ================= 应用表 ================= */
static const mini_app_t s_apps[] = {
    {"计算器",     calc_init, calc_render, calc_action, NULL, calc_touch},
    {"秒表",       sw_init,   sw_render,   sw_action,   sw_poll, NULL},
    {"倒计时",     cd_init,   cd_render,   cd_action,   cd_poll, NULL},
    {"日历",       cal_init,  cal_render,  cal_action,  NULL, NULL},
    {"白板",       wb_init,   wb_render,   wb_action,   wb_poll, NULL},
    {"骰子",       dice_init, dice_render, dice_action, NULL, NULL},
    {"单位换算",   un_init,   un_render,   un_action,   NULL, un_touch},
    {"井字棋",     tt_init,   tt_render,   tt_action,   NULL, tt_touch},
    {"记忆翻牌",   mm_init,  mm_render,  mm_action,  NULL, mm_touch},
    {"猜数字",     gs_init,   gs_render,   gs_action,   NULL, NULL},
    {"2048",       tw_init,   tw_render,   tw_action,   NULL, NULL},
    {"乒乓球",     pp_init,   pp_render,   pp_action,   pp_poll, NULL},
    {"扫雷",       mw_init,   mw_render,   mw_action,   NULL, NULL},
    {"俄罗斯方块", te_init,   te_render,   te_action,   te_poll, NULL},
    {"打砖块",     bk_init,   bk_render,   bk_action,   bk_poll, NULL},
    {"21点",       bj_init,   bj_render,   bj_action,   NULL, NULL},
    {"钓鱼",       fs_init,   fs_render,   fs_action,   fs_poll, NULL},
    {"闹钟",       al_init,   al_render,   al_action,   al_poll, NULL},
    {"猜单词",     hm_init,   hm_render,   hm_action,   NULL, NULL},
    {"深海潜水",   dv_init,   dv_render,   dv_action,   dv_poll, NULL},
    {"二进制",     bn_init,   bn_render,   bn_action,   NULL, NULL},
    {"反应力",     rr_init,   rr_render,   rr_action,   rr_poll, NULL},
    {"猜拳",       mr_init,   mr_render,   mr_action,   NULL, NULL},
    {"赛车",       rc_init,   rc_render,   rc_action,   rc_poll, NULL},
    {"局域网扫描", ns_init,   ns_render,   ns_action,   ns_poll, NULL},
};
#define ALEN2 (sizeof(s_apps)/sizeof(s_apps[0]))

/* NETTOOL: 启动另一迷你应用 (此刻 s_apps 已完整定义, ALEN2 可用) */
static void nt_launch(const char *title) {
    for (int i = 0; i < (int)ALEN2; i++)
        if (strcmp(s_apps[i].title, title) == 0) {
            s_app = i; s_view = 1; if (s_apps[i].init) s_apps[i].init();
            return;
        }
}

/* ================= 对外接口 =================
 * V1.0.92: 删除迷你应用启动器 (原 s_view==0 列表页, 已被应用管理网格取代).
 * 迷你应用仅经应用管理 mini_apps_launch() 直接打开, 以下接口只在应用打开时被调用. */
void mini_apps_reset(menu_state_t *st){
    (void)st;
    s_view = 0; s_app = -1;
}
void mini_apps_render(menu_state_t *st){
    st7305_handle_t *l = st->lcd;
    if (s_app < 0 || s_app >= (int)ALEN2) return;
    const mini_app_t *a = &s_apps[s_app];
    if (a->render) a->render(l);
}
void mini_apps_action(menu_state_t *st, menu_action_t a){
    if (a == MENU_ACTION_BACK || a == MENU_ACTION_HOME) { s_view = 0; s_app = -1; st->needs_redraw = true; return; }
    if (s_app >= 0 && s_app < (int)ALEN2) { const mini_app_t *m = &s_apps[s_app]; if (m->action) m->action(st, a); }
}
bool mini_apps_poll(menu_state_t *st){
    bool r = false;
    if (s_view != 0 && s_app >= 0 && s_app < (int)ALEN2) {
        const mini_app_t *m = &s_apps[s_app];
        if (m->poll) r = m->poll(st);
    }
    if (r) st->needs_redraw = true;
    return r;
}
bool mini_apps_touch(menu_state_t *st, int x, int y){
    if (s_app >= 0 && s_app < (int)ALEN2) {
        const mini_app_t *m = &s_apps[s_app];
        if (m->touch) { m->touch(st, x, y); st->needs_redraw = true; return true; }
    }
    return false;
}

/* ================= 应用管理集成接口 =================
 * 迷你应用并入应用管理右网格: TOOL=小工具, GAME=小游戏.
 * gi 为 s_apps[] 全局索引 (与 mini_icons[] 顺序一致). */
static const int s_tool_gi[] = {0,1,2,3,4,6,18,21,25};
static const int s_game_gi[] = {5,7,8,9,10,11,12,13,14,15,16,17,19,20,22,23,24};

int mini_apps_count(void) { return (int)ALEN2; }
const char *mini_apps_title(int gi) {
    return (gi >= 0 && gi < (int)ALEN2) ? s_apps[gi].title : "";
}
int mini_apps_cat_count(bool game) {
    const int *arr = game ? s_game_gi : s_tool_gi;
    return game ? (int)(sizeof(s_game_gi)/sizeof(s_game_gi[0]))
                : (int)(sizeof(s_tool_gi)/sizeof(s_tool_gi[0]));
}
int mini_apps_cat_gi(bool game, int k) {
    const int *arr = game ? s_game_gi : s_tool_gi;
    int n = mini_apps_cat_count(game);
    return (k >= 0 && k < n) ? arr[k] : -1;
}
bool mini_apps_active(void) { return (s_view == 1 && s_app >= 0 && s_app < (int)ALEN2); }
void mini_apps_launch(int gi) {
    if (gi < 0 || gi >= (int)ALEN2) return;
    s_app = gi; s_view = 1;
    if (s_apps[gi].init) s_apps[gi].init();
}
void mini_apps_close(void) { s_view = 0; s_app = -1; }

/* ================= 网络工具独立主菜单页 (NETTOOL) =================
 * 独立于迷你应用启动器. 若经 nt_launch 打开了嵌套迷你应用 (局域网扫描),
 * 自动转交 mini_apps_* 渲染/交互, BACK 后回到 NETTOOL 列表. */
void nettool_reset(menu_state_t *st) { (void)st; nt_init(); }
void nettool_render(menu_state_t *st) {
    if (mini_apps_active()) { mini_apps_render(st); return; }
    nt_render(st->lcd);
}
void nettool_action(menu_state_t *st, menu_action_t a) {
    if (mini_apps_active()) { mini_apps_action(st, a); return; }
    nt_action(st, a);
}
bool nettool_poll(menu_state_t *st) {
    if (mini_apps_active()) return mini_apps_poll(st);
    if (st->current_page != MENU_PAGE_NETTOOL) return false;
    bool ch = nt_poll(st);
    if (ch) st->needs_redraw = true;
    return ch;
}
bool nettool_touch(menu_state_t *st, int x, int y) {
    if (mini_apps_active()) return mini_apps_touch(st, x, y);
    nt_touch(st, x, y);
    return true;
}