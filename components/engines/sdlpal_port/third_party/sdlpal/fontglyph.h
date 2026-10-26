/* fontglyph.h — 字模表（LinTOS 移植版）
 *
 * ⛔ 与原版 SDLPal 的区别：
 *   原版在这里定义 `static unsigned char unicode_font[65536][32] = {...}`，
 *   是 **2MB 可写数据**（运行时要往里写 ASCII 字形和 wor16.fon 的字）。
 *   这在 ESP32-S3 上直接爆炸：DRAM 只有 512KB，链接报
 *   "dram0_0_seg overflowed by 2.8MB"；加 const 又会因为运行时写入而崩。
 *   ⇒ 字模表已用 tools/gen_pal_fontbin.py 提取成 fontglyph.bin（2MB），
 *     放 TF 卡 /sdcard/pal/，运行时由 font.c 读进 PSRAM。
 *
 * font_width[] 只有 64KB，留在源码里没问题。
 */
#ifndef _FONTGLYPH_H
#define _FONTGLYPH_H

#ifndef _FONT_C
#error "This file should only be included inside font.c!"
#endif

#define UNICODE_FONT_COUNT  65536

/* 从原文件保留: 自定义字的码位映射基址 */
static const int unicode_lower_top  = 0xd800;
static const int unicode_upper_base = 0xf900;
static const int unicode_upper_top  = 65534;

/* font_width[65536] 已外置到 TF 卡 fontglyph.bin 的后 64KB，见 font.c */


#endif /* _FONTGLYPH_H */
