/**
 * ime.h — 中文输入法内核 (无词库, 单字全拼).
 *
 * 定位: 只做"拼音串 → 候选字 → 上屏"这一件事, 不画界面、不碰页面缓冲.
 *   界面由 kbd_ui.c 的第 0 排候选栏负责 (方案 B: 中文模式下数字行让给候选栏);
 *   上屏结果通过 sink 回调交给页面自己插入光标处, 所以任何页面都能接.
 *
 * 单例: 同一时刻只有一个键盘在用, 所以状态是全局的, 不需要传上下文.
 *   页面进入/退出输入场景时调 ime_set(false) 复位, 避免状态串到下一个页面.
 *
 * 数据: 400 个无调音节 / 6781 个汉字, 16.5KB, 全部 const 走 flash XIP, 不占 RAM.
 *   表由 tools/gen_pinyin.py 从固件实际字库 font_zh.bin 生成 —— 候选字保证字库里有,
 *   不会出现"输入法能打、屏幕没字"。
 *
 * 典型接法 (以终端/编辑框为例):
 *     ime_sink(my_insert_utf8);            // 文字最终去处的回调
 *     ...
 *     case KBD_ACT_CHAR: {
 *         char c = kbd_char(&k, idx);
 *         if (ime_char(c)) break;          // 被输入法吃掉(在拼拼音)
 *         my_insert_char(c);               // 否则照常插入英文/数字
 *     } break;
 *     case KBD_ACT_SPACE:
 *         if (ime_pending()) { ime_pick(0); break; }   // 空格 = 上屏第一个候选
 *         my_insert_char(' ');
 *         break;
 *     case KBD_ACT_BKSP:
 *         if (ime_bksp()) break;           // 有拼音先删拼音字母
 *         my_delete_char();
 *         break;
 */
#ifndef _IME_H
#define _IME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IME_PY_MAX    23   /* 拼音串最大长度 (最长音节 zhuang=6, 留余量) */
#define IME_CAND_MAX  32   /* 候选上限 (屏幕一行放得下 22 个, 多出来的先不翻页) */

/* 上屏出口: 交出 UTF-8 字符串 (3 字节 + 0). 页面在这里把字插到光标处. */
typedef void (*ime_out_cb_t)(const char *utf8);

void ime_sink(ime_out_cb_t cb);          /* 注册上屏出口; NULL = 注销 */

bool ime_on(void);                       /* 中文模式是否开启 */
void ime_set(bool on);                   /* 开/关中文模式 (关会清空未上屏拼音) */
void ime_toggle(void);

bool ime_pending(void);                  /* 是否有未上屏的拼音 */
const char *ime_pinyin(void);            /* 当前拼音串 (无则空串) */

/* 键入一个字符
 *   true  = 输入法消费了 (调用方不要再插入)
 *   false = 没消费 (调用方照常插入; 注意: 若本来有拼音, 会先自动上屏第一候选) */
bool ime_char(char c);

/* 退格: true = 删掉了一个拼音字母; false = 没拼音可删, 调用方自己删字符 */
bool ime_bksp(void);

int  ime_cand_n(void);                   /* 候选数量 */
int  ime_cand_cp(int idx);               /* 第 idx 个候选的 Unicode 码点, 越界 -1 (给 UI 画字用) */
const char *ime_cand_utf8(int idx);      /* 第 idx 个候选的 UTF-8 (静态缓冲, 越界 NULL) */
bool ime_pick(int idx);                  /* 上屏第 idx 个候选 */

void ime_clear(void);                    /* 丢弃未上屏拼音 (不输出) */

/* 码点 → UTF-8 (3 字节 + 0). out 至少 4 字节. */
void ime_cp_to_utf8(int cp, char out[4]);

#ifdef __cplusplus
}
#endif
#endif /* _IME_H */
