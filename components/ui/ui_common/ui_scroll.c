/**
 * ui_scroll.c — 全项目统一「跟手滚动」控制器（纯逻辑，不画内容）
 *
 * 物理参数与 components/system/os/kernel/os_dialog.c 的列表完全一致
 * （那套手感是 linit 验收过的"最流畅那档"），2026-10-24 提取成可复用组件。
 *
 * ★ 三种模式（LIST 行吸附 / PIXEL 像素自由 / PAGE 整页翻）× 四个开关
 *   （无惯性 / 无回弹 / 横向 / 循环），页面只调 ui_scroll_config() 选择，
 *   手感参数一律不开放 —— 发布版必须全项目一致。
 *
 * ⛔ 不要在这里加页面相关的东西（列表项怎么画是页面的事）。
 * ⛔ 页面 poll 里必须每帧调 ui_scroll_poll()，否则惯性/回弹不会走。
 */
#include "ui_scroll.h"
#include "st7305.h"
#include "esp_timer.h"

/* 触摸静默多久算"手指早就不在了"(ms)。组件级兜底: 页面切走/事件丢失时
 * pressed 会一直挂着 true, 下次进页面第一次拖动就会拿旧基准 ⇒ 弹回。 */
#define TOUCH_IDLE_MS  600

#define DRAG_THRESH    12      /* 认拖动的位移阈值（与商店 AM_DRAG_THRESH 一致） */
#define FLING_DECAY    0.86f   /* 惯性每帧衰减 */
#define FLING_MIN      0.4f    /* 惯性停止阈值 px/帧 */
#define FLING_MAX      60.0f   /* 单帧最大速度（防止一甩到底） */
#define BOUNCE_DECAY   0.8f    /* 越界余量每帧衰减 */
#define BOUNCE_MIN     2       /* 余量 <2px 归零 */

/* 主轴取值: 横向模式读 x，否则读 y */
static inline int axis_of(const ui_scroll_t *s, int x, int y)
{
    return (s->flags & UI_SCROLL_F_HORIZ) ? x : y;
}

/* 吸附步长: PAGE=整屏, LIST=行高, PIXEL=不吸附 */
static int snap_step(const ui_scroll_t *s)
{
    if (s->mode == UI_SCROLL_PAGE) return s->view_px > 1 ? s->view_px : 1;
    if (s->mode == UI_SCROLL_LIST) return s->row_px  > 1 ? s->row_px  : 1;
    return 1;
}

void ui_scroll_config(ui_scroll_t *s, ui_scroll_mode_t mode, uint8_t flags)
{
    if (!s) return;
    s->mode  = (uint8_t)mode;
    s->flags = flags;
    s->sub   = 0;
    s->fling = 0.0f;
    int m = ui_scroll_max(s);
    if (s->off > m) s->off = m;
    if (s->off < 0) s->off = 0;
}

int ui_scroll_max(const ui_scroll_t *s)
{
    if (!s) return 0;
    int m = s->content_px - s->view_px;
    return m < 0 ? 0 : m;
}

void ui_scroll_init(ui_scroll_t *s, int row_px, int view_px, int content_px)
{
    if (!s) return;
    s->row_px     = row_px  > 0 ? row_px  : 1;
    s->view_px    = view_px > 0 ? view_px : 1;
    s->content_px = content_px;
    s->anchor     = 0;
    s->base_off   = s->off;
    s->dragging   = false;
    s->pressed    = false;
    s->sub        = 0;
    s->fling      = 0.0f;
    s->last_axis  = 0;
    if (s->off < 0) s->off = 0;
    int m = ui_scroll_max(s);
    if (s->off > m) s->off = m;
}

void ui_scroll_set_content(ui_scroll_t *s, int content_px)
{
    if (!s) return;
    s->content_px = content_px;
    int m = ui_scroll_max(s);
    if (s->off > m) s->off = m;
    if (s->off < 0) s->off = 0;
}

void ui_scroll_to_top(ui_scroll_t *s)
{
    if (!s) return;
    s->off = 0;
    s->sub = 0;
    s->fling = 0.0f;
    s->dragging = false;
    s->pressed = false;
    s->anchor = s->last_axis = 0;
}

void ui_scroll_cancel(ui_scroll_t *s)
{
    if (!s) return;
    s->fling = 0.0f;
    s->sub = 0;
    s->dragging = false;
    s->pressed = false;
    s->anchor = s->last_axis = 0;
}

bool ui_scroll_dragging(const ui_scroll_t *s) { return s && s->dragging; }

bool ui_scroll_busy(const ui_scroll_t *s)
{
    if (!s) return false;
    return s->dragging || s->sub != 0 ||
           (s->fling > FLING_MIN || s->fling < -FLING_MIN);
}

