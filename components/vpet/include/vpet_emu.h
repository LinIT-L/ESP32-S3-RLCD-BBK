/*
 * vpet_emu.h — 暴龙机 (虚拟宠物) 引擎宿主
 *
 * 基于 vpet-emu-zepp 的 E0C6200 4-bit CPU 核心 (components/vpet/e0c6200_cpu.c),
 * 模拟第一代虚拟宠物设备 (Tamagotchi P1/P2、Digimon 等). ROM 为 .bin 文件,
 * 从 TF 卡 /sdcard/vpet/ 目录加载.
 *
 * 生命周期 (由 engine_manager + menu 驱动, 与 arduboy/lavax 一致):
 *   - vpet_emu_background_init(): 进入暴龙机二级菜单时后台初始化 (分配 PSRAM, 预热 tone)
 *   - vpet_emu_start():          选中 ROM 后启动模拟任务 (task 方式, 菜单负责退出确认)
 *   - vpet_emu_pause/resume:     退出确认弹窗期间冻结/恢复画面
 *   - vpet_emu_stop():           退出游戏后停止模拟任务
 *   - vpet_emu_unload():         返回主菜单时释放全部内存
 */
#ifndef VPET_EMU_H
#define VPET_EMU_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进入暴龙机二级菜单时调用: 分配 PSRAM 缓冲 + 预热 tone (幂等) */
esp_err_t vpet_emu_background_init(st7305_handle_t *lcd);

/* 选中 ROM 启动: 加载 .bin, 初始化 CPU, 创建模拟任务 (非阻塞, 立即返回).
 * 模拟任务持续运行直至 vpet_emu_stop()/vpet_emu_unload(). */
esp_err_t vpet_emu_start(const char *path);

/* 退出确认弹窗期间冻结模拟 (保持最后一帧画面) */
void vpet_emu_pause(void);
void vpet_emu_resume(void);

/* 每帧由 menu 喂入 GB joypad 掩码 (低电平有效, 与 GB/web 手柄一致) */
void vpet_emu_set_joypad(uint8_t joypad);

/* 停止模拟任务 (阻塞等待任务退出) */
esp_err_t vpet_emu_stop(void);

/* 返回主菜单时释放全部内存 */
void vpet_emu_unload(void);

/* V1.0.9x: 暴龙机独立显示设置: mode 0=点对点 1=放大 2=拉伸; aa=EPX 抗锯齿 */
void vpet_emu_set_display(int mode, bool aa);

#ifdef __cplusplus
}
#endif

#endif /* VPET_EMU_H */
