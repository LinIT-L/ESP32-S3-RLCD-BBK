#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include "st7305.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内置壁纸程序 id (V4: 仅保留星空, 由 menu_system 屏保实现; 其余动态壁纸已删除) */
#define WP_PROG_STARS    0
#define WP_PROG_COUNT    1

const char *wp_prog_name(int id);

/* 渲染一帧到 LCD (内部 1bpp PSRAM 缓冲 + flush) */
void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms);

/* 释放壁纸 PSRAM 帧缓冲 (退出壁纸时调用, 下次渲染自动重新分配) */
void wp_release_buffers(void);
/* ==== 屏保状态机 (从 os_mgr 迁移归位: 渲染+状态同属壁纸模块) ==== */
bool os_screensaver_active(void);
void os_screensaver_reset(void);
void os_screensaver_force_enter(void);
/* 每帧主循环调用: 任意界面空闲>设置延时进入星空屏保; 离开/弹窗/新输入退出.
 * 游戏界面经 input_mark_activity 刷新 last_input_ms, 故"正在玩不睡, 放下睡".
 * 参数 by 值, 避免 wallpapers 依赖 os 头 (无循环依赖). */
void wp_screensaver_poll(uint32_t now_ms, uint32_t last_input_ms, bool in_dialog);
/* 当前息屏空闲延时 (ms); min<=0(自动不息眠) 返回 UINT32_MAX */
uint32_t wp_screensaver_delay_ms(void);
/* 运行中更新息屏延时缓存 (设置休眠时间后调用; ms 为息屏空闲延时) */
void wp_screensaver_set_delay_ms(uint32_t ms);
/* 运行中更新壁纸(屏保)运行时长缓存 (壁纸时间设置后调用; ms 为壁纸运行时长, 超时软关机) */
void wp_screensaver_set_run_ms(uint32_t ms);
/* 关机时间缓存 (0.5 策略无该行为; 为兼容 0.6 设置页保留空接口, 不产生关机动作) */
void wp_screensaver_set_shutdown_ms(uint32_t ms);
/* 壁纸运行时长已耗尽, 主循环应执行软关机 (清屏 + deep sleep) */
bool wp_screensaver_shutdown_requested(void);
/* 触摸唤醒屏保开关 (V1.5.x): 开=触摸长按1秒唤醒屏保; 关=触摸不唤醒, 仅物理键/手柄唤醒.
 * NVS "os_wp"/"wp_touch" (1=开, 默认 0=关). 由 os_core 屏保触摸唤醒路径查询, 设置页修改. */
bool wp_screensaver_touch_wake_enabled(void);
void wp_screensaver_set_touch_wake(bool en);


/* 屏保独占 LCD (壁纸模块内用) 见 board_rlcd.h.
 * ==== 独立屏保监视任务 (休眠最高优先级, 需在开机时调用一次) ====
 * 创建独立任务, 不受任何页面/游戏阻塞: 空闲超时强制进壁纸 + 到点强制软关机.
 * 在 os_init 拿到 LCD 后调用一次. */
void wp_screensaver_supervisor_start(st7305_handle_t *lcd);


#ifdef __cplusplus
}
#endif

#endif
