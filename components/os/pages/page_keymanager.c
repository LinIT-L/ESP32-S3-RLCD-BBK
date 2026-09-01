/**
 * page_keymanager.c — 密钥管理器 页面模块.
 *
 * 功能:
 *   - 首次进入引导设置 PIN (大九宫格 + 底三键: 删除/取消/确定),
 *     并提醒"忘记 PIN 将无法读取所有记录"
 *   - PIN 锁 + 暴力破解退避 -> 解锁后才能看/用密钥
 *   - 左右分栏: 左栏分类(全部/网站/程序/游戏/自定义), 右栏该分类下的密钥
 *   - 分类可 新增/改名/删除; 选择密钥确认 = USB HID 自动输入该密码
 *   - 复用 usb_hid 键盘模拟逐字符输入 (完整 ASCII→HID 映射)
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "keyvault.h"
#include "usb_hid.h"
#include "keyboard.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TAG "KEYM"

/* ==== 逻辑坐标 (横屏 400x300) ==== */
#define S_W   400
#define S_H   300

typedef enum {
    KM_FIRST,        /* 首次进入: 提醒忘记 PIN 后果 */
    KM_SETPIN,       /* 设置新 PIN (两次输入确认) */
    KM_LOCK,         /* 解锁 */
    KM_LIST,         /* 左右分栏: 分类 + 密钥 */
    KM_ADD_NAME,     /* 新增: 输入名称 */
    KM_ADD_SECRET,   /* 新增: 输入密码 */
    KM_CAT_MGR,      /* 分类管理列表 */
    KM_CAT_NAME,     /* 分类: 输入新名/改名 */
    KM_SEND          /* USB 输入中 */
} km_state_t;

/* ==== 九宫格布局 (掌机复古: 紧凑网格) ==== */
#define KP_COL_N   3
#define KP_COL_W   84
#define KP_COL_X0  60
#define KP_COL_GAP 14
#define KP_ROW_H   38
#define KP_ROW_GAP 6
#define KP_TOP     52             /* 数字网格起点 y (标题/PIN 行之下) */
#define KP_BTN_Y   252            /* 底三键行 y */
#define KP_BTN_H   (S_H - KP_BTN_Y)      /* 到 300 */

typedef enum { KP_ACT_INVALID=-1, KP_ACT_DEL=0, KP_ACT_CANCEL, KP_ACT_OK } kp_action_t;

static km_state_t s_state = KM_FIRST;
static uint8_t   s_key[KEYVAULT_KEY_LEN];
static char      s_pin[16];
static int       s_pin_len = 0;
static char      s_pin_again[16]; /* 设置 PIN 第二次输入 */
static int       s_again_len = 0;
static int       s_cursor_r = 0, s_cursor_c = 0;   /* 九宫格光标 */

static char      s_names[KEYVAULT_MAX_ENTRIES][64];
static int       s_cats[KEYVAULT_MAX_ENTRIES];
static int       s_total = 0;

/* 左右分栏状态 */
static int       s_focus = 0;      /* 0=左栏分类 1=右栏密钥 */
static int       s_sel_cat = 0;    /* 0=全部, 1..=分类索引 */
static int       s_view[KEYVAULT_MAX_ENTRIES]; /* 右栏筛选后的全局条目序号 */
static int       s_view_n = 0;
static int       s_sel_item = 0;   /* 右栏光标(在 s_view 中的位置) */
static int       s_cat_scroll = 0;
static int       s_item_scroll = 0;

/* 新增/分类输入缓冲 */
static char      s_buf[130];
static int       s_buf_len = 0;
static char      s_name_buf[60];   /* 新增时名称 */
static int       s_add_cat = 0;    /* 新增时选中的分类 */
static int       s_cat_manage_cursor = 0; /* 分类管理列表光标 */
static int       s_cat_edit_idx = -1;

typedef struct { uint8_t kc; uint8_t mod; } kv_kc_t;
static const kv_kc_t s_asc[95] = {
#include "keymgr_hid.inc"
};

static void kv_kbd_str(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const kv_kc_t *e = &s_asc[c - 0x20];
        usb_hid_key_tap(e->mod, e->kc);
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }
static void km_add_confirm(ui_ctx_t *ctx);   /* 新增输入确认 (触摸/键盘回调) */

