/**
 * page_calculator.c — 科学计算器 页面模块 (v5, 2026-10-30 UI 重绘).
 *
 * 视觉基准: linit 提供的黑白效果图. 纯黑白: 白底 + 黑边框 + 黑字,
 * 无灰阶/渐变/阴影/拟物/圆形按钮. 字体沿用本机原有字库 (draw_text / ui_text16),
 * 不另造字号.
 *
 * 布局 (400×300, 本页隐藏系统状态栏 → 整屏可用):
 *   显示框   x 5..394, y 19..72, 圆角 5, 边框 2px, 数字右对齐, 右侧留 12px
 *   键盘     7 列 × 5 行, 键 52×34, 横缝 4, 竖缝 7
 *            行 y = 83 / 124 / 165 / 206 / 247, 末行底 280
 *
 * 键盘 (顺序与参考图一致, 不得增删):
 *   Rad Deg Inv ( ) %  AC
 *   x!  sin cos 7 8 9  ÷
 *   π   tan log 4 5 6  ×
 *   e   ln  √   1 2 3  −
 *   EXP Ans xʸ  0 . =  +
 *
 * 默认状态: 35 个键全部白底黑框黑字, 视觉权重一致; 只有被按住的那一个变黑底白字.
 * 当前制式 (Rad/Deg) 与 Inv 状态画在显示框左上角, 不靠按键反白.
 *
 * 交互补充 (参考图没有退格键, 但输入必须有):
 *   点 AC → 全清;  长按 AC 600ms → 退一格.
 *
 * 计算: calc_eval(); Rad/Deg 用 calc_set_deg() 交给引擎换算;
 *       Inv 打开时 sin/cos/tan 变 asin/acos/atan, ln→e^x, log→10^x, √→x².
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "calc_engine.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "esp_timer.h"

#define S_W 400
#define S_H 300

/* ★★★ 2026-11-01 布局重排: 四边留白统一为 PAD ★★★
 *   改前: 左右 5 / 7(不对称), 上下各 19 —— 视觉上"两侧贴边、上下发空"。
 *   改后: 上=下=左=右=6。
 *     横向 6 + 7×52 + 6×4 = 394 ⇒ 右侧 400-394 = 6  ✓
 *     纵向 6 + 54(显示框) + 11(间隙) + 5×39+4×7(按键 223) = 294 ⇒ 底部 6  ✓
 *   按键高度顺带从 34 抬到 39, 更好点。
 *   净效果: 内容总高 294 < 屏高 300 ⇒ 当前 max_scroll=0(无需滚动);
 *           一旦把 KEY_H 调大导致内容超高, 下面那套滚动会**自动**接管。 */
#define PAD      6

/* ---- 显示框 ---- */
#define DISP_X   PAD
#define DISP_Y   PAD
#define DISP_W   (S_W - PAD * 2)        /* 388 */
#define DISP_H   66                     /* ★ 2026-11-01: 54→66, 吃下按键行距腾出的 12px */
#define DISP_R   5
#define DISP_PAD 12

/* ---- 键盘网格 ---- */
#define COLS     7
#define ROWS     5
#define KEY_W    52
#define KEY_H    39
#define GAP_X    4
#define GAP_Y    4                      /* ★ 2026-11-01: 7→4, 与 GAP_X 一致, 上下更紧 */
#define KEY_X0   PAD                     /* 6 */
#define KEY_Y0   (PAD + DISP_H + 11)     /* 83 */
#define KEY_R    5
#define BORDER_W 2

/* 内容总高(最后一行按键底 + 1) —— 滚动上限据此算, 不写死 */
#define CONTENT_H (KEY_Y0 + (ROWS - 1) * (KEY_H + GAP_Y) + KEY_H)

typedef enum {
    KT_PUSH,   /* 追加 ins */
    KT_AC,     /* 全清 (长按退格) */
    KT_EQ,     /* 求值 */
    KT_RAD,    /* 弧度制 */
    KT_DEG,    /* 角度制 */
    KT_INV,    /* 反函数开关 */
} ck_kind_t;

/* sym: 0=用字体画 face; p=+ m=− t=× d=÷ r=√ P=π i== */
typedef struct { const char *face; const char *ins; ck_kind_t kind; char sym; } ckey_t;

