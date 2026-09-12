/**
 * page_settings.c — 设置 页面模块.
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 4 项 (显示配置/声音调整/无线连接/系统信息).
 *   - 显示配置: 时间设置 + 字体风格 (粗/细)
 *   - 声音调整: 声音开关 / 音量设置 / 震动开关
 *   - 无线连接: 蓝牙开关 / WiFi 开关 (开后显示"连接WiFi") / 连接WiFi → WiFi 键盘页
 *   - 系统信息: CPU/Flash/RAM/电池等
 * 设置经 NVS 持久化 (namespace os_settings).
 */
#include "os.h"
#include "os_hw.h"
#include "st7305.h"       /* st7305_set_inversion: 暗黑模式(面板反色) */
#include "ui_common.h"
#include "wallpapers.h" /* wp_screensaver_set_delay_ms: 设置休眠时间后更新息屏延时 */
#include "input.h"       /* input_consume_tap / input_get_touch_pos (时间滚轮), esp_util.h */
#include "esp_attr.h"
#include "font_zh.h"
#include "audio_player.h"
#include "vibrator.h"
#include "wifi_manager.h"
#include "keyboard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "board_battery.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#define TAG "PSET"
#define SET_NS "os_settings"

/* 主菜单项 (UTF-8): 显示与时间/声音与反馈/休眠与电源/网络与系统/请作者喝杯饮料 (底部"返回"由 os_dialog 自动加) */
static const char *const s_set_items[5] = {
    "\xe6\x98\xbe\xe7\xa4\xba\xe4\xb8\x8e\xe6\x97\xb6\xe9\x97\xb4",  /* 显示与时间 */
    "\xe5\xa3\xb0\xe9\x9f\xb3\xe4\xb8\x8e\xe5\x8f\x8d\xe9\xa6\x88",  /* 声音与反馈 */
    "\xe4\xbc\x91\xe7\x9c\xa0\xe4\xb8\x8e\xe7\x94\xb5\xe6\xba\x90",  /* 休眠与电源 */
    "\xe7\xbd\x91\xe7\xbb\x9c\xe4\xb8\x8e\xe7\xb3\xbb\xe7\xbb\x9f",  /* 网络与系统 */
    "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe9\xa5\xae\xe6\x96\x99",  /* 请作者喝杯饮料 */
};

/* 震动开关状态 (vibrator 无 get, 自行跟踪 + NVS). V1.2.x: 拆为 UI / 游戏 两个独立通道 */
static bool s_vib_ui   = true;   /* UI 震动: 拖拉/菜单导航/设置调整 */
static bool s_vib_game = true;   /* 游戏震动: 游戏内按键 */
static bool s_key_sim  = true;   /* V1.4.x: 触摸反馈 — 触摸点击=触控板"按下/释放"双脉冲 */
/* 暗黑模式三态 (V1.3.x): 0=关闭 1=暗黑UI模式(除游戏外全反色) 2=全部暗黑模式(含游戏全反色).
 * 面板反色 (黑↔白) 由 st7305_set_inversion 实现; "暗黑UI"在进入游戏时自动恢复正常,
 * 退出游戏恢复反色 — 由 service tick 监听 input_is_game_mode() 驱动. */
static int s_dark = 0;   /* 0/1/2 */
/* V1.3.x: 保存 lcd 供 gamemode 回调同步反色 (游戏阻塞主循环, service tick 不生效,
 * 需在进出游戏瞬间由 input_set_gamemode_cb 即时调用 settings_dark_apply_lcd) */
static st7305_handle_t *s_dark_lcd = NULL;

/* ============ NVS 持久化 ============ */
static void settings_save_u8(const char *k, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(SET_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, k, v);
        nvs_commit(h);
        nvs_close(h);
    }
}
/* 按当前暗黑模式 + 游戏态应用反色 (0=关 1=UI反色 2=全部反色).
 * 供 开机(main) / 设置页切换 / service tick(进出游戏) 三处调用. */
static void settings_dark_apply_lcd(st7305_handle_t *lcd) {
    if (!lcd) return;
    /* 反色判定: 模式2=全部反色(含游戏); 模式1=除游戏外反色; 模式0=不反色.
     * game mode 由 input_is_game_mode() 提供 (引擎进 eng_task_run_loop 时为 true). */
    bool inv = (s_dark == 2) || (s_dark == 1 && !input_is_game_mode());
    st7305_set_inversion(lcd, inv);
}
/* 开机应用暗黑模式 (读 NVS → 面板反色). main 在 LCD 初始化后调用.
 * V1.3.x 三态化: 新 key "darkm" 存 0/1/2; 旧 key "dark"(0/1) 仅作首次迁移 (旧 1=全局反色 → 2 全部暗黑). */
void settings_apply_dark(st7305_handle_t *lcd) {
    s_dark = 0;
    s_dark_lcd = lcd;   /* V1.3.x: 保存供 gamemode 回调同步反色 (游戏阻塞主循环, service 不生效) */
    nvs_handle_t h;
    if (nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "darkm", &v) == ESP_OK) {
            s_dark = (v <= 2) ? (int)v : 2;
        } else if (nvs_get_u8(h, "dark", &v) == ESP_OK) {
            s_dark = (v != 0) ? 2 : 0;   /* 旧 1 → 全部暗黑, 行为不变 */
        }
        nvs_close(h);
    }
    settings_dark_apply_lcd(lcd);
}
/* 保存暗黑模式 (写新 key "darkm", 不污染旧 key) */
static void settings_save_dark(void) { settings_save_u8("darkm", (uint8_t)s_dark); }
static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(SET_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK) {
        /* 兼容: 旧固件存 0-100 百分比, 新固件存档位 0-9 */
        int pct = (v > (uint8_t)(AUDIO_VOL_STEPS - 1)) ? (int)v
                  : audio_player_vol_to_percent((int)v);
        audio_player_set_volume(pct);
    }
    if (nvs_get_u8(h, "snd_on", &v) == ESP_OK)     audio_player_set_muted(!v);
    /* V1.2.x: 震动拆为 UI/游戏 两个独立开关 (NVS vib_ui / vib_game).
     * 旧固件的单键 "vib" 仅作首次迁移: 任一缺省时取旧值. */
    if (nvs_get_u8(h, "vib_ui", &v) == ESP_OK)        s_vib_ui = v;
    else if (nvs_get_u8(h, "vib", &v) == ESP_OK)      s_vib_ui = v;
    if (nvs_get_u8(h, "vib_game", &v) == ESP_OK)      s_vib_game = v;
    else if (nvs_get_u8(h, "vib", &v) == ESP_OK)      s_vib_game = v;
    vibrator_set_ui_enabled(s_vib_ui);
    vibrator_set_game_enabled(s_vib_game);
    /* V1.4.x: 按键模拟开关 (触控板式点击双脉冲) */
    if (nvs_get_u8(h, "vib_key", &v) == ESP_OK) s_key_sim = v;
    vibrator_set_key_sim(s_key_sim);
    nvs_close(h);
}

