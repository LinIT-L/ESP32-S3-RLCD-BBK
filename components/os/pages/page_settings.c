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

/* 主菜单项 (UTF-8): 显示配置/声音调整/无线连接/系统信息/请作者喝杯饮料 (底部"返回"由 os_dialog 自动加) */
static const char *const s_set_items[5] = {
    "\xe6\x98\xbe\xe7\xa4\xba\xe9\x85\x8d\xe7\xbd\xae",  /* 显示配置 */
    "\xe5\xa3\xb0\xe9\x9f\xb3\xe9\x9c\x87\xe5\x8a\xa8",  /* 声音震动 */
    "\xe6\x97\xa0\xe7\xba\xbf\xe8\xbf\x9e\xe6\x8e\xa5",  /* 无线连接 */
    "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf",  /* 系统信息 */
    "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe9\xa5\xae\xe6\x96\x99",  /* 请作者喝杯饮料 */
};

/* 震动开关状态 (vibrator 无 get, 自行跟踪 + NVS). V1.2.x: 拆为 UI / 游戏 两个独立通道 */
static bool s_vib_ui   = true;   /* UI 震动: 拖拉/菜单导航/设置调整 */
static bool s_vib_game = true;   /* 游戏震动: 游戏内按键 */
static bool s_key_sim  = true;   /* V1.4.x: 按键模拟 — 触摸点击=触控板"按下/释放"双脉冲 */
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
    if (nvs_get_u8(h, "font_light", &v) == ESP_OK) font_zh_set_style(v);
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
    s_info_n = 0;
    char b[32];
    snprintf(b, 32, "\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac:1.3"); /* 固件版本:1.3 */
    strcpy(s_info_lines[s_info_n++], b);
    snprintf(b, 32, "\xe5\x89\xa9\xe4\xbd\x99\xe5\x86\x85\xe5\xad\x98:%uKB", /* 剩余内存:XXKB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    strcpy(s_info_lines[s_info_n++], b);
    snprintf(b, 32, "\xe5\x89\xa9\xe4\xbd\x99PSRAM:%uKB", /* 剩余PSRAM:XXKB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    strcpy(s_info_lines[s_info_n++], b);
    snprintf(b, 32, "\xe4\xbd\x9c\xe8\x80\x85\xe4\xbf\xa1\xe6\x81\xaf:LinIT"); /* 作者信息:LinIT */
    strcpy(s_info_lines[s_info_n++], b);
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
static int s_wheel_sleep = 0;    /* 当前滚轮是否休眠模式 (时间=0) */
static int s_wheel_y0 = 0;       /* 滚轮当前值行顶 y (渲染时刷新, 触摸点击选档用) */
static const int s_sleep_mins[] = { 0, 1, 2, 3, 5, 10, 15, 20, 30 };
#define SLEEP_MINS_N (int)(sizeof(s_sleep_mins)/sizeof(s_sleep_mins[0]))
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
/* 某列 current+steps 档显示文本 (休眠到首尾返回空串=不画越界值) */
static void wheel_col_text(int col, int steps, char *out, size_t n) {
    if (s_wheel_sleep) {
        int idx = s_sleep_idx + steps;
        if (idx < 0 || idx >= SLEEP_MINS_N) { out[0] = '\0'; return; }
        int m = s_sleep_mins[idx];
        if (m <= 0) snprintf(out, n, "\xe6\xb0\xb8\xe4\xb8\x8d\xe4\xbc\x91\xe7\x9c\xa0"); /* 永不休眠 */
        else        snprintf(out, n, "%d\xe5\x88\x86\xe9\x92\x9f", m);                 /* X分钟 */
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
/* 调值: 休眠=翻档(夹紧首尾), 时间=调对应字段 */
static void wheel_adjust(int col, int delta) {
    if (s_wheel_sleep) {
        int nb = s_sleep_idx + delta;
        if (nb < 0) nb = 0;
        if (nb >= SLEEP_MINS_N) nb = SLEEP_MINS_N - 1;
        s_sleep_idx = nb;
    } else {
        time_adjust(tm_first() + col, delta);
    }
}
/* 确认: 休眠→NVS+息屏延时; 时间→RTC */
static void wheel_confirm(ui_ctx_t *ctx) {
    if (s_wheel_sleep) {
        int v = s_sleep_mins[s_sleep_idx];
        nvs_handle_t h;
        if (nvs_open(SLEEP_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "min", v); nvs_commit(h); nvs_close(h);
        }
        wp_screensaver_set_delay_ms((v <= 0) ? UINT32_MAX : ((uint32_t)v * 60000UL));
        os_dialog_toast(ctx, "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 休眠时间已设置 */
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
                if (nb < 0) nb = 0;
                if (nb >= SLEEP_MINS_N) nb = SLEEP_MINS_N - 1;
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

/* ---- 字体风格弹窗 ---- */
static void on_font_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) { font_zh_set_style(false); settings_save_u8("font_light", 0); os_dialog_toast(ctx, "\xe6\xa0\x87\xe5\x87\x86\xe5\xad\x97\xe4\xbd\x93"); }  /* 标准字体 */
    else if (result == 1) { font_zh_set_style(true); settings_save_u8("font_light", 1); os_dialog_toast(ctx, "\xe7\xbb\x86\xe4\xbd\x93\xe5\xad\x97\xe4\xbd\x93"); }  /* 细体字体 */
}
static void open_font_dlg(ui_ctx_t *ctx) {
    static const char *items[3] = { "\xe6\xa0\x87\xe5\x87\x86", "\xe7\xbb\x86\xe4\xbd\x93", NULL };  /* 标准/细体/返回 */
    os_dialog_list(ctx, "", items, 2, font_zh_is_light() ? 1 : 0, on_font_select, NULL);
}

/* ---- 暗黑模式三选弹窗: 关闭 / 暗黑UI模式(除游戏外反色) / 全部暗黑模式(含游戏) ---- */
static void on_dark_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;   /* 返回/取消: 不修改 */
    s_dark = result;          /* 0/1/2 */
    settings_save_dark();
    settings_dark_apply_lcd(ctx ? ctx->lcd : NULL);
    if (result == 0)      os_dialog_toast(ctx, "\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f\xe5\x85\xb3");   /* 暗黑模式关 */
    else if (result == 1) os_dialog_toast(ctx, "\xe6\x9a\x97\xe9\xbb\x91UI\xe6\xa8\xa1\xe5\xbc\x8f");          /* 暗黑UI模式 */
    else                 os_dialog_toast(ctx, "\xe5\x85\xa8\xe9\x83\xa8\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f"); /* 全部暗黑模式 */
}
static void open_dark_dlg(ui_ctx_t *ctx) {
    static const char *items[4] = {
        "\xe5\x85\xb3\xe9\x97\xad",          /* 关闭 */
        "\xe6\x9a\x97\xe9\xbb\x91UI\xe6\xa8\xa1\xe5\xbc\x8f",       /* 暗黑UI模式 */
        "\xe5\x85\xa8\xe9\x83\xa8\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f", /* 全部暗黑模式 */
        NULL
    };
    os_dialog_list(ctx, "", items, 3, s_dark, on_dark_select, NULL);
}

/* ---- 显示配置弹窗: 平铺 4 项 (日期/时间/字体/休眠) ---- */
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
    case 2: open_font_dlg(ctx); break;   /* 字体粗细 */
    case 3: open_sleep_dlg(ctx); break;  /* 休眠时间 */
    case 4: open_dark_dlg(ctx); break;   /* 暗黑模式: 三选弹窗 (关闭/暗黑UI/全部暗黑) */
    default: break;
    }
}
static void open_disp_dlg(ui_ctx_t *ctx) {
    static const char *items[5] = {
        "\xe6\x97\xa5\xe6\x9c\x9f\xe6\x9b\xb4\xe6\x94\xb9",  /* 日期更改 */
        "\xe6\x97\xb6\xe9\x97\xb4\xe8\xb0\x83\xe6\x95\xb4",  /* 时间调整 */
        "\xe5\xad\x97\xe4\xbd\x93\xe7\xb2\x97\xe7\xbb\x86",  /* 字体粗细 */
        "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4",  /* 休眠时间 */
        "\xe6\x9a\x97\xe9\xbb\x91\xe6\xa8\xa1\xe5\xbc\x8f",  /* 暗黑模式 */
    };
    os_dialog_list(ctx, "", items, 5, 0, on_disp_select, NULL);
}

/* ---- 声音调整弹窗: 声音开关/音量/震动 (动态显示状态) ---- */
static void open_snd_dlg(ui_ctx_t *ctx, int sel);
static void on_snd_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {   /* 声音开关 */
        bool m = !audio_player_is_muted();
        audio_player_set_muted(m);
        settings_save_u8("snd_on", m ? 0 : 1);
    } else if (result == 1) {   /* 音量步进 0/1/..9 档循环 */
        int st = audio_player_vol_to_step(audio_player_get_volume());
        int ns = (st + 1) % AUDIO_VOL_STEPS;
        int nv = audio_player_vol_to_percent(ns);
        audio_player_set_volume(nv);
        settings_save_u8("vol", (uint8_t)ns);   /* 存档位 0-9 */
    } else if (result == 2) {   /* UI 震动开关 */
        s_vib_ui = !s_vib_ui;
        vibrator_set_ui_enabled(s_vib_ui);
        settings_save_u8("vib_ui", s_vib_ui ? 1 : 0);
    } else if (result == 3) {   /* 游戏震动开关 */
        s_vib_game = !s_vib_game;
        vibrator_set_game_enabled(s_vib_game);
        settings_save_u8("vib_game", s_vib_game ? 1 : 0);
    } else if (result == 4) {   /* 按键模拟开关 (V1.4.x) */
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
    snprintf(items[4], sizeof(items[4]), "\xe6\x8c\x89\xe9\x94\xae\xe6\xa8\xa1\xe6\x8b\x9f:%s", s_key_sim ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 按键模拟:开/关 */
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3]; ptrs[4] = items[4];
    sel = (sel < 0) ? 0 : (sel > 4 ? 4 : sel);
    os_dialog_list(ctx, "", ptrs, 5, sel, on_snd_select, NULL);
}

/* ---- 无线连接弹窗: 蓝牙/WiFi 开关 + 连接WiFi (WiFi 开时显示) ---- */
static void open_wifi_dlg(ui_ctx_t *ctx, int sel);
static void open_wifi_scan(ui_ctx_t *ctx);
extern void wifi_kb_preset_ssid(const char *ssid);   /* WiFi 键盘页预填 SSID */

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
    } else if (result == 2) {   /* 连接WiFi → 列表弹窗扫描选择 (子弹窗, 压栈) */
        open_wifi_scan(ctx);
    }
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
    }
}
static void wifi_scan_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    wifi_manager_scan_stop();
    if (result < 0) return;   /* 返回 */
    char ssid[33];
    if (wifi_manager_get_scan_ssid(result, ssid, sizeof(ssid)) && ssid[0]) {
        /* 选网 → 预填 SSID, 只输密码. 只关掉扫描弹窗一层, 保留"无线连接
         * (蓝牙/WiFi)"弹窗在栈中, 然后把输密码页 push 上去; 这样在输密码页
         * 按返回会回到无线连接界面, 符合"弹窗/列表返回都回到上一个界面". */
        os_dialog_pop(ctx);
        wifi_kb_preset_ssid(ssid);
        os_push(ctx, OS_PAGE_WIFI_KB);
    }
}
static void open_wifi_scan(ui_ctx_t *ctx) {
    wifi_manager_scan_start();
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
        snprintf(items[n], sizeof(items[n]), "\xe8\xbf\x9e\xe6\x8e\xa5WiFi");  /* 连接WiFi */
        ptrs[n] = items[n]; n++;
    }
    sel = (sel < 0) ? 0 : (sel >= n ? n - 1 : sel);
    os_dialog_list(ctx, "", ptrs, n, sel, on_wifi_select, NULL);
}

