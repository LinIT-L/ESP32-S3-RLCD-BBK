/**
 * os_dialog.c — 通用弹窗 (modal 覆盖页) 内核模块 + 弹窗栈.
 *
 * 供所有页面复用统一弹窗: 列表选择 / 确认框 / 提示 toast.
 * 以 OS_PAGE_DIALOG 压栈 (modal), 不退出底层页面; 关闭后自动恢复底层.
 * 内部维护弹窗栈 (settings/list_dialog 嵌套): 开子弹窗压栈, 返回弹栈恢复父弹窗.
 * 复杂弹窗 (时间编辑等) 通过 os_dlg_stack 的 on_render/on_key 自定义.
 * 私有 state 全部 static 留本文件.
 *
 * 使用示例:
 *   os_dialog_list(ctx, "键鼠布局", items, n, sel, on_sel, NULL);
 *   os_dialog_confirm(ctx, "确定退出?", on_res, NULL);
 *   os_dialog_toast(ctx, "已收藏");
 *   os_dialog_push(ctx, &dlg);   [子弹窗 - 栈内嵌套]
 *   os_dialog_pop(ctx);          [返回父弹窗]
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "bt_manager.h"   /* os_confirm_exit_blocking: 手柄退出确认直读电平 */
#include "esp_timer.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

/* 行右侧删除"×"长按即时检测用到: 取手势按下起点 (显式声明, 复用 menu 的约定) */
bool input_touch_start_pos(int *x, int *y);

#define TAG "DLG"

#define DLG_MAX_DEPTH 4
#define DLG_MAX_ITEMS 60   /* 支持数值选择等长列表 */
#define DLG_ITEM_LEN  30

/* 弹窗布局 — 严格复刻 3.3 版 list_dialog:
 * 固定尺寸, 离屏幕上下左右各 25px → 350×250; 行高 40; 无标题;
 * 列表: 2px 黑边框 / 选中行黑底白字 / 底部固定"返回"行+分隔线 / 右侧滚动条;
 * 确认框: 3px 黑边框 / 标题(消息)在上 / 分隔线 / 确定·取消两行. */
#define DLG_MARGIN  25
#define DLG_LINE_H  40
#define DLG_W       (ui_screen_w() - DLG_MARGIN * 2)   /* 350 (横屏) / 250 (竖屏 300 宽) */
#define DLG_H       (ui_screen_h() - DLG_MARGIN * 2)   /* 250 (横屏) / 350 (竖屏 400 高) */

static int dlg_content_count(const os_dlg_stack_t *d);   /* 前向声明 (dlg_geom 用) */

/* 几何 (模板分离):
 *  列表模板: 固定 350×250, 行高40, 底部固定返回行, 右侧滚动条
 *  大确认模板: 350×250, 3px 边框, 标题+分隔线+按钮行 (关机/删除/格式化等)
 *  小确认模板 (small): 更小 (3.2/更早版 退出确认样式):
 *      消息居中在上, 最下方一行 左右两侧 = 确定/取消 并排 */
static void dlg_geom(const os_dlg_stack_t *d, int *x, int *y, int *w, int *h) {
    if (d->no_footer && d->small) {
        int tw = text_width(d->title);
        if (d->icon) {
            /* 图标确认窗 (仅图标, 无标题): 上留10 + 图标 + 间距12 + 按钮区40.
             * h = 62 + 图标高; 宽默认 fix_w(见下), 否则随图标自适应. */
            int ih = d->icon_h > 0 ? d->icon_h : 0;
            int iw = d->icon_w > 0 ? d->icon_w : 0;
            *h = 62 + ih;
            *w = 140;
            if (iw + 24 > *w) *w = iw + 24;   /* 图标左右各留 12px */
        } else {
            /* 固定小窗: 宽随标题文字自适应 (最小 140), 高 74.
             * 标题/横杠/确定取消 之间全 5px 间距, 竖线与文字随宽度对称居中. */
            *w = 140;
            if (tw + 24 > *w) *w = tw + 24;   /* 左右各留 12px */
            *h = 74;
        }
        if (*w > ui_screen_w() - 40) *w = ui_screen_w() - 40;   /* 极限: 屏宽-40 */
        /* fix_w>0: 强制统一宽度 (确认框→图标窗原位变形保持同框同宽) */
        if (d->fix_w > 0) *w = d->fix_w;
        if (*w > ui_screen_w() - 40) *w = ui_screen_w() - 40;
    } else {
        *w = DLG_W;
        *h = DLG_H;
        /* 列表自适应高度版: 内容行少自动收窄(保留右下返回行), 多则保持满高滚动 */
        if (d->auto_h && !d->no_footer) {
            const int inner = 4;
            int c = dlg_content_count(d);
            int avail = DLG_H - inner * 2 - DLG_LINE_H;      /* 除底部返回行外的可视高度 */
            if (c > 0) {
                int need = c * DLG_LINE_H;
                if (need <= avail) *h = inner * 2 + DLG_LINE_H + need;
                /* need>avail → 保持满高, 走滚动 */
            }
        }
    }
    *x = (ui_screen_w() - *w) / 2;
    *y = (ui_screen_h() - *h) / 2;
}

/* 统一布局计算 (渲染/触摸/滚动共用, 保证三者几何完全一致).
 * 确认框: 标题(消息) 在 y+14, 分隔线 y+38, 按钮行区从 y+44 起垂直居中;
 * 列表  : 内容区 y+4 .. 返回分隔线上方 垂直居中, 底部固定"返回"行. */
typedef struct {
    int x, y, w, h;
    int footer_y;        /* 底部"返回"行顶 (无则 -1) */
    int content_y0;      /* 内容区首行顶 y */
    int clip_top;        /* 内容区可视上缘 (含滚动偏移) */
    int clip_bottom;     /* 内容区可视下缘 (含滚动偏移) */
    int visible;         /* 可见行数 */
    int scroll;          /* 当前滚动偏移 (选中窗口首行) */
    int px;              /* 跟手子像素偏移 (内容相对选中窗口平移, [-DLG_LINE_H, DLG_LINE_H)) */
    int content_count;   /* 内容项数 (不含返回行) */
} dlg_layout_t;

