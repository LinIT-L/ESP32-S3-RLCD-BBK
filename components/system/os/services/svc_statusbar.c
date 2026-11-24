/**
 * svc_statusbar.c — 全局服务: 状态栏.
 *
 * 从 menu_system.c 的 menu_draw_status_bar 迁移. 视觉与旧版完全一致:
 * 布局: 最左 日期"MM-DD 星期X" | 正中间 时间"HH:MM" | 右侧 电池->手柄/蓝牙->喇叭音量->数据传输->WiFi
 *
 * V1.5.x: 反白开关 (s_inv) — 默认普通(黑字白底); 网络探测等暗色科幻页通过
 * os_statusbar_set_invert(true) 仅在本页把状态栏整条反白(白字白图 on 黑底).
 * ★ 2026-11-23: 反白/显隐都改为**按页归属**, 换页自动还原 —— 不再依赖应用在
 *   on_exit 里记得复位 (那条路一旦被跳过, 整条状态栏会一直是反的/一直是隐藏的).
 * 解耦: 本服务不引用 menu/bt_manager/wifi_manager 等实现,
 * 系统状态(电池/音量/蓝牙/WiFi/USB) 通过 os_platform_set() 注入的 getter 读取.
 *
 * ★ 2026-11-23 linit 状态栏统一整改: 本文件是**唯一**绘制实现, 应用只声明要不要
 *   (C: os_statusbar_show / Lua: app.statusbar)。旧实现让每个 Lua 应用自己调
 *   app.statusbar() 画一张快照, 时间/电量永远停在那一刻 —— "录音机状态栏不刷新"的根因。
 */
#include "os.h"
#include "ui_common.h"
#include "status_icons.inc"
#include "statusbar_policy.h"   /* ★ 2026-11-23: 显隐判据(纯逻辑, 主机测试同一份) */
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
    /* 快捷栏(音量/蓝牙/Wi-Fi)复用同一份挂钩 —— 外壳只注入一次, 两处服务共用 */
    os_quickbar_set_platform(ops);
}

/* ============ 状态栏绘制 ============ */

#define SB_STRIP_H 24        /* 状态栏条高度 */
/* 高度只能有**一份**定义: os.h 的 OS_STATUS_H 是给应用用的公开值, 这里必须同值,
 * 否则应用按 24 排版、系统按别的值画 ⇒ 顶部被盖住或留白 (2026-11-23 整改)。 */
_Static_assert(SB_STRIP_H == OS_STATUS_H, "SB_STRIP_H 必须等于 os.h 的 OS_STATUS_H");
static bool s_inv = false;   /* 反白模式 (本页暗色: 白字白图 on 黑底) */

/* "有效页" = 真正决定状态栏长相的那一页。
 * ★ 弹窗页(OS_PAGE_DIALOG)只是临时覆盖层: 暗色页(无线探测)弹出"退出确认"、
 *   录音机弹出确认框时, 状态栏必须**保持底下那页的配色与显隐**, 不能因为
 *   current_page 变成 DIALOG 就突然变回普通配色/突然冒出一条栏。
 *   所以 DIALOG 期间沿用"最近一个非弹窗页"作为归属页。 */
static int s_page_last = -0x7FFF;
static int sb_effective_page(void)
{
    int p = os_current_page();
    if (p == OS_PAGE_DIALOG) return s_page_last;
    s_page_last = p;
    return p;
}

/* ★ 2026-11-23 linit: 反白也按页归属。原来只靠页面自己在 on_exit 还原 ——
 *   一旦某页退出路径被跳过(直接 os_pop / 长按回桌面 / 崩溃重启前的残留),
 *   整条状态栏会一直是反的: 白底页面顶部一条黑, 看着像"状态栏坏了"。
 *   现在记录"哪一页要求反白", 换页自动回到普通配色, 无需应用记得还原。 */
static int  s_inv_page = -0x7FFF;

void os_statusbar_set_invert(bool v)
{
    s_inv = v;
    s_inv_page = sb_effective_page();
}

/* ============ 应用声明 (统一接口, 见 os.h) ============ */

/* 声明黏在"声明时所在的那一页": 换页即失效 ⇒ 应用退出/切页不必再还原一次。
 * 这就是"应用退出后状态栏残留/隐藏不恢复"那类问题的根治点。 */
