/**
 * r_carousel.c — 主题引擎 "carousel" (相机轮播桌面).
 *
 * 这是**从 page_main.c 原样搬出来**的现有桌面视觉与交互, 一个数都没改:
 *   - 白底 + 横向无限轮播: 中央图标 100, 两侧 64, 0.5~1.0 格之间平滑缩放
 *   - 相机 cam(单位:格, 可小数): 拖动跟手 / 甩动惯性(抛物线缓出) / 按键弹簧回弹
 *   - 选中项短标签画在图标下方
 * 五层里目前只用了 背景 / 图标区 / 文字层 三层, 装饰与底栏留给后续引擎/主题包。
 *
 * 数据与应用启动一律走 host (theme_renderer.h), 本文件不碰 os_app / NVS。
 */
#include "os.h"
#include "os_theme.h"
#include "theme_renderer.h"
#include "ui_common.h"
#include "font_zh16.h"
#include "input.h"
#include "vibrator.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 设置"字体大小"读取 (由设置页维护): 0=标准(24px) 1=偏小(16px) */
int settings_font_size(void);

#define HOST_COUNT() (os_theme_host() ? os_theme_host()->count() : 0)

/* ============ 引擎私有状态 (相机/拖动/动画/选中) ============ */
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
    int     selected;         /* 选中显示索引 */
} car_state_t;
static car_state_t s_car;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool input_suppressed(void) { return now_ms() < s_car.suppress_until; }

static int mod_wrap(int v) {
    int n = (int)HOST_COUNT();
    if (n <= 0) return 0;          /* 空桌面兜底: 原来 n=0 会触发 %0 (UB) */
    v %= n;
    if (v < 0) v += n;
    return v;
}

static void car_animate(int to_idx, uint32_t dur_ms);   /* 前向声明 */

/* 导航/触摸: 动画到目标索引 (wrap 最短路径, 时长随距离) */
static void car_anim_to(ui_ctx_t *ctx, int target) {
    int curi = lroundf(s_car.cam_cur);
    int dt = mod_wrap(target - curi);
    if (dt > (int)HOST_COUNT() / 2) dt -= (int)HOST_COUNT();
    int steps = dt < 0 ? -dt : dt;
    uint32_t dur = 300 + (uint32_t)steps * 90;   /* 慢速, 放大缩小动画清晰可见 */
    if (dur > 800) dur = 800;
    s_car.anim_spring = true;                    /* 按键导航: 弹簧回弹 */
    car_animate(target, dur);
    ctx->needs_redraw = true;
}

/* 图标尺寸: 其他图标固定 64px, 只在进入/离开中央位置(0.5~1.0 格)做缩放过渡. */
static int car_icon_size(float ad) {
    const os_theme_t *th = os_theme_get();
    int big = th->icon_center, sml = th->icon_normal;
    if (ad <= 0.5f) return big;
    if (ad >= 1.0f) return sml;
    float t = (ad - 0.5f) / 0.5f;
    float ease = t * t * (3.0f - 2.0f * t);
    return big - (int)((float)(big - sml) * ease);
}

