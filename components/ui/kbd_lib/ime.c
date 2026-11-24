/**
 * ime.c — 中文输入法内核实现 (见 ime.h 的用法说明).
 *
 * 查询路径: 拼音串 → 二分/线性扫 400 个音节 → 命中组 → 组内候选字.
 *   候选表是"反向索引"(拼音→字), 正好是输入法需要的方向, 不需要 字→拼音 的反查.
 *
 * 候选策略 (无词库, 只有单字):
 *   ① 拼音串正好等于某个完整音节   → 该音节全部候选 (最多 IME_CAND_MAX 个)
 *   ② 拼音串只是前缀 (还没打完)   → 每个匹配音节取头 2 个字
 *   ③ 无匹配                       → 空候选, 拼音串照常显示, 可以继续退格
 * ②的意义: 用户打完 "zhon" 还没按 "g" 时, 候选栏不是空的, 心里有底;
 *   但不会把 20 个 zh* 音节的所有字铺满, 那样反而找不到想要的。
 */
#include "ime.h"
#include "ime_pinyin.h"
#include <string.h>

static bool          s_on;
static char          s_py[IME_PY_MAX + 1];
static int           s_py_n;
static int           s_cand_cp[IME_CAND_MAX];
static int           s_cand_n;
static ime_out_cb_t  s_sink;

void ime_sink(ime_out_cb_t cb) { s_sink = cb; }

bool ime_on(void) { return s_on; }

void ime_clear(void) { s_py_n = 0; s_py[0] = '\0'; s_cand_n = 0; }

void ime_set(bool on) {
    s_on = on;
    ime_clear();
}

void ime_toggle(void) { ime_set(!s_on); }

bool ime_pending(void) { return s_py_n > 0; }

const char *ime_pinyin(void) { return s_py; }

int ime_cand_n(void) { return s_cand_n; }

int ime_cand_cp(int idx) {
    if (idx < 0 || idx >= s_cand_n) return -1;
    return s_cand_cp[idx];
}

void ime_cp_to_utf8(int cp, char out[4]) {
    if (cp < 0) { out[0] = '\0'; return; }
    if (cp < 0x80) { out[0] = (char)cp; out[1] = '\0'; return; }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}

const char *ime_cand_utf8(int idx) {
    static char b[4];
    int cp = ime_cand_cp(idx);
    if (cp < 0) return NULL;
    ime_cp_to_utf8(cp, b);
    return b;
}

/* 音节 i 的字符串与长度 (blob 里以 0 结尾, 长度 = 下一项偏移 - 本项 - 1) */
static const char *syl_at(int i) { return ime_syl_blob + ime_syl_off[i]; }
static int syl_len(int i) { return (int)ime_syl_off[i + 1] - (int)ime_syl_off[i] - 1; }

/* 重建候选表. 只在拼音串变化时调用, 不是每帧调用. */
static void ime_rebuild(void) {
    s_cand_n = 0;
    if (s_py_n <= 0) return;

    /* ① 完整音节精确匹配 */
    for (int i = 0; i < IME_SYL_N; i++) {
        if (syl_len(i) != s_py_n) continue;
        if (memcmp(syl_at(i), s_py, (size_t)s_py_n) != 0) continue;
        for (int k = ime_grp_start[i]; k < ime_grp_start[i + 1] && s_cand_n < IME_CAND_MAX; k++)
            s_cand_cp[s_cand_n++] = ime_char_cp[k];
        return;
    }

    /* ② 前缀匹配: 每个音节取头 2 个字 */
    for (int i = 0; i < IME_SYL_N && s_cand_n + 2 <= IME_CAND_MAX; i++) {
        if (syl_len(i) <= s_py_n) continue;
        if (memcmp(syl_at(i), s_py, (size_t)s_py_n) != 0) continue;
        for (int k = ime_grp_start[i]; k < ime_grp_start[i] + 2 && k < ime_grp_start[i + 1]; k++)
            s_cand_cp[s_cand_n++] = ime_char_cp[k];
    }
}

bool ime_char(char c) {
    if (!s_on) return false;

    /* 字母 = 拼拼音 (Caps/Shift 在中文模式下没有意义, 一律按小写) */
    if (c >= 'a' && c <= 'z') {
        if (s_py_n < IME_PY_MAX) { s_py[s_py_n++] = c; s_py[s_py_n] = '\0'; ime_rebuild(); }
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        if (s_py_n < IME_PY_MAX) { s_py[s_py_n++] = (char)(c - 'A' + 'a'); s_py[s_py_n] = '\0'; ime_rebuild(); }
        return true;
    }

    /* 非字母(数字/标点): 先把待上屏的拼音变成字, 再由调用方插入该符号。
     * 顺序很重要 —— 先上屏再返回 false, 页面插入的符号才出现在汉字后面。 */
    if (s_py_n > 0) ime_pick(0);
    return false;
}

bool ime_bksp(void) {
    if (!s_on || s_py_n <= 0) return false;
    s_py_n--;
    s_py[s_py_n] = '\0';
    ime_rebuild();
    return true;
}

bool ime_pick(int idx) {
    if (idx < 0 || idx >= s_cand_n) return false;
    char u[4];
    ime_cp_to_utf8(s_cand_cp[idx], u);
    if (s_sink) s_sink(u);
    ime_clear();
    return true;
}
