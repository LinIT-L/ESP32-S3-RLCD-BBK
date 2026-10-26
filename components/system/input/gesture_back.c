/**
 * @file gesture_back.c
 * @brief 系统导航手势识别器 — 底部上滑=返回 / 状态栏长按=回桌面
 *
 * 设计参照见 gesture_back.h 头注释 (webOS / Android EdgeBackGestureHandler /
 * iOS 交互式返回 / LVGL 手势状态机). 由 input.c 在触摸 down/move/up 时喂入
 * 已映射+旋转的逻辑坐标; 所有导航手势的阈值与方向判定集中在此, 单一来源。
 */
#include "gesture_back.h"
#include "input.h"        /* MENU_ACTION_* */
#include "st7305.h"       /* ST7305_HEIGHT — 屏逻辑尺寸 (手势带全宽判定需要) */
#include "vibrator.h"     /* vibrator_click */

/* ===================== (A) 底部上滑 = 返回 ===================== */
/* 阈值 (逻辑像素 / ms). GB_BAND_THICK 见 gesture_back.h (单一来源, input.c 兜底用). */
#define GB_MIN_DIST    26    /* 慢滑返回最小距离 (原 36, 屏高 300 的 ~9%) */
#define GB_FAST_DIST   22    /* 快滑(flick)最小距离 */
#define GB_FAST_VEL   500    /* 快滑速度阈值 px/s (低于旧 600, 更灵敏) */
#define GB_MAX_OFFPATH 40    /* 最大横移容差 (原 60) */

/* 手势识别专用的"底部"判定 — 不复用 input_in_bottom_zone(), 因为后者限定了
 * MINX/MAXX 中心 150px (x∈[125,275]), 屏底左右两侧上滑会被排除在外.
 * 用户直觉"屏底向上滑 = 返回"是**全宽**的, 这里按 s_screen_rot 解析对应边,
 * 只对 default (rot=0) 做全宽底部检查; 非 default 由 input_in_bottom_zone 兜底. */
bool gesture_back_in_bottom_zone(int sx, int sy) {
    /* 注: input.c 的 rotate_screen_coord() 已把屏物理坐标转到逻辑坐标 (sx, sy),
     * 所以手势识别直接用逻辑坐标. 默认 rot=0 时逻辑底 = 物理底 = sy 接近 H. */
    /* ★ 2026-10-18 linit: 起点带收窄 —— 底边 30px 高 + 水平居中 200px 宽 */
    if (sy >= (ST7305_HEIGHT - GB_BAND_THICK)) {
        int x0 = (ST7305_WIDTH - GB_BAND_WIDE) / 2;
        if (sx >= x0 && sx < x0 + GB_BAND_WIDE) return true;
    }
    /* 兜底: 若非默认旋转 (status bar 在侧边), input_in_bottom_zone 已处理,
     * 这里也跟它一致: 只认中心带, 避免把"屏幕短边的水平滑动"误判成上滑. */
    return input_in_bottom_zone(sx, sy, GB_BAND_THICK);
}

/* (A) 状态机静态变量 */
static bool     gb_enabled  = true;   /* 默认开启 (由 os_push/os_pop 统一管控) */
static bool     gb_in_band  = false;  /* 本次按下起点是否在底部手势区 */
static int      gb_sx = 0, gb_sy = 0; /* 起点 (逻辑坐标) */
static int      gb_last_y = 0, gb_last_t = 0;
static int      gb_max_up = 0;        /* 过程中最大向上位移 */
static int      gb_vy_max = 0;        /* 过程中最大向上速度 px/s */
static uint32_t gb_last_sw_ms = 0;    /* V1.1.2: 上一次"明确上滑" release 的 ms — 用于双滑判定 */
#define DOUBLE_SW_MS 1000              /* 1s 内第二次上滑 → CONFIRM (强制退出) */

void gesture_back_enable(bool en) { gb_enabled = en; }

void gesture_back_on_press(int sx, int sy, uint32_t t_ms)
{
    gb_in_band = gb_enabled && gesture_back_in_bottom_zone(sx, sy);
    gb_sx = sx; gb_sy = sy;
    gb_last_y = sy; gb_last_t = (int)t_ms;
    gb_max_up = 0; gb_vy_max = 0;
}

