/**
 * svc_statusbar.c — 全局服务: 状态栏.
 *
 * 从 menu_system.c 的 menu_draw_status_bar 迁移. 视觉与旧版完全一致:
 *   布局: 最左 日期"MM-DD 星期X" | 正中间 时间"HH:MM" | 右侧 电池->手柄/蓝牙->喇叭音量->数据传输->WiFi
 *   高度: 24px, 内容带 16px.
 *
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

/* ============ 状态栏绘制 (与旧 menu_draw_status_bar 同语义) ============ */

/* 电池位图 (7档: 电池0=空 .. 电池满=100%). 255=未检测到 -> 空心框+"?" */
static void svc_draw_battery(st7305_handle_t *lcd, int x, int y, uint8_t percent) {
    if (percent == 255) {
        draw_rect_outline(lcd, x, y, x + BAT_ICON_W - 1, y + BAT_ICON_H - 1, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 2, y + 2, x + BAT_ICON_W - 3, y + BAT_ICON_H - 3, ST7305_COLOR_WHITE);
        draw_ascii_sb(lcd, x + (BAT_ICON_W - 8) / 2, y, '?', false);
        return;
    }
    int idx = (int)(percent * BAT_ICON_N / 100);
    if (idx >= BAT_ICON_N) idx = BAT_ICON_N - 1;
    st7305_draw_bitmap_1bit(lcd, x, y, BAT_ICON_W, BAT_ICON_H, bat_icon[idx]);
}