static void dlg_layout(const os_dlg_stack_t *d, dlg_layout_t *L) {
    const int inner_pad = 4;
    dlg_geom(d, &L->x, &L->y, &L->w, &L->h);
    L->content_count = dlg_content_count(d);
    bool has_footer = !d->no_footer && d->count > 0;
    L->footer_y = has_footer ? (L->y + L->h - inner_pad - DLG_LINE_H) : -1;
    int top, bottom;
    if (d->no_footer) {
        top    = L->y + 40;                       /* 消息 y+4..38 + 分隔线 y+38 之下 */
        bottom = L->y + L->h - 4;
    } else {
        top    = L->y + 4;
        bottom = has_footer ? (L->footer_y - 2) : (L->y + L->h - 4);
    }
    int area_h = bottom - top;
    int total_h = L->content_count * DLG_LINE_H;
    int top_pad = (area_h - total_h) / 2;
    if (top_pad < 0) top_pad = 0;
    L->content_y0 = top + top_pad;
    L->clip_top = top;
    L->clip_bottom = bottom;
    L->visible = area_h / DLG_LINE_H;
    if (L->visible < 1) L->visible = 1;
    L->scroll = 0;
    if (d->sel >= L->visible && d->sel < L->content_count)
        L->scroll = d->sel - L->visible + 1;
    /* 跟手子像素偏移: 只对可滚动列表生效; 顶/底边界处夹到 0 */
    L->px = d->scroll_px;
    if (L->content_count <= L->visible) L->px = 0;
    if ((d->sel == 0 && L->px > 0) || (d->sel >= L->content_count - 1 && L->px < 0))
        L->px = 0;
    if (L->px < -DLG_LINE_H + 1) L->px = -DLG_LINE_H + 1;
    if (L->px > DLG_LINE_H - 1)  L->px = DLG_LINE_H - 1;
}

/* 当前弹窗可视行数 (拖动跟手判定: 内容超一屏才滚动) */
static int dlg_visible_rows(const os_dlg_stack_t *d) {
    dlg_layout_t L;
    dlg_layout(d, &L);
    return L.visible;
}

/* 弹窗栈 (每层一份完整状态, 弹栈时恢复父层). 放 PSRAM, 不占内部 SRAM. */
EXT_RAM_BSS_ATTR static os_dlg_stack_t s_stack[DLG_MAX_DEPTH];
static int s_top = -1;

/* toast (不占弹窗栈: 瞬时提示) */
static char     s_toast_msg[72];   /* 72B=24 个汉字(UTF-8 3B/字), 避免长中文提示被截到汉字中间成乱码 */
static uint32_t s_toast_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static os_dlg_stack_t *top(void) { return (s_top >= 0) ? &s_stack[s_top] : NULL; }

/* 提示弹窗显示时长: 默认满 1 秒自动消失 (用户规范); os_dialog_toast_ms 可指定自定义时长. */
#define TOAST_MS 1000
static uint32_t s_toast_dur_ms = TOAST_MS;   /* V1.5.x: 当前 toast 显示时长 (ms) */
/* 是否需要显示 toast 提示 (有弹窗时也叠在弹窗之上显示, 到时自动消失) */
static bool toast_active(void) {
    return (s_toast_msg[0] != 0) && (now_ms() - s_toast_ms < s_toast_dur_ms);
}

/* ============ 渲染 (不清屏, 覆盖在底层之上) ============ */
/* 内容项数: list 模式若调用方已把"返回"作为最后一项, 该行降级为底部固定返回行 */
static int dlg_content_count(const os_dlg_stack_t *d) {
    int c = d->count;
    if (!d->no_footer && c > 0 &&
        strcmp(d->items[c - 1], "\xe8\xbf\x94\xe5\x9b\x9e") == 0)   /* 返回 */
        c--;
    return c;
}
static const char *dlg_ret_text(const os_dlg_stack_t *d) {
    if (d->footer[0]) return d->footer;   /* 自定义底部标签 (文件浏览: 返回/退出) */
    if (!d->no_footer && d->count > 0 &&
        strcmp(d->items[d->count - 1], "\xe8\xbf\x94\xe5\x9b\x9e") == 0)
        return d->items[d->count - 1];
    /* 深度自适应: 首个(根)列表底部显示"退出", 更深子列表显示"返回上一页" */
    if (s_top <= 0) return "\xe9\x80\x80\xe5\x87\xba";   /* 退出 */
    return "\xe8\xbf\x94\xe5\x9b\x9e";                    /* 返回 */
}

/* 画一行 (3.3 样式): 选中=黑底白字居中; 未选=白底黑字居中 (强制白底防残留) */
static void dlg_draw_row(st7305_handle_t *lcd, const dlg_layout_t *L,
                         int abs_y, const char *text, bool selected) {
    int x0 = L->x + 6, x1 = L->x + L->w - 6;
    const char *t = text;
    if (text_width(text) > x1 - x0) t = text_clip(text, x1 - x0);   /* 超长只显示前20英文/10中文 */
    int top = abs_y + (DLG_LINE_H - 24) / 2;   /* 单行 24px 字垂直居中 */
    if (selected) {
        fill_rect(lcd, x0, abs_y, x1 - 1, abs_y + DLG_LINE_H - 2, ST7305_COLOR_BLACK);
        draw_text_centered(lcd, top, t, true);
    } else {
        fill_rect(lcd, x0, abs_y, x1 - 1, abs_y + DLG_LINE_H - 2, ST7305_COLOR_WHITE);
        draw_text_centered(lcd, top, t, false);
    }
}

/* 居中 toast 提示框 (3.3 notice 样式: 3px 黑边框, 自适应宽, 高 36) */
static void dlg_draw_toast(st7305_handle_t *lcd) {
    int tw = text_width(s_toast_msg);
    int w = 3 * 2 + 3 * 2 + tw;
    if (w > ui_screen_w() - 40) w = ui_screen_w() - 40;
    const int h = 36;
    int x0 = (ui_screen_w() - w) / 2;
    int y0 = (ui_screen_h() - h) / 2;
    fill_rect(lcd, x0, y0, x0 + w - 1, y0 + h - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 3; k++) {
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, x0 + k, x0 + w - 1 - k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, x0 + w - 1 - k, y0 + k, y0 + h - 1 - k, ST7305_COLOR_BLACK);
    }
    draw_text_centered(lcd, y0 + 6, s_toast_msg, false);
}

