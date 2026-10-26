#ifndef APP_BOARD_H
#define APP_BOARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件初始化封装 (从 main.c 抽出, 便于模块化与本地模拟替换).
 *
 * 顺序敏感约束 (V1.0.44 经验):
 *   - LCD -> SD 必须最先 (此时内部 DMA RAM 最干净, 防止碎片化)
 *   - 蓝牙/音频/电池延后到菜单就绪之后
 */
typedef struct {
    st7305_handle_t lcd;      /* 显示句柄 */
    bool            sd_ok;    /* TF 卡是否挂载成功 */
    bool            audio_ok; /* 音频硬件是否就绪 */
    bool            battery_ok;
    bool            gps_ok;   /* GPS (MAX-M10S) 驱动是否就绪 */
} app_board_t;

/* 阶段1: LCD / TF卡 / 输入 / NVS / 事件循环 / 内置资源部署 / SD 监控.
 * 必须在 menu_init 之前调用. */
esp_err_t app_board_init(app_board_t *board);

/* 阶段2: 蓝牙 (菜单之后, 内部 RAM 优先). */
void app_board_init_bt(void);

/* 阶段3: 音频 I2S+ES8311 (SD/蓝牙之后). */
void app_board_init_audio(app_board_t *board);

/* 阶段4: 电池 ADC. */
void app_board_init_battery(app_board_t *board);

/* 阶段5: GPS (u-blox MAX-M10S, UART).
 * 硬件常通电, 固件当前不使用定位 → 初始化后立即送"软件待机"省电 (µA 级).
 * 以后要用定位: 调 gps_wakeup() 唤醒, 再自行解析 NMEA/UBX (gps_read / gps_send_ubx). */
void app_board_init_gps(app_board_t *board);

/* ★ 2026-10-20 GPS 一次性识别 (写完固件的第一次开机只跑一次, 见 hw_cfg_gps_probed)。
 * 真去 UART 上听 UBX/NMEA: 有 ⇒ 送软件待机(46µA)并返回 true; 没有 ⇒ 卸 UART 返回 false。
 * async 版在后台任务里跑, 不占启动路径。 */
bool app_board_gps_probe(void);
void app_board_gps_probe_async(void);
void board_gps_boot_task(void *arg);   /* 已识别到 GPS 时的后台初始化+送待机 */

#ifdef __cplusplus
}
#endif

#endif
