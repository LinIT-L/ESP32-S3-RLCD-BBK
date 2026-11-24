/**
 * page_settings.c — 设置 页面模块.
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 4 项 (显示配置/声音调整/无线连接/系统信息).
 *   - 显示与声音: 声音开关 / 字体字号 / 黑暗模式 / 调整日期 / 修改时间
 *     (★ 2026-11-24: 「字体修改」二级入口与宋/黑体切换已删除, 字号直接在主菜单)
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
#include "wallpapers.h" /* 屏保/省电: wp_screensaver_touch_wake_enabled / wp_eco_* / wp_shut_mins_norm */
#include "power_stat.h"  /* 电池信息页: 运行/睡眠/上次充电至今 三个时长 */
/* ★ 2026-11-22: 不再 include ltc2954.h —— 「硬件关机」开关已从本页移除，
 *   有无 LTC2954 由 ltc2954.c 按"是否识别到时钟芯片(0x68)"自动判定。 */
#include "input.h"       /* input_consume_tap / input_get_touch_pos (时间滚轮), esp_util.h */
#include "esp_attr.h"
#include "font_zh.h"
#include "glyph.h"       /* 统一字形入口 (2026-11-24: 字号 16/24 各取原生宋体) */
#include "audio_player.h"
#include "vibrator.h"
#include "haptic.h"      /* ★ 2026-11-24: 「丰富震动」开关 (效果库唯一归属, 含 NVS 持久化) */
#include "wifi_manager.h"
#include "bt_manager.h"   /* bt_is_on(): 蓝牙实际开关状态 */
#include "gps.h"          /* gps_power_state(): 硬件管理里 GPS 显示"休眠"而非"未检测到" */
#include "svc_rtc.h"      /* 手动设完时间立刻写回 DS3231 时间芯片 */
#include "svc_time.h"     /* ★ 2026-11-24: 手动设时统一走 svc_time（唯一写时间入口） */
#include "kbd_c42.h"   /* 42 键紧凑键盘 (WiFi 密码弹窗用; 与网络检测页同一份实现) */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "board_battery.h"
#include "hw_cfg.h"
#include "esp_netif.h"   /* 网络配置页: IP/掩码/网关/DNS */
#include "esp_wifi.h"    /* 网络配置页: RSSI/加密方式/信道 */
#include "svc_nvs.h"
#include "svc_cfg.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "str_trunc.h"   /* str_trunc(): 短字段的有意截断拷贝 */

/* 性能监视: 2026-11-03 起**只从「应用中心→应用→系统」进**(独立应用页)。
 * 本页的「性能监视」菜单项已删除, 不再需要 perfmon_open()。
 * (page_perfmon.c 里那个 350×250 弹窗实现仍在, 只是没有 UI 入口了。) */

#define TAG "PSET"
#define SET_NS "os_settings"

/* 主菜单 5 项 (2026-10-29 按 linit 目标结构调整):
 *   显示与声音 / 触摸与反馈 / 电源与休眠 / 系统与连接 / 请作者喝杯饮料
 * 变化: 原「显示与时间」「声音与反馈」合并为「显示与声音」;
 *       震动两项从声音里独立出来, 与新增的「触摸校正」组成「触摸与反馈」;
 *       「休眠与电源」改名「电源与休眠」（功能不变）; 「网络与系统」改名「系统与连接」。 */
static const char *const s_set_items[5] = {
    "\xe6\x98\xbe\xe7\xa4\xba\xe4\xb8\x8e\xe5\xa3\xb0\xe9\x9f\xb3",              /* 显示与声音 */
    "\xe8\xa7\xa6\xe6\x91\xb8\xe4\xb8\x8e\xe5\x8f\x8d\xe9\xa6\x88",              /* 触摸与反馈 */
    "\xe7\x94\xb5\xe6\xba\x90\xe4\xb8\x8e\xe4\xbc\x91\xe7\x9c\xa0",              /* 电源与休眠 */
    "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xb8\x8e\xe8\xbf\x9e\xe6\x8e\xa5",              /* 系统与连接 */
    "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe9\xa5\xae\xe6\x96\x99",  /* 请作者喝杯饮料 */
};