/* 弹簧回弹缓动 (easeOutBack): 过冲约 8% 再回落到目标 */
static float spring_ease(float t) {
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

/* 相机归位/切换动画: to 取与 from 最近的同余位置 (wrap 走最短路径) */
static void car_animate(int to_idx, uint32_t dur_ms) {
    int total = (int)HOST_COUNT();
    if (total <= 0) return;
    int to = to_idx % total;
    if (to < 0) to += total;
    float from_c = s_car.cam_cur;
    float to_c = (float)to;
    float half = (float)total / 2.0f;
    while (to_c - from_c >  half) to_c -= (float)total;
    while (from_c - to_c >  half) to_c += (float)total;
    if (fabsf(to_c - from_c) < 0.05f) {
        s_car.selected = to;
        s_car.cam_anim_start = 0;
        s_car.cam_cur = to_c;
        return;
    }
    s_car.selected = to;
    s_car.cam_anim_sel   = to;
    s_car.cam_anim_from  = from_c;
    s_car.cam_anim_to    = to_c;
    s_car.cam_anim_start = now_ms();
    s_car.cam_anim_dur   = dur_ms ? dur_ms : 1;
    ESP_LOGI("MAIN", "anim: from=%.2f to=%.2f sel=%d spring=%d dur=%u",
             from_c, to_c, to, s_car.anim_spring, (unsigned)dur_ms);
}

/* 惯性甩动动画: 保持甩动方向, 直接动画到原始相机位置 to_cam (可越界) */
static void car_fling(float from_cam, float to_cam, uint32_t dur_ms) {
    if (fabsf(to_cam - from_cam) < 0.05f) {
        s_car.cam_anim_start = 0;
        s_car.cam_cur = to_cam;
        s_car.selected = mod_wrap(lroundf(to_cam));
        return;
    }
    s_car.selected = mod_wrap(lroundf(to_cam));
    s_car.cam_anim_sel   = s_car.selected;
    s_car.cam_anim_from  = from_cam;
    s_car.cam_anim_to    = to_cam;
    s_car.cam_anim_start = now_ms();
    s_car.cam_anim_dur   = dur_ms ? dur_ms : 1;
    ESP_LOGI("MAIN", "fling: from=%.2f to=%.2f sel=%d dur=%u",
             from_cam, to_cam, s_car.selected, (unsigned)dur_ms);
}

/* ============ 选中标签缩放绘制 (按设置字号 16/18/20/22/24 从 font_zh 采样) ============ */
static int car_font_sz(void) { static const int t[5]={16,18,20,22,24}; int i=settings_font_size(); if(i<0||i>4)i=4; return t[i]; }
static int car_label_w(const char *s, int sz) {
    int w=0; for(const unsigned char *p=(const unsigned char*)s; *p;){
        if((*p&0xE0)==0xE0){ w+=sz; p+=3; } else { w+=sz*16/24; p++; } }
    return w;
}
static void car_draw_label(st7305_handle_t *l, int cx, int y, const char *s, int sz, bool inv) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    int w = car_label_w(s, sz);
    int x = cx - w/2; if (x < 0) x = 0; if (x + w > UI_SCREEN_W) x = UI_SCREEN_W - w;
    for (const unsigned char *p = (const unsigned char*)s; *p;) {
        if ((*p & 0xE0) == 0xE0) {                        /* 中文: font_zh(24) 采样缩到 sz */
            int idx = font_zh_find_utf8((const char*)p);
            const uint8_t *base = (idx >= 0) ? zh_font_data[idx] : NULL;
            for (int dy=0; dy<sz; dy++) for (int dx=0; dx<sz; dx++) {
                int srow = dy*24/sz, scol = dx*24/sz;
                bool on = base && (base[srow*3 + (scol>>3)] & (0x80>>(scol&7)));
                st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
            }
            x += sz; p += 3;
        } else {                                          /* ASCII: font_zh ascii 同源采样; 兜底 FONT8X12 */
            int idx = (p[0]>=0x20 && p[0]<=0x7e) ? font_zh_find_ascii(p[0]) : -1;
            const uint8_t *za = (idx>=0) ? font_zh_get_bitmap_by_index(idx) : NULL;
            if (za) {
                int aw = sz*16/24;
                for (int dy=0; dy<sz; dy++) for (int dx=0; dx<aw; dx++) {
                    int srow = dy*24/sz, scol = dx*24/aw;
                    bool on = za[srow*3 + (scol>>3)] & (0x80>>(scol&7));
                    st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
                }
                x += aw; p++;
            } else {
                int aw = sz*8/12;
                bool has = (p[0]>=0x20 && p[0]<=0x7e);
                for (int dy=0; dy<sz; dy++) for (int dx=0; dx<aw; dx++) {
                    int srow = dy*12/sz, scol = dx*8/aw;
                    bool on = has && (FONT8X12[p[0]-0x20][srow] & (0x80>>(scol&7)));
                    st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
                }
                x += aw; p++;
            }
        }
    }
}