/* ==== 数据加载 ==== */
static void load_entries(void) {
    s_total = keyvault_list(s_names, s_cats, KEYVAULT_MAX_ENTRIES);
}
static void rebuild_view(void) {
    s_view_n = 0;
    for (int i = 0; i < s_total; i++) {
        if (s_sel_cat == 0 || s_cats[i] == s_sel_cat - 1) s_view[s_view_n++] = i;
    }
    if (s_sel_item >= s_view_n) s_sel_item = 0;
    if (s_sel_item < 0) s_sel_item = 0;
}

/* 分类名惯用名 (前几个内置直接用中文常量, 便于显示跳转) */
static const char *km_cat_label(int idx) {
    const char *n = keyvault_cat_name(idx);
    return n ? n : "";
}

/* ==== 九宫格: 单元 → 屏幕坐标 ==== */
static void kp_rect(int r, int c, int *x, int *y, int *w, int *h) {
    *x = KP_COL_X0 + c * (KP_COL_W + KP_COL_GAP);
    *y = KP_TOP + r * (KP_ROW_H + KP_ROW_GAP);
    *w = KP_COL_W; *h = KP_ROW_H;
}
static void kp_btn_rect(int b, int *x, int *y, int *w, int *h) {
    *x = KP_COL_X0 + b * (KP_COL_W + KP_COL_GAP);
    *y = KP_BTN_Y; *w = KP_COL_W; *h = KP_BTN_H;
}
/* 返回该格字符: r0-2=1..9, r3=0; 无效返回 0 */
static char kp_cell_char(int r, int c) {
    if (r < 0 || r >= 3) return 0;
    int d = r * 3 + c + 1;
    return (d >= 1 && d <= 9) ? (char)('0' + d) : 0;
}
/* 底三键动作 */
static kp_action_t kp_btn_action(int b) {
    switch (b) {
        case 0: return KP_ACT_DEL;
        case 1: return KP_ACT_CANCEL;
        default: return KP_ACT_OK;
    }
}

/* 把九宫格光标 clamp 到合法格子 (r0-3, c0-2; 第4行只有 0 在中间), 落在底三键时用特殊标记 */
static void kp_clamp_cursor(void) {
    if (s_cursor_r < 0) s_cursor_r = 0;
    if (s_cursor_r > 3) s_cursor_r = 3;
    if (s_cursor_c < 0) s_cursor_c = 0;
    if (s_cursor_c > 2) s_cursor_c = 2;
    if (s_cursor_r == 3 && s_cursor_c != 1) s_cursor_c = 1;  /* 第4行仅 0 */
}

/* ==== PIN 校验/设置 ==== */
static void km_submit_pin(ui_ctx_t *ctx, bool setting) {
    if (s_pin_len < 4) { os_dialog_toast(ctx, "至少 4 位"); return; }
    if (setting) {
        if (s_again_len == 0) {
            /* 第一次输入完成, 记录并进入二次确认 */
            memcpy(s_pin_again, s_pin, sizeof(s_pin_again) - 1);
            s_pin_again[sizeof(s_pin_again) - 1] = '\0';
            s_again_len = s_pin_len;
            s_pin[0] = 0; s_pin_len = 0;
            os_dialog_toast(ctx, "再次输入确认");
            redraw(ctx);
            return;
        }
        if (strcmp(s_pin, s_pin_again) != 0) {
            os_dialog_toast(ctx, "两次不一致, 重新输入");
            s_pin[0] = 0; s_pin_len = 0;
            s_again_len = 0; s_pin_again[0] = 0;
            redraw(ctx);
            return;
        }
        keyvault_set_pin(s_pin_again);
        keyvault_unlock_secret(s_pin_again, s_key);
        s_pin[0] = 0; s_pin_len = 0;
        s_again_len = 0; s_pin_again[0] = 0;
        load_entries();
        rebuild_view();
        s_state = KM_LIST; s_focus = 0; s_sel_item = 0;
    } else {
        int r = keyvault_verify_pin(s_pin);
        if (r == 0) {
            keyvault_unlock_secret(s_pin, s_key);
            s_pin[0] = 0; s_pin_len = 0;
            load_entries();
            rebuild_view();
            s_state = KM_LIST; s_focus = 0; s_sel_item = 0;
        } else if (r == 1) {
            os_dialog_toast(ctx, "PIN 错误");
            s_pin[0] = 0; s_pin_len = 0;
        } else if (r == 2) {
            os_dialog_toast(ctx, "已锁定, 请稍后");
            s_pin[0] = 0; s_pin_len = 0;
        }
    }
    redraw(ctx);
}

