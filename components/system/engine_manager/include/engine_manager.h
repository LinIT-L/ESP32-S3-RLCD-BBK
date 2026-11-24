/**
 * @file engine_manager.h
 * @brief 统一游戏模拟器引擎管理器 (模块化)
 *
 * 所有模拟器引擎 (电子词典 gam4980 / GB / 未来 NES 等) 统一通过本管理器进行
 * 生命周期管理:
 *   - 进入对应二级菜单时, 调用 engine_manager_load() 后台加载该引擎
 *   - **离开该二级菜单时**, 调用 engine_manager_unload() 释放这一个引擎
 *     (linit 2026-11-21: 不再等回主菜单才回收, 页面退出即还内存)
 *   - 回主菜单 / 低内存时仍由 engine_manager_unload_all() 兜底
 *
 * 保证同一时刻只有正在使用的引擎驻留内存, 支持不同引擎共存切换.
 * ⚠ 未加载过的引擎不会被调 unload: 有些引擎的 unload 会碰虚拟机/FAT 内部状态,
 *   对"从未 init"的实例调用并不保证安全, 用加载标记挡住.
 */
#ifndef ENGINE_MANAGER_H
#define ENGINE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENGINE_GAM4980 = 0,   /* 电子词典 (gam4980 libretro 核心) */
    ENGINE_GB,            /* GB/GBC (Peanut-GB, 存档目录分开) */
    ENGINE_NES,           /* NES (esp-box-emu nofrendo) */
    ENGINE_ARDUBOY,       /* Arduboy (simavr ATmega32u4) */
    ENGINE_LAVAX,         /* wqx 文曲星 (LavaX 虚拟机 lavaxvm) */
    ENGINE_VPET,          /* 暴龙机 (vpet-emu-zepp E0C6200 4-bit CPU) */
    ENGINE_MD,            /* MD/Genesis (Gwenesis) */
    ENGINE_SMS,           /* SMS/GG (SMSPlus) */
    ENGINE_MESHTASIC,     /* Meshtasic (旧菜单兼容保留; 本版本不再实现) */
    ENGINE_COUNT
} engine_id_t;

/* 引擎生命周期描述: 新增模拟器只需实现 load/unload 并注册到 engine_manager.c 的注册表
 * load 返回 esp_err_t: 同步加载失败必须回传, 否则加载标记会永久为真 (见 .c 说明). */
typedef struct {
    engine_id_t id;
    const char *name;
    esp_err_t (*load)(st7305_handle_t *lcd);  /* 进入对应二级菜单时后台加载 (幂等) */
    void (*unload)(void);                     /* 回到主菜单时释放该引擎全部内存 */
} engine_desc_t;

/* 进入二级菜单时调用: 后台加载对应引擎.
 * 返回 ESP_OK 表示已加载/本次加载成功; 失败时内部已回滚加载标记, 下次进入可重试. */
esp_err_t engine_manager_load(engine_id_t id, st7305_handle_t *lcd);

/* 离开二级菜单时调用: 只卸载这一个引擎 (幂等); 未加载过则直接返回. */
void engine_manager_unload(engine_id_t id);

/* 该引擎当前是否处于"已加载"状态 (加载任务已发起即算) */
bool engine_manager_is_loaded(engine_id_t id);

/* 回到主菜单时调用: 卸载全部引擎, 释放所有引擎占用的内存.
 * do_leak_check: true=卸载后做内存泄漏复核 (正常回主菜单场景);
 *                false=跳过 (低内存回收/周期兜底时, 此刻空闲偏低或重复卸载,
 *                          做比对会误报 [LEAK]). */
void engine_manager_unload_all(bool do_leak_check);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_MANAGER_H */
