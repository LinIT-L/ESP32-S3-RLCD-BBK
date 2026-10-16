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
#include "lintos_version.h"   /* ★ 版本管理: 固件版本/构建信息唯一来源 */
#include "os_hw.h"
#include "st7305.h"       /* st7305_set_inversion: 暗黑模式(面板反色) */
#include "ui_common.h"
#include "wallpapers.h" /* wp_screensaver_set_delay_ms: 设置休眠时间后更新息屏延时 */
#include "power_stat.h"  /* 电池信息页: 运行/睡眠/上次充电至今 三个时长 */
#include "input.h"       /* input_consume_tap / input_get_touch_pos (时间滚轮), esp_util.h */
#include "esp_attr.h"
#include "font_zh.h"
#include "audio_player.h"
#include "vibrator.h"
#include "wifi_manager.h"
#include "bt_manager.h"   /* bt_is_on(): 蓝牙实际开关状态 */
#include "keyboard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "board_battery.h"
#include "hw_cfg.h"
#include "svc_nvs.h"
#include "svc_cfg.h"
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
    svc_cfg_set_u8(SET_NS, k, v);
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
    if (svc_nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (svc_nvs_get_u8(h, "darkm", &v) == ESP_OK) {
            s_dark = (v <= 2) ? (int)v : 2;
        } else if (svc_nvs_get_u8(h, "dark", &v) == ESP_OK) {
            s_dark = (v != 0) ? 2 : 0;   /* 旧 1 → 全部暗黑, 行为不变 */
        }
        svc_nvs_close(h);
    }
    settings_dark_apply_lcd(lcd);
}
/* 保存暗黑模式 (写新 key "darkm", 不污染旧 key) */
static void settings_save_dark(void) { settings_save_u8("darkm", (uint8_t)s_dark); }
static void settings_load(void) {
    nvs_handle_t h;
    if (svc_nvs_open(SET_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    /* ⚠ 音量 "vol" 与声音开关 "snd_on" 已改由 **audio_player_init() 自己开机恢复**
     *   (2026-10-14: 原来只在这里恢复 ⇒ 不进设置页就永远停在默认 100%, 用户以为没生效)。
     *   这里不再重复处理, 保证单一归属。 */
    /* V1.2.x: 震动拆为 UI/游戏 两个独立开关 (NVS vib_ui / vib_game).
     * 旧固件的单键 "vib" 仅作首次迁移: 任一缺省时取旧值. */
    if (svc_nvs_get_u8(h, "vib_ui", &v) == ESP_OK)        s_vib_ui = v;
    else if (svc_nvs_get_u8(h, "vib", &v) == ESP_OK)      s_vib_ui = v;
    if (svc_nvs_get_u8(h, "vib_game", &v) == ESP_OK)      s_vib_game = v;
    else if (svc_nvs_get_u8(h, "vib", &v) == ESP_OK)      s_vib_game = v;
    vibrator_set_ui_enabled(s_vib_ui);
    vibrator_set_game_enabled(s_vib_game);
    /* V1.4.x: 按键模拟开关 (触控板式点击双脉冲) */
    if (svc_nvs_get_u8(h, "vib_key", &v) == ESP_OK) s_key_sim = v;
    vibrator_set_key_sim(s_key_sim);
    svc_nvs_close(h);
}

/* 系统信息弹窗内容 */
static char s_info_lines[8][32];
static int  s_info_n;
static void build_sysinfo(void) {
    /* 直接 snprintf 进固定缓冲(32B x 8), 免中间缓冲+strcpy, 天然边界安全.
     * ★ 2026-10-02 版本管理整合: 版本号/构建信息一律取自 lintos_version.h 的接口,
     *   **不再硬编码**（旧代码这里写死 "1.3", 而代码已到 V1.7.x → 显示的版本是错的）。
     *   中文 3 字节/字符, 故用 %.*s 显式限长, 避免 -Wformat-truncation。 */
    snprintf(s_info_lines[0], sizeof(s_info_lines[0]),
             "\xe5\xaf\xb9\xe5\xa4\x96\xe7\x89\x88\xe6\x9c\xac\x3a%.18s", os_version_public());   /* 对外版本:1.3   (只在推送时改) */
    snprintf(s_info_lines[3], sizeof(s_info_lines[3]),
             "\xe5\x89\xa9\xe4\xbd\x99PSRAM:%uKB", /* 剩余PSRAM:XXKB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(s_info_lines[4], sizeof(s_info_lines[4]),
             "\xe4\xbd\x9c\xe8\x80\x85\xe4\xbf\xa1\xe6\x81\xaf:LinIT"); /* 作者信息:LinIT */
    snprintf(s_info_lines[5], sizeof(s_info_lines[5]),
             "\xe6\x9e\x84\xe5\xbb\xba:%.24s", os_build_stamp());       /* 构建:2026-10-02 11:00 */
    snprintf(s_info_lines[6], sizeof(s_info_lines[6]),
             "\xe6\x8f\x90\xe4\xba\xa4:%.24s", os_git_desc());          /* 提交:v1.3.1-12-g88140ce */
    s_info_n = 7;
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
static int s_wheel_kind = 0;     /* 单列滚轮内容(命名以菜单标签为准): 0=壁纸时间写 os_wp/min
                                  * 1=休眠时间写 os_wp/wp_run. 注意 dialog 函数名叫 open_sleep_dlg/open_wp_dlg
                                  * 属历史命名, 与标签语义相反, 勿被函数名误导. */
static int s_wheel_y0 = 0;       /* 滚轮当前值行顶 y (渲染时刷新, 触摸点击选档用) */
static const int s_sleep_mins[] = { 0, 1, 2, 3, 5, 10, 15, 20, 30 };
#define SLEEP_MINS_N (int)(sizeof(s_sleep_mins)/sizeof(s_sleep_mins[0]))
/* 壁纸(屏保)运行时长档位: 1..30 分钟 (滚轮复用, 数据源=wp_run) */
static const int s_wpgrp_mins[] = { 0, 1, 2, 3, 5, 10, 15, 20, 30 };
#define WPGRP_MINS_N (int)(sizeof(s_wpgrp_mins)/sizeof(s_wpgrp_mins[0]))
/* 关机时间档位 (0=不关机; 含分钟~10天): 须在 wheel_kind_tab(上方)之前定义 */
static const int s_shutgrp_mins[] = { 0, 5, 10, 30, 60, 1440, 4320, 7200, 10080, 14400 };
#define SHUTGRP_MINS_N (int)(sizeof(s_shutgrp_mins)/sizeof(s_shutgrp_mins[0]))
static int sleep_min_get(void) {
    return (int)svc_cfg_get_i32(SLEEP_NS, "min", 10);
}
static int sleep_index_of(int min) { for (int i = 0; i < SLEEP_MINS_N; i++) if (s_sleep_mins[i] == min) return i; return 0; }

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
/* 单列模式数据源(与菜单标签一致): 0=壁纸时间(os_wp/min) 1=休眠时间(os_wp/wp_run) 2=关机时间(os_wp/wp_shut) */
static const int *wheel_kind_tab(int *n) {
    if (s_wheel_kind == 1) { *n = WPGRP_MINS_N; return s_wpgrp_mins; }
    if (s_wheel_kind == 2) { *n = SHUTGRP_MINS_N; return s_shutgrp_mins; }  /* 关机时间档位 */
    *n = SLEEP_MINS_N; return s_sleep_mins;
}
static void wheel_confirm_save(ui_ctx_t *ctx, int min) {
    const char *key = (s_wheel_kind == 1) ? "wp_run" : (s_wheel_kind == 2) ? "wp_shut" : "min";
    svc_cfg_set_i32(SLEEP_NS, key, min);
    if (s_wheel_kind == 1) {
        wp_screensaver_set_run_ms(min > 0 ? (uint32_t)min * 60000UL : UINT32_MAX);
        os_dialog_toast(ctx, "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 休眠时间已设置 */
    } else if (s_wheel_kind == 2) {
        wp_screensaver_set_shutdown_ms(min > 0 ? (uint32_t)min * 60000UL : 0);
        os_dialog_toast(ctx, "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 关机时间已设置 */
    } else {
        wp_screensaver_set_delay_ms(min <= 0 ? UINT32_MAX : (uint32_t)min * 60000UL);
        os_dialog_toast(ctx, "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"); /* 壁纸时间已设置 */
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

/* 时间设置入口: 先选「日期」/「时间」 */

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
/* 休眠时间(菜单标签): 复用同款滚轮 UI, 数据源=os_wp/wp_run (档位 s_wpgrp_mins).
 * 注: 函数名 open_wp_dlg / wpgrp_* 为历史命名(旧称"壁纸时间"), 按当前标签其为"休眠时间". */
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
    if (svc_nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t x = 0;
        if (svc_nvs_get_u8(h, "font_size", &x) == ESP_OK && x < 5) v = (int)x;
        svc_nvs_close(h);
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

/* 休眠相关子菜单 (V1.5.x): 壁纸时间 / 休眠时间 / 关机时间 / 电池信息 / 触摸唤醒
 * (linit 2026-10-14: 触摸唤醒常驻**最下面**, 开关类放末尾) */
static void open_disp_dlg(ui_ctx_t *ctx, int sel);
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel);
/* 休眠时间 (与壁纸页共用 NVS "os_wp"/"wp_run"; 档位数组见上方 s_wpgrp_mins) */
static int wpgrp_min_get(void) {
    return (int)svc_cfg_get_i32(SLEEP_NS, "wp_run", 10);
}
static int shutgrp_min_get(void) {
    return (int)svc_cfg_get_i32(SLEEP_NS, "wp_shut", 1440);
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
/* ============ 电池信息 (休眠与电源 → 电池信息) ============
 * 自绘整层弹窗 (on_render 全接管): 上方当前电压, 主体是**横向电压波形图**。
 *   数据 = board_battery_history() 的真实采样点 (60s 一条, 环形 ≤64 条 ≈ 近 1 小时),
 *   纵轴固定 3.3V(空) ~ 4.2V(满), 与电量换算同口径。
 * ★ 尺寸/底栏必须与 os_dialog 标准列表弹窗**完全一致** (linit 要求):
 *   350×250 (屏 400×300 − DLG_MARGIN 25×2) · 边框 2px ·
 *   底栏 = 40px 行高, 顶 = y + h − 4 − 40, 分隔线在其上一行且左右各内缩 6px, 文字居中。 */
#define BI_W       350                 /* = DLG_W: ui_screen_w() - 25*2 */
#define BI_H       250                 /* = DLG_H: ui_screen_h() - 25*2 */
#define BI_PAD     4                   /* = os_dialog 的 inner_pad */
#define BI_LINE_H  40                  /* = DLG_LINE_H (底栏行高) */
#define BI_V_MIN   3300u               /* 纵轴下限 mV */
#define BI_V_MAX   4200u               /* 纵轴上限 mV */
#define BI_FOOT_Y  (BI_H - BI_PAD - BI_LINE_H)   /* 底栏行顶 (相对弹窗): 206 */

static uint32_t s_bi_last_s;

/* 电压 → 图内 y (纵轴 3.3~4.2V 线性映射, 夹在画框内) */
static int bi_v_to_y(uint32_t mv, int gy0, int gy1) {
    if (mv < BI_V_MIN) mv = BI_V_MIN;
    if (mv > BI_V_MAX) mv = BI_V_MAX;
    int ih = gy1 - gy0 - 1;
    int y = gy1 - 1 - (int)((mv - BI_V_MIN) * (uint32_t)ih / (BI_V_MAX - BI_V_MIN));
    if (y < gy0 + 1) y = gy0 + 1;
    if (y > gy1 - 1) y = gy1 - 1;
    return y;
}

static bool batinfo_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;

    const int bx = (UI_SCREEN_W - BI_W) / 2, by = (UI_SCREEN_H - BI_H) / 2;
    const int fty = by + BI_FOOT_Y;                 /* 底栏行顶 (绝对) */

    /* 白底 + 2px 边框. ⛔ 上方不要标题「电池信息」和横杠;
     *    波形**直接画在这个弹窗边框里**, 不再自画图框 (linit 2026-10-14)。 */
    fill_rect(lcd, bx, by, bx + BI_W - 1, by + BI_H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + BI_W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + BI_W - 1 - k, by + BI_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + BI_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + BI_W - 1 - k, by + k, by + BI_H - 1 - k, ST7305_COLOR_BLACK);
    }

    /* ---- 历史样本: 1 字节/分钟, 保留 7 天 (PSRAM + 按天 NVS), 绘制时按窗口抽稀 ---- */
    static EXT_RAM_BSS_ATTR bat_sample_t smp[BOARD_BATTERY_WAVE_MAX];
    /* 窗口 = 实际已有数据跨度, 上限 7 天 ⇒ 有几条就撑满整宽 (linit: "实时填满") */
    uint32_t win = board_battery_hist_span_s();
    if (win > BOARD_BATTERY_SPAN_MAX) win = BOARD_BATTERY_SPAN_MAX;
    if (win < 600u) win = 600u;
    int n = board_battery_wave(smp, BOARD_BATTERY_WAVE_MAX, win);
    uint32_t now = board_battery_clock_s();
    uint32_t t0  = (now > win) ? (now - win) : 0;

    /* ---- 顶行: 电压靠左 / 百分比靠右 (linit: 贴两侧; 中间不显示时间) ---- */
    board_battery_status_t bat;
    uint32_t mv = 0, pct = 0;
    if (board_battery_read_cached(&bat) == ESP_OK) { mv = bat.voltage_mv; pct = bat.percent; }
    char vtxt[24];
    if (mv) snprintf(vtxt, sizeof(vtxt), "%u.%02uV", (unsigned)(mv / 1000), (unsigned)((mv % 1000) / 10));
    else    snprintf(vtxt, sizeof(vtxt), "无电池数据");
    const int ty = by + 8;
    draw_text(lcd, bx + 10, ty, vtxt, false);                       /* 左 */
    if (mv) {
        char ptxt[12];
        snprintf(ptxt, sizeof(ptxt), "%u%%", (unsigned)pct);
        draw_text(lcd, bx + BI_W - 10 - text_width(ptxt), ty, ptxt, false);   /* 右 */
    }

    /* ---- 电压波形: 直接铺在弹窗内框里 (横轴=时间) ---- */
    const int gx0 = bx + 6, gx1 = bx + BI_W - 7;
    const int gy0 = by + 36, gy1 = fty - 6;
    const int iw = gx1 - gx0 - 1;
    if (n >= 1 && win) {
        /* 相邻样本**直接连线** —— 曲线按真实时间铺在窗口里, 所以**深睡那段也被连起来**
         * (linit: "休眠的时候唤醒波形图, 中间也连接起来 这样就方便判断") */
        int px = -1, py = 0;
        for (int i = 0; i < n; i++) {
            if (smp[i].t < t0) continue;                       /* 窗口外的样本不画 */
            int x = gx0 + (int)((uint64_t)(smp[i].t - t0) * (uint32_t)iw / win);
            if (x > gx1) x = gx1;
            if (x < gx0) x = gx0;
            int y = bi_v_to_y(smp[i].mv, gy0, gy1);
            if (px < 0) { ui_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK); px = x; py = y; continue; }
            int lasty = -1;
            for (int X = px; X <= x; X++) {
                int yy = (x == px) ? y : (py + (int)((int32_t)(y - py) * (X - px) / (x - px)));
                if (lasty < 0) ui_draw_pixel(lcd, X, yy, ST7305_COLOR_BLACK);
                else draw_vline(lcd, X, (lasty < yy) ? lasty : yy,
                                         (lasty < yy) ? yy : lasty, ST7305_COLOR_BLACK);
                lasty = yy;
            }
            px = x; py = y;
        }
    } else if (n == 0) {
        const char *ph = "采样中...";
        draw_text(lcd, gx0 + (gx1 - gx0 - text_width(ph)) / 2, gy0 + (gy1 - gy0) / 2 - 12, ph, false);
    }

    /* ---- 底栏 (与标准列表弹窗同几何: 分隔线在行上一行, 左右内缩 6px; 文字 40px 行内居中) ---- */
    draw_hline(lcd, bx + 6, bx + BI_W - 7, fty - 1, ST7305_COLOR_BLACK);
    const char *ok = "退出";
    draw_text(lcd, bx + BI_W / 2 - text_width(ok) / 2, fty + (BI_LINE_H - 24) / 2, ok, false);
    return true;
}

static bool batinfo_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    const int bx = (UI_SCREEN_W - BI_W) / 2, by = (UI_SCREEN_H - BI_H) / 2;
    const int fty = by + BI_FOOT_Y;
    if (x < bx || x > bx + BI_W - 1 || y < by || y > by + BI_H - 1) return false;
    if (y >= fty) os_dialog_pop(ctx);   /* 底部"退出"行 → 关掉本弹窗 */
    return true;                        /* 窗内点击一律消费, 不穿透到下层 */
}

static void batinfo_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    /* 每秒刷一帧: 三个时长是秒级更新的; 电压本身 60s 才来一个新采样点 */
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s != s_bi_last_s) { s_bi_last_s = s; ctx->needs_redraw = true; }
}

static void open_batinfo_dlg(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;              /* 整层自绘 (含底部"退出") */
    dlg.on_render = batinfo_render;
    dlg.on_poll   = batinfo_poll;
    dlg.on_touch  = batinfo_touch;
    os_dialog_push(ctx, &dlg);
}

/* 休眠相关子菜单 → 选中项 */
static void on_sleepgrp_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0: open_sleep_dlg(ctx); break;          /* 壁纸时间 (os_wp/min: 无操作多久进壁纸) */
    case 1: open_wp_dlg(ctx); break;             /* 休眠时间 (os_wp/wp_run: 壁纸播多久进休眠) */
    case 2: open_shutgrp_min_dlg(ctx); break;    /* 关机时间 */
    case 3: open_batinfo_dlg(ctx); break;        /* 电池信息 (电压波形图) */
    case 4: {  /* 触摸唤醒壁纸开关 (V1.5.x, 常驻最下面) */
        bool en = !wp_screensaver_touch_wake_enabled();
        wp_screensaver_set_touch_wake(en);
        os_dialog_pop(ctx);
        open_sleepgrp_dlg(ctx, 4);
        os_dialog_toast(ctx, en ?
            "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x8f\xaf\xe5\x94\xa4\xe9\x86\x92\xe5\xa3\x81\xe7\xba\xb8" :   /* 触摸可唤醒壁纸 */
            "\xe8\xa7\xa6\xe6\x91\xb8\xe4\xb8\x8d\xe5\x94\xa4\xe9\x86\x92\xe5\xa3\x81\xe7\xba\xb8");  /* 触摸不唤醒壁纸 */
        break;
    }
    default: break;
    }
}
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel) {
    char items[5][24];
    const char *ptrs[5];
    snprintf(items[0], sizeof(items[0]), "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4");  /* 壁纸时间(os_wp/min=无操作多久进壁纸) */
    snprintf(items[1], sizeof(items[1]), "\xe4\xbc\x91\xe7\x9c\xa0\xe6\x97\xb6\xe9\x97\xb4");  /* 休眠时间(os_wp/wp_run=壁纸播多久进休眠) */
    snprintf(items[2], sizeof(items[2]), "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4");  /* 关机时间 */
    snprintf(items[3], sizeof(items[3]), "\xe7\x94\xb5\xe6\xb1\xa0\xe4\xbf\xa1\xe6\x81\xaf");  /* 电池信息 */
    snprintf(items[4], sizeof(items[4]), "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x94\xa4\xe9\x86\x92:%s",  /* 触摸唤醒:开/关 */
             wp_screensaver_touch_wake_enabled() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3]; ptrs[4] = items[4];
    sel = (sel < 0) ? 0 : (sel > 4 ? 4 : sel);
    os_dialog_list(ctx, "\xe4\xbc\x91\xe7\x9c\xa0\xe7\x9b\xb8\xe5\x85\xb3", ptrs, 5, sel, on_sleepgrp_select, NULL);
}