static void km_pin_digit(ui_ctx_t *ctx, char ch) {
    if (s_pin_len < 8) { s_pin[s_pin_len++] = ch; s_pin[s_pin_len] = 0; }
    redraw(ctx);
}
static void km_pin_del(ui_ctx_t *ctx) {
    if (s_pin_len > 0) { s_pin[--s_pin_len] = 0; redraw(ctx); }
}

/* ==== 渲染: 九宫格 PIN 界面 ==== */
static void kp_render_cells(st7305_handle_t *lcd) {
    /* 第一行: 快捷键 1..9 */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int x, y, w, h;
            kp_rect(r, c, &x, &y, &w, &h);
            bool cur = (s_cursor_r == r && s_cursor_c == c);
            draw_rect_outline(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_BLACK);
            if (cur) fill_rect(lcd, x + 1, y + 1, x + w - 2, y + h - 2, ST7305_COLOR_BLACK);
            char buf[2] = { kp_cell_char(r, c), 0 };
            draw_label_centered_at(lcd, x + w / 2, y + (h - 26) / 2, buf, cur);
        }
    }
    /* 第 4 行: 0 */
    {
        int x, y, w, h;
        kp_rect(3, 1, &x, &y, &w, &h);
        bool cur = (s_cursor_r == 3);
        draw_rect_outline(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_BLACK);
        if (cur) fill_rect(lcd, x + 1, y + 1, x + w - 2, y + h - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, x + w / 2, y + (h - 26) / 2, "0", cur);
    }
    /* 底三键: 删除/取消/确定 (与九宫格同列, 光标用列索引表达) */
    static const char *btn[3] = { "删除", "取消", "确定" };
    for (int b = 0; b < 3; b++) {
        int x, y, w, h;
        kp_btn_rect(b, &x, &y, &w, &h);
        bool cur = (s_cursor_r == 4 && s_cursor_c == b);
        draw_rect_outline(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_BLACK);
        if (cur) fill_rect(lcd, x + 1, y + 1, x + w - 2, y + h - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, x + w / 2, y + 12, btn[b], cur);
    }
}

/* ==== 渲染: 左右分栏 ==== */
static void km_render_list(st7305_handle_t *lcd) {
    const int list_y = 30;
    const int left_w = 96;
    const int row_h = 32;
    const int right_x = left_w + 6;
    const int bottom = S_H - 40;

    /* 顶部黑色状态栏: 左"分类" 右"密钥 N" */
    fill_rect(lcd, 0, 0, S_W - 1, 22, ST7305_COLOR_BLACK);
    int tw = text_width("密钥");
    draw_text(lcd, 6, 3, "密钥", true);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d", s_total);
    tw = text_width(cnt);
    draw_text(lcd, S_W - 6 - tw, 3, cnt, true);

    /* 左栏: 分类 (0=全部, 1..=分类) */
    int n_cat = keyvault_cat_count();         /* 分类数 */
    int left_total = 1 + n_cat;               /* + 全部 */
    int max_vis = (bottom - list_y) / row_h;
    if (s_sel_cat < s_cat_scroll) s_cat_scroll = s_sel_cat;
    if (s_sel_cat >= s_cat_scroll + max_vis) s_cat_scroll = s_sel_cat - max_vis + 1;
    int y = list_y;
    for (int i = 0; i < max_vis && (s_cat_scroll + i) < left_total; i++) {
        int idx = s_cat_scroll + i;
        const char *label = (idx == 0) ? "全部" : km_cat_label(idx - 1);
        int l2 = text_width(label);
        int tx = 2 + (left_w - l2) / 2;
        if (tx < 4) tx = 4;
        draw_text(lcd, tx, y + 4, label, false);
        if (s_focus == 0 && idx == s_sel_cat) {
            draw_hline(lcd, 0, left_w, y + row_h - 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, 0, left_w, y + row_h, ST7305_COLOR_BLACK);
        }
        y += row_h;
    }
    draw_vline(lcd, left_w + 3, list_y, bottom, ST7305_COLOR_BLACK);

    /* 右栏: 当前分类下密钥 */
    if (s_view_n == 0) {
        draw_text_centered(lcd, list_y + (bottom - list_y) / 2 - 12, "暂无密钥", false);
    } else {
        int x0 = right_x, x1 = S_W - 4;
        int maxr = (bottom - list_y) / row_h;
        if (s_sel_item < s_item_scroll) s_item_scroll = s_sel_item;
        if (s_sel_item >= s_item_scroll + maxr) s_item_scroll = s_sel_item - maxr + 1;
        y = list_y;
        for (int i = 0; i < maxr && (s_item_scroll + i) < s_view_n; i++) {
            int vi = s_item_scroll + i;
            int gi = s_view[vi];
            bool sel = (s_focus == 1 && vi == s_sel_item);
            if (sel) fill_rect(lcd, x0, y, x1, y + row_h - 2, ST7305_COLOR_BLACK);
            int l2 = text_width(s_names[gi]);
            int tx = x0 + ((x1 - x0) - l2) / 2;
            if (tx < x0) tx = x0;
            draw_text(lcd, tx, y + 4, s_names[gi], sel);
            y += row_h;
        }
    }

    /* 底部: 新增/分类管理/删除(右栏选中)/返回 (掌机底栏按键) */
    int by = S_H - 38;
    draw_hline(lcd, 0, S_W, by - 2, ST7305_COLOR_BLACK);
    draw_label_centered_at(lcd, left_w / 2, by - 1, "分类", false);
    draw_label_centered_at(lcd, 2 * left_w, by - 1, "新增", false);
    draw_label_centered_at(lcd, 2 * left_w + (S_W - 2 * left_w) / 2, by - 1, "删除", false);
    draw_label_centered_at(lcd, S_W - 40, by - 1, "返回", false);
}

