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
#include "ui_common.h"
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
    "\xe5\xa3\xb0\xe9\x9f\xb3\xe8\xb0\x83\xe6\x95\xb4",  /* 声音调整 */
    "\xe6\x97\xa0\xe7\xba\xbf\xe8\xbf\x9e\xe6\x8e\xa5",  /* 无线连接 */
    "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf",  /* 系统信息 */
    "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe9\xa5\xae\xe6\x96\x99",  /* 请作者喝杯饮料 */
};

/* 震动开关状态 (vibrator 无 get, 自行跟踪 + NVS) */
static bool s_vib = true;

/* ============ NVS 持久化 ============ */
static void settings_save_u8(const char *k, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(SET_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, k, v);
        nvs_commit(h);
        nvs_close(h);
    }
}
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
    if (nvs_get_u8(h, "vib", &v) == ESP_OK)        { s_vib = v; vibrator_set_enabled(v); }
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
#define TIME_STEP_H  20   /* 滚轮相邻值行距 */
#define TIME_WHEEL_SIDE 4 /* 上下各显示的相邻值个数 */
static int s_fx[TIME_FIELD_N];   /* 字段列起始 x (渲染/点选共用, 每帧刷新) */
static int s_fw[TIME_FIELD_N];   /* 字段列宽 (数值+单位) */
static int s_time_mode = 0;      /* 0=日期(年/月/日) 1=时间(时/分/秒) */

static void open_time_dlg(ui_ctx_t *ctx);
static void open_time_wheel_dlg(ui_ctx_t *ctx);
/* 当前模式字段范围 */
static int tm_first(void) { return s_time_mode ? 3 : 0; }   /* 3=时 */
static int tm_last(void)  { return s_time_mode ? 5 : 2; }   /* 5=秒 / 2=日 */

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
/* 是否带单位后缀 (年/月/日), 时/分/秒用冒号 */
static const char *time_suffix(int f) {
    static const char *s[TIME_FIELD_N] = { "\xe5\xb9\xb4", "\xe6\x9c\x88", "\xe6\x97\xa5", "", ":", ":" };
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

static bool time_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    if (!s_edit_valid) {
        time_t t = time(NULL); struct tm *ct = localtime(&t);
        if (ct) s_edit_tm = *ct;
        s_edit_valid = true;
    }
    const int W = 350, H = 250;
    int bx = (UI_SCREEN_W - W) / 2, by = (UI_SCREEN_H - H) / 2;
    const int bbt = by + H - 2 - 44;   /* 底部确认/取消按钮区顶 */
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }

    /* 内容区 (按钮区上方) 垂直居中 */
    int cy = by + 2 + (bbt - (by + 2)) / 2;
    int y0 = cy - 12;

    /* 每列宽: 仅本模式的 3 个字段 */
    const int first = tm_first(), last = tm_last();
    int tw[TIME_FIELD_N], line_w = 0;
    for (int f = first; f <= last; f++) {
        char vs[8]; time_field_str(f, time_field_val(f), vs, sizeof(vs));
        const char *su = time_suffix(f);
        int gap = (f == first) ? 0 : ((f == 3 || f == 4) ? 12 : 18);
        tw[f] = text_width(vs) + (su[0] ? text_width(su) : 0) + gap;
        line_w += tw[f];
    }
    int x = bx + (W - line_w) / 2;
    for (int f = first; f <= last; f++) {
        char vs[8]; time_field_str(f, time_field_val(f), vs, sizeof(vs));
        const char *su = time_suffix(f);
        int vw = text_width(vs);
        s_fx[f] = x; s_fw[f] = tw[f];
        if (f == s_time_field) {
            /* 上方 4 个 / 下方 4 个相邻值 (滚轮) */
            for (int k = 1; k <= TIME_WHEEL_SIDE; k++) {
                char ps[8], ns[8];
                time_field_str(f, time_field_steps(f, -k), ps, sizeof(ps));
                time_field_str(f, time_field_steps(f, +k), ns, sizeof(ns));
                draw_text(lcd, x, y0 - k * TIME_STEP_H, ps, false);
                draw_text(lcd, x, y0 + k * TIME_STEP_H, ns, false);
            }
            /* 当前值: 仅数字反色高亮, 单位/冒号保持黑字 */
            fill_rect(lcd, x - 2, y0 - 2, x + vw + 1, y0 + 24 + 1, ST7305_COLOR_BLACK);
            draw_text(lcd, x, y0, vs, true);
            if (su[0]) draw_text(lcd, x + vw, y0, su, false);
        } else {
            draw_text(lcd, x, y0, vs, false);
            if (su[0]) draw_text(lcd, x + vw, y0, su, false);
        }
        x += tw[f];
    }

    /* 底部 确认/取消 两键 (参考小确认弹窗: 分隔线 + 中竖线 + 左右对称) */
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, ty, "\xe7\xa1\xae\xe8\xae\xa4", false);  /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, ty, "\xe5\x8f\x96\xe6\xb6\x88", false); /* 取消 */
    return true;
}

