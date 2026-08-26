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
    bool    anim_spring;      /* 本次动画用弹簧回弹 (false=惯性减速平滑停) */
    /* 惯性速度采样: 最近 N 个 (偏移, 时间) 对 */
    int      v_off[5];
    uint32_t v_t[5];
    int      v_n;
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

static void main_cam_animate(int to_idx, uint32_t dur_ms);   /* 前向声明 (定义见下) */

/* 导航/触摸: 动画到目标索引 (wrap 最短路径, 时长随距离) */
static void main_anim_to(ui_ctx_t *ctx, int target) {
    int curi = lroundf(s_main.cam_cur);
    int dt = mod_wrap(target - curi);
    if (dt > (int)MAIN_MOD_COUNT / 2) dt -= (int)MAIN_MOD_COUNT;
    int steps = dt < 0 ? -dt : dt;
    uint32_t dur = 300 + (uint32_t)steps * 90;   /* 慢速, 放大缩小动画清晰可见 */
    if (dur > 800) dur = 800;
    main_cam_animate(target, dur);
    ctx->needs_redraw = true;
}

/* 图标尺寸: 其他图标固定 64px, 只在进入/离开中央位置(0.5~1.0 格)做缩放过渡.
 * 静止时仅正中间 100, 其余严格 64. */
static int main_icon_size(float ad) {
    if (ad <= 0.5f) return 100;
    if (ad >= 1.0f) return 64;
    float t = (ad - 0.5f) / 0.5f;
    float ease = t * t * (3.0f - 2.0f * t);
    return 100 - (int)(36.0f * ease);
}

