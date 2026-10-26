/* lz4_dec.h — 只读的 LZ4 block 解压 (BSD, 参考 LZ4 block format)
 *
 * 为什么自己写: ESP-IDF 不带 lz4, 引整个 lz4.c 要为用不到的压缩器付代码体积。
 * 这里只实现解压, 约 90 行, 边界检查逐字节做, 不信任输入。
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* 返回值: 0=成功, <0=失败
 *   -1 输入截断   -2 输出溢出   -3 偏移非法(指向输出起点之前)
 * *out_len 为实际写出的字节数 */
int lz4_dec_block(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t dst_cap, size_t *out_len);