/* 震动开关状态 (vibrator 无 get, 自行跟踪 + NVS). V1.2.x: 拆为 UI / 游戏 两个独立通道 */
static bool s_vib_ui   = true;   /* UI 震动: 拖拉/菜单导航/设置调整 */
static bool s_vib_game = true;   /* 游戏震动: 游戏内按键 */
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
    /* ★ 2026-10-24: "触摸反馈"已并入 UI 震动, 不再单独读 NVS vib_key */
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
    /* ★ 2026-10-16: 行号紧凑排列(0..4) —— 删掉"内部版本"后必须连续编号, 否则中间空行 */
    snprintf(s_info_lines[0], sizeof(s_info_lines[0]),
             "\xe7\x89\x88\xe6\x9c\xac\xe5\x8f\xb7:%.18s", os_version_public());   /* 版本号:1.4 */
    /* ★★★ 2026-10-18 linit: 核心内存用**英文标签 + KB 单位**显示, 只显示空闲量。
     *   RAM = 内核内部 RAM(最紧张的资源), PSRAM = 外部 8MB。 */
    snprintf(s_info_lines[1], sizeof(s_info_lines[1]),
             "RAM:%uKB",                  /* 核心内部 RAM 空闲 KB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    snprintf(s_info_lines[2], sizeof(s_info_lines[2]),
             "PSRAM:%uKB",                /* 外部 PSRAM 空闲 KB */
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(s_info_lines[3], sizeof(s_info_lines[3]),
             "\xe4\xbd\x9c\xe8\x80\x85\xe4\xbf\xa1\xe6\x81\xaf:LinIT"); /* 作者信息:LinIT */
    snprintf(s_info_lines[4], sizeof(s_info_lines[4]),
             "\xe6\x9e\x84\xe5\xbb\xba:%.24s", os_build_stamp());       /* 构建:2026-10-02 11:00 */
    /* ★ 2026-10-18 linit: 删掉"提交:xxx" 那一行 —— 内容是 git 描述(v1.4.0-3-g88140ce)，
     *   英文+哈希对用户没意义，需要看版本时看"版本号/构建"两行就够。 */
    s_info_n = 5;
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

/* 单列滚轮: 服务「壁纸时间」(NVS os_wp/min) 与「关机时间」(NVS os_wp/wp_shut) */
#define SLEEP_NS "os_wp"
static int s_sleep_idx = 0;      /* 滚轮当前档位下标 */
static int s_wheel_sleep = 0;    /* 当前滚轮是否单列模式 (时间=0) */
static int s_wheel_kind  = 0;    /* 单列数据源: 0=壁纸时间(os_wp/min) 1=关机时间(os_wp/wp_shut) */
static int s_wheel_y0 = 0;       /* 滚轮当前值行顶 y (渲染时刷新, 触摸点击选档用) */
/* ★ 2026-11-24 linit: 「壁纸时间」恢复为可见可调项 —— 无操作多久进壁纸。
 *   0 = 不自动进入; 其余为分钟。档位与历史一致(老 NVS 值都能命中)。 */
static const int s_sleep_mins[] = { 0, 1, 2, 3, 5, 10, 15, 20, 30 };
#define SLEEP_MINS_N (int)(sizeof(s_sleep_mins)/sizeof(s_sleep_mins[0]))
/* ★ 2026-11-24(终版): 关机时间档位 = 不关机 / 1 / 3 / 5 / 10 / 20 / 30 分 /
 *   1 / 2 / 3 / 6 / 12 小时。**用户选什么就以什么为准**。
 *   表必须与 wallpapers.c 的 k_shut_mins[] 一一对应(显示与生效同源);
 *   老 NVS 里的 15、1440 之类由 wp_shut_mins_norm() 折到"不超过它的最大档"。 */
static const int s_shutgrp_mins[] = { 0, 1, 3, 5, 10, 20, 30, 60, 120, 180, 360, 720 };
#define SHUTGRP_MINS_N (int)(sizeof(s_shutgrp_mins)/sizeof(s_shutgrp_mins[0]))
/* 单列滚轮的档位表 (渲染/夹紧/确认共用一份, 按 s_wheel_kind 选) */
static const int *wheel_kind_tab(void) { return (s_wheel_kind == 1) ? s_shutgrp_mins : s_sleep_mins; }
static int wheel_kind_n(void) { return (s_wheel_kind == 1) ? SHUTGRP_MINS_N : SLEEP_MINS_N; }
/* 时长文本: 60 的整数倍且 ≥60 用"小时"(表里 60/120/180/360/720 都是整小时), 其余用"分"。
 * 0 不在这里处理 —— 调用方按语境写"不关机 / 不进入"。 */
static void fmt_span(int mins, char *out, size_t n) {
    if (mins >= 60 && (mins % 60) == 0)
        snprintf(out, n, "%d\xe5\xb0\x8f\xe6\x97\xb6", mins / 60);   /* N小时 */
    else
        snprintf(out, n, "%d\xe5\x88\x86", mins);                      /* N分 */
}

static void open_time_wheel_dlg(ui_ctx_t *ctx);
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel);   /* 滚轮确认后按新值重建(见 wheel_confirm) */
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
/* 保存当前草稿到系统时钟 + 时间芯片 */
static void time_save_rtc(void) {
    struct tm t = s_edit_tm; t.tm_isdst = -1;
    time_t ts = mktime(&t);
    if (ts == (time_t)-1) return;
    /* ★ 2026-11-24 linit「收口时间修改入口」: 原来这里直接 settimeofday,
     *   绕过了 svc_time ⇒ 系统时间变了但时间状态 / 上次校准记录 / NVS 还是旧的:
     *   下一次开机可能被判"时间倒退"落到 INVALID, 周期校准还会拿 RTC 旧值把它覆盖回来。
     *   现在统一走服务: 设时间 + 置状态 + 落盘一次做完。
     *   （svc_time_set_manual 内部就是唯一那处 settimeofday。） */
    if (!svc_time_set_manual((int64_t)ts)) return;
    /* 手动设的时间必须落到 DS3231 上，否则断电就白设了
     * （svc_rtc_save_now 内部判断"有没有芯片"，没有就什么也不做） */
    svc_rtc_save_now();
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
/* 单列模式数据源: 只有「关机时间」(os_wp/wp_shut) */
/* 单列确认: 按 s_wheel_kind 写对应的 NVS 键(os_wp/min 或 os_wp/wp_shut)。
 * ★ 2026-11-24(终版) linit: **以用户手动设置为准**。
 *   省电模式只在"打开的那一刻"把两档快速写成 1 分 / 3 分(一次性), 之后不再持续强制;
 *   所以这里只存值 + 重载, **不动省电模式开关** —— 用户改完时间, 省电的无线电闲置
 *   关闭照样生效, 不会出现"改了个时间把省电也关了"的连带副作用。
 *   ⛔ 不要用 wp_screensaver_set_shutdown_ms()/set_delay_ms(): 那是"现场诊断覆盖"语义。 */
static void wheel_confirm_save(ui_ctx_t *ctx) {
    (void)ctx;
    const bool is_shut = (s_wheel_kind == 1);
    int m = wheel_kind_tab()[s_sleep_idx];
    svc_cfg_set_i32(SLEEP_NS, is_shut ? "wp_shut" : "min", m);
    wp_screensaver_reload();   /* 让缓存与 NVS 同源 ⇒ 立即按新档生效 */
}
/* 某列 current+steps 档显示文本 (到首尾返回空串=不画越界值) */
static void wheel_col_text(int col, int steps, char *out, size_t n) {
    if (s_wheel_sleep) {
        const int *tab = wheel_kind_tab();
        int idx = s_sleep_idx + steps;
        if (idx < 0 || idx >= wheel_kind_n()) { out[0] = '\0'; return; }
        int m = tab[idx];
        if (m <= 0) {
            snprintf(out, n, (s_wheel_kind == 1)
                ? "\xe4\xb8\x8d\xe5\x85\xb3\xe6\x9c\xba"      /* 不关机 */
                : "\xe4\xb8\x8d\xe8\xbf\x9b\xe5\x85\xa5");    /* 不进入 */
        } else {
            fmt_span(m, out, n);          /* X分 / N小时 */
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
/* 调值: 单列=翻档(夹紧首尾), 时间=调对应字段
 * ★★★ 2026-11-24 linit「滚动调整壁纸时间/关机时间/日期/时间都没有轻震动」:
 *   原来只有"拨到头"才 vibrator_sem(VIB_SEM_BOUNDARY), 正常翻一档**完全不震** ——
 *   用户按住拖动滚轮时没有任何格子感, 分不清滚了几格、甚至怀疑没反应。
 *   现在每**真的**翻一档就 VIB_SEM_STEP (12ms/80, 项目既有的"调值一格"语义,
 *   与 os_dialog 的档位滚轮同一手感), 到头仍旧走 BOUNDARY, 两者不重复。
 *   时间/日期字段同理, 但只在**值真的变了**才震: 年 2020~2099 夹紧、大月调日被夹到 28
 *   这类"到头"不该假装有反馈。 */
static void wheel_adjust(int col, int delta) {
    if (s_wheel_sleep) {
        int nb = s_sleep_idx + delta;
        if (nb < 0) nb = 0;
        if (nb >= wheel_kind_n()) nb = wheel_kind_n() - 1;
        /* ★ 2026-11-24: 已经到头再拨 ⇒ "越界"语义(轻双击)。滚轮/音量到 0 或满都有反馈,
         *   不至于"按了没反应, 分不清是到头还是丢键"。 */
        if (nb == s_sleep_idx) { vibrator_sem(VIB_SEM_BOUNDARY); return; }
        s_sleep_idx = nb;
        vibrator_sem(VIB_SEM_STEP);          /* 翻档一格: 轻震一下 */
    } else {
        int f = tm_first() + col;
        int before = time_field_val(f);
        time_adjust(f, delta);
        if (time_field_val(f) != before) vibrator_sem(VIB_SEM_STEP);
    }
}
/* 确认: 单列→NVS+对应延时; 时间→RTC */
static void wheel_confirm(ui_ctx_t *ctx) {
    if (s_wheel_sleep) {
        const int kind = s_wheel_kind;        /* 0=壁纸时间 1=关机时间 */
        wheel_confirm_save(ctx);
        /* ★ 2026-11-24: 父列表的条目文本是**打开弹窗时**拼好的字符串, 只 pop 滚轮的话
         *   父列表仍显示旧值("改成 3 分钟, 列表还写 1 分钟")。这里按省电开关同一条路径:
         *   关滚轮 → 关旧父列表 → 用新值重建 → 再弹提示。 */
        os_dialog_mark_handled(ctx);   /* 自己接管弹栈, 阻止框架再自动 pop 一层 */
        os_dialog_pop(ctx);
        os_dialog_pop(ctx);
        open_sleepgrp_dlg(ctx, kind);
        os_dialog_toast(ctx, kind
            ? "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae"   /* 关机时间已设置 */
            : "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae");  /* 壁纸时间已设置 */
        return;
    }
    time_save_rtc();
    os_dialog_toast(ctx, "\xe6\x97\xb6\xe9\x97\xb4\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98"); /* 时间已保存 */
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
                /* ⛔ 必须用当前表长度: 壁纸时间 9 档 / 关机时间 12 档。
                 *   原来写死 SHUTGRP_MINS_N ⇒ 壁纸时间点选会越界读到表外(显示错档)。 */
                if (nb >= wheel_kind_n()) nb = wheel_kind_n() - 1;
                if (nb != s_sleep_idx) {
                    s_sleep_idx = nb;
                    vibrator_sem(VIB_SEM_STEP);   /* 点击选档: 与拖动翻档同一手感 */
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

/* ★ 2026-11-24 linit: 「壁纸时间」入口**恢复**(见 open_wp_min_dlg)。
 *   之前的做法是把它删掉、由省电模式固定 1 分钟 —— 但那样等于把手动设置藏起来:
 *   用户既看不到当前值, 也没法调。现在改为"省电模式=一键快速设置(强制写 1分/3分),
 *   壁纸时间本身仍是可调的正式设置项, 手动改就退出省电模式", 不再有假设置。 */

/* ---- 字体大小档位 (V1.7.x 保留: 供 ui_common/page_main 全局渲染字号) ---- */
/* ★ 2026-11-22 linit「设置→显示与声音→字体修改」: 恢复为**两档**
 *   0 = 16px, 4 = 24px (中间 18/20/22 三档不再暴露给用户, 但映射表保留)。
 *   缓存 s_font_size: draw_text 每帧都会问字号, 不能每次都开 NVS。 */
static int s_font_size = -1;   /* -1 = 未加载 */
static int font_size_get(void) {
    if (s_font_size >= 0) return s_font_size;
    int v = 4;                                   /* 默认 24px = 原生字模 */
    nvs_handle_t h;
    if (svc_nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t x = 0;
        if (svc_nvs_get_u8(h, "font_size", &x) == ESP_OK) v = (x == 0) ? 0 : 4;  /* 只认两档 */
        svc_nvs_close(h);
    }
    s_font_size = v;
    return v;
}
void settings_font_size_set(int v) {
    if (v != 0 && v != 4) v = 4;
    if (s_font_size == v) return;
    s_font_size = v;
    settings_save_u8("font_size", (uint8_t)v);
}
int settings_font_size(void) { return font_size_get(); }   /* 档位 0..4 供主菜单标签调字号 */

/* 字体族开机恢复 —— 必须在**第一次绘制之前**调用。
 * ★ 2026-11-24: 族已不参与选源 (glyph 现在按目标字号取原生宋体), 这里保留只为
 *   兼容 NVS 里可能存着的 font_fam 值; 调不调都不再影响任何字形。
 * 字体族本身存在 glyph 模块内存里, NVS 读写留在本文件 (apps 层), 保持分层单向。 */
void settings_font_apply(void) {
    uint8_t v = GLYPH_FAM_HEI;
    nvs_handle_t h;
    if (svc_nvs_open(SET_NS, NVS_READONLY, &h) == ESP_OK) {
        if (svc_nvs_get_u8(h, "font_fam", &v) != ESP_OK) v = GLYPH_FAM_HEI;
        svc_nvs_close(h);
    }
    glyph_set_family((v < GLYPH_FAM_MAX) ? (glyph_fam_t)v : GLYPH_FAM_HEI);
}

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
    /* 2026-10-29 linit: 只有 3 个选项, 用滚轮会觉得上下留白不均;
     * 改成列表弹窗 —— 三项平铺、弹窗高度随项数收紧, 视觉上自然居中。 */
    os_dialog_list(ctx, "", ptrs, 3, s_dark, on_dark_wheel, NULL);
}

/* 电源与休眠子菜单: 壁纸时间 / 关机时间 / 电池信息 / 省电模式 (开关类在最下面) */
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel);
/* 壁纸时间 (os_wp/min): 无操作多久 → 进壁纸/屏保。0 = 不自动进入。
 * ★ 2026-11-24 linit: 从"被省电模式固定 1 分钟"恢复为用户可调项。
 *   省电模式只是**一键快速设置**(打开时把这里强制写成 1 分), 手动改即退出省电模式。 */
static int wp_min_get(void) {
    return (int)svc_cfg_get_i32(SLEEP_NS, "min", 1);
}
static int wp_min_index_of(int min) {
    for (int i = 0; i < SLEEP_MINS_N; i++) if (s_sleep_mins[i] == min) return i;
    return 1;   /* 未知值 → 1 分钟(安全默认, 不是"不进入") */
}
static void open_wp_min_dlg(ui_ctx_t *ctx) {
    /* 复用时间滚轮的单列模式: 同视觉/拖动手势/点击选档 */
    s_wheel_sleep = 1;
    s_wheel_kind  = 0;                        /* 壁纸时间 */
    s_sleep_idx = wp_min_index_of(wp_min_get());
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = wheel_render;
    dlg.on_key    = wheel_key;
    dlg.on_poll   = wheel_poll;
    dlg.on_touch  = wheel_touch;
    os_dialog_push(ctx, &dlg);
}
/* 关机时间 (os_wp/wp_shut): 屏保播满该时长 → 自动关机
 * ★ 2026-11-24: 档位收敛为 1/3/5, **出厂默认 3 分钟**。
 *   老 NVS 里的 10/30/60/1440… 由 wp_shut_mins_norm() 就地折算(不清 NVS),
 *   所以这里读出来也要过一遍归一 —— 否则老用户会看到界面选中"不关机"。 */
static int shutgrp_min_get(void) {
    /* ⚠ 必须与 wallpapers.c:wp_ss_prime() 同一口径(那里也调 wp_shut_mins_norm)。 */
    return wp_shut_mins_norm((int)svc_cfg_get_i32(SLEEP_NS, "wp_shut", 3));
}
static int shutgrp_index_of(int min) {
    for (int i = 0; i < SHUTGRP_MINS_N; i++) if (s_shutgrp_mins[i] == min) return i;
    /* 归一后必命中; 万一没命中, 落到"3 分钟"这个默认档(绝不落到 0=不关机) */
    for (int i = 0; i < SHUTGRP_MINS_N; i++) if (s_shutgrp_mins[i] == 3) return i;
    return 0;
}
static void open_shutgrp_min_dlg(ui_ctx_t *ctx) {
    /* 复用时间滚轮的单列模式: 同视觉/拖动手势/点击选档 */
    s_wheel_sleep = 1;
    s_wheel_kind  = 1;                        /* 关机时间 */
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
 *   数据 = board_battery_wave() 的真实采样点 (60s 一条, 存储 7 天, **显示窗口最长 24 小时**,
 *          24h=1440 点 > 720 上限 ⇒ 自动抽稀成 2 分钟一点),
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

    /* ---- 历史样本: 1 字节/分钟, 存储保留 7 天 (PSRAM + 按天 NVS), 绘制时按窗口抽稀 ---- */
    static EXT_RAM_BSS_ATTR bat_sample_t smp[BOARD_BATTERY_WAVE_MAX];
    /* 窗口 = 实际已有数据跨度, **最长只显示最近 24 小时** (linit 2026-10-26);
     * 不满 24h 时有几条就撑满整宽 ("实时填满"); 满 24h 后固定滚动看最近一天。 */
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

/* 电源与休眠子菜单 → 选中项
 * ★ 2026-11-22 linit: 移除顶部「硬件关机」开关（改由时钟芯片自动判定）。
 * ★ 2026-11-24 linit「省电模式」: 「壁纸时间」**恢复**为可调项(省电模式只是快速设置,
 *   打开时把它强制写成 1 分; 手动改即退出省电模式); 底部「省电模式」开关 (默认开)。
 * ★ 2026-11-24 linit: 「触摸唤醒」迁到「触摸与反馈」(它是触摸行为, 不是休眠策略)。
 *   索引: 0 壁纸时间 / 1 关机时间 / 2 电池信息 / 3 省电模式。 */
static void on_sleepgrp_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0: open_wp_min_dlg(ctx); break;         /* 壁纸时间 (os_wp/min: 无操作多久 → 进壁纸) */
    case 1: open_shutgrp_min_dlg(ctx); break;    /* 关机时间 (os_wp/wp_shut: 屏保播多久 → 关机) */
    case 2: open_batinfo_dlg(ctx); break;        /* 电池信息 (电压波形图) */
    case 3: {  /* 省电模式 (默认开; 分类最底部 —— 开关类一律放末尾) */
        bool on = !wp_eco_enabled();
        /* 打开省电模式 = 一键快速设置: 内部会把壁纸时间强制写成 1 分、关机时间 3 分
         * 并 reload 缓存, 同时启用"闲置关无线电"。两项设置仍然显示在上一级列表里,
         * 用户看到的就是当前实际生效值(不会出现假设置)。 */
        wp_eco_set_enabled(on);
        os_dialog_pop(ctx);
        open_sleepgrp_dlg(ctx, 3);
        os_dialog_toast(ctx, on ?
            "\xe7\x9c\x81\xe7\x94\xb5\xe6\xa8\xa1\xe5\xbc\x8f\xe5\xb7\xb2\xe5\xbc\x80" :   /* 省电模式已开 */
            "\xe7\x9c\x81\xe7\x94\xb5\xe6\xa8\xa1\xe5\xbc\x8f\xe5\xb7\xb2\xe5\x85\xb3");  /* 省电模式已关 */
        break;
    }
    default: break;
    }
}
static void open_sleepgrp_dlg(ui_ctx_t *ctx, int sel) {
    char items[4][32];
    const char *ptrs[4];
    /* ★ 2026-11-22 linit: 「硬件关机」开关**已移除** —— 有没有 LTC2954 由"是否识别到
     *   时钟芯片(0x68)"自动决定，不再暴露给用户手动切换（见 ltc2954.c 的判据）。
     * ★ 2026-11-24 linit: 「壁纸时间」恢复显示 —— 省电模式只是"一键快速设置",
     *   不能把手动设置项藏起来。两项都直接显示当前**生效**值(省电模式打开时会被写成 1分/3分),
     *   用户一眼能看出现在到底按多少跑, 点进去即可手动改, 改完就按手动值跑。 */
    char span[12];
    int wm = wp_min_get();
    if (wm <= 0) {
        snprintf(items[0], sizeof(items[0]), "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\x3a\xe4\xb8\x8d\xe8\xbf\x9b\xe5\x85\xa5");  /* 壁纸时间:不进入 */
    } else {
        fmt_span(wm, span, sizeof(span));
        snprintf(items[0], sizeof(items[0]), "\xe5\xa3\x81\xe7\xba\xb8\xe6\x97\xb6\xe9\x97\xb4\x3a%s", span);   /* 壁纸时间:N分/N小时 */
    }
    int sm = shutgrp_min_get();
    if (sm <= 0) {
        snprintf(items[1], sizeof(items[1]), "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4\x3a\xe4\xb8\x8d\xe5\x85\xb3\xe6\x9c\xba");  /* 关机时间:不关机 */
    } else {
        fmt_span(sm, span, sizeof(span));
        snprintf(items[1], sizeof(items[1]), "\xe5\x85\xb3\xe6\x9c\xba\xe6\x97\xb6\xe9\x97\xb4\x3a%s", span);   /* 关机时间:N分/N小时 */
    }
    snprintf(items[2], sizeof(items[2]), "\xe7\x94\xb5\xe6\xb1\xa0\xe4\xbf\xa1\xe6\x81\xaf");  /* 电池信息 */
    snprintf(items[3], sizeof(items[3]), "\xe7\x9c\x81\xe7\x94\xb5\xe6\xa8\xa1\xe5\xbc\x8f:%s",  /* 省电模式:开/关 */
             wp_eco_enabled() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3];
    sel = (sel < 0) ? 0 : (sel > 3 ? 3 : sel);
    os_dialog_list(ctx, "\xe7\x94\xb5\xe6\xba\x90\xe4\xb8\x8e\xe4\xbc\x91\xe7\x9c\xa0", ptrs, 4, sel, on_sleepgrp_select, NULL);
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
        /* ★ 2026-11-22: 这里原来还有一行 settings_save_u8("snd_on", ...) —— 已删。
         *   静音持久化归 audio_player_set_muted() 自己负责(唯一真相源),
         *   页面再存一份会和 Lua app.mute / 快捷栏改的值互相覆盖。 */
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
    } else {
        return;   /* 返回/其它: 不刷新 */
    }
    /* 原地替换刷新 (先弹当前层再压回, 层级不加深; 返回直接回设置主菜单).
     * V1.3.x: 高亮刚操作的行 (sel=result), 触摸/按键选择对应选项即时变黑反馈 */
    os_dialog_pop(ctx);
    open_snd_dlg(ctx, result);
}
static void open_snd_dlg(ui_ctx_t *ctx, int sel) {
    char items[4][30];
    const char *ptrs[4];
    snprintf(items[0], sizeof(items[0]), "\xe5\xa3\xb0\xe9\x9f\xb3:%s", audio_player_is_muted() ? "\xe5\x85\xb3" : "\xe5\xbc\x80");  /* 声音:关/开 */
    snprintf(items[1], sizeof(items[1]), "\xe9\x9f\xb3\xe9\x87\x8f:%d", /* 音量:N */
             audio_player_vol_to_step(audio_player_get_volume()));
    snprintf(items[2], sizeof(items[2]), "UI\xe9\x9c\x87\xe5\x8a\xa8:%s", s_vib_ui ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* UI震动:开/关 */
    snprintf(items[3], sizeof(items[3]), "\xe6\xb8\xb8\xe6\x88\x8f\xe9\x9c\x87\xe5\x8a\xa8:%s", s_vib_game ? "\xe5\xbc\x80" : "\xe5\x85\xb3");  /* 游戏震动:开/关 */
    ptrs[0] = items[0]; ptrs[1] = items[1]; ptrs[2] = items[2]; ptrs[3] = items[3];
    sel = (sel < 0) ? 0 : (sel > 3 ? 3 : sel);
    os_dialog_list(ctx, "", ptrs, 4, sel, on_snd_select, NULL);
}

/* ==================== 显示与声音 (2026-10-29) ====================
 * 合并原「显示与时间」与「声音与反馈」：
 *   前 3 项 = 目标结构要求的 黑暗模式 / 声音开关 / 音量;
 *   后 2 项 = 原有的日期更改 / 时间调整（按"暂不删除"要求保留, 只调顺序）。
 * 深色模式仍是三态 (关/暗黑UI/全部), 复用 s_dark + open_dark_dlg, 未改底层。 */
static void open_av_dlg(ui_ctx_t *ctx, int sel);
static void open_touchfb_dlg(ui_ctx_t *ctx, int sel);

/* ===== 「字体修改」二级入口已删除 (2026-11-24 linit) =====
 * 原二级弹窗有两项: ①字体: 宋体 ⇄ 黑体   ②字号: 16px ⇄ 24px。
 * 按要求:
 *   · 宋/黑体切换项整个删掉。而且后续查清: 24px 那套原生点阵(HZK24S)本来就是
 *     **宋体**, 固件里从来没有过黑体字库 —— 那个选项的命名一直是错的;
 *   · 字号保留, 不再套二级弹窗, 直接放在「显示与声音」主菜单点一下切换;
 *     标签直接显示档位:「字体:大」=24px /「字体:小」=16px。两档各自命中自己的原生宋体 (16px=文泉驿点阵宋16, 24px=HZK24S),
 *     全程不缩放 (见 glyph.c 的选源逻辑)。
 * 因此 on_font_select() / open_font_dlg() 两个函数已一并删除。 */

static void on_av_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case 0: {                                    /* 声音开关 */
        bool m = !audio_player_is_muted();
        audio_player_set_muted(m);
        /* ★ 2026-11-22: 持久化已由 audio_player 自己负责, 此处不再重复写 snd_on */
        break;
    }
    case 1:                                      /* 字体:大小 —— 点一下 16px ⇄ 24px */
        settings_font_size_set(font_size_get() == 0 ? 4 : 0);
        break;
    case 2: open_dark_dlg(ctx); return;          /* 黑暗模式: 压三级弹窗, 本层保持 */
    case 3: case 4: {                            /* 调整日期 / 修改时间 (原样逻辑) */
        if (!s_edit_valid) {
            time_t t = time(NULL); struct tm *ct = localtime(&t);
            if (ct) s_edit_tm = *ct;
            s_edit_valid = true;
        }
        s_time_mode  = (result == 3) ? 0 : 1;
        s_time_field = tm_first();
        open_time_wheel_dlg(ctx);
        return;
    }
    default: return;
    }
    os_dialog_pop(ctx);
    open_av_dlg(ctx, result);
}

static void open_av_dlg(ui_ctx_t *ctx, int sel) {
    char items[5][40];
    const char *ptrs[5];
    /* ★ 2026-11-24 linit: ①「字体修改」二级入口删除, 字号直接进主菜单;
     *   ②宋体/黑体切换项删除; ③顺序 = 声音 → 字体 → 黑暗模式 → 日期 → 时间。
     *   ④标签**直接显示当前档位**:「字体:大」=24px /「字体:小」=16px。
     *     两档各自命中自己的原生宋体 (16px=文泉驿点阵宋16, 24px=HZK24S), 见 glyph.c;
     *     点一下在大小之间切换, 切完整屏立即按新字号重绘。 */
    snprintf(items[0], sizeof(items[0]), "\xe5\xa3\xb0\xe9\x9f\xb3:%s",                            /* 声音:%s */
             audio_player_is_muted() ? "\xe5\x85\xb3" : "\xe5\xbc\x80");                           /* 关 / 开 */
    /* 字体:大/小 —— font_size_get() 0=16px, 4=24px (与 settings_font_size 的档位表一致) */
    snprintf(items[1], sizeof(items[1]), "\xe5\xad\x97\xe4\xbd\x93:%s",                            /* 字体:%s */
             font_size_get() == 0 ? "\xe5\xb0\x8f" : "\xe5\xa4\xa7");                              /* 小(16px) / 大(24px) */
    snprintf(items[2], sizeof(items[2]), "\xe9\xbb\x91\xe6\x9a\x97\xe6\xa8\xa1\xe5\xbc\x8f");      /* 黑暗模式 */
    snprintf(items[3], sizeof(items[3]), "\xe8\xb0\x83\xe6\x95\xb4\xe6\x97\xa5\xe6\x9c\x9f");      /* 调整日期 */
    snprintf(items[4], sizeof(items[4]), "\xe4\xbf\xae\xe6\x94\xb9\xe6\x97\xb6\xe9\x97\xb4");      /* 修改时间 */
    for (int i = 0; i < 5; i++) ptrs[i] = items[i];
    sel = (sel < 0) ? 0 : (sel > 4 ? 4 : sel);
    os_dialog_list(ctx, "", ptrs, 5, sel, on_av_select, NULL);
}


/* ==================== 触摸与反馈 (2026-10-29) ====================
 * 菜单震动 / 游戏震动：**沿用现有两套独立配置**（s_vib_ui / s_vib_game,
 * vibrator_set_ui_enabled / vibrator_set_game_enabled, NVS 键 vib_ui / vib_game）,
 * 底层实现一行未改, 只是把入口从"声音与反馈"移到本分类。
 * 触摸校正: 2026-10-29 linit 决定不做 —— CST816 电容屏出厂 1:1, 无系统性偏差,
 *   与其加一个没有依据的校正, 不如不做(避免采样误差把坐标搞偏)。 */
static void open_touchfb_dlg(ui_ctx_t *ctx, int sel);

static void on_touchfb_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {
        s_vib_ui = !s_vib_ui;
        vibrator_set_ui_enabled(s_vib_ui);
        settings_save_u8("vib_ui", s_vib_ui ? 1 : 0);
    } else if (result == 1) {
        s_vib_game = !s_vib_game;
        vibrator_set_game_enabled(s_vib_game);
        settings_save_u8("vib_game", s_vib_game ? 1 : 0);
    } else if (result == 2) {
        /* ★ 2026-11-24: 「丰富震动」—— 只改"震得简单还是丰富", 不动上面两个"要不要震"。
         *   持久化与停播都在 haptic_set_rich() 里(单一归属), 关掉会立刻掐掉在播序列。 */
        haptic_set_rich(!haptic_rich_enabled());
    } else if (result == 3) {
        /* 触摸唤醒屏保: 开=触摸长按 1 秒唤醒屏保; 关=只认物理键/手柄。
         * 2026-11-24 从「电源与休眠」迁入 —— 归到"触摸"语义下更好找。 */
        wp_screensaver_set_touch_wake(!wp_screensaver_touch_wake_enabled());
    } else {
        return;
    }
    os_dialog_pop(ctx);
    open_touchfb_dlg(ctx, result);
}

