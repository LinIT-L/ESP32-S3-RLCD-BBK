#ifndef ES7210_H
#define ES7210_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 ES7210 四声道 ADC (双麦克风阵列), I2S 从模式 16kHz/16bit
 * 依赖: I2C 总线已由 es8311_init 安装驱动 (I2C_NUM_0)
 * 返回 0 成功, -1 失败 (无设备) */
int es7210_init(int i2c_port);

/* 只探测 I2C 地址 (读寄存器 0x00), 不初始化; 返回地址 (0x40-0x43) 或 0 */
int es7210_probe(int i2c_port);

/* 关闭 ES7210 ADC (省电) */
void es7210_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