static int s_req      = OS_SB_AUTO;
static int s_req_page = -0x7FFF;

void os_statusbar_show(bool on)
{
    int page = sb_effective_page();
    int v    = on ? OS_SB_SHOW : OS_SB_HIDE;
    /* 幂等 (判据在 statusbar_policy.h, 主机可测): 同页同值直接返回 —— 应用在
     * render 里每帧调一次也不会多触发一次重绘, 更不会打断墨水屏刷新。 */
    if (!os_sb_req_is_new(s_req_page, s_req, page, on)) return;
    s_req      = v;
    s_req_page = page;
    os_request_redraw();                            /* 声明变了, 下一帧才看得到 */
}

void os_statusbar_auto(void)
{
    int page = sb_effective_page();
    if (s_req == OS_SB_AUTO && s_req_page == page) return;   /* 幂等 */
    s_req      = OS_SB_AUTO;
    s_req_page = page;
    os_request_redraw();
}

int os_statusbar_page_req(void)
{
    return os_sb_req_applies(s_req_page, sb_effective_page()) ? s_req : OS_SB_AUTO;
}

/* os_statusbar_visible() 在 os_core 实现 (读 ctx->statusbar_on, 内核每帧合成)。 */

int os_statusbar_height(void)
{
    return OS_STATUS_H;
}

/* 1bpp 反白直写: 先填该格黑底, 再在 bit 置位处画白 (白图)
 * 注: 目前没有调用者 (电池改手绘后位图路径只走 svc_draw_status_icon),
 *     保留以便后续需要"整块反白位图"时有现成实现; 加 unused 免掉编译告警。 */
__attribute__((unused))
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
/* 缩放绘制 (定义在本函数之后, 这里前置声明) */
static void svc_draw_status_icon(st7305_handle_t *lcd, int x, int y,
                                 int srcW, int srcH, int bytes_per_row,
                                 int tw, int th, const uint8_t *bitmap);

/* 电池图标绘制尺寸: 原图 32x16, 实际画 24x12 (缩到 3/4, linit 2026-10-30) */
#define BAT_DW 24
#define BAT_DH 12

/* ★ 2026-11-21: 充电闪电 (4×6, 画在电池内腔中央)。
 *   充电中不画电量块 —— 一是空间小, 二是"闪电"比几格电量更能一眼看出在充电。 */
static void svc_draw_bolt(st7305_handle_t *lcd, int x, int y, st7305_color_t c) {
    fill_rect(lcd, x + 2, y + 0, x + 3, y + 0, c);
    fill_rect(lcd, x + 1, y + 1, x + 2, y + 1, c);
    fill_rect(lcd, x + 0, y + 2, x + 3, y + 2, c);
    fill_rect(lcd, x + 1, y + 3, x + 2, y + 3, c);
    fill_rect(lcd, x + 0, y + 4, x + 1, y + 5, c);
}

