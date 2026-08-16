/* 电子书文本排版内核 (纯 C, 无 ESP-IDF 依赖)
 * - 解码: UTF-8 / GBK / UTF-16 LE/BE
 * - 断行: libunibreak (Unicode UAX#14, 含中文避头尾) — KOReader 同款内核
 * - 排版: 按行宽切行 + 页边界计算 (扫描与渲染共用, 保证分页一致)
 * - 本模块可在 macOS/Linux 上单独编译做一致性测试
 */
#ifndef BOOK_FLOW_H
#define BOOK_FLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BF_ENC_UTF8 = 0,
    BF_ENC_GBK = 1,
    BF_ENC_UTF16LE = 2,
    BF_ENC_UTF16BE = 3
};

enum {
    BF_CH_ASCII = 0,    /* 半角 */
    BF_CH_CJK = 1,      /* 全角 */
    BF_CH_NEWLINE = 2,
    BF_CH_TAB = 3,
    BF_CH_SKIP = 4
};

typedef struct {
    uint32_t cp;        /* UTF-8/UTF-16 模式: unicode 码点 */
    uint8_t  hi, lo;    /* GBK 模式: 双字节码 */
    uint8_t  kind;
    uint8_t  adv;       /* 消耗源字节数 */
} bf_ch_t;

#define BF_MAX_WIN   256           /* 断行上下文窗口 (字符数): 24px 每页 ~192 字, 256 足够 */
#define BF_MAX_LINES 40            /* 单页最大行数 (防御) */

/* UTF-8 合法性校验 (整段) */
bool bf_utf8_valid(const uint8_t *p, size_t n);

/* 码点 -> UTF-8, 返回字节数 (1..4) */
int bf_cp_to_utf8(uint32_t cp, uint8_t *out);

/* 解码下一个字符 (p 指向源, end 为源末尾) */
bf_ch_t bf_next_ch(const uint8_t *p, const uint8_t *end, uint8_t enc);

/* 把窗口字符序列编码成 UTF-8 (换行保留, TAB/控制符 -> 空格), 返回长度 */
size_t bf_build_utf8(const bf_ch_t *win, int cnt, uint8_t *out, size_t cap);

/* 计算每个字符后的断行许可: brk[i] = 0 不可断 / 1 可断 / 2 必须断
 * GBK 模式不启用 UAX#14 (brk 全 0, 纯宽度换行).
 * scratch 为工作区, 需 >= cnt*8+8 字节 */
void bf_breaks(const bf_ch_t *win, int cnt, uint8_t enc,
               uint8_t *brk, uint8_t *scratch, size_t scratch_cap);

/* 排版窗口 [0,cnt): 切出最多 rows 行, 每行结束下标(不含)写入 line_end,
 * line_count 为已排版行数, boundary 为页边界字符下标 (下一行起点, 或 cnt=窗口尽头).
 * brk 为 bf_breaks 的输出 (可为 NULL, 等价于全 0 纯宽度换行).
 * ascii_w/cjk_w 为半角/全角像素宽 (来自当前字库). */
void bf_layout(const bf_ch_t *win, int cnt, const uint8_t *brk,
               int rows, int line_max, int ascii_w, int cjk_w,
               int *line_end, int *line_count, int *boundary);

/* ---- 顺序分页 (扫描器内核, 固件与主机测试共用) ---- */

typedef struct {
    /* 从 pos 读取最多 want 字节到 buf, 返回实际字节数 (0 = EOF/出错) */
    size_t (*read_at)(void *ud, uint32_t pos, uint8_t *buf, size_t want);
    void *ud;
} bf_src_t;

/* 追加一页起始偏移 (返回 0 成功) */
typedef int (*bf_add_page_t)(void *ud, uint32_t off);

/* 每页内字符回调 (章节检测等), 每个字符只回调一次 */
typedef void (*bf_on_char_t)(void *ud, const bf_ch_t *ch, uint32_t pos);

/* 从 start_off 顺序分页到 fsz, 页偏移通过 add_page 输出.
 * chunk: 读缓冲; win/brk/scratch/win_off: 工作区 (win>=BF_MAX_WIN*sizeof(bf_ch_t),
 * brk>=BF_MAX_WIN, scratch>=BF_MAX_WIN*8+8, win_off>=BF_MAX_WIN*4).
 * rows/line_max 为当前布局参数.
 * limit: 阶段检查点 — 分页推进到 >= limit 的页边界即返回 (供后台任务分步扫描,
 *        不改变页边界结果; 传 fsz 则一次扫完).
 * resume_open: 续扫模式 (侧边索引部分落盘后继续), 不再重复记录起始页.
 * 返回 0 成功; *out_next 为下次起始偏移. */
int bf_paginate(bf_src_t src, uint32_t fsz, uint8_t enc, uint32_t start_off,
                int rows, int line_max, int ascii_w, int cjk_w,
                bf_add_page_t add_page, void *add_ud,
                bf_on_char_t on_char, void *on_ud,
                uint8_t *chunk, uint32_t chunk_cap,
                bf_ch_t *win, uint8_t *brk, uint8_t *scratch, uint32_t scratch_cap,
                uint32_t *win_off,
                uint32_t limit, bool resume_open, uint32_t *out_next);

#ifdef __cplusplus
}
#endif

#endif /* BOOK_FLOW_H */