static void dlg_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    /* 独立 toast (无弹窗栈): 整页只画提示框 */
    if (toast_active() && s_top < 0) {
        dlg_draw_toast(lcd);
        return;
    }

    os_dlg_stack_t *d = top();
    if (!d) return;

    /* 自定义渲染: 若消费则不再画默认列表 */
    if (d->on_render && d->on_render(ctx, d, d->ud)) return;

    /* ==== 3.3 固定尺寸弹窗 ==== */
    /* 弹窗不清屏: 仅绘制弹窗本身, 弹窗之外保留原背景 (白板画布/主菜单等).
     * 弹窗区域内由下方 fill_rect 白底覆盖, 保证内容干净. */
    dlg_layout_t L;
    dlg_layout(d, &L);
    bool is_confirm = d->no_footer;
    /* V1.6: 两套输入方案 — 触屏优先. 触屏下"选择"用点哪算哪, 不默认高亮黑块;
     * 手柄下才默认高亮一项并用方向键. 统一查 input_scheme_touch() 保证全项目一致. */
    bool touch_scheme = input_scheme_touch();

    if (is_confirm && d->small && d->icon) {
        /* 图标确认窗 (仅图标, 无标题): 图标居中于上部, 底部按钮区.
         * 几何与 dlg_geom/touch 完全一致: 图标顶 L.y+10; 按钮区 btn_top/btn_bot;
         * 单按钮(items[1]空)居中, 双按钮左右对称. */
        int icon_top = L.y + 10;
        int btn_top  = L.y + 22 + (d->icon_h > 0 ? d->icon_h : 0);
        int btn_bot  = L.y + L.h - 6;
        /* 弹窗白底 + 2px 黑边框 */
        fill_rect(lcd, L.x, L.y, L.x + L.w - 1, L.y + L.h - 1, ST7305_COLOR_WHITE);
        for (int k = 0; k < 2; k++) {
            draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + k, ST7305_COLOR_BLACK);
            draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, L.x + k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, L.x + L.w - 1 - k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
        }
        /* 居中位图 (挂载图标等) */
        if (d->icon_w > 0 && d->icon_h > 0)
            st7305_draw_bitmap_1bit(lcd, L.x + (L.w - d->icon_w) / 2, icon_top,
                                    d->icon_w, d->icon_h, d->icon);
        /* 底部按钮区: 上方 2px 分隔线 + 按钮, 24px 黑字垂直居中.
         * 单按钮(items[1]空)居中, 双按钮左右对称. */
        draw_hline(lcd, L.x,     L.x + L.w - 1, btn_top,     ST7305_COLOR_BLACK);
        draw_hline(lcd, L.x,     L.x + L.w - 1, btn_top + 1, ST7305_COLOR_BLACK);
        int cy = btn_top + 2 + ((btn_bot - btn_top - 2) - 24) / 2;
        const char *lbl = d->items[0][0] ? d->items[0] : "\xe9\x80\x80\xe5\x87\xba\xe6\x8c\x82\xe8\xbd\xbd"; /* 退出挂载 */
        if (d->items[1][0]) {
            int mid = L.x + L.w / 2;
            draw_vline(lcd, mid,     btn_top + 2, btn_bot, ST7305_COLOR_BLACK);
            draw_vline(lcd, mid + 1, btn_top + 2, btn_bot, ST7305_COLOR_BLACK);
            int cx0 = (L.x + 2 + mid - 1) / 2;
            int cx1 = (mid + 2 + L.x + L.w - 3) / 2;
            draw_text(lcd, cx0 - text_width(d->items[0]) / 2, cy, d->items[0], false);
            draw_text(lcd, cx1 - text_width(d->items[1]) / 2, cy, d->items[1], false);
        } else {
            draw_text(lcd, L.x + L.w / 2 - text_width(lbl) / 2, cy, lbl, false);
        }
        if (toast_active()) dlg_draw_toast(lcd);
        return;
    }

    /* ==== 小确认模板: 固定 140×74, 标题在上, 按钮区白底黑字(无选中高亮).
     * 几何: 边框2px; 标题 y+7(24px, 距上边框5px); 横杠 y+36/+37 连接左右边框
     * (标题距横杠5px); 按钮白底区 左右各留2px(不盖边框), 下留5px;
     * 正中竖线 x=L.x+70 从横杠到底边框; 确定/取消 24px @ y+43 (距横杠5px),
     * 左右两半对称居中. ==== */
    if (is_confirm && d->small) {
        /* 弹窗区域白底 (背景只在弹窗之外透出) */
        fill_rect(lcd, L.x, L.y, L.x + L.w - 1, L.y + L.h - 1, ST7305_COLOR_WHITE);
        for (int k = 0; k < 2; k++) {
            draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + k, ST7305_COLOR_BLACK);
            draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, L.x + k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, L.x + L.w - 1 - k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
        }
        /* 消息居中在上 (距上边框内沿 5px) */
        draw_text_centered(lcd, L.y + 7, d->title, false);
        /* 按钮区: 横杠/竖线连接到弹窗边框 (横杠连左右, 竖线连横杠与底边框),
         * 确定/取消 对称居中于左右两半 */
        int bx0 = L.x + 2, bx1 = L.x + L.w - 1 - 2;   /* 按钮白底区: 不覆盖左右边框 */
        int bot = L.y + L.h - 1 - 2;                  /* 竖线到底边框内 2px */
        int by  = L.y + 36;                           /* 横杠 y (按钮区上缘, 标题距横杠5px) */
        int mid = L.x + L.w / 2;                      /* 正中分割竖线 x (2px) */
        const char *s0 = d->items[0][0] ? d->items[0] : "\xe7\xa1\xae\xe5\xae\x9a"; /* 确定 */
        const char *s1 = d->items[1][0] ? d->items[1] : "\xe5\x8f\x96\xe6\xb6\x88"; /* 取消 */
        /* 按钮区白底/黑底 (选中侧=d->sel 加黑, 手柄 LEFT/RIGHT 切换选择) */
        fill_rect(lcd, bx0, by, mid - 1, bot, (touch_scheme || d->sel != 0) ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
        fill_rect(lcd, mid + 1, by, bx1, bot, (touch_scheme || d->sel != 1) ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
        /* 顶部横线: 左右连接到弹窗边框 (L.x .. L.x+w-1) */
        draw_hline(lcd, L.x,     L.x + L.w - 1, by,     ST7305_COLOR_BLACK);
        draw_hline(lcd, L.x,     L.x + L.w - 1, by + 1, ST7305_COLOR_BLACK);
        /* 中间竖线: 从横杠到弹窗底边框 */
        draw_vline(lcd, mid,     by, bot, ST7305_COLOR_BLACK);
        draw_vline(lcd, mid + 1, by, bot, ST7305_COLOR_BLACK);
        /* 文字各占半边居中 (选中侧白字, 未选黑字) */
        int ty = by + 7;
        int cx0 = (bx0 + mid - 1) / 2;
        int cx1 = (mid + 2 + bx1) / 2;
        draw_text(lcd, cx0 - text_width(s0) / 2, ty, s0, (!touch_scheme && d->sel == 0));
        draw_text(lcd, cx1 - text_width(s1) / 2, ty, s1, (!touch_scheme && d->sel == 1));
        if (toast_active()) dlg_draw_toast(lcd);
        return;
    }

    /* 弹窗整体白底覆盖底层 */
    fill_rect(lcd, L.x, L.y, L.x + L.w - 1, L.y + L.h - 1, ST7305_COLOR_WHITE);
    /* 边框: 列表 2px / 确认框 3px */
    int border = is_confirm ? 3 : 2;
    for (int k = 0; k < border; k++) {
        draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, L.x + k, L.x + L.w - 1 - k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, L.x + k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, L.x + L.w - 1 - k, L.y + k, L.y + L.h - 1 - k, ST7305_COLOR_BLACK);
    }

    /* 确认模板 (3.3): 标题(消息) 居中于 y+14, 分隔线 y+38, 下方按钮行 */
    if (is_confirm && d->title[0]) {
        draw_text_centered(lcd, L.y + 14, d->title, false);
        draw_hline(lcd, L.x + 10, L.x + L.w - 10, L.y + 38, ST7305_COLOR_BLACK);
    }

    /* 内容行 (确认框=确定/取消; 列表=各项).
     * 跟手拖动: 用 L.px 平移内容, 并多画上下各 1 行做部分出界显示 (按可视区裁剪). */
    for (int i = -1; i <= L.visible; i++) {
        int idx = i + L.scroll;
        if (idx < 0 || idx >= L.content_count) continue;
        int row_y = L.content_y0 + i * DLG_LINE_H + L.px;
        /* 裁剪: 行完全在可视区外(上下)则跳过; 部分出界由 dlg_draw_row 自己不越界即可 */
        if (row_y + DLG_LINE_H <= L.clip_top || row_y >= L.clip_bottom) continue;
        dlg_draw_row(lcd, &L, row_y, d->items[idx], (!touch_scheme && idx == d->sel));
        /* 行最右侧删除"×" (row_x): 与行反相 */
        if (d->row_x) {
            bool sel = (!touch_scheme && idx == d->sel);
            int ox = L.x + L.w - 6 - 16;
            int oy = row_y + (DLG_LINE_H - 24) / 2;
            draw_text(lcd, ox, oy, "X", sel);
        }
    }

    /* 列表: 底部固定"返回"行 + 上方分隔线 (仅内容多于返回行时画分隔线) */
    if (!is_confirm && L.footer_y >= 0 && d->count > 0) {
        if (L.content_count > 0)
            draw_hline(lcd, L.x + 6, L.x + L.w - 6, L.footer_y - 1, ST7305_COLOR_BLACK);
        dlg_draw_row(lcd, &L, L.footer_y, dlg_ret_text(d), (!touch_scheme && d->sel == L.content_count));
    }

    /* 滚动条: 内容超出可见区时右侧竖条 + 滑块 */
    if (L.content_count > L.visible) {
        int bar_x = L.x + L.w - 3;
        int bar_y0 = L.content_y0;
        int bar_y1 = L.content_y0 + L.visible * DLG_LINE_H - 1;
        draw_vline(lcd, bar_x, bar_y0, bar_y1, ST7305_COLOR_BLACK);
        int track_h = bar_y1 - bar_y0 + 1;
        int thumb_h = (track_h * L.visible) / L.content_count;
        if (thumb_h < 4) thumb_h = 4;
        int max_scroll = L.content_count - L.visible;
        int thumb_y = bar_y0 + (max_scroll > 0 ? (track_h - thumb_h) * L.scroll / max_scroll : 0);
        if (thumb_y < bar_y0) thumb_y = bar_y0;
        if (thumb_y + thumb_h > bar_y1 + 1) thumb_y = bar_y1 + 1 - thumb_h;
        draw_vline(lcd, bar_x - 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, bar_x + 1, thumb_y, thumb_y + thumb_h - 1, ST7305_COLOR_BLACK);
        for (int ty = thumb_y; ty < thumb_y + thumb_h; ty++)
            st7305_draw_pixel(lcd, bar_x, ty, ST7305_COLOR_BLACK);
    }

    /* toast 叠在弹窗之上 (居中提示) */
    if (toast_active()) dlg_draw_toast(lcd);
}