/* ============ 渲染 (第 1/3/4 层: 背景 / 图标区 / 文字层) ============ */
static void car_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    const os_theme_t *th = os_theme_get();

    /* 背景: 主题钩子或默认白底 */
    if (th->draw_bg) th->draw_bg(ctx, lcd);
    else             st7305_clear(lcd, th->bg_color);

    const int icon_spacing = th->icon_spacing;
    const int center_x = UI_SCREEN_W / 2;
    const int icon_center_y = th->icon_center_y;
    uint32_t now = now_ms();

    /* 相机: 拖动平滑跟手 (cam=基点-偏移/间距); 动画用弹簧回弹/抛物线缓出 */
    float cam;
    if (s_car.drag_active) {
        cam = s_car.cam_base - (float)s_car.drag_offset / (float)icon_spacing;
    } else if (s_car.cam_anim_start > 0) {
        if (s_car.selected != s_car.cam_anim_sel) {
            s_car.cam_anim_start = 0;
            cam = (float)s_car.selected;
        } else {
            uint32_t d = now - s_car.cam_anim_start;
            float tt = d < s_car.cam_anim_dur ? (float)d / (float)s_car.cam_anim_dur : 1.0f;
            float ease = s_car.anim_spring ? spring_ease(tt)
                       : (1.0f - (float)pow(1.0f - tt, 2.0f));   /* 甩动: 抛物线缓出 */
            cam = s_car.cam_anim_from +
                  (s_car.cam_anim_to - s_car.cam_anim_from) * ease;
            if (tt >= 1.0f) {
                s_car.cam_anim_start = 0;
                cam = s_car.cam_anim_to;
            }
        }
    } else {
        cam = (float)s_car.selected;
    }
    s_car.cam_cur = cam;

    int sel = mod_wrap(lroundf(cam));

    /* 遍历相机附件的整数格, 屏内即绘制 (索引取模回绕) */
    int g0 = (int)floorf(cam) - 3;
    int g1 = (int)ceilf(cam) + 3;
    for (int g = g0; g <= g1; g++) {
        int idx = mod_wrap(g);
        int cx = center_x + (int)roundf(((float)g - cam) * (float)icon_spacing);
        float dist = fabsf((float)g - cam);
        int size = car_icon_size(dist);
        if (cx + size > 0 && cx - size < UI_SCREEN_W) {
            if (th->draw_icon)
                th->draw_icon(ctx, lcd, os_theme_host()->icon_idx(idx), cx, icon_center_y,
                              size, (idx == sel));
            else
                draw_main_icon_stretched(lcd, cx, icon_center_y, size, size,
                                         os_theme_host()->icon_idx(idx));
        }
    }

    /* 选中图标下方短标签 (位置由主题指定): 按设置字号缩放绘制 */
    int label_y = icon_center_y + th->label_dy;
    if (label_y > UI_SCREEN_H - 28) label_y = UI_SCREEN_H - 28;
    if (th->draw_label)
        th->draw_label(ctx, lcd, os_theme_host()->label(sel), center_x, label_y, true);
    else
        car_draw_label(lcd, center_x, label_y, os_theme_host()->label(sel), car_font_sz(), false);
}

/* ============ 按键 ============ */
static bool car_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_LEFT:
        ESP_LOGI("MAIN", "act L suppressed=%d", input_suppressed());
        if (input_suppressed()) break;
        car_anim_to(ctx, mod_wrap(s_car.selected - 1));
        break;
    case OS_ACTION_RIGHT:
        ESP_LOGI("MAIN", "act R suppressed=%d", input_suppressed());
        if (input_suppressed()) break;
        car_anim_to(ctx, mod_wrap(s_car.selected + 1));
        break;
    case OS_ACTION_CONFIRM:
        if (input_suppressed()) break;
        os_theme_host()->launch(ctx, s_car.selected);
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        break;                       /* 桌面即根: 不动作 */
    default:
        break;
    }
    return true;                     /* 桌面自己处理全部动作 */
}

/* ============ 触摸 ============ */
static bool car_touch(ui_ctx_t *ctx, int x, int y)
{
    if (input_suppressed()) return false;
    /* 命中检测几何必须与 car_render 同源 (都从主题取) */
    const os_theme_t *th = os_theme_get();
    if (y < 0 || y >= UI_SCREEN_H) return false;
    const int hit_top    = th->icon_center_y - th->icon_center / 2;
    const int hit_bottom = th->icon_center_y + th->icon_center / 2 + 20;
    for (int g = (int)floorf(s_car.cam_cur) - 3; g <= (int)ceilf(s_car.cam_cur) + 3; g++) {
        int idx = mod_wrap(g);
        int cx = (int)roundf((float)(UI_SCREEN_W / 2) + ((float)g - s_car.cam_cur) * (float)th->icon_spacing);
        int size = car_icon_size(fabsf((float)g - s_car.cam_cur));
        if (x >= cx - size / 2 - 8 && x <= cx + size / 2 + 8 &&
            y >= hit_top && y <= hit_bottom) {
            os_theme_host()->launch(ctx, idx);   /* 点侧边小图标: 直接打开, 不改相机 */
            return true;
        }
    }
    return false;
}