/* ---- 系统信息: 列表弹窗 (只读展示) ---- */
static void info_noop(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)result; (void)ud;   /* 只读, 项无操作; BACK 返回 */
}
static void open_info_dlg(ui_ctx_t *ctx) {
    build_sysinfo();
    EXT_RAM_BSS_ATTR static char items[8][32];
    static const char *ptrs[8];
    for (int i = 0; i < s_info_n && i < 8; i++) {
        snprintf(items[i], sizeof(items[i]), "%s", s_info_lines[i]);
        ptrs[i] = items[i];
    }
    os_dialog_list(ctx, "", ptrs, s_info_n, -1, info_noop, NULL);
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
        case 0: open_disp_dlg(ctx); break;   /* 显示配置 */
        case 1: open_snd_dlg(ctx, 0);  break;   /* 声音调整 */
        case 2: open_wifi_dlg(ctx, 0); break;   /* 无线连接 */
        case 3: open_info_dlg(ctx); break;   /* 系统信息 */
        case 4: os_push(ctx, OS_PAGE_SPONSOR); break;   /* 请作者喝杯水 → 全屏二维码弹窗 */
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
    dlg.count = 5;   /* 5 项: 显示配置/声音调整/无线连接/系统信息/请作者喝杯水 */
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

void os_page_settings_register(void) {
    os_register(&s_mod_settings);
    input_set_gamemode_cb(on_gamemode_changed);
}
