/**
 * sdr_view.c — 频谱仪渲染层实现 (见 sdr_view.h)
 *
 * 绘制顺序不能换 (1bpp 的关键就在这)：
 *   顶栏 → 刻度 → 网格+本底线 → 谱线 → peak-hold → marker → 频率轴 → 信道条 → 瀑布 → 底栏菜单
 *   网格在谱线**之前** → 谱线自动盖住网格
 *   marker 在谱线**之后** → 点线压在谱线上也看得见
 */
#include "os_app_api.h"
#include "sdr_view.h"

#include <stdio.h>
#include <string.h>

/* ============================ 量程 ============================ */

static float s_bot = -100.0f, s_top = 0.0f;

void  sdrv_range_set(float bot_db, float top_db) { s_bot = bot_db; s_top = top_db; }
float sdrv_range_top(void) { return s_top; }
float sdrv_range_bot(void) { return s_bot; }

/* ============================ 小工具 ============================ */

int sdrv_ascii_w(const char *s)
{
    int w = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; ) {
        if (*p < 0x80) { w += 8; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { w += 16; p += 2; }
        else if ((*p & 0xF0) == 0xE0) { w += 16; p += 3; }
        else { w += 16; p += 4; }
    }
    return w;
}

void sdrv_ascii(st7305_handle_t *lcd, int x, int y, const char *s, bool inv)
{
    for (const char *p = s; *p; p++, x += 8) draw_ascii_small(lcd, x, y, *p, inv);
}

void sdrv_ascii_center(st7305_handle_t *lcd, int cx, int y, const char *s, bool inv)
{
    int w = sdrv_ascii_w(s);
    int x = cx - w / 2;
    if (x < 0) x = 0;
    if (x + w > SDRV_W) x = SDRV_W - w;
    sdrv_ascii(lcd, x, y, s, inv);
}

/* 16px 中英混排, 在 [x0,x1] 内居中 */

static void dotted_hline(st7305_handle_t *lcd, int x0, int x1, int y, int on, int period)
{
    for (int x = x0; x <= x1; x++)
        if ((x % period) < on) ui_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
}

/* ============================ 坐标换算 ============================ */

int sdrv_y_of_db(float db)
{
    float t = (db - s_bot) / (s_top - s_bot);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return SDRV_PLOT_Y1 - (int)(t * (float)(SDRV_PLOT_Y1 - SDRV_PLOT_Y0) + 0.5f);
}

int sdrv_x_of_mhz(float mhz, unsigned center_mhz, float span_mhz)
{
    float f0 = (float)center_mhz - span_mhz * 0.5f;
    float t  = (mhz - f0) / span_mhz;
    int   x  = SDRV_PLOT_X0 + (int)(t * (float)(SDRV_PLOT_W - 1) + 0.5f);
    if (x < SDRV_PLOT_X0) x = SDRV_PLOT_X0;
    if (x > SDRV_PLOT_X1) x = SDRV_PLOT_X1;
    return x;
}

float sdrv_mhz_of_x(int x, unsigned center_mhz, float span_mhz)
{
    float f0 = (float)center_mhz - span_mhz * 0.5f;
    return f0 + (float)(x - SDRV_PLOT_X0) * span_mhz / (float)(SDRV_PLOT_W - 1);
}

float sdrv_mhz_of_col(int idx, unsigned center_mhz, float span_mhz)
{
    return sdrv_mhz_of_x(SDRV_PLOT_X0 + idx, center_mhz, span_mhz);
}

int sdrv_axis_ticks(float span_mhz)
{
    int n = (int)(span_mhz / (float)SDRV_TICK_STEP_MHZ + 0.5f) + 1;
    if (n < 2) n = 2;
    if (n > 17) n = 17;
    return n;
}

int sdrv_axis_tick_x(int i, int n)
{
    if (n < 2) return SDRV_PLOT_X0;
    return SDRV_PLOT_X0 + (int)((long)i * (SDRV_PLOT_W - 1) / (n - 1));
}

/* ============================ ① 顶栏 (纯状态) ============================ */