/* 弹簧回弹缓动 (easeOutBack): 过冲约 8% 再回落到目标, 让归位吸附有明显"弹回"感 */
static float spring_ease(float t) {
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

/* 相机归位/切换动画 (参照原 menu_main_cam_animate 语义):
 * from = 当前相机(可小数), to_idx = 目标索引.
 * to 取与 from 最近的同余位置 (wrap 走最短路径); 几乎未移动则直接定档. */
static void main_cam_animate(int to_idx, uint32_t dur_ms) {
    int total = (int)MAIN_MOD_COUNT;
    int to = to_idx % total;
    if (to < 0) to += total;
    float from_c = s_main.cam_cur;
    float to_c = (float)to;
    float half = (float)total / 2.0f;
    while (to_c - from_c >  half) to_c -= (float)total;
    while (from_c - to_c >  half) to_c += (float)total;
    if (fabsf(to_c - from_c) < 0.05f) {
        s_main.selected = to;
        s_main.cam_anim_start = 0;
        s_main.cam_cur = to_c;
        return;
    }
    s_main.selected = to;
    s_main.cam_anim_sel   = to;
    s_main.cam_anim_from  = from_c;
    s_main.cam_anim_to    = to_c;
    s_main.cam_anim_start = now_ms();
    s_main.cam_anim_dur   = dur_ms ? dur_ms : 1;
}

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

    /* 相机: 拖动平滑跟手 (cam=基点-偏移/间距, 有过渡感);
     * 动画(松手归位/切换)用弹簧回弹缓动, 明显弹回; 静止=选中槽位. */
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
            /* 惯性甩动: 缓动 1-(1-t)^2.2 (前面快、后面利落收尾, 不拖沓);
             * 短距离归位: 弹簧回弹. */
            float ease = s_main.anim_spring ? spring_ease(tt)
                       : (1.0f - (float)pow(1.0f - tt, 2.2f));
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

    /* 遍历相机附件的整数格, 屏内即绘制 (索引取模回绕).
     * 图标连续滑动; 尺寸: 中央 100, 其余 64 (进入/离开时平滑缩放). */
    int g0 = (int)floorf(cam) - 3;
    int g1 = (int)ceilf(cam) + 3;
    for (int g = g0; g <= g1; g++) {
        int idx = mod_wrap(g);
        int cx = center_x + (int)roundf(((float)g - cam) * (float)icon_spacing);
        float dist = fabsf((float)g - cam);
        int size = main_icon_size(dist);
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
    for (int g = (int)floorf(s_main.cam_cur) - 3; g <= (int)ceilf(s_main.cam_cur) + 3; g++) {
        int idx = mod_wrap(g);
        int cx = (int)roundf((float)(UI_SCREEN_W / 2) + ((float)g - s_main.cam_cur) * 100.0f);
        int size = main_icon_size(fabsf((float)g - s_main.cam_cur));
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

/* ============ 每帧轮询: 拖动 (平滑跟手) ============ */
static void p_main_poll(ui_ctx_t *ctx)
{
    if (os_modal_active(ctx)) return;   /* 弹窗/覆盖期间禁止背景拖动 */

    /* 归位/切换动画进行中: 每帧请求重绘 (os 仅在 needs_redraw 时渲染) */
    if (s_main.cam_anim_start > 0) ctx->needs_redraw = true;

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (down && !s_main.drag_active) {
        s_main.drag_active   = true;
        s_main.drag_start_x  = tx;
        s_main.drag_offset   = 0;
        s_main.drag_moved    = 0;
        s_main.cam_base      = s_main.cam_cur;
        s_main.cam_anim_start = 0;
        s_main.v_n = 0;                       /* 惯性采样清零 */
        ctx->needs_redraw = true;
    }
    if (s_main.drag_active) {
        /* 拖动中: 每帧请求重绘, 图标平滑跟手 (渲染用 cam=基点-偏移/间距) */
        ctx->needs_redraw = true;
        if (down) {
            int off = tx - s_main.drag_start_x;
            s_main.drag_offset = off;
            int m = off < 0 ? -off : off;
            if (m > s_main.drag_moved) s_main.drag_moved = m;
            /* 速度采样: 记录最近 5 个 (偏移,时间) */
            if (s_main.v_n < 5) {
                s_main.v_off[s_main.v_n] = off;
                s_main.v_t[s_main.v_n]   = now_ms();
                s_main.v_n++;
            } else {
                for (int i = 1; i < 5; i++) { s_main.v_off[i-1] = s_main.v_off[i]; s_main.v_t[i-1] = s_main.v_t[i]; }
                s_main.v_off[4] = off;
                s_main.v_t[4]   = now_ms();
            }
        } else {
            /* 松手: 归位吸附到最近槽位, 并叠加惯性 (苹果式: 前面快后面降速).
             * 释放速度越快 → 滑得越远 (快速甩动 5-7 格); 慢速就近停. */
            s_main.drag_active = false;
            if (s_main.drag_moved >= 40) {
                s_main.suppress_until = now_ms() + 150;
                const int spacing = 100;
                float cur = s_main.cam_base - (float)s_main.drag_offset / (float)spacing;

                /* 释放速度 (px/ms): 取最近两个"偏移不同"的采样, 排除触摸未更新的
                 * 同值样本, 避免被拖尾稀释 → 快甩能测到真实末速. */
                float vel = 0.0f;
                if (s_main.v_n >= 2) {
                    int last = s_main.v_n - 1;
                    while (last > 0) {
                        int j = last - 1;
                        while (j > 0 && s_main.v_t[last] == s_main.v_t[j]) j--;
                        if (s_main.v_off[last] != s_main.v_off[j] &&
                            s_main.v_t[last] > s_main.v_t[j]) {
                            vel = (float)(s_main.v_off[last] - s_main.v_off[j]) /
                                  (float)(s_main.v_t[last] - s_main.v_t[j]);
                            break;
                        }
                        last = j;
                    }
                }
                /* 苹果式惯性: 减速时间常数 ~800ms, 释放速度→滑行距离 (不设距离上限,
                 * 由速度与封顶 7 格决定; 阈值 0.15px/ms: 特别慢才不甩). */
                int extra = 0;
                if (fabsf(vel) >= 0.15f) {
                    float glide_px = vel * 800.0f;              /* 速度→滑行距离 */
                    extra = (int)lroundf(glide_px / (float)spacing);
                    if (extra > 7) extra = 7; else if (extra < -7) extra = -7;
                }
                int base_target = lroundf(cur);
                int target = base_target - extra;              /* 惯性方向与拖动一致 */

                /* 惯性甩动: 平滑减速停 (利落收尾); 无惯性(extra=0): 弹簧回弹归位 */
                s_main.anim_spring = (extra == 0);
                int steps = (int)(target - lroundf(s_main.cam_base));
                if (steps < 0) steps = -steps;
                uint32_t dur = 220 + (uint32_t)steps * 70;      /* 甩得远稍久, 但收尾快 */
                if (dur > 650) dur = 650;
                s_main.cam_cur = cur;                          /* 动画起点 = 松手位置 */
                main_cam_animate(target, dur);
                ctx->needs_redraw = true;
                ESP_LOGI("MAIN", "归位: cur=%.2f vel=%.2f extra=%d target=%d moved=%d",
                         cur, vel, extra, target, s_main.drag_moved);
            }
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
