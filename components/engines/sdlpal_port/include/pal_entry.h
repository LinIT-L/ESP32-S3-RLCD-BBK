/* pal_entry.h — 给终端/OS 用的 SDLPal 启动接口
 *
 * 用法（终端 `pal` 命令）：
 *     if (pal_launch()) { ...进入游戏显示态... }
 * 每个 UI 帧：
 *     pal_tick(lcd);         // 有脏帧才真正转换上屏，无脏帧开销≈0
 * 退出：
 *     pal_stop();
 */
#ifndef PAL_ENTRY_H
#define PAL_ENTRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pal_launch(void);        /* 起任务跑 SDLPal; 返回 false = 数据目录不在/内存不足 */
bool pal_is_running(void);
/* 本次启动时刻(us): 载入各阶段的 0 点 (量真实耗时用) */
int64_t pal_launch_us(void);
void pal_stop(void);          /* 请求退出（游戏会在下一次主循环检查） */
/* 请求退出并**等它真的结束**; 超时强杀任务. true=自己退出, false=被强杀 */
bool pal_stop_and_wait(int timeout_ms);
/* 全量退出: 停音频(RIX 音序器) + 清 PCM + 杀游戏任务 + 兜底清残留任务。
 * 退出仙剑必须走这个, 只杀游戏任务会留下"回桌面还在放音乐"。 */
void pal_shutdown(void);
void pal_tick(void *lcd);     /* 每帧调用：8bpp -> 1bpp 上屏 */

/* 抽象键码：页面层只认这 8 个，不用包含 SDL 头。
 * SDLPal 的 PAL_KeyDown() 看的是"当前是否按住"，所以必须成对发 down=1/0。 */
typedef enum {
    PAL_KEY_UP = 0,
    PAL_KEY_DOWN,
    PAL_KEY_LEFT,
    PAL_KEY_RIGHT,
    PAL_KEY_OK,      /* 回车 / 对话 / 确认 */
    PAL_KEY_MENU,    /* ESC / 菜单 / 取消 */
    PAL_KEY_PGUP,    /* 列表上翻 */
    PAL_KEY_PGDN,    /* 列表下翻 */
    PAL_KEY_NONE
} pal_key_t;

void pal_input(pal_key_t k, int down);

/* 虚拟按键：直接用系统的 components/ui/virtual_keys（GBC/NES 同款布局）。
 * 进游戏开、退游戏关；绘制由 pal_port_present 内部完成，页面只负责 poll 取键值。 */
void pal_keys_enable(bool on);

/* 每帧取一次手柄/屏幕虚拟按键状态 → 转成仙剑按键 (含按下/松开两个沿)。
 * 虚拟手柄是"joypad 位"而不是 OS 动作，必须由这里桥接，否则画出来也按不动。 */
void pal_poll_keys(void);

const char *pal_entry_datadir(void);
void        pal_entry_set_datadir(const char *d);

/* 仙剑单独限帧 (0=不限)。默认 20fps: 主循环里 pal_tick 是"超时跳过",
 * 不阻塞输入/音频; 面板本身也只有 16~24Hz, 再高纯属白耗 CPU 并把主任务占满。 */
void pal_set_max_fps(int fps);
int  pal_get_max_fps(void);

#ifdef __cplusplus
}
#endif
#endif /* PAL_ENTRY_H */
