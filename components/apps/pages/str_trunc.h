#ifndef STR_TRUNC_H
#define STR_TRUNC_H

/* ============================================================================
 * str_trunc —— "装不下就截断"的字符串拷贝 (明确表达意图)
 *
 * 用途: 目标缓冲装的是**短字段**(名字 24B / 描述 32B / URL 56B / 弹窗行 30B ...),
 *   源串比它长是**正常情况**, 我们要的就是截断到缓冲能放下。
 *
 * 为什么不用 snprintf("%s"):
 *   ① GCC 的 -Wformat-truncation 无法知道"截断是设计如此", 每处都报一条告警,
 *      真正的告警会被淹掉;
 *   ② 用 memcpy 手写, 意图一眼可读。
 *
 * ⛔ 只用于"截断无害"的字段。要求完整性的(JSON/协议/文件名)请放大缓冲, 别用这个。
 * ==========================================================================*/
#include <stddef.h>
#include <string.h>

static inline void str_trunc(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#endif /* STR_TRUNC_H */