/* ==== 渲染主入口 ==== */
static void km_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    if (s_state == KM_FIRST) {
        /* 顶部黑色状态栏 */
        fill_rect(lcd, 0, 0, S_W - 1, 24, ST7305_COLOR_BLACK);
        int tw = text_width("首次使用");
        draw_text(lcd, (S_W - tw) / 2, 4, "首次使用", true);
        draw_text_centered(lcd, 50, "请设置 PIN 并牢记", false);
        draw_text_centered(lcd, 90, "忘记 PIN 将无法读取", false);
        draw_text_centered(lcd, 120, "所有已保存的记录", false);
        draw_hline(lcd, 20, S_W - 20, 150, ST7305_COLOR_BLACK);
        draw_text_centered(lcd, 235, "确定: 开始设置  ·  BACK: 取消", false);
    }
    else if (s_state == KM_SETPIN || s_state == KM_LOCK) {
        /* 顶部黑色状态栏 (掌机信息条) */
        const char *t = (s_state == KM_SETPIN) ? "设置 PIN" : "输入 PIN 解锁";
        fill_rect(lcd, 0, 0, S_W - 1, 24, ST7305_COLOR_BLACK);
        int tw = text_width(t);
        draw_text(lcd, (S_W - tw) / 2, 4, t, true);
        /* PIN 槽位 (掌机输入槽) */
        int n_slot = 8;
        int slot_w = 22, slot_gap = 10, slot_all = n_slot * slot_w + (n_slot - 1) * slot_gap;
        int x0 = (S_W - slot_all) / 2;
        for (int i = 0; i < n_slot; i++) {
            int sx = x0 + i * (slot_w + slot_gap);
            bool filled = (i < s_pin_len);
            if (filled) fill_rect(lcd, sx, 34, sx + slot_w - 1, 52, ST7305_COLOR_BLACK);
            else draw_rect_outline(lcd, sx, 34, sx + slot_w - 1, 52, ST7305_COLOR_BLACK);
        }
        kp_render_cells(lcd);
    }
    else if (s_state == KM_LIST) {
        km_render_list(lcd);
    }
    else if (s_state == KM_ADD_NAME || s_state == KM_ADD_SECRET) {
        fill_rect(lcd, 0, 0, S_W - 1, 22, ST7305_COLOR_BLACK);
        int l2 = text_width((s_state == KM_ADD_NAME) ? "输入名称" : "输入密码");
        draw_text(lcd, (S_W - l2) / 2, 3, (s_state == KM_ADD_NAME) ? "输入名称" : "输入密码", true);
        draw_text_centered(lcd, 30, s_buf, false);
        kbd_draw(lcd, 52, &kbd_std, -1);
        draw_text_centered(lcd, S_H - 12, "回车: 确认  ·  BACK: 返回", false);
    }
    else if (s_state == KM_CAT_MGR) {
        fill_rect(lcd, 0, 0, S_W - 1, 22, ST7305_COLOR_BLACK);
        int l2 = text_width("分类管理");
        draw_text(lcd, (S_W - l2) / 2, 3, "分类管理", true);
        int n_cat = keyvault_cat_count();
        int y = 30;
        int maxr = (S_H - 44 - 40) / 30;
        int scroll = 0;
        if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor >= maxr) scroll = s_cat_manage_cursor - maxr + 1;
        for (int i = 0; i < maxr && (scroll + i) < n_cat; i++) {
            int idx = scroll + i;
            bool sel = (idx == s_cat_manage_cursor);
            if (sel) fill_rect(lcd, 30, y, S_W - 30, y + 26, ST7305_COLOR_BLACK);
            int tw = text_width(km_cat_label(idx));
            int tx = (S_W - tw) / 2;
            draw_text(lcd, tx, y + 4, km_cat_label(idx), sel);
            y += 30;
        }
        int by = S_H - 40;
        draw_hline(lcd, 0, S_W, by - 2, ST7305_COLOR_BLACK);
        draw_label_centered_at(lcd, (S_W / 3) / 2, by - 1, "新增", false);
        draw_label_centered_at(lcd, (S_W / 3) * 1.5f, by - 1, "改名", false);
        draw_label_centered_at(lcd, (S_W / 3) * 2.5f, by - 1, "删除", false);
    }
    else if (s_state == KM_CAT_NAME) {
        fill_rect(lcd, 0, 0, S_W - 1, 22, ST7305_COLOR_BLACK);
        int l2 = text_width("分类名称");
        draw_text(lcd, (S_W - l2) / 2, 3, "分类名称", true);
        draw_text_centered(lcd, 30, s_buf, false);
        kbd_draw(lcd, 52, &kbd_std, -1);
        draw_text_centered(lcd, S_H - 12, "回车: 确认  ·  BACK: 返回", false);
    }
    else if (s_state == KM_SEND) {
        draw_text_centered(lcd, 140, "正在输入...", false);
    }
}