static void open_touchfb_dlg(ui_ctx_t *ctx, int sel) {
    /* ★ 2026-11-24 linit: 「触摸唤醒」从 电源与休眠 移到这里 —— 它本质是**触摸**行为,
     *   不属于休眠策略; 开关类仍放分类末尾。
     *   索引: 0 菜单震动 / 1 游戏震动 / 2 丰富震动 / 3 触摸唤醒。 */
    char items[4][32];
    const char *ptrs[4];
    snprintf(items[0], sizeof(items[0]), "\xe8\x8f\x9c\xe5\x8d\x95\xe9\x9c\x87\xe5\x8a\xa8:%s",
             s_vib_ui ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    snprintf(items[1], sizeof(items[1]), "\xe6\xb8\xb8\xe6\x88\x8f\xe9\x9c\x87\xe5\x8a\xa8:%s",
             s_vib_game ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    snprintf(items[2], sizeof(items[2]), "\xe4\xb8\xb0\xe5\xaf\x8c\xe9\x9c\x87\xe5\x8a\xa8:%s",
             haptic_rich_enabled() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    snprintf(items[3], sizeof(items[3]), "\xe8\xa7\xa6\xe6\x91\xb8\xe5\x94\xa4\xe9\x86\x92:%s",
             wp_screensaver_touch_wake_enabled() ? "\xe5\xbc\x80" : "\xe5\x85\xb3");
    for (int i = 0; i < 4; i++) ptrs[i] = items[i];
    sel = (sel < 0) ? 0 : (sel > 3 ? 3 : sel);
    os_dialog_list(ctx, "", ptrs, 4, sel, on_touchfb_select, NULL);
}

/* ---- 无线连接弹窗: 蓝牙/WiFi 开关 + 连接WiFi (WiFi 开时显示) ---- */
static void open_wifi_dlg(ui_ctx_t *ctx, int sel);
static void open_wifi_scan(ui_ctx_t *ctx);
static void open_info_dlg(ui_ctx_t *ctx);
static void open_hw_selfcheck_dlg(ui_ctx_t *ctx);   /* ★硬件自检 */
static void open_hw_cfg_dlg(ui_ctx_t *ctx);         /* ★硬件配置 */
static int  s_wifi_idx_selfcheck = -1;
/* 乐观 UI (定义在后): 蓝牙/WiFi 开关的"在途"状态 + 后台执行入口 */
static volatile int s_bt_opt;
static volatile int s_wifi_opt;
/* ★ 2026-11-04: 在途操作的起始时刻。后台切换最坏会卡在初始化超时(20s),
 *   期间 s_bt_opt/s_wifi_opt 非 0 会吞掉所有点击 ⇒ 用户看到的就是"点了没反应/无法开关"。
 *   超过 HW_OPT_STALE_MS 视为该次已失效, 允许重新点。 */
static volatile uint32_t s_bt_opt_t0;
static volatile uint32_t s_wifi_opt_t0;
#define HW_OPT_STALE_MS 25000u
static void hw_opt_start(bool bt);

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
/* ★★★ 2026-11-01: 「从确认框直接转入扫描」时**原位改写当前弹窗层**。
 *   原来应用中心的做法是：os_dialog_pop 关确认框（这会连弹窗页一起 pop，露出下层页面）
 *   再 os_dialog_push 扫描弹窗 ⇒ 中间闪一帧底层页面。
 *   用户看到的就是"选 Wi-Fi 的二级弹窗是在主菜单上弹出来的"。
 *   改成复用当前层：确认框直接"变"成扫描弹窗，全程只有一层、不 pop 页面、不闪。
 *   只有主动要求复用的调用方走这条路，其余调用点（设置页/手柄页等）行为完全不变。 */
static bool s_scan_reuse_top = false;

void settings_wifi_connect_open_reuse(ui_ctx_t *ctx);
void settings_wifi_connect_open_reuse(ui_ctx_t *ctx) {
    s_scan_reuse_top = true;
    settings_wifi_connect_open(ctx);
    s_scan_reuse_top = false;
}

void settings_wifi_connect_open(ui_ctx_t *ctx) {
    if (!wifi_is_on()) { os_hw_request(OS_HW_WIFI); s_wifi_conn_we_opened = true; }
    /* ★ 2026-11-04: 计数错位时 os_hw_request 只 ++ 不启动 ⇒ 扫描会跑在"WiFi 没开"上,
     *   表现为扫描列表永远空。这里兜底 ensure 一次 (幂等)。 */
    if (s_wifi_conn_we_opened && !wifi_manager_is_enabled()) wifi_manager_enable();
    open_wifi_scan(ctx);
}
static void open_wifi_netcfg(ui_ctx_t *ctx);   /* 网络配置二级弹窗 (定义在后) */
static void on_wifi_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    /* ★ 2026-10-16: 硬件自检 / 硬件配置 (位置动态, 用记录的下标判断) */
    if (result >= 0 && result == s_wifi_idx_selfcheck) { open_hw_selfcheck_dlg(ctx); return; }
    if (result == 0) {   /* 蓝牙开关 —— ★ 乐观 UI: 先改标签+震动, 后台真开/真关 */
        /* 上一个还在跑 → 忽略连点。但**卡超时必须解锁**: 初始化失败最坏等 20s,
         * 这期间吞掉所有点击 = 用户眼里"怎么点都没反应"。 */
        if (s_bt_opt) {
            if ((uint32_t)(esp_timer_get_time() / 1000) - s_bt_opt_t0 < HW_OPT_STALE_MS) return;
            ESP_LOGW(TAG, "蓝牙切换超时未收尾 ⇒ 强制解锁, 允许重新点");
            s_bt_opt = 0;
        }
        s_bt_opt = bt_is_on() ? 2 : 1;
        s_bt_opt_t0 = (uint32_t)(esp_timer_get_time() / 1000);
        vibrator_click();                           /* 立刻给触觉反馈 */
        os_dialog_pop(ctx);
        open_wifi_dlg(ctx, result);                 /* 立刻重画: 标签变"开···"/"关···" */
        hw_opt_start(true);
    } else if (result == 1) {   /* WiFi 开关 —— 同上 */
        /* 同蓝牙: 卡超时必须解锁, 否则 WiFi 初始化(1~3s)失败时界面会长期点不动 */
        if (s_wifi_opt) {
            if ((uint32_t)(esp_timer_get_time() / 1000) - s_wifi_opt_t0 < HW_OPT_STALE_MS) return;
            ESP_LOGW(TAG, "WiFi 切换超时未收尾 ⇒ 强制解锁, 允许重新点");
            s_wifi_opt = 0;
        }
        s_wifi_opt = wifi_is_on() ? 2 : 1;
        s_wifi_opt_t0 = (uint32_t)(esp_timer_get_time() / 1000);
        vibrator_click();
        os_dialog_pop(ctx);
        open_wifi_dlg(ctx, result);
        hw_opt_start(false);                        /* 意图(WiFi 重启是否自启)在任务里写 NVS */
    } else if (result == 2) {
        /* WiFi 开且已连接 → "网络配置"; 仅开未连 → 直接扫描; WiFi 关 → 系统信息 */
        if (wifi_is_on() && wifi_manager_is_connected()) open_wifi_netcfg(ctx);
        else if (wifi_is_on())                           open_wifi_scan(ctx);
        else                                             open_info_dlg(ctx);
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
/* ★ 2026-11-22 linit: 连接失败后回到键盘时, 在密码框下方显示一行原因/引导
 *   ("密码错误，请重新输入"). 空串 = 正常输入模式, 不占位不画. */
static char  s_wifi_pwd_hint[32];
/* ★ 2026-11-24 键盘精简: 42 键键盘改用共享模块 ui/kbd_lib/kbd_c42 (同一份布局/命中/
 *   标签/字符)。本页只留"弹窗版"的几何预设 + 自己的一份输入状态。
 *   原来这里是一整套自绘实现 (行表 wp_abc/wp_symk + wp_disp + wp_row_cols + 逐键
 *   触摸区间 + 5 段绘制), 与网络检测页那份逐字节重复。 */
static kbd_c42_state_t s_wp_st;    /* 符号层 / 大写 */
static kbd_c42_geo_t   s_wp_geo;   /* 弹窗版几何 (wifi_pwd_open 里铺开) */
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

/* ---- 手柄光标下的 (行,列) → 42 键 id (行表在共享模块里) ----
 * 列数: r0/r1=10, r2=9, r3=10 (Shift + 8字母 + 删除), r4=3 (&123 + 空格 + 回车) */
static int wp_row_cols(int r) {
    return (r == 2) ? 9 : ((r == 4) ? 3 : 10);
}
static int wp_id_of(int r, int c) {
    if (r < 0 || r > 4) return -1;
    if (r < 3) return r * 10 + c;                       /* 0..28 */
    if (r == 3) return (c == 0) ? KBD_C42_SHIFT
                       : (c == 9) ? KBD_C42_DEL
                       : KBD_C42_LETTER0 + (c - 1);
    return (c == 0) ? KBD_C42_SYM : (c == 1) ? KBD_C42_SPACE : KBD_C42_ENTER;
}
static void wifi_pwd_connect(ui_ctx_t *ctx);   /* 定义在下方 (回车键要用) */
/* 敲下某个键 id: 与触摸命中后做的事**完全一致** */
static void wp_apply(ui_ctx_t *ctx, int id) {
    switch (kbd_c42_act(id)) {
    case KBD_C42_ACT_CHAR: {
        char ch = kbd_c42_char(&s_wp_st, id);
        if (ch && s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ch;
        break;
    }
    case KBD_C42_ACT_SHIFT:   /* 符号层里按首键 = 退出符号层 (沿用原行为) */
        if (s_wp_st.sym) s_wp_st.sym = false;
        else             s_wp_st.shift = !s_wp_st.shift;
        break;
    case KBD_C42_ACT_BKSP:
        if (s_wifi_pwd_len > 0) s_wifi_pwd_len--;
        break;
    case KBD_C42_ACT_SYM:
        s_wp_st.sym = !s_wp_st.sym;
        break;
    case KBD_C42_ACT_SPACE:
        if (s_wifi_pwd_len < 63) s_wifi_pwd_pass[s_wifi_pwd_len++] = ' ';
        break;
    case KBD_C42_ACT_ENTER:
        wifi_pwd_connect(ctx);
        break;
    default: break;
    }
    ctx->needs_redraw = true;
}

/* 密码页发起连接后, 后台轮询连接结果并 toast 提示 */
static bool s_wifi_conn_pending = false;
/* ★ 2026-11-22: 上次失败的原因 (判断在丢弃凭据之前做, 否则 status 已被改成"未连接") */
static bool s_wifi_fail_pwd_bad  = false;
static bool s_wifi_fail_notfound = false;
/* ★ 2026-11-01: 记下发起时刻 —— 用来给"连接失败"判定加握手宽限期(见下面 poll 的注释)。 */
static uint32_t s_wifi_conn_t0 = 0;

static void wifi_pwd_connect(ui_ctx_t *ctx) {
    s_wifi_pwd_pass[s_wifi_pwd_len] = '\0';
    s_wifi_pwd_hint[0] = 0;   /* 这次是用户主动发起的连接尝试: 先撤掉上一次的失败提示 */
    /* ★ 2026-11-01 linit: 「输完密码就全程后台, 出问题再弹窗」。
     *   原来这里 os_dialog_pop 关键盘后又 os_dialog_toast("正在连接...")，
     *   而 toast 在弹窗栈空时会**自己开一个弹窗页** ⇒ 用户看到一个框挡在那里等。
     *   现在：关掉键盘就彻底回列表，连接在后台跑；成功静默, 失败才弹窗(见 conn_poll)。 */
    os_dialog_pop(ctx);                        /* 关密码键盘(扫描层) → 回列表 */
    if (!os_hw_is_active(OS_HW_WIFI)) os_hw_request(OS_HW_WIFI);   /* 与入口一致, 避免双重引用计数 */
    s_wifi_conn_we_opened = false;   /* 已确认连接意图: 后续取消不再释放(用户要保留 WiFi) */
    if (wifi_manager_connect(s_wifi_pwd_ssid, s_wifi_pwd_pass)) {
        s_wifi_conn_pending = true;
        s_wifi_conn_t0 = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        /* ★ 2026-11-22: 原来这里**什么都不做** —— 用户按回车后键盘关掉、界面毫无反应,
         *   看起来就是"点了没响应"。实际是管理器拒绝发起(同一凭据正在连 / set_config 失败)。
         *   给一句提示, 至少让人知道系统收到了。 */
        os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99"); /* 正在连接，请稍候 */
    }
    ESP_LOGI("APPSTORE", "connect_start ssid=%s dlg_depth=%d", s_wifi_pwd_ssid, os_dialog_depth());
}

/* 每帧: 连接成功→"连接成功"提示; 失败(重试结束)→"连接失败" (设置内连接不提示 IP) */
/* ★ 2026-11-01: 导出给应用中心调用。
 *   原因: 连接结果轮询原来只挂在**设置页的 tick** 上 —— 从应用中心进 Wi-Fi 流程时
 *   设置页根本没被 push, 所以 `got_ip / connect_failed` 永远没人处理,
 *   `s_wifi_conn_pending` 一直挂着; 等用户日后进设置页时才突然弹一个过期的结果提示。
 *   连接结果是**全局事件**, 不该绑在某一页的 tick 上。 */
void settings_wifi_conn_poll(ui_ctx_t *ctx);
void settings_wifi_conn_poll(ui_ctx_t *ctx) {
    if (!s_wifi_conn_pending) return;
    if (wifi_manager_is_connected()) {
        s_wifi_conn_pending = false;
        { char ipb[24] = {0}; wifi_manager_get_ip(ipb, sizeof(ipb));
          ESP_LOGI("APPSTORE", "got_ip ip=%s （静默成功, 不弹窗）", ipb); }
        /* ★ 成功不弹任何东西 —— 用户要"全程后台"。列表会自动刷新, 那就是最好的反馈。 */
        return;
    }

    /* ★★★ 2026-11-01 修「一发起就报连接失败, 几秒后后台又连上了」★★★
     *
     *   根因是**竞态**，不是真的失败：
     *   wifi_manager_connect() 内部先 esp_wifi_disconnect()(换目标时)，这个断开是**异步**的，
     *   WIFI_EVENT_STA_DISCONNECTED 会在事件任务里把 s_connecting 清成 false
     *   （见 wifi_manager.c 的 DISCONNECTED 分支）。于是：
     *     主线程 s_connecting = true → 事件任务清成 false → 驱动其实正在握手
     *   ⇒ 下一帧这里看到"既没连上、也不在连接中" → 立刻误判失败、弹"连接失败"；
     *     而 2~3 秒后 IP_EVENT_STA_GOT_IP 到了，网络其实是通的。
     *
     *   修法：给"失败"判定加一个**握手宽限期**。前 4 秒无论 s_connecting 是什么都不判失败；
     *   4 秒后若仍在重试则继续等，最多等到 15 秒（真·连不上才报）。 */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t dt  = now - s_wifi_conn_t0;
    if (dt < 4000u) return;                                     /* 握手窗口内: 不判失败 */
    if (wifi_manager_is_connecting()) {
        if (dt < 15000u) return;                                /* 还在自愈重试: 继续等 */
    } else if (dt < 6000u) {
        /* 4~6 秒"不在连接中"再复核 2 秒 —— 断开事件把 s_connecting 清掉、
         * 但驱动其实还在握手的窗口通常就在这几秒内。 */
        return;
    }
    s_wifi_conn_pending = false;
    ESP_LOGW("APPSTORE", "connect_failed (dt=%ums connecting=%d)", (unsigned)dt,
             (int)wifi_manager_is_connecting());
    /* ★★★ 2026-11-22 linit「密码输错不要保存 + 失败直接重输」★★★
     *   ① 先把失败原因记下来 (丢弃凭据会把 status 改成"未连接", 之后就看不出是密码错);
     *   ② 丢弃这份没连上的凭据 (只清内存, NVS 里上次能用的那份保留);
     *   ③ 再弹失败提示, 用户一点"确定"就**原位变回密码键盘** (密码保留, 方便改错的那一位),
     *      并在密码框下面写一行"密码错误，请重新输入" —— 不用退回列表重新找网络。 */
    { const char *st = wifi_manager_get_status();
      s_wifi_fail_pwd_bad  = (st && (strstr(st, "密码") || strstr(st, "PASSWORD")));
      s_wifi_fail_notfound = (st && strstr(st, "没找到")); }
    wifi_manager_discard_pending();
    /* ★ 只有失败才打断用户, 并且用**确认框**(需要用户确认), 不是一闪而过的 toast。 */
    { extern void conn_fail_notify(ui_ctx_t *);
      conn_fail_notify(ctx); }
}

/* 连接失败提示: 单独抽出来, 避免在 poll 里直接弹窗造成重入。
 * 内容按实际原因给一句能看懂的(密码错/找不到网络/超时)。
 * ★ 2026-11-22 linit: 提示框的"确定"直接接回密码键盘 (原位变形, 见 wifi_pwd_retry_open)。 */
static void wifi_pwd_retry_open(ui_ctx_t *ctx);   /* 定义在密码键盘之后 */
static void wifi_fail_retry_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)result; (void)ud;
    wifi_pwd_retry_open(ctx);
}
void conn_fail_notify(ui_ctx_t *ctx);
void conn_fail_notify(ui_ctx_t *ctx) {
    char msg[96];
    /* ★ 2026-11-22: 用 poll 里提前记下的原因 —— 凭据已被丢弃, 现在的 status 是"未连接" */
    if (s_wifi_fail_pwd_bad) {
        snprintf(msg, sizeof(msg), "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a\xe5\xaf\x86\xe7\xa0\x81\xe5\x8f\xaf\xe8\x83\xbd\xe4\xb8\x8d\xe5\xaf\xb9"); /* 连接失败：密码可能不对 */
    } else if (s_wifi_fail_notfound) {
        snprintf(msg, sizeof(msg), "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a\xe6\xb2\xa1\xe6\x89\xbe\xe5\x88\xb0\xe8\xaf\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 连接失败：没找到该网络 */
    } else {
        snprintf(msg, sizeof(msg), "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a\xe8\xb6\x85\xe6\x97\xb6\xe6\x88\x96\xe4\xbf\xa1\xe5\x8f\xb7\xe5\xa4\xaa\xe5\xbc\xb1"); /* 连接失败：超时或信号太弱 */
    }
    os_dialog_confirm_ex(ctx, msg, 0, -1, wifi_fail_retry_cb, NULL);
}

static bool wifi_pwd_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);

static bool wifi_pwd_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    s_wp_gp = false;                    /* 一触摸就把手柄光标收掉 (不让触屏用户看到黑块) */
    if (s_wifi_pwd_len < 0) s_wifi_pwd_len = 0;
    if (s_wifi_pwd_len > 63) s_wifi_pwd_len = 63;
    int id = kbd_c42_hit(&s_wp_geo, x, y);   /* 键盘外的点不动作, 但仍然吞掉 */
    if (id >= 0) wp_apply(ctx, id);
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
        if (s_wp_gp) wp_apply(ctx, wp_id_of(s_wp_sel_r, s_wp_sel_c));   /* 光标可见 → 敲当前键 */
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
    /* ★ 2026-11-22: 失败后重输模式的提示行 (在密码框与键盘之间的空白区) */
    if (s_wifi_pwd_hint[0])
        draw_text_centered(lcd, dy + 70, s_wifi_pwd_hint, false);
    /* 数字行上方横杠: 画在键盘顶上方 (密码不再碰线) */
    draw_hline(lcd, dx + 4, dx + WPW_W - 4, WKB_TOP - 2, ST7305_COLOR_BLACK);

    /* 当前触点 (按住反黑): 用无副作用 input_touch_now (不受 main.c 已消费
     * input_get_touch_pos 的锁存竞态影响, 保证按下去即变黑、松手即恢复) */
    int tv = 0, tx = -1, ty = -1;
    if (input_touch_now(&tx, &ty)) tv = 1;

    /* 键盘: 共享 42 键模块一次性画完 (布局/标签/分割线都在 kbd_c42).
     * 按下高亮: 手柄光标优先, 其次实时触点 (都用同一个 hit, 保证"点哪黑哪")。 */
    int press = s_wp_gp ? wp_id_of(s_wp_sel_r, s_wp_sel_c) : -1;
    if (tv) { int tid = kbd_c42_hit(&s_wp_geo, tx, ty); if (tid >= 0) press = tid; }
    kbd_c42_draw(lcd, &s_wp_geo, &s_wp_st, press);

    /* 键盘贴弹窗底部 (无底返回行) */
    ctx->needs_redraw = true;
    return true;
}

/* 打开/回到密码键盘. keep_text=true: 保留已输入的密码 (失败重输时用,
 * 用户只需改错的那几个字符, 不用从头敲). */
static void wifi_pwd_open(ui_ctx_t *ctx, const char *ssid, bool keep_text) {
    if (ssid) snprintf(s_wifi_pwd_ssid, sizeof(s_wifi_pwd_ssid), "%s", ssid);
    else s_wifi_pwd_ssid[0] = '\0';
    if (!keep_text) { s_wifi_pwd_len = 0; s_wifi_pwd_pass[0] = '\0'; }
    if (s_wifi_pwd_len < 0) s_wifi_pwd_len = 0;
    if (s_wifi_pwd_len > 63) { s_wifi_pwd_len = 63; s_wifi_pwd_pass[63] = '\0'; }
    s_wp_st.sym = false; s_wp_st.shift = false;
    kbd_c42_geo_popup(&s_wp_geo, WKB_LX, WKB_TOP);      /* 弹窗版几何, 与本页框内位置对齐 */
    s_wp_gp = false; s_wp_sel_r = 4; s_wp_sel_c = 2;   /* 光标默认停"回车" */

    /* 原位改写当前弹窗 (扫描列表 / 失败提示 → 输密码键盘) */
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (!d) {
        /* ★ 2026-11-22: 键盘上一步已经被 pop 掉时 (输完密码就关键盘去后台连),
         *   栈里没东西可改写 ⇒ 直接新开一层, 否则失败后没办法回到输入界面。 */
        os_dlg_stack_t nd = {0};
        nd.no_footer = true;
        nd.on_render = wifi_pwd_render;
        nd.on_touch  = wifi_pwd_touch;
        nd.on_key    = wifi_pwd_key;
        os_dialog_push(ctx, &nd);
        os_dialog_mark_handled(ctx);
        ctx->needs_redraw = true;
        return;
    }
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

static void open_wifi_pwd_dlg(ui_ctx_t *ctx, const char *ssid) {
    s_wifi_pwd_hint[0] = '\0';           /* 正常输入模式: 不带失败提示 */
    wifi_pwd_open(ctx, ssid, false);
}

/* ★ 2026-11-22 linit「连接失败直接重新输入」: 失败提示框点"确定"后走这里 ——
 *   同一个弹窗就地变回键盘, 密码保留, 密码框下方写一行原因。
 *   没有目标 SSID (不是输密码起来的连接) 就不强行弹键盘。 */
static void wifi_pwd_retry_open(ui_ctx_t *ctx) {
    if (!s_wifi_pwd_ssid[0]) return;
    if (s_wifi_fail_pwd_bad)
        snprintf(s_wifi_pwd_hint, sizeof(s_wifi_pwd_hint), "\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x8c\xe8\xaf\xb7\xe9\x87\x8d\xe6\x96\xb0\xe8\xbe\x93\xe5\x85\xa5"); /* 密码错误，请重新输入 */
    else
        snprintf(s_wifi_pwd_hint, sizeof(s_wifi_pwd_hint), "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe9\x87\x8d\xe6\x96\xb0\xe8\xbe\x93\xe5\x85\xa5"); /* 连接失败，请重新输入 */
    wifi_pwd_open(ctx, s_wifi_pwd_ssid, true);
}

/* ---- WiFi 扫描列表弹窗 (3.3: 同一列表模板扫描/选择, 选后再输密码) ---- */
/* ★★★ 2026-10-27 修两个真问题 (linit 报: "扫描特别慢 / 看不见网络名称 / 画面刷新很慢"):
 *   ① 填充判据原来是 `cnt != d->count`, 而 d->count 初值就是 1 (占位行 "正在扫描 WiFi...")
 *      ⇒ **只扫到 1 个网络时 cnt==d->count, 条件不成立, 列表永远不填充**, 界面一直停在
 *      "正在扫描 WiFi...", 一个名字都看不到。改成显式哨兵 s_scan_ui_cnt(-1=未填充)。
 *   ② wifi_manager_scan_start() 原来在 **UI 线程**里同步跑 (要 esp_wifi_start + 断开
 *      + 两次 vTaskDelay + 失败重试 200ms) ⇒ 射频启动那 1~2 秒整屏不刷新, 手感像卡死。
 *      改成: 弹窗立刻出现 → 后台任务开射频+扫描 → poll 只在结果变化时重绘。 */
static volatile int  s_scan_ui_cnt = -1;   /* -1=还没填充过; >=0=已填充的条数 */
static volatile bool s_scan_busy   = false;/* 后台 scan_start 正在跑 */
static volatile bool s_scan_failed = false;/* 本次 scan_start 失败 */
static volatile bool s_scan_cancel = false;/* 弹窗已关, 后台别再留扫描 */
static volatile bool s_scan_retried = false;/* ★ 2026-11-22: 本次弹窗已自动重扫过一次 */

static void wifi_scan_task(void *arg)
{
    (void)arg;
    bool ok = wifi_manager_scan_start();
    if (s_scan_cancel) wifi_manager_scan_stop();   /* 弹窗已关: 别让扫描空转/占驱动内存 */
    s_scan_failed = !ok;
    s_scan_busy   = false;
    vTaskDelete(NULL);
}

static void wifi_scan_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    int cnt = wifi_manager_get_scan_count();
    if (cnt > 24) cnt = 24;                    /* MAX_AP (wifi_manager.h) */
    if (cnt > 0) {
        if (cnt != s_scan_ui_cnt) {            /* 结果有变化才重建列表 */
            d->count = cnt;
            for (int i = 0; i < cnt; i++) {
                char ssid[33];
                if (wifi_manager_get_scan_ssid(i, ssid, sizeof(ssid)) && ssid[0])
                    snprintf(d->items[i], sizeof(d->items[i]), "%.*s", (int)sizeof(d->items[i]) - 1, ssid);
                else
                    snprintf(d->items[i], sizeof(d->items[i]), "??");
            }
            if (d->sel >= d->count) d->sel = d->count - 1;
            s_scan_ui_cnt = cnt;
            ctx->needs_redraw = true;
        }
        return;
    }
    /* cnt == 0: 结果还没到 / 真没有 / 起不来 */
    if (s_scan_ui_cnt < 0 && !s_scan_busy) {
        if (s_scan_failed) {
            snprintf(d->items[0], sizeof(d->items[0]),
                     "\xe6\x89\xab\xe6\x8f\x8f\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe9\x87\x8d\xe8\xaf\x95"); /* 扫描失败，请重试 */
            d->count = 1; d->sel = 0; s_scan_ui_cnt = 0;
            ctx->needs_redraw = true;
        } else if (wifi_manager_is_scan_done()) {
            /* ★★★ 2026-11-22 linit「刚开 WiFi 第一次搜索提示没有网络, 再搜一次才行」★★★
             *   即使已经修掉"连接流程抢空口"(wifi_manager 里扫描前提前置
             *   s_suppress_reconnect), 仍有一种情况会真的返回 0 个 AP:
             *   射频刚起来那几个百毫秒, 主动扫描的 probe 还没换来任何回应。
             *   这时**自动再扫一次**, 不要用"未找到网络"打发用户 ——
             *   只重试一次, 第二次仍为空才认为附近确实没有 AP(不会无限循环)。 */
            if (!s_scan_retried) {
                s_scan_retried = true;
                s_scan_busy = true;
                if (xTaskCreate(wifi_scan_task, "wifi_scan", 5120, NULL, 4, NULL) == pdPASS) {
                    ESP_LOGI("APPSTORE", "首次扫描结果为空 → 自动重扫一次 (不算用户操作)");
                    ctx->needs_redraw = true;
                    return;                 /* 界面继续显示"正在扫描 WiFi..." */
                }
                s_scan_busy = false;
            }
            /* 扫描完成但没搜到任何网络 → 显示"未找到", 不卡在"正在扫描..." */
            snprintf(d->items[0], sizeof(d->items[0]),
                     "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\xe7\xbd\x91\xe7\xbb\x9c"); /* 未找到网络 */
            d->count = 1; d->sel = 0; s_scan_ui_cnt = 0;
            ctx->needs_redraw = true;
        }
    }
}
static void wifi_scan_dlg_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_scan_cancel = true;              /* 告诉后台任务: 弹窗已关, 起完扫描自行收尾 */
    wifi_manager_scan_stop();
    s_scan_ui_cnt = -1;                /* 下次进来重新填充 */
    if (result < 0) {   /* 返回/取消: 若本次仅为扫码而开 WiFi 且尚未连上 → 关掉, 避免射频常开 */
        if (s_wifi_conn_we_opened && !wifi_manager_is_connected()) os_hw_release(OS_HW_WIFI);
        s_wifi_conn_we_opened = false;
        return;
    }
    char ssid[33];
    if (!wifi_manager_get_scan_ssid(result, ssid, sizeof(ssid)) || !ssid[0]) return;
    ESP_LOGI("APPSTORE", "ap_selected idx=%d ssid=%s", result, ssid);

    /* ★ 2026-10-20 linit 定的三条规则 */
    char saved_ssid[33] = {0}, saved_pass[65] = {0};
    wifi_manager_get_saved(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
    bool same = (saved_ssid[0] && strcmp(saved_ssid, ssid) == 0);

    /* ① 点的就是当前已连的那个 ⇒ 只看信息, **不要重连** */
    if (same && wifi_manager_is_connected()) {
        /* ★ 2026-10-30 linit: 不再显示 IP, 只提示"已连接 + SSID"。
         *   用户明确要求"连接成功不用显示 IP"; IP 在"网络检测"页仍可查。 */
        char msg[64];
        snprintf(msg, sizeof(msg), "%.20s  \xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5", ssid);  /* 已连接 */
        os_dialog_toast(ctx, msg);
        return;
    }
    /* ② 有保存密码的同名网络 ⇒ 直接用保存的密码连, 不弹键盘;
     *    连不上再说 (自愈任务会重试, 用户也可以再点一次, 这次会走③输密码) */
    if (same && saved_pass[0]) {
        /* ★ 2026-11-22: 记下目标网络 —— 保存的密码也可能过期/被改, 连不上时
         *   失败提示的"确定"要能直接把这个网络的密码键盘弹出来重输。 */
        snprintf(s_wifi_pwd_ssid, sizeof(s_wifi_pwd_ssid), "%s", ssid);
        s_wifi_pwd_len = 0;
        s_wifi_pwd_pass[0] = '\0';
        /* ★ 2026-10-20: 三种情况给三种提示, 不再"点了没反应" */
        if (!wifi_manager_connect(ssid, saved_pass)) {
            os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99"); /* 正在连接，请稍候 */
            return;
        }
        /* ★ 2026-11-22: 这条路径原来**不进结果轮询** ⇒ 保存的密码过期时连不上也
         *   一声不响, 用户只看到列表里的网一直不出现"已连接"。现在和输密码路径一样
         *   等结果: 成功静默、失败弹提示并让用户重输。 */
        s_wifi_conn_pending = true;
        s_wifi_conn_t0 = (uint32_t)(esp_timer_get_time() / 1000);
        os_dialog_toast(ctx, "\xe7\x94\xa8\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xe5\xaf\x86\xe7\xa0\x81\xe8\xbf\x9e\xe6\x8e\xa5\xe2\x80\xa6"); /* 用已保存密码连接… */
        return;
    }
    /* ③ 其余 (新网络 / 密码变了) → 输密码键盘 */
    ESP_LOGI("APPSTORE", "password_required ssid=%s dlg_depth=%d", ssid, os_dialog_depth());
    open_wifi_pwd_dlg(ctx, ssid);
}
/* ================= 网络配置 (二级) =================
 * 连接记录(可逐个忘记) + 扫描新网络 + 当前 Wi-Fi 信息(IP/网关/DNS/密码明文)。 */
static void info_noop(ui_ctx_t *ctx, int result, void *ud);   /* 只读列表回调 (定义在后) */
static char s_netcfg_ssid[33];
#define WIFI_HIST_MAX_UI 6

static void wifi_info_dlg(ui_ctx_t *ctx)
{
    static char L[10][44];
    const char *ptrs[10];
    int n = 0;
    char ssid[33] = "", pass[65] = "";
    wifi_manager_get_saved(ssid, sizeof(ssid), pass, sizeof(pass));

    char ip[16] = "-", mask[16] = "-", gw[16] = "-", d1[16] = "-", d2[16] = "-";
    uint8_t ch = 0; int rssi = 0; int auth = -1;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif) {
        esp_netif_ip_info_t ii;
        if (esp_netif_get_ip_info(nif, &ii) == ESP_OK) {
            snprintf(ip,   sizeof(ip),   IPSTR, IP2STR(&ii.ip));
            snprintf(mask, sizeof(mask), IPSTR, IP2STR(&ii.netmask));
            snprintf(gw,   sizeof(gw),   IPSTR, IP2STR(&ii.gw));
        }
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN,   &d) == ESP_OK)
            snprintf(d1, sizeof(d1), IPSTR, IP2STR(&d.ip.u_addr.ip4));
        if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_BACKUP, &d) == ESP_OK &&
            d.ip.u_addr.ip4.addr != 0)
            snprintf(d2, sizeof(d2), IPSTR, IP2STR(&d.ip.u_addr.ip4));
    }
    /* ★ 2026-11-22: 统一走 wifi_manager 只读接口(RSSI/加密/信道), 不再直连驱动 */
    int _auth = -1;
    wifi_manager_get_ap_info(&rssi, &_auth, &ch);
    auth = _auth;

    const char *auths = (auth == WIFI_AUTH_OPEN) ? "开放"
                      : (auth == WIFI_AUTH_WEP)  ? "WEP"
                      : (auth >= WIFI_AUTH_WPA3_PSK) ? "WPA3"
                      : (auth >= WIFI_AUTH_WPA2_PSK) ? "WPA2" : "WPA";

    snprintf(L[n], 44, "\xe7\xbd\x91\xe7\xbb\x9c:%s", ssid);              n++;  /* 网络: */
    snprintf(L[n], 44, "\xe5\xaf\x86\xe7\xa0\x81:%s", pass[0] ? pass : "-"); n++; /* 密码: 明文 */
    snprintf(L[n], 44, "IP:%s", ip);                                      n++;
    snprintf(L[n], 44, "\xe6\x8e\xa9\xe7\xa0\x81:%s", mask);              n++;  /* 掩码: */
    snprintf(L[n], 44, "\xe7\xbd\x91\xe5\x85\xb3:%s", gw);                 n++;  /* 网关: */
    snprintf(L[n], 44, "DNS:%s", d1);                                     n++;
    snprintf(L[n], 44, "\xe5\xa4\x87\xe7\x94\xa8" "DNS:%s", d2);   /* 备用 + DNS (断开字符串, 免 \xa8D 被当成三字节转义) */             n++;  /* 备用DNS: */
    snprintf(L[n], 44, "\xe4\xbf\xa1\xe5\x8f\xb7:%d dBm", rssi);           n++;  /* 信号: */
    snprintf(L[n], 44, "\xe5\x8a\xa0\xe5\xaf\x86:%s  \xe4\xbf\xa1\xe9\x81\x93:%d", auths, ch); n++; /* 加密/信道 */
    for (int i = 0; i < n; i++) ptrs[i] = L[i];
    os_dialog_list(ctx, "", ptrs, n, 0, info_noop, NULL);
}

