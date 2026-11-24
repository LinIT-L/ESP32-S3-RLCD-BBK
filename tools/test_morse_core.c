/**
 * test_morse_core.c — 摩斯核心逻辑 主机端单元测试
 *
 * 编译运行 (不需要 ESP-IDF):
 *   cc -I components/apps/pages tools/test_morse_core.c \
 *      components/apps/pages/morse_codec.c -o /tmp/test_morse_core && /tmp/test_morse_core
 *
 * 覆盖: 码表数量 / 编解码往返 / 空格与标点 / 非法字符 / 最大长度 /
 *       缓冲区边界 / 时间轴(点划、字符间隔、词间隔、WPM)
 */
#include "morse_codec.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } \
} while (0)

static mr_el_t   s_els[MR_MAX_EL];
static mr_plan_t s_plan;

static int expand(const char *text, uint8_t wpm, uint32_t *total)
{
    if (mr_build_plan(text, &s_plan) != 0) return -100;
    return mr_expand(&s_plan, wpm, s_els, MR_MAX_EL, total);
}

int main(void)
{
    printf("[码表]\n");
    CHECK(mr_charset_count() == 54, "码表应为 54 项 (26+10+18)");
    CHECK(strcmp(mr_encode('a'), ".-") == 0, "小写 a 应编码为 .-");
    CHECK(strcmp(mr_encode('Z'), "--..") == 0, "大写 Z 正确");
    CHECK(strcmp(mr_encode('7'), "--...") == 0, "数字 7 正确");
    CHECK(strcmp(mr_encode('$'), "...-..-") == 0, "$ 为 7 符号");
    CHECK(mr_encode('#') == NULL, "# 不支持应返回 NULL");
    CHECK(mr_decode("-.-") == 'K', "-.- → K");
    CHECK(mr_decode("......") == 0, "非法序列 → 0");
    CHECK(mr_decode("") == 0, "空串 → 0");
    {
        int bad = 0;
        for (int i = 0; i < mr_charset_count(); i++) {
            char c = mr_charset_at(i);
            const char *code = mr_charset_code(i);
            if (!code || !code[0] || mr_decode(code) != c) { bad++; printf("   roundtrip fail: %c\n", c); }
            if (mr_encode(c) != code) bad++;   /* 编码/查表必须同一份数据源 */
        }
        CHECK(bad == 0, "54 项 编码/查表/解码 全一致");
    }

    printf("[时间轴 @20WPM]\n");
    {
        uint32_t t = 0;
        CHECK((expand("E", 20, &t)) == 3, "E: mark+gap_c+END");
        CHECK(t == 240, "E = 4u = 240ms");
        CHECK((expand("T", 20, &t)) == 3, "T: mark+gap_c+END");
        CHECK(t == 360, "T = 6u = 360ms");
        CHECK((expand("A", 20, &t)) == 5, "A: 2符号+1符号间隔+1字符间隔+END");
        CHECK(t == 480, "A = 8u = 480ms");
        CHECK((expand("PARIS", 20, &t)) > 0, "PARIS 可展开");
        CHECK(t == 46u * 60u, "PARIS = 46u = 2760ms (末尾无词间隔)");
        CHECK((expand("PARIS ", 20, &t)) > 0, "PARIS+空格 可展开");
        CHECK(t == 50u * 60u, "PARIS+空格 = 50u = 3000ms (标准 20WPM)");
        CHECK((expand("A B", 20, &t)) > 0, "A B 可展开");
        CHECK(t == 24u * 60u, "A B = 24u; A 起→B 起 = 12u (词间隔 7u 不重复累计)");
        /* 元素顺序核对: A B 的第 5 个元素应是词间隔补足 4u */
        CHECK(s_els[4].kind == MR_EL_GAP_W && s_els[4].ms == 240, "词间隔补足 = 4u = 240ms");
    }

    printf("[WPM 换算]\n");
    CHECK(mr_unit_ms(20) == 60, "20WPM → 60ms");
    CHECK(mr_unit_ms(10) == 120, "10WPM → 120ms");
    CHECK(mr_unit_ms(5) == 240, "5WPM → 240ms");
    CHECK(mr_unit_ms(35) == 34, "35WPM → 34ms");
    CHECK(mr_unit_ms(1) == 240, "低于 5 夹到 5WPM");
    CHECK(mr_unit_ms(99) == 34, "高于 35 夹到 35WPM");
    {
        uint32_t t5 = 0, t20 = 0;
        expand("E", 5, &t5);
        expand("E", 20, &t20);
        CHECK(t5 == 4u * 240u, "5WPM 的 E = 4×240ms");
        CHECK(t20 * 4 == t5, "20WPM 与 5WPM 时长比 = 1:4");
    }

    printf("[空格与标点]\n");
    {
        char buf[128];
        mr_plan_t p;
        CHECK(mr_build_plan("A B", &p) == 0, "含空格构建成功");
        CHECK(p.n == 3 && p.ch[1].n == 0, "空格成为独立的词间隔项");
        CHECK(mr_fmt_morse(&p, buf, sizeof(buf)) > 0, "格式化成功");
        CHECK(strcmp(buf, ".- / -...") == 0, "显示串用 ' / ' 表示词间隔");

        CHECK(mr_build_plan(",?.", &p) == 0, "标点构建成功");
        CHECK(mr_fmt_morse(&p, buf, sizeof(buf)) > 0, "标点格式化成功");
        CHECK(strcmp(buf, "--..-- ..--.. .-.-.-") == 0, ",?. 编码正确");

        CHECK(mr_build_plan("   ", &p) == 0, "纯空格构建成功");
        CHECK(mr_char_count(&p) == 0, "纯空格 = 0 个可播放字符");
        uint32_t t = 0;
        CHECK(mr_expand(&p, 20, s_els, MR_MAX_EL, &t) == 1, "纯空格只有 END, 无可播元素");
    }

    printf("[非法字符]\n");
    {
        mr_plan_t p;
        CHECK(mr_build_plan("A#B", &p) == 0, "含非法字符仍可构建");
        CHECK(p.n == 3, "A#B → 3 个计划项");
        CHECK(p.bad_cnt == 1 && p.bad[0] == '#', "非法字符被记录 (不静默丢弃)");
        char buf[64];
        CHECK(mr_fmt_morse(&p, buf, sizeof(buf)) > 0, "格式化成功");
        CHECK(strcmp(buf, ".- ? -...") == 0, "非法字符显示为 '?'");
        uint32_t t = 0;
        /* A(4 元素) + B(8 元素) + END = 13, 非法项不占时间 */
        CHECK(mr_expand(&p, 20, s_els, MR_MAX_EL, &t) == 13, "非法项不参与播放");
    }

    printf("[长度与缓冲区边界]\n");
    {
        char max[MR_TEXT_MAX + 1];
        memset(max, 'A', MR_TEXT_MAX);
        max[MR_TEXT_MAX] = 0;
        mr_plan_t p;
        CHECK(mr_build_plan(max, &p) == 0, "48 字符 (上限) 构建成功");
        uint32_t t = 0;
        CHECK(mr_expand(&p, 20, s_els, MR_MAX_EL, &t) > 0, "48 字符可完整展开");

        char over[MR_TEXT_MAX + 2];
        memset(over, 'A', MR_TEXT_MAX + 1);
        over[MR_TEXT_MAX + 1] = 0;
        CHECK(mr_build_plan(over, &p) == -2, "49 字符必须返回 -2 (不截断)");

        /* 最坏情形: 48 个 '$' (每字符 7 符号 = 14 元素) */
        char worst[MR_TEXT_MAX + 1];
        memset(worst, '$', MR_TEXT_MAX);
        worst[MR_TEXT_MAX] = 0;
        CHECK(mr_build_plan(worst, &p) == 0, "48 个 $ 构建成功");
        int n = mr_expand(&p, 20, s_els, MR_MAX_EL, &t);
        CHECK(n == MR_TEXT_MAX * 14 + 1, "48×$ 展开 = 673 元素");
        CHECK(n <= MR_MAX_EL, "MR_MAX_EL 必须容纳最坏情形");
        CHECK(mr_expand(&p, 20, s_els, 100, &t) == -2, "容量不足返回 -2");
        CHECK(mr_expand(&p, 20, s_els, 0, &t) == -1, "cap<=0 返回 -1");
    }

    printf("[格式化容量]\n");
    {
        mr_plan_t p;
        mr_build_plan("PARIS", &p);
        char small[8];
        CHECK(mr_fmt_morse(&p, small, sizeof(small)) == -1, "容量不足返回 -1 (不写半截)");
        char big[128];
        CHECK(mr_fmt_morse(&p, big, sizeof(big)) > 0, "容量足够成功");
        CHECK(strcmp(big, ".--. .- .-. .. ...") == 0, "PARIS 显示串正确");
        char empty[16];
        mr_build_plan("", &p);
        CHECK(mr_fmt_morse(&p, empty, sizeof(empty)) == 0, "空文本 → 空串");
        CHECK(mr_char_count(&p) == 0, "空文本 → 0 字符");
    }

    printf("\n%s  pass=%d fail=%d\n", g_fail ? "FAILED" : "OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
