#ifndef BOOK_READER_H
#define BOOK_READER_H

#include <stdbool.h>
#include <stdint.h>

#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开电子书 (从 /sdcard/books 下的 txt 文件), 成功返回 true */
bool book_reader_open(const char *path);

bool book_reader_is_open(void);

/* 关闭阅读器: 释放文本缓冲 */
void book_reader_close(void);

/* 阅读器按键处理 (menu_action_t 值); 返回 true = 已消费 */
bool book_reader_handle_action(int action);

/* V1.0.68: 阅读器触摸命中 (屏幕坐标 400x300, 已含旋转映射).
 * 上半页=上一页, 下半页=下一页, 中间=进设置菜单. 返回 true=已消费. */
bool book_reader_handle_touch(int x, int y);

/* 渲染当前页 (含状态栏/页码) */
void book_reader_render(st7305_handle_t *lcd);

/* 主循环每帧轮询 (保留接口, 返回 false) */
bool book_reader_poll(void);

/* 当前文件名 (无路径, 用于标题) */
const char *book_reader_title(void);

/* 麦克风敲击检测已移除, 恒返回 false */
bool book_reader_knock_active(void);

/* ★ 2026-11-22: 阅读页震动开关 (「书库 → 阅读优化 → 震动」)。
 *   false = 阅读器内点按/翻页不震 (只挡点击反馈, 不动设置页的「UI 震动」)。
 *   打开书籍前下发; 阅读器内部会在打开/关闭时自动应用与复位. */
void book_reader_set_page_vib(bool en);
bool book_reader_page_vib(void);

/* 应用电子书设置: 夜间模式 / 显示页码 / 旋转方向
 * / 字体大小(0=20 1=24 2=28 3=32) / 边距(0窄 1中 2宽) / 行高(0紧凑 1标准 2宽松)
 * / 字距(0标准 1宽松) / 段落缩进(0关 1开)
 * 在打开书籍前或设置变更时调用 */
void book_reader_set_settings(bool knock, int sens, bool night, bool pagenum, int rot,
                              int fontstyle, int fontsize, int margin, int lineh, int gap,
                              int indent);

/* ★ 2026-11-21: 已删除 book_reader_set_external_menu / book_reader_menu_* 共 7 个
 *   声明 —— 它们在 .c 里没有任何定义, 全项目也没有任何调用 (外部菜单方案已废弃,
 *   阅读器现在自绘菜单)。需要外部驱动菜单时请用 os_dialog 模板, 不要再按这套接口写。 */

#ifdef __cplusplus
}
#endif

#endif
