#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include "st7305.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内置壁纸程序 id */
#define WP_PROG_STARS     0     /* 星空 (动态) */
#define WP_PROG_LIFEGUIDE 1     /* 人生指南 (静态文字, 每分钟随机换一条) */
#define WP_PROG_COUNT     2

/* ★ 2026-10-21: 休眠壁纸 —— 深睡(休眠到点)时留在屏上的整屏画面: 黑底 + 居中图。
 * 手动关机不走这里 (关机一律刷白, 不显示任何东西)。图数据见 wp_sleep_default.h。 */
void wp_sleep_wallpaper_draw(st7305_handle_t *lcd);

/* 人生指南壁纸是否可用 (应用已安装): 壁纸列表据此决定是否显示该项 */
bool wp_lifeguide_installed(void);

/* ==== 壁纸模块注册表 (壁纸模块化: 各壁纸通过注册表分发, 便于以后从商店下载新壁纸) ====
 * ★ 渲染返回值: 1 = 本帧有新内容, 需要 flush 上屏; 0 = 没有变化, 不要刷屏。
 *   静态壁纸(人生指南)靠它做到"一分钟只刷一次", 避免每帧 15000B 白耗电。 */
typedef int (*wp_render_fn)(st7305_handle_t *lcd, uint32_t now_ms);
typedef struct {
    int id;
    const char *name;
    wp_render_fn render;
} wp_module_t;

void wp_register_module(const wp_module_t *m);
const wp_module_t *wp_get_module(int id);
const wp_module_t *wp_get_module_by_index(int idx);
int wp_get_module_count(void);

const char *wp_prog_name(int id);

/* 渲染一帧到 LCD (内部 1bpp PSRAM 缓冲 + flush) */
int  wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms);   /* 1=有新内容需送屏 */

/* 释放壁纸 PSRAM 帧缓冲 (退出壁纸时调用, 下次渲染自动重新分配) */
void wp_release_buffers(void);
/* ==== 屏保状态机 (从 os_mgr 迁移归位: 渲染+状态同属壁纸模块) ==== */
bool os_screensaver_active(void);
void os_screensaver_reset(void);
void os_screensaver_force_enter(void);
/* 当前息屏空闲延时 (ms); min<=0(自动不息眠) 返回 UINT32_MAX */
uint32_t wp_screensaver_delay_ms(void);
/* 运行中更新息屏延时缓存 (设置息屏时间后调用) */
void wp_screensaver_set_delay_ms(uint32_t ms);
/* 设置页"关机时间"档位写入点: 本版本不使用该档位(自动关机由"休眠时间"驱动), 值忽略。 */
void wp_screensaver_set_shutdown_ms(uint32_t ms);
/* 设置页"休眠时间"档位写入点: 屏保播满该时长 → 自动关机(任意键唤醒)。 */
void wp_screensaver_set_run_ms(uint32_t ms);
/* 屏保已播满"休眠时间" → 主循环执行自动关机(深睡, 任意键唤醒)。 */
bool wp_screensaver_shutdown_requested(void);
/* 触摸唤醒屏保开关 (V1.5.x): 开=触摸长按1秒唤醒屏保; 关=触摸不唤醒, 仅物理键/手柄唤醒.
 * NVS "os_wp"/"wp_touch" (1=开, 默认 0=关). 由 os_core 屏保触摸唤醒路径查询, 设置页修改. */
bool wp_screensaver_touch_wake_enabled(void);
void wp_screensaver_set_touch_wake(bool en);


/* 屏保独占 LCD (壁纸模块内用) 见 board_rlcd.h.
 * ==== 独立屏保监视任务 (需在开机时调用一次) ====
 * 创建独立任务, 不受任何页面/游戏阻塞: 空闲超时强制进壁纸并独占渲染。
 * 本任务不产生任何关机动作。 */
void wp_screensaver_supervisor_start(st7305_handle_t *lcd);
/* 关机前调用: 挂起屏保监督任务(PSRAM 栈持续刷屏) + 解除屏保独占,
 * 避免深睡禁用 flash/PSRAM 缓存瞬间撞断言 → 关机变重启。 */
void wp_screensaver_halt(void);


#ifdef __cplusplus
}
#endif

/* ★ 2026-10-20: 页面级"别息屏"占位。on=true 期间不因空闲进屏保/壁纸;
 *   置回 false 时**重新开始算空闲时长**(不会一松手就息屏)。可重入调用(同值无操作)。 */
void wp_idle_hold(bool on);
bool wp_idle_held(void);

#endif