static void draw_battery(st7305_handle_t *lcd, int pct)
{
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    const int bw = 22, bh = 12;
    const int x  = SDRV_W - 5 - bw - 3;
    const int y  = SDRV_TOP_Y + (SDRV_TOP_H - bh) / 2;

    draw_rect_outline(lcd, x, y, x + bw - 1, y + bh - 1, ST7305_COLOR_WHITE);
    draw_vline(lcd, x + bw, y + 4, y + bh - 5, ST7305_COLOR_WHITE);
    draw_hline(lcd, x + bw, x + bw + 1, y + 4, ST7305_COLOR_WHITE);
    draw_hline(lcd, x + bw, x + bw + 1, y + bh - 5, ST7305_COLOR_WHITE);

    int fw = (bw - 4) * pct / 100;
    if (fw > 0) fill_rect(lcd, x + 2, y + 2, x + 1 + fw, y + bh - 3, ST7305_COLOR_WHITE);
}

void sdrv_draw_top(st7305_handle_t *lcd, const sdrv_info_t *in)
{
    fill_rect(lcd, 0, SDRV_TOP_Y, SDRV_W - 1, SDRV_TOP_Y + SDRV_TOP_H - 1, ST7305_COLOR_BLACK);

    /* 只显示"看什么频段"和"还剩多少电" —— 其它参数全自动, 摆出来只是噪音 */
    char b[32];
    snprintf(b, sizeof b, "扫 %uMHz 附近", in->center_mhz);
    ui_text16(lcd, 6, 2, b, true);

    if (in->overrun_word >= 0)
        sdrv_ascii(lcd, SDRV_W - 5 - 32, 5, "!OVR", true);
    else
        draw_battery(lcd, in->batt_pct);
}

/* ============================ ② 谱图 ============================ */

static const int s_tick_rel[4] = { 60, 40, 20, 0 };
static int tick_y(int i) { return sdrv_y_of_db(s_top - (float)(60 - s_tick_rel[i])); }

void sdrv_draw_scale(st7305_handle_t *lcd)
{
    /* ★ 2026-10-15 linit: 最底下那格原来是中文"本底", 改成数字 **0**。
     *   理由: ① 四格统一成 0/20/40/60, 一眼就是"高于本底多少 dB"(相对刻度);
     *         ② 两个汉字 32px 会顶到绘图区左边 (PLOT_X0=32), 单个数字只有 8px, 干净。
     *   (当初写"本底"是怕只写 0 被当成 0dBm 绝对功率 —— 但 60/40/20 已说明是相对量。) */
    for (int i = 0; i < 4; i++) {
        char b[8];
        snprintf(b, sizeof b, "%d", s_tick_rel[i]);
        int y = tick_y(i) - 5;
        if (y < SDRV_PLOT_Y0) y = SDRV_PLOT_Y0;
        if (y > SDRV_PLOT_Y1 - 12) y = SDRV_PLOT_Y1 - 12;
        int w = sdrv_ascii_w(b);
        sdrv_ascii(lcd, (w < 26 ? 26 - w : 0), y, b, false);
    }
}

void sdrv_draw_grid(st7305_handle_t *lcd, const sdrv_info_t *in)
{
    (void)in;
    /* ★ 2026-10-14 linit: "虚线做成一格一格的正方形" —— 横竖都是 **32px 间距**
     *   (绘图区 368×128 ⇒ 横 11 格 / 竖 4 格), 一律点阵 (1bpp 靠"点/实"分层级)。 */
    for (int y = SDRV_PLOT_Y0 + 8; y <= SDRV_PLOT_Y1 - 4; y += 32)
        dotted_hline(lcd, SDRV_PLOT_X0, SDRV_PLOT_X1, y, 1, 4);
    for (int x = SDRV_PLOT_X0; x <= SDRV_PLOT_X1; x += 32)
        for (int y = SDRV_PLOT_Y0; y <= SDRV_PLOT_Y1; y++)
            if ((y % 4) == 0) ui_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
}

/* 谱线: 只有线不填面积 (实填/点阵填充在本底偏高时都会糊成一片黑, 实测过) */
static int s_tr_shift;                     /* 谱线水平偏移 (上方拖动调频的跟手预览) */
void sdrv_set_trace_shift(int px) { s_tr_shift = px; }