static const ckey_t s_keys[ROWS][COLS] = {
/*  Rad           Deg           Inv           (            )            %            AC   */
  {{"Rad","",KT_RAD,0},{"Deg","",KT_DEG,0},{"Inv","",KT_INV,0},
   {"(","(",KT_PUSH,0},{")",")",KT_PUSH,0},{"%","%",KT_PUSH,0},{"AC","",KT_AC,0}},
/*  x!            sin           cos           7            8            9            ÷    */
  {{"x!","!",KT_PUSH,0},{"sin","sin(",KT_PUSH,0},{"cos","cos(",KT_PUSH,0},
   {"7","7",KT_PUSH,0},{"8","8",KT_PUSH,0},{"9","9",KT_PUSH,0},{"","/",KT_PUSH,'d'}},
/*  π             tan           log           4            5            6            ×    */
  {{"","p",KT_PUSH,'P'},{"tan","tan(",KT_PUSH,0},{"log","log(",KT_PUSH,0},
   {"4","4",KT_PUSH,0},{"5","5",KT_PUSH,0},{"6","6",KT_PUSH,0},{"","*",KT_PUSH,'t'}},
/*  e             ln            √             1            2            3            −    */
  {{"e","e",KT_PUSH,0},{"ln","ln(",KT_PUSH,0},{"","sqrt(",KT_PUSH,'r'},
   {"1","1",KT_PUSH,0},{"2","2",KT_PUSH,0},{"3","3",KT_PUSH,0},{"","-",KT_PUSH,'m'}},
/*  EXP           Ans           xʸ            0            .            =            +    */
  {{"EXP","*10^(",KT_PUSH,0},{"Ans","Ans",KT_PUSH,0},{"","^",KT_PUSH,0},
   {"0","0",KT_PUSH,0},{".",".",KT_PUSH,0},{"","",KT_EQ,'i'},{"","+",KT_PUSH,'p'}},
};

/* ---- 状态 ---- */
static char   s_expr[96];
static int    s_len;
static double s_ans;
static bool   s_has_ans;
static bool   s_show_res;
static char   s_res[48];
static char   s_last_expr[80];
static bool   s_show_err;
static bool   s_deg;
static bool   s_inv;
static int    s_cursor_r, s_cursor_c;
static int    s_press_r = -1, s_press_c = -1;
static uint32_t s_press_ms;
static bool   s_long_done;

/* ---- 纵向滚动 (2026-11-01) ----
 * 内容总高 CONTENT_H 目前 294 < 300 ⇒ max=0, 不会动。
 * 把 KEY_H 调大让内容超过一屏时, 这里会自动给出可滚动范围, 手指上下拖即可。
 * 拖动与"按按键"互斥: 位移超过 8px 就判定为拖动, 取消这次按键按下, 抬手不触发。 */
#define DRAG_GATE_PX 8
static int  s_scroll;          /* 当前偏移 0..s_max_scroll */
static int  s_max_scroll;      /* 0 = 内容装得下, 不允许滚 */
static int  s_drag_y0 = -1;    /* 本次按下的起点 y (-1 = 没按住) */
static int  s_drag_last;       /* 上一帧 y, 用于增量滚动 */
static bool s_dragging;        /* 已进入拖动 */
static bool s_was_down;        /* 上一帧是否按住 —— 用"上升沿"认按下, 不靠 touch 回调时序 */

static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void scroll_clamp(void)
{
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_max_scroll) s_scroll = s_max_scroll;
}

/* 方向键把光标移到看不见的行时, 自动滚一屏 —— 保证"选中的格一定在屏上"。
 * 当前内容装得下(max_scroll=0) 时这里直接返回, 行为与旧版完全一致。 */
static void ensure_cursor_visible(void)
{
    if (s_max_scroll <= 0) return;
    int top = KEY_Y0 + s_cursor_r * (KEY_H + GAP_Y);
    int ky  = top - s_scroll;
    if (ky < 0)                       s_scroll = top;
    else if (ky + KEY_H > S_H)        s_scroll = top + KEY_H - S_H;
    scroll_clamp();
}

