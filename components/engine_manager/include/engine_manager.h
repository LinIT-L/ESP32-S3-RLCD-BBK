/**
 * @file engine_manager.h
 * @brief 统一游戏模拟器引擎管理器 (模块化)
 *
 * 所有模拟器引擎 (电子词典 gam4980 / GB / 未来 NES 等) 统一通过本管理器进行
 * 生命周期管理:
 *   - 进入对应二级菜单时, 调用 engine_manager_load() 后台加载该引擎
 *   - 回到主菜单时, 调用 engine_manager_unload_all() 卸载全部引擎并释放内存
 *
 * 保证同一时刻只有正在使用的引擎驻留内存, 支持不同引擎共存切换.
 */
#ifndef ENGINE_MANAGER_H
#define ENGINE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENGINE_GAM4980 = 0,   /* 电子词典 (gam4980 libretro 核心) */
    ENGINE_GB,            /* GB (esp-box-emu gnuboy) */
    ENGINE_GBC,           /* GBC (esp-box-emu gnuboy) */
    ENGINE_NES,           /* NES (esp-box-emu nofrendo) */
    ENGINE_ARDUBOY,       /* Arduboy (simavr ATmega32u4) */
    ENGINE_COUNT
} engine_id_t;

/* 引擎生命周期描述: 新增模拟器只需实现 load/unload 并注册到 engine_manager.c 的注册表 */
typedef struct {
    engine_id_t id;
    const char *name;
    void (*load)(st7305_handle_t *lcd);   /* 进入对应二级菜单时后台加载 (幂等) */
    void (*unload)(void);                 /* 回到主菜单时释放该引擎全部内存 */
} engine_desc_t;

/* 进入二级菜单时调用: 后台加载对应引擎 */
void engine_manager_load(engine_id_t id, st7305_handle_t *lcd);

/* 回到主菜单时调用: 卸载全部引擎, 释放所有引擎占用的内存 */
void engine_manager_unload_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_MANAGER_H */
