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

/* ★ 2026-11-22 自愈: 确认芯片处于"已配置且已上电"状态, 不满足就地重配。
 *   背景: 内部 s_started 只是**软件记忆**, 芯片却可能因 MCLK 停止 / 供电抖动 /
 *   外部复位回到上电默认态(REG00 bit7=0) —— 此后 es8311_start() 因幂等直接返回,
 *   寄存器永远补不回来, 表现是"一切正常就是没声音"。
 *   本函数回读 REG00 校验, 掉电就清标志重配; 读失败(I2C 无应答)不做判断, 不误伤。
 *   返回 0=已就绪(或本机无此芯片)。 */
int es8311_ensure_running(void);

/* 根据采样率更新时钟分频器 */
int es8311_set_sample_rate(int sample_rate);

/* 设置音量 (0-100) */
int es8311_set_volume(int percent);

/* === 运行时诊断 API (供 remcon 串口排查, 2026-10-09) ===
 * 背景: "初始化全成功、就是没声音" 时, 唯一能确认 codec 是否真被配好/寄存器是否真的写进去
 * 的办法就是**回读**。这三个 API 让串口可以直接读/写任意寄存器, 用于现场定位。 */
int es8311_peek(uint8_t reg, uint8_t *val);   /* 读一个寄存器, 0=成功 */
int es8311_poke(uint8_t reg, uint8_t val);    /* 写一个寄存器, 0=成功 */
int es8311_dump_regs(void);                   /* 回读 0x00~0x4E 并打印, 返回成功条数 */

/* 关闭/休眠 */
void es8311_shutdown(void);

#endif
