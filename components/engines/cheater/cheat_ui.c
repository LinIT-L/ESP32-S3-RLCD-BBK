/**
 * cheat_ui.c — 修改机共享 UI (引擎无关).
 *
 * 从 components/gam4980/gam4980_emu.c 抽离 (原 bbk_cheat_* 系列), 去除对步步高
 * 引擎全局变量(s_cheat_bg/g_lcd)的依赖:
 *   - LCD 句柄改为入参;
 *   - 冻结帧快照缓冲(PSRAM 15KB)移入本模块;
 *   - 搜索区域改为由 cheat_ui_open() 传入的 cheater_region_t.
 *
 * 安全: 所有搜索/读写都经 cheater 组件对 region.peek/poke 进行, 只覆盖引擎自有的
 * 游戏 RAM, 绝不会触碰系统/其他软件内存.
 *
 * 交互: 手柄直读电平(UP/DOWN/CONFIRM/BACK/LEFT/RIGHT) + input 触摸动作, 均进程全局.
 * 循环每轮 vTaskDelay(16) 让出 CPU; 不调用 esp_task_wdt_reset (app_main 等未注册
 * TWDT 的任务调用会刷屏 error, 与 gam4980 原逻辑一致, 靠让出 CPU 保持喂狗正常).
 *
 * ★ 2026-10-25 强化 (参照 烧饼修改器 / 八门神器):
 *   ① 主菜单 4 项: 搜索 / 过滤(带剩余数) / 记录(候选列表) / 锁定(锁定表);
 *   ② 候选列表与锁定列表改 **ui_scroll 统一跟手滚动** (惯性+回弹+行吸附),
 *      原来手写 1:1 拖动, 8000 条候选要拖到手酸;
 *   ③ 点选某条 → 行操作菜单 (改值 / 锁定·解锁 / 删除), 触屏与手柄同一套;
 *   ④ 过滤菜单增「撤销上一步」: 误选"变大/变小"能退回;
 *   ⑤ 头部显示 "候选 N · 第 R 轮", 搜索后回显耗时 (性能可观测).
 */
#include "cheat_ui.h"
#include "ui_common.h"
#include "ui_scroll.h"
#include "input.h"
#include "bt_manager.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vibrator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define CUI_TAG "CHEAT_UI"

#define FB_BYTES (ST7305_WIDTH * ST7305_HEIGHT / 8)   /* 15000 */
#define CUI_ROW_H  36
#define CUI_EDGE   4    /* 内容区上缘 = 窗顶 + 4 */
#define CUI_FOOT   "\xe8\xbf\x94\xe5\x9b\x9e"   /* 返回 */

/* 修改机大浮动图标: 无边框, 默认盖右上角, 可拖动. */
#define CUI_FLOAT_SZ   48
#define CUI_FLOAT_DFX  (ST7305_WIDTH - CUI_FLOAT_SZ - 2)
#define CUI_FLOAT_DFY  2
#define CUI_FLOAT_DRAG_PX 6

/* 冻结帧快照(PSRAM) + 当前会话 region(在 cheat_ui_open 时绑定, "搜索"时重建会话) */
EXT_RAM_BSS_ATTR static uint8_t s_cheat_bg[FB_BYTES];
static cheater_region_t s_reg;
static cheater_bits_t    s_bits = CHEAT_BITS_8;

static void cheat_center_msg(st7305_handle_t *lcd, const char *msg);
static void cui_touch_vib(void);   /* 触摸(无手柄)命中可操作键时的反馈震动 */

/* ================= 热键 ================= */
bool cheat_ui_hotkey(void)
{
    return bt_manager_is_key_pressed(F_SELECT) &&
           bt_manager_is_key_pressed(F_START);
}

/* ================= 触摸浮动图标 ================= */
static bool cheat_float_visible(void)
{
    return input_has_touch() && !bt_manager_is_connected();
}
static int s_float_x = CUI_FLOAT_DFX;
static int s_float_y = CUI_FLOAT_DFY;
static bool s_fpressed = false;
static bool s_fmoved   = false;
static bool s_fstart_in = false;
static bool s_farmed   = false;       /* 长按≥1s 已震动解锁, 之后才允许拖动 */
static uint32_t s_fpress_ms = 0;      /* 按下时刻 ms */
static int  s_fpress_x = 0, s_fpress_y = 0;

void cheat_ui_float_clear(void)
{
    s_float_x = CUI_FLOAT_DFX;
    s_float_y = CUI_FLOAT_DFY;
    s_fpressed = false; s_fmoved = false; s_farmed = false;
}

void cheat_ui_float_draw(st7305_handle_t *lcd)
{
    if (!lcd || !cheat_float_visible()) return;
    int cx = s_float_x + CUI_FLOAT_SZ / 2;
    int cy = s_float_y + CUI_FLOAT_SZ / 2;
    draw_main_icon_stretched(lcd, cx, cy, CUI_FLOAT_SZ, CUI_FLOAT_SZ, ICON_MODTOOL);
}

