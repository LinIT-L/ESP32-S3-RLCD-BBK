/**
 * calc_engine.c — 表达式求值引擎 (自研, 轻量).
 * 分词 -> 调车场(shunting-yard)转后缀 -> 后缀求值, double.
 * 支持数字/科学计数、一元负、隐式乘法、pi/e/Ans、^ % ! 及科学函数.
 */
#include "calc_engine.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.7182818284590452354
#endif

/* 运算符/函数 id (输出类型用; 负数区间为运算符, 其余为函数) */
enum {
    T_NUM  = -11,          /* 数字/常量 */
    T_ADD  = -1, T_SUB = -2, T_MUL = -3, T_DIV = -4,
    T_MOD  = -5, T_POW = -6, T_FACT = -7, T_UNEG = -8,
    T_LP   = -9, T_RP   = -10,
    F_SIN=1,F_COS,F_TAN,F_ASIN,F_ACOS,F_ATAN,F_SINH,F_COSH,F_TANH,
    F_LN,F_LOG,F_LOG2,F_SQRT,F_EXP,F_ABS,F_FLOOR,F_CEIL
};

#define MAX_TOK  192
#define MAX_STACK 96

typedef struct { int t; double v; } etok;   /* t=T_NUM 时 v=数值, 否则 v 无意义 */

static const struct { const char *name; int f; } s_fns[] = {
    {"sin",F_SIN},{"cos",F_COS},{"tan",F_TAN},{"asin",F_ASIN},{"acos",F_ACOS},
    {"atan",F_ATAN},{"sinh",F_SINH},{"cosh",F_COSH},{"tanh",F_TANH},
    {"ln",F_LN},{"log",F_LOG},{"log2",F_LOG2},{"sqrt",F_SQRT},{"exp",F_EXP},
    {"abs",F_ABS},{"floor",F_FLOOR},{"ceil",F_CEIL},{NULL,0}
};

static int fn_lookup(const char *s, size_t len) {
    for (int i = 0; s_fns[i].name; i++)
        if (strlen(s_fns[i].name) == len && strncmp(s, s_fns[i].name, len) == 0)
            return s_fns[i].f;
    return 0;
}
static bool is_unary(int f) { return f >= F_SIN && f <= F_CEIL; }
static int prec(int t) {
    switch (t) {
    case T_ADD: case T_SUB: return 1;
    case T_MUL: case T_DIV: case T_MOD: return 2;
    case T_POW: return 3;
    case T_FACT: case T_UNEG: return 4;
    default: return is_unary(t) ? 5 : 0;
    }
}
static bool right_assoc(int t) { return t == T_POW; }

static double apply_fn(int f, double x) {
    switch (f) {
    case F_SIN: return sin(x);
    case F_COS: return cos(x);
    case F_TAN: return tan(x);
    case F_ASIN: return asin(x);
    case F_ACOS: return acos(x);
    case F_ATAN: return atan(x);
    case F_SINH: return sinh(x);
    case F_COSH: return cosh(x);
    case F_TANH: return tanh(x);
    case F_LN:  return x > 0 ? log(x)   : NAN;
    case F_LOG: return x > 0 ? log10(x) : NAN;
    case F_LOG2:return x > 0 ? log2(x)  : NAN;
    case F_SQRT:return x >= 0 ? sqrt(x) : NAN;
    case F_EXP: return exp(x);
    case F_ABS: return fabs(x);
    case F_FLOOR:return floor(x);
    case F_CEIL: return ceil(x);
    default: return NAN;
    }
}