/* 系统信息弹窗内容 */
static char s_info_lines[8][32];
static int  s_info_n;
static void build_sysinfo(void) {
    /* 直接 snprintf 进固定缓冲(32B x 8), 免中间缓冲+strcpy, 天然边界安全 */
    snprintf(s_info_lines[0], sizeof(s_info_lines[0]),
             "\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac:1.3"); /* 固件版本:1.3 */
    snprintf(s_info_lines[1], sizeof(s_info_lines[1]),
             "\xe5\x89\xa9\xe4\xbd\x99\xe5\x86\x85\xe5\xad\x98:%uKB", /* 剩余内存:XXKB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    snprintf(s_info_lines[2], sizeof(s_info_lines[2]),
             "\xe5\x89\xa9\xe4\xbd\x99PSRAM:%uKB", /* 剩余PSRAM:XXKB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(s_info_lines[3], sizeof(s_info_lines[3]),
             "\xe4\xbd\x9c\xe8\x80\x85\xe4\xbf\xa1\xe6\x81\xaf:LinIT"); /* 作者信息:LinIT */
    s_info_n = 4;
}

/* ---- 时间设置: 3.3 风格滚轮弹窗.
 * 单行横排 "年 月 日 时:分:秒", 焦点字段反色高亮;
 * 焦点字段上下显示相邻值 (prev/current/next), 上滑/下滑拖动调值, 也可 LEFT/RIGHT 切字段、UP/DOWN 调值.
 * CONFIRM 保存到 RTC 并关闭, BACK 丢弃关闭. */
static struct tm s_edit_tm;
static bool s_edit_valid = false;
static int  s_time_field = 0;      /* 0=年 1=月 2=日 3=时 4=分 5=秒 */

#define TIME_FIELD_N 6
#define WHEEL_W       350
#define WHEEL_H       250   /* 与普通列表弹窗尺寸 (350×250) 一致 */
#define WHEEL_STEP_H  30   /* 相邻值行距(加大→上下值不重叠、不触边框) */
#define WHEEL_SIDE    2    /* 上下各显示的相邻值个数 */
#define WHEEL_COL_MAX 3
static int s_fx[TIME_FIELD_N];   /* 字段列起始 x (渲染/触摸共用, 每帧刷新) */
static int s_fw[TIME_FIELD_N];   /* 字段列宽 (数值+单位) */
static int s_time_mode = 0;      /* 0=日期(年/月/日) 1=时间(时/分/秒) */

/* 休眠滚轮: 复用壁纸 NVS "min" (0=自动不息眠, 1..30 分钟) */
#define SLEEP_NS "os_wp"
static int s_sleep_idx = 0;      /* 休眠脚本当前档位下标 */
static int s_wheel_sleep = 0;    /* 当前滚轮是否单列(休眠/壁纸)模式 (时间=0) */
static int s_wheel_kind = 0;     /* 单列模式内容: 0=休眠时间(os_wp/min) 1=壁纸时间(os_wp/wp_run) */
static int s_wheel_y0 = 0;       /* 滚轮当前值行顶 y (渲染时刷新, 触摸点击选档用) */
static const int s_sleep_mins[] = { 0, 1, 2, 3, 5, 10, 15, 20, 30 };
#define SLEEP_MINS_N (int)(sizeof(s_sleep_mins)/sizeof(s_sleep_mins[0]))
/* 壁纸(屏保)运行时长档位: 1..30 分钟 (滚轮复用, 数据源=wp_run) */
static const int s_wpgrp_mins[] = { 1, 2, 3, 5, 10, 15, 20, 30 };
#define WPGRP_MINS_N (int)(sizeof(s_wpgrp_mins)/sizeof(s_wpgrp_mins[0]))
/* 关机时间档位 (0=不关机; 含分钟~10天): 须在 wheel_kind_tab(上方)之前定义 */
static const int s_shutgrp_mins[] = { 0, 5, 10, 30, 60, 1440, 4320, 7200, 10080, 14400 };
#define SHUTGRP_MINS_N (int)(sizeof(s_shutgrp_mins)/sizeof(s_shutgrp_mins[0]))
static int sleep_min_get(void) {
    int v = 3;   /* 默认 3 分钟 (未在 NVS 设置时) */
    nvs_handle_t h;
    if (nvs_open(SLEEP_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "min", &t) == ESP_OK) v = (int)t;
        nvs_close(h);
    }
    return v;
}
static int sleep_index_of(int min) { for (int i = 0; i < SLEEP_MINS_N; i++) if (s_sleep_mins[i] == min) return i; return 0; }

static void open_time_dlg(ui_ctx_t *ctx);
static void open_time_wheel_dlg(ui_ctx_t *ctx);
/* 当前模式字段起点 (时间=时 3, 日期=年 0) */
static int tm_first(void) { return s_time_mode ? 3 : 0; }   /* 3=时 */

static int time_field_val(int f) {
    switch (f) {
        case 0: return s_edit_tm.tm_year + 1900;
        case 1: return s_edit_tm.tm_mon + 1;
        case 2: return s_edit_tm.tm_mday;
        case 3: return s_edit_tm.tm_hour;
        case 4: return s_edit_tm.tm_min;
        default: return s_edit_tm.tm_sec;
    }
}
static void time_field_str(int f, int v, char *out, size_t n) {
    if (f == 0)      snprintf(out, n, "%d", v);      /* 年 */
    else if (f < 3)  snprintf(out, n, "%d", v);      /* 月/日 不补零 */
    else             snprintf(out, n, "%02d", v);    /* 时/分/秒 补零 */
}
/* 单位后缀: 年/月/日/小时/分钟/秒 (不用冒号, 状态栏格式保持不变) */
static const char *time_suffix(int f) {
    static const char *s[TIME_FIELD_N] = { "\xe5\xb9\xb4", "\xe6\x9c\x88", "\xe6\x97\xa5",
        "\xe5\xb0\x8f\xe6\x97\xb6", "\xe5\x88\x86\xe9\x92\x9f", "\xe7\xa7\x92" }; /* 年/月/日/小时/分钟/秒 */
    return s[f];
}
static void time_adjust(int f, int delta) {
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    switch (f) {
    case 0: {
        int y = s_edit_tm.tm_year + 1900 + delta;
        if (y < 2020) y = 2020;
        if (y > 2099) y = 2099;
        s_edit_tm.tm_year = y - 1900;
        break;
    }
    case 1: {
        int m = s_edit_tm.tm_mon + delta;
        while (m < 0) m += 12;
        while (m > 11) m -= 12;
        s_edit_tm.tm_mon = m;
        break;
    }
    case 2: {
        int m = s_edit_tm.tm_mon, yy = s_edit_tm.tm_year + 1900;
        int bd = dim[m];
        if (m == 1) {
            bool leap = ((yy % 4 == 0) && (yy % 100 != 0)) || (yy % 400 == 0);
            if (leap) bd = 29;
        }
        int dd = s_edit_tm.tm_mday + delta;
        if (dd < 1) dd = bd;
        if (dd > bd) dd = 1;
        s_edit_tm.tm_mday = dd;
        break;
    }
    case 3: {
        int h = s_edit_tm.tm_hour + delta;
        if (h < 0) h = 23;
        if (h > 23) h = 0;
        s_edit_tm.tm_hour = h;
        break;
    }
    case 4: {
        int mi = s_edit_tm.tm_min + delta;
        if (mi < 0) mi = 59;
        if (mi > 59) mi = 0;
        s_edit_tm.tm_min = mi;
        break;
    }
    case 5: {
        int s = s_edit_tm.tm_sec + delta;
        if (s < 0) s = 59;
        if (s > 59) s = 0;
        s_edit_tm.tm_sec = s;
        break;
    }
    default:
        break;
    }
}
/* 取 field 偏移 steps 步后的值 (沿调整方向连续走 steps 步, 不改动当前草稿) */
static int time_field_steps(int f, int steps) {
    struct tm save = s_edit_tm;
    int dir = (steps < 0) ? -1 : 1, n = (steps < 0) ? -steps : steps;
    for (int i = 0; i < n; i++) time_adjust(f, dir);
    int v = time_field_val(f);
    s_edit_tm = save;
    return v;
}
/* 保存当前草稿到 RTC */
static void time_save_rtc(void) {
    struct tm t = s_edit_tm; t.tm_isdst = -1;
    time_t ts = mktime(&t);
    if (ts != (time_t)-1) {
        struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
}

/* ---- 滚轮渲染 (时间/休眠共用): 上下列各 WHEEL_SIDE 个相邻值, 当前值高亮下托 ---- */
static int  wheel_col_count(void);
static int  wheel_active(void);
static void wheel_col_text(int col, int steps, char *out, size_t n);
static const char *wheel_col_unit(int col);
static bool wheel_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    if (!s_edit_valid) {
        time_t t = time(NULL); struct tm *ct = localtime(&t);
        if (ct) s_edit_tm = *ct;
        s_edit_valid = true;
    }
    const int W = WHEEL_W, H = WHEEL_H;
    int bx = (UI_SCREEN_W - W) / 2, by = (UI_SCREEN_H - H) / 2;
    const int bbt = by + H - 2 - 44;   /* 底部确认/取消按钮区顶 */
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }
    const int ncol = wheel_col_count();
    /* 每列宽 = max(所有显示行 数值+单位 宽度) */
    int colw[WHEEL_COL_MAX], total = 0;
    for (int c = 0; c < ncol; c++) {
        const char *unit = wheel_col_unit(c);
        int uw = unit[0] ? text_width(unit) : 0;
        int mw = 0;
        for (int k = -WHEEL_SIDE; k <= WHEEL_SIDE; k++) {
            char vs[24]; wheel_col_text(c, k, vs, sizeof(vs));
            if (!vs[0]) continue;
            int w = text_width(vs) + uw;
            if (w > mw) mw = w;
        }
        colw[c] = mw; total += mw;
    }
    const int gap = 18;
    total += gap * (ncol - 1);
    /* 内容区垂直居中 (按钮区上方) */
    const int cy = by + 2 + (bbt - (by + 2)) / 2;
    const int y0 = cy - 13;   /* 当前值文字顶 (字形约 26px) */
    s_wheel_y0 = y0;          /* 供 wheel_touch 点击选档换算档位 */
    int x = bx + (W - total) / 2;
    const int ac = wheel_active();
    for (int c = 0; c < ncol; c++) {
        const char *unit = wheel_col_unit(c);
        int ccx = x + colw[c] / 2;   /* 列中心 */
        s_fx[c] = x; s_fw[c] = colw[c];   /* 触摸热区 */
        char vs[24]; wheel_col_text(c, 0, vs, sizeof(vs));
        int vw = text_width(vs);
        int vcx = ccx - vw / 2;      /* 数字左缘 (数字水平居中于列) */
        bool act = (c == ac);
        if (act) {
            /* 活动列: 上下相邻值 */
            for (int k = -WHEEL_SIDE; k <= WHEEL_SIDE; k++) {
                if (k == 0) continue;
                char ns[24]; wheel_col_text(c, k, ns, sizeof(ns));
                if (!ns[0]) continue;
                draw_text(lcd, ccx - text_width(ns) / 2, y0 + k * WHEEL_STEP_H, ns, false);
            }
            /* 黑框只包数字 (不包单位), 数字在框内水平垂直都居中; 单位保持黑字非反色 */
            fill_rect(lcd, vcx - 4, y0 - 3, vcx + vw + 3, y0 + 25, ST7305_COLOR_BLACK);
            draw_text(lcd, vcx, y0, vs, true);
            if (unit[0]) draw_text(lcd, vcx + vw + 5, y0, unit, false);
        } else {
            draw_text(lcd, vcx, y0, vs, false);
            if (unit[0]) draw_text(lcd, vcx + vw + 5, y0, unit, false);
        }
        x += colw[c] + gap;
    }
    /* 底部 确认/取消 */
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, ty, "\xe7\xa1\xae\xe8\xae\xa4", false);   /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, ty, "\xe5\x8f\x96\xe6\xb6\x88", false);  /* 取消 */
    return true;
}