/* ==== 九宫格动作处理 (触摸/手柄光标统一) ==== */
static void kp_press_cell(ui_ctx_t *ctx, int r, int c) {
    char ch = kp_cell_char(r, c);
    if (ch) { km_pin_digit(ctx, ch); return; }
    if (r == 3 && c == 1) { km_pin_digit(ctx, '0'); return; }
    if (r == 4) {
        kp_action_t a = kp_btn_action(c);
        if (a == KP_ACT_DEL) km_pin_del(ctx);
        else if (a == KP_ACT_OK) km_submit_pin(ctx, s_state == KM_SETPIN);
        /* CANCEL 由外层处理 */
    }
}

/* ==== 按键 ==== */
static void km_action(ui_ctx_t *ctx, os_action_t a) {
    /* 手柄全局: 九宫格界面用光标导航 */
    if (s_state == KM_LOCK || s_state == KM_SETPIN) {
        if (a == OS_ACTION_UP) { if (s_cursor_r > 0) { s_cursor_r--; kp_clamp_cursor(); redraw(ctx);} return; }
        if (a == OS_ACTION_DOWN) { if (s_cursor_r < 4) { s_cursor_r++; kp_clamp_cursor(); redraw(ctx);} return; }
        if (a == OS_ACTION_LEFT) { if (s_cursor_c > 0 && s_cursor_r != 3) { s_cursor_c--; } redraw(ctx); return; }
        if (a == OS_ACTION_RIGHT) { if (s_cursor_c < 2 && s_cursor_r != 3) { s_cursor_c++; } redraw(ctx); return; }
        if (a == OS_ACTION_CONFIRM) { kp_press_cell(ctx, s_cursor_r, s_cursor_c); return; }
        if (a == OS_ACTION_BACK) {
            if (s_state == KM_LOCK) { os_pop(ctx); }
            else { s_pin[0]=0; s_pin_len=0; os_pop(ctx); }
            return;
        }
        return;
    }

    if (s_state == KM_FIRST) {
        if (a == OS_ACTION_CONFIRM) { s_state = KM_SETPIN; s_pin[0]=0; s_pin_len=0; s_cursor_r=0; s_cursor_c=0; redraw(ctx); }
        else if (a == OS_ACTION_BACK) { os_pop(ctx); }
        return;
    }

    if (s_state == KM_LIST) {
        if (a == OS_ACTION_LEFT) { s_focus = 0; redraw(ctx); return; }
        if (a == OS_ACTION_RIGHT) { s_focus = 1; redraw(ctx); return; }
        if (a == OS_ACTION_UP) {
            if (s_focus == 0) { if (s_sel_cat > 0) { s_sel_cat--; rebuild_view(); } }
            else { if (s_sel_item > 0) s_sel_item--; }
            redraw(ctx); return;
        }
        if (a == OS_ACTION_DOWN) {
            if (s_focus == 0) {
                int n_cat = keyvault_cat_count();
                if (s_sel_cat < n_cat) { s_sel_cat++; rebuild_view(); }
            } else { if (s_sel_item < s_view_n - 1) s_sel_item++; }
            redraw(ctx);
            return;
        }
        if (a == OS_ACTION_CONFIRM) {
            if (s_focus == 0) { rebuild_view(); redraw(ctx); }
            else if (s_view_n > 0) {
                int gi = s_view[s_sel_item];
                char sec[KEYVAULT_MAX_SECRET + 1];
                if (keyvault_get_secret(s_key, gi, sec, sizeof(sec)) == 0) {
                    s_state = KM_SEND; redraw(ctx);
                    input_set_gamepad_nav_enabled(false);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    kv_kbd_str(sec);
                    input_set_gamepad_nav_enabled(true);
                    s_state = KM_LIST; redraw(ctx);
                }
            }
            return;
        }
        if (a == OS_ACTION_BACK) { os_pop(ctx); return; }
        return;
    }

    if (s_state == KM_ADD_NAME || s_state == KM_ADD_SECRET) {
        if (a == OS_ACTION_BACK) {
            s_buf_len = 0; s_buf[0] = 0;
            if (s_state == KM_ADD_SECRET) s_state = KM_ADD_NAME;
            else s_state = KM_LIST;
            redraw(ctx);
        }
        return;
    }

    if (s_state == KM_CAT_MGR) {
        int n_cat = keyvault_cat_count();
        if (a == OS_ACTION_UP) { if (s_cat_manage_cursor > 0) s_cat_manage_cursor--; redraw(ctx); return; }
        if (a == OS_ACTION_DOWN) { if (s_cat_manage_cursor < n_cat - 1) s_cat_manage_cursor++; redraw(ctx); return; }
        if (a == OS_ACTION_BACK) { s_state = KM_LIST; redraw(ctx); return; }
        return;
    }

    if (s_state == KM_CAT_NAME) {
        if (a == OS_ACTION_BACK) { s_buf_len = 0; s_buf[0] = 0; s_state = KM_CAT_MGR; redraw(ctx); }
        return;
    }

    if (s_state == KM_SEND) {
        if (a == OS_ACTION_BACK) { s_state = KM_LIST; redraw(ctx); }
    }
}