void sdrv_draw_trace(st7305_handle_t *lcd, const float *db, int n)
{
    /* ⛔ 必须判空: "空白优先"会传 NULL 进来 (2026-10-14 漏判 ⇒ 打开就重启) */
    if (!db || n <= 0) return;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        int x = SDRV_PLOT_X0 + i;
        if (x > SDRV_PLOT_X1) break;
        int si = i - s_tr_shift;           /* ★ 内容跟手平移 (拖动调频时的像素零头) */
        /* ⛔ 越界不要 "跳过不画" —— 那会在拖动方向的反侧留一条空白竖条。
         *   夹到边界 = 边缘值延展, 视觉上跟手更顺 (实测)。 */
        if (si < 0) si = 0;
        if (si >= n) si = n - 1;
        int y = sdrv_y_of_db(db[si]);
        if (y < SDRV_PLOT_Y0) y = SDRV_PLOT_Y0;
        if (y > SDRV_PLOT_Y1) y = SDRV_PLOT_Y1;
        int a = y, b2 = (prev >= 0) ? prev : y;
        if (a > b2) { int t = a; a = b2; b2 = t; }
        if (b2 > a) draw_vline(lcd, x, a, b2, ST7305_COLOR_BLACK);
        else        draw_vline(lcd, x, a, (b2 + 1 <= SDRV_PLOT_Y1) ? b2 + 1 : b2, ST7305_COLOR_BLACK);
        prev = y;
    }
}

void sdrv_draw_hold(st7305_handle_t *lcd, const float *hold, int n)
{
    if (!hold || n <= 0) return;      /* 同上: 判空 */
    for (int i = 0; i < n; i++) {
        if ((i % 3) == 2) continue;
        int x = SDRV_PLOT_X0 + i;
        if (x > SDRV_PLOT_X1) break;
        int si = i - s_tr_shift;                       /* 与谱线同步平移 */
        if (si < 0) si = 0;
        if (si >= n) si = n - 1;
        int y = sdrv_y_of_db(hold[si]);
        if (y <= SDRV_PLOT_Y0) y = SDRV_PLOT_Y0 + 1;
        ui_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        if ((i & 1) == 0) ui_draw_pixel(lcd, x, y - 1, ST7305_COLOR_BLACK);
    }
}

/* marker: ▼ + 点线 + **紧跟其下的反白读数框**（读数和标记放一起, 不用来回找） */
void sdrv_draw_marker(st7305_handle_t *lcd, int x, const char *label)
{
    if (x < SDRV_PLOT_X0) x = SDRV_PLOT_X0;
    if (x > SDRV_PLOT_X1) x = SDRV_PLOT_X1;

    for (int k = 0; k < 6; k++)
        draw_hline(lcd, x - (5 - k), x + (5 - k), SDRV_PLOT_Y0 + k, ST7305_COLOR_BLACK);
    for (int y = SDRV_PLOT_Y0 + 7; y <= SDRV_PLOT_Y1; y++)
        if ((y % 3) == 0) ui_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);

    if (!label || !*label) return;
    int w = ui_text16w(label) + 8;
    int x0 = x - w / 2;
    if (x0 < 2) x0 = 2;
    if (x0 + w > SDRV_W - 2) x0 = SDRV_W - 2 - w;
    int y0 = SDRV_PLOT_Y0 + 13;
    fill_rect(lcd, x0, y0, x0 + w - 1, y0 + 17, ST7305_COLOR_WHITE);
    draw_rect_outline(lcd, x0, y0, x0 + w - 1, y0 + 17, ST7305_COLOR_BLACK);
    ui_text16(lcd, x0 + 4, y0 + 1, label, false);
}

/* ============================ ③ 频率轴 (每 20MHz 才标数字) ============================ */

void sdrv_draw_axis(st7305_handle_t *lcd, const sdrv_info_t *in)
{
    draw_hline(lcd, 0, SDRV_W - 1, SDRV_AXIS_LINE, ST7305_COLOR_BLACK);
    int n = sdrv_axis_ticks(in->span_mhz);
    bool mid_labeled = false;
    for (int i = 0; i < n; i++) {
        int x = sdrv_axis_tick_x(i, n);
        bool mid = (i == n / 2);
        draw_vline(lcd, x, SDRV_AXIS_TICK, SDRV_AXIS_TICK + (mid ? 4 : 2), ST7305_COLOR_BLACK);

        /* tick 每 10MHz 一根(细, 不占地方); **数字每 20MHz 才标一个** */
        if ((i % SDRV_LABEL_EVERY) != 0 && i != n - 1 && !mid) continue;
        if (mid) mid_labeled = true;
        float mhz = (float)in->center_mhz - in->span_mhz * 0.5f
                  + in->span_mhz * (float)i / (float)(n - 1);
        char b[12];
        snprintf(b, sizeof b, "%d", (int)(mhz + 0.5f));
        sdrv_ascii_center(lcd, x, SDRV_AXIS_TXT_Y, b, false);
    }
    (void)mid_labeled;
    /* 轴上不再标 "MHz": 位置和第一个频率数字重叠(实测 "MHz397"),
     * 单位由顶栏"中心2437 / 跨度80M"表达已足够 */
}