/* ============ 每帧轮询: 拖动 / 惯性 / 跨格震动 ============ */
/* 齿轮震动状态 (原为 poll 内 static, 语义相同) */
static int      s_vib_grid_last = -1;
static uint32_t s_vib_grid_ts   = 0;
static int      s_tail_seq      = 0;   /* 尾段已震次数 */
static uint32_t s_tail_last     = 0;   /* 尾段上一震时刻 */
static uint32_t s_tail_pend     = 0;   /* 延迟触发时刻 (0=无) */

/* ★★★ 2026-10-25 僵尸拖动保护 (实测定位的"界面就几帧"根因)
 *   现场证据: 一次进出仙剑之后, 触摸芯片(I2C)读失败率从 0% 升到 30%,
 *   input_get_touch_pos() 长期返回 down=true 而且坐标不再变化 ⇒ 下面的
 *   s_car.drag_active 永远不结束, 第 391 行每帧 ctx->needs_redraw = true,
 *   于是每帧重绘**完全一样**的画面(两次 fbdump 的 CRC 恒为 92b26814),
 *   主菜单 draw 从 21ms 涨到 213ms — 用户看到的就是"所有界面只有几帧"。
 *   判据: 位移连续 800ms 一点没变 —— 人手拖动不可能这样, 按"触点状态残留"处理。
 *   (真正的长按走 touch_gesture_poll 的独立状态机, 不读这两个变量, 不受影响。) */
static int      s_drag_lock_off = 0;
static uint32_t s_drag_lock_ms  = 0;