static bool time_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    const int first = tm_first(), last = tm_last();
    switch (a) {
    case OS_ACTION_LEFT:  s_time_field = (s_time_field > first) ? (s_time_field - 1) : last; ctx->needs_redraw = true; return true;
    case OS_ACTION_RIGHT: s_time_field = (s_time_field < last) ? (s_time_field + 1) : first; ctx->needs_redraw = true; return true;
    case OS_ACTION_UP:   time_adjust(s_time_field, +1); ctx->needs_redraw = true; return true;
    case OS_ACTION_DOWN: time_adjust(s_time_field, -1); ctx->needs_redraw = true; return true;
    case OS_ACTION_CONFIRM: {
        time_save_rtc();
        os_dialog_toast(ctx, "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); /* 时间已保存 */
        os_dialog_pop(ctx);
        return true;
    }
    default:
        return false;   /* BACK 走默认关闭 (取消) */
    }
}

static void time_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    const int first = tm_first(), last = tm_last();
    int bx = (UI_SCREEN_W - 350) / 2, by = (UI_SCREEN_H - 250) / 2;
    const int bbt = by + 250 - 2 - 44;   /* 底部按钮区顶 (与渲染一致) */
    int tx, ty;
    if (input_consume_tap(&tx, &ty)) {
        /* 底部 确认/取消 */
        if (ty >= bbt) {
            int mid = bx + 350 / 2;
            if (tx < mid) { time_save_rtc(); os_dialog_toast(ctx, "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); os_dialog_pop(ctx); } /* 确认 */
            else os_dialog_pop(ctx);   /* 取消 */
            return;
        }
        /* 内容区: 命中本模式字段 → 切换焦点 */
        for (int f = first; f <= last; f++) {
            if (tx >= s_fx[f] - 3 && tx <= s_fx[f] + s_fw[f] + 3) {
                if (s_time_field != f) { s_time_field = f; ctx->needs_redraw = true; }
                break;
            }
        }
    }
    /* 上下拖动: 调值 (上滑=+1, 下滑=-1, 跟手) */
    static int s_t_last = -1, s_t_acc = 0;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_t_last < 0) { s_t_last = ty; s_t_acc = 0; }
        else {
            s_t_acc += ty - s_t_last;
            s_t_last = ty;
            bool changed = false;
            while (s_t_acc <= -TIME_STEP_H) { time_adjust(s_time_field, +1); s_t_acc += TIME_STEP_H; changed = true; }
            while (s_t_acc >= +TIME_STEP_H) { time_adjust(s_time_field, -1); s_t_acc -= TIME_STEP_H; changed = true; }
            if (changed) ctx->needs_redraw = true;
        }
    } else {
        s_t_last = -1;
        s_t_acc = 0;
    }
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

