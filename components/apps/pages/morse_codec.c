/**
 * morse_codec.c — 摩斯码表 / 编解码 / 时间轴 (纯逻辑)
 */
#include "morse_codec.h"
#include <string.h>

/* ================= 码表 (54 项: 26 字母 + 10 数字 + 18 标点) ================= */
typedef struct { char ch; const char *code; } mr_ent_t;

static const mr_ent_t s_tab[] = {
    /* 字母 26 */
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
    {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
    {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
    {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
    {'Z', "--.."},
    /* 数字 10 */
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
    {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
    /* 标点 18 (ITU) */
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."},
};
#define MR_TAB_N ((int)(sizeof(s_tab) / sizeof(s_tab[0])))

uint8_t mr_charset_count(void) { return (uint8_t)MR_TAB_N; }

char mr_charset_at(uint8_t i) { return (i < (uint8_t)MR_TAB_N) ? s_tab[i].ch : 0; }

const char *mr_charset_code(uint8_t i) { return (i < (uint8_t)MR_TAB_N) ? s_tab[i].code : ""; }

static char mr_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

const char *mr_encode(char ch)
{
    char u = mr_upper(ch);
    for (int i = 0; i < MR_TAB_N; i++)
        if (s_tab[i].ch == u) return s_tab[i].code;
    return NULL;
}

char mr_decode(const char *pat)
{
    if (!pat || !pat[0]) return 0;
    size_t n = strlen(pat);
    if (n > MR_MARK_MAX) return 0;
    for (int i = 0; i < MR_TAB_N; i++)
        if (strlen(s_tab[i].code) == n && memcmp(s_tab[i].code, pat, n) == 0)
            return s_tab[i].ch;
    return 0;
}

/* ================= 计划构建 ================= */
int mr_build_plan(const char *text, mr_plan_t *p)
{
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    if (!text) return 0;

    size_t len = strlen(text);
    if (len > MR_TEXT_MAX) return -2;          /* 超长: 明确失败, 不截断 */

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        mr_ch_t *e = &p->ch[p->n];
        if (c == ' ') {                         /* 词间隔 */
            e->n = 0; e->bits = 0; e->bad = 0;
            p->n++;
            continue;
        }
        const char *code = mr_encode((char)c);
        if (!code) {                            /* 不支持: 记录, 不吞掉 */
            e->n = 0; e->bits = 0; e->bad = 1;
            p->n++;
            if (p->bad_cnt < (uint8_t)sizeof(p->bad)) p->bad[p->bad_cnt] = (char)c;
            p->bad_cnt++;
            continue;
        }
        uint8_t n = 0, bits = 0;
        for (const char *q = code; *q && n < MR_MARK_MAX; q++, n++)
            if (*q == '-') bits |= (uint8_t)(1u << n);
        e->n = n; e->bits = bits; e->bad = 0;
        p->n++;
    }
    return 0;
}

/* ================= 时间轴 ================= */
uint16_t mr_unit_ms(uint8_t wpm)
{
    if (wpm < 5)  wpm = 5;
    if (wpm > 35) wpm = 35;
    return (uint16_t)(1200u / wpm);
}

/* 单字符的元素数: n 符号 + (n-1) 符号间隔 + 1 字符间隔 */
static int mr_el_total(const mr_plan_t *p)
{
    int n = 0;
    for (int i = 0; i < p->n; i++) {
        const mr_ch_t *c = &p->ch[i];
        if (c->bad) continue;
        if (c->n == 0) { if (n > 0) n += 1; continue; }   /* 词间隔补足 4u */
        n += (int)c->n + ((int)c->n - 1) + 1;
    }
    return n + 1;                                          /* + END */
}

int mr_expand(const mr_plan_t *p, uint8_t wpm, mr_el_t *out, int cap, uint32_t *total_ms)
{
    if (!p || !out || cap <= 0) return -1;
    uint32_t u = mr_unit_ms(wpm);

    int need = mr_el_total(p);
    if (need > cap) return -2;                  /* 容量不足: 明确失败, 不截断 */

    int k = 0;
    uint32_t acc = 0;
    for (int i = 0; i < p->n; i++) {
        const mr_ch_t *c = &p->ch[i];
        if (c->bad) continue;
        if (c->n == 0) {                        /* 空格: 前字符已给 3u, 这里补到 7u */
            if (k == 0) continue;               /* 开头空格忽略 */
            uint32_t ms = 4u * u;
            out[k].kind = MR_EL_GAP_W; out[k].dash = 0; out[k].ms = (uint16_t)ms;
            acc += ms; k++;
            continue;
        }
        for (int m = 0; m < c->n; m++) {
            int dash = (c->bits >> m) & 1;
            uint32_t ms = dash ? (3u * u) : u;
            out[k].kind = MR_EL_MARK; out[k].dash = (uint8_t)dash; out[k].ms = (uint16_t)ms;
            acc += ms; k++;
            int last = (m == c->n - 1);
            uint32_t gap = last ? (3u * u) : u;
            out[k].kind = last ? MR_EL_GAP_C : MR_EL_GAP_E;
            out[k].dash = 0; out[k].ms = (uint16_t)gap;
            acc += gap; k++;
        }
    }
    out[k].kind = MR_EL_END; out[k].dash = 0; out[k].ms = 0;
    if (total_ms) *total_ms = acc;
    return k + 1;
}

/* ================= 显示 ================= */
int mr_fmt_morse(const mr_plan_t *p, char *out, int cap)
{
    if (!p || !out || cap <= 2) return -1;
    int o = 0;
    for (int i = 0; i < p->n; i++) {
        const mr_ch_t *c = &p->ch[i];
        char tmp[MR_MARK_MAX + 1];
        const char *tok;
        if (c->bad) { tok = "?"; }
        else if (c->n == 0) { tok = "/"; }
        else {
            for (int m = 0; m < c->n; m++) tmp[m] = ((c->bits >> m) & 1) ? '-' : '.';
            tmp[c->n] = 0; tok = tmp;
        }
        int tl = (int)strlen(tok);
        int need = tl + (o ? 1 : 0);
        if (o + need >= cap) return -1;          /* 明确失败, 不写半截 */
        if (o) out[o++] = ' ';
        memcpy(&out[o], tok, (size_t)tl);
        o += tl;
    }
    out[o] = 0;
    return o;
}

int mr_char_count(const mr_plan_t *p)
{
    int n = 0;
    if (!p) return 0;
    for (int i = 0; i < p->n; i++)
        if (!p->ch[i].bad && p->ch[i].n > 0) n++;
    return n;
}