static void svc_draw_battery(st7305_handle_t *lcd, int x, int y, uint8_t percent, uint8_t chg) {
    st7305_color_t c = s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    /* 24x12 手绘: 21x12 电池身 + 右侧 3x6 正极.
     * ⚠ 不能把 32x16 位图直接降采样 —— 原图边框只有 1px, 缩到 3/4 后
     *   采样点跳过边框, 满电档整块变纯黑, 认不出是电池 (实测)。 */
    draw_rect_outline(lcd, x, y, x + 20, y + 11, c);     /* 外壳 x 0..20 */
    fill_rect(lcd, x + 21, y + 3, x + 23, y + 8, c);     /* 正极 x 21..23 */

    if (percent == 255) {                                 /* 未检测到: 中间问号 */
        draw_ascii_sb(lcd, x + 7, y + 1, '?', s_inv);
        return;
    }
    if (chg == 1) {                                       /* 充电中: 闪电 */
        svc_draw_bolt(lcd, x + 8, y + 3, c);
        return;
    }
    if (chg == 2) {                                       /* 已充满: 满格 */
        fill_rect(lcd, x + 2, y + 2, x + 18, y + 9, c);
        return;
    }
    /* ★ 2026-11-22 linit「线性显示一点, 不要一下子掉太多电格」。
     *   原规则是 7 档(每档 12%) 且 percent>=70 一律算满格 ——
     *   结果顶部 30% 完全不动, 一过 70% 突然掉 3px,
     *   这就是"很久不掉, 一掉掉一大截"的真因。
     *   现在: 内腔 17px 按百分比**连续**填充 (1% = 0.17px, 约 6% 才走 1px),
     *        没有档位、没有满格死区, 视觉上是"慢慢爬"。 */
    const int inner = 17;                                 /* 内腔 x 2..18 */
    int w = ((int)percent * inner + 50) / 100;            /* 四舍五入到像素 */
    if (percent > 0 && w < 2) w = 2;                      /* 有电就至少 2px, 不显成空 */
    if (w > inner) w = inner;
    if (w > 0) fill_rect(lcd, x + 2, y + 2, x + 1 + w, y + 9, c);
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
static uint8_t  s_chg         = 0;    /* ★ 2026-11-21: 0=未充 1=充电中 2=已充满 */
static int      s_last_minute = -1;
/* ★ 2026-10-27: 状态栏刷新策略 = "1 分钟一跳 (时间) + 任何状态变化立刻一跳"
 *   之前只盯电池(60s 采样)和分钟, Wi-Fi 由关到开 / 由未连到连上都**不触发重绘**,
 *   图标要等下一次分钟跳变才出现 (最坏晚 60 秒) —— 这就是"网络图标出现得慢"的真因。 */
static uint8_t  s_wifi_state  = 0xFF;   /* bit0=已开 bit1=已连; 0xFF=尚未采样 */
static uint8_t  s_bt_state    = 0xFF;   /* bit0=已开 bit1=已连 */

/* ★ 2026-11-23 linit 状态栏统一整改: 删掉"内容签名不重画"的缓存。
 *   为什么必须删: st7305 是**整帧**送屏(窗口 0..399 × 0..299)，页面任何一处变化都会
 *   把整块 fb 推上屏；而不少应用 (离线维基/录音机的部分界面) 会 app.clear() 整屏或
 *   从 y=0 重绘。若状态栏因"签名没变"跳过落笔，这一帧推上去的就是**没有状态栏**的画面
 *   —— 表现为"状态栏时有时无/不刷新"。正确做法只有一条: 只要本帧显示状态栏,
 *   就必须在 fb 里把它画好; 帧内像素完全没变时驱动层(整帧指纹)自己会跳过送屏,
 *   不会因为多画这一条而多闪一次。 */

/* V1.2 性能: time()/localtime() 结果缓存 (最多每秒刷新一次).
 * 原先 svc_tick 每帧调一次, render 里 left/time 两个函数又各调一次 —— 每帧 3 次
 * localtime (含时区换算 + 内部锁). 改为统一走本缓存后, 每帧最多 0 次, 每秒 1 次. */
static struct tm s_tm_cache;
static bool      s_tm_valid = false;
static uint32_t  s_tm_ms    = 0;

static const struct tm *svc_time_cached(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_tm_valid || (uint32_t)(now - s_tm_ms) >= 1000) {
        time_t ts = time(NULL);
        struct tm *t = localtime(&ts);
        if (t) { s_tm_cache = *t; s_tm_valid = true; s_tm_ms = now; }
    }
    return s_tm_valid ? &s_tm_cache : NULL;
}

