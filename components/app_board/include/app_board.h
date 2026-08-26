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

#ifdef __cplusplus
}
#endif

#endif
