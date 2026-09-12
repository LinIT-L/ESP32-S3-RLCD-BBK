/**
 * page_calculator.c — 科学计算器 页面模块.
 *
 * 自绘网格按钮 (6 行 x 5 列, ASCII 全字符集, 屏幕仅显示中英文字库支持的字符):
 *   - 主页面: AC/DEL/括号/运算符/数字/常量(pi,e,Ans)/(!)(=)/sqrt
 *   - 科学页: sin cos tan sqrt ln log log2 exp abs asin acos atan sinh cosh tanh floor ceil
 *   - FUNC 键(FN) 在两页间切换; 触碰可点, 手柄方向键+CONFIRM 可导航.
 * 计算: 按 "=" 或 CONFIRM 调到结果时调用 calc_eval(expr, ans, &res).
 *   ÷->'/' ×->'*' ; π 记为 'p'(引擎单字节常量); 函数插入 "sin(" 等, 由用户补右括号.
 *   calc_eval 返回 -1 显示"语法错", 返回 -2 显示"数学错".
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "calc_engine.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define S_W 400
#define S_H 300

/* ---- 网格几何 (经典 4 列棋盘, 共线不做每格独立框) ---- */
#define COLS      4
#define ROWS      6
#define COL_W     100
#define ROW_H     28              /* 压缩行高, 让出底部与键上方空间给显示区 */
#define GAP       0
#define GRID_X    0
#define GRID_Y    130             /* 键盘顶 (显示区分隔线在 GRID_Y-8) */

typedef enum {
    KT_PUSH,     /* 追加文本 (ins) */
    KT_AC,       /* 全清 */
    KT_DEL,      /* 退格 */
    KT_EQ,       /* 求值 */
    KT_FN,       /* 切换科学页 */
} ck_kind_t;

typedef struct { const char *face; const char *ins; ck_kind_t kind; } ckey_t;

/* 主页面按键 (face=中文显示, ins=插入引擎表达式, 均为 ASCII).
 * 经典布局: 左上清空, 右上括号, 右侧运算符 ÷ × - +, 数字 9→0 递减, 底行功能键. */
static const ckey_t s_main[ROWS][COLS] = {
    {{"\xe6\xb8\x85\xe7\xa9\xba","",KT_AC},{"(","(",KT_PUSH},{")",")",KT_PUSH},{"\xc3\xb7","/",KT_PUSH}},                 /* 清空 ( ) ÷ */
    {{"7","7",KT_PUSH},{"8","8",KT_PUSH},{"9","9",KT_PUSH},{"\xc3\x97","*",KT_PUSH}},                 /* 7 8 9 × */
    {{"4","4",KT_PUSH},{"5","5",KT_PUSH},{"6","6",KT_PUSH},{"\xe2\x88\x92","-",KT_PUSH}},             /* 4 5 6 − */
    {{"1","1",KT_PUSH},{"2","2",KT_PUSH},{"3","3",KT_PUSH},{"+","+",KT_PUSH}},                        /* 1 2 3 + */
    {{"0","0",KT_PUSH},{".",".",KT_PUSH},{"pi","p",KT_PUSH},{"e","e",KT_PUSH}},                        /* 0 . π e */
    {{"\xe5\x87\xbd\xe6\x95\xb0","",KT_FN},{"\xe9\x80\x80\xe6\xa0\xbc","",KT_DEL},{"Ans","Ans",KT_PUSH},{"=","",KT_EQ}}, /* 函数 退格 Ans = */
};