static void svc_tick(ui_ctx_t *ctx)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_ops) return;

    /* ★ 2026-11-23: 反白/声明都按页归属 —— 换页自动回到普通配色与"自动"显隐,
     *   应用退出时忘了还原也不会把下一页带成黑底白字。
     *   ⚠ 用"有效页"(弹窗期间沿用底层页), 否则暗色页一弹确认框配色就跳回普通。 */
    if (!os_sb_req_applies(s_inv_page, sb_effective_page())) s_inv = false;

    if (s_last_bat_ms == 0 || now - s_last_bat_ms >= 60000) {   /* 开机立即采一次, 之后每 60 秒 */
        s_last_bat_ms = now;
        uint8_t b = s_ops->get_battery ? s_ops->get_battery() : 255;
        if (b != s_bat) {
            s_bat = b;
            ctx->needs_redraw = true;
        }
        /* ★ 2026-11-21: 充电状态同节拍取 (充电中/已充满 都在这 60s 窗口内变),
         *   变了立即重绘 —— 否则插上充电线图标要等一分钟才出来。 */
        uint8_t c = s_ops->get_chg_state ? s_ops->get_chg_state() : 0;
        if (c != s_chg) {
            s_chg = c;
            ctx->needs_redraw = true;
        }
    }
    const struct tm *t = svc_time_cached();   /* 1s 节流, 不再每帧 localtime */
    if (t && t->tm_min != s_last_minute) {
        s_last_minute = t->tm_min;
        ctx->needs_redraw = true;
    }
    /* ★ 2026-10-27 linit: "正常 1 分钟更新一次, 但收到新信息 (电池/其它) 要及时刷新"。
     *   电量变化上面已经在刷 (60s 采样, 变化才重绘); 这里补上**连接类状态**:
     *   Wi-Fi / 蓝牙 由关到开、由未连到连上, 一变就立刻重绘一次(不等分钟跳变)。
     *   代价: 每帧两次只读 bool, 可忽略。 */
    {
        uint8_t ws = 0, bs = 0;
        if (s_ops->wifi_enabled   && s_ops->wifi_enabled())   ws |= 1;
        if (s_ops->wifi_connected && s_ops->wifi_connected()) ws |= 2;
        if (s_ops->bt_enabled     && s_ops->bt_enabled())     bs |= 1;
        if (s_ops->bt_connected   && s_ops->bt_connected())   bs |= 2;
        if (ws != s_wifi_state) { s_wifi_state = ws; ctx->needs_redraw = true; }
        if (bs != s_bt_state)   { s_bt_state   = bs; ctx->needs_redraw = true; }
    }
}

static const char *s_wd_cn[] = {"\xe6\x97\xa5","\xe4\xb8\x80","\xe4\xba\x8c","\xe4\xb8\x89",
                                "\xe5\x9b\x9b","\xe4\xba\x94","\xe5\x85\xad"};  /* 日/一/二/三/四/五/六 */

static void svc_draw_common_left(st7305_handle_t *lcd, int text_y)
{
    const int M = 4;
    const struct tm *t = svc_time_cached();   /* V1.2: 走 1s 节流缓存, 不再每帧 localtime */
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d-%02d", t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0);

    int x = M;
    /* ★ 2026-11-22 linit「状态栏字体保持原来的」: 一律走 _song (固定 16px 宋体原生),
     *   不跟随设置里的 宋/黑 × 16/24 切换 —— 状态栏是常驻信息条, 变粗/变大会挤位。 */
    ui_text16_song(lcd, x, text_y, date_str, s_inv);            /* 数字: 标准16px(英8×16) */
    x += ui_text16w(date_str) + 2;
    ui_text16_song(lcd, x, text_y, "\xe6\x98\x9f\xe6\x9c\x9f", s_inv);          /* 星期 */
    x += ui_text16w("\xe6\x98\x9f\xe6\x9c\x9f");
    ui_text16_song(lcd, x, text_y, s_wd_cn[t ? t->tm_wday : 0], s_inv);          /* 日/一/... */
}

static void svc_draw_common_time(st7305_handle_t *lcd, int text_y, int SCREEN_W)
{
    const struct tm *t = svc_time_cached();   /* V1.2: 1s 节流缓存 */
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t ? t->tm_hour : 0, t ? t->tm_min : 0);
    ui_text16_song_c(lcd, SCREEN_W / 2, text_y, time_str, s_inv);   /* 时间: 固定16px宋, 居中 */
}

static void svc_draw_common_right(st7305_handle_t *lcd, int text_y, int SCREEN_W,
                                  uint8_t bat, bool pad_connected, int *out_right_x)
{
    const int M = 4;
    const int GAP = 4;
    int right_x = SCREEN_W - M;
    svc_draw_battery(lcd, right_x - BAT_DW, text_y + 2, bat, s_chg);
    right_x -= BAT_DW + GAP;
    if (pad_connected) {
        /* ★ 2026-11-23 linit: 手柄图标改为 **24×12** (与电池同尺寸) 并 1:1 绘制。
         *   原来按 STATUS_ICON_W/H (32×16) 当目标尺寸画 32×16 位图 ⇒ 比电池宽 8px、
         *   高 4px, 状态栏里明显偏大。现在源图与槽位都是 24×12, 纵向也和电池一样
         *   落在 text_y + 2, 两者底边对齐。 */
        svc_draw_status_icon(lcd, right_x - PAD_ICON_W, text_y + 2,
                             PAD_ICON_W, PAD_ICON_H, (PAD_ICON_W + 7) / 8,
                             PAD_ICON_W, PAD_ICON_H, status_icon_pad_24x12);
        right_x -= PAD_ICON_W + GAP;
    }
    if (out_right_x) *out_right_x = right_x;
}

