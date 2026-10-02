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

/* ==== 壁纸模块注册表 (壁纸模块化: 各壁纸通过注册表分发, 便于以后从商店下载新壁纸) ==== */
typedef void (*wp_render_fn)(st7305_handle_t *lcd, uint32_t now_ms);
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
void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms);

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

#endif