/* 滚动列信息: 休眠=单列; 时间=当前模式的前 3 字段 */
static int  wheel_col_count(void) { return s_wheel_sleep ? 1 : 3; }
static int  wheel_active(void)    { return s_wheel_sleep ? 0 : (s_time_field - tm_first()); }
static void wheel_set_active(int col) { if (!s_wheel_sleep) s_time_field = tm_first() + col; }
/* 单列模式数据源: 0=休眠(os_wp/min) 1=壁纸(os_wp/wp_run) 2=关机(os_wp/wp_shut) */
static const int *wheel_kind_tab(int *n) {
    if (s_wheel_kind == 1) { *n = WPGRP_MINS_N; return s_wpgrp_mins; }
    if (s_wheel_kind == 2) { *n = SHUTGRP_MINS_N; return s_shutgrp_mins; }  /* 关机时间档位 */
    *n = SLEEP_MINS_N; return s_sleep_mins;
}
static void wheel_confirm_save(ui_ctx_t *ctx, int min) {
    nvs_handle_t h;
    if (nvs_open(SLEEP_NS, NVS_READWRITE, &h) == ESP_OK) {
        const char *key = (s_wheel_kind == 1) ? "wp_run" : (s_wheel_kind == 2) ? "wp_shut" : "min";
        nvs_set_i32(h, key, min); nvs_commit(h); nvs_close(h);
    }
    if (s_wheel_kind == 1) {
        wp_screensaver_set_run_ms(min > 0 ? (uint32_t)min * 60000UL : UINT32_MAX);
        os_dialog_toast(ctx, "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 壁纸时间已设置 */
    } else if (s_wheel_kind == 2) {
        wp_screensaver_set_shutdown_ms(min > 0 ? (uint32_t)min * 60000UL : 0);
        os_dialog_toast(ctx, "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 关机时间已设置 */
    } else {
        wp_screensaver_set_delay_ms(min <= 0 ? UINT32_MAX : (uint32_t)min * 60000UL);
        os_dialog_toast(ctx, "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 休眠时间已设置 */
    }
}
/* 某列 current+steps 档显示文本 (到首尾返回空串=不画越界值) */
static void wheel_col_text(int col, int steps, char *out, size_t n) {
    if (s_wheel_sleep) {
        int cnt; const int *tab = wheel_kind_tab(&cnt);
        int idx = s_sleep_idx + steps;
        if (idx < 0 || idx >= cnt) { out[0] = '\0'; return; }
        int m = tab[idx];
        if (m <= 0) {
            /* 档位 0: 休眠/壁纸="永不休眠", 关机="不关机" */
            if (s_wheel_kind == 2) snprintf(out, n, "\xe4\xb8\x8d\xe5\x85\xb3\xe6\x9c\xba"); /* 不关机 */
            else                   snprintf(out, n, "\xe6\xb0\xb8\xe4\xb8\x8d\xe4\xbc\x91\xe7\x9c\xa0"); /* 永不休眠 */
        } else if (s_wheel_kind == 2 && m % 1440 == 0) {
            snprintf(out, n, "%d\xe5\xa4\xa9", m / 1440);     /* X天 (关机时间天级档) */
        } else {
            snprintf(out, n, "%d\xe5\x88\x86\xe9\x92\x9f", m); /* X分钟 */
        }
    } else {
        int f = tm_first() + col;
        time_field_str(f, time_field_steps(f, steps), out, n);
    }
}
/* 某列单位: 时间=年/月/日/小时/分钟/秒; 休眠文本已含单位 */
static const char *wheel_col_unit(int col) {
    if (s_wheel_sleep) return "";
    return time_suffix(tm_first() + col);
}
/* 调值: 单列=翻档(夹紧首尾), 时间=调对应字段 */
static void wheel_adjust(int col, int delta) {
    if (s_wheel_sleep) {
        int cnt; (void)wheel_kind_tab(&cnt);
        int nb = s_sleep_idx + delta;
        if (nb < 0) nb = 0;
        if (nb >= cnt) nb = cnt - 1;
        s_sleep_idx = nb;
    } else {
        time_adjust(tm_first() + col, delta);
    }
}
/* 确认: 单列→NVS+对应延时; 时间→RTC */
static void wheel_confirm(ui_ctx_t *ctx) {
    if (s_wheel_sleep) {
        int cnt; const int *tab = wheel_kind_tab(&cnt);
        wheel_confirm_save(ctx, tab[s_sleep_idx]);
    } else {
        time_save_rtc();
        os_dialog_toast(ctx, "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); /* 时间已保存 */
    }
    os_dialog_pop(ctx);
}

static bool wheel_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    const int ncol = wheel_col_count();
    switch (a) {
    case OS_ACTION_LEFT:  wheel_set_active((wheel_active() + ncol - 1) % ncol); ctx->needs_redraw = true; return true;
    case OS_ACTION_RIGHT: wheel_set_active((wheel_active() + 1) % ncol); ctx->needs_redraw = true; return true;
    case OS_ACTION_UP:    wheel_adjust(wheel_active(), +1); ctx->needs_redraw = true; return true;
    case OS_ACTION_DOWN:  wheel_adjust(wheel_active(), -1); ctx->needs_redraw = true; return true;
    case OS_ACTION_CONFIRM: wheel_confirm(ctx); return true;
    case OS_ACTION_BACK:
        if (!s_wheel_sleep) s_time_field = tm_first();   /* 取消: 复位字段选中 (不改状态) */
        return false;   /* 走默认关闭 */
    default: return false;   /* BACK → 默认取消 */
    }
}

static void wheel_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    int tx, ty;
    /* 上下拖动: 累积余差, 每满一行调一级 (跟手; 休眠边界由 wheel_adjust 夹紧, 不越界) */
    static int s_t_last = -1, s_t_acc = 0;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_t_last < 0) {
            s_t_last = ty; s_t_acc = 0;
            /* 按下即把手指所在列设为活动列, 随后拖动只改该列 (修复"无论拖到哪都改年").
             * V1.3.x: 仅内容区 (y < 按钮区顶) 才切列 — 点底部"确认/取消"按钮时
             * 若 x 恰好落在某列热区 (如时间滚轮第3列=秒) 不再误选中该列. */
            const int H = WHEEL_H;
            const int by = (UI_SCREEN_H - H) / 2;
            const int bbt = by + H - 2 - 44;   /* 与 wheel_touch/wheel_render 同一按钮区顶 */
            if (!s_wheel_sleep && ty < bbt) {
                int ncol = wheel_col_count();
                for (int c = 0; c < ncol; c++) {
                    if (tx >= s_fx[c] - 4 && tx <= s_fx[c] + s_fw[c] - 1 + 4) {
                        if (wheel_active() != c) {
                            wheel_set_active(c);
                            if (ctx) ctx->needs_redraw = true;
                        }
                        break;
                    }
                }
            }
        } else {
            s_t_acc += ty - s_t_last;
            s_t_last = ty;
            bool changed = false;
            while (s_t_acc <= -WHEEL_STEP_H) { wheel_adjust(wheel_active(), +1); s_t_acc += WHEEL_STEP_H; changed = true; }
            while (s_t_acc >= +WHEEL_STEP_H) { wheel_adjust(wheel_active(), -1); s_t_acc -= WHEEL_STEP_H; changed = true; }
            if (changed) ctx->needs_redraw = true;
        }
    } else {
        s_t_last = -1; s_t_acc = 0;
    }
}