/* ============================ ④ 信道标注条 ============================ */

void sdrv_draw_channels(st7305_handle_t *lcd, const sdrv_info_t *in)
{
    draw_hline(lcd, 0, SDRV_W - 1, SDRV_CH_Y0 - 1, ST7305_COLOR_BLACK);
    ui_text16(lcd, 1, SDRV_CH_Y0 + 1, "信道", false);

    float f_lo = (float)in->center_mhz - in->span_mhz * 0.5f;
    float f_hi = (float)in->center_mhz + in->span_mhz * 0.5f;

    for (int n = 1; n <= 13; n++) {                     /* WiFi 1~13 */
        float mhz = 2412.0f + 5.0f * (float)(n - 1);
        if (mhz < f_lo || mhz > f_hi) continue;
        int x = sdrv_x_of_mhz(mhz, in->center_mhz, in->span_mhz);
        bool major = (n == 1 || n == 6 || n == 11);
        draw_vline(lcd, x, SDRV_CH_Y0, SDRV_CH_Y0 + (major ? 4 : 2), ST7305_COLOR_BLACK);
        if (major) {
            char b[4];
            snprintf(b, sizeof b, "%d", n);
            sdrv_ascii_center(lcd, x, SDRV_CH_Y0 + 5, b, false);
        }
    }
    static const float ble[3] = { 2402.0f, 2426.0f, 2480.0f };
    for (int i = 0; i < 3; i++) {                       /* BLE 广播 */
        if (ble[i] < f_lo || ble[i] > f_hi) continue;
        int x = sdrv_x_of_mhz(ble[i], in->center_mhz, in->span_mhz);
        draw_vline(lcd, x, SDRV_CH_Y0, SDRV_CH_Y0 + 4, ST7305_COLOR_BLACK);
        sdrv_ascii_center(lcd, x, SDRV_CH_Y0 + 5, "B", false);
    }
}

/* ============================ ⑤ 瀑布 ============================ */

void sdrv_draw_waterfall(st7305_handle_t *lcd, const uint8_t *bmp)
{
    if (!bmp) return;
    /* ★ 轴与瀑布之间的分隔基线 (2px): 明确切开"频谱区"与"时间轴" */
    draw_hline(lcd, 6, SDRV_W - 7, SDRV_WF_Y0 - 3, ST7305_COLOR_BLACK);
    draw_hline(lcd, 6, SDRV_W - 7, SDRV_WF_Y0 - 2, ST7305_COLOR_BLACK);
    /* ⛔ 2026-10-14 linit: 瀑布左侧的「强/弱」标注与强度刻度不要显示 */
    st7305_draw_bitmap_1bit(lcd, SDRV_PLOT_X0, SDRV_WF_Y0, SDRV_PLOT_W, SDRV_WF_H, bmp);
}

/* ============================ ⑥ 底栏功能菜单 ============================ */
/* 5 个等分槽: 常态显示各功能当前值; 菜单模式下选中槽反白。
 * "看到的槽" = "按到的槽" —— 键位判定在 page_sdr.c, 用同一组 SDRV_MENU_W。 */
void sdrv_draw_hint(st7305_handle_t *lcd, const char *left, const char *right)
{
    fill_rect(lcd, 0, SDRV_BOT_Y, SDRV_W - 1, SDRV_H - 1, ST7305_COLOR_BLACK);
    if (left && *left)   ui_text16(lcd, 8, SDRV_BOT_Y + 5, left, true);
    if (right && *right) {
        int w = ui_text16w(right);
        ui_text16(lcd, SDRV_W - 8 - w, SDRV_BOT_Y + 5, right, true);
    }
}

/* ============================ 扫描判读报告 ============================ */

