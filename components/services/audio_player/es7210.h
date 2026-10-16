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

/* === 运行时诊断 (供 remcon 串口排查, 2026-10-14) ===
 * "初始化成功但录音全 0" 时唯一能确认寄存器到底写进去没有的办法就是回读。 */
int es7210_peek(uint8_t reg, uint8_t *val);   /* 读一个寄存器, 0=成功 (需已 init) */
int es7210_poke(uint8_t reg, uint8_t val);    /* 写一个寄存器, 0=成功 (需已 init) */
int es7210_dump_regs(void);                   /* 回读 0x00~0x4C 并打印, 返回成功条数 */
int es7210_address(void);                     /* 探测到的 I2C 地址 (0 = 未探测到) */

#ifdef __cplusplus
}
#endif

#endif