/* 自定义触摸: 底部=确认/取消, 内容=点列切焦点 (弹窗框内才响应) */
static bool wheel_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    const int W = WHEEL_W, H = WHEEL_H;
    int bx = (UI_SCREEN_W - W) / 2, by = (UI_SCREEN_H - H) / 2;
    if (x < bx || x > bx + W - 1 || y < by || y > by + H - 1) return false;
    const int bbt = by + H - 2 - 44;
    if (y >= bbt) {
        int mid = bx + W / 2;
        if (x < mid) wheel_confirm(ctx);   /* 确认 */
        else {
            /* 取消: 复位字段选中 (回到打开时状态, 不残留) */
            if (!s_wheel_sleep) s_time_field = tm_first();
            os_dialog_pop(ctx);
        }
        return true;
    }
    const int ncol = wheel_col_count();
    for (int c = 0; c < ncol; c++) {
        if (x >= s_fx[c] - 4 && x <= s_fx[c] + s_fw[c] - 1 + 4) {
            if (s_wheel_sleep) {
                /* V1.3.x: 休眠滚轮支持"点击选档" — 点哪行选中哪档 (拖动调档保留).
                 * 行距 WHEEL_STEP_H, 当前值行顶 s_wheel_y0, 相邻行 = y0 ± k*STEP. */
                int k = (y - s_wheel_y0 + WHEEL_STEP_H / 2) / WHEEL_STEP_H;
                if (k < -WHEEL_SIDE) k = -WHEEL_SIDE;
                if (k > +WHEEL_SIDE) k = +WHEEL_SIDE;
                int nb = s_sleep_idx + k;
                int cnt; (void)wheel_kind_tab(&cnt);   /* 当前数据源档数 (休眠/壁纸/关机各不相同) */
                if (nb < 0) nb = 0;
                if (nb >= cnt) nb = cnt - 1;
                if (nb != s_sleep_idx) {
                    s_sleep_idx = nb;
                    if (ctx) ctx->needs_redraw = true;
                }
            } else if (wheel_active() != c) {
                wheel_set_active(c);
                if (ctx) ctx->needs_redraw = true;
            }
            return true;
        }
    }
    return true;   /* 弹窗内其他区域吞掉, 不外泄到下层 */
}

/* 日期/时间 选择回调 */
static void on_time_choice(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;   /* 返回 */
    s_time_mode = result;     /* 0=日期 1=时间 */
    s_time_field = tm_first();
    open_time_wheel_dlg(ctx);
}

/* 时间设置入口: 先选「日期」/「时间」 */
static void open_time_dlg(ui_ctx_t *ctx) {
    if (!s_edit_valid) {
        time_t t = time(NULL); struct tm *ct = localtime(&t);
        if (ct) s_edit_tm = *ct;
        s_edit_valid = true;
    }
    static const char *items[3] = {
        "\xe6\x97\xa5\xe6\x9c\x9f",   /* 日期 */
        "\xe6\x97\xb6\xe9\x97\xb4",   /* 时间 */
        NULL
    };
    os_dialog_list(ctx, "", items, 2, s_time_mode, on_time_choice, NULL);
}

/* 滚轮设置弹窗 (时间/休眠共用; on_touch 处理点列+按钮) */
static void open_time_wheel_dlg(ui_ctx_t *ctx) {
    s_wheel_sleep = 0;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = wheel_render;
    dlg.on_key    = wheel_key;
    dlg.on_poll   = wheel_poll;
    dlg.on_touch  = wheel_touch;
    os_dialog_push(ctx, &dlg);
}

/* 休眠时间: 同款滚轮 UI (单列, 夹紧首尾) */
static void open_sleep_dlg(ui_ctx_t *ctx) {
    s_wheel_sleep = 1;
    s_wheel_kind = 0;
    s_sleep_idx = sleep_index_of(sleep_min_get());
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = wheel_render;
    dlg.on_key    = wheel_key;
    dlg.on_poll   = wheel_poll;
    dlg.on_touch  = wheel_touch;
    os_dialog_push(ctx, &dlg);
}
/* 壁纸时间: 复用休眠同款滚轮 UI, 数据源=os_wp/wp_run (档位 s_wpgrp_mins) */
static int wpgrp_min_get(void);   /* 定义在后 (休眠相关区) */
static int wpgrp_index_of(int min) { for (int i = 0; i < WPGRP_MINS_N; i++) if (s_wpgrp_mins[i] == min) return i; return 0; }
static void open_wp_dlg(ui_ctx_t *ctx) {
    s_wheel_sleep = 1;
    s_wheel_kind = 1;
    s_sleep_idx = wpgrp_index_of(wpgrp_min_get());
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = wheel_render;
    dlg.on_key    = wheel_key;
    dlg.on_poll   = wheel_poll;
    dlg.on_touch  = wheel_touch;
    os_dialog_push(ctx, &dlg);
}

/* ---- 字体大小档位 (V1.7.x 保留: 供 ui_common/page_main 全局渲染字号) ---- */
/* [V1.0.9x 精简: 设置页"字体调整"菜单(粗细/大小)已整体移除, 字号固定取 NVS 存档或默认] */
static int font_size_get(void) {
    int v = 3;   /* 默认 22px (档索引 3) */
    nvs_handle_t h;
    if (nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t x = 0;
        if (nvs_get_u8(h, "font_size", &x) == ESP_OK && x < 5) v = (int)x;
        nvs_close(h);
    }
    return v;
}
int settings_font_size(void) { return font_size_get(); }   /* 档位 0..4 供主菜单标签调字号 */

/* ---- 暗黑模式滚轮: 关闭 / 暗黑UI模式(除游戏外反色) / 全部暗黑模式(含游戏) ---- */
static void on_dark_wheel(ui_ctx_t *ctx, int sel, void *ud) {
    (void)ud;
    if (sel < 0) return;   /* 取消: 不修改 */
    s_dark = sel;          /* 0/1/2 */
    settings_save_dark();
    settings_dark_apply_lcd(ctx ? ctx->lcd : NULL);
    if (sel == 0)      os_dialog_toast(ctx, "\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f\xe5\x85\xb3");   /* 暗黑模式关 */
    else if (sel == 1) os_dialog_toast(ctx, "\xe6\x9a\x97\xe9\xbb\x91UI\xe6\xa8\xa1\xe5\xbc\x8f");          /* 暗黑UI模式 */
    else               os_dialog_toast(ctx, "\xe5\x85\xa8\xe9\x83\xa8\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f"); /* 全部暗黑模式 */
}
static void open_dark_dlg(ui_ctx_t *ctx) {
    static const char *ptrs[3] = {
        "\xe5\x85\xb3\xe9\x97\xad",          /* 关闭 */
        "\xe6\x9a\x97\xe9\xbb\x91UI\xe6\xa8\xa1\xe5\xbc\x8f",       /* 暗黑UI模式 */
        "\xe5\x85\xa8\xe9\x83\xa8\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f", /* 全部暗黑模式 */
    };
    os_dialog_wheel(ctx, ptrs, 3, s_dark, on_dark_wheel);
}

