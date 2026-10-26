/*
 * emu8950_opl.h - 把开源 emu8950 (MIT) 接成 adplug 的 OPLCORE 后端
 *
 * 为什么单独做这一层:
 *   adplug 的 OPLCORE 抽象只有 Reset/Write/Generate/Duplicate 四个方法,
 *   emu8950 的 C 接口 (OPL_new/OPL_writeReg/OPL_calc) 与它一一对应。
 *   所以这里只写"翻译", 不改上游播放器 (rixplay.cpp / adplug/rix.cpp),
 *   也不改 emu8950 本体 (adplug/emu8950/ 整目录保持上游原样)。
 *
 * 为什么换掉 DBFLT:
 *   DOSBox DBOPL 在 ESP32-S3 上实测单核实时间因子 < 1
 *   (44.1k 双芯片 / 22.05k 单芯片都触发 task_wdt), 反解栈落在
 *   DBOPL2::adlib_getsample。emu8950 是整数为主的轻量核, RP2040 上已能实时。
 *
 * 采样率说明 (重要, 别改错):
 *   emu8950 内部生成率 = clk / 72, 与**输出** rate 无关;
 *   降低输出 rate 只减少重采样与 I2S 开销, 不减少 FM 运算量。
 *   要真正省 CPU 只能降 clk (见 emu8950_opl.cpp 的 EMU8950_CLK_DIV)。
 */

#ifndef SDLPAL_EMU8950_OPL_H
#define SDLPAL_EMU8950_OPL_H

#include "emuopls.h"

class EMU8950OPL2 : public OPLCORE {
public:
	explicit EMU8950OPL2(uint32_t rate, uint32_t clk = 0);
	~EMU8950OPL2();

	virtual void Reset();
	virtual void Write(uint32_t reg, uint8_t val);
	virtual void Generate(short *buf, int samples);
	virtual OPLCORE *Duplicate();

private:
	void *opl;   /* 实际是 OPL*, 这里用 void* 免得把 C 头文件塞进 C++ 头 */
};

#endif
