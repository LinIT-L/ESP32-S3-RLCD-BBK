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

/* 绑定主屏句柄 (main.c 在 LCD 初始化之后调用; 传 st7305_handle_t* 或 NULL).
 * 供 `fbinfo` / `fbdump` 抓屏命令读取帧缓冲 —— 这样真机和 QEMU 都能抓屏,
 * 不需要额外的显示适配层。传 void* 是为了不让本文件依赖 st7305 头。 */
void remcon_set_lcd(void *lcd);

/* 远程关机 (深度休眠): main.c 实现。
 * auto_mode=1 → 休眠到点自动关机路径(任意键唤醒, 不显示 Logo);
 * auto_mode=0 → 手动关机路径(仅 PWR 键, 须按住 0.5s 开机)。 */
void os_remote_shutdown(int auto_mode);

#endif /* REMCON_H */