/* ---- 休眠相关子菜单 (V1.5.x): 休眠时间 / 壁纸时间 / 触摸唤醒 ---- */
static void open_disp_dlg(ui_ctx_t *ctx, int sel);
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel);
/* 壁纸时间 (与壁纸页共用 NVS "os_wp"/"wp_run"; 档位数组见上方 s_wpgrp_mins) */
static int wpgrp_min_get(void) {
    int v = 10;   /* 默认 10 分钟 */
    nvs_handle_t h;
    if (nvs_open(SLEEP_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "wp_run", &t) == ESP_OK) v = (int)t;
        nvs_close(h);
    }
    return v;
}
static void wpgrp_min_save(ui_ctx_t *ctx, int v) {
    nvs_handle_t h;
    if (nvs_open(SLEEP_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "wp_run", v); nvs_commit(h); nvs_close(h);
    }
    if (v > 0) wp_screensaver_set_run_ms((uint32_t)v * 60000UL);
    os_dialog_toast(ctx, "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 壁纸时间已设置 */
}
static void on_sleepgrp_min_wheel(ui_ctx_t *ctx, int sel, void *ud) {
    (void)ud;
    if (sel >= 0 && sel < WPGRP_MINS_N) { wpgrp_min_save(ctx, s_wpgrp_mins[sel]); }
}
static void open_sleepgrp_min_dlg(ui_ctx_t *ctx) {
    char labels[WPGRP_MINS_N][12];
    const char *ptrs[WPGRP_MINS_N];
    int cur = wpgrp_min_get(), sel = 0;
    for (int i = 0; i < WPGRP_MINS_N; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%d\xe5\x88\x86\xe9\x92\x9f", s_wpgrp_mins[i]); /* N分钟 */
        ptrs[i] = labels[i];
        if (cur == s_wpgrp_mins[i]) sel = i;
    }
    os_dialog_wheel(ctx, ptrs, WPGRP_MINS_N, sel, on_sleepgrp_min_wheel);
}
static int shutgrp_min_get(void) {
    int v = 0;
    nvs_handle_t h;
    if (nvs_open(SLEEP_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "wp_shut", &t) == ESP_OK) v = (int)t;
        nvs_close(h);
    }
    return v;
}
static int shutgrp_index_of(int min) { for (int i = 0; i < SHUTGRP_MINS_N; i++) if (s_shutgrp_mins[i] == min) return i; return 0; }
static void open_shutgrp_min_dlg(ui_ctx_t *ctx) {
    /* 复用休眠同款滚轮 (s_wheel_sleep=1 + s_wheel_kind=2): 同视觉/拖动手势/点击选档 */
    s_wheel_sleep = 1;
    s_wheel_kind = 2;
    s_sleep_idx = shutgrp_index_of(shutgrp_min_get());
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = wheel_render;
    dlg.on_key    = wheel_key;
    dlg.on_poll   = wheel_poll;
    dlg.on_touch  = wheel_touch;
    os_dialog_push(ctx, &dlg);
}
/* 休眠相关 触摸唤醒 toggle (关后原地刷新) */
static void on_sleepgrp_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0: open_wp_dlg(ctx); break;             /* 壁纸时间 (滚轮) */
    case 1: open_sleep_dlg(ctx); break;          /* 休眠时间 */
    case 2: open_shutgrp_min_dlg(ctx); break;    /* 关机时间 */
    case 3: {  /* 触摸唤醒壁纸开关 (V1.5.x) */
        bool en = !wp_screensaver_touch_wake_enabled();
        wp_screensaver_set_touch_wake(en);
        os_dialog_pop(ctx);
        open_sleepgrp_dlg(ctx, 3);
        os_dialog_toast(ctx, en ?
            "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x8f\xaf\xe5\x94\xa4\xe9\x86\x92\xe5\xa3\x81\xe7\xba\xb8" :   /* 触摸可唤醒壁纸 */
            "\xe8\xa7\xa6\xe6\x91\xb8\xe4\xb8\x8d\xe5\x94\xa4\xe9\x86\x92\xe5\xa3\x81\xe7\xba\xb8");  /* 触摸不唤醒壁纸 */
        break;
    }
    default: break;
    }
}
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel) {
    char items[4][24];
    const char *ptrs[4];
    snprintf(items[0], sizeof(items[0]), "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4");  /* 休眠时间(wp_run=壁纸播多久进休眠) */
    snprintf(items[1], sizeof(items[1]), "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4");  /* 壁纸时间(min=多久不动进壁纸) */
    snprintf(items[2], sizeof(items[2]), "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4");  /* 关机时间 */
    snprintf(items[3], sizeof(items[3]), "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x94\xa4\xe9\x86\x92:%s",  /* 触摸唤醒:开/关 */
             wp_screensaver_touch_wake_enabled() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3];
    sel = (sel < 0) ? 0 : (sel > 3 ? 3 : sel);
    os_dialog_list(ctx, "\xe4\xbc\x91\xe7\x9c\xa0\xe7\x9b\xb8\xe5\x85\xb3", ptrs, 4, sel, on_sleepgrp_select, NULL);
}

/* ---- 显示与时间弹窗: 平铺 4 项 (日期/时间/字体/暗黑; 休眠独立为主菜单项) ---- */
static void on_disp_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0:   /* 日期更改 */
        if (!s_edit_valid) {
            time_t t = time(NULL); struct tm *ct = localtime(&t);
            if (ct) s_edit_tm = *ct;
            s_edit_valid = true;
        }
        s_time_mode = 0; s_time_field = tm_first();
        open_time_wheel_dlg(ctx);
        break;
    case 1:   /* 时间调整 */
        if (!s_edit_valid) {
            time_t t = time(NULL); struct tm *ct = localtime(&t);
            if (ct) s_edit_tm = *ct;
            s_edit_valid = true;
        }
        s_time_mode = 1; s_time_field = tm_first();
        open_time_wheel_dlg(ctx);
        break;
    case 2: open_dark_dlg(ctx); break;   /* 暗黑模式: 三选弹窗 (关闭/暗黑UI/全部暗黑) */
    default: break;
    }
}
static void open_disp_dlg(ui_ctx_t *ctx, int sel) {
    char items[3][24];
    const char *ptrs[3];
    snprintf(items[0], sizeof(items[0]), "\xe6\x97\xa5\xe6\x9c\x9f\xe6\x9b\xb4\xe6\x94\xb9");  /* 日期更改 */
    snprintf(items[1], sizeof(items[1]), "\xe6\x97\xb6\xe9\x97\xb4\xe8\xb0\x83\xe6\x95\xb4");  /* 时间调整 */
    /* [V1.0.9x 精简: 移除"字体调整"项 (细宋已删, 粗细无意义; 字号固定) */
    snprintf(items[2], sizeof(items[2]), "\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f");  /* 暗黑模式 */
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2];
    sel = (sel < 0) ? 0 : (sel > 2 ? 2 : sel);
    os_dialog_list(ctx, "", ptrs, 3, sel, on_disp_select, NULL);
}

