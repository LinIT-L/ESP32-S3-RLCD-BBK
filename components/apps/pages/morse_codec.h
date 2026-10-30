/**
 * morse_codec.h — 摩斯电码 码表 / 编解码 / 时间轴 (纯逻辑, 无 ESP-IDF 依赖)
 *
 * 全应用**唯一码表数据源**: 编码、解码、查表、试听、播放都从这里取,
 * 不存在两套映射对不上的可能。
 *
 * 设计要点:
 *   ① 54 项 (26 字母 + 10 数字 + 18 标点), 最长 7 个符号 ($ = ...-..-);
 *   ② 紧凑编码: 每字符 2 字节 (n=符号数, bits=每位 0 点/1 划), 48 字符 = 96B;
 *   ③ 不支持的字符**不静默丢弃**: 记入 bad[]/bad_cnt, 页面负责提示;
 *   ④ 本文件可被主机端 gcc 直接编译 (tools/test_morse_core.c), 不依赖 FreeRTOS.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MR_TEXT_MAX   48                        /* 文本上限 (字符), 明确且有边界 */
#define MR_CH_MAX     MR_TEXT_MAX
#define MR_MARK_MAX   7                         /* 单字符最长符号数 ($) */
/* 元素上限: 每字符最坏 = 7 符号 + 6 符号间隔 + 1 字符间隔 = 14; 另加词间隔与结尾 */
#define MR_MAX_EL     (MR_CH_MAX * 14 + 2)

/* 一个字符的紧凑编码 */
typedef struct {
    uint8_t n;      /* 符号数: 0=词间隔(空格) */
    uint8_t bits;   /* bit i: 0=点 1=划 (i<n) */
    uint8_t bad;    /* 1=不支持的字符 (不播放, 显示为 '?') */
} mr_ch_t;

typedef struct {
    uint8_t n;                      /* 字符数 (含空格) */
    mr_ch_t ch[MR_CH_MAX];
    uint8_t bad_cnt;                /* 不支持字符个数 */
    char    bad[8];                 /* 前 8 个不支持字符 (供提示) */
} mr_plan_t;

typedef enum {
    MR_EL_MARK = 0,   /* 发声元素 */
    MR_EL_GAP_E,      /* 符号间隔 (1u) */
    MR_EL_GAP_C,      /* 字符间隔 (3u) */
    MR_EL_GAP_W,      /* 词间隔补足 (4u; 与前一字符的 3u 合计 7u) */
    MR_EL_END,
} mr_el_kind_t;

typedef struct {
    uint8_t  kind;    /* mr_el_kind_t */
    uint8_t  dash;    /* 仅 MR_EL_MARK: 1=划 */
    uint16_t ms;      /* 该元素时长 (毫秒) */
} mr_el_t;

/* ---------- 码表 ---------- */
const char *mr_encode(char ch);          /* NULL = 不支持 */
char        mr_decode(const char *pat);  /* "-.-" → 'K'; 0 = 非法序列 */
uint8_t     mr_charset_count(void);      /* 码表项数 (54) */
char        mr_charset_at(uint8_t i);    /* 第 i 项的字符 */
const char *mr_charset_code(uint8_t i);  /* 第 i 项的点划 (查表页直接用) */

/* ---------- 时间轴 ---------- */
uint16_t mr_unit_ms(uint8_t wpm);        /* 1 单位 = 1200/WPM, 夹到 5..35 WPM */

/* 文本 → 计划. 返回 0=成功; -1=参数错; -2=超长(>MR_TEXT_MAX).
 * 不支持字符不导致失败, 记入 bad_cnt/bad[]。 */
int mr_build_plan(const char *text, mr_plan_t *p);

/* 计划 → 元素表. 返回元素个数(含 END); <0=容量不足(不截断)。total_ms 可空。 */
int mr_expand(const mr_plan_t *p, uint8_t wpm, mr_el_t *out, int cap, uint32_t *total_ms);

/* 计划 → 显示串 (".... . .-.. .-.. --- / .-- --- .-. .-.. -..")
 * 词间用 " / "; 不支持字符显示 '?'。返回长度; <0=容量不足(不写半截)。 */
int mr_fmt_morse(const mr_plan_t *p, char *out, int cap);

/* 发声字符数 (不含空格/不支持项) —— 进度分母 */
int mr_char_count(const mr_plan_t *p);

#ifdef __cplusplus
}
#endif