/* 滚轮设置弹窗 (仅显示当前模式字段) */
static void open_time_wheel_dlg(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = time_render;
    dlg.on_key    = time_key;
    dlg.on_poll   = time_poll;
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

/* ---- 显示配置弹窗: 时间设置 + 字体风格 ---- */
static void on_disp_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) open_time_dlg(ctx);
    else if (result == 1) open_font_dlg(ctx);
}
static void open_disp_dlg(ui_ctx_t *ctx) {
    static const char *items[3] = { "\xe6\x97\xb6\xe9\x97\xb4\xe8\xae\xbe\xe7\xbd\xae", "\xe5\xad\x97\xe4\xbd\x93\xe9\xa3\x8e\xe6\xa0\xbc", NULL };  /* 时间设置/字体风格/返回 */
    os_dialog_list(ctx, "", items, 2, 0, on_disp_select, NULL);
}

/* ---- 声音调整弹窗: 声音开关/音量/震动 (动态显示状态) ---- */
static void open_snd_dlg(ui_ctx_t *ctx);
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
    } else if (result == 2) {   /* 震动开关 */
        s_vib = !s_vib;
        vibrator_set_enabled(s_vib);
        settings_save_u8("vib", s_vib ? 1 : 0);
    } else {
        return;   /* 返回/其它: 不刷新 */
    }
    /* 原地替换刷新 (先弹当前层再压回, 层级不加深; 返回直接回设置主菜单) */
    os_dialog_pop(ctx);
    open_snd_dlg(ctx);
}
static void open_snd_dlg(ui_ctx_t *ctx) {
    char items[4][30];
    const char *ptrs[4];
    snprintf(items[0], sizeof(items[0]), "\xe5\xa3\xb0\xe9\x9f\xb3:%s", audio_player_is_muted() ? "\xe5\x85\xb3" : "\xe5\xbc\x80");  /* 声音:关/开 */
    snprintf(items[1], sizeof(items[1]), "\xe9\x9f\xb3\xe9\x87\x8f:%d", /* 音量:N */
             audio_player_vol_to_step(audio_player_get_volume()));
    snprintf(items[2], sizeof(items[2]), "\xe9\x9c\x87\xe5\x8a\xa8:%s", s_vib ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 震动:开/关 */
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2];
    os_dialog_list(ctx, "", ptrs, 3, 0, on_snd_select, NULL);
}

/* ---- 无线连接弹窗: 蓝牙/WiFi 开关 + 连接WiFi (WiFi 开时显示) ---- */
static void open_wifi_dlg(ui_ctx_t *ctx);
static void open_wifi_scan(ui_ctx_t *ctx);
extern void wifi_kb_preset_ssid(const char *ssid);   /* WiFi 键盘页预填 SSID */
static void on_wifi_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {   /* 蓝牙开关 (懒加载 os_hw) */
        if (os_hw_is_active(OS_HW_BT)) os_hw_release(OS_HW_BT);
        else os_hw_request(OS_HW_BT);
        os_dialog_pop(ctx); open_wifi_dlg(ctx);   /* 原地替换, 层级不加深 */
    } else if (result == 1) {   /* WiFi 开关 */
        if (os_hw_is_active(OS_HW_WIFI)) os_hw_release(OS_HW_WIFI);
        else os_hw_request(OS_HW_WIFI);
        os_dialog_pop(ctx); open_wifi_dlg(ctx);   /* 原地替换, 层级不加深 */
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

static void open_wifi_dlg(ui_ctx_t *ctx) {
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
    os_dialog_list(ctx, "", ptrs, n, 0, on_wifi_select, NULL);
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
        case 1: open_snd_dlg(ctx);  break;   /* 声音调整 */
        case 2: open_wifi_dlg(ctx); break;   /* 无线连接 */
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

void os_page_settings_register(void) { os_register(&s_mod_settings); }
