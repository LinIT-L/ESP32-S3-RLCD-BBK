/**
 * page_main.c — 主菜单桌面 页面模块 (P2 试点页).
 *
 * 从 menu_system.c 的 render_main 迁移, 视觉与旧版一致:
 *   - 白底 + 状态栏(全局 service 叠加) + 大图标横向轮盘
 *   - 中间图标 100x100, 其余 64x64; 图标下方短标签
 *   - 相机(cam, 单位:格, 可小数): 拖动跟手 / 按键缓动 / 静止=选中
 *   - 私有 state 全部 static 留本文件, 不放进 ui_ctx.
 *
 * 依赖: ui_common 绘制原语 + input (触摸坐标拖动).
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============ 模块注册表 (主菜单显示的应用) ============
 * 迁移规则: 每新增/点亮一个应用, 在此加一行; 私有实现留在各自 pages 目录.
 * icon_idx = 主菜单 SF 图标索引 (见 icons_main.inc 的 main_icons[] 表). */
typedef struct {
    const char *label;     /* 图标下方短标签 (UTF-8) */
    int         icon_idx;  /* SF 图标索引 */
    os_page_t   page;      /* 进入的目标页 */
} os_main_mod_t;

static const os_main_mod_t s_main_mods[] = {
    { "\xe6\xad\xa5\xe6\xad\xa5\xe9\xab\x98", 21, OS_PAGE_SELECT_GAME },  /* 步步高 */
    { "\xe9\x98\x85\xe8\xaf\xbb",              9, OS_PAGE_BOOK },          /* 阅读 */
    { "\xe9\x9f\xb3\xe4\xb9\x90",              7, OS_PAGE_MP3_PLAYER },    /* 音乐 */
    { "\xe6\x89\x8b\xe6\x9f\x84",              3, OS_PAGE_GAMEPAD },       /* 手柄 */
    { "\xe5\xa3\x81\xe7\xba\xb8",             10, OS_PAGE_WALLPAPER },     /* 壁纸 */
    { "\xe7\x95\xaa\xe8\x8c\x84\xe9\x92\x9f", 11, OS_PAGE_POMODORO },      /* 番茄钟 */
    { "\xe4\xbb\xbf\xe7\x9c\x9f\xe9\x94\xae\xe9\xbc\xa0", 1, OS_PAGE_USB_HID }, /* 仿真键鼠 */
    { "\xe5\xad\x98\xe5\x82\xa8",              5, OS_PAGE_SETTINGS },       /* 存储 */
    { "\xe8\xae\xbe\xe7\xbd\xae",              6, OS_PAGE_SETTINGS },       /* 设置 */
    { "\xe5\xba\x94\xe7\x94\xa8\xe7\xae\xa1\xe7\x90\x86", 0, OS_PAGE_APP_MANAGER }, /* 应用管理 */
    { "\xe7\x99\xbd\xe6\x9d\xbf",             18, OS_PAGE_WHITEBOARD },    /* 白板 */
    { "\xe7\xbb\x88\xe7\xab\xaf",             23, OS_PAGE_TERMINAL },      /* 终端 */
};
#define MAIN_MOD_COUNT (sizeof(s_main_mods) / sizeof(s_main_mods[0]))

/* ============ 私有状态 (相机/拖动/动画/选中) ============ */
typedef struct {
    float   cam_cur;          /* 当前相机 (格, 可小数) */
    float   cam_base;         /* 拖动起点相机 */
    int     cam_anim_sel;     /* 动画锁定选中 (外部改选中则作废动画) */
    float   cam_anim_from;
    float   cam_anim_to;
    uint32_t cam_anim_start;
    uint32_t cam_anim_dur;
    bool    drag_active;
    int     drag_start_x;
    int     drag_offset;
    int     drag_moved;
    uint32_t suppress_until;  /* 拖动松手后短暂抑制 swipe 方向键/确认 (ms) */
    int     selected;         /* 选中显示索引 (0..MAIN_MOD_COUNT-1) */
} main_state_t;
static main_state_t s_main;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool input_suppressed(void) { return now_ms() < s_main.suppress_until; }

static int mod_wrap(int v) {
    int n = (int)MAIN_MOD_COUNT;
    v %= n;
    if (v < 0) v += n;
    return v;
}