/* ---- 显示与时间弹窗: 平铺 4 项 (日期/时间/字体/暗黑; 休眠独立为主菜单项) ---- */
/* ---- 菜单主题 ----
 * 2026-09-22: 曾加过「显示与时间 → 菜单主题」入口 (当时有 4 个内置主题)。
 * linit 要求先只保留"默认"主题 (他要的主题是完全不同的视觉方案, 不是参数微调),
 * 只剩一项的菜单没意义 → 入口一并撤掉。真主题定了之后在这里再挂回来:
 *   open_theme_dlg() 用 os_theme_count()/os_theme_name(i) 列项,
 *   选中 → os_theme_set(i) + os_theme_save() + toast("已切换主题")。 */

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
        /* ★ 音量统一入口: 只调它, 持久化由 audio_player 自己负责 (存百分比)。
         *   ⛔ 不要再在这里 settings_save_u8("vol", sel) —— 两处存档会不一致。 */
        audio_player_set_volume(audio_player_vol_to_percent(sel));
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
static void open_hw_selfcheck_dlg(ui_ctx_t *ctx);   /* ★硬件自检 */
static void open_hw_cfg_dlg(ui_ctx_t *ctx);         /* ★硬件配置 */
static int  s_wifi_idx_selfcheck = -1, s_wifi_idx_hwcfg = -1;

