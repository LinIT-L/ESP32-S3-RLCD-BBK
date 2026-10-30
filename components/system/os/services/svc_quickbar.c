/**
 * svc_quickbar.c — 全局下拉快捷栏 (音量 / 蓝牙 / Wi-Fi)
 *
 * ── 形态 (400×300 单色屏, 2026-10-29) ──────────────────────────────────────
 *   从屏幕顶部向下展开一条 **40px** 高的横向控制栏:
 *
 *     x=1                                     x=366        x=399
 *     ┌──┐                                        ┌──┐
 *     │BT│  10px   [🔈 ▓▓▓▓▓░░░░░░░░░░]   10px   │WiFi│
 *     └──┘                                        └──┘
 *      45                                        355
 *      └────────── 音量控制区, 中心 = (45+355)/2 = 200 ──────────┘
 *
 *   几何推导 (把"中心严格 200"当成硬约束反解出来, 不是凑出来的):
 *     直径 34 的圆钮 ×2 + 间距 10 ×2 = 88; 400 - 88 = 312
 *     要让音量区中心正好 200, 区宽必须是**奇数**: 311 ⇒ x 从 45 到 355
 *     ⇒ 左钮 1..34, 间隙 35..44, 音量 45..355, 间隙 356..365, 右钮 366..399
 *     左右边距 1px / 0px —— 差 1px 是"中心 200"换来的, 视觉上不可辨。
 *
 * ── 交互 ─────────────────────────────────────────────────────────────────
 *   · 起手区: 顶部中央 200×10 (x 100..299, y 0..10), 只在**收起时**受理;
 *   · 下滑 ≥12px 展开; 展开后上滑 ≥12px 收起 (音量区内的拖动除外);
 *   · 展开期间抽屉范围 (y<40) 的触摸优先归本服务, 不透给下方页面/游戏;
 *   · 睡眠/屏保: 内核在屏保态直接 return, 本服务收不到 tick/touch, 天然不响应。
 *
 * ── 数据来源 ─────────────────────────────────────────────────────────────
 *   全部走 os_platform_ops_t 注入的 getter/setter, 不直接引用 bt_ctl /
 *   wifi_manager / audio_player —— 与状态栏保持同一套解耦约定。
 *   setter 为 NULL 时按钮画成"不可操作"(空心 + 叉), 绝不假装成功。
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "wallpapers.h"
#include "vid_tune.h"
#include "vibrator.h"   /* 音量档位咔哒反馈 */   /* 画面优化器: 快捷栏内入口 */   /* os_screensaver_active: 屏保/睡眠时不响应下拉 */
#include "esp_log.h"
/* 蓝牙 / Wi-Fi 图标直接用状态栏那一套位图, 保证与状态栏完全一致
 * (status_icons.inc 里的 static 数组, 各编译单元各存一份, 不会冲突)。 */
#include "status_icons.inc"
#include <string.h>

static const char *TAG = "quickbar";

/* ===== 布局 (px) =====
 *  2026-10-29 改版: 控件整体收拢居中, 只占 65..334 (原来铺满 1..399 显得过宽)。
 *  蓝牙/Wi-Fi 紧贴音量区两侧各 10px, 音量区中心仍严格 = 200。
 *
 *     57                        65    98      109        291        334   342
 *     ┌──────────────────────────┬─────┬────────┬──────────┬─────────┬──────┐
 *     │ 白底                                 │  [🔊 ▬▬▬●▬▬▬]        │            │
 *     └──────────────────────────┴─────┴────────┴──────────┴─────────┴──────┘
 *                              蓝牙34  10   喇叭16  滑轨  10  Wi-Fi34
 *                                        └──── 音量区 109..291, 中心 200 ────┘
 *  白底 57..342 (控件外各留 8px), 左右边距均为 57 ⇒ 视觉对称。
 */
#define QB_H         40          /* 抽屉高度 */
#define QB_BTN_D     34          /* 按钮边长 (方块) */
/* 蓝牙/Wi-Fi 图形尺寸: 原 32x32 与音量喇叭摆在一起显得偏大, 缩到 24x24 对齐 */
#define QB_ICON_SZ   24
#define QB_ICON_HALF (QB_ICON_SZ / 2)

/* ===== 24x24 专用图标 (2026-10-30 重绘) =====
 * 原来是把状态栏 16x16 位图硬放大 2 倍得到 32x32, 缩小到 24 后又用最近邻采样,
 * 线条会断、圆点变方。这两张是**按 24x24 直接画**的: 蓝牙是经典 BLE 符文
 * (竖干 + 上下两个三角), Wi-Fi 是底部圆点 + 三道同心弧。
 * 格式: 每行 3 字节, MSB 在前, 1 = 黑。 */
static const uint8_t qb_bt_24[3 * 24] = {
    0x00,0x00,0x00,
    0x00,0x10,0x00,
    0x00,0x1C,0x00,
    0x00,0x13,0x00,
    0x00,0x10,0x80,
    0x00,0x10,0x60,
    0x10,0x10,0x10,
    0x0C,0x10,0x60,
    0x02,0x10,0x80,
    0x01,0x93,0x00,
    0x00,0x54,0x00,
    0x00,0x38,0x00,
    0x00,0x38,0x00,
    0x00,0x54,0x00,
    0x01,0x93,0x00,
    0x02,0x10,0x80,
    0x0C,0x10,0x60,
    0x10,0x10,0x10,
    0x00,0x10,0x60,
    0x00,0x10,0x80,
    0x00,0x13,0x00,
    0x00,0x1C,0x00,
    0x00,0x10,0x00,
    0x00,0x00,0x00,
};