void gesture_back_on_move(int sx, int sy, uint32_t t_ms)
{
    if (!gb_in_band) return;
    int up = gb_sy - sy;                 /* 当前向上位移 (正=上) */
    if (up > gb_max_up) gb_max_up = up;
    int dt = (int)t_ms - gb_last_t;
    if (dt > 0) {
        int vy = (gb_last_y - sy) * 1000 / dt;   /* 瞬时向上速度 px/s */
        if (vy > gb_vy_max) gb_vy_max = vy;
    }
    gb_last_y = sy; gb_last_t = (int)t_ms;
    /* V1.7.x (2026-09-28): 手势识别过程**不发触觉**.
     * 原来跨过第 1 档 (GB_MIN_DIST) 时震一下, 松手后动作处理 (os_handle_action 的
     * UI 震动) 又震一下 → 用户反馈"底部上滑返回有两次震动".
     * 现统一由动作处理侧发声: 一次上滑返回只响"动作生效"那一下. */
}

menu_action_t gesture_back_on_release(int sx, int sy, uint32_t t_ms)
{
    (void)t_ms;
    if (!gb_in_band) { gb_in_band = false; return MENU_ACTION_NONE; }
    gb_in_band = false;
    int dx = sx - gb_sx;
    int up = gb_sy - sy;                 /* 总向上位移 (正=上) */
    int adx = dx < 0 ? -dx : dx;
    if (up <= 0) return MENU_ACTION_NONE;                /* 不是向上滑 */
    if (up < adx) return MENU_ACTION_NONE;               /* 横移支配 → 不算向上滑 (角度判定) */
    if (adx > GB_MAX_OFFPATH) return MENU_ACTION_NONE;   /* 横移过大 → 不算 */
    /* 命中上滑: 检查距离 + 速度, 任何不满足都不算 (不让"一点点"触发返回) */
    bool hit = (up >= GB_MIN_DIST) ||
               (up >= GB_FAST_DIST && gb_vy_max >= GB_FAST_VEL);
    if (!hit) {
        gb_last_sw_ms = 0;   /* 不算上滑, 重置双滑计时器 */
        return MENU_ACTION_NONE;
    }
    /* V1.1.2: 双滑判定 — 1s 内第二次明确上滑 → CONFIRM (强制退出).
     * 真因: 单滑=返回是普通用户的"直觉手势"; 想退应用 / 急着退出时连滑两次
     * 是 webOS/iOS/Android EdgeBackGestureHandler 的标准"force exit" 路径.
     * 双滑给两次连续触觉 (区别单滑的一次轻咔哒), 用户清楚"我触发了强动作". */
    bool double_sw = gb_last_sw_ms &&
                     (int32_t)(t_ms - gb_last_sw_ms) <= DOUBLE_SW_MS;
    gb_last_sw_ms = t_ms;   /* 每次明确上滑都更新时间, 让连滑成立 */
    if (double_sw) {
        vibrator_click();   /* 第 2 次触觉, 与单滑的 1 次明显不同 */
        return MENU_ACTION_CONFIRM;
    }
    return MENU_ACTION_BACK;
}

/* ===================== (B) 状态栏长按 = 回桌面 (HOME) ===================== */
/* 阈值 (mirror input.c 的 TOUCH_STATUS_H / TOUCH_LONG_MS / TOUCH_STILL_MAX,
 * 集中到本模块后由这里作为唯一来源; 改这里即改全局行为) */
#define GH_STATUS_H    24    /* 状态栏高度: 按下点 y < 此值 视为状态栏 */
#define GH_LONG_MS   3000    /* 长按时长: 3 秒 */
#define GH_STILL_MAX   20    /* 长按允许抖动位移 */

bool gesture_home_decide(int sx0, int sy0, int cur_x, int cur_y, uint32_t dt_ms)
{
    if (sy0 >= GH_STATUS_H) return false;        /* 起点不在状态栏 */
    if (dt_ms < GH_LONG_MS)  return false;        /* 未到长按时长 */
    int dx = cur_x - sx0, dy = cur_y - sy0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= GH_STILL_MAX || ady >= GH_STILL_MAX) return false;  /* 移动过大, 不算长按 */
    return true;
}