bool cheat_ui_float_tick(st7305_handle_t *lcd)
{
    (void)lcd;
    if (!cheat_float_visible()) {
        s_fpressed = false; s_fmoved = false; s_farmed = false;
        return false;
    }
    int x, y;
    bool down = input_get_touch_pos(&x, &y);
    if (!down) {
        /* 短按(未长按解锁、未移动、起点在框内) = 点击呼出修改机; 长按解锁后松手不呼出 */
        bool click = s_fpressed && !s_farmed && !s_fmoved && s_fstart_in;
        s_fpressed = false; s_fmoved = false; s_farmed = false;
        return click;
    }
    if (!s_fpressed) {
        s_fpressed = true;
        s_fmoved = false;
        s_farmed = false;
        s_fpress_x = x; s_fpress_y = y;
        s_fpress_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_fstart_in = (x >= s_float_x && x < s_float_x + CUI_FLOAT_SZ &&
                       y >= s_float_y && y < s_float_y + CUI_FLOAT_SZ);
    } else {
        /* 长按 1s → 震动解锁, 之后才允许拖动 (防误触) */
        if (!s_farmed && (uint32_t)(esp_timer_get_time() / 1000) - s_fpress_ms >= 1000) {
            s_farmed = true;
            vibrator_click();
        }
        int dx = x - s_fpress_x, dy = y - s_fpress_y;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        if (s_farmed && !s_fmoved && (adx > CUI_FLOAT_DRAG_PX || ady > CUI_FLOAT_DRAG_PX)) {
            s_fmoved = true;
            s_fstart_in = false;
        }
        if (s_fmoved) {
            int nx = x - CUI_FLOAT_SZ / 2;
            int ny = y - CUI_FLOAT_SZ / 2;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > ST7305_WIDTH - CUI_FLOAT_SZ) nx = ST7305_WIDTH - CUI_FLOAT_SZ;
            if (ny > ST7305_HEIGHT - CUI_FLOAT_SZ) ny = ST7305_HEIGHT - CUI_FLOAT_SZ;
            if (nx != s_float_x || ny != s_float_y) {
                s_float_x = nx; s_float_y = ny;
            }
        }
    }
    return false;
}

/* ================= 输入/布局 ================= */
static void cheat_flush_input(void)
{
    for (int i = 0; i < 3; i++) {
        menu_action_t a = input_get_action();
        (void)a;
        input_consume_tap(NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* 自适应布局 (渲染/触摸共用): sel==n 表示底部"返回"行 */
static void cheat_win_layout(int n, int sel, const char *const rows[],
                             int *x0, int *y0, int *content_y0, int *footer_y,
                             int *visible, int *scroll, int *win_w, int *win_h)
{
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (rows && rows[i]) ? text_width(rows[i]) : 0;
        if (w > maxw) maxw = w;
    }
    int fw = text_width(CUI_FOOT); if (fw > maxw) maxw = fw;
    int W = maxw + 28; if (W < 180) W = 180; if (W > 390) W = 390;
    int H = CUI_EDGE + n * CUI_ROW_H + 2 + CUI_ROW_H + CUI_EDGE;
    if (H > 292) H = 292;
    if (H < 44) H = 44;
    *win_w = W; *win_h = H;
    *x0 = (ST7305_WIDTH - W) / 2; *y0 = (ST7305_HEIGHT - H) / 2;
    *footer_y = *y0 + H - CUI_EDGE - CUI_ROW_H;
    *content_y0 = *y0 + CUI_EDGE;
    int area_vis = (*footer_y - 2) - *content_y0;
    int vis = area_vis / CUI_ROW_H; if (vis < 1) vis = 1;
    *visible = vis;
    int sc = 0;
    if (sel >= vis && sel < n) sc = sel - vis + 1;
    *scroll = sc;
}

static void cheat_draw_row(st7305_handle_t *lcd, int x0, int y0, int w, int row_y,
                           const char *text, bool sel)
{
    int rx0 = x0 + 6, rx1 = x0 + w - 6;
    int top = row_y + (CUI_ROW_H - 24) / 2;
    if (sel) {
        fill_rect(lcd, rx0, row_y, rx1 - 1, row_y + CUI_ROW_H - 2, ST7305_COLOR_BLACK);
        draw_text_centered(lcd, top, text, true);
    } else {
        fill_rect(lcd, rx0, row_y, rx1 - 1, row_y + CUI_ROW_H - 2, ST7305_COLOR_WHITE);
        draw_text_centered(lcd, top, text, false);
    }
}

static void cheat_draw_win(st7305_handle_t *lcd, const char *const rows[], int n, int sel)
{
    int x0, y0, content_y0, footer_y, visible, scroll, win_w, win_h;
    cheat_win_layout(n, sel, rows, &x0, &y0, &content_y0, &footer_y,
                     &visible, &scroll, &win_w, &win_h);
    int x1 = x0 + win_w - 1, y1 = y0 + win_h - 1;

    /* 回填冻结帧于窗口之外 (窗外保留游戏画面) */
    if (lcd->fb) memcpy(lcd->fb, s_cheat_bg, FB_BYTES);
    fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, x0 + k, x1 - k, y0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + k, x1 - k, y1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + k, y0 + k, y1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x1 - k, y0 + k, y1 - k, ST7305_COLOR_BLACK);
    }
    for (int i = 0; i < visible; i++) {
        int idx = i + scroll;
        if (idx < 0 || idx >= n) continue;
        int row_y = content_y0 + i * CUI_ROW_H;
        if (row_y + CUI_ROW_H <= y0 + CUI_EDGE || row_y >= footer_y - 2) continue;
        if (rows[idx] && rows[idx][0])
            cheat_draw_row(lcd, x0, y0, win_w, row_y, rows[idx], (idx == sel));
    }
    if (n > 0)
        draw_hline(lcd, x0 + 6, x1 - 6, footer_y - 1, ST7305_COLOR_BLACK);
    cheat_draw_row(lcd, x0, y0, win_w, footer_y, CUI_FOOT, (sel >= n));

    if (n > visible) {
        int bar_x = x1 - 3;
        int bar_y0 = content_y0, bar_y1 = content_y0 + visible * CUI_ROW_H - 1;
        draw_vline(lcd, bar_x, bar_y0, bar_y1, ST7305_COLOR_BLACK);
        int track_h = bar_y1 - bar_y0 + 1;
        int thumb_h = (track_h * visible) / n; if (thumb_h < 4) thumb_h = 4;
        int max_scroll = n - visible;
        int thumb_y = bar_y0 + (max_scroll > 0 ? (track_h - thumb_h) * scroll / max_scroll : 0);
        if (thumb_y < bar_y0) thumb_y = bar_y0;
        if (thumb_y + thumb_h > bar_y1 + 1) thumb_y = bar_y1 + 1 - thumb_h;
        draw_vline(lcd, bar_x - 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, bar_x + 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        for (int ty = thumb_y; ty < thumb_y + thumb_h; ty++)
            st7305_draw_pixel(lcd, bar_x, ty, ST7305_COLOR_BLACK);
    }
    st7305_flush(lcd);
}

static void cheat_center_msg(st7305_handle_t *lcd, const char *msg)
{
    if (!lcd) return;
    if (lcd->fb) memcpy(lcd->fb, s_cheat_bg, FB_BYTES);
    int tw = text_width(msg);
    int w = tw + 16; if (w > 360) w = 360;
    int h = 36;
    int x0 = (ST7305_WIDTH - w) / 2, y0 = (ST7305_HEIGHT - h) / 2, x1 = x0 + w - 1, y1 = y0 + h - 1;
    fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 3; k++) {
        draw_hline(lcd, x0 + k, x1 - k, y0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + k, x1 - k, y1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + k, y0 + k, y1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x1 - k, y0 + k, y1 - k, ST7305_COLOR_BLACK);
    }
    draw_text_centered(lcd, y0 + 6, msg, false);
    st7305_flush(lcd);
}