void sdrv_draw_report(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    char b[64];

    /* 版式语言照 netdect(网络检测): ① 顶部标题 + 3px 横线 ② 一个"恒定内容框"包住主体
     *   ③ 底部贴边导航文字 ④ 内容全是逐行文本/图形, 不留大片空白。 */
    ui_text16(lcd, 6, 2, r->title ? r->title : "扫描判读", false);
    snprintf(b, sizeof b, "%uM", (unsigned)r->span_mhz);
    ui_text16(lcd, SDRV_W - 6 - ui_text16w(b), 2, b, false);
    draw_hline(lcd, 0, SDRV_W - 1, 22, ST7305_COLOR_BLACK);
    draw_hline(lcd, 0, SDRV_W - 1, 23, ST7305_COLOR_BLACK);
    draw_hline(lcd, 0, SDRV_W - 1, 24, ST7305_COLOR_BLACK);

    draw_rect_outline(lcd, 4, 28, SDRV_W - 5, SDRV_BOT_Y - 6, ST7305_COLOR_BLACK);

    /* ---- 框内 ① 实时谱线小条 ("浮动"的那张图的缩略) ---- */
    const int mx0 = 12, mx1 = SDRV_W - 13, my0 = 34, my1 = 78;
    draw_hline(lcd, mx0, mx1, my1, ST7305_COLOR_BLACK);
    if (r->trace && r->trace_n > 0) {
        float lo = sdrv_range_bot(), hi = sdrv_range_top();   /* 与主图同一个量程 */
        int prev = -1;
        for (int i = 0; i < r->trace_n; i++) {
            int x = mx0 + i;
            if (x > mx1) break;
            float t = (r->trace[i] - lo) / (hi - lo);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            int y = my1 - (int)(t * (float)(my1 - my0) + 0.5f);
            int a = y, b2 = (prev >= 0) ? prev : y;
            if (a > b2) { int t2 = a; a = b2; b2 = t2; }
            if (b2 > a) draw_vline(lcd, x, a, b2, ST7305_COLOR_BLACK);   /* 细线: 不填面积 */
            else        draw_vline(lcd, x, a, (b2 + 1 <= my1) ? b2 + 1 : b2, ST7305_COLOR_BLACK);
            prev = y;
        }
    }

    /* ---- 框内 ② 13 条 WiFi 信道占用 ---- */
    const int bar_base = 138, bar_h = 44, bw = 24, gap = 4;
    const int bx0 = 18;
    draw_hline(lcd, bx0 - 2, bx0 + 12 * (bw + gap) + bw + 1, bar_base, ST7305_COLOR_BLACK);
    ui_text16(lcd, 8, 82, "信道", false);          /* 固定在小条下方, 别压谱线 */
    for (int i = 0; i < 13; i++) {
        int bx = bx0 + i * (bw + gap);
        int h = (r->pct[i] < 0) ? 0 : r->pct[i] * bar_h / 100;
        if (h > bar_h) h = bar_h;
        if (h > 0) fill_rect(lcd, bx, bar_base - h, bx + bw - 1, bar_base - 1, ST7305_COLOR_BLACK);
        else       draw_hline(lcd, bx, bx + bw - 1, bar_base - 1, ST7305_COLOR_BLACK);
        snprintf(b, sizeof b, "%d", i + 1);
        sdrv_ascii_center(lcd, bx + bw / 2, bar_base + 3, b, false);
    }

    /* ---- 框内 ③ 统计 + 结论 + 建议 (逐行, 填满下半) ---- */
    int y = 156;
    if (r->worst_ch) {
        snprintf(b, sizeof b, "最挤 ch%d %d%%", r->worst_ch, r->pct[r->worst_ch - 1]);
        ui_text16(lcd, 10, y, b, false);
    }
    if (r->best_ch) {
        snprintf(b, sizeof b, "最空 ch%d %d%%", r->best_ch, r->pct[r->best_ch - 1]);
        ui_text16(lcd, 208, y, b, false);
    }
    y += 22;
    if (r->ble_mhz) snprintf(b, sizeof b, "蓝牙 有%dM", r->ble_mhz);
    else            snprintf(b, sizeof b, "蓝牙 未检到");
    ui_text16(lcd, 10, y, b, false);
    ui_text16(lcd, 208, y, r->broadband ? "宽带 有" : "宽带 无", false);

    y += 24;
    ui_text16(lcd, 10, y, r->verdict ? r->verdict : "", false);
    y += 22;
    ui_text16(lcd, 10, y, r->advice ? r->advice : "", false);

    /* ---- 底部贴边导航 (netdect 同款: 左右贴边, 无边距框) ---- */
    fill_rect(lcd, 0, SDRV_BOT_Y, SDRV_W - 1, SDRV_H - 1, ST7305_COLOR_BLACK);
    ui_text16(lcd, 10, SDRV_BOT_Y + 5, "确认 频谱", true);
    b[0] = 0; snprintf(b, sizeof b, "返回 退出");
    ui_text16(lcd, SDRV_W - 10 - ui_text16w(b), SDRV_BOT_Y + 5, b, true);
}