int ui_scroll_offset(const ui_scroll_t *s)
{
    if (!s) return 0;
    return s->off + s->sub;
}

int ui_scroll_first_row(const ui_scroll_t *s)
{
    if (!s) return 0;
    int step = snap_step(s);
    if (step <= 0) step = 1;
    int o = ui_scroll_offset(s);
    return o > 0 ? (o / step) : 0;
}

bool ui_scroll_ensure_visible(ui_scroll_t *s, int item_y, int item_h)
{
    if (!s) return false;
    if (item_h < 1) item_h = 1;
    int m = ui_scroll_max(s);
    if (m <= 0) return false;
    int want = s->off;
    if (item_y < s->off) {
        want = item_y;
    } else if (item_y + item_h > s->off + s->view_px) {
        want = item_y + item_h - s->view_px;
    }
    if (want < 0) want = 0;
    if (want > m) want = m;
    if (want == s->off) return false;
    s->off = want;
    s->sub = 0;
    s->fling = 0.0f;
    return true;
}

bool ui_scroll_touch(ui_scroll_t *s, int y, bool down)
{
    return ui_scroll_touch_xy(s, 0, y, down);
}

bool ui_scroll_touch_xy(ui_scroll_t *s, int x, int y, bool down)
{
    if (!s) return false;
    s->last_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const int a = axis_of(s, x, y);

    /* ---- 松手 ---- */
    if (!down) {
        bool was = s->dragging;
        s->dragging  = false;
        s->pressed   = false;
        s->anchor    = 0;
        s->last_axis = 0;
        if (s->flags & UI_SCROLL_F_NO_FLING) s->fling = 0.0f;
        return was;            /* 交给 ui_scroll_poll() 走惯性/回弹/吸附 */
    }

    /* ---- 按下 ---- */
    if (!s->pressed) {
        s->pressed   = true;
        s->anchor    = a;
        s->base_off  = s->off;
        s->last_axis = a;
        s->dragging  = false;
        s->fling     = 0.0f;
        s->sub       = 0;
        return false;
    }

    /* ---- 移动 ---- */
    int moved = a - s->anchor;
    if (moved < 0) moved = -moved;
    if (!s->dragging && moved >= DRAG_THRESH) {
        s->dragging  = true;
        s->base_off  = s->off;      /* 从越过阈值这一刻重新起算, 避免前 12px 跳一下 */
        s->anchor    = a;
        s->last_axis = a;
    }
    if (!s->dragging) return false;

    /* 惯性速度: 主轴减小(手指上滑/左滑) ⇒ 内容前进 ⇒ off 增大 */
    int d = a - s->last_axis;
    s->last_axis = a;
    if (!(s->flags & UI_SCROLL_F_NO_FLING)) {
        s->fling = s->fling * 0.45f + (float)(-d) * 0.55f;
        if (s->fling >  FLING_MAX) s->fling =  FLING_MAX;
        if (s->fling < -FLING_MAX) s->fling = -FLING_MAX;
    }

    /* 跟手: 越界不硬夹零, 超出部分记进 sub 交给松手回弹 */
    int raw = s->base_off - (a - s->anchor);
    int m   = ui_scroll_max(s);
    int old_off = s->off, old_sub = s->sub;

    if (s->flags & UI_SCROLL_F_LOOP) {
        /* 循环: 位移直接绕回, 不留余量 */
        if (m > 0) {
            while (raw < 0)   raw += m;
            while (raw > m)   raw -= m;
        } else raw = 0;
        s->off = raw;
        s->sub = 0;
    } else if (s->flags & UI_SCROLL_F_NO_BOUNCE) {
        /* 硬边界: 直接夹紧, 拉不动 */
        s->off = raw < 0 ? 0 : (raw > m ? m : raw);
        s->sub = 0;
    } else {
        int lim = s->view_px / 2;   /* 最多拉出半屏 */
        if (raw < 0)      { s->off = 0; s->sub = raw; if (s->sub < -lim) s->sub = -lim; }
        else if (raw > m) { s->off = m; s->sub = raw - m; if (s->sub > lim) s->sub = lim; }
        else              { s->off = raw; s->sub = 0; }
    }
    return (s->off != old_off) || (s->sub != old_sub);
}