static void svc_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    /* 见文件头"删掉内容签名缓存"的说明: 只要本帧显示状态栏就必须落笔,
     * 否则页面整屏重绘后这一帧推上屏的就是"没有状态栏"的画面。
     * 整帧像素完全没变时, 驱动层 st7305_flush 的整帧指纹会跳过送屏, 不会多闪。 */

    const int text_y = 3;
    const int GAP = 4;
    const int SCREEN_W = ui_screen_w();

    /* ★ 2026-10-19: 状态栏区域**必须铺底**(普通=白/反色=黑), 否则页面画到 y<24 的文字
     * 会透上来和状态栏文字叠在一起 (linit: 很多界面都覆盖到状态栏了)。 */
    fill_rect(lcd, 0, 0, SCREEN_W - 1, SB_STRIP_H - 1,
              s_inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);

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
                             STATUS_ICON_W, STATUS_ICON_H, status_icon_data_32x16);
        right_x -= STATUS_ICON_W + GAP;
    }
    bool wifi_on = s_ops && s_ops->wifi_enabled && s_ops->wifi_enabled();
    bool wifi_conn = s_ops && s_ops->wifi_connected && s_ops->wifi_connected();
    /* ★ 2026-10-27 linit: "网络图标要及时出现" —— 只要 Wi-Fi 射频已开就画图标,
     *   不再等"已连上"(连 AP 通常要 2~5 秒, 那几秒状态栏是空的, 看着像没开)。
     *   未连上时图标右下角加一个 3×3 小叉, 和"已连"区分开(不换图标资源)。 */
    if (wifi_on) {
        /* ★ 2026-10-15: 目标框按**图标自身尺寸 (16×16 方形)** 画, 不再套 STATUS_ICON_W(32) ——
         *   否则 16 宽源图被横向拉成 2 倍, 弧线糊成一团 (linit: "WiFi 图标太丑")。 */
        svc_draw_status_icon(lcd, right_x - WIFI_ICON_W, text_y,
                             WIFI_ICON_W, WIFI_ICON_H, (WIFI_ICON_W + 7) / 8,
                             WIFI_ICON_W, WIFI_ICON_H, status_icon_wifi_16);
        if (!wifi_conn) {
            /* 未连接: 右下角画一个小叉 (3 像素对角线), 状态栏高度只有 24px, 不能再大 */
            int cx = right_x - 4, cy = text_y + WIFI_ICON_H - 4;
            for (int k = 0; k < 3; k++)
                st7305_draw_pixel(lcd, cx + k, cy + k, s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
            for (int k = 0; k < 3; k++)
                st7305_draw_pixel(lcd, cx + 2 - k, cy + k, s_inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
        }
        right_x -= WIFI_ICON_W + GAP;
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

/* 游戏内状态栏 — 与桌面共用同一套反白/普通绘制。
 * ★ 2026-11-23: 电量以**服务采样值**为准 (引擎传进来的只是兜底), 充电态/时间也
 *   走同一份缓存 ⇒ 游戏页与系统页的显示不会再出现"一个准一个旧"的两套数据。 */
void svc_statusbar_draw_game(st7305_handle_t *lcd, uint8_t battery, bool pad_connected) {
    if (!lcd) return;
    const int text_y = 3;
    const int SCREEN_W = UI_SCREEN_W;
    if (s_inv) fill_rect(lcd, 0, 0, SCREEN_W - 1, SB_STRIP_H - 1, ST7305_COLOR_BLACK);
    uint8_t bat = (s_bat != 255) ? s_bat : battery;
    svc_draw_common_left(lcd, text_y);
    svc_draw_common_time(lcd, text_y, SCREEN_W);
    svc_draw_common_right(lcd, text_y, SCREEN_W, bat, pad_connected, NULL);
}