/* ============================ 提示 ============================ */

void sdrv_draw_notice(st7305_handle_t *lcd, const char *line1, const char *line2)
{
    const int cx = (SDRV_PLOT_X0 + SDRV_PLOT_X1) / 2;
    if (line1) ui_text16c(lcd, cx, SDRV_PLOT_Y0 + 40, line1, false);
    if (line2) ui_text16c(lcd, cx, SDRV_PLOT_Y0 + 62, line2, false);
}

/* ============================ 按功能分屏 ============================
 * 一屏只干一件事: 全景只看图; 信道只看 13 根柱; 蓝牙只看三条; 干扰只看三行指示。
 * 不再把"图 + 统计 + 结论 + 建议"拼在一屏里。 */

/* 顶栏: 左=当前源/页名 (16px) · 右=频段 (11px) —— **不画标题横线** */
void sdrv_draw_topbar(st7305_handle_t *lcd, const char *title, const char *right)
{
    if (title && *title) ui_text16(lcd, 6, 1, title, false);
    if (right && *right) {
        int w = sdrv_ascii_w(right);
        sdrv_ascii(lcd, SDRV_W - 6 - w, 4, right, false);
    }
}

/* 底栏软键栏 (与 netdect / logdiag 同套): 盖子线 + 左右斜坡 + 谷线; 凹口放状态字 */
void sdrv_draw_softbar(st7305_handle_t *lcd, const char *left, const char *right, const char *mid)
{
    int wl = (left && *left) ? ui_text16w(left) : 0;
    int wr = (right && *right) ? ui_text16w(right) : 0;
    if (wl) ui_text16(lcd, SDRV_SOFT_MARGIN, SDRV_SOFT_KEYY, left, false);
    if (wr) ui_text16(lcd, SDRV_W - SDRV_SOFT_MARGIN - wr, SDRV_SOFT_KEYY, right, false);
    if (mid && *mid) {
        int w = sdrv_ascii_w(mid);
        sdrv_ascii(lcd, (SDRV_W - w) / 2, SDRV_SOFT_VALLEY - 13, mid, false);
    }
    int Lend = wl ? (SDRV_SOFT_MARGIN + wl + 12) : 40;
    int Rstart = wr ? (SDRV_W - SDRV_SOFT_MARGIN - wr - 12) : (SDRV_W - 40);
    draw_hline(lcd, 0, Lend, SDRV_SOFT_LID, ST7305_COLOR_BLACK);
    draw_hline(lcd, Rstart, SDRV_W - 1, SDRV_SOFT_LID, ST7305_COLOR_BLACK);
    int slope = SDRV_SOFT_VALLEY - SDRV_SOFT_LID;
    if (Lend + slope < Rstart - slope)
        draw_hline(lcd, Lend + slope, Rstart - slope, SDRV_SOFT_VALLEY, ST7305_COLOR_BLACK);
    for (int d = 0; d <= slope && d < 40; d++) {
        ui_draw_pixel(lcd, Lend + d, SDRV_SOFT_LID + d, ST7305_COLOR_BLACK);
        ui_draw_pixel(lcd, Rstart - d, SDRV_SOFT_LID + d, ST7305_COLOR_BLACK);
    }
}

