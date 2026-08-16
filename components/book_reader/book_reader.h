#ifndef BOOK_READER_H
#define BOOK_READER_H

#include <stdbool.h>
#include <stdint.h>

#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开电子书 (从 /sdcard/books 下的 txt 文件), 成功返回 true
 * 打开期间会启动麦克风敲击翻页 (失败不阻塞, 按键仍可用) */
bool book_reader_open(const char *path);

bool book_reader_is_open(void);

/* 关闭阅读器: 释放文本缓冲 + 停止麦克风 */
void book_reader_close(void);

/* 阅读器按键处理 (menu_action_t 值); 返回 true = 已消费 */
bool book_reader_handle_action(int action);

/* V1.0.68: 阅读器触摸命中 (屏幕坐标 400x300, 已含旋转映射).
 * 上半页=上一页, 下半页=下一页, 中间=进设置菜单. 返回 true=已消费. */
bool book_reader_handle_touch(int x, int y);

/* 渲染当前页 (含状态栏/页码) */
void book_reader_render(st7305_handle_t *lcd);

/* 主循环每帧轮询: 处理麦克风敲击翻页, 返回 true 表示页面已变化 */
bool book_reader_poll(void);

/* 当前文件名 (无路径, 用于标题) */
const char *book_reader_title(void);

/* 敲击检测是否已激活 (麦克风可用) */
bool book_reader_knock_active(void);

/* 应用电子书设置: 敲击翻页 / 灵敏度 / 夜间模式 / 显示页码 / 旋转方向
 * / 字体大小(0=20 1=24 2=28 3=32) / 边距(0窄 1中 2宽) / 行高(0紧凑 1标准 2宽松) / 字距(0标准 1宽松)
 * 在打开书籍前或设置变更时调用 */
void book_reader_set_settings(bool knock, int sens, bool night, bool pagenum, int rot,
                              int fontsize, int margin, int lineh, int gap);

#ifdef __cplusplus
}
#endif

#endif
