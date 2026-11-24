/*
 * emu8950_opl.cpp - OPLCORE 适配层 (薄胶水, 约 90 行)
 *
 * 只做三件事:
 *   1) 生命周期: OPL_new / OPL_delete
 *   2) 寄存器:   adplug write(reg,val) -> OPL_writeReg
 *   3) 出样本:   Generate(buf,n)        -> n 次 OPL_calc
 *
 * 时钟/采样率:
 *   OPL2 标准主频 3579545Hz, 内部生成率 = clk/72 = 49716Hz。
 *   EMU8950_CLK_DIV = 2 时把 clk 减半 (内部 24858Hz, CPU 直接减半),
 *   同时按 DIABLITO (RP2040 Doom 移植) 的做法补偿音高/包络:
 *       block + 1  (音高翻倍, 抵消主频减半)
 *       rate  + 1  (包络速度翻倍, 抵消每秒步数减半)
 *   默认 1 = 不做补偿。先用满速测 duty, 只有 duty 超标才改 2 ——
 *   一次只动一个变量, 避免"补偿不准"和"性能不够"混在一起查不出来。
 */

#include "emu8950_opl.h"

extern "C" {
#include "emu8950/emu8950.h"
}

#include "esp_timer.h"
#include "esp_log.h"

/* 性能探针: 只统计"纯合成"耗时, 不掺 feed_pcm。
 * 判据: us/样本 × 22050 / 1e6 = 合成占用率, <0.6 才留得下余量。 */
static int64_t  s_perf_us = 0;
static uint32_t s_perf_n  = 0;
static void emu8950_perf_log(int64_t dt_us, int n)
{
	s_perf_us += dt_us;
	s_perf_n  += (uint32_t)n;
	if (s_perf_us >= 2000000) {
		const double per = (double)s_perf_us / (double)(s_perf_n ? s_perf_n : 1);
		ESP_LOGW("EMU8950", "合成 %u 样本: %.1f us/样本 ⇒ 占用 %.0f%% (单核实时预算 100%%)",
		         (unsigned)s_perf_n, per, per * 22050.0 / 10000.0);
		s_perf_us = 0;
		s_perf_n  = 0;
	}
}

#ifndef EMU8950_CLK_DIV
/* 时钟分频: 必须是 2 的幂。
 *   1 = 满速(内部 49716Hz): 实测占用 77%, 余量不足, 会触发看门狗
 *   2 = 1/2  (24858Hz): 44.1k 输出下占用 62% —— 仍然太紧, 仍会 WDT
 *   4 = 1/4  (12429Hz): 目标占用 ~35%, 留足余量
 * CPU 只与 clk 成正比 (与输出率无关), 所以这是唯一有效的调节手段。 */
#define EMU8950_CLK_DIV 4
#endif

#define EMU8950_CLK_BASE 3579545u

/* emu8950 的 chip type: 0=Y8950 1=YM3526 2=YM3812(OPL2)
 * 必须显式设成 YM3812: 否则默认 Y8950 会分配 ADPCM 对象并每个样本都算一次,
 * 白白多花 CPU (仙剑的 RIX 不用 ADPCM)。 */
#define EMU8950_CHIP_YM3812 2

static inline uint32_t emu8950_norm_rate(uint32_t r) { return r ? r : 22050; }

EMU8950OPL2::EMU8950OPL2(uint32_t rate, uint32_t clk)
	: OPLCORE(emu8950_norm_rate(rate))
{
	if (clk == 0) clk = EMU8950_CLK_BASE / EMU8950_CLK_DIV;
	OPL *p = OPL_new(clk, emu8950_norm_rate(rate));
	if (p) OPL_setChipType(p, EMU8950_CHIP_YM3812);
	opl = p;
}

EMU8950OPL2::~EMU8950OPL2()
{
	if (opl) OPL_delete((OPL *)opl);
}

