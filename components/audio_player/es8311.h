#ifndef ES8311_H
#define ES8311_H

#include <stdint.h>
#include <stdbool.h>

/* ES8311 I2C 地址 */
#define ES8311_ADDR         0x18

/* 初始化 ES8311 codec (I2C + DAC 输出)
 * sda/scl: I2C 引脚
 * 返回 0=成功 (注意: 未检测到芯片时也返回 0, 用 es8311_is_present 判断芯片是否存在) */
int es8311_init(int sda, int scl);

/* 芯片是否真实存在 (I2C 探测结果).
 * 用于区分平台: 有 ES8311=官方板, 无 ES8311=自制板(NS4168 I2S 数字功放直驱) */
bool es8311_is_present(void);

/* 启动播放配置 (在 I2S MCLK 启动后调用!)
 * 必须在 es8311_init() 之后、I2S 时钟就绪之后调用 */
int es8311_start(void);

/* 根据采样率更新时钟分频器 */
int es8311_set_sample_rate(int sample_rate);

/* 设置音量 (0-100) */
int es8311_set_volume(int percent);

/* 关闭/休眠 */
void es8311_shutdown(void);

#endif