/* 1bpp 位图最近邻放大 (与旧 draw_status_icon_stretch 一致) */
#define STATUS_ICON_W   32
#define STATUS_ICON_H   16
static void svc_draw_status_icon(st7305_handle_t *lcd, int x, int y,
                                 int srcW, int srcH, int bytes_per_row,
                                 int tw, int th, const uint8_t *bitmap) {
    if (!lcd || !bitmap || srcW <= 0 || srcH <= 0 || tw <= 0 || th <= 0) return;
    for (int dy = 0; dy < th; dy++) {
        int sy = (dy * srcH) / th;
        for (int dx = 0; dx < tw; dx++) {
            int sx = (dx * srcW) / tw;
            uint8_t byte = bitmap[sy * bytes_per_row + (sx >> 3)];
            if (byte & (0x80u >> (sx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_BLACK);
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

    /* 电量每 60s 采样一次 (避免频繁 ADC) */
    if (now - s_last_bat_ms >= 60000) {
        s_last_bat_ms = now;
        uint8_t b = s_ops->get_battery ? s_ops->get_battery() : 255;
        if (b != s_bat) {
            s_bat = b;
            ctx->needs_redraw = true;
        }
    }
    /* 分钟变化 -> 时间/日期更新, 触发重绘 */
    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);
    if (t && t->tm_min != s_last_minute) {
        s_last_minute = t->tm_min;
        ctx->needs_redraw = true;
    }
}

static void svc_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    const int sb_h = 24;
    const int sb_y = 0;
    const int M  = 4;
    const int GAP = 4;
    const int text_y = sb_y + 3;
    const int SCREEN_W = ui_screen_w();   /* 跟随屏幕宽度: 竖屏 300, 横屏 400 */
                                            /* 竖屏时经 st7305 绘图路由进逻辑 pfb, flush 随屏旋转 */

    time_t now_ts = time(NULL);
    struct tm *t = localtime(&now_ts);
    static const char *wd[] = {"\xe6\x97\xa5","\xe4\xb8\x80","\xe4\xba\x8c","\xe4\xb8\x89",
                               "\xe5\x9b\x9b","\xe4\xba\x94","\xe5\x85\xad"};  /* 日/一/二/三/四/五/六 */
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d-%02d", t->tm_mon + 1, t->tm_mday);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t->tm_hour, t->tm_min);

    /* 最左: 日期 "MM-DD" + 中文星期 "星期X" */
    int x = M;
    for (int i = 0; date_str[i]; i++) {
        draw_ascii_medium(lcd, x, text_y, date_str[i], false);
        x += 12;
    }
    x += 2;
    draw_zh_sb(lcd, x,       text_y + 1, "\xe6\x98\x9f", false);  /* 星 */
    draw_zh_sb(lcd, x + 16,  text_y + 1, "\xe6\x9c\x9f", false);  /* 期 */
    draw_zh_sb(lcd, x + 32,  text_y + 1, wd[t->tm_wday], false);  /* 日/一/.../六 */

    /* 正中间: 时间 "HH:MM" 水平居中 */
    int time_w = (int)strlen(time_str) * 12;
    int time_x = (SCREEN_W - time_w) / 2;
    for (int i = 0; time_str[i]; i++) {
        draw_ascii_medium(lcd, time_x, text_y, time_str[i], false);
        time_x += 12;
    }

    /* 右侧从右到左: 电池 -> 手柄/蓝牙 -> 喇叭音量 -> 数据传输 -> WiFi */
    int right_x = SCREEN_W - M;
    svc_draw_battery(lcd, right_x - BAT_ICON_W, text_y, s_bat);
    right_x -= BAT_ICON_W + GAP;

    bool bt_on  = s_ops && s_ops->bt_enabled  && s_ops->bt_enabled();
    bool bt_conn = s_ops && s_ops->bt_connected && s_ops->bt_connected();
    if (bt_on && bt_conn) {
        svc_draw_status_icon(lcd, right_x - STATUS_ICON_W, text_y,
                             PAD_ICON_W, PAD_ICON_H, (PAD_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, pad12_icon);
        right_x -= STATUS_ICON_W + GAP;
    }

    /* V1.1.1 用户需求: 状态栏默认不显示喇叭音量图标 */

    bool data_active = s_ops && s_ops->usb_data_active && s_ops->usb_data_active();
    /* USB-BT (蓝牙适配器) 运行时不显示数据传输图标: 该图标仅电脑认到串口(MSC/CDC)时显示 */
    if (data_active && !usb_bt_is_running()) {
        svc_draw_status_icon(lcd, right_x - STATUS_ICON_W, text_y,
                             DATA_ICON_W, DATA_ICON_H, (DATA_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, data12_icon);
        right_x -= STATUS_ICON_W + GAP;
    }

    bool wifi_on = s_ops && s_ops->wifi_enabled && s_ops->wifi_enabled();
    bool wifi_conn = s_ops && s_ops->wifi_connected && s_ops->wifi_connected();
    if (wifi_on && wifi_conn) {
        st7305_draw_bitmap_1bit(lcd, right_x - WIFI_ICON_W, text_y,
                                WIFI_ICON_W, WIFI_ICON_H, wifi12_icon);
        right_x -= WIFI_ICON_W + GAP;
    }

    /* USB-BT (蓝牙适配器) 运行时: 状态栏右侧画 "BLE" 小号瘦英文(8px) + 紧凑方框, 文本上下居中 */
    if (usb_bt_is_running()) {
        const char *s = "BLE";
        const int n = 3;
        const int tw = n * 8;            /* 3 个 ASCII × 8px 小号瘦英文 */
        int tx = right_x - tw;           /* 文本右对齐 */
        /* 先画文字 (glyph 高约 12px), 再画框: 框上下各留 1px 且不与文字行重叠, 实现上下居中 */
        for (int i = 0; i < n; i++)
            draw_ascii_small(lcd, tx + i * 8, 5, s[i], false);
        draw_rect_outline(lcd, tx - 1, 4, tx + tw, 5 + 12, ST7305_COLOR_BLACK);  /* 框: y=4..17 */
        right_x -= tw + 2 + GAP;
    }

    /* USB 网卡共享工作指示: 网卡模式运行 且 USB 被主机识别(enumerated/tud_mounted) 才显示.
     * 未插/未被识别则不显示, 方便确认设备是否真的在工作. */
    if (usb_net_is_running() && usb_composite_is_connected()) {
        const char *s = "NET";
        const int n = 3;
        const int tw = n * 8;
        int tx = right_x - tw;
        for (int i = 0; i < n; i++)
            draw_ascii_small(lcd, tx + i * 8, 5, s[i], false);
        draw_rect_outline(lcd, tx - 1, 4, tx + tw, 5 + 12, ST7305_COLOR_BLACK);
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

/* V1.1.1: 游戏内状态栏 (供引擎在游戏渲染循环自绘顶部条).
 * 轻量版: 居中时间 HH:MM + 右侧 电池/手柄. 从旧 menu_draw_status_bar 迁出精简,
 * 由引擎把电量/手柄状态作为参数传入 (不读全局菜单状态). */
void svc_statusbar_draw_game(st7305_handle_t *lcd, uint8_t battery, bool pad_connected) {
    if (!lcd) return;
    const int text_y = 3;
    const int M  = 4;
    const int GAP = 4;
    const int SCREEN_W = UI_SCREEN_W;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%02d:%02d", t ? t->tm_hour : 0, t ? t->tm_min : 0);
    int tw = (int)strlen(ts) * 12;
    int tx = (SCREEN_W - tw) / 2;
    for (int i = 0; ts[i]; i++) { draw_ascii_medium(lcd, tx, text_y, ts[i], false); tx += 12; }
    int rx = SCREEN_W - M;
    svc_draw_battery(lcd, rx - BAT_ICON_W, text_y, battery);
    rx -= BAT_ICON_W + GAP;
    if (pad_connected) {
        svc_draw_status_icon(lcd, rx - STATUS_ICON_W, text_y,
                             PAD_ICON_W, PAD_ICON_H, (PAD_ICON_W + 7) / 8,
                             STATUS_ICON_W, STATUS_ICON_H, pad12_icon);
    }
}
