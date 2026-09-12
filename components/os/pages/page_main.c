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
#include "os_theme.h"
#include "ui_common.h"
#include "input.h"
#include "font_zh16.h"
#include "vibrator.h"   /* V1.4.x: 齿轮震动 (拖动/滑行跨格) + UI 震动 */
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 设置"字体大小"读取 (由设置页维护): 0=标准(24px) 1=偏小(16px), 用于主菜单选中标签 */
int settings_font_size(void);

/* ============ 应用来源: 统一走 os_app 注册表 ============
 * 内置应用在 os_pages.c 静态注册; 商店安装的 WASM 应用运行时注册后主菜单自动出现.
 * icon_idx = 主菜单 SF 图标索引 (见 icons_main.inc 的 main_icons[] 表).
 * 隐藏应用 (引擎/应用管理) 默认不显示, 赞助页 5 连点后显示. */
/* 可见应用缓存: os_app_visible_at()/os_app_visible_count() 每次都会读 NVS(flash 很慢,
 * ~几百µs/次). 主菜单渲染每帧要调用十几次 → 一帧几十次 NVS ≈ 80ms 卡顿.
 * 进入主菜单(on_enter)时一次性构建成纯数组, 渲染只做纯索引, 绝不再碰 NVS. */
static const os_app_t *s_apps_cache[64];
static int s_apps_n = 0;
static void main_rebuild_apps(void) {
    s_apps_n = 0;
    for (int i = 0; i < os_app_count() && s_apps_n < 64; i++) {
        const os_app_t *a = os_app_get(i);
        if (a && os_app_visible(a)) s_apps_cache[s_apps_n++] = a;
    }
}
static int main_count(void)          { return s_apps_n; }
static const os_app_t *main_app(int idx) { return s_apps_cache[idx]; }
#define MAIN_MOD_COUNT main_count()

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
    const os_theme_t *th = os_theme_get();
    int big = th->icon_center, sml = th->icon_normal;
    if (ad <= 0.5f) return big;
    if (ad >= 1.0f) return sml;
    float t = (ad - 0.5f) / 0.5f;
    float ease = t * t * (3.0f - 2.0f * t);
    return big - (int)((float)(big - sml) * ease);
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

