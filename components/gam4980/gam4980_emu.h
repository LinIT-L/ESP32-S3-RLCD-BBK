/**
 * @file gam4980_emu.h
 * @brief gam4980 模拟器 wrapper - 集成 libretro 核心到 ST7305 显示
 */
#ifndef GAM4980_EMU_H
#define GAM4980_EMU_H

#include "st7305.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化模拟器 (加载 ROM 等) */
esp_err_t gam4980_emu_init(st7305_handle_t *lcd);

/* 设置状态栏信息 (供全屏模式状态栏使用) */
void gam4980_set_status_info(uint8_t battery, bool pad_connected);

/* 后台初始化: 系统启动时在菜单后台自动加载引擎 */
void gam4980_emu_background_init(st7305_handle_t *lcd);

/* 检查引擎是否已就绪 */
bool gam4980_emu_is_ready(void);

/* 后台初始化状态 (供菜单状态栏显示进度) */
bool gam4980_emu_is_bg_active(void);   /* 后台是否正在初始化 */
int  gam4980_emu_get_bg_progress(void); /* 后台初始化进度 0-100 */

/* 显示模式: 点对点(320x192 2x缩放居中) / 全屏(400x240按比例缩放) / 拉伸(400x240铺满) */
typedef enum {
    DISP_MODE_POINT2POINT = 0,  /* 点对点 2x (320x192) */
    DISP_MODE_FULLSCREEN,       /* 全屏 (400x240) */
    DISP_MODE_STRETCH,          /* 拉伸 (400x240) */
} display_mode_t;

/* V1.0.46: 画面优化 (0=关, 1=标准圆角, 2=深度灰度模拟) */
void gam4980_set_pic_opt(int level);

/* 设置显示模式 */
void gam4980_set_display_mode(display_mode_t mode);
display_mode_t gam4980_get_display_mode(void);



/* 兼容旧接口 */
void gam4980_set_fullscreen(int mode);   /* 0=点对点, 1=全屏, 2=拉伸 */
/* V1.1.1: 引擎优化 (os 传入, 独立于旧 menu g_menu): 虚拟按键三态 / 游戏内状态栏 */
void gam4980_set_vkey(int vkey);         /* 0=关 1=开 2=自动(无手柄时开) */
void gam4980_set_status_bar(bool on);     /* 游戏内状态栏 显示/隐藏 */
bool gam4980_get_fullscreen(void);

/* 游戏壁纸模式: 打开时任意设备按键强制退出游戏 (V1.0.64) */
void gam4980_set_wallpaper_mode(bool on);

/* 游戏退出确认结果 (gam4980 引擎专有, 从旧 menu_system.h 迁出归位) */
typedef enum {
    GAME_EXIT_CANCEL = 0,   /* 取消, 恢复游戏继续 */
    GAME_EXIT_CONFIRMED,    /* 确认退出 (返回游戏二级菜单) */
    GAME_EXIT_TIMEOUT,      /* 无操作超时退出 (返回桌面) */
} game_exit_result_t;

/* 设置 OS 启动进度回调 */
void gam4980_set_boot_progress_cb(void (*cb)(int percent, const char *msg));

/* flash 懒初始化 (在游戏主循环中调用, 每帧初始化一部分) */
bool gam4980_flash_init_step(void);

/* 设置游戏 (从文件系统加载) */
int gam4980_emu_load(const char *path);

/* 启动运行游戏 (隐藏菜单, 进入游戏循环)
 * 返回退出结果: GAME_EXIT_CONFIRMED=确认退出, GAME_EXIT_CANCEL=取消(未退出),
 *              GAME_EXIT_TIMEOUT=无操作超时退出. */
game_exit_result_t gam4980_emu_run(void);

/* 退出游戏 (返回菜单) */
void gam4980_emu_stop(void);

/* === 游戏存档持久化 (解决重启后存档丢失问题) ===
 * 文件路径: /sdcard/dict/<romname>.sav (32KB, sys_flash[0..0x7FFF] 真正存档区)
 * - gam4980_emu_save_state: 退出游戏时调用, 把 PSRAM 中的存档写入 SD 卡
 * - gam4980_emu_load_state: 进入游戏时调用, 从 SD 卡恢复存档到 PSRAM
 * 调用方需保证 rom_path 指向已加载的 .gam 文件, 与 gam4980_emu_load 传入的路径一致.
 * 失败时静默返回 (无存档/SD 卡未挂载等), 不影响游戏正常运行.
 *
 * 兼容说明: V1.0.7 及之前的旧版 .sav 文件大小为 80KB (3 段拼接, 含游戏数据), 新版
 *   load_state 会自动检测并仅提取前 32KB (即真正的存档区) 加载, 旧段 2/段 3 丢弃. */
void gam4980_emu_save_state(const char *rom_path);
void gam4980_emu_load_state(const char *rom_path);

/* 卸载引擎 (清除 ROM 数据, 释放 PSRAM 占用, 下次进入电子词典需重新加载) */
void gam4980_emu_unload(void);

/* === 修改机 (内存搜索) 支持: 仅帧边界冻结时调用, 同任务无锁 ===
 * 返回 NULL = 引擎未加载 (无可搜内存). 只读写步步高自身 RAM/存档区. */
uint8_t *gam4980_modtool_ram_ptr(void);     /* 32KB 热 RAM (游戏变量主战场) */
size_t   gam4980_modtool_ram_size(void);    /* 0x8000 */
uint8_t  gam4980_modtool_ram_peek(uint16_t off);
void     gam4980_modtool_ram_poke(uint16_t off, uint8_t v);
uint8_t *gam4980_modtool_flash_save_ptr(void);  /* 存档区 sys_flash[0..0x7FFF] */
uint16_t gam4980_modtool_pc(void);          /* CPU PC (帧边界) */

#ifdef __cplusplus
}
#endif

#endif
