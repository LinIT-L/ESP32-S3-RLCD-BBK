/* FB2 (FictionBook) 流式转换器
 * 把 .fb2 的章节标题/段落提取为纯文本缓存文件, 供现有流式阅读引擎使用.
 * 参考: Porfiry-Petrovich 项目 (Python) 的 FB2 章节解析思路, C 语言流式实现.
 */
#ifndef FB2_CONVERT_H
#define FB2_CONVERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 转换 src(.fb2) -> dst(.txt 缓存). 章节标题按原样输出一行 + 空行,
 * 段落之间空行分隔. 可回传书名 (title 可为 NULL). 返回 0 成功. */
int fb2_convert(const char *src, const char *dst, char *title, size_t title_cap);

#ifdef __cplusplus
}
#endif

#endif /* FB2_CONVERT_H */