static void wifi_forget_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result == 0) wifi_manager_forget(s_netcfg_ssid);
    os_dialog_pop(ctx);
    open_wifi_netcfg(ctx);          /* 重开刷新记录列表 */
}

/* 三级: 连接记录 (已连过的 Wi-Fi, 点一条 → 忘记?) */
static void wifi_hist_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result < 0) return;
    if (wifi_manager_history_get(result, s_netcfg_ssid, sizeof(s_netcfg_ssid), NULL, 0))
        os_dialog_confirm_ex(ctx, "\xe5\xbf\x98\xe8\xae\xb0\xe8\xbf\x99\xe4\xb8\xaa\xe7\xbd\x91\xe7\xbb\x9c?",
                             0, -1, wifi_forget_cb, NULL);   /* 忘记这个网络? */
}

static void open_wifi_hist(ui_ctx_t *ctx)
{
    static char items[WIFI_HIST_MAX_UI][40];
    const char *ptrs[WIFI_HIST_MAX_UI];
    int n = 0, cnt = wifi_manager_history_count();
    for (int i = 0; i < cnt && n < WIFI_HIST_MAX_UI; i++) {
        char ssid[33];
        if (wifi_manager_history_get(i, ssid, sizeof(ssid), NULL, 0))
            snprintf(items[n], sizeof(items[n]), "%.*s", (int)sizeof(items[0]) - 1, ssid);
        else
            snprintf(items[n], sizeof(items[n]), "-");
        ptrs[n] = items[n]; n++;
    }
    if (n == 0) {                                     /* 一条都没有 */
        snprintf(items[0], sizeof(items[0]), "\xe6\x9a\x82\xe6\x97\xa0\xe8\xae\xb0\xe5\xbd\x95");  /* 暂无记录 */
        ptrs[0] = items[0]; n = 1;
    }
    os_dialog_list(ctx, "", ptrs, n, 0, wifi_hist_cb, NULL);
}