/* ============ 选中标签缩放绘制 (按设置字号 16/18/20/22/24 从 font_zh 采样) ============ */
static int main_font_sz(void) { static const int t[5]={16,18,20,22,24}; int i=settings_font_size(); if(i<0||i>4)i=4; return t[i]; }
static int main_label_w(const char *s, int sz) {
    int w=0; for(const unsigned char *p=(const unsigned char*)s; *p;){
        if((*p&0xE0)==0xE0){ w+=sz; p+=3; } else { w+=sz*16/24; p++; } }
    return w;
}
static void main_draw_label(st7305_handle_t *l, int cx, int y, const char *s, int sz, bool inv) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    int w = main_label_w(s, sz);
    int x = cx - w/2; if (x < 0) x = 0; if (x + w > UI_SCREEN_W) x = UI_SCREEN_W - w;
    for (const unsigned char *p = (const unsigned char*)s; *p;) {
        if ((*p & 0xE0) == 0xE0) {                        /* 中文: 统一 font_zh(24) 采样缩到 sz (0.4 同款缩放, 22 档=24→22) */
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

/* ============ 渲染 ============ */
static void p_main_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    int total = (int)MAIN_MOD_COUNT;

    const os_theme_t *th = os_theme_get();

    /* 背景: 主题钩子或默认白底 */
    if (th->draw_bg) th->draw_bg(ctx, lcd);
    else             st7305_clear(lcd, th->bg_color);

    const int icon_spacing = th->icon_spacing;
    const int center_x = UI_SCREEN_W / 2;
    const int icon_center_y = th->icon_center_y;
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
            /* 惯性/归位: 抛物线减速 1-(1-t)^2 (速度线性衰减, 自然停, 无过冲);
             * 尾部速度线性走低 → 最后减速段滑行更远, "慢慢停"不突停.
             * (V1.4.x: 由三次方缓出 1-(1-t)^3 改为二次方 — 三次方速度按平方衰减,
             *  尾部掉速太快会急停; 二次方速度线性衰减, 甩动末段还能平滑滑过数格.)
             * 按键导航(anim_spring): 弹簧回弹. */
            float ease = s_main.anim_spring ? spring_ease(tt)
                       : (1.0f - (float)pow(1.0f - tt, 2.0f));   /* 甩动: 抛物线缓出 */
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
            if (th->draw_icon)
                th->draw_icon(ctx, lcd, main_app(idx)->icon_idx, cx, icon_center_y,
                              size, (idx == sel));
            else
                draw_main_icon_stretched(lcd, cx, icon_center_y, size, size,
                                         main_app(idx)->icon_idx);
        }
    }

    /* 选中图标下方短标签 (位置由主题指定): 按设置字号缩放绘制 */
    int label_y = icon_center_y + th->label_dy;
    if (label_y > UI_SCREEN_H - 28) label_y = UI_SCREEN_H - 28;
    if (th->draw_label)
        th->draw_label(ctx, lcd, main_app(sel)->label, center_x, label_y, true);
    else
        main_draw_label(lcd, center_x, label_y, main_app(sel)->label, main_font_sz(), false);
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
        /* 打开应用: 震动由 os_handle_action 的 CONFIRM 导航反馈统一触发
         * (tap_ui 18ms, UI震动开关), 此处不再震 → 避免"导航震+点击震"双击. */
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
            /* 点侧边小图标: 不播归位动画、不改相机, 直接打开对应页.
             * (原先 main_anim_to 会把该图标弹簧挪到中央再接 history, 返回后
             *  主菜单中央已换成所点项 → 用户感觉"位置被刷新/闪烁".)
             * V1.5.x: 点击应用程序 → 打开瞬间短震一下. */
            vibrator_click();
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

    /* V1.4.x: 齿轮震动 — 拖动或惯性滑行中每跨过一个居中图标震一次.
     * 覆盖松手后的 fling 动画段 (之前只震拖动段, 快速甩动滑行全程静默,
     * 且跳格只震一次 → 次数对不上图标数). 短促高强脉冲(10ms/100)出
     * "咔哒"手感; 25ms 防抖: 同帧跳多格/过快跨格时合并, 保持脉冲清晰不粘连.
     * V1.4.x: 动画尾段(剩余≤650ms)收尾 — 跨格震动间隔强制递增 100/200/300ms,
     * 模拟机械滚轮缓停: 图标滑过最后几格"哒…哒…哒"渐慢, 不突停. */
    {
        static int s_vib_grid_last = -1;
        static uint32_t s_vib_grid_ts = 0;
        static int s_tail_seq = 0;           /* 尾段已震次数 */
        static uint32_t s_tail_last = 0;     /* 尾段上一震时刻(实际或计划) */
        static uint32_t s_tail_pend = 0;     /* 延迟触发时刻 (0=无) */
        uint32_t now = now_ms();

        /* 拖动中不延迟 (跟格即震); 尾段延迟震动到点则触发 */
        if (s_main.drag_active) s_tail_pend = 0;
        if (s_tail_pend && now >= s_tail_pend) {
            s_tail_pend = 0;
            s_tail_last = now;
            vibrator_tap_ui(10, 100);
        }

        int grid = mod_wrap(lroundf(s_main.cam_cur));
        if (s_main.drag_active || s_main.cam_anim_start > 0) {
            if (grid != s_vib_grid_last) {
                bool tail = false;
                if (s_main.cam_anim_start > 0) {
                    uint32_t end = s_main.cam_anim_start + s_main.cam_anim_dur;
                    tail = (now + 650 >= end);      /* 动画尾段窗口 650ms */
                }
                if (tail) {
                    /* 收尾: 间隔自适应递增 — 取上一格实际跨格间隔 ×1.5
                     * (动画减速越明显间隔越大), 限制 90~300ms.
                     * 动画跨格太快(< 目标间隔) → 延迟到满间隔再震; 动画本身
                     * 已够慢(≥ 间隔) → 跟格即震. 效果: 从 ~100ms 平滑渐至
                     * ~300ms 的"哒…哒…哒"收尾, 完全随动画自然减速, 不机械. */
                    uint32_t prev_iv = now - s_vib_grid_ts;  /* 上一格→本格 */
                    if (prev_iv > 400) prev_iv = 400;        /* 保护: 别撑太长 */
                    uint32_t gap = prev_iv + prev_iv / 2;    /* ×1.5 递增 */
                    if (gap < 90)   gap = 90;                /* 下限: 别太密 */
                    if (gap > 300)  gap = 300;               /* 上限: 别拖沓 */
                    uint32_t want = s_tail_last + gap;
                    if (s_tail_seq == 0) {
                        s_tail_last = now;
                        vibrator_tap_ui(10, 100);
                    } else if (now >= want) {
                        s_tail_last = now;
                        vibrator_tap_ui(10, 100);
                    } else {
                        s_tail_pend = want;                  /* 延迟兜底 */
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
                 * 甩动用 main_cam_fling: 保持方向不回绕 (修"用力甩后瞬间后移").
                 * 2026-08-26: 甩动滑行时长加长 (上限 550→1000ms, 每格 50→90ms),
                 * 快速甩动"慢慢停"更跟手; 甩动格数(前面速度)保持不变.
                 * 2026-08-31 (V1.4.x): 每格 90→130ms, 上限 1000→1600ms —
                 * 配合抛物线缓出, 甩动末段减速滑行更长, 最后还能慢慢滑过 2-3 格. */
                s_main.anim_spring = false;
                int steps = (int)(target - lroundf(s_main.cam_base));
                if (steps < 0) steps = -steps;
                uint32_t dur = 320 + (uint32_t)steps * 130;   /* 甩动滑行: 每格 130ms, 慢慢滑停 */
                if (dur > 1600) dur = 1600;
                s_main.cam_cur = cur;
                main_cam_fling(cur, (float)target, dur);
                ctx->needs_redraw = true;
                ESP_LOGI("MAIN", "归位: cur=%.2f vel=%.2f span=%d n=%d extra=%d target=%d moved=%d",
                         cur, vel, vel_span, s_main.v_n, extra, target, s_main.drag_moved);
            }
        }
    }
}

/* 进入主菜单: 重建可见应用缓存 (应用管理/隐藏开关可能已变更; 渲染用纯索引免 NVS) */
static void p_main_enter(ui_ctx_t *ctx) {
    main_rebuild_apps();
    ctx->needs_redraw = true;
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_main = {
    .name      = "main",
    .page_id   = OS_PAGE_MAIN,
    .on_enter  = p_main_enter,
    .render    = p_main_render,
    .action    = p_main_action,
    .touch     = p_main_touch,
    .poll      = p_main_poll,
    .fullscreen = false,
};

void os_page_main_register(void) { os_register(&s_mod_main); }