/* ---- 声音调整弹窗: 声音开关/音量/震动 (动态显示状态) ---- */
static void open_snd_dlg(ui_ctx_t *ctx, int sel);
/* 音量滚轮: 0-9 档; 完成后刷新声音弹窗音量显示 */
static void on_vol_wheel(ui_ctx_t *ctx, int sel, void *ud) {
    (void)ud;
    if (sel >= 0 && sel < AUDIO_VOL_STEPS) {
        audio_player_set_volume(audio_player_vol_to_percent(sel));
        settings_save_u8("vol", (uint8_t)sel);   /* 存档位 0-9 */
    }
    os_dialog_mark_handled(ctx);   /* 阻止自动弹栈, 由下面自行替换刷新 */
    os_dialog_pop(ctx);            /* 滚轮 */
    os_dialog_pop(ctx);            /* 声音弹窗 */
    open_snd_dlg(ctx, 1);
}
static void open_vol_dlg(ui_ctx_t *ctx) {
    static const char *ptrs[10] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
    int cur = audio_player_vol_to_step(audio_player_get_volume());
    os_dialog_wheel(ctx, ptrs, AUDIO_VOL_STEPS, cur, on_vol_wheel);
}
static void on_snd_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {   /* 声音开关 */
        bool m = !audio_player_is_muted();
        audio_player_set_muted(m);
        settings_save_u8("snd_on", m ? 0 : 1);
    } else if (result == 1) {   /* 音量 → 滚轮选档 0-9 */
        open_vol_dlg(ctx);
        return;
    } else if (result == 2) {   /* UI 震动开关 */
        s_vib_ui = !s_vib_ui;
        vibrator_set_ui_enabled(s_vib_ui);
        settings_save_u8("vib_ui", s_vib_ui ? 1 : 0);
    } else if (result == 3) {   /* 游戏震动开关 */
        s_vib_game = !s_vib_game;
        vibrator_set_game_enabled(s_vib_game);
        settings_save_u8("vib_game", s_vib_game ? 1 : 0);
    } else if (result == 4) {   /* 触摸反馈开关 (V1.4.x) */
        s_key_sim = !s_key_sim;
        vibrator_set_key_sim(s_key_sim);
        settings_save_u8("vib_key", s_key_sim ? 1 : 0);
    } else {
        return;   /* 返回/其它: 不刷新 */
    }
    /* 原地替换刷新 (先弹当前层再压回, 层级不加深; 返回直接回设置主菜单).
     * V1.3.x: 高亮刚操作的行 (sel=result), 触摸/按键选择对应选项即时变黑反馈 */
    os_dialog_pop(ctx);
    open_snd_dlg(ctx, result);
}
static void open_snd_dlg(ui_ctx_t *ctx, int sel) {
    char items[5][30];
    const char *ptrs[5];
    snprintf(items[0], sizeof(items[0]), "\xe5\xa3\xb0\xe9\x9f\xb3:%s", audio_player_is_muted() ? "\xe5\x85\xb3" : "\xe5\xbc\x80");  /* 声音:关/开 */
    snprintf(items[1], sizeof(items[1]), "\xe9\x9f\xb3\xe9\x87\x8f:%d", /* 音量:N */
             audio_player_vol_to_step(audio_player_get_volume()));
    snprintf(items[2], sizeof(items[2]), "UI\xe9\x9c\x87\xe5\x8a\xa8:%s", s_vib_ui ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* UI震动:开/关 */
    snprintf(items[3], sizeof(items[3]), "\xe6\xb8\xb8\xe6\x88\x8f\xe9\x9c\x87\xe5\x8a\xa8:%s", s_vib_game ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 游戏震动:开/关 */
    snprintf(items[4], sizeof(items[4]), "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x8f\x8d\xe9\xa6\x88:%s", s_key_sim ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 触摸反馈:开/关 */
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3]; ptrs[4] = items[4];
    sel = (sel < 0) ? 0 : (sel > 4 ? 4 : sel);
    os_dialog_list(ctx, "", ptrs, 5, sel, on_snd_select, NULL);
}

/* ---- 无线连接弹窗: 蓝牙/WiFi 开关 + 连接WiFi (WiFi 开时显示) ---- */
static void open_wifi_dlg(ui_ctx_t *ctx, int sel);
static void open_wifi_scan(ui_ctx_t *ctx);
static void open_info_dlg(ui_ctx_t *ctx);

/* USB 网卡菜单"连接WiFi"入口: 复用以设置页同样的扫描→输密码 流程.
 * 需在设置页同编译单元内, 故放这里并导出让 page_usb_net 调用. */
void settings_wifi_connect_open(ui_ctx_t *ctx) {
    if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);   /* 先确保 WiFi 打开 */
    open_wifi_scan(ctx);
}
static void on_wifi_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {   /* 蓝牙开关 (懒加载 os_hw) */
        if (os_hw_is_active(OS_HW_BT)) os_hw_release(OS_HW_BT);
        else os_hw_request(OS_HW_BT);
        os_dialog_pop(ctx); open_wifi_dlg(ctx, result);   /* 原地替换, 层级不加深 */
    } else if (result == 1) {   /* WiFi 开关 */
        if (os_hw_is_active(OS_HW_WIFI)) os_hw_release(OS_HW_WIFI);
        else os_hw_request(OS_HW_WIFI);
        os_dialog_pop(ctx); open_wifi_dlg(ctx, result);   /* 原地替换, 层级不加深 */
    } else if (result == 2) {
        /* WiFi 开时 = "连接WiFi"; WiFi 关时该项为"系统信息" (固定末项映射) */
        if (os_hw_is_active(OS_HW_WIFI)) open_wifi_scan(ctx);
        else open_info_dlg(ctx);
    } else if (result == 3) {   /* WiFi 开时末项 = 系统信息 */
        open_info_dlg(ctx);
    }
}

/* ---- WiFi 输密码键盘弹窗: 选网后同一弹窗原位变为 (SSID + 密码框 + 经典键盘) ----
 * 键盘用自定义经典 5 排 (非 52 键): 与之前版本 wiFi 键盘一致, 无底返回行, 键贴弹窗框
 * (键不画边框, 以弹窗 2px 外框为界). 顶部: WiFi 名称 + 密码框. */
static char  s_wifi_pwd_ssid[33];
static char  s_wifi_pwd_pass[65];
static int   s_wifi_pwd_len = 0;
static bool  s_wp_sym   = false;   /* 符号层 */
static bool  s_wp_shift = false;   /* 大写 */

/* 弹窗式几何: 直接复用"搜索WiFi"同款 350×250 居中弹窗外框 (margins 25);
 * 顶部 SSID/密码框, 中部经典 5 排自定义键盘贴弹窗底部 (无返回行). */
#define WPW_MARGIN  25
#define WPW_W       (UI_SCREEN_W - 2 * WPW_MARGIN)   /* 350 */
#define WPW_H       250                              /* 与搜索WiFi弹窗同高 */
#define WKB_LX      (WPW_MARGIN + 4)                 /* 键盘左内边 x = 29 */
#define WKB_ROWH    25                               /* 键盘排高 (整体缩短 1/3: 原 38) */
#define WKB_TOP     (WPW_MARGIN + WPW_H - 2 - 5*WKB_ROWH)  /* 键盘顶 y = 148 (贴弹窗底) */

/* 自定义经典键盘行 (字母/符号两层, 与之前版本 wiFi 键盘一致) */
static const char *const wp_abc[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."
};
static const char *const wp_symk[4] = {
    "1234567890", "!@#$%^&*()", "-_=+[]{};:'", "\\|/?<>\"~"
};
static char wp_disp(char c) {
    if (!s_wp_sym && s_wp_shift && c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

/* 密码页发起连接后, 后台轮询连接结果并 toast 提示 */
static bool s_wifi_conn_pending = false;

static void wifi_pwd_connect(ui_ctx_t *ctx) {
    s_wifi_pwd_pass[s_wifi_pwd_len] = '\0';
    os_dialog_pop(ctx);                        /* 关密码键盘(扫描层) → 回"网络与系统"菜单 */
    os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5..."); /* 正在连接... */
    os_hw_request(OS_HW_WIFI);
    if (wifi_manager_connect(s_wifi_pwd_ssid, s_wifi_pwd_pass))
        s_wifi_conn_pending = true;
    ESP_LOGI(TAG, "WiFi 连接: %s", s_wifi_pwd_ssid);
}

/* 每帧: 连接成功→"连接成功"提示; 失败(重试结束)→"连接失败" (设置内连接不提示 IP) */
static void wifi_conn_poll(ui_ctx_t *ctx) {
    if (!s_wifi_conn_pending) return;
    if (wifi_manager_is_connected()) {
        s_wifi_conn_pending = false;
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f"); /* 连接成功 */
        return;
    }
    if (!wifi_manager_is_connecting()) {          /* 连接结束但未连上 = 失败 */
        s_wifi_conn_pending = false;
        os_dialog_toast(ctx, "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5"); /* 连接失败 */
    }
}

/* 画一个键: 整块填充 + 居中文本 + 右分割线 (垂直);
 * 行间水平分割线由渲染循环统一满宽绘制 (画到边框边缘),
 * 底排不画下分割线, 避免与弹窗外框重叠成 3px. */
static void wp_draw_key(st7305_handle_t *lcd, int x, int y, int w, const char *lb, bool sel) {
    fill_rect(lcd, x, y, x + w - 1, y + WKB_ROWH - 1, sel ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
    draw_vline(lcd, x + w - 1, y, y + WKB_ROWH - 1, ST7305_COLOR_BLACK);   /* 右分割线 */
    int tw = text_width(lb);
    draw_text(lcd, x + (w - tw) / 2, y + (WKB_ROWH - 24) / 2, lb, sel);
}

static bool wifi_pwd_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);

static bool wifi_pwd_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    if (s_wifi_pwd_len < 0) s_wifi_pwd_len = 0;
    if (s_wifi_pwd_len > 63) s_wifi_pwd_len = 63;
    const int kb0 = WKB_TOP, R = WKB_ROWH;
    int r = (y >= kb0 && y < kb0 + 5 * R) ? (y - kb0) / R : -1;
    if (r >= 0) {
        const char *const *ll = s_wp_sym ? wp_symk : wp_abc;
        if (r == 0 || r == 1) {
            int i = (x - WKB_LX) / 34;
            if (i >= 0 && i < 10) {
                char c = wp_disp(ll[r][i]);
                if (c && s_wifi_pwd_len < 64) s_wifi_pwd_pass[s_wifi_pwd_len++] = c;
                ctx->needs_redraw = true; return true;
            }
        } else if (r == 2) {
            int i = (x - WKB_LX) / 38;
            if (i >= 0 && i < 9) {
                char c = wp_disp(ll[2][i]);
                if (c && s_wifi_pwd_len < 64) s_wifi_pwd_pass[s_wifi_pwd_len++] = c;
                ctx->needs_redraw = true; return true;
            }
        } else if (r == 3) {
            if (x >= WKB_LX && x < WKB_LX + 50) {            /* Shift/ABC */
                if (s_wp_sym) { s_wp_sym = false; } else { s_wp_shift = !s_wp_shift; }
                ctx->needs_redraw = true; return true;
            }
            if (x >= WKB_LX + 290 && x < WKB_LX + 342) {     /* 退格 */
                if (s_wifi_pwd_len > 0) s_wifi_pwd_len--;
                ctx->needs_redraw = true; return true;
            }
            int i = (x - (WKB_LX + 50)) / 30;                /* 8 字母 */
            if (i >= 0 && i < 8) {
                char c = wp_disp(ll[3][i]);
                if (c && s_wifi_pwd_len < 64) s_wifi_pwd_pass[s_wifi_pwd_len++] = c;
                ctx->needs_redraw = true; return true;
            }
        } else {                                             /* r4 */
            if (x >= WKB_LX && x < WKB_LX + 64) {            /* &123/ABC 切层 (加宽防文字溢出) */
                s_wp_sym = !s_wp_sym;
                ctx->needs_redraw = true; return true;
            }
            if (x >= WKB_LX + 64 && x < WKB_LX + 64 + 218) { /* 空格 (往右挪) */
                if (s_wifi_pwd_len < 64) s_wifi_pwd_pass[s_wifi_pwd_len++] = ' ';
                ctx->needs_redraw = true; return true;
            }
            if (x < WKB_LX + 342) wifi_pwd_connect(ctx);     /* 回车=连接 (限键内, 防越界误连) */
            return true;
        }
        return true;
    }
    return true;
}

static bool wifi_pwd_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    if (a == OS_ACTION_BACK) { os_dialog_pop(ctx); return true; }
    if (a == OS_ACTION_CONFIRM) { wifi_pwd_connect(ctx); return true; }
    if (a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT || a == OS_ACTION_UP || a == OS_ACTION_DOWN) {
        return true;   /* 方向键暂不导航键盘, 触摸为主 */
    }
    return true;
}