void EMU8950OPL2::Reset()
{
	if (opl) OPL_reset((OPL *)opl);
}

void EMU8950OPL2::Write(uint32_t reg, uint8_t val)
{
	if (!opl) return;
	reg &= 0xFF;   /* adplug 对 OPL3 会带 0x100 的芯片位; 单 OPL2 只用低 8 位 */

#if EMU8950_CLK_DIV > 1
	/* ---- 分频补偿: 主频除以 N ⇒ 音高/包络都慢 N 倍, 用 block+N 与 rate+N 抵消。
	 *   N = 2 的幂次, 所以 block 加 log2(N) 正好是 ×N。
	 *   代价: block 接近 7 的高音没有补偿空间, 会低八度/低两个八度(极少出现),
	 *   这比"为了个别高音把 fnum 砍半、破坏整体和声关系"要好。 */
	{
		const int shift = (EMU8950_CLK_DIV == 2) ? 1 : (EMU8950_CLK_DIV == 4) ? 2 : 3;
		if (reg >= 0xB0 && reg <= 0xB8) {          /* key-on / block / fnum hi */
			int block = (val >> 2) & 7;
			block += shift;
			if (block > 7) block = 7;
			val = (uint8_t)((val & ~0x1C) | (block << 2));
		} else if ((reg >= 0x60 && reg <= 0x6F) || (reg >= 0x80 && reg <= 0x8F)) {
			/* AR/DECAY 与 SL/RR 的 4bit 速率, 各 +shift (封顶 15) */
			int hi = (val >> 4) & 0x0F, lo = val & 0x0F;
			if (hi) { hi += shift; if (hi > 15) hi = 15; }
			if (lo) { lo += shift; if (lo > 15) lo = 15; }
			val = (uint8_t)((hi << 4) | lo);
		}
	}
#endif

	OPL_writeReg((OPL *)opl, reg, val);
}

void EMU8950OPL2::Generate(short *buf, int samples)
{
	if (!opl) {
		for (int i = 0; i < samples; i++) buf[i] = 0;
		return;
	}
	OPL *p = (OPL *)opl;
	const int64_t t0 = esp_timer_get_time();
	for (int i = 0; i < samples; i++) buf[i] = (short)OPL_calc(p);
	emu8950_perf_log(esp_timer_get_time() - t0, samples);
}

/* 自检: 强制起一个音, 生成 256 个样本并量峰值。
 * 与 rixplay / 音量 / I2S 全都无关 —— 用来区分:
 *   peak>0 ⇒ 核本身正常, "没声音"在上层;
 *   peak=0 ⇒ 核在这颗芯片上算不出波形 (数值/内存问题)。 */
extern "C" int emu8950_selftest(void)
{
	OPL *p = OPL_new(EMU8950_CLK_BASE / EMU8950_CLK_DIV, 22050);
	if (!p) return -1;
	OPL_setChipType(p, EMU8950_CHIP_YM3812);
	OPL_writeReg(p, 0x01, 0x20);
	OPL_writeReg(p, 0x20, 0x21); OPL_writeReg(p, 0x23, 0x21);
	OPL_writeReg(p, 0x40, 0x10); OPL_writeReg(p, 0x43, 0x10);
	OPL_writeReg(p, 0x60, 0xF0); OPL_writeReg(p, 0x63, 0xF0);
	OPL_writeReg(p, 0x80, 0x77); OPL_writeReg(p, 0x83, 0x77);
	OPL_writeReg(p, 0xA0, 0x98);
	OPL_writeReg(p, 0xB0, 0x31);
	int peak = 0;
	for (int i = 0; i < 256; i++) {
		int v = OPL_calc(p);
		if (v < 0) v = -v;
		if (v > peak) peak = v;
	}
	OPL_delete(p);
	return peak;
}

OPLCORE *EMU8950OPL2::Duplicate()
{
	return new EMU8950OPL2(rate);
}