/* 二级: 三个固定入口 */
static void wifi_netcfg_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result == 0)      open_wifi_hist(ctx);                                   /* 连接记录 */
    else if (result == 1) wifi_info_dlg(ctx);                                    /* 当前Wi-Fi信息 */
    else if (result == 2) { os_dialog_pop(ctx); open_wifi_scan(ctx); }           /* 扫描新网络 */
}

static void open_wifi_netcfg(ui_ctx_t *ctx)
{
    static char items[3][40];
    const char *ptrs[3];
    int n = 0;
    snprintf(items[n], sizeof(items[n]), "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xae\xb0\xe5\xbd\x95"); n++;              /* 连接记录 */
    snprintf(items[n], sizeof(items[n]), "\xe5\xbd\x93\xe5\x89\x8dWi-Fi\xe4\xbf\xa1\xe6\x81\xaf"); n++;         /* 当前Wi-Fi信息 */
    snprintf(items[n], sizeof(items[n]), "\xe6\x89\xab\xe6\x8f\x8f\xe6\x96\xb0\xe7\xbd\x91\xe7\xbb\x9c"); n++;  /* 扫描新网络 */
    for (int i = 0; i < n; i++) ptrs[i] = items[i];
    os_dialog_list(ctx, "", ptrs, n, 0, wifi_netcfg_cb, NULL);
}