static void car_poll(ui_ctx_t *ctx)
{
    /* 归位/切换动画进行中: 每帧请求重绘 */
    if (s_car.cam_anim_start > 0) ctx->needs_redraw = true;

    /* 齿轮震动 — 拖动或惯性滑行中每跨过一个居中图标震一次; 尾段间隔递增模拟滚轮缓停 */
    {
        uint32_t now = now_ms();
        if (s_car.drag_active) s_tail_pend = 0;
        if (s_tail_pend && now >= s_tail_pend) {
            s_tail_pend = 0;
            s_tail_last = now;
            vibrator_tap_ui(10, 100);
        }

        int grid = mod_wrap(lroundf(s_car.cam_cur));
        if (s_car.drag_active || s_car.cam_anim_start > 0) {
            if (grid != s_vib_grid_last) {
                bool tail = false;
                if (s_car.cam_anim_start > 0) {
                    uint32_t end = s_car.cam_anim_start + s_car.cam_anim_dur;
                    tail = (now + 650 >= end);      /* 动画尾段窗口 650ms */
                }
                if (tail) {
                    uint32_t prev_iv = now - s_vib_grid_ts;
                    if (prev_iv > 400) prev_iv = 400;
                    uint32_t gap = prev_iv + prev_iv / 2;
                    if (gap < 90)   gap = 90;
                    if (gap > 300)  gap = 300;
                    uint32_t want = s_tail_last + gap;
                    if (s_tail_seq == 0) {
                        s_tail_last = now;
                        vibrator_tap_ui(10, 100);
                    } else if (now >= want) {
                        s_tail_last = now;
                        vibrator_tap_ui(10, 100);
                    } else {
                        s_tail_pend = want;
                    }
                    s_tail_seq++;
                    s_vib_grid_ts = now;
                } else {
                    s_tail_seq = 0;
                    if (now - s_vib_grid_ts >= 25) {
                        s_vib_grid_ts = now;
                        vibrator_tap_ui(10, 100);
                    }
                }
                s_vib_grid_last = grid;
            }
        } else {
            s_vib_grid_last = grid;   /* 静止: 同步当前格, 避免下次拖动首格误震 */
            s_tail_seq = 0;
            s_tail_pend = 0;
        }
    }

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    /* 抑制窗口内禁止开启新拖拽 (甩动松手后手指回弹会被误认新拖拽) */
    if (down && !s_car.drag_active && now_ms() >= s_car.suppress_until) {
        s_car.drag_active   = true;
        s_car.drag_start_x  = tx;
        s_car.drag_offset   = 0;
        s_car.drag_moved    = 0;
        s_car.cam_base      = s_car.cam_cur;
        s_car.cam_anim_start = 0;
        s_car.v_n = 0;
        ctx->needs_redraw = true;
    }
    if (s_car.drag_active) {
        ctx->needs_redraw = true;
        if (down) {
            int off = tx - s_car.drag_start_x;
            s_car.drag_offset = off;
            int m = off < 0 ? -off : off;
            if (m > s_car.drag_moved) s_car.drag_moved = m;
            /* ★ 僵尸拖动看门狗 (见文件头 static 变量处的说明) */
            uint32_t nowp = now_ms();
            if (off != s_drag_lock_off) {
                s_drag_lock_off = off;
                s_drag_lock_ms  = nowp;
            } else if (s_drag_lock_ms && (uint32_t)(nowp - s_drag_lock_ms) > 800u) {
                s_car.drag_active    = false;
                s_car.drag_moved     = 0;
                s_car.suppress_until = nowp + 200;   /* 防同一残留触点立刻重启拖动 */
                s_drag_lock_ms       = 0;
                ESP_LOGW("MAIN", "拖动位移 800ms 未变 ⇒ 触摸触点残留, 结束拖动");
                return;                              /* 本帧不再采样速度 */
            }
            if (s_car.v_n < 16) {
                s_car.v_off[s_car.v_n] = off;
                s_car.v_t[s_car.v_n]   = now_ms();
                s_car.v_n++;
            } else {
                for (int i = 1; i < 16; i++) { s_car.v_off[i-1] = s_car.v_off[i]; s_car.v_t[i-1] = s_car.v_t[i]; }
                s_car.v_off[15] = off;
                s_car.v_t[15]   = now_ms();
            }
        } else {
            /* 松手: 释放速度 → 惯性 (抛物线减速) */
            s_car.drag_active = false;
            if (s_car.drag_moved >= 10) {
                s_car.suppress_until = now_ms() + 150;
                const int spacing = os_theme_get()->icon_spacing;
                float cur = s_car.cam_base - (float)s_car.drag_offset / (float)spacing;

                /* 释放速度 (px/ms): 松手前 ~80ms 窗口平均速度 */
                float vel = 0.0f;
                int vel_span = 0;
                if (s_car.v_n >= 2) {
                    int last = s_car.v_n - 1;
                    int first = 0;
                    for (int i = last; i >= 1; i--) {
                        if ((int)(s_car.v_t[last] - s_car.v_t[i]) >= 80) { first = i; break; }
                    }
                    if (s_car.v_t[last] > s_car.v_t[first]) {
                        vel_span = (int)(s_car.v_t[last] - s_car.v_t[first]);
                        vel = (float)(s_car.v_off[last] - s_car.v_off[first]) /
                              (float)vel_span;
                    }
                }

                /* 速度→格数: 二次曲线 */
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

                s_car.anim_spring = false;
                int steps = (int)(target - lroundf(s_car.cam_base));
                if (steps < 0) steps = -steps;
                uint32_t dur = 320 + (uint32_t)steps * 130;   /* 每格 130ms */
                if (dur > 1600) dur = 1600;
                s_car.cam_cur = cur;
                car_fling(cur, (float)target, dur);
                ctx->needs_redraw = true;
                ESP_LOGI("MAIN", "归位: cur=%.2f vel=%.2f span=%d n=%d extra=%d target=%d moved=%d",
                         cur, vel, vel_span, s_car.v_n, extra, target, s_car.drag_moved);
            }
        }
    }
}

/* ★ 2026-10-25: 进主菜单时清掉上一条命留下的拖动/动画状态。
 *   从游戏/终端这类"会吃掉触摸"的页面回来时, drag_active 可能还是 true,
 *   那样一进桌面就每帧置脏空转(见 car_poll 的僵尸拖动说明)。
 *   进页面时不可能正处于拖动中, 所以这里无条件复位是安全的。 */
static void car_enter(ui_ctx_t *ctx)
{
    s_car.drag_active    = false;
    s_car.drag_offset    = 0;
    s_car.drag_moved     = 0;
    s_car.cam_anim_start = 0;
    s_car.v_n            = 0;
    s_drag_lock_ms       = 0;
    ctx->needs_redraw    = true;
}

/* ============ 注册 ============ */
const theme_renderer_t r_carousel = {
    .id     = "carousel",
    .enter  = car_enter,
    .exit   = NULL,
    .render = car_render,
    .poll   = car_poll,
    .action = car_action,
    .touch  = car_touch,
};