static const uint8_t qb_wifi_24[3 * 24] = {
    0x00,0x00,0x00,
    0x00,0x7E,0x00,
    0x01,0xFF,0x80,
    0x07,0x00,0xE0,
    0x0C,0x00,0x30,
    0x38,0x00,0x1C,
    0x20,0x00,0x04,
    0x60,0x00,0x06,
    0xC0,0x00,0x03,
    0x00,0x7E,0x00,
    0x01,0xC3,0x80,
    0x03,0x00,0xC0,
    0x06,0x00,0x60,
    0x0C,0x00,0x30,
    0x00,0x00,0x00,
    0x00,0x00,0x00,
    0x00,0x3C,0x00,
    0x00,0x66,0x00,
    0x00,0xC3,0x00,
    0x00,0x00,0x00,
    0x00,0x00,0x00,
    0x00,0x18,0x00,
    0x00,0x18,0x00,
    0x00,0x00,0x00,
};
#define QB_BTN_R     5           /* 圆角半径 R5 */
#define QB_BTN_Y0    3           /* 按钮上沿 */
#define QB_BTN_LX    65          /* 蓝牙: 65..98 */
#define QB_VOL_X0    109         /* 音量控制区左沿 */
#define QB_VOL_X1    291         /* 音量控制区右沿 → 中心 (109+291)/2 = 200 */
#define QB_SPK_X     109         /* 音量图标: 109..140 (MP3 同款 32×32) */
#define QB_BAR_X0    155         /* 滑轨起点(球心最小处): 图标右缘 140 + 球半径 15 = 155, 恰好相切不压住 */
#define QB_BAR_X1    291         /* 滑轨末端 */
#define QB_BAR_CY    20          /* 滑轨垂直中心 */
#define QB_BAR_HW    5           /* 滑轨半高 —— 与 MP3 音量条 MP3_VOL_HW 一致 */
#define QB_DOT_R     12          /* 圆球半径 —— 与 MP3 MP3_VOL_DOT_R 一致 */
#define QB_BTN_RX    301         /* Wi-Fi: 301..334 */
#define QB_PANEL_X0  57          /* 白底范围 (比控件各外扩 8px) */
#define QB_PANEL_X1  342         /* 面板右沿 —— 画面优化器未启用时 */
#define QB_OPT_X     344         /* 画面优化器图标: 仅"启用时"显示, 贴在 Wi-Fi 右侧 */
#define QB_OPT_SZ    28          /* 28×28 (2026-10-29 linit: 在 14 基础上放大一倍) */
#define QB_PANEL_X1B 379         /* 优化器显示时的面板右沿 */
#define QB_PANEL_R   6           /* 面板圆角 R6 */

/* ===== 画面调节展开态 (2026-10-30 linit 定稿) =====
 *   从 1 格(40) 下拉到 3 格(120): 左"保存" 右"取消", 中间 2×2 四条
 *   (亮度/对比/白点/黑点), 每条 = 2 字标签 + 与音量条同款的胶囊滑轨 + 圆球。 */
#define QB_H_EXP      120
#define QB_TUNE_X0    56
#define QB_TUNE_X1    344
#define QB_TUNE_CW    138        /* 每格宽 (含标签) */
#define QB_TUNE_ROW0  52
#define QB_TUNE_ROW1  86
#define QB_TUNE_TRK_W 92         /* 滑轨长度 */
#define QB_BTN_SAVE_X 8
#define QB_BTN_CANC_X (400 - 8 - 36)

/* 前向: 展开态渲染, 定义在 qb_touch 之前 */
static void qb_draw_tune(st7305_handle_t *lcd);

/* 手势 */
#define QB_HS_X0     100         /* 起手区 200×10, 水平居中 */
#define QB_HS_X1     299
#define QB_HS_Y1     10
#define QB_TH        12          /* 下拉/上滑判定阈值 */

/* ===== 状态 ===== */
static bool s_open;              /* 抽屉是否展开 */
static bool s_gesture;           /* 本次触摸是否已被本服务接管 */
static bool s_from_hs;           /* 本次手势是否从起手区开始 */
static bool s_vol_track;         /* 正在拖音量条 */
/* ★ 2026-10-29: 本次触摸已被"下拉打开抽屉"消费掉。
 *   不加这个会误调音量: 打开后 s_gesture 复位, 下一帧手指还在屏上,
 *   又按"新按下"处理, 而它正落在音量区 ⇒ 一拉下来音量就被改了。 */
static bool s_swallowed;
/* ★ 游戏内没有 os 的 tap 分发 (引擎阻塞主循环), 所以"点击按钮"必须本服务自己判:
 *   按下时记位置, 抬起时若没被下拉手势消费, 就当作一次点击交给 qb_touch。 */
static bool s_expand;            /* 画面调节展开态 */
static int  s_drag_param = -1;   /* 正在拖的滑条 0..3, -1 无 */
static bool s_tap_valid;
/* ★ 同一次按下是否已经由 os 触摸回调处理过 —— 防止"按下切换一次、抬手又切换一次"相互抵消 */
static bool s_press_done;
static int  s_tap_x, s_tap_y;
static int  s_start_y;