static void open_wifi_scan(ui_ctx_t *ctx) {
    /* ★★★ 2026-10-27: 先把弹窗顶上去, 再后台开射频+扫描 —— 射频启动 1~2 秒不再冻住 UI。
     *   原来这里同步调 wifi_manager_scan_start(), 它会 esp_wifi_start() + 断开 + 两次
     *   vTaskDelay, 全程占着 UI 线程 ⇒ "扫描特别慢 / 画面刷新很慢" 的主因。 */
    s_scan_ui_cnt = -1;
    s_scan_failed = false;
    s_scan_cancel = false;
    s_scan_retried = false;   /* ★ 2026-11-22: 每次新开扫描弹窗都重新给一次自动重扫机会 */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "%s", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f WiFi..."); /* 正在扫描 WiFi... */
    dlg.count = 1;
    dlg.sel = 0;
    dlg.on_poll = wifi_scan_dlg_poll;
    dlg.cb = wifi_scan_dlg_cb;
    {
        /* 需要复用时：把当前弹窗(确认框)原位改写成扫描列表，不 pop 页面、不闪 */
        os_dlg_stack_t *cur = (s_scan_reuse_top && os_dialog_depth() > 0)
                              ? os_dialog_top_mut(ctx) : NULL;
        if (cur) {
            uint32_t keep = cur->open_ms;    /* 保留打开时刻, 不触发超时 */
            *cur = dlg;
            cur->open_ms = keep;
            os_dialog_mark_handled(ctx);     /* 回调返回后别把这一层自动关掉 */
            ESP_LOGI("APPSTORE", "scan_ui: 原位改写当前弹窗(不 pop 页面, 无闪烁)");
        } else {
            os_dialog_push(ctx, &dlg);
        }
    }
    ESP_LOGI("APPSTORE", "scan_start busy=%d wifi_en=%d conn=%d dlg_depth=%d",
             (int)s_scan_busy, (int)wifi_manager_is_enabled(),
             (int)wifi_manager_is_connected(), os_dialog_depth());
    if (!s_scan_busy) {
        s_scan_busy = true;
        if (xTaskCreate(wifi_scan_task, "wifi_scan", 5120, NULL, 4, NULL) != pdPASS) {
            s_scan_busy = false;
            s_scan_failed = true;
        }
    }
    ESP_LOGI(TAG, "WiFi 扫描开始(后台)");
}