bool ui_scroll_poll(ui_scroll_t *s)
{
    if (!s) return false;

    /* ★★★ 2026-10-25 组件级兜底: 600ms 没收到任何触摸却还挂着 pressed
     *   ⇒ 手指早就不在了(页面被切走 / 事件丢失)。不清的话下次进页面第一次拖动
     *   会沿用旧基准点(base_off/anchor) ⇒ 一上手就弹回原位。
     *   放在这里而不是每个页面的 on_exit，一次修全部调用方。 */
    if (s->pressed && s->last_touch_ms &&
        (uint32_t)(esp_timer_get_time() / 1000) - s->last_touch_ms > TOUCH_IDLE_MS) {
        s->pressed = false;
        s->dragging = false;
        s->anchor = 0;
        s->fling = 0.0f;
    }

    if (s->dragging) return false;
    bool changed = false;

    /* ---- 1) 惯性滑行 ---- */
    if (s->fling > FLING_MIN || s->fling < -FLING_MIN) {
        int step = (int)s->fling;
        if (step == 0) step = (s->fling > 0) ? 1 : -1;
        int m  = ui_scroll_max(s);
        int ns = s->off + step;

        if ((s->flags & UI_SCROLL_F_LOOP) && m > 0) {
            while (ns < 0) ns += m;
            while (ns > m) ns -= m;
            s->off = ns;
        } else if (ns < 0) {
            if (s->flags & UI_SCROLL_F_NO_BOUNCE) { s->off = 0; s->sub = 0; }
            else                                  { s->sub += ns; s->off = 0; }
            s->fling = 0.0f;
        } else if (ns > m) {
            if (s->flags & UI_SCROLL_F_NO_BOUNCE) { s->off = m; s->sub = 0; }
            else                                  { s->sub += ns - m; s->off = m; }
            s->fling = 0.0f;
        } else {
            s->off = ns;
        }
        s->fling *= FLING_DECAY;
        if (s->fling < FLING_MIN && s->fling > -FLING_MIN) s->fling = 0.0f;
        changed = true;
    }

    /* ---- 2) 越界回弹 ---- */
    if (!(s->flags & UI_SCROLL_F_NO_BOUNCE) && s->fling == 0.0f && s->sub != 0) {
        s->sub = (int)((float)s->sub * BOUNCE_DECAY);
        if (s->sub > -BOUNCE_MIN && s->sub < BOUNCE_MIN) s->sub = 0;
        changed = true;
    }

    /* ---- 3) 吸附（LIST 行 / PAGE 页；PIXEL 不吸附）---- */
    int step_snap = snap_step(s);
    if (s->fling == 0.0f && s->sub == 0 && step_snap > 1) {
        int half = step_snap / 2;
        int snap = ((s->off + half) / step_snap) * step_snap;
        int m = ui_scroll_max(s);
        if (snap > m) snap = m;
        if (snap < 0) snap = 0;
        if (snap != s->off) { s->off = snap; changed = true; }
    }
    return changed;
}

/* 统一滚动条: 1px 主线 + 两端 6px 点阵渐隐（视觉铁律: 不画整条实线）。
 * 内容不足一屏 ⇒ 什么都不画（"没有滚动条"本身就是"这页不用滚"）。 */
void ui_scroll_drawbar(const ui_scroll_t *s, void *lcd, int a0, int a1, int a2)
{
    if (!s || !lcd) return;
    int m = ui_scroll_max(s);
    if (m <= 0) return;
    st7305_handle_t *h = (st7305_handle_t *)lcd;
    bool horiz = (s->flags & UI_SCROLL_F_HORIZ) != 0;

    int track = a2 - a1;
    if (track < 8) return;

    int thumb = track * s->view_px / s->content_px;
    if (thumb < 6) thumb = 6;
    if (thumb > track) thumb = track;
    int travel = track - thumb;
    int pos = travel * s->off / m;
    int t0 = a1 + pos;
    int t1 = t0 + thumb;

    if (horiz) {
        for (int x = t0; x < t1; x++) st7305_draw_pixel(h, x, a0, ST7305_COLOR_BLACK);
        for (int i = 1; i <= 6; i++) {
            if ((i % 2) == 0) {
                if (t0 - i >= a1) st7305_draw_pixel(h, t0 - i, a0, ST7305_COLOR_BLACK);
                if (t1 + i <= a2) st7305_draw_pixel(h, t1 + i, a0, ST7305_COLOR_BLACK);
            }
        }
    } else {
        for (int y = t0; y < t1; y++) st7305_draw_pixel(h, a0, y, ST7305_COLOR_BLACK);
        for (int i = 1; i <= 6; i++) {
            if ((i % 2) == 0) {
                if (t0 - i >= a1) st7305_draw_pixel(h, a0, t0 - i, ST7305_COLOR_BLACK);
                if (t1 + i <= a2) st7305_draw_pixel(h, a0, t1 + i, ST7305_COLOR_BLACK);
            }
        }
    }
}