/* ---------------- 圆角矩形 ---------------- */
typedef struct { int l, r; bool ok; } rr_span_t;

static rr_span_t rr_span(int y, int x0, int y0, int x1, int y1, int r)
{
    rr_span_t sp = { 0, 0, false };
    if (y < y0 || y > y1 || r < 0) return sp;
    if (y < y0 + r) {
        int dy = (y0 + r) - y;
        int dx = 0, v = r * r - dy * dy;
        while ((dx + 1) * (dx + 1) <= v) dx++;
        sp.l = x0 + (r - dx);
        sp.r = x1 - (r - dx);
    } else if (y > y1 - r) {
        int dy = y - (y1 - r);
        int dx = 0, v = r * r - dy * dy;
        while ((dx + 1) * (dx + 1) <= v) dx++;
        sp.l = x0 + (r - dx);
        sp.r = x1 - (r - dx);
    } else {
        sp.l = x0;
        sp.r = x1;
    }
    sp.ok = (sp.l <= sp.r);
    return sp;
}

/* 圆角矩形: filled=true 整块填色; 否则画 bw 像素边框.
 * 边框 = 外轮廓跨度 − 内轮廓跨度, 角与直边同一套算法, 圆弧自然接上. */
static void round_box(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                      int r, int bw, bool filled, st7305_color_t c)
{
    if (r < 1) r = 1;
    if (bw < 1) bw = 1;
    for (int y = y0; y <= y1; y++) {
        rr_span_t o = rr_span(y, x0, y0, x1, y1, r);
        if (!o.ok) continue;
        if (filled) { fill_rect(lcd, o.l, y, o.r, y, c); continue; }
        rr_span_t in = rr_span(y, x0 + bw, y0 + bw, x1 - bw, y1 - bw, r - bw);
        if (!in.ok) { fill_rect(lcd, o.l, y, o.r, y, c); continue; }
        if (o.l <= in.l - 1) fill_rect(lcd, o.l, y, in.l - 1, y, c);
        if (in.r + 1 <= o.r) fill_rect(lcd, in.r + 1, y, o.r, y, c);
    }
}

/* ---------------- 手绘符号 ---------------- */
static void px2(st7305_handle_t *lcd, int x, int y, st7305_color_t c)
{
    st7305_draw_pixel(lcd, x, y, c);
    st7305_draw_pixel(lcd, x + 1, y, c);
    st7305_draw_pixel(lcd, x, y + 1, c);
    st7305_draw_pixel(lcd, x + 1, y + 1, c);
}

static void draw_sym(st7305_handle_t *lcd, int cx, int cy, char sym, st7305_color_t c)
{
    const int s = 7;
    switch (sym) {
    case 'p':                                   /* + */
        fill_rect(lcd, cx - s, cy - 1, cx + s, cy, c);
        fill_rect(lcd, cx - 1, cy - s, cx, cy + s, c);
        break;
    case 'm':                                   /* − */
        fill_rect(lcd, cx - s, cy - 1, cx + s, cy, c);
        break;
    case 't':                                   /* × */
        for (int k = -s; k <= s; k++) {
            px2(lcd, cx + k, cy + k, c);
            px2(lcd, cx + k, cy - k, c);
        }
        break;
    case 'd':                                   /* ÷ */
        fill_rect(lcd, cx - s, cy - 1, cx + s, cy, c);
        fill_rect(lcd, cx - 2, cy - s - 1, cx + 1, cy - s + 2, c);
        fill_rect(lcd, cx - 2, cy + s - 2, cx + 1, cy + s + 1, c);
        break;
    case 'i':                                   /* = */
        fill_rect(lcd, cx - 7, cy - 5, cx + 7, cy - 3, c);
        fill_rect(lcd, cx - 7, cy + 3, cx + 7, cy + 5, c);
        break;
    case 'r': {                                 /* √ */
        int sx = cx - 7, sy = cy + 1;
        draw_vline(lcd, sx,     sy,     sy + 3, c);
        draw_vline(lcd, sx + 1, sy + 1, sy + 4, c);
        for (int i = 0; i <= 5; i++) {
            st7305_draw_pixel(lcd, sx + 2 + i, sy + 3 - i, c);
            st7305_draw_pixel(lcd, sx + 3 + i, sy + 3 - i, c);
        }
        fill_rect(lcd, cx + 1, cy - 7, cx + 7, cy - 5, c);
        break;
    }
    case 'P':                                   /* π */
        fill_rect(lcd, cx - 7, cy - 6, cx + 7, cy - 4, c);
        fill_rect(lcd, cx - 5, cy - 6, cx - 3, cy + 5, c);
        fill_rect(lcd, cx + 3, cy - 6, cx + 5, cy + 5, c);
        break;
    default: break;
    }
}

