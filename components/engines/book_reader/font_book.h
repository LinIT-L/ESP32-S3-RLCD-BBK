#ifndef FONT_BOOK_H
#define FONT_BOOK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个字形: 1bpp 行存储位图, 位=1 为字 (黑) */
typedef struct {
    uint8_t        w;
    uint8_t        h;
    const uint8_t *bitmap;
} book_glyph_t;

/* 校验内嵌字库头 (幂等, 可在启动时调用一次) */
bool font_book_init(void);

/* 半角 ASCII 查找 (0x20..0x7E), 返回 8x16 位图 */
bool font_book_glyph_ascii(uint8_t c, book_glyph_t *out);

/* GB2312/GBK 双字节码查找, 返回 16x16 位图 */
bool font_book_glyph_gb(uint8_t hi, uint8_t lo, book_glyph_t *out);

/* Unicode 码点查找 (UTF-8 文本解码后使用), 返回 16x16 位图 */
bool font_book_glyph_unicode(uint32_t cp, book_glyph_t *out);

/* 字库度量 (阅读器按此动态排版) */
int font_book_cell_w(void);
int font_book_cell_h(void);
int font_book_ascii_w(void);
int font_book_ascii_h(void);

/* 切换字体: style 0=仿宋 1=黑体(菜单字体款式); size 0=16 1=24 2=32 (全原生简体点阵) */
void font_book_select(int style, int size_id);

/* V1.0.88: 从 TF 卡加载 .fnt 字体文件 (路径如 /sdcard/fonts/hei24.fnt)
 * 加载到 PSRAM, 成功后后续渲染均用此字库.
 * 返回 true=加载成功, false=失败 (文件不存在/格式错误/内存不足) */
bool font_book_select_file(const char *path);

/* V1.0.88: 扫描 TF 卡 /sdcard/fonts/ 下的 .fnt 文件
 * names: 输出文件名列表 (不含路径, 含 .fnt 后缀)
 * max: 最多扫描多少个
 * 返回: 实际找到的文件数 */
int font_book_scan_sd(char names[][64], int max);

/* V1.0.88: 当前是否使用 TF 卡字库 */
bool font_book_is_sd_active(void);

/* V1.0.88: 切回内嵌字库 (退出 SD 字库模式) */
void font_book_select_embedded(int style, int size_id);

/* 字库校验失败的说明 (调试用) */
const char *font_book_error(void);

#ifdef __cplusplus
}
#endif

#endif
