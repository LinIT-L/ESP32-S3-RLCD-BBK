/**
 * svc_statusbar.c — 全局服务: 状态栏.
 *
 * 从 menu_system.c 的 menu_draw_status_bar 迁移. 视觉与旧版完全一致:
 * 布局: 最左 日期"MM-DD 星期X" | 正中间 时间"HH:MM" | 右侧 电池->手柄/蓝牙->喇叭音量->数据传输->WiFi
 *
 * V1.5.x: 反白开关 (s_inv) — 默认普通(黑字白底); 网络探测等暗色科幻页通过
 * os_statusbar_set_invert(true) 仅在本页把状态栏整条反白(白字白图 on 黑底), 退出还原.
 * 解耦: 本服务不引用 menu/bt_manager/wifi_manager 等实现,
 * 系统状态(电池/音量/蓝牙/WiFi/USB) 通过 os_platform_set() 注入的 getter 读取.
 */
#include "os.h"
#include "ui_common.h"
#include "status_icons12.inc"
#include "usb_bt.h"
#include "usb_net.h"
#include "usb_hid.h"
#include "esp_timer.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

/* 当前平台状态挂钩 (由 main.c 注入) */
static const os_platform_ops_t *s_ops = NULL;

void os_platform_set(const os_platform_ops_t *ops)
{
    s_ops = ops;
}

/* ============ 状态栏绘制 ============ */

#define SB_STRIP_H 24        /* 状态栏条高度 */
static bool s_inv = false;   /* 反白模式 (本页暗色: 白字白图 on 黑底) */

void os_statusbar_set_invert(bool v)
{
    s_inv = v;
}