/* ============ 动作 ============ */
/* cb 内是否已"接管"弹窗 (push 下一步 / replace 刷新 / toast 提示).
 * 接管后 dlg_finish_top 不再自动关闭当前层, 保持"同一窗口换内容/回原位". */
static bool s_cb_handled = false;

static void dlg_finish_top(ui_ctx_t *ctx, int result) {
    os_dlg_stack_t *d = top();
    if (!d) return;
    os_dialog_cb_t cb = d->cb;
    void *ud = d->ud;
    s_cb_handled = false;
    if (cb) cb(ctx, result, ud);
    /* cb 未接管 (未 push/replace/toast) → 关闭当前层回上层/关弹窗页 */
    if (!s_cb_handled) os_dialog_pop(ctx);
}

static void dlg_action(ui_ctx_t *ctx, os_action_t a) {
    if (toast_active()) {
        /* 有 toast: 任何按键/点击先关掉提示; 仅独立 toast (无弹窗) 才弹层 */
        if (s_top < 0) os_pop(ctx);
        s_toast_msg[0] = 0;
        return;
    }
    os_dlg_stack_t *d = top();
    if (!d) return;
    /* 统一置脏: 任何弹窗按键都触发重绘. 修复"自定义 on_key 消费 CONFIRM/打开子弹窗
     * 却没置脏 → 画面不刷新, 需再按一次方向键才显示"的共性缺漏 (触摸路径先置脏故正常). */
    ctx->needs_redraw = true;
    /* 触摸长按: 先用长按位置命中弹窗行, 再交给 on_key (供"长按删除"等) */
    if (a == OS_ACTION_LONG_PRESS) {
        int tx, ty;
        if (input_consume_tap(&tx, &ty)) {
            dlg_layout_t L;
            dlg_layout(d, &L);
            int rel = ty - (L.content_y0 + L.px);
            if (rel >= 0) {
                int idx = rel / DLG_LINE_H + L.scroll;
                if (idx >= 0 && idx < L.content_count) { d->sel = idx; ctx->needs_redraw = true; }
            }
        }
    }
    /* 自定义按键: 消费则不再走默认 */
    if (d->on_key && d->on_key(ctx, d, a, d->ud)) return;
    int content_count = dlg_content_count(d);   /* 不含底部返回行 */
    int max_sel = d->no_footer ? (d->count - 1) : content_count;   /* list 含底部返回行 */
    switch (a) {
    case OS_ACTION_UP:
        if (d->sel > 0) d->sel--;
        d->scroll_px = 0;   /* 按键导航吸附回正 */
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (d->sel < max_sel) d->sel++;
        d->scroll_px = 0;   /* 按键导航吸附回正 */
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        /* 物理键激活也置脏 (与触摸选中一致): 立即请求重绘, 否则界面要到下次输入才刷新 */
        ctx->needs_redraw = true;
        if (!d->no_footer && d->sel == content_count) dlg_finish_top(ctx, -1);   /* 返回行 → 取消 */
        else dlg_finish_top(ctx, d->sel);
        break;
    case OS_ACTION_BACK:
        ctx->needs_redraw = true;
        dlg_finish_top(ctx, -1);
        break;
    case OS_ACTION_LEFT:
    case OS_ACTION_RIGHT:
        /* 小确认框: 手柄 LEFT/RIGHT 在 确定/取消 (或三按钮) 之间切换黑块选择,
         * CONFIRM=选中项, BACK=取消. 列表/其它弹窗左右键忽略. */
        if (d->no_footer && d->small && d->count >= 2) {
            int n = (d->count == 2) ? 2 : 3;
            if (a == OS_ACTION_LEFT) d->sel = (d->sel + n - 1) % n;
            else d->sel = (d->sel + 1) % n;
            ctx->needs_redraw = true;
        }
        break;
    default:
        break;
    }
}

