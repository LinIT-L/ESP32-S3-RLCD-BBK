/**
 * font_part.h — 字库分区加载 (components/fonts).
 *
 * 字库二进制从固件迁移到 appdata 分区字库区 (分区内偏移 4MB 起, 见 merge_flash.sh):
 *   槽1  appdata+0x400000  font_zh.bin       (粗体 24px, 槽 512KB)
 *   槽2  appdata+0x480000  font_zh16.bin     (宋体 16px, 槽 256KB)
 *   槽3  appdata+0x4C0000  lav_font.bin      (文曲星字库, 槽 512KB)
 *   [V1.0.9x 精简: 删除电子书全部 .fnt 槽 及 24px 细宋(font_zh_light);
 *    字库区只剩 font_zh/font_zh16/lav_font 三槽; 电子书正文复用 UI 字体]
 * 字库区结束 0x540000, 之后为 os_db 区 (MACOUI_DB_OFF=0x540000).
 * 通过 esp_partition_mmap 把对应槽位映射为内存指针 (XIP 只读, 不占内部 RAM),
 * 访问方式与旧 _binary_*_start 嵌入符号一致 (返回 const uint8_t*).
 */
#ifndef FONT_PART_H
#define FONT_PART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 字库区在 appdata 分区内的起始偏移 */
#define FONT_REGION_OFF   0x400000UL

/* 槽位偏移与长度 (固定槽位, 与 merge_flash.sh 一致)
 * [V1.0.9x 精简: 移除全部电子书 .fnt 槽 及 24px 细宋(font_zh_light),
 *  lav_font 前移填回空档, os_db 随之调整以释放 appdata 尾部空间] */
#define FONT_SLOT_ZH_OFF   (FONT_REGION_OFF)                    /* font_zh.bin       */
#define FONT_SLOT_16_OFF   (FONT_REGION_OFF + 0x80000UL)       /* font_zh16.bin     */
#define FONT_SLOT_LAV_OFF  (FONT_REGION_OFF + 0xC0000UL)       /* lav_font.bin      */

#define FONT_SLOT_ZH_LEN   0x80000UL
#define FONT_SLOT_16_LEN   0x40000UL
#define FONT_SLOT_LAV_LEN  0x80000UL

/* 映射 appdata 分区内 [off, off+len) 区域为内存指针 (只读).
 * 相同 off+len 的重复调用返回已映射指针 (不重复 mmap).
 * 失败返回 NULL (未找到分区/映射失败). 生命周期为整个系统运行期. */
const uint8_t *font_part_map(size_t off, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FONT_PART_H */