/* ==== 触摸 ==== */
static bool km_touch(ui_ctx_t *ctx, int x, int y) {
    if (x < 0 || y < 0) return false;

    if (s_state == KM_LOCK || s_state == KM_SETPIN) {
        /* 命中九宫格单元 */
        for (int r = 0; r < 3; r++) {
            int ry = KP_TOP + r * (KP_ROW_H + KP_ROW_GAP);
            if (y >= ry && y < ry + KP_ROW_H) {
                int c = (x - KP_COL_X0) / (KP_COL_W + KP_COL_GAP);
                if (c >= 0 && c <= 2 && kp_cell_char(r, c)) { s_cursor_r = r; s_cursor_c = c; kp_press_cell(ctx, r, c); return true; }
                return true;
            }
        }
        /* 0 行 */
        if (y >= KP_TOP + 3 * (KP_ROW_H + KP_ROW_GAP) && y < KP_TOP + 3 * (KP_ROW_H + KP_ROW_GAP) + KP_ROW_H) {
            int c = (x - KP_COL_X0) / (KP_COL_W + KP_COL_GAP);
            if (c == 1) { s_cursor_r = 3; s_cursor_c = 1; km_pin_digit(ctx, '0'); return true; }
            return true;
        }
        /* 底三键 */
        if (y >= KP_BTN_Y) {
            for (int b = 0; b < 3; b++) {
                int bx = KP_COL_X0 + b * (KP_COL_W + KP_COL_GAP);
                if (x >= bx && x < bx + KP_COL_W) {
                    s_cursor_r = 4; s_cursor_c = b;
                    kp_action_t act = kp_btn_action(b);
                    if (act == KP_ACT_DEL) km_pin_del(ctx);
                    else if (act == KP_ACT_OK) km_submit_pin(ctx, s_state == KM_SETPIN);
                    /* CANCEL */
                    return true;
                }
            }
            return true;
        }
        return true;
    }

    if (s_state == KM_FIRST) {
        return true;   /* 手柄 BACK/CONFIRM 控制 */
    }

    if (s_state == KM_LIST) {
        const int list_y = 30;
        const int left_w = 96;
        const int row_h = 32;
        /* 左栏分类 */
        if (x < left_w + 3) {
            int idx = s_cat_scroll + (y - list_y) / row_h;
            if (idx >= 0 && idx <= keyvault_cat_count()) {
                if (s_sel_cat != idx) { s_sel_cat = idx; rebuild_view(); }
                s_focus = 0;
                redraw(ctx);
            }
            return true;
        }
        /* 右栏密钥 */
        if (y >= list_y && y < S_H - 40) {
            int vi = s_item_scroll + (y - list_y) / row_h;
            if (vi >= 0 && vi < s_view_n) { s_sel_item = vi; s_focus = 1; redraw(ctx); }
            return true;
        }
        /* 底部操作 */
        if (y >= S_H - 40) {
            if (x < left_w) { s_focus = 0; s_state = KM_CAT_MGR; s_cat_manage_cursor = 0; redraw(ctx); }
            else if (x < 2 * left_w) {
                /* 新增 */
                s_add_cat = 0; s_buf[0] = 0; s_buf_len = 0;
                s_state = KM_ADD_NAME; redraw(ctx);
            }
            else if (x < S_W - 60) {
                /* 删除右栏选中 */
                if (s_view_n > 0) {
                    int gi = s_view[s_sel_item];
                    keyvault_remove(s_key, gi);
                    os_dialog_toast(ctx, "已删除");
                    load_entries();
                    rebuild_view();
                    redraw(ctx);
                }
            }
            else { os_pop(ctx); }
            return true;
        }
        return true;
    }

    if (s_state == KM_ADD_NAME || s_state == KM_ADD_SECRET) {
        int ki = kbd_hit(&kbd_std, 52, x, y);
        if (ki >= 0) {
            int act = kbd_press(&kbd_std, ki);
            if (act == KBD_ACT_CHAR) {
                char ch = kbd_char(&kbd_std, ki);
                if (ch && s_buf_len < (int)sizeof(s_buf) - 1) { s_buf[s_buf_len++] = ch; s_buf[s_buf_len] = 0; }
            } else if (act == KBD_ACT_BKSP) { if (s_buf_len > 0) s_buf[--s_buf_len] = 0; }
            else if (act == KBD_ACT_ENTER) { km_add_confirm(ctx); }
            else if (act == KBD_ACT_SPACE) { if (s_buf_len < (int)sizeof(s_buf) - 1) { s_buf[s_buf_len++] = ' '; s_buf[s_buf_len] = 0; } }
            redraw(ctx);
        }
        return true;
    }

    if (s_state == KM_CAT_MGR) {
        /* 列表 */
        if (y >= 44 && y < S_H - 44) {
            int idx = (s_cat_manage_cursor >= 0 && s_cat_manage_cursor * 30 >= (y - 44)) ? s_cat_manage_cursor : 0;
            (void)idx;
            int i = (y - 44) / 30 + ((s_cat_manage_cursor >= 0 && s_cat_manage_cursor >= (S_H - 44 - 40) / 30) ? s_cat_manage_cursor - ((S_H - 44 - 40) / 30) + 1 : 0);
            if (i >= 0 && i < keyvault_cat_count()) { s_cat_manage_cursor = i; redraw(ctx); }
            return true;
        }
        /* 底部: 新增/改名/删除 */
        if (y >= S_H - 40) {
            if (x < S_W / 3) {
                s_cat_edit_idx = -1; s_buf[0] = 0; s_buf_len = 0; s_state = KM_CAT_NAME; redraw(ctx);
            }
            else if (x < 2 * S_W / 3) {
                if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor < keyvault_cat_count()) {
                    s_cat_edit_idx = s_cat_manage_cursor;
                    strncpy(s_buf, km_cat_label(s_cat_manage_cursor), sizeof(s_buf) - 1);
                    s_buf_len = (int)strlen(s_buf);
                    s_state = KM_CAT_NAME; redraw(ctx);
                }
            }
            else {
                if (s_cat_manage_cursor >= 0 && s_cat_manage_cursor < keyvault_cat_count()) {
                    if (keyvault_cat_remove(s_cat_manage_cursor) != 0)
                        os_dialog_toast(ctx, "无法删除");
                    else {
                        load_entries(); rebuild_view();
                        if (s_cat_manage_cursor >= keyvault_cat_count()) s_cat_manage_cursor--;
                        redraw(ctx);
                    }
                }
            }
            return true;
        }
        return true;
    }

    if (s_state == KM_CAT_NAME) {
        int ki = kbd_hit(&kbd_std, 52, x, y);
        if (ki >= 0) {
            int act = kbd_press(&kbd_std, ki);
            if (act == KBD_ACT_CHAR) {
                char ch = kbd_char(&kbd_std, ki);
                if (ch && s_buf_len < 30) { s_buf[s_buf_len++] = ch; s_buf[s_buf_len] = 0; }
            } else if (act == KBD_ACT_BKSP) { if (s_buf_len > 0) s_buf[--s_buf_len] = 0; }
            else if (act == KBD_ACT_ENTER) {
                if (s_buf_len == 0) os_dialog_toast(ctx, "名称不能为空");
                else if (s_cat_edit_idx < 0) { if (keyvault_cat_add(s_buf) != 0) os_dialog_toast(ctx, "无法添加(重名/已满)"); }
                else { if (keyvault_cat_rename(s_cat_edit_idx, s_buf) != 0) os_dialog_toast(ctx, "重名"); }
                s_buf[0] = 0; s_buf_len = 0; s_cat_edit_idx = -1;
                load_entries(); rebuild_view();
                s_state = KM_CAT_MGR; redraw(ctx);
            }
            else if (act == KBD_ACT_SPACE) { if (s_buf_len < 30) { s_buf[s_buf_len++] = ' '; s_buf[s_buf_len] = 0; } }
            redraw(ctx);
        }
        return true;
    }

    return false;
}