static bool dlg_touch(ui_ctx_t *ctx, int x, int y) {
    if (toast_active()) {
        if (s_top < 0) os_pop(ctx);
        s_toast_msg[0] = 0;
        return true;
    }
    os_dlg_stack_t *d = top();
    if (!d) return false;
    dlg_layout_t L;
    dlg_layout(d, &L);
    if (x < L.x || x > L.x + L.w - 1) return false;

    /* Icon 小确认窗按钮区 (渲染对齐: 单按钮=整区0; 双按钮=左右对称, 命中即回传 0/1) */
    if (d->no_footer && d->small && d->icon) {
        int btn_top = L.y + 22 + (d->icon_h > 0 ? d->icon_h : 0);
        int btn_bot = L.y + L.h - 6;
        if (y >= btn_top && y <= btn_bot) {
            if (d->items[2][0]) {
                /* 三按钮: 三等分 左=0 中=1 右=2 */
                int x0 = L.x + 2, x1 = L.x + L.w - 3;
                int w3 = (x1 - x0 + 1) / 3;
                if (x < x0 + w3) d->sel = 0;
                else if (x < x0 + 2 * w3) d->sel = 1;
                else d->sel = 2;
            } else if (d->items[1][0])
                d->sel = (x < L.x + L.w / 2) ? 0 : 1;   /* 双按钮: 左=idx0 右=idx1 */
            else
                d->sel = 0;
            dlg_finish_top(ctx, d->sel);
            return true;
        }
        return true;   /* 弹窗内其他区域忽略 */
    }

    /* 小确认模板: 按钮区 (渲染对齐: 左右留2, 下留2, 顶 y+36, 正中竖线) */
    if (d->no_footer && d->small) {
        int bx0 = L.x + 2, bx1 = L.x + L.w - 1 - 2;
        int by = L.y + 36, bot = L.y + L.h - 1 - 2;
        int mid = L.x + L.w / 2;
        if (y >= by && y <= bot && x >= bx0 && x <= bx1) {
            d->sel = (x < mid) ? 0 : 1;   /* 左=确定 右=取消 */
            dlg_finish_top(ctx, d->sel);
            return true;
        }
        return true;   /* 弹窗内其他区域忽略 */
    }

    /* 自定义整层渲染对话框 (如滚轮/时间/休眠等, 含 count 手柄项): 
     * 触摸由 on_touch 全权处理整个弹窗框 (含上部相邻值, 点列/点按钮).
     * V1.3: 原条件要求 count==0, 导致滚轮(count>0)触摸落入下方通用列表几何,
     * 点选与渲染错位. 凡带 on_render+on_touch 的自定义弹窗一律优先派发 on_touch. */
    if (d->on_render && d->on_touch)
        return d->on_touch(ctx, d, x, y, d->ud);

    /* 底部"返回"行 */
    if (!d->no_footer && y >= L.footer_y && y < L.footer_y + DLG_LINE_H) {
        dlg_finish_top(ctx, -1);
        return true;
    }
    /* 内容区: 与渲染共用 dlg_layout, 几何完全一致 (含确认框消息区) */
    int rel = y - (L.content_y0 + L.px);
    if (rel < 0) return false;
    int idx = rel / DLG_LINE_H + L.scroll;
    if (idx >= 0 && idx < L.content_count) {
        d->sel = idx;
        ctx->needs_redraw = true;
        /* 行右侧删除"×": 点击直接命中该行并投递 LONG_PRESS (弹删除确认), 不真正选中/打开 */
        if (d->row_x && x >= L.x + L.w - 6 - 16 &&
            x <= L.x + L.w - 6) {
            if (d->on_key && d->on_key(ctx, d, OS_ACTION_LONG_PRESS, d->ud)) return true;
            return true;
        }
        /* 与按键 CONFIRM 同语义: 先过 on_key (如设置主菜单: 开子弹窗不关闭),
         * 被消费则不 dlg_finish_top; 否则按默认完成 (选中项 / 底部返回行=取消) */
        if (d->on_key && d->on_key(ctx, d, OS_ACTION_CONFIRM, d->ud)) return true;
        dlg_finish_top(ctx, d->sel);
        return true;
    }
    /* 自定义整层触摸(非列表弹窗: 如滚轮 点列/确认/取消按钮), 弹窗框内转交回调 */
    if (d->on_touch) return d->on_touch(ctx, d, x, y, d->ud);
    return false;
}

static void dlg_poll(ui_ctx_t *ctx) {
    /* 空 DIALOG 页自愈: 独立 toast 已消失 (超时/被任意输入清除) 但页面未退出时自动回
     * 底层 — 防止"提示没了却卡在空白弹窗页, 按键/触摸全无效" (触摸路径只清 toast 不弹页) */
    if (s_top < 0 && !toast_active()) {
        os_pop(ctx);
        return;
    }
    /* toast 超时: 到时后自动关闭 (回到弹出提示前的位置).
     * 注意不能用 toast_active() 判断 (它要求 < 时长, 与超时判断矛盾) */
    if (s_toast_msg[0] && now_ms() - s_toast_ms >= s_toast_dur_ms) {
        if (s_top < 0) os_pop(ctx);   /* 独立 toast: 关弹窗页; 叠弹窗上的提示仅清消息 */
        s_toast_msg[0] = 0;
        ctx->needs_redraw = true;
    }
    /* 弹窗超时: 打开后 timeout_ms 毫秒未操作, 自动以 timeout_result 完成 */
    os_dlg_stack_t *d = top();
    if (d && d->timeout_ms > 0 && now_ms() - d->open_ms >= d->timeout_ms) {
        dlg_finish_top(ctx, d->timeout_result);
        d = top();
    }
    /* 当前层自定义轮询 (按键映射捕获等) */
    if (d && d->on_poll) d->on_poll(ctx, d, d->ud);

    /* 行右侧删除"×"长按即时弹出 (按住期间即触发, 不须松手): 命中内容行 → 投递 LONG_PRESS */
    static bool s_rowx_fired = false;
    if (d && d->row_x) {
        uint32_t hold = input_touch_hold_ms();
        if (hold == 0) {
            s_rowx_fired = false;   /* 手指已松, 允许下次再触发 */
        } else if (!s_rowx_fired && hold >= 800) {
            int sx, sy;
            if (input_touch_start_pos(&sx, &sy)) {
                dlg_layout_t Lx;
                dlg_layout(d, &Lx);
                int rel = sy - (Lx.content_y0 + Lx.px);
                if (rel >= 0) {
                    int idx = rel / DLG_LINE_H + Lx.scroll;
                    if (idx >= 0 && idx < dlg_content_count(d)) {
                        /* 命中的是非删除×区域 (×区点击由 dlg_touch 单独处理) */
                        s_rowx_fired = true;
                        d->sel = idx;
                        ctx->needs_redraw = true;
                        if (d->on_key) d->on_key(ctx, d, OS_ACTION_LONG_PRESS, d->ud);
                    }
                }
            }
        }
    }

    /* 触摸拖动滚动 (跟手): 手指上滑内容跟着上滑, 下滑内容跟着下滑.
     * 通过子像素偏移 scroll_px 平移渲染内容; 平移满 1 行高则切换选中行并回退偏移,
     * 保证平滑连续且选中项随拖动手高 '跟手'. */
    static int s_drag_last_y = -1;
    static bool s_drag_active = false;
    int tx, ty;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_drag_last_y < 0) { s_drag_last_y = ty; s_drag_active = false; }
        else {
            int dy = ty - s_drag_last_y;
            s_drag_last_y = ty;
            os_dlg_stack_t *dd = top();
            if (dd && !dd->no_footer && dlg_content_count(dd) > dlg_visible_rows(dd)) {
                s_drag_active = true;   /* 内容超一屏才算拖动 (短路列表点选不受影响) */
                int max_sel = dlg_content_count(dd);   /* 含底部返回行 */
                dd->scroll_px += dy;                    /* 内容跟随手指平移 */
                bool moved = false;
                /* 平移满 1 行高: 切换到下一/上一行, 并把偏移回收到 (-LINE_H, LINE_H) */
                while (dd->scroll_px <= -DLG_LINE_H && dd->sel < max_sel) {
                    dd->sel++; dd->scroll_px += DLG_LINE_H; moved = true;
                }
                while (dd->scroll_px >= DLG_LINE_H && dd->sel > 0) {
                    dd->sel--; dd->scroll_px -= DLG_LINE_H; moved = true;
                }
                /* 顶/底边界: 超出部分抵消并在原地显示边界行 */
                if (dd->sel <= 0 && dd->scroll_px > 0) dd->scroll_px = 0;
                if (dd->sel >= max_sel && dd->scroll_px < 0) dd->scroll_px = 0;
                if (moved || dy != 0) ctx->needs_redraw = true;
            }
        }
    } else {
        s_drag_last_y = -1;
        if (s_drag_active) {
            s_drag_active = false;
            os_dlg_stack_t *dd = top();
            if (dd) {
                dd->scroll_px = 0;   /* 松手吸附: 内容回正到选中窗口 */
                ctx->needs_redraw = true;
            }
        }
    }
}

