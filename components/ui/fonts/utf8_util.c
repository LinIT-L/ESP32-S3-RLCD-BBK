#include "utf8_util.h"

#include <stdint.h>

/* 首字节 → 该字符应占的字节数 (非法/续字节一律按 1 处理, 不越界) */
static inline size_t u8_len(unsigned char c)
{
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

size_t utf8_align(const char *s, size_t len)
{
    if (!s || len == 0) return 0;
    size_t i = len - 1;
    /* 从末字节往前跳过所有续字节 (10xxxxxx), 直到该字符的首字节 */
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    size_t need = u8_len((unsigned char)s[i]);
    /* 首字节位置 i, 完整字符应占 need 字节。若实际不足 need ⇒ 是半个, 砍掉 */
    return (len - i >= need) ? len : i;
}

size_t utf8_drop_last(const char *s, size_t len)
{
    len = utf8_align(s, len);          /* 先保证末尾是完整字符 */
    if (len == 0) return 0;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;                          /* 砍到该字符的首字节 */
}
