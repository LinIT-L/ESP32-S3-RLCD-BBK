/* EPUB 转换器: miniz 解压 + 流式 HTML 文本提取 -> 缓存 TXT.
 * 章节按 spine 顺序输出, 每个文档的 <h1-3> 作为标题行. */
#ifndef EPUB_CONVERT_H
#define EPUB_CONVERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int epub_convert(const char *src, const char *dst, char *title, size_t title_cap);

#ifdef __cplusplus
}
#endif

#endif /* EPUB_CONVERT_H */