/* ---------------- 表达式美化 ---------------- */
static void render_expr(char *out, int maxc, const char *src)
{
    int oi = 0;
    const char *p = src;
    while (*p && oi < maxc - 1) {
        if (isalpha((unsigned char)*p)) {
            int l = 0; while (p[l] && isalpha((unsigned char)p[l])) l++;
            if (l == 1 && (p[0] == 'p' || p[0] == 'P')) {
                if (oi + 2 < maxc) { out[oi++] = 'p'; out[oi++] = 'i'; }
            } else {
                for (int k = 0; k < l && oi < maxc - 1; k++) out[oi++] = p[k];
            }
            p += l;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = 0;
}

static void fmt_num(double v, char *b, int n) { snprintf(b, n, "%.10g", v); }

/* ---------------- 单元格几何 ----------------
 * 返回的是**屏幕坐标**, 已经减掉滚动偏移 —— 绘制和命中判定共用同一份,
 * 所以拖动后"看到的格"和"点到的格"永远一致。 */
static void cell_rect(int r, int c, int *x, int *y)
{
    *x = KEY_X0 + c * (KEY_W + GAP_X);
    *y = KEY_Y0 + r * (KEY_H + GAP_Y) - s_scroll;
}

static const char *key_ins(const ckey_t *k)
{
    if (!s_inv || k->kind != KT_PUSH) return k->ins;
    if (!strcmp(k->ins, "sin("))  return "asin(";
    if (!strcmp(k->ins, "cos("))  return "acos(";
    if (!strcmp(k->ins, "tan("))  return "atan(";
    if (!strcmp(k->ins, "ln("))   return "e^(";
    if (!strcmp(k->ins, "log("))  return "10^(";
    if (!strcmp(k->ins, "sqrt(")) return "^2";
    return k->ins;
}

static const char *key_face(const ckey_t *k)
{
    if (!s_inv) return k->face;
    if (!strcmp(k->ins, "sin("))  return "sin-1";
    if (!strcmp(k->ins, "cos("))  return "cos-1";
    if (!strcmp(k->ins, "tan("))  return "tan-1";
    if (!strcmp(k->ins, "ln("))   return "e^x";
    if (!strcmp(k->ins, "log("))  return "10^x";
    if (!strcmp(k->ins, "sqrt(")) return "x^2";
    return k->face;
}

/* ---------------- 画一格 ---------------- */
static void draw_cell(st7305_handle_t *lcd, int r, int c, bool cur, bool pressed)
{
    const ckey_t *k = &s_keys[r][c];
    int x, y; cell_rect(r, c, &x, &y);
    int x1 = x + KEY_W - 1, y1 = y + KEY_H - 1;

    bool touchdev = input_has_touch();
    bool active = touchdev ? pressed : cur;

    fill_rect(lcd, x, y, x1, y1, ST7305_COLOR_WHITE);
    round_box(lcd, x, y, x1, y1, KEY_R, BORDER_W, active, ST7305_COLOR_BLACK);

    bool inv_txt = active;
    int cx = x + KEY_W / 2, cy = y + KEY_H / 2;

    if (k->sym) { draw_sym(lcd, cx, cy, k->sym,
                           active ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK); return; }

    /* xʸ: x 正文字 + y 小字上标 */
    if (!strcmp(k->ins, "^")) {
        /* xʸ: y 用 8×12 小字, 抬高 6px 当上标; 抬太高会把下伸的尾巴裁掉 (变成 u) */
        ui_text16(lcd, cx - 11, cy - 8, "x", inv_txt);
        draw_ascii_small(lcd, cx + 1, cy - 9, 'y', inv_txt);
        return;
    }

    const char *face = key_face(k);
    if (!face[0]) return;

    /* 数字与小数点用 24px 正文字 (与原来计算器一致); 功能词用 16px,
     * 参考图里数字明显比 Rad/sin/log 这些词大一号. */
    if (isdigit((unsigned char)face[0]) || face[0] == '.') {
        int dw = text_width(face);
        draw_text(lcd, cx - dw / 2, cy - 12, face, inv_txt);
        return;
    }
    int w = ui_text16w(face);
    if (w > KEY_W - 8) {                     /* 反函数标签 (sin-1) 缩成小字 */
        int n = (int)strlen(face);
        int tx = cx - n * 4;
        for (int i = 0; i < n; i++)
            draw_ascii_small(lcd, tx + i * 8, cy - 6, face[i], inv_txt);
        return;
    }
    ui_text16(lcd, cx - w / 2, cy - 8, face, inv_txt);
}

/* ---------------- 显示框 ---------------- */
static void draw_display(st7305_handle_t *lcd)
{
    /* 显示框跟着内容一起滚, 这样"顶部留白"始终等于 PAD */
    int dy = DISP_Y - s_scroll;
    round_box(lcd, DISP_X, dy, DISP_X + DISP_W - 1, dy + DISP_H - 1,
              DISP_R, BORDER_W, false, ST7305_COLOR_BLACK);

    /* 左上角: 制式 (Rad/Deg) 与 Inv 状态 —— 固定在框顶, 与下面的数字彻底分开 */
    char tag[16];
    snprintf(tag, sizeof(tag), "%s%s", s_deg ? "Deg" : "Rad", s_inv ? " INV" : "");
    ui_text16(lcd, DISP_X + 8, dy + 4, tag, false);

    /* 中间偏下: 求值后显示原式 (16px 小字, 让位给下面的大数字)
     * 改前用 draw_text(24px) 贴在框底, 和大数字在垂直方向有 13px 重叠区,
     * 只靠"一个靠左一个靠右"遮着 —— 原式一长就撞上。现在三段分开:
     *   Rad/Deg  dy+4 (16px)  |  原式 dy+22 (16px)  |  大数字 dy+38 (24px) */
    if (s_show_res && s_last_expr[0]) {
        char pretty[80];
        render_expr(pretty, (int)sizeof(pretty), s_last_expr);
        char line[96];
        snprintf(line, sizeof(line), "%s =", pretty);
        int maxc = (DISP_W - 120) / 8;
        const char *p = line;
        if (maxc < 1) maxc = 1;
        if ((int)strlen(line) > maxc) p = line + strlen(line) - maxc;
        ui_text16(lcd, DISP_X + 8, dy + 22, p, false);
    }

    /* 主数字: 右对齐, 右侧留 12px
     * big 取 128: 表达式美化缓冲 pretty 最多 110 字节, 96 会让 -Wformat-truncation 报警 */
    char big[128];
    if (s_show_err)      snprintf(big, sizeof(big), "Error");
    else if (s_show_res) snprintf(big, sizeof(big), "%s", s_res);
    else if (s_len == 0) snprintf(big, sizeof(big), "0");
    else {
        char pretty[110];
        render_expr(pretty, (int)sizeof(pretty), s_expr);
        int maxc = (DISP_W - DISP_PAD * 2) / 8;
        if (maxc < 1) maxc = 1;
        int n = (int)strlen(pretty);
        snprintf(big, sizeof(big), "%s", (n > maxc) ? pretty + (n - maxc) : pretty);
    }
    int w = text_width(big);
    int bx = DISP_X + DISP_W - DISP_PAD - w;
    /* 大数字靠下: 66 高的框里放 38..62, 上方留给 Rad/Deg 与原式, 互不重叠 */
    int by = dy + 38;
    draw_text(lcd, bx, by, big, false);
}

/* ---------------- 输入/求值 ---------------- */
/* 表达式末尾是不是一个可以"被替换掉"的二元运算符。
 * 只认 + - * / ^ —— % 与 ! 是后缀运算符, 没有"替换"语义。 */
static bool tail_binop(char *out)
{
    if (s_len < 1) return false;
    char c = s_expr[s_len - 1];
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') {
        if (out) *out = c;
        return true;
    }
    return false;
}

/* 该位置上已经有小数点了吗 (只看当前这个数, 回退到上一个非数字字符) */
static bool cur_num_has_dot(void)
{
    int i = s_len;
    while (i > 0 && (isdigit((unsigned char)s_expr[i - 1]) || s_expr[i - 1] == '.')) i--;
    for (int j = i; j < s_len; j++)
        if (s_expr[j] == '.') return true;
    return false;
}

static void push_token(ui_ctx_t *ctx, const ckey_t *k, bool is_op)
{
    const char *s = key_ins(k);
    int ilen = (int)strlen(s);
    if (ilen == 0 || s_len + ilen >= (int)sizeof(s_expr)) { redraw(ctx); return; }

    if (s_show_res) {
        if (is_op) {
            fmt_num(s_ans, s_expr, sizeof(s_expr));
            s_len = (int)strlen(s_expr);
        } else {
            s_len = 0; s_expr[0] = 0;
        }
        s_show_res = false;
    }
    if (s_show_err) { s_show_err = false; s_len = 0; s_expr[0] = 0; }

    /* ★★★ 2026-11-01 修「运算符可以连按」★★★
     *
     *   根因: 原来这里只判长度, 然后 memcpy 无条件追加 ⇒ `5 + +` 直接拼成 "5++",
     *   引擎把它当 +(+…) 解析, 要么算出怪结果要么 Error。用户看到的"能连按"就是这个。
     *
     *   规则(只作用于二元运算符 + - * / ^):
     *     ① 末尾已是运算符, 又按**同一个** → 忽略          5 + +  ⇒ 5 +
     *     ② 末尾已是运算符, 按**不同**的 → 替换掉前一个    5 + *  ⇒ 5 *
     *     ③ 例外: 末尾是 + - * / ^ 且新按的是 `-` → 保留为**负号**追加
     *        这是 5 × -2 必须的; 而 5 - - 会在 ① 处被拦掉, 所以不会出现 "5--"
     *     ④ 末尾是运算符却按后缀 % / ! → 忽略(对运算符取百分号/阶乘无意义)
     *   小数点: 当前数已有 `.` 就忽略, 避免 5..3 这种。 */
    if (ilen == 1) {
        char c = s[0];
        bool new_bin  = (c == '+' || c == '-' || c == '*' || c == '/' || c == '^');
        bool new_post = (c == '%' || c == '!');
        char last = 0;
        if (tail_binop(&last)) {
            if (new_bin) {
                if (c == last) { redraw(ctx); return; }              /* ① 连按同一个 */
                if (c != '-') {                                      /* ② 换一个: 顶掉 */
                    /* 顶掉几个字符要考虑"末尾的 - 其实是负号"的情况:
                     *   5 -  按 + → 5+   (顶 1 个: 减号本身就是二元运算符)
                     *   5/-  按 + → 5+   (顶 2 个: / 和它后面的负号一起消失)
                     * 判据: 末尾是 '-', 且它前面也是一个运算符 ⇒ 这个 '-' 是负号。 */
                    int cut = 1;
                    if (last == '-' && s_len >= 2) {
                        char prev = s_expr[s_len - 2];
                        if (prev == '+' || prev == '-' || prev == '*' || prev == '/' || prev == '^')
                            cut = 2;
                    }
                    s_expr[s_len - cut] = c;
                    s_len = s_len - cut + 1;
                    s_expr[s_len] = 0;
                    redraw(ctx);
                    return;
                }
                /* ③ c=='-' 且 last!='-': 落到下面正常追加, 当作负号 */
            } else if (new_post) {
                redraw(ctx); return;                                 /* ④ 5 + % */
            }
        }
        if (c == '.' && cur_num_has_dot()) { redraw(ctx); return; }  /* 5..3 */
    }

    if (s_len == 0 && (!strcmp(s, "*10^(") || !strcmp(s, "^2"))) {
        memcpy(s_expr, "1", 1); s_len = 1;
    }
    memcpy(s_expr + s_len, s, (size_t)ilen);
    s_len += ilen;
    s_expr[s_len] = 0;
    redraw(ctx);
}

static void do_backspace(ui_ctx_t *ctx)
{
    if (s_show_res || s_show_err) {
        s_show_res = false; s_show_err = false;
        s_len = 0; s_expr[0] = 0;
        redraw(ctx); return;
    }
    if (s_len == 0) { redraw(ctx); return; }
    int i = s_len - 1;
    while (i > 0 && isalpha((unsigned char)s_expr[i - 1])) i--;
    if (i < s_len - 1 && s_expr[s_len - 1] == '(') s_len = i;
    else s_len--;
    s_expr[s_len] = 0;
    redraw(ctx);
}

static void do_equals(ui_ctx_t *ctx)
{
    if (s_len == 0) { redraw(ctx); return; }
    double res = 0;
    int rc = calc_eval(s_expr, s_has_ans ? s_ans : 0, &res);
    if (rc == 0) {
        snprintf(s_last_expr, sizeof(s_last_expr), "%.*s",
                 (int)sizeof(s_last_expr) - 1, s_expr);
        s_ans = res; s_has_ans = true;
        fmt_num(res, s_res, sizeof(s_res));
        s_show_res = true; s_show_err = false;
        s_len = 0; s_expr[0] = 0;
    } else {
        s_show_err = true; s_show_res = false;
    }
    redraw(ctx);
}

static void cell_action(ui_ctx_t *ctx, int r, int c)
{
    const ckey_t *k = &s_keys[r][c];
    switch (k->kind) {
    case KT_AC:  s_len = 0; s_expr[0] = 0; s_show_res = false; s_show_err = false;
                 s_last_expr[0] = 0; redraw(ctx); break;
    case KT_EQ:  do_equals(ctx); break;
    case KT_RAD: s_deg = false; calc_set_deg(false); redraw(ctx); break;
    case KT_DEG: s_deg = true;  calc_set_deg(true);  redraw(ctx); break;
    case KT_INV: s_inv = !s_inv; redraw(ctx); break;
    case KT_PUSH: {
        const char *ins = key_ins(k);
        bool is_op = ins[0] && (strchr("+-*/^%!()", ins[0]) ||
                                isalpha((unsigned char)ins[0]));
        push_token(ctx, k, is_op);
        break;
    }
    }
}

/* ---------------- 渲染 ---------------- */
static void calc_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    draw_display(lcd);

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x, y;
            cell_rect(r, c, &x, &y);
            if (y + KEY_H <= 0 || y >= S_H) continue;   /* 滚到屏外的整行不画 */
            draw_cell(lcd, r, c,
                      (s_cursor_r == r && s_cursor_c == c),
                      (s_press_r == r && s_press_c == c));
        }
    }
}