/* 科学页按键 (中文 face, ASCII ins) */
static const ckey_t s_sci[ROWS][COLS] = {
    {{"\xe5\x87\xbd\xe6\x95\xb0","",KT_FN},{"\xe6\xad\xa3\xe5\xbc\xa6","sin(",KT_PUSH},{"\xe4\xbd\x99\xe5\xbc\xa6","cos(",KT_PUSH},{"\xe6\xad\xa3\xe5\x88\x87","tan(",KT_PUSH}}, /* 函数/正弦/余弦/正切 */
    {{"\xe5\x8f\x8d\xe6\xad\xa3\xe5\xbc\xa6","asin(",KT_PUSH},{"\xe5\x8f\x8d\xe4\xbd\x99\xe5\xbc\xa6","acos(",KT_PUSH},{"\xe5\x8f\x8d\xe6\xad\xa3\xe5\x88\x87","atan(",KT_PUSH},{"\xe5\xbc\x80\xe6\x96\xb9","sqrt(",KT_PUSH}}, /* 反正弦/反余弦/反正切/开方 */
    {{"\xe5\x8f\x8c\xe6\xad\xa3","sinh(",KT_PUSH},{"\xe5\x8f\x8c\xe4\xbd\x99","cosh(",KT_PUSH},{"\xe5\x8f\x8c\xe6\xad\xa3\xe5\x88\x87","tanh(",KT_PUSH},{"\xe6\x8c\x87\xe6\x95\xb0","exp(",KT_PUSH}}, /* 双正/双余/双切/指数 */
    {{"\xe8\x87\xaa\xe7\x84\xb6\xe5\xaf\xb9\xe6\x95\xb0","ln(",KT_PUSH},{"\xe5\xb8\xb8\xe7\x94\xa8\xe5\xaf\xb9\xe6\x95\xb0","log(",KT_PUSH},{"\xe5\xba\x95\xe4\xb8\x802","log2(",KT_PUSH},{"\xe7\xbb\x9d\xe5\xaf\xb9\xe5\x80\xbc","abs(",KT_PUSH}}, /* 自然对数/常用对数/底数2/绝对值 */
    {{"\xe5\x8f\x96\xe6\x95\xb4","floor(",KT_PUSH},{"\xe5\x90\x91\xe4\xb8\x8a\xe5\x8f\x96\xe6\x95\xb4","ceil(",KT_PUSH},{"\xe9\x98\xb6\xe4\xb9\x98","!",KT_PUSH},{"pi","p",KT_PUSH}}, /* 取整/向上取整/阶乘/pi */
    {{"e","e",KT_PUSH},{"Ans","Ans",KT_PUSH},{"=","",KT_EQ},{"\xe8\xbf\x94\xe5\x9b\x9e","",KT_FN}},     /* 返回主页 */
};

/* ---- 状态 ---- */
static char   s_expr[96];      /* 当前输入表达式 (引擎语法, ASCII) */
static int    s_len;
static double s_ans;           /* 上次结果 */
static bool   s_has_ans;
static bool   s_show_res;      /* '='后显示结果 */
static char   s_res[48];       /* 结果文本 */
static char   s_last_expr[80]; /* 供 "expr = res" 顶行显示 */
static bool   s_show_err;
static char   s_err[24];
static bool   s_func_page;     /* false=主页面 true=科学页 */
static int    s_cursor_r, s_cursor_c;
static int    s_press_r = -1, s_press_c = -1;

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }

static void fmt_num(double v, char *b, int n) {
    snprintf(b, n, "%.10g", v);
}

/* 单词级渲染: 单字母 'p'/'P'(pi) -> "pi"; 其它原样. 全程 ASCII, 避免字库缺字. */
static void render_expr(char *out, int maxw, const char *src) {
    int oi = 0;
    const char *p = src;
    while (*p && oi < maxw - 1) {
        if (isalpha((unsigned char)*p)) {
            int l = 0; while (p[l] && isalpha((unsigned char)p[l])) l++;
            if (l == 1 && (p[0] == 'p' || p[0] == 'P')) {
                if (oi + 2 < maxw) { out[oi++] = 'p'; out[oi++] = 'i'; }
            } else if (l == 1 && (p[0] == 'e' || p[0] == 'E')) {
                if (oi + 1 < maxw) out[oi++] = 'e';
            } else {
                for (int k = 0; k < l && oi < maxw - 1; k++) out[oi++] = p[k];
            }
            p += l;
        } else {
            if (oi < maxw - 1) out[oi++] = *p;
            p++;
        }
    }
    out[oi] = 0;
}