/* 1bpp 反白直写: 先填该格黑底, 再在 bit 置位处画白 (白图) */
static void svc_blit_inv(st7305_handle_t *lcd, int x, int y, int w, int h,
                         int bytes_per_row, const uint8_t *bitmap)
{
    fill_rect(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_BLACK);
    for (int dy = 0; dy < h; dy++) {
        const uint8_t *row = bitmap + dy * bytes_per_row;
        for (int dx = 0; dx < w; dx++) {
            if (row[dx >> 3] & (0x80u >> (dx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
        }
    }
}

/* 电池位图 (7档: 电池0=空 .. 电池满=100%). 255=未检测到 -> 空心框+"?" */
static void svc_draw_battery(st7305_handle_t *lcd, int x, int y, uint8_t percent) {
    if (percent == 255) {
        if (s_inv) {
            draw_rect_outline(lcd, x, y, x + BAT_ICON_W - 1, y + BAT_ICON_H - 1, ST7305_COLOR_WHITE);
            fill_rect(lcd, x + 2, y + 2, x + BAT_ICON_W - 3, y + BAT_ICON_H - 3, ST7305_COLOR_BLACK);
            draw_ascii_sb(lcd, x + (BAT_ICON_W - 8) / 2, y + 1, '?', true);
        } else {
            draw_rect_outline(lcd, x, y, x + BAT_ICON_W - 1, y + BAT_ICON_H - 1, ST7305_COLOR_BLACK);
            fill_rect(lcd, x + 2, y + 2, x + BAT_ICON_W - 3, y + BAT_ICON_H - 3, ST7305_COLOR_WHITE);
            draw_ascii_sb(lcd, x + (BAT_ICON_W - 8) / 2, y, '?', false);
        }
        return;
    }
    int idx = (int)(percent * BAT_ICON_N / 100);
    if (idx >= BAT_ICON_N) idx = BAT_ICON_N - 1;
    if (s_inv) svc_blit_inv(lcd, x, y, BAT_ICON_W, BAT_ICON_H, (BAT_ICON_W + 7) / 8, bat_icon[idx]);
    else       st7305_draw_bitmap_1bit(lcd, x, y, BAT_ICON_W, BAT_ICON_H, bat_icon[idx]);
}

/* 1bpp 位图反白最近邻放大 (见 s_inv: 白图或黑图) */
#define STATUS_ICON_W   32
#define STATUS_ICON_H   16
static void svc_draw_status_icon(st7305_handle_t *lcd, int x, int y,
                                 int srcW, int srcH, int bytes_per_row,
                                 int tw, int th, const uint8_t *bitmap) {
    if (!lcd || !bitmap || srcW <= 0 || srcH <= 0 || tw <= 0 || th <= 0) return;
    st7305_color_t px = s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    for (int dy = 0; dy < th; dy++) {
        int sy = (dy * srcH) / th;
        for (int dx = 0; dx < tw; dx++) {
            int sx = (dx * srcW) / tw;
            uint8_t byte = bitmap[sy * bytes_per_row + (sx >> 3)];
            if (byte & (0x80u >> (sx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, px);
        }
    }
}

/* 每帧驱动的内部状态: 电量采样节流 + 分钟变化触发重绘 */
static uint32_t s_last_bat_ms = 0;
static uint8_t  s_bat         = 255;
static int      s_last_minute = -1;

static void svc_tick(ui_ctx_t *ctx)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_ops) return;

    if (s_last_bat_ms == 0 || now - s_last_bat_ms >= 60000) {   /* 开机立即采一次, 之后每 60 秒 */
        s_last_bat_ms = now;
        uint8_t b = s_ops->get_battery ? s_ops->get_battery() : 255;
        if (b != s_bat) {
            s_bat = b;
            ctx->needs_redraw = true;
        }
    }
    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);
    if (t && t->tm_min != s_last_minute) {
        s_last_minute = t->tm_min;
        ctx->needs_redraw = true;
    }
}

static const char *s_wd_cn[] = {"\xe6\x97\xa5","\xe4\xb8\x80","\xe4\xba\x8c","\xe4\xb8\x89",
                                "\xe5\x9b\x9b","\xe4\xba\x94","\xe5\x85\xad"};  /* 日/一/二/三/四/五/六 */

static void svc_draw_common_left(st7305_handle_t *lcd, int text_y)
{
    const int M = 4;
    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d-%02d", t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0);

    int x = M;
    ui_text16(lcd, x, text_y, date_str, s_inv);            /* 数字: 标准16px(英8×16) */
    x += ui_text16w(date_str) + 2;
    ui_text16(lcd, x, text_y, "\xe6\x98\x9f\xe6\x9c\x9f", s_inv);          /* 星期 */
    x += ui_text16w("\xe6\x98\x9f\xe6\x9c\x9f");
    ui_text16(lcd, x, text_y, s_wd_cn[t ? t->tm_wday : 0], s_inv);          /* 日/一/... */
}

static void svc_draw_common_time(st7305_handle_t *lcd, int text_y, int SCREEN_W)
{
    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t ? t->tm_hour : 0, t ? t->tm_min : 0);
    ui_text16c(lcd, SCREEN_W / 2, text_y, time_str, s_inv);   /* 时间: 标准16px(英8×16)居中 */
}

static void svc_draw_common_right(st7305_handle_t *lcd, int text_y, int SCREEN_W,
                                  uint8_t bat, bool pad_connected, int *out_right_x)
{
    const int M = 4;
    const int GAP = 4;
    int right_x = SCREEN_W - M;
    svc_draw_battery(lcd, right_x - BAT_ICON_W, text_y, bat);
    right_x -= BAT_ICON_W + GAP;
    if (pad_connected) {
        svc_draw_status_icon(lcd, right_x - STATUS_ICON_W, text_y,
                             PAD_ICON_W, PAD_ICON_H, (PAD_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, pad12_icon);
        right_x -= STATUS_ICON_W + GAP;
    }
    if (out_right_x) *out_right_x = right_x;
}

static void svc_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    const int text_y = 3;
    const int M  = 4;
    const int GAP = 4;
    const int SCREEN_W = ui_screen_w();

    if (s_inv) fill_rect(lcd, 0, 0, SCREEN_W - 1, SB_STRIP_H - 1, ST7305_COLOR_BLACK);

    svc_draw_common_left(lcd, text_y);
    svc_draw_common_time(lcd, text_y, SCREEN_W);
    int right_x;
    svc_draw_common_right(lcd, text_y, SCREEN_W, s_bat,
                          s_ops && s_ops->bt_connected && s_ops->bt_connected(),
                          &right_x);

    bool data_active = s_ops && s_ops->usb_data_active && s_ops->usb_data_active();
    if (data_active && !usb_bt_is_running()) {
        svc_draw_status_icon(lcd, right_x - STATUS_ICON_W, text_y,
                             DATA_ICON_W, DATA_ICON_H, (DATA_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, data12_icon);
        right_x -= STATUS_ICON_W + GAP;
    }
    bool wifi_on = s_ops && s_ops->wifi_enabled && s_ops->wifi_enabled();
    bool wifi_conn = s_ops && s_ops->wifi_connected && s_ops->wifi_connected();
    if (wifi_on && wifi_conn) {
        svc_draw_status_icon(lcd, right_x - STATUS_ICON_W, text_y,
                             WIFI_ICON_W, WIFI_ICON_H, (WIFI_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, wifi12_icon);
        right_x -= STATUS_ICON_W + GAP;
    }
    if (usb_bt_is_running()) {
        const char *s = "BLE";
        const int n = 3;
        const int tw = n * 8;
        int tx = right_x - tw;
        for (int i = 0; i < n; i++)
            draw_ascii_small(lcd, tx + i * 8, 5, s[i], s_inv);
        draw_rect_outline(lcd, tx - 1, 3, tx + tw, 5 + 12, s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
        right_x -= tw + 2 + GAP;
    }
    if (os_ap_active() || (usb_net_is_running() && usb_composite_is_connected())) {
        const char *s = "NET";
        const int n = 3;
        const int tw = n * 8;
        int tx = right_x - tw;
        for (int i = 0; i < n; i++)
            draw_ascii_small(lcd, tx + i * 8, 5, s[i], s_inv);
        draw_rect_outline(lcd, tx - 1, 3, tx + tw, 5 + 12, s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
        right_x -= tw + 2 + GAP;
    }
}

static const os_service_t s_svc_statusbar = {
    .name   = "statusbar",
    .tick   = svc_tick,
    .render = svc_render,
};

void os_svc_statusbar_register(void)
{
    os_register_service(&s_svc_statusbar);
}

/* 游戏内状态栏 — 与桌面共用同一套反白/普通绘制 */
void svc_statusbar_draw_game(st7305_handle_t *lcd, uint8_t battery, bool pad_connected) {
    if (!lcd) return;
    const int text_y = 3;
    const int SCREEN_W = UI_SCREEN_W;
    if (s_inv) fill_rect(lcd, 0, 0, SCREEN_W - 1, SB_STRIP_H - 1, ST7305_COLOR_BLACK);
    svc_draw_common_left(lcd, text_y);
    svc_draw_common_time(lcd, text_y, SCREEN_W);
    svc_draw_common_right(lcd, text_y, SCREEN_W, battery, pad_connected, NULL);
}