void sdrv_draw_carousel(st7305_handle_t *lcd, const char *const *nm, int n, int sel, int dx)
{
    const int step = 76;                       /* 项间距 (靠中间一点, 保证两边文字显示全) */
    const int base = SDRV_SOFT_VALLEY - 18;    /* 16px 字的顶 (按 linit 要求上移 3px) */
    /* ★ 2026-10-14 linit: 下面这条是"当前在第几页"的指示器, **不是 1:1 跟手** ——
     *   上面拖满一屏(400px) ⇒ 下面正好走 **一个项间距**(即两个名字之间的距离),
     *   比例 ≈ step/SDRV_W (76/400)。原来直接把 s_slide(±400) 当位移用,
     *   一拖名字就全飞出凹口 ⇒ 看起来"滑动时没有名字"。 */
    const int cdx = dx * step / SDRV_W;
    for (int k = -1; k <= 1; k++) {
        /* ★ 环形轮播: 首尾相接 ⇒ **名字永远都在** (linit: "只显示两个")
         *   ⛔ 原来是线性的 (i<0 就跳过) ⇒ 停在第一页时左边没有上一项, 只画得出 2 个。 */
        int i = (sel + k + n) % n;
        /* ★ 方向: 手指往右拖(dx>0) → 内容跟着往右 → 左边的项移进来 (同向) */
        int vx = k * step + cdx;
        if (vx < -190 || vx > 190) continue;
        int w = ui_text16w(nm[i]);
        int x = SDRV_W / 2 + vx - w / 2;
        /* ★ 拖动时三个名字都画出来 (linit: "提前加载所有菜单文字, 别等松手才显示");
         *   超出凹口的部分由**软键栏的线**盖住 (绘制顺序: 轮播 → 涂白 → 软键栏)。 */
        ui_text16(lcd, x, base, nm[i], false);
    }
}

void sdrv_draw_modebar(st7305_handle_t *lcd, const char *title)
{
    ui_text16(lcd, 6, 2, title, false);
    draw_hline(lcd, 0, SDRV_W - 1, 22, ST7305_COLOR_BLACK);
    draw_hline(lcd, 0, SDRV_W - 1, 23, ST7305_COLOR_BLACK);
    draw_hline(lcd, 0, SDRV_W - 1, 24, ST7305_COLOR_BLACK);
}

void sdrv_draw_modemenu(st7305_handle_t *lcd, int sel, const char *const *names,
                        const char *const *notes, int n)
{
    const int row_h = 44, y0 = 58;
    for (int i = 0; i < n; i++) {
        int y = y0 + i * row_h;
        bool on = (i == sel);
        if (on) fill_rect(lcd, 6, y - 6, SDRV_W - 7, y + 26, ST7305_COLOR_BLACK);
        /* ⚠ ui_text16 的 inverted=true 表示"底色是黑的, 画白字"。所以:
         *   选中(黑底) → true; 未选中(白底) → false。写成 !on 会两边都看不见。 */
        ui_text16(lcd, 16, y, names[i], on);
        if (notes && notes[i] && *notes[i]) {
            int w = ui_text16w(notes[i]);
            ui_text16(lcd, SDRV_W - 16 - w, y, notes[i], on);
        }
    }
}

void sdrv_draw_panorama(st7305_handle_t *lcd, const sdrv_info_t *in,
                        const float *trace, const float *hold, int n, const uint8_t *wf)
{
    /* 纯图: 频谱 + 瀑布。只有刻度这种"读图必需"的字, 没有任何统计/结论 */
    sdrv_draw_scale(lcd);
    sdrv_draw_grid(lcd, in);
    sdrv_draw_trace(lcd, trace, n);
    sdrv_draw_hold(lcd, hold, n);
    sdrv_draw_axis(lcd, in);
    sdrv_draw_waterfall(lcd, wf);
}