/* USB 网卡菜单"连接WiFi"入口: 复用以设置页同样的扫描→输密码 流程.
 * 需在设置页同编译单元内, 故放这里并导出让 page_usb_net 调用. */
/* 追踪"本次扫码是否由我们开 WiFi": 仅在真正为我们开启时才负责关闭,
 * 避免误关用户本来就在用的 WiFi (如局域网服务/已连状态). */
/* ★ 蓝牙/WiFi 的"当前是否开着"一律以**底层实际状态**为准 (2026-10-02 linit 实测)：
 *   手柄已经连上、蓝牙明明在跑，"网络与系统"却显示「蓝牙:关」—— 因为原来只看
 *   os_hw_is_active()，那只是资源管家的**引用计数视图**。一旦某处直接调
 *   bt_manager_enable()/wifi_manager_enable()（或计数曾错位），底层在跑而计数为 0：
 *   ① 菜单显示错误；② 按开关会再 os_hw_request 一次 → 引用计数 +1 → **关不掉**。
 *   所以这里显示与开关都改问底层。 */
static bool bt_is_on(void)   { return bt_manager_is_ready() || bt_manager_is_enabled(); }
static bool wifi_is_on(void) { return wifi_manager_is_enabled() || os_hw_is_active(OS_HW_WIFI); }