/* 动画: 相机 from -> to (to 取与 from 最近的同余, 走 wrap 最短路径) */
static void main_anim_to(ui_ctx_t *ctx, int target) {
    int total = (int)MAIN_MOD_COUNT;
    /* 把 target 归到与当前相机同余的最近值 */
    float cur = s_main.cam_cur;
    int curi = lroundf(cur);
    int dt = mod_wrap(target - curi);
    if (dt > total / 2) dt -= total;
    float to = (float)(curi + dt);
    s_main.cam_anim_sel  = target;
    s_main.cam_anim_from = cur;
    s_main.cam_anim_to   = to;
    s_main.cam_anim_start = now_ms();
    s_main.cam_anim_dur   = 260 + (uint32_t)(abs(dt) < 0 ? 0 : abs(dt)) * 60;
    if (s_main.cam_anim_dur > 700) s_main.cam_anim_dur = 700;
    s_main.selected = target;
    ctx->needs_redraw = true;
}

/* 图标尺寸: 中央放大图标固定 100px, 其余固定 64px (见 render 内钉中央逻辑). */

/* ============ 渲染 ============ */
static void p_main_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    int total = (int)MAIN_MOD_COUNT;

    st7305_clear(lcd, ST7305_COLOR_WHITE);

    const int icon_spacing = 100;
    const int center_x = UI_SCREEN_W / 2;
    const int icon_center_y = UI_SCREEN_H / 2;
    uint32_t now = now_ms();

    /* 相机: 拖动跟手 / 动画缓动 / 静止=选中 */
    float cam;
    if (s_main.drag_active) {
        cam = s_main.cam_base - (float)s_main.drag_offset / (float)icon_spacing;
    } else if (s_main.cam_anim_start > 0) {
        if (s_main.selected != s_main.cam_anim_sel) {
            s_main.cam_anim_start = 0;
            cam = (float)s_main.selected;
        } else {
            uint32_t d = now - s_main.cam_anim_start;
            float tt = d < s_main.cam_anim_dur ? (float)d / (float)s_main.cam_anim_dur : 1.0f;
            float ease = 1.0f - (float)pow(1.0f - tt, 3.0f);   /* 缓出 */
            cam = s_main.cam_anim_from +
                  (s_main.cam_anim_to - s_main.cam_anim_from) * ease;
            if (tt >= 1.0f) {
                s_main.cam_anim_start = 0;
                cam = s_main.cam_anim_to;
            }
        }
    } else {
        cam = (float)s_main.selected;
    }
    s_main.cam_cur = cam;

    int sel = mod_wrap(lroundf(cam));
    int pinned = lroundf(cam);           /* 放大图标槽位 (固定在中央) */

    /* 遍历相机附件的整数格, 屏内即绘制 (索引取模回绕).
     * 中央放大图标(round(cam))始终钉在 center_x; 其余图标连续滑动,
     * 相邻图标被推开防止与中央 100px 图标重叠. */
    int g0 = (int)floorf(cam) - 3;
    int g1 = (int)ceilf(cam) + 3;
    const int big_half = 50, small_half = 32, gap = 2;   /* 半宽 50/32 + 间距 */
    for (int g = g0; g <= g1; g++) {
        int idx = mod_wrap(g);
        int cx, size;
        if (g == pinned) {
            cx = center_x;               /* 固定中央 200px */
            size = 100;
        } else {
            cx = center_x + (int)roundf(((float)g - cam) * (float)icon_spacing);
            size = 64;
            /* 推开相邻图标, 避免与中央放大图标重叠 */
            if (g == pinned + 1) {
                int minc = center_x + big_half + small_half + gap;
                if (cx < minc) cx = minc;
            } else if (g == pinned - 1) {
                int maxc = center_x - big_half - small_half - gap;
                if (cx > maxc) cx = maxc;
            }
        }
        if (cx + size > 0 && cx - size < UI_SCREEN_W) {
            draw_main_icon_stretched(lcd, cx, icon_center_y, size, size,
                                     s_main_mods[idx].icon_idx);
        }
    }

    /* 选中图标下方短标签 */
    int label_y = icon_center_y + 68;
    if (label_y > UI_SCREEN_H - 28) label_y = UI_SCREEN_H - 28;
    draw_label_centered_at(lcd, center_x, label_y, s_main_mods[sel].label, false);
}