static bool wifi_pwd_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    /* 不整屏清屏: 保留弹窗外围已有内容 (父菜单), 仅重画本弹窗口框 */
    int dx = WPW_MARGIN, dy = WPW_MARGIN;
    fill_rect(lcd, dx, dy, dx + WPW_W - 1, dy + WPW_H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, dx + k, dx + WPW_W - 1 - k, dy + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, dx + k, dx + WPW_W - 1 - k, dy + WPW_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, dx + k, dy + k, dy + WPW_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, dx + WPW_W - 1 - k, dy + k, dy + WPW_H - 1 - k, ST7305_COLOR_BLACK);
    }
    /* WiFi 名称 (居中, 超长只显示前 20英文/10中文 防溢出) */
    const char *nm = s_wifi_pwd_ssid[0] ? s_wifi_pwd_ssid : "...";
    nm = text_clip(nm, WPW_W - 24);
    draw_text_centered(lcd, dy + 6, nm, false);
    /* 密码框 (明文显示) */
    char pl[72];
    size_t p = 0;
    static const char pw_pfx[] = "\xe5\xaf\x86\xe7\xa0\x81: ";  /* "密码: " */
    for (size_t i = 0; i < sizeof(pw_pfx) - 1 && p < sizeof(pl) - 1; i++) pl[p++] = pw_pfx[i];
    int shown = (s_wifi_pwd_len > 16) ? 16 : s_wifi_pwd_len;
    for (int i = 0; i < shown; i++) {                 /* 明文: 直接显示密码字符 */
        char ch = s_wifi_pwd_pass[i];
        if (ch == ' ') ch = '\xc2\xb7';               /* 空格显示为 · 占位 */
        if (p < sizeof(pl) - 2) { pl[p++] = ch; }
    }
    pl[p] = '\0';
    draw_text_capped(lcd, dx + 8, dy + 39, pl, false, WPW_W - 20);   /* 密码下移 5px */
    /* 数字行上方横杠: 画在键盘顶上方 (密码不再碰线) */
    draw_hline(lcd, dx + 4, dx + WPW_W - 4, WKB_TOP - 2, ST7305_COLOR_BLACK);

    /* 当前触点 (按住反黑): 用无副作用 input_touch_now (不受 main.c 已消费
     * input_get_touch_pos 的锁存竞态影响, 保证按下去即变黑、松手即恢复) */
    int tv = 0, tx = -1, ty = -1;
    if (input_touch_now(&tx, &ty)) tv = 1;

    const char *const *ll = s_wp_sym ? wp_symk : wp_abc;
    const int kb0 = WKB_TOP, R = WKB_ROWH;
    /* 行0/1: 10 键 (行间分割线满宽画到边框边缘) */
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 10; i++) {
            int kx = WKB_LX + i * 34;
            char lb[2] = { wp_disp(ll[r][i]), 0 };
            bool sel = tv && ty >= kb0 + r * R && ty < kb0 + r * R + R && tx >= kx && tx < kx + 34;
            wp_draw_key(lcd, kx, kb0 + r * R, 34, lb, sel);
        }
        draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + (r + 1) * R - 1, ST7305_COLOR_BLACK);
    }
    /* 行2: 9 键居中 */
    for (int i = 0; i < 9; i++) {
        int kx = WKB_LX + i * 38;
        char lb[2] = { wp_disp(ll[2][i]), 0 };
        bool sel = tv && ty >= kb0 + 2 * R && ty < kb0 + 2 * R + R && tx >= kx && tx < kx + 38;
        wp_draw_key(lcd, kx, kb0 + 2 * R, 38, lb, sel);
    }
    draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + 3 * R - 1, ST7305_COLOR_BLACK);
    /* 行3: Shift + 8 字母 + 退格 */
    {
        int ry = kb0 + 3 * R;
        const char *sh = s_wp_sym ? "ABC"
                        : (s_wp_shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"); /* 小写/大写 */
        bool s0 = tv && ty >= ry && ty < ry + R && tx >= WKB_LX && tx < WKB_LX + 50;
        wp_draw_key(lcd, WKB_LX, ry, 50, sh, s0);
        for (int i = 0; i < 8; i++) {
            int kx = WKB_LX + 50 + i * 30;
            char lb[2] = { wp_disp(ll[3][i]), 0 };
            bool se = tv && ty >= ry && ty < ry + R && tx >= kx && tx < kx + 30;
            wp_draw_key(lcd, kx, ry, 30, lb, se);
        }
        bool s9 = tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 290 && tx < WKB_LX + 342;
        wp_draw_key(lcd, WKB_LX + 290, ry, 52, "\xe5\x88\xa0\xe9\x99\xa4", s9); /* 删除 */
        draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + 4 * R - 1, ST7305_COLOR_BLACK);
    }
    /* 行4: &123 + 空格(大, 往右) + 回车(原连接, 去删除并入退格) */
    {
        int ry = kb0 + 4 * R;
        bool s0 = tv && ty >= ry && ty < ry + R && tx >= WKB_LX && tx < WKB_LX + 64;
        wp_draw_key(lcd, WKB_LX, ry, 64, s_wp_sym ? "ABC" : "&123", s0);
        bool s1 = tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 64 && tx < WKB_LX + 64 + 218;
        wp_draw_key(lcd, WKB_LX + 64, ry, 218, "\xe7\xa9\xba\xe6\xa0\xbc", s1); /* 空格 */
        bool s3 = tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 282 && tx < WKB_LX + 342;
        wp_draw_key(lcd, WKB_LX + 282, ry, 60, "\xe5\x9b\x9e\xe8\xbd\xa6", s3); /* 回车 */
    }

    /* 键盘贴弹窗底部 (无底返回行) */
    ctx->needs_redraw = true;
    return true;
}

static void open_wifi_pwd_dlg(ui_ctx_t *ctx, const char *ssid) {
    if (ssid) snprintf(s_wifi_pwd_ssid, sizeof(s_wifi_pwd_ssid), "%s", ssid);
    else s_wifi_pwd_ssid[0] = '\0';
    s_wifi_pwd_len = 0;
    s_wifi_pwd_pass[0] = '\0';
    s_wp_sym = false; s_wp_shift = false;
    /* 原位改写当前弹窗 (扫描列表 → 输密码键盘) */
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (!d) return;
    d->count = 0;
    d->on_render = wifi_pwd_render;
    d->on_touch = wifi_pwd_touch;
    d->on_key = wifi_pwd_key;
    d->on_poll = NULL;   /* 关键: 清除扫描轮询 — 否则它会把 count 又改回网络数、
                           * 替换 items, 导致 on_render/count==0 分流失效, 键盘触点收不到 */
    d->cb = NULL;
    d->no_footer = true;   /* 自定义渲染无底"返回"行 */
    os_dialog_mark_handled(ctx);
    ctx->needs_redraw = true;
}