/* ===== 平台挂钩 ===== */
static const os_platform_ops_t *s_ops;
void os_quickbar_set_platform(const os_platform_ops_t *ops) { s_ops = ops; }

bool os_quickbar_is_open(void) { return s_open; }

/* 立刻收起 (2026-10-29: 按返回键时优先关快捷栏, 而不是退出页面/游戏) */
void os_quickbar_close(void)
{
    if (!s_open) return;
    s_open = false;
    s_gesture = false;
    s_from_hs = false;
    s_vol_track = false;
    input_set_overlay_lock(false);
}

/* ===== 小工具 ===== */
static int qb_isqrt(int v)
{
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* 实心圆 (逐行填矩形, 单色屏上就是最省的做法) */
static void qb_disc(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = qb_isqrt(r * r - dy * dy);
        fill_rect(lcd, cx - dx, cy + dy, cx + dx, cy + dy, c);
    }
}

/* 圆环: 外圆填 c, 内圆填反色 */
static void qb_ring(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c)
{
    qb_disc(lcd, cx, cy, r, c);
    qb_disc(lcd, cx, cy, r - 1, (c == ST7305_COLOR_BLACK) ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
}

/* ===== 位图与圆角矩形 ===== */

/* 1bpp 位图 → 指定颜色 (w 行对齐到字节) */
static void qb_blit_icon(st7305_handle_t *lcd, int x, int y, int w, int h,
                         const uint8_t *src, st7305_color_t c)
{
    int bpr = (w + 7) >> 3;
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            if (src[dy * bpr + (dx >> 3)] & (0x80u >> (dx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, c);
}

/* 圆角矩形填充: 中间十字矩形 + 四角整圆 (R 阶圆角) */
static void qb_fill_round_rect(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                               int r, st7305_color_t c)
{
    if (r <= 0) { fill_rect(lcd, x0, y0, x1, y1, c); return; }
    fill_rect(lcd, x0 + r, y0, x1 - r, y1, c);              /* 横向带 */
    fill_rect(lcd, x0, y0 + r, x0 + r - 1, y1 - r, c);      /* 左直边 */
    fill_rect(lcd, x1 - r + 1, y0 + r, x1, y1 - r, c);      /* 右直边 */
    qb_disc(lcd, x0 + r, y0 + r, r, c);                     /* 四角 */
    qb_disc(lcd, x1 - r, y0 + r, r, c);
    qb_disc(lcd, x0 + r, y1 - r, r, c);
    qb_disc(lcd, x1 - r, y1 - r, r, c);
}

/* 圆角矩形 1px 边框: 外框填色 → 内框填反色 */
static void qb_ring_round_rect(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                               int r, st7305_color_t c)
{
    st7305_color_t inv = (c == ST7305_COLOR_BLACK) ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    qb_fill_round_rect(lcd, x0, y0, x1, y1, r, c);
    qb_fill_round_rect(lcd, x0 + 1, y0 + 1, x1 - 1, y1 - 1, (r > 1) ? r - 1 : 0, inv);
}

/* ===== 图标: 全部取自状态栏位图 (status_icons.inc) =====
 * inv = true 表示画在实心黑底上, 用白色; 反之黑色。 */
/* 2 倍放大绘制 1bpp 位图 (最近邻, 每源像素画 2×2) */
static void qb_blit_1bpp_scaled(st7305_handle_t *lcd, int x, int y, int sw, int sh,
                              const uint8_t *src, int dw, int dh, st7305_color_t c)
{
    int bpr = (sw + 7) >> 3;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            if (src[sy * bpr + (sx >> 3)] & (0x80u >> (sx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, c);
        }
    }
}

/* 关闭态标记: 3px 粗斜杠 (无方框后靠这个区分开/关) */

/* 蓝牙: 裁出原图中间 16 列 → 16×16, 再**放大一倍**到 32×32; 无方框 */
static void qb_icon_bt_big(st7305_handle_t *lcd, int cx, int cy, bool on)
{
    (void)on;                                  /* 关闭态由外层的斜点线表示 */
    qb_blit_1bpp_scaled(lcd, cx - QB_ICON_HALF, cy - QB_ICON_HALF,
                        QB_ICON_SZ, QB_ICON_SZ, qb_bt_24,
                        QB_ICON_SZ, QB_ICON_SZ, ST7305_COLOR_BLACK);
}

/* Wi-Fi: 16×16 原图**放大一倍**到 32×32; 无方框 */
static void qb_icon_wifi_big(st7305_handle_t *lcd, int cx, int cy, bool on)
{
    (void)on;                                  /* 关闭态由外层的斜点线表示 */
    qb_blit_1bpp_scaled(lcd, cx - QB_ICON_HALF, cy - QB_ICON_HALF,
                        QB_ICON_SZ, QB_ICON_SZ, qb_wifi_24,
                        QB_ICON_SZ, QB_ICON_SZ, ST7305_COLOR_BLACK);
}

/* 音量图标: **直接复用 MP3 播放器音量键的那个 32×32 图标**
 *   (数据原样取自 apps/pages/mp3_icons.h 的 mp3_icon_vol_32)。
 *   os 层不能依赖 apps 组件, 所以这里复制了一份; 若 MP3 侧换图标, 需同步此处。 */
#define QB_ICON_VOL_W 32
#define QB_ICON_VOL_H 32
static const uint8_t qb_icon_vol[128] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00,
    0x00, 0x07, 0x80, 0x00, 0x00, 0x0C, 0x80, 0x00, 0x00, 0x18, 0x80, 0x00,
    0x00, 0x70, 0x81, 0x00, 0x07, 0xC0, 0x81, 0x80, 0x0C, 0x00, 0x80, 0x80,
    0x08, 0x00, 0x80, 0xC0, 0x08, 0x00, 0x88, 0xC0, 0x08, 0x00, 0x8C, 0x60,
    0x08, 0x00, 0x84, 0x60, 0x08, 0x00, 0x84, 0x60, 0x08, 0x00, 0x84, 0x60,
    0x08, 0x00, 0x8C, 0x60, 0x08, 0x00, 0x88, 0x40, 0x08, 0x00, 0x80, 0xC0,
    0x0C, 0x00, 0x80, 0x80, 0x07, 0xC0, 0x81, 0x80, 0x00, 0x70, 0x81, 0x00,
    0x00, 0x18, 0x80, 0x00, 0x00, 0x0C, 0x80, 0x00, 0x00, 0x07, 0x80, 0x00,
    0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void qb_icon_spk(st7305_handle_t *lcd, int x, int y, st7305_color_t c)
{
    const int bpr = (QB_ICON_VOL_W + 7) >> 3;
    for (int dy = 0; dy < QB_ICON_VOL_H; dy++)
        for (int dx = 0; dx < QB_ICON_VOL_W; dx++)
            if (qb_icon_vol[dy * bpr + (dx >> 3)] & (0x80u >> (dx & 7)))
                st7305_draw_pixel(lcd, x + dx, y + dy, c);
}

/* ===== 状态读取 ===== */
static bool qb_bt_on(void)   { return (s_ops && s_ops->bt_enabled)   ? s_ops->bt_enabled()   : false; }
static bool qb_wifi_on(void) { return (s_ops && s_ops->wifi_enabled) ? s_ops->wifi_enabled() : false; }
static int  qb_vol(void)     { return (s_ops && s_ops->get_volume)   ? s_ops->get_volume()   : 0; }

/* ===== 绘制 =====
 * 蓝牙 / Wi-Fi: **不要方框**, 直接用状态栏图标**整体放大一倍** (16→32px)。
 *   开 = 黑色图标; 关 = 图标 + 3px 斜杠。 */
/* 禁用态虚线框: 沿边框 3 像素实 / 3 像素空.
 * 关闭的蓝牙 / Wi-Fi 用它表示"当前禁用", 比原来的斜杠更直观,
 * 也不遮住图标本身 (斜杠会压在人字形中间, 小屏上容易糊成一团). */

/* 关闭态标记: **斜虚线** (3 像素实 / 3 像素空, 2px 粗), 压在图标上表示已关闭.
 * 用虚线而不是实线: 小屏 32px 图标上一条实斜杠太重, 会把图标本身的形状吃掉;
 * 断开之后既能看出"被划掉", 又能看清底下的蓝牙/Wi-Fi 图形。 */
static void qb_icon_slash_dashed(st7305_handle_t *lcd, int cx, int cy, int r)
{
    /* 点状斜线: 沿对角线每隔 4 步画一个 2x2 点.
     * 之前是 3px 实/3px 空, 连起来看还是一条粗斜杠; 改成独立小点后
     * 才真正是"一点一点", 既压住图标表示关闭, 又不盖掉形状. */
    for (int i = -r; i <= r; i++) {
        if (((i + r) % 4) != 0) continue;
        st7305_draw_pixel(lcd, cx + i,     cy + i,     ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx + i + 1, cy + i,     ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx + i,     cy + i + 1, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, cx + i + 1, cy + i + 1, ST7305_COLOR_BLACK);
    }
}

static void qb_draw_button(st7305_handle_t *lcd, int x, bool on, bool usable, int icon)
{
    int cx = x + QB_BTN_D / 2;
    int cy = QB_BTN_Y0 + QB_BTN_D / 2;
    bool active = on && usable;
    /* 关闭态: 图标只表示"这是蓝牙/Wi-Fi", 用虚线框统一表示不可用 */
    if (!active) qb_icon_slash_dashed(lcd, cx, cy, 11);   /* 关闭: 斜虚线 */
    if (icon == 0) qb_icon_bt_big(lcd, cx, cy, active);
    else           qb_icon_wifi_big(lcd, cx, cy, active);
}

/* 画面优化器图标 (28×28): 白底黑框 + 三根滑条, 与 vid_tune 浮标同款 */
static void qb_icon_opt(st7305_handle_t *lcd, int x, int y)
{
    fill_rect(lcd, x, y, x + QB_OPT_SZ - 1, y + QB_OPT_SZ - 1, ST7305_COLOR_WHITE);
    draw_rect_outline(lcd, x, y, x + QB_OPT_SZ - 1, y + QB_OPT_SZ - 1, ST7305_COLOR_BLACK);
    for (int k = 0; k < 3; k++) {
        int cx = x + 6 + k * 8;
        fill_rect(lcd, cx, y + 6, cx + 1, y + QB_OPT_SZ - 7, ST7305_COLOR_BLACK);
        fill_rect(lcd, cx - 2, y + 8 + k * 6, cx + 3, y + 11 + k * 6, ST7305_COLOR_BLACK);
    }
}

/* 横向音量条 —— 完全复用 MP3 播放器那一条的样式与尺寸:
 *   滑轨 = 外白圈(hw+3) → 黑轮廓(hw) → 白芯(hw-1) 三层;
 *   滑块 = 黑芯(r) + 3px 白圈;  无"已选段填充"。
 *   横向化: 中间矩形 + 两端半圆拼成胶囊 (与竖版的 capsule 等价)。 */
static void qb_draw_volume(st7305_handle_t *lcd)
{
    int vol = qb_vol();
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    const int cy = QB_BAR_CY;

    /* 音量图标 (32×32, 垂直居中于 40px 抽屉: 4..35) */
    qb_icon_spk(lcd, QB_SPK_X, cy - 16, ST7305_COLOR_BLACK);

    /* ① 外白圈 (比黑轮廓外扩 3px): 矩形主体 + 两端圆 */
    fill_rect(lcd, QB_BAR_X0, cy - QB_BAR_HW - 3, QB_BAR_X1, cy + QB_BAR_HW + 3, ST7305_COLOR_WHITE);
    qb_disc(lcd, QB_BAR_X0, cy, QB_BAR_HW + 3, ST7305_COLOR_WHITE);
    qb_disc(lcd, QB_BAR_X1, cy, QB_BAR_HW + 3, ST7305_COLOR_WHITE);

    /* ② 黑轮廓 */
    fill_rect(lcd, QB_BAR_X0, cy - QB_BAR_HW, QB_BAR_X1, cy + QB_BAR_HW, ST7305_COLOR_BLACK);
    qb_disc(lcd, QB_BAR_X0, cy, QB_BAR_HW, ST7305_COLOR_BLACK);
    qb_disc(lcd, QB_BAR_X1, cy, QB_BAR_HW, ST7305_COLOR_BLACK);

    /* ③ 白芯 (比黑轮廓内缩 1px) */
    fill_rect(lcd, QB_BAR_X0, cy - QB_BAR_HW + 1, QB_BAR_X1, cy + QB_BAR_HW - 1, ST7305_COLOR_WHITE);
    qb_disc(lcd, QB_BAR_X0, cy, QB_BAR_HW - 1, ST7305_COLOR_WHITE);
    qb_disc(lcd, QB_BAR_X1, cy, QB_BAR_HW - 1, ST7305_COLOR_WHITE);

    /* ④ 圆球: 白圈 3px + 黑芯; 位置随音量在滑轨上移动 */
    int cx = QB_BAR_X0 + ((QB_BAR_X1 - QB_BAR_X0) * vol) / 100;
    qb_disc(lcd, cx, cy, QB_DOT_R + 3, ST7305_COLOR_WHITE);
    qb_disc(lcd, cx, cy, QB_DOT_R, ST7305_COLOR_BLACK);
}

static void qb_render(ui_ctx_t *ctx)
{
    if (!s_open || !ctx || !ctx->lcd) return;
    st7305_handle_t *lcd = ctx->lcd;

    /* ★ 面板: **顶部两角直角** (贴屏幕边, 不留 R 角) + 底部保留 R6 圆角。
     *   做法: 外框 = 上部矩形 + 底部圆角块; 内部同形状填白, 整体内缩 1px 形成边框。 */
    const bool opt_on = vid_tune_enabled();   /* 画面优化器开启才在 Wi-Fi 右侧多一个图标 */
    const int  qbH   = s_expand ? QB_H_EXP : QB_H;   /* 展开态: 3 格高 */
    {
        const int x0 = QB_PANEL_X0, x1 = opt_on ? QB_PANEL_X1B : QB_PANEL_X1;
        const int y1 = qbH - 1, r = QB_PANEL_R;
        fill_rect(lcd, x0, 0, x1, y1 - r, ST7305_COLOR_BLACK);
        qb_fill_round_rect(lcd, x0, y1 - 2 * r, x1, y1, r, ST7305_COLOR_BLACK);
        fill_rect(lcd, x0 + 1, 1, x1 - 1, y1 - r, ST7305_COLOR_WHITE);
        qb_fill_round_rect(lcd, x0 + 1, y1 - 2 * r, x1 - 1, y1 - 1,
                           (r > 1) ? r - 1 : 0, ST7305_COLOR_WHITE);
    }

    bool bt_ok   = (s_ops && s_ops->set_bt_enabled);
    bool wifi_ok = (s_ops && s_ops->set_wifi_enabled);
    qb_draw_button(lcd, QB_BTN_LX, qb_bt_on(),   bt_ok,   0);
    qb_draw_button(lcd, QB_BTN_RX, qb_wifi_on(), wifi_ok, 1);
    if (opt_on) qb_icon_opt(lcd, QB_OPT_X, (QB_H - QB_OPT_SZ) / 2);   /* 垂直居中 */
    qb_draw_volume(lcd);
    if (s_expand) qb_draw_tune(lcd);   /* 展开态: 2×2 四条 + 保存/取消 */
}

/* ===== 操作 ===== */
/* ★ 2026-10-30 音量改为**离散档位** (0..10 档, 每档 10%):
 *   - 滑块位置吸附到最近档位, 松手后不会停在两档之间;
 *   - **只有跨档才震动一次** (咔哒感), 同一档内反复滑动不重复震;
 *   - 震动走 vibrator_tap_ui, 遵守"菜单震动"开关 —— 关了就完全不震 (不强制)。
 *   - 松手不震: 本条只在档位真的变化时触发, 抬手路径不调它。 */
#define QB_VOL_STEPS 10
static int s_last_step = -1;      /* 本次拖动上一次的档位, -1 = 尚未定档 */

static void qb_set_vol(ui_ctx_t *ctx, int x)
{
    int span = QB_BAR_X1 - QB_BAR_X0;
    int raw = ((x - QB_BAR_X0) * 100) / (span > 0 ? span : 1);
    if (raw < 0) raw = 0;
    if (raw > 100) raw = 100;

    /* 吸附到 0..QB_VOL_STEPS 的最近档, 再换算回百分比 (保持 set_volume 语义不变) */
    int step = (raw * QB_VOL_STEPS + 50) / 100;
    if (step < 0) step = 0;
    if (step > QB_VOL_STEPS) step = QB_VOL_STEPS;
    int v = step * 100 / QB_VOL_STEPS;

    if (s_ops && s_ops->set_volume) s_ops->set_volume(v);

    /* 跨档才咔哒一下; 同档内不重复 (快速划过也只按实际经过的档位各震一次) */
    if (step != s_last_step) {
        s_last_step = step;
        vibrator_tap_ui(8, 60);      /* 8ms / 60%: 短促机械感, 遵守 UI 震动开关 */
    }
    if (ctx) ctx->needs_redraw = true;
}

static void qb_toggle_bt(ui_ctx_t *ctx)
{
    if (!s_ops || !s_ops->set_bt_enabled) {
        ESP_LOGW(TAG, "蓝牙不可操作 (未注入 set_bt_enabled)");
        return;
    }
    s_ops->set_bt_enabled(!qb_bt_on());
    if (ctx) ctx->needs_redraw = true;
}

static void qb_toggle_wifi(ui_ctx_t *ctx)
{
    if (!s_ops || !s_ops->set_wifi_enabled) {
        ESP_LOGW(TAG, "Wi-Fi 不可操作 (未注入 set_wifi_enabled)");
        return;
    }
    bool want = !qb_wifi_on();
    if (!s_ops->set_wifi_enabled(want))
        ESP_LOGW(TAG, "Wi-Fi %s 失败 — 按钮状态保持真实值", want ? "开启" : "关闭");
    if (ctx) ctx->needs_redraw = true;
}

/* ===== 手势 (每帧轮询) =====
 * 点击事件 (tap) 走 qb_touch; 持续拖动只能轮询 input_get_touch_pos,
 * 与 os_pane / 各页面拖动的做法一致。 */
/* ===== 画面调节展开态 (2026-10-30 linit 定稿) =====
 *   快捷栏从 1 格(40) 下拉到 3 格(120):
 *     左 = 保存, 右 = 取消, 中间 2×2 四条 (亮度/对比/白点/黑点),
 *     每条 = 2 字标签 + 与音量条同款的横向胶囊滑轨 + 圆球。 */
static const char *qb_tune_label(int i)
{
    static const char *L[4] = {
        "\xe4\xba\xae\xe5\xba\xa6",   /* 亮度 */
        "\xe5\xaf\xb9\xe6\xaf\x94",   /* 对比 */
        "\xe7\x99\xbd\xe7\x82\xb9",   /* 白点 */
        "\xe9\xbb\x91\xe7\x82\xb9",   /* 黑点 */
    };
    return L[i & 3];
}
static int qb_tune_bx(int i)   { return QB_TUNE_X0 + (i & 1) * QB_TUNE_CW; }
static int qb_tune_rowy(int i) { return (i < 2) ? QB_TUNE_ROW0 : QB_TUNE_ROW1; }
static int qb_tune_trk_x0(int i) { return qb_tune_bx(i) + 40; }
static int qb_tune_trk_x1(int i) { return qb_tune_trk_x0(i) + QB_TUNE_TRK_W; }

static void qb_draw_tune(st7305_handle_t *lcd)
{
    for (int i = 0; i < 4; i++) {
        int bx = qb_tune_bx(i), y = qb_tune_rowy(i);
        int x0 = qb_tune_trk_x0(i), x1 = qb_tune_trk_x1(i);
        int cy = y + 6;

        draw_text(lcd, bx, y, qb_tune_label(i), false);   /* 2 字标签 */

        /* 滑轨: 与音量条同款三层胶囊 (外白圈 → 黑轮廓 → 白芯) */
        fill_rect(lcd, x0, cy - QB_BAR_HW - 2, x1, cy + QB_BAR_HW + 2, ST7305_COLOR_WHITE);
        qb_disc(lcd, x0, cy, QB_BAR_HW + 2, ST7305_COLOR_WHITE);
        qb_disc(lcd, x1, cy, QB_BAR_HW + 2, ST7305_COLOR_WHITE);
        fill_rect(lcd, x0, cy - QB_BAR_HW, x1, cy + QB_BAR_HW, ST7305_COLOR_BLACK);
        qb_disc(lcd, x0, cy, QB_BAR_HW, ST7305_COLOR_BLACK);
        qb_disc(lcd, x1, cy, QB_BAR_HW, ST7305_COLOR_BLACK);
        fill_rect(lcd, x0, cy - QB_BAR_HW + 1, x1, cy + QB_BAR_HW - 1, ST7305_COLOR_WHITE);
        qb_disc(lcd, x0, cy, QB_BAR_HW - 1, ST7305_COLOR_WHITE);
        qb_disc(lcd, x1, cy, QB_BAR_HW - 1, ST7305_COLOR_WHITE);

        /* 圆球 (黑芯 + 白圈), 位置 = 当前参数 */
        int pct = vid_tune_get_pct(i);
        int kx = x0 + (QB_TUNE_TRK_W * pct) / 100;
        qb_disc(lcd, kx, cy, 7, ST7305_COLOR_WHITE);
        qb_disc(lcd, kx, cy, 5, ST7305_COLOR_BLACK);
    }

    /* 左右按钮 */
    draw_rect_outline(lcd, QB_BTN_SAVE_X, 62, QB_BTN_SAVE_X + 35, 98, ST7305_COLOR_BLACK);
    draw_text(lcd, QB_BTN_SAVE_X + 2, 76, "\xe4\xbf\x9d\xe5\xad\x98", false);   /* 保存 */
    draw_rect_outline(lcd, QB_BTN_CANC_X, 62, QB_BTN_CANC_X + 35, 98, ST7305_COLOR_BLACK);
    draw_text(lcd, QB_BTN_CANC_X + 2, 76, "\xe5\x8f\x96\xe6\xb6\x88", false);   /* 取消 */
}

/* 展开态的触摸: 拖滑条 / 点保存 / 点取消。返回 true 表示已消费。 */
static bool qb_tune_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 按钮: y 62..98 */
    if (y >= 58 && y <= 102) {
        if (x >= QB_BTN_SAVE_X && x <= QB_BTN_SAVE_X + 35) {
            vid_tune_save();
            s_expand = false;
            if (ctx) ctx->needs_redraw = true;
            return true;
        }
        if (x >= QB_BTN_CANC_X && x <= QB_BTN_CANC_X + 35) {
            vid_tune_restore();
            s_expand = false;
            if (ctx) ctx->needs_redraw = true;
            return true;
        }
    }
    /* 滑条命中 (上下各放宽 12px 便于手指) */
    for (int i = 0; i < 4; i++) {
        int x0 = qb_tune_trk_x0(i), x1 = qb_tune_trk_x1(i);
        int y0 = qb_tune_rowy(i);
        if (y >= y0 - 10 && y <= y0 + 22 && x >= x0 - 8 && x <= x1 + 8) {
            int pct = (x - x0) * 100 / QB_TUNE_TRK_W;
            vid_tune_set_pct(i, pct);
            s_drag_param = i;
            return true;
        }
    }
    return true;   /* 展开区其它位置也吃掉, 不落到游戏 */
}

static bool qb_touch(ui_ctx_t *ctx, int x, int y);   /* 前向: 游戏内点击复用同一套判定 */
static void qb_tick(ui_ctx_t *ctx)
{
    /* ★ 2026-10-29 linit: 屏保/睡眠/动态壁纸状态下不响应下拉。
     *   此时既不该弹出抽屉, 也不该保持展开 (退出时顺带归还触摸控制权)。 */
    if (os_screensaver_active()) {
        if (s_open) {
            s_open = false;
            input_set_overlay_lock(false);
        }
        s_gesture = false;
        s_from_hs = false;
        s_vol_track = false;
        return;
    }

    int tx = 0, ty = 0;
    /* 用 _raw 读: 本服务自己必须能看到触摸, 即使已经置了独占锁 */
    bool down = input_get_touch_pos_raw(&tx, &ty);

    if (!down) {
        /* 抬起: 若这次不是"下拉打开"而是"点了一下", 就按按钮处理。
         * 游戏内没有 os_handle_touch, 这是唯一的点击通路。 */
        /* ⚠ 2026-10-30 修"松手弹回": 必须排除 s_vol_track ——
         * 拖动过音量条时不能把这个手势再当"点击", 否则会用**按下位置**重设音量,
         * 把刚拖到的档位覆盖掉 (这正是弹回的真正原因)。 */
        if (s_open && s_tap_valid && !s_swallowed && !s_vol_track && s_tap_y < QB_H)
            qb_touch(ctx, s_tap_x, s_tap_y);
        /* 本次手势结束, 允许下一次按下重新处理 (放在抬手侧复位,
         * 才能保证"按下即生效", 而不是从第二次起变成松手才响应) */
        s_press_done = false;
        s_tap_valid = false;
        s_gesture = false;
        s_from_hs = false;
        s_vol_track = false;
        s_swallowed = false;      /* 抬手: 本次手势彻底结束, 下次按下可正常调音量 */
        s_last_step = -1;         /* 下次拖动第一档也应有咔哒反馈 */
        s_start_y = -1;
        /* 手指抬起: 抽屉还开着就继续独占(免得点穿到下层), 已收起则立即还给下层 */
        input_set_overlay_lock(s_open);
        return;
    }

    if (!s_gesture) {
        s_gesture = true;
        s_start_y = ty;
        s_vol_track = false;
        s_tap_valid = true;          /* 记录本次按下的落点, 供抬起时判点击 */
        s_tap_x = tx;
        s_tap_y = ty;
        /* 只在收起时认顶部起手区 */
        s_from_hs = (!s_open && tx >= QB_HS_X0 && tx <= QB_HS_X1 && ty <= QB_HS_Y1);
        /* ★ 关键: 从起手区按下、或抽屉已展开 → 立刻独占触摸,
         *   使下方主菜单/列表的 poll 拿不到手指, 下拉时不会跟着滚动。 */
        if (s_from_hs || s_open) input_set_overlay_lock(true);
        /* 已展开且按在音量区: 直接进入跟手调音量。
         * ⚠ s_swallowed 必须判: 刚用这次触摸把抽屉拉下来时, 不许顺手改音量。 */
        if (s_open && !s_swallowed && ty < QB_H &&
            tx >= QB_VOL_X0 && tx <= QB_VOL_X1) {
            s_vol_track = true;
            qb_set_vol(ctx, tx);
        }
        return;
    }

    if (s_vol_track) {
        qb_set_vol(ctx, tx);
        return;
    }

    int dy = ty - s_start_y;
    if (!s_open) {
        if (s_from_hs && dy >= QB_TH) {
            s_open = true;
            s_gesture = false;          /* 一次手势只做一件事 */
            s_swallowed = true;         /* ★ 本次触摸只用于打开, 不触发音量 */
            if (ctx) ctx->needs_redraw = true;
            ESP_LOGI(TAG, "展开 (dy=%d)", dy);
        }
    } else if (dy <= -QB_TH && !s_from_hs) {
        s_open = false;
        s_gesture = false;
        if (ctx) ctx->needs_redraw = true;
        ESP_LOGI(TAG, "收起 (dy=%d)", dy);
    }
}

/* ===== 触摸 (tap) ===== */
static bool qb_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 展开态: 全部交给调节区 (滑条/保存/取消) */
    if (s_open && s_expand) return qb_tune_touch(ctx, x, y);
    if (!s_open) {
        /* 收起时只占住起手区: 消费掉, 避免下层把它当点击 (下拉由 tick 完成) */
        return (y <= QB_HS_Y1 && x >= QB_HS_X0 && x <= QB_HS_X1);
    }

    /* 抽屉内: 一次按下只认一次动作.
     * 普通页面里 qb_touch 会被调两次 —— os 的触摸回调(按下) + qb_tick 的抬手判定
     * (为游戏内没有 os 触摸分发而留的通路)。两次都执行 toggle 就等于没按,
     * 现象正是"按一下立刻恢复原状"。 */
    if (s_press_done) return true;
    s_press_done = true;

    /* 抽屉下方: 关闭并吃掉这一次点击, 防止误触下方页面/游戏 */
    if (y >= QB_H) {
        s_open = false;
        s_expand = false;
        input_set_overlay_lock(false);   /* 收起即归还触摸, 下层马上恢复响应 */
        if (ctx) ctx->needs_redraw = true;
        return true;
    }

    if (x >= QB_BTN_LX && x < QB_BTN_LX + QB_BTN_D) { qb_toggle_bt(ctx);   return true; }
    if (x >= QB_BTN_RX && x < QB_BTN_RX + QB_BTN_D) { qb_toggle_wifi(ctx); return true; }
    if (vid_tune_enabled() && x >= QB_OPT_X && x < QB_OPT_X + QB_OPT_SZ && y < QB_H) {
        /* 画面优化器: 快捷栏**就地向下展开到 3 格**, 显示 2×2 调节条 */
        vid_tune_begin_edit();     /* 备份当前值, 供"取消"还原 */
        s_expand = true;
        if (ctx) ctx->needs_redraw = true;
        return true;
    }
    if (x >= QB_VOL_X0 && x <= QB_VOL_X1)           { qb_set_vol(ctx, x);  return true; }

    return true;   /* 抽屉内的空白也算本服务消费 */
}

/* ===== 服务注册 ===== */
static const os_service_t s_svc_quickbar = {
    .name        = "quickbar",
    .tick        = qb_tick,
    .render      = qb_render,
    .touch       = qb_touch,
    .overlay_top = true,     /* 全屏游戏/应用里也要能画、能收触摸 */
};

/* ★ 2026-10-29 修「游戏全屏下拉不出快捷栏」:
 *   游戏引擎运行时**阻塞主循环** (eng_task_run_loop / gam4980_emu_run 自己 while),
 *   os_tick 不执行 → 本服务的 tick/render 一次都不会被调到, 所以全屏游戏里拉不出来。
 *   这个接口由引擎的**帧循环**直接调用 (与 vid_tune_overlay_tick / virtual_keys 同一层),
 *   内部: 收手势 → 需要时把抽屉画在游戏帧上面。flush 仍由引擎负责。 */
void os_quickbar_game_frame(st7305_handle_t *lcd)
{
    if (!lcd || !s_ops) return;
    qb_tick(NULL);                        /* ctx=NULL: 内部已判空, 仅用于收手势 */
    if (s_open || s_gesture) {
        /* 游戏内没有 ui_ctx_t: 造一个只带 lcd 的临时上下文给渲染用
         * (qb_render 只读 ctx->lcd, 其余字段写回无效但不影响)。 */
        ui_ctx_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.lcd = lcd;
        qb_render(&tmp);
    }
}

/* void* 适配层: input 层的钩子用 void* 以避免依赖 st7305 类型 */
static void qb_game_frame_void(void *lcd)
{
    os_quickbar_game_frame((st7305_handle_t *)lcd);
}

void os_svc_quickbar_register(void)
{
    os_register_service(&s_svc_quickbar);
    /* 把实现注入给引擎层: 阻塞式引擎每帧调 engine_overlay_quickbar() 就能用 */
    engine_overlay_set_quickbar(qb_game_frame_void);
}
