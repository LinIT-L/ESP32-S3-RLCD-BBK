/* 电子书文本排版内核实现 (见 book_flow.h) */
#include "book_flow.h"

#include <string.h>

#include "linebreak.h"

/* ---------- UTF-8 / 解码 ---------- */

bool bf_utf8_valid(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) { i++; continue; }
        int extra;
        uint32_t cp;
        if ((b & 0xE0) == 0xC0)      { extra = 1; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; }
        else return false;
        if (i + extra >= n) return false;
        for (int k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

int bf_cp_to_utf8(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t rd_u16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

bf_ch_t bf_next_ch(const uint8_t *p, const uint8_t *end, uint8_t enc) {
    bf_ch_t ch;
    memset(&ch, 0, sizeof(ch));
    if (p >= end) return ch;
    uint8_t b0 = p[0];

    if (enc == BF_ENC_UTF16LE || enc == BF_ENC_UTF16BE) {
        if (p + 2 > end) { ch.adv = 0; return ch; }
        uint16_t u = (enc == BF_ENC_UTF16LE) ? rd_u16le(p) : rd_u16be(p);
        uint32_t cp = u;
        uint8_t adv = 2;
        if (u >= 0xD800 && u <= 0xDBFF && p + 4 <= end) {
            uint16_t u2 = (enc == BF_ENC_UTF16LE) ? rd_u16le(p + 2) : rd_u16be(p + 2);
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(u - 0xD800)) << 10) + (u2 - 0xDC00);
                adv = 4;
            }
        }
        ch.cp = cp;
        ch.adv = adv;
        if (cp == '\n') { ch.kind = BF_CH_NEWLINE; return ch; }
        if (cp == '\r') {
            ch.kind = BF_CH_NEWLINE;
            ch.adv = (p + adv <= end && (enc == BF_ENC_UTF16LE
                         ? rd_u16le(p + adv) : rd_u16be(p + adv)) == '\n')
                         ? (uint8_t)(adv + 2) : adv;
            return ch;
        }
        if (cp == '\t') { ch.kind = BF_CH_TAB; return ch; }
        if (cp < 0x20) { ch.kind = BF_CH_SKIP; return ch; }
        ch.kind = (cp < 0x80) ? BF_CH_ASCII : BF_CH_CJK;
        return ch;
    }

    if (enc == BF_ENC_UTF8) {
        if (b0 < 0x80) {
            ch.cp = b0;
            ch.adv = 1;
            if (b0 == '\n') { ch.kind = BF_CH_NEWLINE; return ch; }
            if (b0 == '\r') {
                ch.kind = BF_CH_NEWLINE;
                ch.adv = (p + 1 < end && p[1] == '\n') ? 2 : 1;
                return ch;
            }
            if (b0 == '\t') { ch.kind = BF_CH_TAB; return ch; }
            if (b0 < 0x20) { ch.kind = BF_CH_SKIP; return ch; }
            ch.kind = BF_CH_ASCII;
            return ch;
        }
        int extra;
        uint32_t cp;
        if ((b0 & 0xE0) == 0xC0)      { extra = 1; cp = b0 & 0x1F; }
        else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
        else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
        else { ch.kind = BF_CH_SKIP; ch.adv = 1; return ch; }
        if (p + extra >= end) { ch.kind = BF_CH_SKIP; ch.adv = 1; return ch; }
        for (int k = 1; k <= extra; k++) {
            if ((p[k] & 0xC0) != 0x80) { ch.kind = BF_CH_SKIP; ch.adv = 1; return ch; }
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        ch.cp = cp;
        ch.adv = (uint8_t)(extra + 1);
        ch.kind = (cp < 0x80) ? BF_CH_ASCII : BF_CH_CJK;
        return ch;
    }

    /* GBK */
    if (b0 >= 0x81 && p + 1 < end) {
        uint8_t b1 = p[1];
        if (b1 >= 0x40 && b1 != 0x7F) {
            ch.hi = b0;
            ch.lo = b1;
            ch.adv = 2;
            ch.kind = BF_CH_CJK;
            return ch;
        }
    }
    ch.cp = b0;
    ch.adv = 1;
    if (b0 == '\n') { ch.kind = BF_CH_NEWLINE; return ch; }
    if (b0 == '\r') {
        ch.kind = BF_CH_NEWLINE;
        ch.adv = (p + 1 < end && p[1] == '\n') ? 2 : 1;
        return ch;
    }
    if (b0 == '\t') { ch.kind = BF_CH_TAB; return ch; }
    if (b0 < 0x20) { ch.kind = BF_CH_SKIP; return ch; }
    ch.kind = BF_CH_ASCII;
    return ch;
}

/* ---------- 断行 ---------- */

size_t bf_build_utf8(const bf_ch_t *win, int cnt, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < cnt; i++) {
        const bf_ch_t *ch = &win[i];
        if (ch->kind == BF_CH_NEWLINE || ch->kind == BF_CH_TAB ||
            ch->kind == BF_CH_SKIP) {
            if (n >= cap) break;
            out[n] = ' ';
            n++;
            continue;
        }
        if (n >= cap) break;
        int k = bf_cp_to_utf8(ch->cp, out + n);
        if (n + (size_t)k > cap) break;
        n += (size_t)k;
    }
    if (n < cap) out[n] = 0;
    return n;
}