/* ================= 乐观 UI: 蓝牙/WiFi 开关 (2026-10-25 linit) =================
 * 问题: 点"蓝牙/ WiFi 开关"要等 bt_manager_enable()/esp_wifi_start() 真正跑完
 *       (蓝牙 300ms~1s, WiFi 1~3s) 才重画列表 —— 这段时间界面是**死的**, 按键无反应。
 * 做法: 点击瞬间先改状态标签("开···"/"关···")+ 震动反馈 + 立即重画, 真正启用丢到
 *       后台任务里跑; 跑完由 wifi_dlg_poll 把标签刷新成最终值。
 *       ⇒ 手感上"点了就生效", 实际耗时被藏到后面。 */
/* s_bt_opt / s_wifi_opt 已在文件上方前置声明 (0=空闲 1=正在开 2=正在关) */

static void hw_opt_task(void *arg)
{
    bool bt = (bool)(intptr_t)arg;
    if (bt) {
        int want = s_bt_opt;
        if (want == 1) {
            os_hw_request(OS_HW_BT);
            /* ★ 2026-11-04: 引用计数可能被历史路径漏 release 抬到 >0, 那时 os_hw_request
             *   只会 ++ 而**不启动底层** ⇒ 点了"开"什么都不发生(实测"经常无法开关")。
             *   这里再显式 ensure 一次: bt_manager_enable() 幂等, 已就绪时毫秒返回。 */
            if (!bt_manager_is_ready()) bt_manager_enable();
            /* ★ 2026-11-24: 不再写「用户主动关」标记 —— 该标记现在只表示
             *   **手柄页的「开机自动回连」开关**, 由用户在那里显式切换;
             *   设置页开/关蓝牙只作用于当前射频, 不应替用户改开机偏好。 */
        } else if (want == 2) {
            os_hw_reset(OS_HW_BT);   /* 先抹平错位计数, 否则下一次"开"仍然起不来 */
            /* ★ 2026-11-24 linit「关蓝牙要真还内存」: 原来调 bt_manager_disable()
             *   = bt_shutdown(false) = **只关射频, 栈结构全留着** (controller/Bluedroid/
             *   esp_hidh 约 71KB 内部 RAM 一直占着), 用户观察到的就是"关了蓝牙内存没恢复"。
             *   设置页手动关 = 最后一个使用者退出, 走 disable_release 深拆栈才对
             *   (与 os_hw 引用计数归零时的行为一致)。代价: 下次开要 1~2s 完整初始化,
             *   但这本来就在 hw_opt_task 后台跑, 界面显示"开···", 不卡主循环。 */
            bt_manager_disable_release();
            /* ★ 2026-11-24: 同上 —— 关蓝牙不等关开机回连。用户要"关了蓝牙也照常
             *   开机试回连" (linit 2026-11-24 明确要求), 所以这里也不写标记。 */
        }
        s_bt_opt = 0;
    } else {
        int want = s_wifi_opt;
        if (want == 1) {
            os_hw_request(OS_HW_WIFI);
            wifi_manager_set_want_on(true);
            /* ★ 2026-11-04: 同蓝牙 —— 引用计数若被历史路径抬到 >0, os_hw_request 只会 ++
             *   而不启动底层, 表现就是"点了开但 WiFi 一直起不来"。这里显式 ensure 一次
             *   (wifi_manager_enable 幂等, 已开时立即返回)。 */
            if (!wifi_manager_is_enabled() && !wifi_manager_enable())
                ESP_LOGW("SETTINGS", "WiFi 启动失败");
        } else if (want == 2) {
            os_hw_reset(OS_HW_WIFI);   /* 先抹平错位计数, 否则下一次"开"仍然起不来 */
            wifi_manager_disable();
            wifi_manager_set_want_on(false);
        }
        s_wifi_opt = 0;
    }
    ESP_LOGI("SETTINGS", "%s 后台切换完成 (bt_is_on=%d wifi_is_on=%d)",
             bt ? "蓝牙" : "WiFi", (int)bt_is_on(), (int)wifi_is_on());
    vTaskDelete(NULL);
}