static void cell_rect(int r, int c, int *x, int *y, int *w, int *h) {
    *x = GRID_X + c * COL_W;
    *y = GRID_Y + r * ROW_H;
    *w = COL_W;
    *h = ROW_H;
}

/* 绘制算符大符号 (÷ × − +), 以格子中心 (cx,cy) 为基准 / 24px 方块内.
 * 字库不含这些符号 (2 字节 UTF-8 在 draw_text 中被跳过), 故手绘像素. */
static void draw_opsym(st7305_handle_t *lcd, int cx, int cy, char mode, bool white) {
    st7305_color_t c = white ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    const int s = 6;
    if (mode == 'p') {                            /* + */
        draw_hline(lcd, cx - s, cx + s, cy, c);
        draw_vline(lcd, cx, cy - s, cy + s, c);
    } else if (mode == 'm') {                     /* − */
        draw_hline(lcd, cx - s, cx + s, cy, c);
    } else if (mode == 't') {                     /* × (两条斜线, 2px 粗) */
        for (int k = -s; k <= s; k++) {
            st7305_draw_pixel(lcd, cx + k,     cy + k, c);
            st7305_draw_pixel(lcd, cx + k + 1, cy + k, c);
            st7305_draw_pixel(lcd, cx + k,     cy - k, c);
            st7305_draw_pixel(lcd, cx + k + 1, cy - k, c);
        }
    } else if (mode == 'd') {                     /* ÷ (横线 + 上下点) */
        draw_hline(lcd, cx - s, cx + s, cy, c);
        fill_rect(lcd, cx - 1, cy - s - 2, cx + 1, cy - s, c);
        fill_rect(lcd, cx - 1, cy + s,     cx + 1, cy + s + 2, c);
    }
}

/* 绘制一格: 不画独立框 (由 render 画棋盘共线), 仅画选中/按下黑底与文字.
 * 文字按 24px 高垂直居中且强制不超出格宽; 算符符号单独手绘居中. */
static void draw_cell(st7305_handle_t *lcd, int r, int c, bool cur, bool pressed) {
    const ckey_t *k = (s_func_page ? &s_sci[r][c] : &s_main[r][c]);
    int x, y, w, h; cell_rect(r, c, &x, &y, &w, &h);
    bool touchdev = input_has_touch();
    bool active = touchdev ? pressed : cur;
    if (active) fill_rect(lcd, x + 1, y + 1, x + w - 2, y + h - 2, ST7305_COLOR_BLACK);
    /* 识别算符符号 ÷(C3 B7) ×(C3 97) −(E2 88 92) +(2B) */
    const unsigned char *f = (const unsigned char *)k->face;
    char mode = 0;
    if      (f[0] == 0xC3 && f[1] == 0xB7)                    mode = 'd';
    else if (f[0] == 0xC3 && f[1] == 0x97)                    mode = 't';
    else if (f[0] == 0xE2 && f[1] == 0x88 && f[2] == 0x92)    mode = 'm';
    else if (f[0] == '+')                                     mode = 'p';
    if (mode) { draw_opsym(lcd, x + w / 2, y + h / 2, mode, active); return; }
    /* draw_text 的 ASCII 与汉字字形高度均为 24px (draw_ascii/draw_zh), 统一按 24 垂直居中 */
    int fh = 24;
    int fw = text_width(k->face);
    if (fw > w - 8) fw = w - 8;               /* 防越框 */
    int tx = x + (w - fw) / 2;
    if (tx < x + 4) tx = x + 4;
    int ty = y + (h - fh) / 2;
    draw_text(lcd, tx, ty, k->face, active);
}

