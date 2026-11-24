#pragma once
/* UTF-8 字符边界工具 —— 全项目唯一实现。
 *
 * 为什么单独成模块: 按宽度/字节上限裁剪字符串后"别留半个汉字"这件事, 原来在
 * page_fav / page_mp3 / page_lua / book_reader 里各自手写了 8 份, 写法已经出现偏差
 * (book_reader 那两行的退字节逻辑跟其余处不一致, 会把 3 字节字符只留首字节)。
 * 统一到这里, 各层都只调这两个函数。
 *
 * 依赖: 无 (纯 C, 不碰堆/不碰硬件), 所以 st7305 / fonts / ui_common / engines / apps
 * 都能直接用, 不会引入循环依赖。 */

#include <stddef.h>

/* 把 len 收缩到完整字符边界:
 *   · 末尾若是"半个字符"(被截断的多字节序列) → 砍掉它
 *   · 末尾已完整 → 原样返回 len
 * 用于"按字节上限 memcpy 之后修正"。等价于原来的
 *   while (n > 0 && (s[len-1] & 0xC0) == 0x80) ... 那套循环。 */
size_t utf8_align(const char *s, size_t len);

/* 砍掉末尾**一个完整字符**, 返回新长度。
 * 用于"显示宽度不够, 逐字收缩"的循环:
 *   while (width(tmp) > avail && (n = utf8_drop_last(tmp, n)) > 0) tmp[n] = 0;
 * 已按 utf8_align 对齐过, 所以不会切出半个字符。 */
size_t utf8_drop_last(const char *s, size_t len);
