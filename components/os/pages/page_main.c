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

/* ============ 应用来源: 统一走 os_app 注册表 ============
 * 内置应用在 os_pages.c 静态注册; 商店安装的 WASM 应用运行时注册后主菜单自动出现.
 * icon_idx = 主菜单 SF 图标索引 (见 icons_main.inc 的 main_icons[] 表). */
#define MAIN_MOD_COUNT  os_app_count()
static const os_app_t *main_app(int idx) { return os_app_get(idx); }

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
    int      v_off[16];
    uint32_t v_t[16];
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
    s_main.anim_spring = true;                  /* 按键导航: 弹簧回弹 */
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
    ESP_LOGI("MAIN", "anim: from=%.2f to=%.2f sel=%d spring=%d dur=%u",
             from_c, to_c, to, s_main.anim_spring, (unsigned)dur_ms);
}

/* 惯性甩动动画: 保持甩动方向, 直接动画到原始相机位置 to_cam (可越界).
 * 不能用 main_cam_animate(最短回绕): 用力甩动 extra>=半圈时最短路径会反向,
 * 图标瞬间朝反方向移动 ("甩完后瞬间后移"). 渲染用 mod_wrap 处理越界相机. */
static void main_cam_fling(float from_cam, float to_cam, uint32_t dur_ms) {
    if (fabsf(to_cam - from_cam) < 0.05f) {
        s_main.cam_anim_start = 0;
        s_main.cam_cur = to_cam;
        s_main.selected = mod_wrap(lroundf(to_cam));
        return;
    }
    s_main.selected = mod_wrap(lroundf(to_cam));
    s_main.cam_anim_sel   = s_main.selected;
    s_main.cam_anim_from  = from_cam;
    s_main.cam_anim_to    = to_cam;
    s_main.cam_anim_start = now_ms();
    s_main.cam_anim_dur   = dur_ms ? dur_ms : 1;
    ESP_LOGI("MAIN", "fling: from=%.2f to=%.2f sel=%d dur=%u",
             from_cam, to_cam, s_main.selected, (unsigned)dur_ms);
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
            /* 惯性/归位: 抛物线减速 1-(1-t)^2 (速度线性衰减自然停);
             * 按键导航(anim_spring): 弹簧回弹. */
            float ease = s_main.anim_spring ? spring_ease(tt)
                       : (1.0f - (float)pow(1.0f - tt, 2.0f));
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
                                     main_app(idx)->icon_idx);
        }
    }

    /* 选中图标下方短标签 */
    int label_y = icon_center_y + 68;
    if (label_y > UI_SCREEN_H - 28) label_y = UI_SCREEN_H - 28;
    draw_label_centered_at(lcd, center_x, label_y, main_app(sel)->label, false);
}

/* ============ 按键 ============ */
static void p_main_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_LEFT:
        ESP_LOGI("MAIN", "act L suppressed=%d", input_suppressed());
        if (input_suppressed()) break;
        main_anim_to(ctx, mod_wrap(s_main.selected - 1));
        break;
    case OS_ACTION_RIGHT:
        ESP_LOGI("MAIN", "act R suppressed=%d", input_suppressed());
        if (input_suppressed()) break;
        main_anim_to(ctx, mod_wrap(s_main.selected + 1));
        break;
    case OS_ACTION_CONFIRM:
        if (input_suppressed()) break;
        os_push(ctx, main_app(s_main.selected)->page);
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
            os_push(ctx, main_app(idx)->page);
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
    /* 抑制窗口内禁止开启新拖拽: 用力甩动松手后手指反弹接触会被误认为新拖拽,
     * 把相机基点重置到动画中途 → 图标瞬间后跳 ("甩完后移"感). */
    if (down && !s_main.drag_active && now_ms() >= s_main.suppress_until) {
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
            /* 速度采样: 记录最近 16 个 (偏移,时间) */
            if (s_main.v_n < 16) {
                s_main.v_off[s_main.v_n] = off;
                s_main.v_t[s_main.v_n]   = now_ms();
                s_main.v_n++;
            } else {
                for (int i = 1; i < 16; i++) { s_main.v_off[i-1] = s_main.v_off[i]; s_main.v_t[i-1] = s_main.v_t[i]; }
                s_main.v_off[15] = off;
                s_main.v_t[15]   = now_ms();
            }
        } else {
            /* 松手: 释放速度 → 惯性 (抛物线减速); 缓慢滑动不甩直接归位.
             *   vel < 0.25   → 0 格 (直接归位, 不甩)
             *   vel 0.4-0.6  → 1-2 格 (用力一点)
             *   vel 0.9-1.2  → 5-7 格 (速度快, 甩得远) */
            s_main.drag_active = false;
            if (s_main.drag_moved >= 10) {
                s_main.suppress_until = now_ms() + 150;
                const int spacing = 100;
                float cur = s_main.cam_base - (float)s_main.drag_offset / (float)spacing;

                /* 释放速度 (px/ms): 取松手前 ~80ms 窗口的平均速度 (从最新样本往回找
                 * 第一个时间差>=80ms 的样本). 之前误用了整个 200ms 跨度 → 快甩峰值
                 * 被稀释. 80ms 窗口: 后面变慢→末速低(及时停), 快甩→末速高(甩得远). */
                float vel = 0.0f;
                int vel_span = 0;
                if (s_main.v_n >= 2) {
                    int last = s_main.v_n - 1;
                    int first = 0;
                    for (int i = last; i >= 1; i--) {
                        if ((int)(s_main.v_t[last] - s_main.v_t[i]) >= 80) { first = i; break; }
                    }
                    if (s_main.v_t[last] > s_main.v_t[first]) {
                        vel_span = (int)(s_main.v_t[last] - s_main.v_t[first]);
                        vel = (float)(s_main.v_off[last] - s_main.v_off[first]) /
                              (float)vel_span;
                    }
                }

                /* 速度→格数: 二次曲线 (低区缓增→用力甩 1-2 格, 高区陡增→快甩 5-7 格) */
                int sign = vel < 0 ? -1 : 1;
                float av = fabsf(vel);
                int extra = 0;
                if (av >= 0.25f) {
                    float d = av - 0.25f;
                    float slots = d * 4.0f + d * d * 6.0f;
                    extra = (int)lroundf(slots);
                    if (extra > 7) extra = 7;
                    extra *= sign;
                }
                int base_target = lroundf(cur);
                int target = base_target - extra;   /* 惯性方向与拖动一致 */

                /* 动画: 抛物线减速 1-(1-t)^2 (速度线性衰减, 自然停下, 无过冲).
                 * 缓慢归位同曲线 → 平滑回落到槽位, 不甩不弹.
                 * 甩动用 main_cam_fling: 保持方向不回绕 (修"用力甩后瞬间后移"). */
                s_main.anim_spring = false;
                int steps = (int)(target - lroundf(s_main.cam_base));
                if (steps < 0) steps = -steps;
                uint32_t dur = 220 + (uint32_t)steps * 50;
                if (dur > 550) dur = 550;
                s_main.cam_cur = cur;
                main_cam_fling(cur, (float)target, dur);
                ctx->needs_redraw = true;
                ESP_LOGI("MAIN", "归位: cur=%.2f vel=%.2f span=%d n=%d extra=%d target=%d moved=%d",
                         cur, vel, vel_span, s_main.v_n, extra, target, s_main.drag_moved);
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