/* 按钮动作 */
static void calc_do(ui_ctx_t *ctx, const ckey_t *k, bool is_operator) {
    switch (k->kind) {
    case KT_AC:
        s_len = 0; s_expr[0] = 0;
        s_show_res = false; s_show_err = false;
        s_last_expr[0] = 0;
        redraw(ctx);
        break;
    case KT_DEL:
        if (s_show_res || s_show_err) { /* 退格当作开始新输入 */
            s_show_res = false; s_show_err = false;
            s_len = 0; s_expr[0] = 0;
        } else if (s_len > 0) {
            s_expr[--s_len] = 0;
        }
        redraw(ctx);
        break;
    case KT_EQ: {
        if (s_len == 0) { redraw(ctx); break; }
        double res = 0;
        int rc = calc_eval(s_expr, s_has_ans ? s_ans : 0, &res);
        if (rc == 0) {
            snprintf(s_last_expr, sizeof(s_last_expr), "%s", s_expr);
            s_ans = res; s_has_ans = true;
            fmt_num(res, s_res, sizeof(s_res));
            s_show_res = true; s_show_err = false;
            s_len = 0; s_expr[0] = 0;
        } else {
            s_err[0] = 0;
            strcpy(s_err, (rc == -1) ? "语法错" : "数学错");
            s_show_err = true; s_show_res = false;
        }
        redraw(ctx);
        break;
    }
    case KT_FN:
        s_func_page = !s_func_page;
        s_cursor_r = 0; s_cursor_c = 0;
        redraw(ctx);
        break;
    case KT_PUSH: {
        const char *s = k->ins;
        int ilen = (int)strlen(s);
        if (s_len + ilen >= (int)sizeof(s_expr)) { redraw(ctx); break; }
        if (s_show_res) {
            if (is_operator) {            /* 运算符/函数: 用上次结果续算 */
                s_len = 0; fmt_num(s_ans, s_expr, sizeof(s_expr));
                s_len = (int)strlen(s_expr);
            } else {                       /* 数字: 重新开始 */
                s_len = 0; s_expr[0] = 0;
            }
            s_show_res = false;
        }
        if (s_show_err) { s_show_err = false; s_len = 0; s_expr[0] = 0; }
        memcpy(s_expr + s_len, s, (size_t)ilen);
        s_len += ilen;
        s_expr[s_len] = 0;
        redraw(ctx);
        break;
    }
    }
}

static void cell_action(ui_ctx_t *ctx, int r, int c) {
    const ckey_t *k = (s_func_page ? &s_sci[r][c] : &s_main[r][c]);
    bool is_op = !(k->kind == KT_PUSH) ? false :
        (*k->ins && (strchr("+-*/^%!()", *k->ins) || isalpha((unsigned char)*k->ins)));
    calc_do(ctx, k, is_op);
}

/* ---- 渲染 ---- */
static void calc_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 顶行: Ans 或 求值式 =  (右对齐) */
    char line1[100];
    if (s_show_res) {
        char pretty[80];
        render_expr(pretty, (int)sizeof(pretty), s_last_expr);
        snprintf(line1, sizeof(line1), "%s =", pretty);
    } else if (s_has_ans) {
        char av[48];
        fmt_num(s_ans, av, sizeof(av));
        snprintf(line1, sizeof(line1), "Ans = %s", av);
    } else {
        line1[0] = 0;
    }
    if (line1[0]) {
        int lw = text_width(line1);
        draw_text(lcd, S_W - 8 - lw, 38, line1, false);
    }

    /* 第二行: 当前输入 (右对齐, 编辑在末端所以显示尾部) */
    if (s_show_res) {
        int lw = text_width(s_res);
        draw_text(lcd, S_W - 8 - lw, 70, s_res, false);
    } else if (s_show_err) {
        int lw = text_width(s_err);
        draw_text(lcd, S_W - 8 - lw, 70, s_err, false);
    } else {
        char pretty[110];
        render_expr(pretty, (int)sizeof(pretty), s_expr);
        int len = (int)strlen(pretty);
        const char *show = pretty;
        int maxc = 32;
        if (len > maxc) show = pretty + (len - maxc);
        int lw = text_width(show);
        draw_text(lcd, S_W - 8 - lw, 70, show, false);
    }

    /* 分隔线 (显示区与键盘之间): 与键盘顶 GRID_Y 对齐, 与竖网格线相接 */
    draw_hline(lcd, 0, S_W - 1, GRID_Y, ST7305_COLOR_BLACK);
    if (s_func_page) draw_text(lcd, S_W - 32, 28, "FN", true);

    /* 棋盘共线网格: 只画键与键之间的横/竖分割线, 不加外框
     * (顶部由显示区分隔线担当, 避免"清空上方/函数下方"出现多余框线) */
    int gx0 = GRID_X, gy0 = GRID_Y, gx1 = GRID_X + COLS * COL_W - 1, gy1 = GRID_Y + ROWS * ROW_H - 1;
    for (int c = 1; c < COLS; c++) draw_vline(lcd, gx0 + c * COL_W, gy0, gy1, ST7305_COLOR_BLACK);
    for (int r = 1; r < ROWS; r++) draw_hline(lcd, gx0, gx1, gy0 + r * ROW_H, ST7305_COLOR_BLACK);

    /* 按键只是棋盘上的格子 (文字+选中), 不再各自描框 */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            draw_cell(lcd, r, c,
                      (s_cursor_r == r && s_cursor_c == c),
                      (s_press_r == r && s_press_c == c));
}