/* 列表菜单通用: 返回选中索引 0..n; 返回 back_val 表示"返回" */
static int cheat_menu_loop(st7305_handle_t *lcd, const char *const items[], int n,
                           int back_val)
{
    int sel = 0;
    bool conn = bt_manager_is_connected();
    bool up_prev = conn && bt_manager_is_key_pressed(F_UP);
    bool dn_prev = conn && bt_manager_is_key_pressed(F_DOWN);
    bool ok_prev = conn && bt_manager_is_key_pressed(F_CONFIRM);
    bool bk_prev = conn && bt_manager_is_key_pressed(F_BACK);

    cheat_flush_input();
    int x0, y0, content_y0, footer_y, visible, scroll, win_w, win_h;
    cheat_win_layout(n, 0, items, &x0, &y0, &content_y0, &footer_y,
                     &visible, &scroll, &win_w, &win_h);
    while (1) {
        cheat_draw_win(lcd, items, n, sel);
        menu_action_t act = input_get_action();
        if (act == MENU_ACTION_UP || act == MENU_ACTION_LEFT) {
            if (sel > 0) sel--;
        } else if (act == MENU_ACTION_DOWN || act == MENU_ACTION_RIGHT) {
            if (sel < n) sel++;
        } else if (act == MENU_ACTION_BACK) {
            return back_val;
        } else if (act == MENU_ACTION_CONFIRM) {
            int tx, ty2;
            if (input_consume_tap(&tx, &ty2)) {
                if (ty2 >= footer_y && ty2 < footer_y + CUI_ROW_H) {
                    cui_touch_vib();
                    return back_val;
                }
                int rel = ty2 - (content_y0 - scroll * CUI_ROW_H);
                int row = rel / CUI_ROW_H;
                if (row >= 0 && row < n) {
                    cui_touch_vib();
                    return row;
                }
                /* 弹窗外/非按键空白: 无动作、不震动 */
            } else {
                return (sel >= n) ? back_val : sel;
            }
        }
        bool up = conn && bt_manager_is_key_pressed(F_UP);
        bool dn = conn && bt_manager_is_key_pressed(F_DOWN);
        bool ok = conn && bt_manager_is_key_pressed(F_CONFIRM);
        bool bk = conn && bt_manager_is_key_pressed(F_BACK);
        if (up && !up_prev) { if (sel > 0) sel--; }
        if (dn && !dn_prev) { if (sel < n) sel++; if (sel < 0) sel = 0; }
        if (ok && !ok_prev) return (sel >= n) ? back_val : sel;
        if (bk && !bk_prev) return back_val;
        up_prev = up; dn_prev = dn; ok_prev = ok; bk_prev = bk;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* ================= 数值键盘: 3x4 棋盘 (1..9 + [删除|0|确定]) =================
 * 用系统分割线整块划分, 不做每格独立方框. 触摸(无手柄)下无常驻加黑光标:
 * 按下的那一格才加黑, 松手即恢复; 点空白不震动. 网格外框贴紧弹窗边缘. */
#define KP_COLS   3
#define KP_ROWS   4
#define KP_WIN_W  194
#define KP_WIN_H  204
#define KP_WIN_X0 ((ST7305_WIDTH - KP_WIN_W) / 2)
#define KP_WIN_Y0 ((ST7305_HEIGHT - KP_WIN_H) / 2)
#define KP_WIN_X1 (KP_WIN_X0 + KP_WIN_W - 1)
#define KP_WIN_Y1 (KP_WIN_Y0 + KP_WIN_H - 1)
#define KP_X0     KP_WIN_X0                     /* 网格左缘 = 弹窗左缘 */
#define KP_TOP    (KP_WIN_Y0 + 62)              /* 分隔线下方起网格 */
#define KP_GRID_W (KP_WIN_X1 - KP_WIN_X0 + 1)   /* 与弹窗同宽 */
#define KP_GRID_H (KP_WIN_Y1 - KP_TOP + 1)      /* 撑到弹窗底缘 */
#define KP_CELL_W (KP_GRID_W / KP_COLS)
#define KP_CELL_H (KP_GRID_H / KP_ROWS)

/* 返回 0=确定 1=取消/返回 */
static void kp_draw_cell(st7305_handle_t *lcd, int r, int c, const char *label, bool hi)
{
    int x = KP_X0 + c * KP_CELL_W;
    int y = KP_TOP + r * KP_CELL_H;
    if (hi) fill_rect(lcd, x, y, x + KP_CELL_W - 1, y + KP_CELL_H - 1, ST7305_COLOR_BLACK);
    int lx = x + (KP_CELL_W - text_width(label)) / 2;
    int ly = y + (KP_CELL_H - 24) / 2;   /* 字体高 24, 上下留白对称居中 */
    draw_text(lcd, lx, ly, label, hi);
}

/* 触摸(无手柄)模式命中可操作键/选项时补一次反馈震动; 空白无动作保持静默. */
static void cui_touch_vib(void)
{
    if (!bt_manager_is_connected()) vibrator_click();
}

static int cheat_value_edit(st7305_handle_t *lcd, const char *title, uint32_t *val)
{
    cheat_flush_input();
    int cr = 0, cc = 0;
    bool up_prev = false, dn_prev = false, lf_prev = false, rt_prev = false;
    bool ok_prev = false, bk_prev = false;
    for (;;) {
        bool conn = bt_manager_is_connected();
        /* 触摸(无手柄)高亮: 手指按下的那一格才加黑 */
        bool touch_hl = false; int tr = 0, tc = 0;
        if (!conn) {
            int tx, ty;
            if (input_get_touch_pos(&tx, &ty)) {
                if (tx >= KP_X0 && tx < KP_X0 + KP_GRID_W &&
                    ty >= KP_TOP && ty < KP_TOP + KP_GRID_H) {
                    touch_hl = true;
                    tr = (ty - KP_TOP) / KP_CELL_H;
                    tc = (tx - KP_X0) / KP_CELL_W;
                    if (tr < 0) tr = 0;
                    if (tr >= KP_ROWS) tr = KP_ROWS - 1;
                    if (tc < 0) tc = 0;
                    if (tc >= KP_COLS) tc = KP_COLS - 1;
                }
            }
        }
        int hr = conn ? cr : tr, hc = conn ? cc : tc;
        bool hl_on = conn || touch_hl;

        if (lcd->fb) memcpy(lcd->fb, s_cheat_bg, FB_BYTES);
        fill_rect(lcd, KP_WIN_X0, KP_WIN_Y0, KP_WIN_X1, KP_WIN_Y1, ST7305_COLOR_WHITE);
        for (int k = 0; k < 2; k++) {
            draw_hline(lcd, KP_WIN_X0 + k, KP_WIN_X1 - k, KP_WIN_Y0 + k, ST7305_COLOR_BLACK);
            draw_hline(lcd, KP_WIN_X0 + k, KP_WIN_X1 - k, KP_WIN_Y1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, KP_WIN_X0 + k, KP_WIN_Y0 + k, KP_WIN_Y1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, KP_WIN_X1 - k, KP_WIN_Y0 + k, KP_WIN_Y1 - k, ST7305_COLOR_BLACK);
        }
        if (title && title[0])
            draw_text_centered(lcd, KP_WIN_Y0 + 6, title, false);
        char valbuf[16];
        snprintf(valbuf, sizeof(valbuf), "%lu", (unsigned long)(*val));
        int vx = KP_WIN_X0 + (KP_WIN_W - text_width(valbuf)) / 2;
        draw_text(lcd, vx, KP_WIN_Y0 + 32, valbuf, false);   /* 数值正常显示, 不反色 */
        draw_hline(lcd, KP_WIN_X0 + 6, KP_WIN_X1 - 6, KP_WIN_Y0 + 58, ST7305_COLOR_BLACK);

        /* 整块棋盘: 白色底 + 外框 + 内部分割线 */
        int gx0 = KP_X0, gy0 = KP_TOP, gx1 = gx0 + KP_GRID_W - 1, gy1 = gy0 + KP_GRID_H - 1;
        fill_rect(lcd, gx0, gy0, gx1, gy1, ST7305_COLOR_WHITE);
        draw_rect_outline(lcd, gx0, gy0, gx1, gy1, ST7305_COLOR_BLACK);
        for (int c = 1; c < KP_COLS; c++)
            draw_vline(lcd, gx0 + c * KP_CELL_W - 1, gy0, gy1, ST7305_COLOR_BLACK);
        for (int r = 1; r < KP_ROWS; r++)
            draw_hline(lcd, gx0, gx1, gy0 + r * KP_CELL_H - 1, ST7305_COLOR_BLACK);
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                char d[2] = { (char)('1' + r * 3 + c), 0 };
                kp_draw_cell(lcd, r, c, d, hl_on && hr == r && hc == c);
            }
        }
        kp_draw_cell(lcd, 3, 0, "\xe5\x88\xa0\xe9\x99\xa4", hl_on && hr == 3 && hc == 0); /* 删除 */
        kp_draw_cell(lcd, 3, 1, "0",                              hl_on && hr == 3 && hc == 1);
        kp_draw_cell(lcd, 3, 2, "\xe7\xa1\xae\xe5\xae\x9a", hl_on && hr == 3 && hc == 2); /* 确定 */
        st7305_flush(lcd);

        menu_action_t act = input_get_action();
        if (act == MENU_ACTION_BACK || act == MENU_ACTION_HOME) return 1;
        int tx, ty;
        bool tap = (act == MENU_ACTION_CONFIRM) && input_consume_tap(&tx, &ty);
        if (tap) {
            if (tx >= KP_X0 && tx < KP_X0 + KP_GRID_W &&
                ty >= KP_TOP && ty < KP_TOP + KP_GRID_H) {
                int r = (ty - KP_TOP) / KP_CELL_H;
                int c = (tx - KP_X0) / KP_CELL_W;
                if (r >= 0 && r < KP_ROWS && c >= 0 && c < KP_COLS) {
                    if (r == 3) {
                        if (c == 0) *val /= 10;                                   /* 删除 */
                        else if (c == 1) *val = (*val * 10 > 65535) ? *val : *val * 10; /* 0 */
                        else return 0;                                             /* 确定 */
                    } else {
                        uint32_t d = (uint32_t)(r * 3 + c + 1);
                        *val = (*val * 10 + d > 65535) ? *val : *val * 10 + d;
                    }
                    cui_touch_vib();
                }
            }
            continue;   /* 空白(网格外)触碰: 无动作、不震动 */
        }
        if (conn) {
            bool up = bt_manager_is_key_pressed(F_UP);
            bool dn = bt_manager_is_key_pressed(F_DOWN);
            bool lf = bt_manager_is_key_pressed(F_LEFT);
            bool rt = bt_manager_is_key_pressed(F_RIGHT);
            bool ok = bt_manager_is_key_pressed(F_CONFIRM);
            bool bk = bt_manager_is_key_pressed(F_BACK);
            if (up && !up_prev && cr > 0) cr--;
            if (dn && !dn_prev && cr < KP_ROWS - 1) cr++;
            if (lf && !lf_prev && cc > 0) cc--;
            if (rt && !rt_prev && cc < KP_COLS - 1) cc++;
            if (ok && !ok_prev) {
                if (cr == 3) {
                    if (cc == 0) *val /= 10;
                    else if (cc == 1) *val = (*val * 10 > 65535) ? *val : *val * 10;
                    else return 0;
                } else {
                    uint32_t d = (uint32_t)(cr * 3 + cc + 1);
                    *val = (*val * 10 + d > 65535) ? *val : *val * 10 + d;
                }
            }
            if (bk && !bk_prev) return 1;
            up_prev = up; dn_prev = dn; lf_prev = lf; rt_prev = rt; ok_prev = ok; bk_prev = bk;
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* 等待任意键/触摸 或 超时(ms) 后返回 (提示"找到 N 个"等停留 → 自动关闭).
 * 参照开源作弊器的"toast 自动消失"惯例: 即使某个键/触摸未被识别, 超时也必定关闭,
 * 杜绝"小弹窗卡住只能按硬件返回"的问题. */
static void cheat_wait_any_key(uint32_t ms)
{
    bool conn = bt_manager_is_connected();
    bool any_prev = conn && (bt_manager_is_key_pressed(F_CONFIRM) ||
                             bt_manager_is_key_pressed(F_BACK) ||
                             bt_manager_is_key_pressed(F_UP));
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < ms) {
        menu_action_t act = input_get_action();
        if (act != MENU_ACTION_NONE) return;
        bool any = conn && (bt_manager_is_key_pressed(F_CONFIRM) ||
                            bt_manager_is_key_pressed(F_BACK) ||
                            bt_manager_is_key_pressed(F_UP) ||
                            bt_manager_is_key_pressed(F_DOWN));
        if (any && !any_prev) return;
        any_prev = any;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* ================= 列表数据源 (候选 / 锁定 共用一套窗口) ================= */
typedef struct {
    int      count;
    bool     lock_mode;                  /* true = 锁定表, false = 候选列表 */
    uint16_t (*addr)(int);
    uint32_t (*value)(int);
    uint8_t  (*width)(int);
} cheat_listsrc_t;

static void cheat_fmt_row(char *buf, size_t n, int idx, const cheat_listsrc_t *src)
{
    uint16_t a = src->addr(idx);
    uint32_t v = src->value(idx);
    uint8_t  w = src->width(idx);
    bool mark = src->lock_mode ? true : cheater_cand_locked(idx);
    snprintf(buf, n, "0x%04X %c %lu%s",
             (unsigned)a, (w == (uint8_t)CHEAT_BITS_16) ? 'W' : 'B',
             (unsigned long)v, mark ? " *" : "");
}

/* ---- 锁定表适配: 界面按 0..count-1 连续下标, 内部换算到锁定槽 ---- */
static uint16_t lk_addr(int i)  { int l = cheater_lock_nth(i); return l < 0 ? 0 : cheater_lock_addr(l); }
static uint32_t lk_value(int i) { int l = cheater_lock_nth(i); return l < 0 ? 0 : cheater_lock_value(l); }
static uint8_t  lk_width(int i) { int l = cheater_lock_nth(i); return l < 0 ? 8 : cheater_lock_width(l); }

/* 行操作: 返回 true = 列表条数变了(需重算 content) */
static bool cheat_row_actions(st7305_handle_t *lcd, int idx, bool lock_mode)
{
    int      l  = lock_mode ? cheater_lock_nth(idx) : -1;
    uint32_t cur = lock_mode ? (l < 0 ? 0 : cheater_lock_value(l)) : cheater_read(idx);
    bool locked  = lock_mode ? (l >= 0) : cheater_cand_locked(idx);

    const char *its[3];
    int n = 0;
    its[n++] = "\xe6\x94\xb9\xe5\x80\xbc";                 /* 改值 */
    its[n++] = locked ? "\xe8\xa7\xa3\xe9\x94\x81"         /* 解锁 */
                      : "\xe9\x94\x81\xe5\xae\x9a";        /* 锁定 */
    if (!lock_mode) its[n++] = "\xe5\x88\xa0\xe9\x99\xa4"; /* 删除 */

    int r = cheat_menu_loop(lcd, its, n, n);
    if (r == 0) {
        uint32_t v = cur;
        if (cheat_value_edit(lcd, "\xe6\x96\xb0\xe5\x80\xbc", &v) == 0) {
            if (lock_mode) {
                if (l >= 0) cheater_lock_set_value(l, v);
            } else {
                cheater_write(idx, v);
                int cl = cheater_cand_lock_index(idx);
                if (cl >= 0) cheater_lock_set_value(cl, v);   /* 已锁的同步锁定值 */
            }
        }
        return false;
    }
    if (r == 1) {
        if (locked) {
            if (lock_mode) { cheater_unlock(l); return true; }
            cheater_cand_unlock(idx);
        } else {
            if (cheater_lock(idx) < 0) {
                cheat_center_msg(lcd, "\xe9\x94\x81\xe5\xae\x9a\xe6\xbb\xa1");  /* 锁定满 */
                cheat_wait_any_key(800);
            }
        }
        return false;
    }
    if (r == 2 && !lock_mode) {
        cheater_remove(idx);
        return true;
    }
    return false;
}

/* 通用列表窗口: ui_scroll 跟手滚动 + 行点选操作
 * 返回 true = 有删除发生 (调用方需重算候选数) */
static bool cheat_list_window(st7305_handle_t *lcd, const cheat_listsrc_t *src)
{
    static ui_scroll_t sc;
    static bool sc_rdy = false;

    const int win_w   = 300;
    const int win_x0  = (ST7305_WIDTH - win_w) / 2, win_x1 = win_x0 + win_w - 1;
    const int row_h   = 28, foot_h = CUI_ROW_H, hdr_h = 34;
    int win_h = hdr_h + 5 * row_h + foot_h + 8;
    if (win_h > 292) win_h = 292;
    const int win_y0 = (ST7305_HEIGHT - win_h) / 2, win_y1 = win_y0 + win_h - 1;
    const int content_y0 = win_y0 + hdr_h;
    const int footer_y   = win_y1 - foot_h - 2;
    const int vis_h      = footer_y - 2 - content_y0;

    int cnt = src->count;
    if (!sc_rdy) {
        ui_scroll_config(&sc, UI_SCROLL_LIST, 0);
        sc_rdy = true;
    }
    ui_scroll_init(&sc, row_h, vis_h, cnt * row_h);
    ui_scroll_to_top(&sc);

    cheat_flush_input();
    int sel = 0;
    bool changed = false;
    bool conn = bt_manager_is_connected();
    bool up_prev = conn && bt_manager_is_key_pressed(F_UP);
    bool dn_prev = conn && bt_manager_is_key_pressed(F_DOWN);
    bool ok_prev = conn && bt_manager_is_key_pressed(F_CONFIRM);
    bool bk_prev = conn && bt_manager_is_key_pressed(F_BACK);

    for (;;) {
        /* ---- 渲染 ---- */
        if (lcd->fb) memcpy(lcd->fb, s_cheat_bg, FB_BYTES);
        fill_rect(lcd, win_x0, win_y0, win_x1, win_y1, ST7305_COLOR_WHITE);
        for (int k = 0; k < 2; k++) {
            draw_hline(lcd, win_x0 + k, win_x1 - k, win_y0 + k, ST7305_COLOR_BLACK);
            draw_hline(lcd, win_x0 + k, win_x1 - k, win_y1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, win_x0 + k, win_y0 + k, win_y1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, win_x1 - k, win_y0 + k, win_y1 - k, ST7305_COLOR_BLACK);
        }
        {
            char hdr[48];
            if (src->lock_mode) {
                snprintf(hdr, sizeof(hdr), "\xe9\x94\x81\xe5\xae\x9a %d \xe4\xb8\xaa", cnt);
            } else {
                snprintf(hdr, sizeof(hdr),
                         "\xe5\x80\x99\xe9\x80\x89 %d \xc2\xb7 \xe7\xac\xac%d\xe8\xbd\xae",
                         cnt, cheater_round());
            }
            draw_text_centered(lcd, win_y0 + 5, hdr, false);
        }
        draw_hline(lcd, win_x0 + 6, win_x1 - 6, content_y0 - 1, ST7305_COLOR_BLACK);

        int roff = ui_scroll_offset(&sc);
        if (roff < 0) roff = 0;
        int first = roff / row_h;
        int skip  = roff % row_h;
        int rx0 = win_x0 + 6, rx1 = win_x1 - 8;

        st7305_set_clip_y(lcd, content_y0, footer_y - 2);
        for (int i = 0; ; i++) {
            int idx = first + i;
            if (idx >= cnt) break;
            int ry = content_y0 - skip + i * row_h;
            if (ry >= footer_y - 2) break;
            char rb[40];
            cheat_fmt_row(rb, sizeof(rb), idx, src);
            if (idx == sel)
                fill_rect(lcd, rx0, ry, rx1 - 1, ry + row_h - 1, ST7305_COLOR_BLACK);
            draw_text(lcd, rx0 + 6, ry + (row_h - 24) / 2, rb, idx == sel);
        }
        st7305_reset_clip_y(lcd);

        if (cnt > 0 && vis_h > 4)
            ui_scroll_drawbar(&sc, lcd, win_x1 - 3, content_y0, footer_y - 3);

        draw_hline(lcd, win_x0 + 6, win_x1 - 6, footer_y - 1, ST7305_COLOR_BLACK);
        cheat_draw_row(lcd, win_x0, win_y0, win_w, footer_y, CUI_FOOT, false);
        st7305_flush(lcd);

        /* ---- 触摸: 跟手滚动 (只喂内容区; 拖出区外继续跟手) ---- */
        int tx, ty;
        bool down = input_get_touch_pos(&tx, &ty);
        bool in_area = down &&
                       tx >= win_x0 && tx <= win_x1 &&
                       ty >= content_y0 && ty < footer_y - 2;
        bool feed = down && (in_area || sc.pressed);
        ui_scroll_touch_xy(&sc, tx, ty, feed);

        /* ---- 输入 ---- */
        menu_action_t act = input_get_action();
        bool activate = false;

        if (act == MENU_ACTION_UP) {
            if (sel > 0) sel--;
        } else if (act == MENU_ACTION_DOWN) {
            if (sel < cnt - 1) sel++;
        } else if (act == MENU_ACTION_BACK) {
            ui_scroll_cancel(&sc);
            return changed;
        } else if (act == MENU_ACTION_CONFIRM) {
            int x2, y2;
            if (input_consume_tap(&x2, &y2)) {
                if (ui_scroll_dragging(&sc)) {
                    /* 刚拖过, 松手不当作点击 */
                } else if (x2 < win_x0 || x2 > win_x1 || y2 < win_y0 ||
                           y2 > win_y1 || y2 >= footer_y) {
                    ui_scroll_cancel(&sc);      /* 窗外 / 返回键 */
                    return changed;
                } else if (y2 >= content_y0) {
                    int rr = (y2 - content_y0 + ui_scroll_offset(&sc)) / row_h;
                    if (rr >= 0 && rr < cnt) { sel = rr; activate = true; }
                }
            } else if (cnt > 0 && sel < cnt) {
                activate = true;                /* 手柄 CONFIRM */
            }
        }

        if (activate && cnt > 0 && sel < cnt) {
            cui_touch_vib();
            if (cheat_row_actions(lcd, sel, src->lock_mode)) {
                changed = true;
                cnt = src->lock_mode ? cheater_lock_count() : cheater_count();
                if (cnt <= 0) { ui_scroll_cancel(&sc); return changed; }
                if (sel >= cnt) sel = cnt - 1;
                ui_scroll_set_content(&sc, cnt * row_h);
            }
        }

        if (conn) {
            bool up = bt_manager_is_key_pressed(F_UP);
            bool dn = bt_manager_is_key_pressed(F_DOWN);
            bool ok = bt_manager_is_key_pressed(F_CONFIRM);
            bool bk = bt_manager_is_key_pressed(F_BACK);
            if (dn && !dn_prev) { if (sel < cnt - 1) sel++; }
            if (up && !up_prev) { if (sel > 0) sel--; }
            if (ok && !ok_prev && cnt > 0 && sel < cnt) {
                if (cheat_row_actions(lcd, sel, src->lock_mode)) {
                    changed = true;
                    cnt = src->lock_mode ? cheater_lock_count() : cheater_count();
                    if (cnt <= 0) { ui_scroll_cancel(&sc); return changed; }
                    if (sel >= cnt) sel = cnt - 1;
                    ui_scroll_set_content(&sc, cnt * row_h);
                }
            }
            if (bk && !bk_prev) { ui_scroll_cancel(&sc); return changed; }
            up_prev = up; dn_prev = dn; ok_prev = ok; bk_prev = bk;
        }

        /* 选中行跟随 + 推进惯性/吸附 */
        ui_scroll_ensure_visible(&sc, sel * row_h, row_h);
        ui_scroll_poll(&sc);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* ================= 主菜单 / 搜索 / 过滤 / 记录 / 锁定 ================= */
#define CUI_NEW  0
#define CUI_FILT 1
#define CUI_BROW 2
#define CUI_LOCK 3
#define CUI_BACK 4

/* 主菜单 4 项, 标签带实时计数 (候选/锁定), 比纯文字更有信息量 */
static int cheat_menu(st7305_handle_t *lcd)
{
    static char mi[4][40];
    int cnt = cheater_count();
    int lk  = cheater_lock_count();

    snprintf(mi[0], sizeof(mi[0]), "\xe6\x90\x9c\xe7\xb4\xa2");   /* 搜索 */
    if (cnt >= 0)
        snprintf(mi[1], sizeof(mi[1]), "\xe8\xbf\x87\xe6\xbb\xa4 %d", cnt);   /* 过滤 N */
    else
        snprintf(mi[1], sizeof(mi[1]), "\xe8\xbf\x87\xe6\xbb\xa4");          /* 过滤 */
    if (cnt > 0)
        snprintf(mi[2], sizeof(mi[2]), "\xe8\xae\xb0\xe5\xbd\x95 %d", cnt);   /* 记录 N */
    else
        snprintf(mi[2], sizeof(mi[2]), "\xe8\xae\xb0\xe5\xbd\x95");          /* 记录 */
    if (lk > 0)
        snprintf(mi[3], sizeof(mi[3]), "\xe9\x94\x81\xe5\xae\x9a %d", lk);    /* 锁定 N */
    else
        snprintf(mi[3], sizeof(mi[3]), "\xe9\x94\x81\xe5\xae\x9a");          /* 锁定 */

    const char *items[4] = { mi[0], mi[1], mi[2], mi[3] };
    return cheat_menu_loop(lcd, items, 4, CUI_BACK);
}

static void cheat_new_search(st7305_handle_t *lcd)
{
    uint32_t val = 0;
    if (cheat_value_edit(lcd, "\xe8\xbe\x93\xe5\x85\xa5\xe6\x95\xb0\xe5\x80\xbc", /* 输入数值 */
                         &val) != 0) return;
    cheater_begin(&s_reg, s_bits);
    cheat_center_msg(lcd, "\xe6\x90\x9c\xe7\xb4\xa2\xe4\xb8\xad\xe2\x80\xa6"); /* 搜索中… */
    int n = cheater_first_scan(val);
    ESP_LOGI(CUI_TAG, "first_scan val=%lu -> %d 个, scan=%dms",
             (unsigned long)val, n, cheater_scan_ms());
    char msg[56];
    if (n < 0) {
        snprintf(msg, sizeof(msg), "\xe6\x9c\xaa\xe7\x99\xbb\xe8\xae\xb0\xe5\x8c\xba\xe5\x9f\x9f"); /* 未登记区域 */
    } else {
        snprintf(msg, sizeof(msg), "\xe6\x89\xbe\xe5\x88\xb0 %d \xe4\xb8\xaa %dms",
                 n, cheater_scan_ms());   /* 找到 N 个 Xms */
    }
    cheat_center_msg(lcd, msg);
    cheat_wait_any_key(1000);
}

static void cheat_filter(st7305_handle_t *lcd)
{
    if (cheater_count() < 0) {
        cheat_center_msg(lcd, "\xe8\xaf\xb7\xe5\x85\x88\xe6\x90\x9c\xe7\xb4\xa2"); /* 请先搜索 */
        vTaskDelay(pdMS_TO_TICKS(700));
        return;
    }
    /* 顺序须与 cheater_op_t 一致: EQ/GT/LT/CHANGED/UNCHANGED */
    static const char *base[5] = {
        "\xe7\xad\x89\xe4\xba\x8e\xe2\x80\xa6",         /* 等于… */
        "\xe5\x8f\x98\xe5\xa4\xa7",                     /* 变大 */
        "\xe5\x8f\x98\xe5\xb0\x8f",                     /* 变小 */
        "\xe6\x9c\x89\xe5\x8f\x98\xe5\x8c\x96",         /* 有变化 */
        "\xe6\x97\xa0\xe5\x8f\x98\xe5\x8c\x96",         /* 无变化 */
    };
    const char *items[6];
    int n = 0;
    for (int i = 0; i < 5; i++) items[n++] = base[i];
    bool can_undo = cheater_can_undo();
    if (can_undo) items[n++] = "\xe6\x92\xa4\xe9\x94\x80";   /* 撤销 */

    int sel = cheat_menu_loop(lcd, items, n, n);
    if (sel < 0 || sel >= n) return;

    if (sel == 5) {                    /* 撤销上一步 */
        int back = cheater_undo();
        char msg[48];
        if (back < 0) snprintf(msg, sizeof(msg), "\xe6\x97\xa0\xe5\x8f\xaf\xe6\x92\xa4\xe9\x94\x80"); /* 无可撤销 */
        else          snprintf(msg, sizeof(msg), "\xe5\x89\xa9 %d \xe4\xb8\xaa", back);              /* 剩 N 个 */
        cheat_center_msg(lcd, msg);
        cheat_wait_any_key(900);
        return;
    }
    if (cheater_count() <= 0) return;

    uint32_t eqval = 0;
    int out;
    if (sel == 0) {   /* 等于: 需输数值 */
        if (cheat_value_edit(lcd, "\xe7\xad\x89\xe4\xba\x8e\xe6\x95\xb0\xe5\x80\xbc", /* 等于数值 */
                             &eqval) != 0) return;
        out = cheater_filter(CHEAT_EQ, eqval);
    } else {           /* sel 1..4 直接映射 变大/变小/有变/无变 */
        out = cheater_filter((cheater_op_t)sel, 0);
    }
    char msg[48];
    if (out < 0) snprintf(msg, sizeof(msg), "\xe9\x94\x99\xe8\xaf\xaf");  /* 错误 */
    else         snprintf(msg, sizeof(msg), "\xe5\x89\xa9 %d \xe4\xb8\xaa", out); /* 剩 N 个 */
    cheat_center_msg(lcd, msg);
    cheat_wait_any_key(900);
}

static void cheat_browse(st7305_handle_t *lcd)
{
    int cnt = cheater_count();
    if (cnt < 0) {
        cheat_center_msg(lcd, "\xe8\xaf\xb7\xe5\x85\x88\xe6\x90\x9c\xe7\xb4\xa2"); /* 请先搜索 */
        vTaskDelay(pdMS_TO_TICKS(700));
        return;
    }
    if (cnt == 0) {
        cheat_center_msg(lcd, "\xe6\x9a\x82\xe6\x97\xa0\xe5\x80\x99\xe9\x80\x89"); /* 暂无候选 */
        vTaskDelay(pdMS_TO_TICKS(700));
        return;
    }
    cheat_listsrc_t src = {
        .count = cnt,
        .lock_mode = false,
        .addr  = cheater_addr,
        .value = cheater_read,
        .width = cheater_width,
    };
    cheat_list_window(lcd, &src);
}

static void cheat_locks(st7305_handle_t *lcd)
{
    int cnt = cheater_lock_count();
    if (cnt <= 0) {
        cheat_center_msg(lcd, "\xe6\xb2\xa1\xe6\x9c\x89\xe9\x94\x81\xe5\xae\x9a\xe9\xa1\xb9"); /* 没有锁定项 */
        vTaskDelay(pdMS_TO_TICKS(700));
        return;
    }
    cheat_listsrc_t src = {
        .count     = cnt,
        .lock_mode = true,
        .addr      = lk_addr,
        .value     = lk_value,
        .width     = lk_width,
    };
    cheat_list_window(lcd, &src);
}

/* ================= 主流程 (阻塞) ================= */
void cheat_ui_open(st7305_handle_t *lcd, const cheater_region_t *reg, cheater_bits_t bits)
{
    if (!lcd) return;
    memset(&s_reg, 0, sizeof(s_reg));
    if (reg) s_reg = *reg;
    s_bits = bits;

    /* 等待进入瞬间 SELECT/START 释放, 防连触发 */
    for (int i = 0; i < 15; i++) {
        if (!bt_manager_is_key_pressed(F_SELECT) && !bt_manager_is_key_pressed(F_START)) break;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    /* 快照冻结帧: 各界面"回填此帧再画二级弹窗窗口" */
    if (lcd->fb) memcpy(s_cheat_bg, lcd->fb, FB_BYTES);

    /* 触摸(无手柄)模式: 屏蔽通用"按下即震", 让空白/无动作触碰静默,
     * 仅当真正命中可操作键/选项时由 cui_touch_vib() 补一次反馈. */
    bool conn0 = bt_manager_is_connected();
    if (!conn0) input_set_key_sim_ctx_block(true);

    for (;;) {
        int r = cheat_menu(lcd);
        if (r == CUI_NEW)       cheat_new_search(lcd);
        else if (r == CUI_FILT) cheat_filter(lcd);
        else if (r == CUI_BROW) cheat_browse(lcd);
        else if (r == CUI_LOCK) cheat_locks(lcd);
        else break;   /* 返回 */
    }
    input_set_key_sim_ctx_block(false);
    cheat_center_msg(lcd, "\xe7\xbb\xa7\xe7\xbb\xad\xe6\xb8\xb8\xe6\x88\x8f"); /* 继续游戏 */
}