/* ---- WiFi 扫描列表弹窗 (3.3: 同一列表模板扫描/选择, 选后再输密码) ---- */
static void wifi_scan_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    int cnt = wifi_manager_get_scan_count();
    if (cnt != d->count && cnt > 0) {
        if (cnt > 60) cnt = 60;
        d->count = cnt;
        for (int i = 0; i < cnt; i++) {
            char ssid[33];
            if (wifi_manager_get_scan_ssid(i, ssid, sizeof(ssid)) && ssid[0])
                snprintf(d->items[i], sizeof(d->items[i]), "%s", ssid);
            else
                snprintf(d->items[i], sizeof(d->items[i]), "??");
        }
        if (d->sel >= d->count) d->sel = d->count - 1;
        ctx->needs_redraw = true;
    } else if (cnt == 0 && wifi_manager_is_scan_done() && d->count == 1) {
        /* 扫描完成但没搜到任何网络 → 显示"未找到", 避免一直卡在"正在扫描..." */
        snprintf(d->items[0], sizeof(d->items[0]), "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\xe7\xbd\x91\xe7\xbb\x9c"); /* 未找到网络 */
        ctx->needs_redraw = true;
    }
}
static void wifi_scan_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    wifi_manager_scan_stop();
    if (result < 0) return;   /* 返回 */
    char ssid[33];
    if (wifi_manager_get_scan_ssid(result, ssid, sizeof(ssid)) && ssid[0]) {
        /* 选网 → 同一弹窗原位变为"输密码+键盘"界面 (不再跳独立全屏键盘页). */
        open_wifi_pwd_dlg(ctx, ssid);
    }
}
static void open_wifi_scan(ui_ctx_t *ctx) {
    if (!wifi_manager_scan_start()) {          /* 失败 (含内部一次重试) → 提示并留在原菜单 */
        os_dialog_toast(ctx, "\xe6\x89\xab\xe6\x8f\x8f\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe9\x87\x8d\xe8\xaf\x95"); /* 扫描失败，请重试 */
        return;
    }
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f WiFi..."); /* 正在扫描 WiFi... */
    dlg.count = 1;
    dlg.sel = 0;
    dlg.on_poll = wifi_scan_dlg_poll;
    dlg.cb = wifi_scan_dlg_cb;
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(TAG, "WiFi 扫描开始");
}

static void open_wifi_dlg(ui_ctx_t *ctx, int sel) {
    char items[4][30];
    const char *ptrs[4];
    int n = 0;
    snprintf(items[n], sizeof(items[n]), "\xe8\x93\x9d\xe7\x89\x99:%s", os_hw_is_active(OS_HW_BT) ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 蓝牙:开/关 */
    ptrs[n] = items[n]; n++;
    snprintf(items[n], sizeof(items[n]), "WiFi:%s", os_hw_is_active(OS_HW_WIFI) ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* WiFi:开/关 */
    ptrs[n] = items[n]; n++;
    if (os_hw_is_active(OS_HW_WIFI)) {
        /* 已连 WiFi → 该项显示"已连接:<IP>"; 未连 → "连接WiFi" */
        char ip[16] = "";
        if (wifi_manager_get_ip(ip, sizeof(ip)) && ip[0])
            snprintf(items[n], sizeof(items[n]), "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5:%s", ip);  /* 已连接:<IP> */
        else
            snprintf(items[n], sizeof(items[n]), "\xe8\xbf\x9e\xe6\x8e\xa5WiFi");  /* 连接WiFi */
        ptrs[n] = items[n]; n++;
    }
    snprintf(items[n], sizeof(items[n]), "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf");  /* 系统信息 */
    ptrs[n] = items[n]; n++;
    sel = (sel < 0) ? 0 : (sel >= n ? n - 1 : sel);
    os_dialog_list(ctx, "", ptrs, n, sel, on_wifi_select, NULL);
}

/* ---- 系统信息: 只读列表弹窗, 每秒刷新(内存等实时值) ---- */
static void info_noop(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)result; (void)ud;   /* 只读, 项无操作; BACK 返回 */
}
static void info_refresh_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    static uint32_t last = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - last < 500) return;   /* 每 0.5s 重刷 */
    last = now;
    build_sysinfo();
    if (s_info_n > 8) s_info_n = 8;
    for (int i = 0; i < s_info_n; i++)
        snprintf(d->items[i], sizeof(d->items[i]), "%s", s_info_lines[i]);
    d->count = s_info_n;
    ctx->needs_redraw = true;
}
static void open_info_dlg(ui_ctx_t *ctx) {
    build_sysinfo();
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    if (s_info_n > 8) s_info_n = 8;
    for (int i = 0; i < s_info_n; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", s_info_lines[i]);
    dlg.count = s_info_n;
    dlg.sel = -1;            /* 只读, 无选中 */
    dlg.on_poll = info_refresh_poll;
    dlg.cb = info_noop;
    os_dialog_push(ctx, &dlg);
}

/* ---- 主菜单: 弹窗栈嵌套 (选子项不关主菜单, 子弹窗 BACK 逐层返回) ---- */
static void on_settings_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) os_pop(ctx);   /* 主菜单弹窗返回/取消 → 退出设置页 (回上一级) */
}

static bool settings_main_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    switch (a) {
    case OS_ACTION_UP:
        if (d->sel > 0) { d->sel--; ctx->needs_redraw = true; }
        return true;
    case OS_ACTION_DOWN:
        if (d->sel < 4) { d->sel++; ctx->needs_redraw = true; }   /* 5 项 (含喝杯水) */
        return true;
    case OS_ACTION_CONFIRM:
        if (d->sel >= d->count) return false;   /* 底部"返回"行 → 默认处理 (关闭) */
        switch (d->sel) {
        case 0: open_disp_dlg(ctx, 0); break;        /* 显示与时间 */
        case 1: open_snd_dlg(ctx, 0);  break;        /* 声音与反馈 */
        case 2: open_sleepgrp_dlg(ctx, 0); break;    /* 休眠与电源 */
        case 3: open_wifi_dlg(ctx, 0); break;        /* 网络与系统 (蓝牙/WiFi/连接WiFi/系统信息) */
        case 4: os_push(ctx, OS_PAGE_SPONSOR); break; /* 请作者喝杯饮料 → 全屏二维码弹窗 */
        default: break;
        }
        return true;   /* 不关闭主菜单弹窗, 子弹窗压栈 (BACK 可逐层返回) */
    default:
        return false;   /* BACK/LEFT/RIGHT 走默认 */
    }
}

static void p_settings_enter(ui_ctx_t *ctx) {
    static bool s_loaded = false;
    if (!s_loaded) { settings_load(); s_loaded = true; }
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 5;   /* 5 项: 显示与时间/声音与反馈/休眠与电源/网络与系统/请作者喝杯饮料 */
    for (int i = 0; i < 5; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", s_set_items[i]);
    dlg.sel = 0;
    dlg.on_key = settings_main_key;
    dlg.cb = on_settings_select;   /* BACK 关闭主菜单弹窗时 → 退出设置页 */
    os_dialog_push(ctx, &dlg);
}

static const os_module_t s_mod_settings = {
    .name       = "settings",
    .page_id    = OS_PAGE_SETTINGS,
    .on_enter   = p_settings_enter,
    .fullscreen = false,
};

/* ---- 暗黑模式游戏态同步 ----
 * 引擎进运行循环时 input_set_gamepad_gamemode(true) (游戏渲染中), 退出后=false.
 * 仅当 s_dark==1 (暗黑UI) 时反色随游戏态切换; ==2 (全部) 恒反色; ==0 (关闭) 恒不反色.
 * 关键: 游戏引擎运行是阻塞主循环的 (eng_task_run_loop while(1) / gam4980_emu_run),
 * 每帧 service tick 在游戏内不会执行 — 必须用 input_set_gamemode_cb 在进出游戏的
 * 瞬间即时同步, 否则"暗黑UI模式"进入游戏不会恢复正常 (画面仍反色). */
static void on_gamemode_changed(bool enabled) {
    (void)enabled;   /* settings_dark_apply_lcd 内部自判 input_is_game_mode() */
    settings_dark_apply_lcd(s_dark_lcd);
}

/* WiFi 连接结果后台轮询: 任意页面期间都能检测连接成功/失败并 toast 提示
 * (连接由"连接WiFi"密码页发起, 但连上可能发生在用户回到桌面之后). */
static const os_service_t s_svc_wifi_conn = {
    .name = "wifi_conn",
    .tick = wifi_conn_poll,
};

void os_page_settings_register(void) {
    os_register(&s_mod_settings);
    os_register_service(&s_svc_wifi_conn);
    input_set_gamemode_cb(on_gamemode_changed);
}
