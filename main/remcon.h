/**
 * remcon.h — 串口远程控制台 (Remote Console).
 *
 * 通过 USB Serial/JTAG 控制台串口 (设备枚举为 /dev/cu.usbmodemXXXX, 即烧录口)
 * 接收 PC 端文本命令, 用于远程自动化测试/排障:
 *   - 打开任意页面 / 注入按键动作 / 注入触摸点击
 *   - 查询内存/堆/任务栈
 *   - 脚本节奏控制 (wait), 支持一行内 ';' 分隔多条命令顺序执行
 *
 * 线程模型:
 *   - remcon_init(): 创建一个低优先级读任务, 从 stdin(fd 0) 阻塞逐行读命令;
 *     UI 类命令 (page/key/tap) 放入带锁队列, 立即命令 (hello/heap/loglevel/reboot)
 *     在读任务内直接执行并回显.
 *   - remcon_tick(ctx): 主循环(os_shell_loop)每帧调用, 取出一条 UI 命令交给 os 内核执行.
 *     因此所有页面切换/按键注入都在主线程执行, 避免跨线程 UI 竞态.
 */
#ifndef REMCON_H
#define REMCON_H
#include "os.h"

/* 启动远程控制台: 建立命令队列 + 创建 stdin 读任务 (应在 os_init 之后调用). */
void remcon_init(void);

/* 主循环每帧调用: 派发一条队列内的远程命令 (page/key/tap). */
void remcon_tick(ui_ctx_t *ctx);

#endif /* REMCON_H */