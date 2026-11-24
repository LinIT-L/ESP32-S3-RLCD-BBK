/* 自动生成, 请勿手改 —— python3 tools/gen_pinyin.py */
#ifndef _IME_PINYIN_H
#define _IME_PINYIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IME_SYL_N   400
#define IME_CHAR_N  6781

extern const uint16_t ime_syl_n;
extern const uint16_t ime_char_n;

extern const char     ime_syl_blob[];
extern const uint16_t ime_syl_off[IME_SYL_N + 1];
extern const uint16_t ime_grp_start[IME_SYL_N + 1];
extern const uint16_t ime_char_cp[IME_CHAR_N];

#ifdef __cplusplus
}
#endif
#endif /* _IME_PINYIN_H */