void sdrv_draw_chanbars(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    char b[40];    /* ⚠ 中文一字符 3 字节: b[16] 放不下 "较忙 2 条 / 13"(19B) 会被截断 */
    const int base = 214, hmax = 158, bw = 26, gap = 2;
    const int x0 = 8;
    /* 柱子按"最强信道"自动缩放: 目的是比出"哪条最挤", 绝对百分比在柱顶写出来就够了;
     * 固定 0~100% 的话, 安静环境全是 6~13% 的矮柱, 完全看不出差别 (实测)。 */
    int mx = 20;
    for (int i = 0; i < 13; i++) if (r->pct[i] > mx) mx = r->pct[i];
    draw_hline(lcd, 4, SDRV_W - 5, base, ST7305_COLOR_BLACK);
    if (r->best_ch) {
        snprintf(b, sizeof b, "建议用 ch%d", r->best_ch);
        ui_text16(lcd, 12, 32, b, false);          /* 结论放顶部, 不占柱子的地方 */
    }
    /* 判断说明 (linit: "下面判断可以写多一点") */
    {
        int busy = 0, mx2 = 0;
        for (int i = 0; i < 13; i++) {
            if (r->pct[i] >= 10) busy++;
            if (r->pct[i] > mx2) mx2 = r->pct[i];
        }
        snprintf(b, sizeof b, "较忙 %d 条 / 13", busy);
        ui_text16(lcd, 12, 54, b, false);
        snprintf(b, sizeof b, "占用 = 高出本底");
        ui_text16(lcd, 210, 54, b, false);
        /* 底部: 最挤 / 最空 写全 */
        if (r->worst_ch) {
            snprintf(b, sizeof b, "最挤 ch%d %d%%", r->worst_ch, r->pct[r->worst_ch - 1]);
            ui_text16(lcd, 12, 246, b, false);
        }
        if (r->best_ch) {
            snprintf(b, sizeof b, "最空 ch%d %d%%", r->best_ch, r->pct[r->best_ch - 1]);
            ui_text16(lcd, 210, 246, b, false);
        }
    }
    for (int i = 0; i < 13; i++) {
        int bx = x0 + i * (bw + gap);
        int h = (r->pct[i] < 0) ? 0 : r->pct[i] * hmax / mx;
        if (h > hmax) h = hmax;
        if (h > 6) {
            fill_rect(lcd, bx, base - h, bx + bw - 1, base - 1, ST7305_COLOR_BLACK);
            snprintf(b, sizeof b, "%d%%", r->pct[i]);
            sdrv_ascii_center(lcd, bx + bw / 2, base - h - 12, b, false);
        }
        snprintf(b, sizeof b, "%d", i + 1);
        sdrv_ascii_center(lcd, bx + bw / 2, base + 4, b, false);
    }
}

void sdrv_draw_blepage(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    static const int mhz[3] = { 2402, 2426, 2480 };
    char b[40];
    const int bx = 78, bw = 280, y0 = 60, rh = 52;
    for (int i = 0; i < 3; i++) {
        int y = y0 + i * rh;
        snprintf(b, sizeof b, "%dM", mhz[i]);
        ui_text16(lcd, 10, y, b, false);
        draw_rect_outline(lcd, bx - 1, y + 4, bx + bw, y + 26, ST7305_COLOR_BLACK);
        int db = r->ble_db[i]; if (db < 0) db = 0; if (db > 30) db = 30;
        int w = db * bw / 30;
        if (w > 0) fill_rect(lcd, bx, y + 5, bx + w - 1, y + 25, ST7305_COLOR_BLACK);
        snprintf(b, sizeof b, "+%ddB", db);
        ui_text16(lcd, bx + bw + 8, y, b, false);
    }
    ui_text16(lcd, 12, 228, r->ble_mhz ? "有蓝牙广播" : "未检到蓝牙广播", false);
}

void sdrv_draw_jampage(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    char b[40];
    const int y0 = 64, rh = 46;
    ui_text16(lcd, 14, y0,        "宽带干扰", false);
    ui_text16(lcd, 210, y0, r->broadband ? "有" : "无", false);
    ui_text16(lcd, 14, y0 + rh,   "明显信号", false);
    snprintf(b, sizeof b, "%d 个", r->n_sig);
    ui_text16(lcd, 210, y0 + rh, b, false);
    ui_text16(lcd, 14, y0 + rh * 2, "最挤信道", false);
    if (r->worst_ch) snprintf(b, sizeof b, "ch%d", r->worst_ch);
    else             snprintf(b, sizeof b, "--");
    ui_text16(lcd, 210, y0 + rh * 2, b, false);
    draw_hline(lcd, 8, SDRV_W - 9, 214, ST7305_COLOR_BLACK);
    ui_text16(lcd, 14, 226, r->verdict ? r->verdict : "", false);
}

/* 扫描中: 只显示进度, 不出任何数字 —— 结果要给就给定格的那一份 */
void sdrv_draw_scanning(st7305_handle_t *lcd, const char *title, int pct)
{
    char b[16];
    sdrv_draw_modebar(lcd, title);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ui_text16c(lcd, SDRV_W / 2, 120, "扫描中...", false);
    draw_rect_outline(lcd, 40, 152, SDRV_W - 41, 176, ST7305_COLOR_BLACK);
    int w = (SDRV_W - 84) * pct / 100;
    if (w > 0) fill_rect(lcd, 42, 154, 41 + w, 174, ST7305_COLOR_BLACK);
    snprintf(b, sizeof b, "%d%%", pct);
    sdrv_ascii_center(lcd, SDRV_W / 2, 182, b, false);
}