static int cp_utf8_len(uint32_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

void bf_breaks(const bf_ch_t *win, int cnt, uint8_t enc,
               uint8_t *brk, uint8_t *scratch, size_t scratch_cap) {
    if (cnt <= 0) return;
    if (enc == BF_ENC_GBK) {
        memset(brk, 0, (size_t)cnt);
        return;
    }
    /* scratch: 前半 UTF-8 缓冲, 后半 libunibreak 结果 */
    size_t half = scratch_cap / 2;
    if (half < 8) { memset(brk, 0, (size_t)cnt); return; }
    uint8_t *utf8 = scratch;
    uint8_t *lb = scratch + half;
    size_t n = bf_build_utf8(win, cnt, utf8, half - 1);
    if (n) {
        size_t lbcap = scratch_cap - half;
        if (n > lbcap) n = lbcap;
        set_linebreaks_utf8(utf8, n, "zh", (char *)lb);
    }
    int bi = 0;
    for (int i = 0; i < cnt; i++) {
        int adv;
        switch (win[i].kind) {
            case BF_CH_NEWLINE:
            case BF_CH_TAB:
            case BF_CH_SKIP:
                adv = 1;
                break;
            default:
                adv = cp_utf8_len(win[i].cp);
                if (adv < 1) adv = 1;
                break;
        }
        bi += adv;
        uint8_t v = (bi <= (int)n && n > 0) ? lb[bi - 1] : 0;
        if (v == LINEBREAK_MUSTBREAK) brk[i] = 2;
        else if (v == LINEBREAK_ALLOWBREAK) brk[i] = 1;
        else brk[i] = 0;
    }
}

/* ---------- 排版 ---------- */

void bf_layout(const bf_ch_t *win, int cnt, const uint8_t *brk,
               int rows, int line_max, int ascii_w, int cjk_w,
               int *line_end, int *line_count, int *boundary) {
    int i = 0, lines = 0, x = 0, cand = -1, ls = 0;
    while (i < cnt && lines < rows) {
        const bf_ch_t *ch = &win[i];
        if (ch->kind == BF_CH_NEWLINE) {
            line_end[lines++] = i;
            i++;
            x = 0;
            cand = -1;
            ls = i;
            if (lines >= rows) { *line_count = lines; *boundary = i; return; }
            continue;
        }
        int w = (ch->kind == BF_CH_ASCII) ? ascii_w
              : (ch->kind == BF_CH_SKIP) ? 0 : cjk_w;
        if (x + w > line_max && x > 0) {
            if (cand >= ls) {
                line_end[lines++] = cand + 1;
                if (lines >= rows) { *line_count = lines; *boundary = cand + 1; return; }
                i = cand + 1;
                x = 0;
                cand = -1;
                ls = i;
                continue;
            }
            line_end[lines++] = i;
            if (lines >= rows) { *line_count = lines; *boundary = i; return; }
            x = 0;
            cand = -1;
            ls = i;
        }
        if (w > 0 && brk && brk[i]) cand = i;
        x += w;
        i++;
    }
    line_end[lines++] = cnt;
    *line_count = lines;
    *boundary = cnt;
}

/* ---------- 顺序分页 ---------- */

int bf_paginate(bf_src_t src, uint32_t fsz, uint8_t enc, uint32_t start_off,
                int rows, int line_max, int ascii_w, int cjk_w,
                bf_add_page_t add_page, void *add_ud,
                bf_on_char_t on_char, void *on_ud,
                uint8_t *chunk, uint32_t chunk_cap,
                bf_ch_t *win, uint8_t *brk, uint8_t *scratch, uint32_t scratch_cap,
                uint32_t *win_off,
                uint32_t limit, bool resume_open, uint32_t *out_next) {
    uint32_t pos = start_off;
    uint32_t win_base = 0, win_len = 0;
    bool need_page = !resume_open;
    int rc = 0;

    while (pos < fsz) {
        if (need_page) {
            if (add_page(add_ud, pos) != 0) { rc = -1; break; }
            need_page = false;
        }
        /* 解码窗口: 最多 BF_MAX_WIN 字符 */
        int n = 0;
        uint32_t cpos = pos;
        bool undecodable = false;
        while (n < BF_MAX_WIN && cpos < fsz) {
            if (cpos + 4 > win_base + win_len || cpos < win_base) {
                uint32_t want = chunk_cap;
                if (want > fsz - cpos) want = fsz - cpos;
                size_t got = src.read_at(src.ud, cpos, chunk, want);
                if (got == 0) break;
                win_base = cpos;
                win_len = (uint32_t)got;
            }
            const uint8_t *p = chunk + (cpos - win_base);
            const uint8_t *end = chunk + win_len;
            bf_ch_t ch = bf_next_ch(p, end, enc);
            if (ch.adv == 0) { undecodable = true; break; }
            win[n] = ch;
            win_off[n] = cpos;
            n++;
            cpos += ch.adv;
        }
        if (n == 0) {
            /* 无内容可解码: 直接到文件尾 (跳过不可解码尾巴) */
            need_page = true;
            pos = fsz;
            break;
        }
        bf_breaks(win, n, enc, brk, scratch, scratch_cap);
        int line_end[BF_MAX_LINES], line_count, boundary;
        bf_layout(win, n, brk, rows, line_max, ascii_w, cjk_w,
                  line_end, &line_count, &boundary);
        if (boundary >= n) {
            /* 窗口耗尽: 本页到窗口尽头结束 (文件尾或大量零宽控制符) */
            if (on_char) {
                for (int k = 0; k < n; k++) on_char(on_ud, &win[k], win_off[k]);
            }
            need_page = true;
            pos = undecodable ? fsz : cpos;
            if (pos >= fsz) break;
            if (pos >= limit) break;   /* 阶段检查点 */
            continue;
        }
        /* 页完成: 页内容为 [0, boundary) */
        if (on_char) {
            for (int k = 0; k < boundary; k++) on_char(on_ud, &win[k], win_off[k]);
        }
        pos = win_off[boundary];
        need_page = true;
        if (pos >= fsz) break;
        if (pos >= limit) break;   /* 阶段检查点 */
    }
    *out_next = pos;
    if (rc != 0) return -1;
    return 0;
}