/* ---- 触摸 ---- */
static bool calc_touch(ui_ctx_t *ctx, int x, int y) {
    if (x < 0 || y < 0) return false;
    if (y < GRID_Y || y >= GRID_Y + ROWS * ROW_H) return false;
    int c = x / COL_W;
    int r = (y - GRID_Y) / ROW_H;
    if (c < 0) c = 0;
    if (c > COLS - 1) c = COLS - 1;
    if (r < 0) r = 0;
    if (r > ROWS - 1) r = ROWS - 1;
    s_press_r = s_cursor_r = r;
    s_press_c = s_cursor_c = c;
    cell_action(ctx, r, c);
    return true;
}

/* 每帧: 松手恢复按键原色 */
static void calc_poll(ui_ctx_t *ctx) {
    static bool prev_down = false;
    bool down = input_get_touch_pos(NULL, NULL);
    if (!down && prev_down) {
        if (s_press_r >= 0 || s_press_c >= 0) { s_press_r = s_press_c = -1; redraw(ctx); }
    }
    prev_down = down;
}

/* ---- 按键 ---- */
static void calc_action(ui_ctx_t *ctx, os_action_t a) {
    if (a == OS_ACTION_UP)    { if (s_cursor_r > 0) s_cursor_r--; redraw(ctx); return; }
    if (a == OS_ACTION_DOWN)  { if (s_cursor_r < ROWS - 1) s_cursor_r++; redraw(ctx); return; }
    if (a == OS_ACTION_LEFT)  { if (s_cursor_c > 0) s_cursor_c--; redraw(ctx); return; }
    if (a == OS_ACTION_RIGHT) { if (s_cursor_c < COLS - 1) s_cursor_c++; redraw(ctx); return; }
    if (a == OS_ACTION_CONFIRM) { cell_action(ctx, s_cursor_r, s_cursor_c); return; }
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { os_pop(ctx); return; }
}

/* ---- 进入/退出 ---- */
static void calc_enter(ui_ctx_t *ctx) {
    s_len = 0; s_expr[0] = 0;
    s_ans = 0; s_has_ans = false;
    s_show_res = false; s_show_err = false;
    s_res[0] = 0; s_last_expr[0] = 0;
    s_func_page = false;
    s_cursor_r = 0; s_cursor_c = 0;
    ctx->needs_redraw = true;
}

static const os_module_t s_mod_calc = {
    .name      = "calc",
    .page_id   = OS_PAGE_CALC,
    .on_enter  = calc_enter,
    .render    = calc_render,
    .action    = calc_action,
    .touch     = calc_touch,
    .poll      = calc_poll,
    .fullscreen = false,
};

void os_page_calculator_register(void) { os_register(&s_mod_calc); }