static const os_module_t s_mod_dialog = {
    .name       = "dialog",
    .page_id    = OS_PAGE_DIALOG,
    .render     = dlg_render,
    .action     = dlg_action,
    .touch      = dlg_touch,
    .poll       = dlg_poll,
    .modal      = true,
};

void os_page_dialog_register(void) { os_register(&s_mod_dialog); }

/* ============ 弹窗栈 API ============ */
void os_dialog_push(ui_ctx_t *ctx, const os_dlg_stack_t *dlg) {
    if (!dlg) return;
    if (s_top + 1 >= DLG_MAX_DEPTH) return;   /* 栈满: 拒绝 */
    if (s_top < 0) os_push(ctx, OS_PAGE_DIALOG);   /* 首层: 打开弹窗页 */
    s_stack[++s_top] = *dlg;                       /* 整体拷贝, 含回调/ud/自定义 */
    s_stack[s_top].open_ms = now_ms();             /* 记录打开时刻 (供超时) */
    s_cb_handled = true;   /* cb 内调用=下一步同窗替换, 不自动关闭父层 */
}

/* 覆盖当前层内容 (刷新/下一步, 不加深层级) 已由调用方改为 pop+push 组合实现, 此 API 删除. */
void os_dialog_clear_all(ui_ctx_t *ctx) {
    while (s_top >= 0) os_dialog_pop(ctx);
    s_toast_msg[0] = 0;
    ctx->needs_redraw = true;
}

void os_dialog_pop(ui_ctx_t *ctx) {
    if (s_top > 0) {
        s_top--;   /* 弹栈, 恢复父弹窗 (sel 已在各自层保存) */
    } else if (s_top == 0) {
        s_top = -1;
        os_pop(ctx);   /* 关闭弹窗页, 恢复底层 */
    }
    s_cb_handled = true;   /* cb 内显式弹栈=已接管, 避免 dlg_finish_top 再自动关 */
    ctx->needs_redraw = true;
}

int os_dialog_depth(void) { return s_top + 1; }
const os_dlg_stack_t *os_dialog_top(void) { return top(); }

/* 原位改写当前弹窗 (同一框变形): 返回栈顶可写句柄. 供"确认框 → 图标窗"驻留同一弹窗使用. */
os_dlg_stack_t *os_dialog_top_mut(ui_ctx_t *ctx) {
    ctx->needs_redraw = true;
    return top();
}

/* 标记回调已"接管"弹窗 (保持同一窗, dlg_finish_top 不再自动关闭). */
void os_dialog_mark_handled(ui_ctx_t *ctx) {
    s_cb_handled = true;
    ctx->needs_redraw = true;
}

/* ============ 旧 API (单层快捷方式, 复用弹窗栈) ============ */
void os_dialog_list(ui_ctx_t *ctx, const char *title,
                    const char *const *items, int count, int sel,
                    os_dialog_cb_t cb, void *ud) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", title ? title : "");
    dlg.count = count < DLG_MAX_ITEMS ? count : DLG_MAX_ITEMS;
    if (dlg.count < 0) dlg.count = 0;
    for (int i = 0; i < dlg.count; i++)
        snprintf(dlg.items[i], DLG_ITEM_LEN, "%s", items[i]);
    dlg.sel = sel < 0 ? 0 : sel;
    if (dlg.sel >= dlg.count) dlg.sel = 0;
    dlg.cb = cb;
    dlg.ud = ud;
    os_dialog_push(ctx, &dlg);
}

void os_dialog_confirm_ex(ui_ctx_t *ctx, const char *msg, uint32_t timeout_ms,
                          int timeout_result, os_dialog_cb_t cb, void *ud) {
    /* 小自适应确认模板 (3.3 退出 notice 样式): 退出程序?/关机? 等 */
    const char *items[2] = { "\xe7\xa1\xae\xe5\xae\x9a", "\xe5\x8f\x96\xe6\xb6\x88" }; /* 确定/取消 */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", msg ? msg : "");
    dlg.count = 2;
    snprintf(dlg.items[0], DLG_ITEM_LEN, "%s", items[0]);
    snprintf(dlg.items[1], DLG_ITEM_LEN, "%s", items[1]);
    dlg.sel = 0;
    dlg.no_footer = true;
    dlg.small = true;    /* 小自适应 */
    dlg.timeout_ms = timeout_ms;
    dlg.timeout_result = timeout_result;
    dlg.cb = cb;
    dlg.ud = ud;
    os_dialog_push(ctx, &dlg);
}

/* ================= 通用档位滚轮模板 (V1.7.x) =================
 * 上下滑动/方向键翻档, 触摸点击对应文字直接选中; 底部确认/取消.
 * 回调 on_done(ctx, sel): sel=选中档; 取消/BACK → sel=-1.
 * 供"关机时间/字体大小/音量/暗黑模式"等档位型设置复用 (标准模板). */
