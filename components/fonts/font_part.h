/**
 * font_part.h — 字库分区加载 (components/fonts).
 *
 * 字库二进制从固件迁移到 appdata 分区字库区 (分区内偏移 4MB 起, 见 merge_flash.sh):
 *   槽1  appdata+0x400000  font_zh.bin       (粗体 24px, 槽 512KB)
 *   槽2  appdata+0x480000  font_zh_light.bin (细体 24px, 槽 512KB)
 *   槽3  appdata+0x500000  font_zh16.bin     (宋体 16px, 槽 256KB)
 *   槽4  appdata+0x540000  book16_fs.fnt     (电子书仿宋 16, 槽 384KB)
 *   槽5  appdata+0x5A0000  book24_fs.fnt     (电子书仿宋 24, 槽 640KB)
 *   槽6  appdata+0x660000  book24_hz.fnt     (电子书黑体 24, 槽 640KB)
 *   槽7  appdata+0x720000  book32_fs.fnt     (电子书仿宋 32, 槽 1MB)
 *   槽8  appdata+0x820000  lav_font.bin      (文曲星字库, 槽 512KB)
 * 字库区结束 0x8A0000, 之后为 os_db 区.
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

/* 槽位偏移与长度 (固定槽位, 与 merge_flash.sh 一致) */
#define FONT_SLOT_ZH_OFF   (FONT_REGION_OFF)                    /* font_zh.bin       */
#define FONT_SLOT_LIGHT_OFF (FONT_REGION_OFF + 0x80000UL)       /* font_zh_light.bin */
#define FONT_SLOT_16_OFF   (FONT_REGION_OFF + 0x100000UL)       /* font_zh16.bin     */
#define FONT_SLOT_BK16_OFF (FONT_REGION_OFF + 0x140000UL)       /* book16_fs.fnt     */
#define FONT_SLOT_BK24_OFF (FONT_REGION_OFF + 0x1A0000UL)       /* book24_fs.fnt     */
#define FONT_SLOT_BK24HZ_OFF (FONT_REGION_OFF + 0x260000UL)     /* book24_hz.fnt     */
#define FONT_SLOT_BK32_OFF (FONT_REGION_OFF + 0x320000UL)       /* book32_fs.fnt     */
#define FONT_SLOT_LAV_OFF  (FONT_REGION_OFF + 0x420000UL)       /* lav_font.bin      */

#define FONT_SLOT_ZH_LEN   0x80000UL
#define FONT_SLOT_16_LEN   0x40000UL
#define FONT_SLOT_BK16_LEN 0x60000UL
#define FONT_SLOT_BK24_LEN 0xA0000UL
#define FONT_SLOT_BK32_LEN 0x100000UL
#define FONT_SLOT_LAV_LEN  0x80000UL

/* 映射 appdata 分区内 [off, off+len) 区域为内存指针 (只读).
 * 相同 off+len 的重复调用返回已映射指针 (不重复 mmap).
 * 失败返回 NULL (未找到分区/映射失败). 生命周期为整个系统运行期. */
const uint8_t *font_part_map(size_t off, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FONT_PART_H */