/* ============ 按键 ============ */
static void p_main_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_LEFT:
        if (input_suppressed()) break;
        main_anim_to(ctx, mod_wrap(s_main.selected - 1));
        break;
    case OS_ACTION_RIGHT:
        if (input_suppressed()) break;
        main_anim_to(ctx, mod_wrap(s_main.selected + 1));
        break;
    case OS_ACTION_CONFIRM:
        if (input_suppressed()) break;
        os_push(ctx, s_main_mods[s_main.selected].page);
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        /* 桌面即根: 不动作 */
        break;
    default:
        break;
    }
}

/* ============ 触摸 ============ */
static bool p_main_touch(ui_ctx_t *ctx, int x, int y)
{
    /* 拖动松手后的点击抑制: 防止回弹瞬间误触进入页面 */
    if (input_suppressed()) return false;
    /* 命中检测: 点中某个图标 -> 选中并进入 */
    if (y < 0 || y >= UI_SCREEN_H) return false;
    int total = (int)MAIN_MOD_COUNT;
    int pinned = lroundf(s_main.cam_cur);
    const int big_half = 50, small_half = 32, gap = 2;
    for (int g = (int)floorf(s_main.cam_cur) - 3; g <= (int)ceilf(s_main.cam_cur) + 3; g++) {
        int idx = mod_wrap(g);
        int cx, size;
        if (g == pinned) {
            cx = UI_SCREEN_W / 2;   size = 100;
        } else {
            cx = (int)roundf((float)(UI_SCREEN_W / 2) + ((float)g - s_main.cam_cur) * 100.0f);
            size = 64;
            if (g == pinned + 1) {
                int minc = UI_SCREEN_W / 2 + big_half + small_half + gap;
                if (cx < minc) cx = minc;
            } else if (g == pinned - 1) {
                int maxc = UI_SCREEN_W / 2 - big_half - small_half - gap;
                if (cx > maxc) cx = maxc;
            }
        }
        if (x >= cx - size / 2 - 8 && x <= cx + size / 2 + 8 &&
            y >= UI_SCREEN_H / 2 - 50 && y <= UI_SCREEN_H / 2 + 70) {
            if (idx != s_main.selected) {
                main_anim_to(ctx, idx);   /* 先选中动画, 再进入 */
            }
            os_push(ctx, s_main_mods[idx].page);
            return true;
        }
    }
    return false;
}

/* ============ 每帧轮询: 拖动 ============ */
static void p_main_poll(ui_ctx_t *ctx)
{
    if (os_modal_active(ctx)) return;   /* 弹窗/覆盖期间禁止背景拖动 */

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (down && !s_main.drag_active) {
        s_main.drag_active   = true;
        s_main.drag_start_x  = tx;
        s_main.drag_offset   = 0;
        s_main.drag_moved    = 0;
        s_main.cam_base      = s_main.cam_cur;
        s_main.cam_anim_start = 0;
        ctx->needs_redraw = true;
    }
    if (s_main.drag_active) {
        if (down) {
            int off = tx - s_main.drag_start_x;
            if (off != s_main.drag_offset) {
                s_main.drag_offset = off;
                ctx->needs_redraw = true;
            }
            int m = off < 0 ? -off : off;
            if (m > s_main.drag_moved) s_main.drag_moved = m;
        } else {
            /* 松手: 吸附到最近格, 相机 from=当前位置 -> to=吸附位, 像素级衔接 */
            s_main.drag_active = false;
            s_main.suppress_until = now_ms() + 200;   /* 抑制松手 swipe 方向/确认 */
            const int spacing = 100;
            float cur = s_main.cam_base - (float)s_main.drag_offset / (float)spacing;
            int target = lroundf(cur);
            int steps = (int)(target - lroundf(s_main.cam_base));
            if (steps < 0) steps = -steps;
            uint32_t dur = 240 + (uint32_t)steps * 70;
            if (dur > 700) dur = 700;
            s_main.cam_anim_sel   = mod_wrap(target);
            s_main.cam_anim_from  = cur;
            s_main.cam_anim_to    = (float)target;
            s_main.cam_anim_start = now_ms();
            s_main.cam_anim_dur   = dur;
            s_main.selected       = mod_wrap(target);
            ctx->needs_redraw = true;
            ESP_LOGI("MAIN", "松手: cur=%.2f target=%d moved=%d", cur, target, s_main.drag_moved);
        }
    }
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_main = {
    .name      = "main",
    .page_id   = OS_PAGE_MAIN,
    .render    = p_main_render,
    .action    = p_main_action,
    .touch     = p_main_touch,
    .poll      = p_main_poll,
    .fullscreen = false,
};

void os_page_main_register(void) { os_register(&s_mod_main); }