/* ---------------- 触摸 ---------------- */
static bool calc_touch(ui_ctx_t *ctx, int x, int y)
{
    if (x < 0 || y < 0) return false;
    /* 正在拖动: 这次触摸属于滚动手势, 不落到按键上 */
    if (s_dragging) return true;
    int r = -1, c = -1;
    /* 命中与绘制共用 cell_rect(): 逐格比较, 间隙不响应, 不会吸到邻格 */
    for (int rr = 0; rr < ROWS && r < 0; rr++) {
        for (int cc = 0; cc < COLS; cc++) {
            int kx, ky; cell_rect(rr, cc, &kx, &ky);
            if (x >= kx && x <= kx + KEY_W - 1 &&
                y >= ky && y <= ky + KEY_H - 1) { r = rr; c = cc; break; }
        }
    }
    if (r < 0) return false;    /* 点空白: 什么都不做, 拖动由 calc_poll 自己起头 */

    if (s_press_r != r || s_press_c != c) {
        s_press_r = r; s_press_c = c;
        s_press_ms = now_ms();
        s_long_done = false;
    }
    s_cursor_r = r; s_cursor_c = c;
    ctx->needs_redraw = true;
    return true;
}

static void calc_poll(ui_ctx_t *ctx)
{
    int tx = 0, ty = 0;
    bool down = input_get_touch_pos(&tx, &ty);
    uint32_t now = now_ms();

    /* ---- 纵向拖动 (2026-11-01) ----
     * 按住后位移超过 DRAG_GATE_PX 就判定为"滚动"而不是"点按键":
     *   ① 立刻清掉 s_press_r/c ⇒ 抬手时不会触发 cell_action, 不会误输入
     *   ② 之后按增量改 s_scroll, 松手停住
     * 只有 s_max_scroll > 0(内容确实超出一屏) 才进入拖动, 否则手感和原来完全一样。 */
    if (down) {
        if (!s_was_down) {                    /* 按下的上升沿: 以这里为起点, 不依赖 touch 回调时机 */
            s_drag_y0 = ty; s_drag_last = ty; s_dragging = false;
        } else if (!s_dragging && s_max_scroll > 0 &&
                   abs(ty - s_drag_y0) >= DRAG_GATE_PX) {
            s_dragging = true;
            s_press_r = s_press_c = -1;
            s_long_done = false;
        }
        if (s_dragging) {
            int d = ty - s_drag_last;
            if (d) {
                int old = s_scroll;
                s_scroll -= d;              /* 手指下滑 = 内容跟着下移 */
                scroll_clamp();
                s_drag_last = ty;
                if (s_scroll != old) redraw(ctx);
            }
        }
    } else {
        s_drag_y0 = -1;
        if (s_dragging) { s_dragging = false; redraw(ctx); }
    }
    s_was_down = down;

    if (s_press_r >= 0) {
        const ckey_t *k = &s_keys[s_press_r][s_press_c];
        if (down) {
            if (!s_long_done && k->kind == KT_AC && (now - s_press_ms) >= 600) {
                s_long_done = true;
                do_backspace(ctx);
            }
        } else {
            int r = s_press_r, c = s_press_c;
            bool was_long = s_long_done;
            s_press_r = s_press_c = -1;
            s_long_done = false;
            if (!was_long) cell_action(ctx, r, c);
            redraw(ctx);
        }
    }
}

