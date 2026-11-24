/*
 * rg_stub.h — lavaxvm 移植到本固件的平台替身.
 *
 * lavaxvm 原实现基于 retro-go (rg_*) 框架。本固件不使用 retro-go,
 * 因此这里提供一组最小化的替身: 定义 RG_KEY/RG_LOG/battery/utf8 及
 * 少量工具函数, 并把实际平台调用(按键/时间/运行退出判断)转发给
 * 固件的 input / esp_timer / lavax_emu.c。
 *
 * 文档字符串与可见宏以英文/中文注释标出用途。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 手柄逻辑键位 (沿用 retro-go 定义, 位掩码) ----
 * lavaxvm 的 hardware.c 用它把手柄状态归一化成 LavaX 键码。
 */
#define RG_KEY_UP      (1 << 0)
#define RG_KEY_RIGHT   (1 << 1)
#define RG_KEY_DOWN    (1 << 2)
#define RG_KEY_LEFT    (1 << 3)
#define RG_KEY_SELECT  (1 << 4)
#define RG_KEY_START   (1 << 5)
#define RG_KEY_MENU    (1 << 6)
#define RG_KEY_OPTION  (1 << 7)
#define RG_KEY_A       (1 << 8)
#define RG_KEY_B       (1 << 9)
#define RG_KEY_X       (1 << 10)
#define RG_KEY_Y       (1 << 11)
#define RG_KEY_L       (1 << 12)
#define RG_KEY_R       (1 << 13)
#define RG_KEY_COUNT   14
#define RG_KEY_ANY     0xFFFF

/* ---- 日志宏 (简化: 直接打 UART) ---- */
#include "esp_log.h"
#define RG_LOGW(...) ESP_LOGW("lavax", __VA_ARGS__)
#define RG_LOGI(...) ESP_LOGI("lavax", __VA_ARGS__)
#define RG_LOGE(...) ESP_LOGE("lavax", __VA_ARGS__)

/* ---- 电池 (固件暂不提供, 恒为"不存在") ---- */
typedef struct {
    bool present;
    float level;      /* 0..1 */
    float volts;
    bool charging;
} rg_battery_t;

/* ---- 工具宏 ---- */
#define RG_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* ---- UTF-8 (移植自 retro-go 的轻量实现, 消除对 rg_utils 依赖) ----
 * rg_utf8_decode: 解析 ptr 指向的一个码点, 并推进 ptr; 失败返回 -1。
 * rg_utf8_encode: 将码点写入 out(至多4字节), 返回写入长度; 失败返回 0。
 */
int  rg_utf8_decode(const char **ptr);
size_t rg_utf8_encode(char *out, int codepoint);

/* ---- 全局小工具 (宿主持有的) ---- */

/* 读取归一化的手柄按键位掩码 (RG_KEY_*). 由 lavax_emu.c 实现. */
uint32_t rg_input_read_gamepad(void);
/* 读取电池状态 (本固件指示不存在). 由 lavax_platform_api.c 实现. */
rg_battery_t rg_input_read_battery(void);
/* 屏幕存在期间每 ~60ms 抛给顶层做心跳/看门狗. 由 lavax_emu.c 提供. */
void rg_task_yield(void);
/* 单调微秒时间. 由 lavax_emu.c 提供. */
int64_t rg_system_timer(void);
/* 设置系统时间(秒). 本固件空实现. */
void rg_system_set_time(time_t seconds);

/* GBK(OEM/936) <-> Unicode 转码, 替代 fatfs 的 ff_oem2uni/ff_uni2oem.
 * 由 lavax_emu.c 提供实现 (lavax_gbk_*.c)。 */
unsigned int lavax_gbk_oem2uni(unsigned int oem);
unsigned int lavax_gbk_uni2oem(unsigned int uni);

/* 供 file.c 使用的别名 */
#define ff_oem2uni(oem, cp)  lavax_gbk_oem2uni(oem)
#define ff_uni2oem(uni, cp)  lavax_gbk_uni2oem(uni)

#ifdef __cplusplus
}
#endif