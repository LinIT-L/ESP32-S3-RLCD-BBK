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

/* 人生指南壁纸是否可用 (应用已安装): 壁纸列表据此决定是否显示该项。
 * ★ 2026-10-31: 改为**多路径探测** —— 商店默认把应用装到 TF 卡(/sdcard/apps/app),
 *   原来只认 /store/apps/app 会导致"装了但壁纸里没有这一项"。 */
bool wp_lifeguide_installed(void);

/* ★ 2026-10-31: 预加载 / 释放人生指南数据段（约 380KB PSRAM）。
 *
 * 为什么必须由外部在**安全上下文**调用，而不是让渲染函数自己去读文件：
 *   屏保监督任务的栈位于 **PSRAM**（wallpapers.c 的 SS_SUPER_STACK + EXT_RAM_BSS_ATTR）。
 *   该上下文里一旦触发内部 flash 操作（SPIFFS/NVS），spi_flash 会临时禁用缓存 →
 *   PSRAM 栈不可访问 → 断言/panic → 重启循环（项目里已踩过这个坑，见 wp_ss_prime 注释）。
 *   TF 卡走 SDMMC 不受影响，但 /store 是 SPIFFS，属于禁区内。
 * 因此：**读文件只允许在 UI 任务/开机任务（内部 RAM 栈）里做**，
 *       渲染函数只消费已经装进 PSRAM 的内存。
 * 返回 true = 数据已就绪（含"本来就已经加载好"）。 */
bool wp_lifeguide_preload(void);
/* 释放已加载的数据段。多次调用安全。
 *
 * ★ 2026-10-31 两段式释放：本函数**只登记意图，不立即 free**。
 *   数据的使用者是屏保监督任务（栈在 PSRAM），UI 任务无法确定它此刻不在 render 里。
 *   真正的 free 由 `wp_lifeguide_reap()` 在屏保任务确认空闲后执行。
 *   调用方照常调本函数即可，不需要知道这套机制。 */
void wp_lifeguide_release(void);
/* 由屏保监督任务在**非屏保**分支调用（wallpapers.c 的 ss_supervisor_task）。
 * 只有在 pending 且 render 不在跑时才真正 free；其余情况立即返回。
 * 不对外使用。 */
void wp_lifeguide_reap(void);

/* ==== 壁纸模块注册表 (壁纸模块化: 各壁纸通过注册表分发, 便于以后从商店下载新壁纸) ====
 * ★ 渲染返回值: 1 = 本帧有新内容, 需要 flush 上屏; 0 = 没有变化, 不要刷屏。
 *   静态壁纸(人生指南)靠它做到"一分钟只刷一次", 避免每帧 15000B 白耗电。 */
typedef int (*wp_render_fn)(st7305_handle_t *lcd, uint32_t now_ms);
typedef struct {
    int id;
    const char *name;
    wp_render_fn render;
    /* ★ 2026-10-31: 静态壁纸轮询间隔(ms)。0 = 动态壁纸（按"本帧是否有新内容"分档，
     *   有内容 62ms/240MHz，无内容 1000ms/80MHz —— 与原有行为完全一致）。
     *   >0 = 静态壁纸：**固定 80MHz + 浅睡 + 该间隔轮询**，不因偶尔画一帧就升到 240MHz。
     *
     * 修的是什么：静态壁纸每 60 秒换一条会产生 1 帧 drew=1 →
     *   原逻辑立刻 esp_pm_configure(240MHz) → 下一帧又切回 80MHz；
     *   也就是"每分钟整机调频两次"。调频本身有代价（DFS 重配置、浅睡门控 SPI），
     *   而且这一帧根本不需要 240MHz（画 20 行 16px 汉字在 80MHz 下也就几 ms）。
     * 现在静态壁纸全程停在 80MHz，只在换条那一帧多花几 ms。 */
    uint32_t idle_ms;
} wp_module_t;

void wp_register_module(const wp_module_t *m);
const wp_module_t *wp_get_module(int id);
const wp_module_t *wp_get_module_by_index(int idx);
int wp_get_module_count(void);

const char *wp_prog_name(int id);

/* ★ 2026-10-31: 让"下一帧屏保用哪个壁纸"立刻生效。
 *
 * 修的问题：屏保任务用的 `s_ss_prog` 原来**只在开机读一次 NVS**，
 *   用户在壁纸设置里改完只写了 NVS（os_wp/prog），屏保任务根本不知道 ——
 *   表现就是"换了壁纸没反应，重启才变"。
 *
 * 只能在 UI 任务（内部 RAM 栈）里调：本函数只改一个 int，不碰 NVS、不碰 flash。
 * （不能改成让屏保任务自己读 NVS —— 那是在 PSRAM 栈上碰 flash，会 panic。）
 * 传入的 id 未注册时收敛为星空。 */
void wp_set_program(int prog);

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