static bool s_wifi_conn_we_opened = false;
void settings_wifi_connect_open(ui_ctx_t *ctx) {
    if (!wifi_is_on()) { os_hw_request(OS_HW_WIFI); s_wifi_conn_we_opened = true; }
    open_wifi_scan(ctx);
}
static void on_wifi_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    /* ★ 2026-10-16: 硬件自检 / 硬件配置 (位置动态, 用记录的下标判断) */
    if (result >= 0 && result == s_wifi_idx_selfcheck) { open_hw_selfcheck_dlg(ctx); return; }
    if (result == 0) {   /* 蓝牙开关 (懒加载 os_hw) */
        if (bt_is_on()) { os_hw_release(OS_HW_BT); bt_manager_disable(); }  /* 计数错位时兜底直关 */
        else            os_hw_request(OS_HW_BT);
        os_dialog_pop(ctx); open_wifi_dlg(ctx, result);   /* 原地替换, 层级不加深 */
    } else if (result == 1) {   /* WiFi 开关 */
        if (wifi_is_on()) { os_hw_release(OS_HW_WIFI); wifi_manager_disable(); }
        else              os_hw_request(OS_HW_WIFI);
        os_dialog_pop(ctx); open_wifi_dlg(ctx, result);   /* 原地替换, 层级不加深 */
    } else if (result == 2) {
        /* WiFi 开时 = "连接WiFi"; WiFi 关时该项为"系统信息" (固定末项映射) */
        if (wifi_is_on()) open_wifi_scan(ctx);
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
/* 手柄光标 (2026-10-02 linit): 用触屏时不需要选中态; **一旦按方向键**就出现黑块光标,
 * 方向键移动、确认键敲下当前键。触摸一下即隐藏光标 (回归纯触屏观感)。 */
static bool  s_wp_gp     = false;  /* 已用方向键 → 显示光标 */
static int   s_wp_sel_r  = 4, s_wp_sel_c = 2;   /* 默认停在"回车"(直接按确认就是连接) */

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

/* ---- 键盘单元格几何 (与渲染/触摸同一套数字; 行 r / 列 c) ----
 * 列数: r0/r1=10, r2=9, r3=10 (Shift + 8字母 + 删除), r4=3 (&123 + 空格 + 回车) */
static int wp_row_cols(int r) {
    return (r == 2) ? 9 : ((r == 4) ? 3 : 10);
}
static void wifi_pwd_connect(ui_ctx_t *ctx);   /* 定义在下方 (回车键要用) */
/* 敲下 (r,c) 键: 与触摸命中后做的事**完全一致** */
static void wp_apply(ui_ctx_t *ctx, int r, int c) {
    const char *const *ll = s_wp_sym ? wp_symk : wp_abc;
    if (r == 0 || r == 1) {
        char ch = wp_disp(ll[r][c]);
        if (ch && s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ch;
    } else if (r == 2) {
        char ch = wp_disp(ll[2][c]);
        if (ch && s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ch;
    } else if (r == 3) {
        if (c == 0)      { if (s_wp_sym) s_wp_sym = false; else s_wp_shift = !s_wp_shift; }
        else if (c == 9) { if (s_wifi_pwd_len > 0) s_wifi_pwd_len--; }
        else {
            char ch = wp_disp(ll[3][c - 1]);
            if (ch && s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ch;
        }
    } else {
        if (c == 0)      s_wp_sym = !s_wp_sym;
        else if (c == 1) { if (s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ' '; }
        else             wifi_pwd_connect(ctx);
    }
    ctx->needs_redraw = true;
}

/* 密码页发起连接后, 后台轮询连接结果并 toast 提示 */
static bool s_wifi_conn_pending = false;

static void wifi_pwd_connect(ui_ctx_t *ctx) {
    s_wifi_pwd_pass[s_wifi_pwd_len] = '\0';
    os_dialog_pop(ctx);                        /* 关密码键盘(扫描层) → 回"网络与系统"菜单 */
    os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5..."); /* 正在连接... */
    if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);   /* 与入口一致, 避免双重引用计数 */
    s_wifi_conn_we_opened = false;   /* 已确认连接意图: 后续取消不再释放(用户要保留 WiFi) */
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
    s_wp_gp = false;                    /* 一触摸就把手柄光标收掉 (不让触屏用户看到黑块) */
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
    if (a == OS_ACTION_BACK) {   /* 密码框返回 = 退出连接流程: 若我们开的且未连上则释放 WiFi */
        if (s_wifi_conn_we_opened && !wifi_manager_is_connected()) os_hw_release(OS_HW_WIFI);
        s_wifi_conn_we_opened = false;
        os_dialog_pop(ctx); return true;
    }
    /* 方向键 = 光标导航 (2026-10-02 linit: "用手柄按方向键的时候就要选中") */
    if (a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT || a == OS_ACTION_UP || a == OS_ACTION_DOWN) {
        s_wp_gp = true;                                  /* 出现黑块光标 */
        if (a == OS_ACTION_UP || a == OS_ACTION_DOWN) {
            int r = s_wp_sel_r + (a == OS_ACTION_DOWN ? 1 : -1);
            if (r < 0) r = 4; else if (r > 4) r = 0;     /* 5 排循环 */
            s_wp_sel_r = r;
            if (s_wp_sel_c >= wp_row_cols(r)) s_wp_sel_c = wp_row_cols(r) - 1;
        } else {
            int n = wp_row_cols(s_wp_sel_r);
            int c = s_wp_sel_c + (a == OS_ACTION_RIGHT ? 1 : -1);
            if (c < 0) c = n - 1; else if (c >= n) c = 0;
            s_wp_sel_c = c;
        }
        ctx->needs_redraw = true;
        return true;
    }
    if (a == OS_ACTION_CONFIRM) {
        if (s_wp_gp) wp_apply(ctx, s_wp_sel_r, s_wp_sel_c);   /* 光标可见 → 敲当前键 */
        else         wifi_pwd_connect(ctx);                   /* 纯触屏/未导航过 → 原行为: 直接连 */
        return true;
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
        if (ch == ' ') ch = (char)0xB7;               /* 空格显示为 · 占位(单字节) */
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
            bool sel = (s_wp_gp && s_wp_sel_r == r && s_wp_sel_c == i)
                       || (tv && ty >= kb0 + r * R && ty < kb0 + r * R + R && tx >= kx && tx < kx + 34);
            wp_draw_key(lcd, kx, kb0 + r * R, 34, lb, sel);
        }
        draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + (r + 1) * R - 1, ST7305_COLOR_BLACK);
    }
    /* 行2: 9 键居中 */
    for (int i = 0; i < 9; i++) {
        int kx = WKB_LX + i * 38;
        char lb[2] = { wp_disp(ll[2][i]), 0 };
        bool sel = (s_wp_gp && s_wp_sel_r == 2 && s_wp_sel_c == i)
                   || (tv && ty >= kb0 + 2 * R && ty < kb0 + 2 * R + R && tx >= kx && tx < kx + 38);
        wp_draw_key(lcd, kx, kb0 + 2 * R, 38, lb, sel);
    }
    draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + 3 * R - 1, ST7305_COLOR_BLACK);
    /* 行3: Shift + 8 字母 + 退格 */
    {
        int ry = kb0 + 3 * R;
        const char *sh = s_wp_sym ? "ABC"
                        : (s_wp_shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"); /* 小写/大写 */
        bool s0 = (s_wp_gp && s_wp_sel_r == 3 && s_wp_sel_c == 0)
                  || (tv && ty >= ry && ty < ry + R && tx >= WKB_LX && tx < WKB_LX + 50);
        wp_draw_key(lcd, WKB_LX, ry, 50, sh, s0);
        for (int i = 0; i < 8; i++) {
            int kx = WKB_LX + 50 + i * 30;
            char lb[2] = { wp_disp(ll[3][i]), 0 };
            bool se = (s_wp_gp && s_wp_sel_r == 3 && s_wp_sel_c == i + 1)
                      || (tv && ty >= ry && ty < ry + R && tx >= kx && tx < kx + 30);
            wp_draw_key(lcd, kx, ry, 30, lb, se);
        }
        bool s9 = (s_wp_gp && s_wp_sel_r == 3 && s_wp_sel_c == 9)
                  || (tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 290 && tx < WKB_LX + 342);
        wp_draw_key(lcd, WKB_LX + 290, ry, 52, "\xe5\x88\xa0\xe9\x99\xa4", s9); /* 删除 */
        draw_hline(lcd, WKB_LX, dx + WPW_W - 4, kb0 + 4 * R - 1, ST7305_COLOR_BLACK);
    }
    /* 行4: &123 + 空格(大, 往右) + 回车(原连接, 去删除并入退格) */
    {
        int ry = kb0 + 4 * R;
        bool s0 = (s_wp_gp && s_wp_sel_r == 4 && s_wp_sel_c == 0)
                  || (tv && ty >= ry && ty < ry + R && tx >= WKB_LX && tx < WKB_LX + 64);
        wp_draw_key(lcd, WKB_LX, ry, 64, s_wp_sym ? "ABC" : "&123", s0);
        bool s1 = (s_wp_gp && s_wp_sel_r == 4 && s_wp_sel_c == 1)
                  || (tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 64 && tx < WKB_LX + 64 + 218);
        wp_draw_key(lcd, WKB_LX + 64, ry, 218, "\xe7\xa9\xba\xe6\xa0\xbc", s1); /* 空格 */
        bool s3 = (s_wp_gp && s_wp_sel_r == 4 && s_wp_sel_c == 2)
                  || (tv && ty >= ry && ty < ry + R && tx >= WKB_LX + 282 && tx < WKB_LX + 342);
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
    s_wp_gp = false; s_wp_sel_r = 4; s_wp_sel_c = 2;   /* 光标默认停"回车" */
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
                snprintf(d->items[i], sizeof(d->items[i]), "%.*s", (int)sizeof(d->items[i]) - 1, ssid);
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
    if (result < 0) {   /* 返回/取消: 若本次仅为扫码而开 WiFi 且尚未连上 → 关掉, 避免射频常开 */
        if (s_wifi_conn_we_opened && !wifi_manager_is_connected()) os_hw_release(OS_HW_WIFI);
        s_wifi_conn_we_opened = false;
        return;
    }
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
    char items[8][32];
    const char *ptrs[8];
    int n = 0;
    snprintf(items[n], sizeof(items[n]), "\xe8\x93\x9d\xe7\x89\x99:%s", bt_is_on() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 蓝牙:开/关 */
    ptrs[n] = items[n]; n++;
    snprintf(items[n], sizeof(items[n]), "WiFi:%s", wifi_is_on() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* WiFi:开/关 */
    ptrs[n] = items[n]; n++;
    if (wifi_is_on()) {
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
    s_wifi_idx_selfcheck = n;
    snprintf(items[n], sizeof(items[n]), "\xe7\xa1\xac\xe4\xbb\xb6\xe7\xae\xa1\xe7\x90\x86");  /* 硬件管理 */
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

/* ============ ★ 硬件自检 / 硬件配置 (2026-10-16 linit 要求) ============ */
static const char *HWCFG_NAMES[6] = { "触摸", "SD卡", "音频", "电池", "GPS", "震动" };

/* 自检页: 板型 + 6 项"检测到/未检测到(开/关)" + 最后一行"重新自检" */
static int hwsc_build(char lines[][28]) {
    int k = 0;
    snprintf(lines[k++], 28, "板型:%s", hw_cfg_board_name());
    for (int i = 0; i < HW_COUNT; i++)
        snprintf(lines[k++], 28, "%s:%s%s", HWCFG_NAMES[i],
                 hw_cfg_detected((hw_id_t)i) ? "检测到" : "未检测到",
                 hw_cfg_enabled((hw_id_t)i) ? "(开)" : "(关)");
    snprintf(lines[k++], 28, "重新自检");
    return k;
}
static void hwsc_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    static uint32_t last = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - last < 1000) return;
    last = now;
    char L[8][28]; int n = hwsc_build(L);
    for (int i = 0; i < n; i++) snprintf(d->items[i], sizeof(d->items[i]), "%s", L[i]);
    d->count = n; ctx->needs_redraw = true;
}
/* ★ 只有 音频/GPS/震动 可点击开关; 触摸/SD卡/电池 只显示不可点(linit 2026-10-16) */
static bool hwmg_clickable(int id) {
    return (id == HW_AUDIO || id == HW_GPS || id == HW_VIB);
}
static int s_hwmg_id = -1;
static void hwmg_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0 && s_hwmg_id >= 0) {          /* 0=确定 1=取消 */
        hw_id_t id = (hw_id_t)s_hwmg_id;
        hw_cfg_set_enabled(id, !hw_cfg_enabled(id));
    }
    s_hwmg_id = -1;
    os_dialog_pop(ctx);
    open_hw_selfcheck_dlg(ctx);                   /* 重开刷新状态 */
}
static void hwsc_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    char L[8][28]; int n = hwsc_build(L);
    if (result == n - 1) {                 /* 最后一行 = 重新自检 */
        hw_cfg_rescan();
        os_dialog_pop(ctx);
        open_hw_selfcheck_dlg(ctx);
        os_dialog_toast(ctx, "已重新自检");
        return;
    }
    /* 第 1..HW_COUNT 行 = 硬件项(第 0 行是板型标题, 不可点) */
    if (result >= 1 && result <= HW_COUNT) {
        int id = result - 1;
        if (!hwmg_clickable(id)) return;                 /* 触摸/SD/电池: 点击无反应 */
        if (!hw_cfg_detected((hw_id_t)id)) return;       /* 未检测到: 无反应 */
        s_hwmg_id = id;
        char msg[48];
        snprintf(msg, sizeof(msg), "%s%s?", hw_cfg_enabled((hw_id_t)id) ? "禁用" : "启用",
                 HWCFG_NAMES[id]);
        os_dialog_confirm_ex(ctx, msg, 0, -1, hwmg_confirm_cb, NULL);   /* 左确定/右取消 */
    }
}
static void open_hw_selfcheck_dlg(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg; memset(&dlg, 0, sizeof(dlg));
    char L[8][28]; int n = hwsc_build(L);
    for (int i = 0; i < n; i++) snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", L[i]);
    dlg.count = n; dlg.sel = -1; dlg.on_poll = hwsc_poll; dlg.cb = hwsc_select;
    os_dialog_push(ctx, &dlg);
}

/* 配置页(2026-10-16 linit 定): 只列"已检测到"的硬件; 没检测到的不显示;
 * 已识别的无论当前开/关都列出, 点一下即切换并立即存 NVS */
static int s_hwcfg_list[HW_COUNT];       /* 行号 → hw_id 映射(只含检测到的) */
static void hwcfg_pass1(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0) return;
    int n = 0;
    for (int i = 0; i < HW_COUNT; i++) if (hw_cfg_detected((hw_id_t)i)) n++;
    if (result >= n) return;
    hw_id_t id = (hw_id_t)s_hwcfg_list[result];
    hw_cfg_set_enabled(id, !hw_cfg_enabled(id));
    os_dialog_pop(ctx);
    open_hw_cfg_dlg(ctx);                 /* 重开以刷新开关状态 */
}
static void open_hw_cfg_dlg(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg; memset(&dlg, 0, sizeof(dlg));
    int n = 0;
    for (int i = 0; i < HW_COUNT; i++) {
        if (!hw_cfg_detected((hw_id_t)i)) continue;      /* ★没检测到的不显示 */
        s_hwcfg_list[n] = i;
        snprintf(dlg.items[n], sizeof(dlg.items[n]), "%s:%s", HWCFG_NAMES[i],
                 hw_cfg_enabled((hw_id_t)i) ? "开" : "关");
        n++;
    }
    if (n == 0) { os_dialog_toast(ctx, "没有检测到可配置的硬件"); return; }
    dlg.count = n; dlg.sel = 0; dlg.cb = hwcfg_pass1;
    os_dialog_push(ctx, &dlg);
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