/* 新增确认回调: 名称段 → 保存名称并切密码段; 密码段 → 保存条目 */
static void km_add_confirm(ui_ctx_t *ctx) {
    if (s_state == KM_ADD_NAME) {
        if (s_buf_len == 0) { os_dialog_toast(ctx, "名称不能为空"); return; }
        strncpy(s_name_buf, s_buf, sizeof(s_name_buf) - 1); s_name_buf[sizeof(s_name_buf) - 1] = 0;
        s_buf[0] = 0; s_buf_len = 0;
        s_state = KM_ADD_SECRET;
        redraw(ctx);
    } else if (s_state == KM_ADD_SECRET) {
        if (s_buf_len == 0) { os_dialog_toast(ctx, "密码不能为空"); return; }
        /* 归类到当前左栏选中的分类 (若为"全部"则归入第一个分类) */
        int cat = (s_sel_cat > 0) ? (s_sel_cat - 1) : 0;
        if (keyvault_add(s_key, cat, s_name_buf, s_buf) == 0)
            os_dialog_toast(ctx, "已添加");
        else
            os_dialog_toast(ctx, "添加失败");
        s_buf[0] = 0; s_buf_len = 0;
        load_entries();
        rebuild_view();
        s_state = KM_LIST;
        redraw(ctx);
    }
}

