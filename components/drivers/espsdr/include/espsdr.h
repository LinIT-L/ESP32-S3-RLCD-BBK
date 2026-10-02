#pragma once
/* ============================================================================
 * espsdr — ESP32-S3 原始 I/Q 采集驱动 (L1 驱动层)
 * ----------------------------------------------------------------------------
 * 来源: ESPARGOS/esp-sdr (https://github.com/ESPARGOS/esp-sdr) 的
 *       main/targets/esp32s3/receiver.c —— 这里**只保留采集部分**:
 *       去掉 UART/命令行/USB 传输与打包, 把 64KB IQ 缓冲直接暴露给上层,
 *       让应用在本机做 FFT/显示 (不走串口, 因此不受"搬运瓶颈"限制)。
 *
 * 原理 (详见 docs/频谱仪应用.md):
 *   ESP32 正常路径是 ADC → 硬件调制解调器 → 协议栈, 软件拿不到原始采样;
 *   本驱动用未公开的调试寄存器把 ADC 的 I/Q 旁路写进一块 SRAM,
 *   再通过 0x60033d5c 的控制位触发一次"突发采集"(burst)。
 *
 * ⚠ 关键前提 (三者缺一不可, 详见 .c 里的注释):
 *   ① 0x3fcd0000 起的 ESPSDR_APERTURE_BYTES 字节必须从堆/静态段里**保留出来**
 *      (SOC_RESERVE_MEMORY_REGION + sram_guard.ld 的链接断言);
 *   ② 必须先 esp_wifi_start() 把 PHY 拉起来, 并置**混杂模式**;
 *   ③ 采集窗口内**不要写 flash**(NVS/日志落盘)。写 flash 会禁 cache,
 *      虽然 modem 写 SRAM 不受影响, 但 CPU 侧搬运/FFT 会被打断, 观感变差。
 *
 * ⚠ 射频独占: 进入本模式会接管 PHY, **不能与正常 WiFi/BT 收发同时使用**。
 *   调用顺序由上层负责协调 WiFi 服务 (先 wifi_manager_disable(), 退出后再恢复)。
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 采集区保留窗口 (字节). 硬件 aperture 钉在 0x3fcd0000, 整窗口 64KB; 但 modem 只写前 n 个字,
 * 所以理论上只保留"够用的一段"就够 —— 内部 RAM 每 KB 都很贵 (蓝牙+WiFi 都要抢)。
 *
 * ⚠ 缩小窗口**必须先拿到实测证据** (不要凭推测):
 *   第一阶段 (2026-10-01, 已通过): 保留满 64KB, 由 espsdr_capture() 的尾区守卫逐帧复核,
 *     实测日志 `越写守卫通过: 窗口 16384 字 (64 KB), 本次用 2048 字, 其后 14336 字未被写入`
 *     → 证明硬件严格只写 n 个字。
 *   第二阶段 (当前): 收到 20KB 窗口 + 样本上限 4096 (最多用 16KB), 放回 44KB 内部堆。
 *   ⚠ 窗口越小, 越写留下的余量越小 —— 守卫必须常开; 再想缩窗口请重跑第一阶段验证。 */
/* ⚠ 2026-10-01 复核结论: **必须保留满 64KB**, 不要缩。
 *  曾试过只保留 20KB 把剩下 44KB 还给堆 —— 结果采集触发后几秒内必然
 *  `INT_WDT` 复位 (panic 正文来不及打就重启), 不进频谱仪则完全稳定。
 *  原因: 硬件 aperture 是**整块 64KB**, MAC_DUMP_USAGE=4 让 MAC 接管整块,
 *  而尾区守卫只能证明"dump 写了 n 个字", 证明不了 MAC 对整块窗口的占用范围 ——
 *  还回去的 44KB 会被堆拿去用, 于是被踩。**这条不要再试第二次**;
 *  要省内部 RAM 请走"低风险途径"(见 docs/内部RAM预算与蓝牙WiFi精简.md §6)。 */
#define ESPSDR_APERTURE_BYTES   (64u * 1024u)     /* 满窗口, 必须 */

/* 采集区能装的样本数上限 (≤ ESPSDR_APERTURE_BYTES/4 = 16384) */
#define ESPSDR_MAX_SAMPLES   16380u

/* 官方标称可用调谐范围 (硬件实际能拉得更远, 但超出这段不保证) */
#define ESPSDR_FREQ_MIN_MHZ  2200u
#define ESPSDR_FREQ_MAX_MHZ  2700u

/* 采样率档位 (对应 capture 的 divider 参数; 数值见 espsdr_rate_msps) */
#define ESPSDR_DIV_80M       0u
#define ESPSDR_DIV_40M       1u
#define ESPSDR_DIV_16M       6u

/* 增益: 传 ESPSDR_GAIN_AUTO 用硬件 AGC; 0..espsdr_gain_max() 手动固定 */
#define ESPSDR_GAIN_AUTO     (-1)

/* ---- 生命周期 ---- */
esp_err_t espsdr_enter(void);   /* 接管射频: 起 WiFi(NULL+混杂) + PHY 准备 + 调谐 + 增益 */
void      espsdr_exit(void);    /* 归还: 停抓取 + 还原寄存器 + 关混杂 (WiFi 由上层恢复) */
bool      espsdr_ready(void);

/* ---- 参数 ---- */
void espsdr_tune(unsigned mhz);     /* 中心频率 (WiFi 信道走 chanfreq, 其它走 PLL 直设) */
void espsdr_set_gain(int code);     /* ESPSDR_GAIN_AUTO 或 0..gain_max() */
int  espsdr_gain_max(void);         /* 当前校准表允许的最大手动增益档 (S3 通常 ≤82) */
int  espsdr_gain_get(void);         /* 当前档位 (自动时返回 ESPSDR_GAIN_AUTO) */
unsigned espsdr_rate_msps(unsigned divider);  /* 0→80, 1→40, 6→16 (MSa/s) */

/* ---- 采集 ---- */
/* 抓一帧 n 个样本 (n ≤ ESPSDR_MAX_SAMPLES) 到内部缓冲; 返回样本数, <0 = 失败 */
int espsdr_capture(unsigned n, unsigned divider);

/* 采集缓冲原始指针 (32bit/样本)。样本格式:
 *   bit19-10 = I (有符号 10bit) · bit9-0 = Q (有符号 10bit)
 *   bit27-20 = 当前接收增益索引 · bit31-28 = AGC 状态机内部状态 */
const uint32_t *espsdr_raw(void);

/* 解码第 i 个样本的 I/Q, 归一化到 [-1, 1)  (除以 512) */
void espsdr_iq(int i, float *out_i, float *out_q);

/* 最近一次采集耗时 (µs), 供 UI 显示吞吐 */
uint32_t espsdr_last_elapsed_us(void);

/* 采集区保留窗口的字数 (= ESPSDR_APERTURE_BYTES / 4) */
unsigned espsdr_aperture_words(void);

/* 越写守卫结果: modem 越过 n 写到的最高字下标; -1 = 从未越写 (窗口大小可放心保持/缩小) */
int espsdr_overrun_word(void);
