#ifndef FONT_BOOK_H
#define FONT_BOOK_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* size_t —— font_book_short_name() 的缓冲长度 */

#ifdef __cplusplus
extern "C" {
#endif

/* 一个字形: 1bpp 行存储位图, 位=1 为字 (黑) */
typedef struct {
    uint8_t        w;
    uint8_t        h;
    const uint8_t *bitmap;
} book_glyph_t;

/* 统一字体后端（阅读器唯一取字入口）
 *
 * 自 V1.1 起，阅读器正式运行格式只有 **LBF**；内置 UI 字库（font_zh16/font_zh）
 * 仅作为 TF 卡上没有 LBF 时的兜底数据源。两条路径共用同一个取字函数，
 * 不再存在 .fnt 解析器。
 */

/* 幂等初始化。**不覆盖已经选择的字体**（旧实现在此强制切回 UI 字体，
 * 导致 page_book 预选的 TF 字体在打开书时被静默丢弃）。 */
bool font_book_init(void);

/* 半角 ASCII 查找 (0x20..0x7E) */
bool font_book_glyph_ascii(uint8_t c, book_glyph_t *out);

/* GB2312/GBK 双字节码查找 */
bool font_book_glyph_gb(uint8_t hi, uint8_t lo, book_glyph_t *out);

/* Unicode 码点查找（UTF-8 文本解码后使用） */
bool font_book_glyph_unicode(uint32_t cp, book_glyph_t *out);

/* 字库度量（阅读器按此动态排版） */
int font_book_cell_w(void);
int font_book_cell_h(void);
int font_book_ascii_w(void);
int font_book_ascii_h(void);

/* 切换内置字体：size_id 为**目标像素**（16/24/32）。
 * 规则：只允许原生尺寸或其严格 2 倍，其它值向上收敛到最近的合法档。 */
void font_book_select(int style, int size_id);

/* 从 TF 卡加载 LBF 字体（如 /sdcard/fonts/ark12.lbf）。
 * 成功返回 true；失败返回 false 并保留原字体不变（不静默替换）。 */
bool font_book_select_file(const char *path);

/* 2026-10-30: 当前正在使用的 LBF 绝对路径（内置字库时为空串）。
 * 商店卸载前用它判断"有没有正在用的字体"，避免删掉正在读的字库。 */
const char *font_book_current_file(void);

/* 2026-10-31: 释放全部字体实例（当前槽 + PSRAM 缓存槽）。关书时调用。 */
void font_book_release_all(void);

/* 2026-10-31: 字体显示名 → 中文短名 (≤6 字, 保留字号数字)。
 * 阅读器菜单与书架「字体」滚轮共用这一份实现, 避免两处名字不一致。
 * 只影响显示, 不改 LBF 内部名、不改持久化路径。 */
const char *font_book_short_name(const char *raw, char *buf, size_t cap);

/* 扫描 TF 卡 /sdcard/fonts/ 下的 .lbf 文件 */
int font_book_scan_sd(char names[][64], int max);

/* 当前是否使用 TF 卡 LBF 字体 */
bool font_book_is_sd_active(void);

/* 切回内置字库并释放 LBF 资源 */
void font_book_select_embedded(int style, int size_id);

/* 字库错误说明（失败原因，供 UI 提示/日志） */
const char *font_book_error(void);

/* 当前字体名（LBF 为文件内名称，内置为 "内置宋体"） */
const char *font_book_current_name(void);

/* 当前字体的原生像素尺寸与倍率 */
int font_book_native_px(void);
int font_book_scale(void);

/* 当前可用字号列表（原生 / 2 倍），返回个数，写入 out */
int font_book_size_options(int *out, int max);

#ifdef __cplusplus
}
#endif

#endif