#define WHEEL_GEN_ROW   26    /* 档位行高 */
#define WHEEL_GEN_SIDE  2     /* 当前档上下各显示 2 档 */

static bool wheel_gen_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int W = 170, H = 34 + WHEEL_GEN_ROW * 5 + 40;   /* 5 档位区 + 按钮区 */
    for (int i = 0; i < d->count; i++) {
        int w = text_width(d->items[i]) + 40;
        if (w > W) W = w;
    }
    int bx = (ui_screen_w() - W) / 2, by = (ui_screen_h() - H) / 2;
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }
    /* 档位区: 当前档居中黑框, 上下各 2 档 */
    int y0 = by + 14;
    for (int k = -WHEEL_GEN_SIDE; k <= WHEEL_GEN_SIDE; k++) {
        int idx = d->sel + k;
        if (idx < 0 || idx >= d->count) continue;
        int ry = y0 + (k + WHEEL_GEN_SIDE) * WHEEL_GEN_ROW;
        int tw = text_width(d->items[idx]);
        int cx = bx + (W - tw) / 2;
        if (k == 0) {
            fill_rect(lcd, cx - 6, ry, cx + tw + 5, ry + 24, ST7305_COLOR_BLACK);
            draw_text(lcd, cx, ry, d->items[idx], true);
        } else {
            draw_text(lcd, cx, ry, d->items[idx], false);
        }
    }
    /* 底部确认/取消 */
    int bbt = by + H - 40;
    draw_hline(lcd, bx + 2, bx + W - 3, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 3, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("\xe7\xa1\xae\xe8\xae\xa4") / 2, ty, "\xe7\xa1\xae\xe8\xae\xa4", false);  /* 确认 */
    draw_text(lcd, bx + W * 3 / 4 - text_width("\xe5\x8f\x96\xe6\xb6\x88") / 2, ty, "\xe5\x8f\x96\xe6\xb6\x88", false); /* 取消 */
    return true;
}

static bool wheel_gen_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)ud;
    int W = 170, H = 34 + WHEEL_GEN_ROW * 5 + 40;
    for (int i = 0; i < d->count; i++) {
        int w = text_width(d->items[i]) + 40;
        if (w > W) W = w;
    }
    int bx = (ui_screen_w() - W) / 2, by = (ui_screen_h() - H) / 2;
    int y0 = by + 14;
    /* 点击档位文字 → 直接选中 */
    for (int k = -WHEEL_GEN_SIDE; k <= WHEEL_GEN_SIDE; k++) {
        int idx = d->sel + k;
        if (idx < 0 || idx >= d->count) continue;
        int ry = y0 + (k + WHEEL_GEN_SIDE) * WHEEL_GEN_ROW;
        if (y >= ry - 2 && y <= ry + 25 && x >= bx && x <= bx + W - 1) {
            if (idx != d->sel) { d->sel = idx; if (ctx) ctx->needs_redraw = true; }
            return true;
        }
    }
    /* 底部按钮: 确认=完成, 取消=取消 */
    int bbt = by + H - 40;
    if (y >= bbt && y <= by + H - 3) {
        if (x < bx + W / 2) dlg_finish_top(ctx, d->sel);
        else                dlg_finish_top(ctx, -1);
        return true;
    }
    return true;
}

static void wheel_gen_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    int tx, ty;
    static int s_t_last = -1, s_t_acc = 0;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_t_last < 0) { s_t_last = ty; s_t_acc = 0; return; }
        s_t_acc += ty - s_t_last;
        s_t_last = ty;
        while (s_t_acc >= WHEEL_GEN_ROW) { s_t_acc -= WHEEL_GEN_ROW; if (d->sel > 0) { d->sel--; ctx->needs_redraw = true; } }
        while (s_t_acc <= -WHEEL_GEN_ROW) { s_t_acc += WHEEL_GEN_ROW; if (d->sel < d->count - 1) { d->sel++; ctx->needs_redraw = true; } }
    } else {
        s_t_last = -1; s_t_acc = 0;
    }
}

static bool wheel_gen_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    switch (a) {
    case OS_ACTION_UP:
        if (d->sel > 0) { d->sel--; if (ctx) ctx->needs_redraw = true; }
        return true;
    case OS_ACTION_DOWN:
        if (d->sel < d->count - 1) { d->sel++; if (ctx) ctx->needs_redraw = true; }
        return true;
    case OS_ACTION_CONFIRM:
        dlg_finish_top(ctx, d->sel);
        return true;
    case OS_ACTION_BACK:
        dlg_finish_top(ctx, -1);
        return true;
    default:
        return false;
    }
}

/* 通用档位滚轮: items 档位文字, sel 当前档, on_done 完成回调 (result=档位, -1=取消) */
void os_dialog_wheel(ui_ctx_t *ctx, const char *const *items, int n, int sel,
                     os_dialog_cb_t on_done) {
    if (!ctx || !items || n <= 0) return;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    int c = n > 60 ? 60 : n;
    for (int i = 0; i < c; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i] ? items[i] : "");
    dlg.count = c;
    dlg.sel = (sel < 0) ? 0 : (sel >= c ? c - 1 : sel);
    dlg.no_footer = true;
    dlg.cb = on_done;
    dlg.on_render = wheel_gen_render;
    dlg.on_touch  = wheel_gen_touch;
    dlg.on_poll   = wheel_gen_poll;
    dlg.on_key    = wheel_gen_key;
    os_dialog_push(ctx, &dlg);
}

void os_dialog_toast(ui_ctx_t *ctx, const char *msg) {
    os_dialog_toast_ms(ctx, msg, TOAST_MS);
}

/* V1.6.x: toast 查询/清除 (os_core 任意按键/触摸即时关闭) */
bool os_dialog_toast_active(void) { return toast_active(); }
void os_dialog_toast_clear(void) {
    s_toast_msg[0] = 0;
    s_toast_dur_ms = TOAST_MS;
}
/* V1.7.x: 全局 toast 超时清理 — 由 os_core 每帧调用 (不依赖 DIALOG 页的 dlg_poll,
 * 保证 toast 叠在普通弹窗/页面之上时同样 1s 自动关并重绘, 不残留不卡死) */
void os_dialog_toast_tick(ui_ctx_t *ctx) {
    if (s_toast_msg[0] && now_ms() - s_toast_ms >= s_toast_dur_ms) {
        if (s_top < 0) os_pop(ctx);   /* 独立 toast: 关弹窗页 */
        s_toast_msg[0] = 0;
        if (ctx) ctx->needs_redraw = true;
    }
}