/* ---------------- 按键 ---------------- */
static void calc_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_UP)      { if (s_cursor_r > 0) s_cursor_r--; ensure_cursor_visible(); redraw(ctx); return; }
    if (a == OS_ACTION_DOWN)    { if (s_cursor_r < ROWS - 1) s_cursor_r++; ensure_cursor_visible(); redraw(ctx); return; }
    if (a == OS_ACTION_LEFT)    { if (s_cursor_c > 0) s_cursor_c--; redraw(ctx); return; }
    if (a == OS_ACTION_RIGHT)   { if (s_cursor_c < COLS - 1) s_cursor_c++; redraw(ctx); return; }
    if (a == OS_ACTION_CONFIRM) { cell_action(ctx, s_cursor_r, s_cursor_c); return; }
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) { os_pop(ctx); return; }
}

/* ---------------- 进入 ---------------- */
static void calc_enter(ui_ctx_t *ctx)
{
    s_len = 0; s_expr[0] = 0;
    s_ans = 0; s_has_ans = false;
    s_show_res = false; s_show_err = false;
    s_res[0] = 0; s_last_expr[0] = 0;
    s_deg = false; calc_set_deg(false);
    s_inv = false;
    s_cursor_r = 0; s_cursor_c = 0;
    s_press_r = s_press_c = -1;
    /* 滚动: 内容装得下就锁死在 0, 拖不动(当前 KEY_H=39 正是这种); 变高才放开 */
    s_scroll = 0;
    s_max_scroll = (CONTENT_H > S_H) ? (CONTENT_H - S_H) : 0;
    s_drag_y0 = -1; s_drag_last = 0; s_dragging = false; s_was_down = false;
    ctx->needs_redraw = true;
}

static const os_module_t s_mod_calc = {
    .name       = "calc",
    .page_id    = OS_PAGE_CALC,
    .on_enter   = calc_enter,
    .render     = calc_render,
    .action     = calc_action,
    .touch      = calc_touch,
    .poll       = calc_poll,
    .fullscreen = true,        /* 计算器整屏排版, 状态栏让位 */
    .exit_confirm = true,
};

void os_page_calculator_register(void) { os_register(&s_mod_calc); }