static void hw_opt_start(bool bt)
{
    if (xTaskCreate(hw_opt_task, "hw_opt", 4096, (void *)(intptr_t)bt, 3, NULL) != pdPASS) {
        /* 任务建不出来 (内存极端紧张): 退回同步执行, 功能正确但不乐观 */
        if (bt) { int w = s_bt_opt; s_bt_opt = 0;
                  if (w == 1) { os_hw_request(OS_HW_BT);
                                if (!bt_manager_is_ready()) bt_manager_enable(); }
                  else if (w == 2) { os_hw_reset(OS_HW_BT); bt_manager_disable_release(); } }
        else    { int w = s_wifi_opt; s_wifi_opt = 0;
                  if (w == 1) { os_hw_request(OS_HW_WIFI); wifi_manager_set_want_on(true);
                                if (!wifi_manager_is_enabled()) wifi_manager_enable(); }
                  else if (w == 2) { os_hw_reset(OS_HW_WIFI); wifi_manager_disable();
                                     wifi_manager_set_want_on(false); } }
    }
}

/* 状态文字: 有在途操作时显示"开···/关···", 否则显示真实状态 */
/* ★ 2026-10-27 linit: 状态文字**只保留单个"开/关"** —— 原来在途时显示 "开···"/"关···",
 *   字数变多 ⇒ 这一行(居中/右对齐)整体往左挪一下, 观感像"抖了一下"。
 *   现在固定两字宽度, 点开关时只有文字本身变, 位置不动。 */
static const char *bt_state_str(void)
{
    return bt_is_on() ? "开" : "关";
}
static const char *wifi_state_str(void)
{
    return wifi_is_on() ? "开" : "关";
}

/* 按当前(乐观)状态生成菜单项. 返回条数. */
static int wifi_dlg_build(char items[][32], const char *ptrs[], int cap)
{
    int n = 0;
    if (n < cap) { snprintf(items[n], 32, "蓝牙:%s", bt_state_str());   ptrs[n] = items[n]; n++; }
    if (n < cap) { snprintf(items[n], 32, "WiFi:%s", wifi_state_str()); ptrs[n] = items[n]; n++; }
    if (wifi_is_on() && n < cap) {
        snprintf(items[n], 32, "%s",
                 wifi_manager_is_connected() ? "网络配置" : "连接WiFi");
        ptrs[n] = items[n]; n++;
    }
    if (n < cap) { snprintf(items[n], 32, "系统信息"); ptrs[n] = items[n]; n++; }
    s_wifi_idx_selfcheck = n;
    if (n < cap) { snprintf(items[n], 32, "硬件管理"); ptrs[n] = items[n]; n++; }
    /* 2026-11-03: 「性能监视」入口已删除 —— 它现在是独立应用
     * (应用中心 → 应用 → 系统), 不再藏在本页的二级菜单里。 */
    return n;
}

/* 在途操作跑完 / WiFi 连上 ⇒ 就地把标签刷新成最终值 (不重开弹窗, 不打断用户) */
static void wifi_dlg_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)ud;
    static uint32_t last = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - last < 150) return;      /* 150ms 节流, 别每帧重排字符串 */
    last = now;

    char items[8][32];
    const char *ptrs[8];
    int n = wifi_dlg_build(items, ptrs, 8);
    bool changed = (n != d->count);
    for (int i = 0; i < n && !changed; i++)
        if (strcmp(d->items[i], items[i]) != 0) changed = true;
    if (!changed) return;

    for (int i = 0; i < n; i++)
        snprintf(d->items[i], sizeof(d->items[0]), "%.*s",
                 (int)(sizeof(d->items[0]) - 1), items[i]);
    d->count = n;
    if (d->sel >= n) d->sel = n - 1;
    if (d->sel < 0)  d->sel = 0;
    ctx->needs_redraw = true;
}

static void open_wifi_dlg(ui_ctx_t *ctx, int sel) {
    char items[8][32];
    const char *ptrs[8];
    int n = wifi_dlg_build(items, ptrs, 8);
    sel = (sel < 0) ? 0 : (sel >= n ? n - 1 : sel);
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = n;
    for (int i = 0; i < n; i++) snprintf(dlg.items[i], sizeof(dlg.items[0]), "%s", ptrs[i]);
    dlg.sel = sel;
    dlg.cb  = on_wifi_select;
    /* ★ 乐观 UI 的"刷新"半边: 后台开关跑完 / WiFi 连上后自动更新标签 */
    dlg.on_poll = wifi_dlg_poll;
    os_dialog_push(ctx, &dlg);
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
/* ★ 2026-11-20: 新增第 7 项「时钟」(HW_RTC, DS3231MZ+ 0x68)。
 *   名字取两字: 列表行 "时钟:未检测到(关)" = 24 字节, 仍在 items[30] 内不截断
 *   (叫"时间芯片"会到 30 字节被砍掉右括号)。 */
static const char *HWCFG_NAMES[7] = { "触摸", "SD卡", "音频", "电池", "GPS", "震动", "时钟" };

/* 自检页: 板型 + 6 项"检测到/未检测到(开/关)" + 最后一行"重新自检" */
static int hwsc_build(char lines[][28]) {
    int k = 0;
    /* ★ 2026-10-20: 板型挪到弹窗标题, 列表只剩 6 项硬件 + 重新自检 = 7 行。
     *   原来 8 行超出一屏, 要滚一屏才看到 GPS, 而滚动会回弹 ⇒ 点不到 GPS。 */
    for (int i = 0; i < HW_COUNT; i++) {
        /* ★★★ 2026-11-23 linit: GPS 必须分**三态**, 而且三态都要能一眼看懂:
         *   ① 休眠   = 已确认装了 + 当前处于软件待机(46µA) —— 不是"没检测到";
         *   ② 未检测到 = 已确认**没装**(探过一次没有, 之后不再碰) —— 用户可能需要焊;
         *   ③ 待识别 = 新固件第一次开机 / 刚点过"重新识别", 后台正在探。
         *   前两态靠 NVS (gps_probed + det 位) 持久区分, 所以重启后仍然准:
         *   不会出现"明明没装, 每次开机都显示检测了一会儿"。
         *   gps_power_state() 在 park 末尾会按实测还原成 SLEEP, 拆 UART 后照样读得到。 */
        const char *st;
        if ((hw_id_t)i == HW_GPS) {
            if (!hw_cfg_gps_probed()) {
                st = "待识别";
            } else if (!hw_cfg_detected(HW_GPS)) {
                st = "未检测到";
            } else {
                st = (gps_power_state() == GPS_POWER_SLEEP) ? "休眠" : "检测到";
            }
        } else {
            st = hw_cfg_detected((hw_id_t)i) ? "检测到" : "未检测到";
        }
        snprintf(lines[k++], 28, "%s:%s%s", HWCFG_NAMES[i], st,
                 hw_cfg_enabled((hw_id_t)i) ? "(开)" : "(关)");
    }
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
    for (int i = 0; i < n; i++) str_trunc(d->items[i], sizeof(d->items[i]), L[i]);
    d->count = n; ctx->needs_redraw = true;
}
/* ★ 只有 音频/GPS/震动/时钟 可点击开关; 触摸/SD卡/电池 只显示不可点(linit 2026-10-16)
 * ★ 2026-11-20: 时钟(RTC) 可关 —— 关掉就不跑自动校时, 断电后时间归零。 */
static bool hwmg_clickable(int id) {
    return (id == HW_AUDIO || id == HW_GPS || id == HW_VIB || id == HW_RTC);
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
/* ★ 2026-10-20: GPS 是选装件 —— 没识别到也可以点它"重新识别一遍"
 *   (焊上模块后不用刷固件, 在这里点一次就行)。 */
static void hwsc_gps_reprobe_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {
        extern void app_board_gps_probe_async(void);
        hw_cfg_gps_invalidate();          /* 清掉"未识别"的记录 ⇒ 允许重探 */
        app_board_gps_probe_async();      /* 后台探 (~1.5s), 完事列表自己刷新 */
        os_dialog_toast(ctx, "正在识别 GPS…");
    }
    s_hwmg_id = -1;
    os_dialog_pop(ctx);
    open_hw_selfcheck_dlg(ctx);
}
static void hwsc_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    char L[8][28]; int n = hwsc_build(L);
    if (result == n - 1) {                 /* 最后一行 = 重新自检 */
        hw_cfg_rescan();
        os_dialog_pop(ctx);
        open_hw_selfcheck_dlg(ctx);
        /* 重新自检会换引脚表，但外设已按旧表初始化 ⇒ 必须重启才真正生效 */
        os_dialog_toast(ctx, "已重新自检，重启后生效");
        return;
    }
    /* 第 0..HW_COUNT-1 行 = 硬件项 (板型已挪到标题) */
    if (result >= 0 && result < HW_COUNT) {
        int id = result;
        if (!hwmg_clickable(id)) return;                 /* 触摸/SD/电池: 点击无反应 */
        if (!hw_cfg_detected((hw_id_t)id)) {
            /* 未检测到的只有 GPS 允许"重新识别"(选装件, 用户可能刚焊上) */
            if (id == HW_GPS) {
                s_hwmg_id = id;
                os_dialog_confirm_ex(ctx, "重新识别 GPS?", 0, -1, hwsc_gps_reprobe_cb, NULL);
            }
            return;
        }
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
    for (int i = 0; i < n; i++) str_trunc(dlg.items[i], sizeof(dlg.items[i]), L[i]);
    dlg.count = n; dlg.sel = -1; dlg.on_poll = hwsc_poll; dlg.cb = hwsc_select;
    snprintf(dlg.title, sizeof(dlg.title), "%s", hw_cfg_board_name());   /* 板型显示在标题 */
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
        d->sel = (d->sel > 0) ? (d->sel - 1) : 4;   /* ★ 2026-10-17: 首尾循环(原来夹紧在 0) */
        ctx->needs_redraw = true;
        if (0) { }
        return true;
    case OS_ACTION_DOWN:
        d->sel = (d->sel < 4) ? (d->sel + 1) : 0;   /* 5 项 (含喝杯水), 首尾循环 */
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_CONFIRM:
        if (d->sel >= d->count) return false;   /* 底部"返回"行 → 默认处理 (关闭) */
        switch (d->sel) {
        case 0: open_av_dlg(ctx, 0); break;          /* 显示与声音 (黑暗/声音/音量 + 日期时间) */
        case 1: open_touchfb_dlg(ctx, 0); break;     /* 触摸与反馈 (菜单震动/游戏震动/丰富震动/触摸唤醒) */
        case 2: open_sleepgrp_dlg(ctx, 0); break;    /* 电源与休眠 (原休眠与电源, 功能不变) */
        case 3: open_wifi_dlg(ctx, 0); break;        /* 系统与连接 (WiFi/蓝牙/固件信息/存储空间) */
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
    .tick = settings_wifi_conn_poll,
};

void os_page_settings_register(void) {
    os_register(&s_mod_settings);
    os_register_service(&s_svc_wifi_conn);
    input_set_gamemode_cb(on_gamemode_changed);
}