/* V1.5.x: 自定义时长 toast — ms 毫秒后自动消失 (0=默认 1s). 用于"回连成功"等短提示. */
void os_dialog_toast_ms(ui_ctx_t *ctx, const char *msg, uint32_t ms) {
    s_toast_dur_ms = (ms == 0) ? TOAST_MS : ms;
    snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg ? msg : "");
    s_toast_ms = now_ms();
    if (s_top < 0) os_push(ctx, OS_PAGE_DIALOG);   /* 无弹窗栈时需打开弹窗页 */
    ctx->needs_redraw = true;                      /* 立即触发重绘, 让 toast 叠在顶层显示 */
    s_cb_handled = true;   /* cb 内提示: 保持当前弹窗, 关闭后回到原位置 */
}

/* 全局顶层 toast 覆盖: 由 os_render 在**所有模块渲染之后、flush 之前**调用,
 * 确保提示弹窗恒显示在最上 (不受全屏页如赞助二维码 st7305_clear 覆盖). */
void os_dialog_draw_overlay(ui_ctx_t *ctx) {
    if (!ctx->lcd) return;
    if (toast_active()) dlg_draw_toast(ctx->lcd);
}

/* ==== 统一退出确认 (冻结帧直绘版) ====
 * 步步高/文曲星等"自驱动游戏画面"引擎不能走 os 弹窗管道 (它们自己画帧),
 * 故退到冻结的游戏帧上直绘同一套确认框。外观/交互与 os_dialog_confirm_ex
 * 的 small 小确认模板完全一致 (标题"退出程序?"在上、左"确定"右"取消"、四边黑框、
 * 手柄 LEFT=确定/RIGHT=取消 黑块选择, CONFIRM=当前选中项, BACK=取消+松手守卫,
 * 触摸弹窗内左=确定 右=取消)。阻塞直到用户决定, 返回 true=确认退出。
 *
 * 输入一律直读手柄电平 (LEFT/RIGHT/CONFIRM/BACK) 并配上升沿/松手守卫, 不依赖
 * input 层的 nav/gamemode 开关 —— 游戏内 nav 已关闭时手柄左右仍然可控, 这是与
 * 其它自驱动引擎可靠交互的关键。物理返回/触摸仍走 input_get_action。 */
bool os_confirm_exit_blocking(st7305_handle_t *lcd) {
    if (!lcd) return true;
    const int W = 140, H = 74, MID = W / 2;
    const int BX = (ui_screen_w() - W) / 2, BY = (ui_screen_h() - H) / 2;
    const char *s0 = "\xe7\xa1\xae\xe5\xae\x9a"; /* 确定 */
    const char *s1 = "\xe5\x8f\x96\xe6\xb6\x88"; /* 取消 */
    const char *title = "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?"; /* 退出程序? */
    int sel = 0;   /* 0=确定 1=取消 */

    bool conn = bt_manager_is_connected();
    /* 各键上次电平 (上升沿触发一次) + 触发键/确认键是否还按着 (松手守卫) */
    bool l_prev = conn && bt_manager_is_key_pressed(F_LEFT);
    bool r_prev = conn && bt_manager_is_key_pressed(F_RIGHT);
    bool c_prev = conn && bt_manager_is_key_pressed(F_CONFIRM);
    bool b_prev = conn && bt_manager_is_key_pressed(F_BACK);
    /* 进入弹窗这一刻若触发键/确认键还按着, 不立即当作确认/取消 (松手守卫) */
    bool confirm_held = c_prev;
    bool back_held    = b_prev;

    while (1) {
        fill_rect(lcd, BX, BY, BX + W - 1, BY + H - 1, ST7305_COLOR_WHITE);
        for (int k = 0; k < 2; k++) {
            draw_hline(lcd, BX + k, BX + W - 1 - k, BY + k, ST7305_COLOR_BLACK);
            draw_hline(lcd, BX + k, BX + W - 1 - k, BY + H - 1 - k, ST7305_COLOR_BLACK);
            draw_vline(lcd, BX + k, BY + k, BY + H - 1 - k, ST7305_COLOR_BLACK);          /* 左边线 */
            draw_vline(lcd, BX + W - 1 - k, BY + k, BY + H - 1 - k, ST7305_COLOR_BLACK);  /* 右边线 */
        }
        draw_text_centered(lcd, BY + 7, title, false);
        int by = BY + 36;
        int bot = BY + H - 1 - 2;
        int bx0 = BX + 2, bx1 = BX + W - 1 - 2;
        int mid = BX + W / 2;   /* 正中分界竖线 (绝对坐标, 对齐 os_dialog small 模板) */
        fill_rect(lcd, bx0, by, mid - 1, bot, (sel == 0) ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        fill_rect(lcd, mid + 1, by, bx1, bot, (sel == 1) ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        draw_hline(lcd, BX,     BX + W - 1, by,     ST7305_COLOR_BLACK);
        draw_hline(lcd, BX,     BX + W - 1, by + 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, mid,     by, bot, ST7305_COLOR_BLACK);
        draw_vline(lcd, mid + 1, by, bot, ST7305_COLOR_BLACK);
        int ty = by + 7;
        int cx0 = (bx0 + mid - 1) / 2, cx1 = (mid + 2 + bx1) / 2;
        draw_text(lcd, cx0 - text_width(s0) / 2, ty, s0, (sel == 0));
        draw_text(lcd, cx1 - text_width(s1) / 2, ty, s1, (sel == 1));
        st7305_flush(lcd);

        /* ---- 手柄: 直读电平上升沿, 与 nav/gamemode 无关 ---- */
        if (conn) {
            bool l = bt_manager_is_key_pressed(F_LEFT);
            if (l && !l_prev) sel = 0;          /* LEFT=确定 */
            l_prev = l;
            bool r = bt_manager_is_key_pressed(F_RIGHT);
            if (r && !r_prev) sel = 1;          /* RIGHT=取消 */
            r_prev = r;
            bool c = bt_manager_is_key_pressed(F_CONFIRM);
            if (c && !c_prev && !confirm_held) return (sel == 0);   /* CONFIRM=当前选中项 */
            confirm_held = confirm_held || c;
            if (!c) confirm_held = false;
            c_prev = c;
            bool b = bt_manager_is_key_pressed(F_BACK);
            if (b && !b_prev && !back_held) return false;           /* BACK=取消 */
            back_held = back_held || b;
            if (!b) back_held = false;
            b_prev = b;
        }

        /* ---- 物理键 / 触摸: 走 input 层动作 ---- */
        menu_action_t action = input_get_action();
        if (action == MENU_ACTION_LEFT) sel = 0;
        else if (action == MENU_ACTION_RIGHT) sel = 1;
        else if (action == MENU_ACTION_BACK) return false;          /* 物理返回=取消 */
        else if (action == MENU_ACTION_CONFIRM || action == MENU_ACTION_LONG_LEFT) {
            int tx, ty2;
            if (input_consume_tap(&tx, &ty2)) {
                if (tx >= BX && tx < BX + W && ty2 >= BY && ty2 < BY + H)
                    return (tx < BX + MID);                          /* 触摸: 左=确定 右=取消 */
                continue;                                            /* 点在弹窗外 → 忽略 */
            }
            if (!confirm_held) return (sel == 0);                    /* 物理确认键 → 当前选中项 */
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