/* 进入: 初始化密钥库 + 复位界面 */
static void km_enter(ui_ctx_t *ctx) {
    keyvault_init();
    s_state = keyvault_has_pin() ? KM_LOCK : KM_FIRST;
    s_pin[0] = 0; s_pin_len = 0;
    s_pin_again[0] = 0; s_again_len = 0;
    s_cursor_r = 0; s_cursor_c = 0;
    s_sel_cat = 0; s_focus = 0; s_sel_item = 0;
    s_cat_scroll = 0; s_item_scroll = 0;
    load_entries();
    rebuild_view();
    s_buf[0] = 0; s_buf_len = 0;
    ctx->needs_redraw = true;
}
/* 退出: 复位到"未解锁" */
static void km_exit(ui_ctx_t *ctx) {
    (void)ctx;
    s_state = KM_FIRST;
    memset(s_key, 0, sizeof(s_key));   /* 清除派生密钥 */
}

static const os_module_t s_mod_keymgr = {
    .name      = "keymgr",
    .page_id   = OS_PAGE_PLACEHOLDER_SECURE,
    .on_enter  = km_enter,
    .on_exit   = km_exit,
    .render    = km_render,
    .action    = km_action,
    .touch     = km_touch,
    .fullscreen = true,
};

void os_page_keymanager_register(void) { os_register(&s_mod_keymgr); }