int calc_eval(const char *expr, double ans, double *out) {
    if (!expr || !out) return -1;

    /* ---- 1. 分词 ---- */
    etok tk[MAX_TOK];
    int tkn = 0;
    const char *p = expr;
    while (*p) {
        while (*p == ' ') p++;
        char c = *p;
        if (c == 0) break;

        bool is_literal_start = isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p[1])) ||
                                isalpha((unsigned char)c) || c == '(';
        /* 隐式乘法: 数字/常量/函数/左括号 之前是 数字/右括号/阶乘 */
        if (is_literal_start && tkn > 0) {
            int prev = tk[tkn - 1].t;
            if (prev == T_NUM || prev == T_FACT || prev == T_RP) {
                if (tkn >= MAX_TOK) return -1;
                tk[tkn++].t = T_MUL;
            }
        }

        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            char *end; double v = strtod(p, &end);
            if (end == p) return -1;
            if (tkn >= MAX_TOK) return -1;
            tk[tkn].t = T_NUM; tk[tkn].v = v; tkn++;
            p = end;
        } else if (isalpha((unsigned char)c)) {
            size_t len = 0; while (p[len] && isalpha((unsigned char)p[len])) len++;
            double v = 0; int tt = T_NUM;
            if (len == 3 && strncasecmp(p, "Ans", 3) == 0) v = ans;
            else if (len == 1 && (c=='p'||c=='P')) v = M_PI;
            else if (len == 1 && (c=='e'||c=='E')) v = M_E;
            else { int f = fn_lookup(p, len); if (!f) return -1; tt = f; }
            if (tkn >= MAX_TOK) return -1;
            tk[tkn].t = tt; tk[tkn].v = v; tkn++;
            p += len;
        } else {
            int t;
            if (c == '+') t = T_ADD; else if (c == '-') t = T_SUB;
            else if (c == '*') t = T_MUL; else if (c == '/') t = T_DIV;
            else if (c == '%') t = T_MOD; else if (c == '^') t = T_POW;
            else if (c == '!') t = T_FACT;
            else if (c == '(') t = T_LP; else if (c == ')') t = T_RP;
            else return -1;
            /* 一元负号 */
            if (t == T_SUB) {
                bool uneg = (tkn == 0);
                if (!uneg) {
                    int pt = tk[tkn - 1].t;
                    uneg = (pt == T_LP || is_unary(pt) || pt == T_ADD || pt == T_SUB ||
                            pt == T_MUL || pt == T_DIV || pt == T_MOD || pt == T_POW || pt == T_UNEG);
                }
                if (!uneg) uneg = (tkn==0);
                if (uneg) t = T_UNEG;
            }
            if (tkn >= MAX_TOK) return -1;
            tk[tkn].t = t; tkn++;
            p++;
        }
    }
    if (tkn == 0) return -1;

    /* ---- 2. 调车场转后缀 ---- */
    int postt[MAX_TOK]; double postv[MAX_TOK]; int pn = 0;
    int os[MAX_STACK]; int osn = 0;

    for (int i = 0; i < tkn; i++) {
        int t = tk[i].t;
        if (t == T_NUM) {
            if (pn >= MAX_TOK) return -1;
            postt[pn] = T_NUM; postv[pn] = tk[i].v; pn++;
        } else if (is_unary(t)) {          /* 函数: 压栈 */
            if (osn >= MAX_STACK) return -1;
            os[osn++] = t;
        } else if (t == T_LP) {
            if (osn >= MAX_STACK) return -1;
            os[osn++] = t;
        } else if (t == T_RP) {
            while (osn > 0 && os[osn-1] != T_LP) {
                int o = os[--osn];
                if (pn >= MAX_TOK) return -1;
                postt[pn] = o; postv[pn] = 0; pn++;
            }
            if (osn == 0) return -1;
            osn--;             /* 弹 '(' */
            if (osn > 0 && is_unary(os[osn-1])) {   /* 函数在栈顶: 出栈 */
                int f = os[--osn];
                if (pn >= MAX_TOK) return -1;
                postt[pn] = f; postv[pn] = 0; pn++;
            }
        } else {                             /* 运算/一元负/阶乘 */
            if (t == T_FACT) {
                if (pn >= MAX_TOK) return -1;
                postt[pn] = t; postv[pn] = 0; pn++;
                continue;
            }
            int pr = prec(t);
            while (osn > 0) {
                int top = os[osn-1];
                if (top == T_LP) break;
                int tpr = prec(top);
                if (tpr > pr || (tpr == pr && !right_assoc(t))) {
                    if (pn >= MAX_TOK) return -1;
                    postt[pn] = top; postv[pn] = 0; pn++;
                    osn--;
                } else break;
            }
            if (osn >= MAX_STACK) return -1;
            os[osn++] = t;
        }
    }
    while (osn > 0) {
        int t = os[--osn];
        if (t == T_LP) return -1;
        if (pn >= MAX_TOK) return -1;
        postt[pn] = t; postv[pn] = 0; pn++;
    }

    /* ---- 3. 后缀求值 ---- */
    double st[MAX_TOK]; int sn = 0;
    for (int i = 0; i < pn; i++) {
        int t = postt[i];
        if (t == T_NUM) {
            if (sn >= MAX_TOK) return -1;
            st[sn++] = postv[i];
        } else if (is_unary(t)) {
            if (sn < 1) return -1;
            double x = st[--sn];
            double r = apply_fn(t, x);
            if (isnan(r)) return -2;
            st[sn++] = r;
        } else if (t == T_FACT) {
            if (sn < 1) return -1;
            double x = st[--sn];
            if (x < 0 || floor(x) != x || x > 170) return -2;
            double r = 1; for (int k = 2; k <= (int)x; k++) r *= k;
            st[sn++] = r;
        } else if (t == T_UNEG) {
            if (sn < 1) return -1;
            st[sn-1] = -st[sn-1];
        } else {
            if (sn < 2) return -1;
            double b = st[--sn], a = st[--sn];
            double r = NAN;
            switch (t) {
            case T_ADD: r = a + b; break;
            case T_SUB: r = a - b; break;
            case T_MUL: r = a * b; break;
            case T_DIV: if (b == 0) return -2; r = a / b; break;
            case T_MOD: if (b == 0) return -2; r = fmod(a, b); break;
            case T_POW: r = pow(a, b); break;
            default: return -1;
            }
            st[sn++] = r;
        }
    }
    if (sn != 1) return -1;
    if (isnan(st[0]) || isinf(st[0])) return -2;
    *out = st[0];
    return 0;
}