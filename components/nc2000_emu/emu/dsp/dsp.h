/*
 * dsp.h — 扬声器 DSP 驱动接口 (SPDS104A 模拟)
 *
 * 上游 wangyu-/NC2000 的 dsp/ 目录并未提交到公开仓库，而 sound.cpp / io_new.cpp /
 * settings.cpp 又引用了它。这里提供一个最小可编译存根：
 *   - Dsp   : 数据/命令接收对象 (reset / write / callback)
 *   - set_dsp_log_level : 日志等级 (占位)
 * 语音合成 DSP 实际还原后续再补 (不影响模拟器主体运行)。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void set_dsp_log_level(int level);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* 与上游同名的 DSP 对象；只是一个可编译的最小占位实现 */
class Dsp {
public:
    /* 主机侧回调: 产出 PCM 数据 (p=缓冲区指针, len=字节数)。ESP32 移植版可
     * 通过该回调把合成语音喂给扬声器驱动。 */
    void (*callback)(unsigned char *p, int len);

    int dspMode;      /* 当前 DSP 模式 (io_new.cpp 会读它) */

    Dsp() : callback(0), dspMode(0) {}

    void reset() { dspMode = 0; }

    /* 命令/数据写入: (high, low) 各一字节。占位实现只解析 dspMode,
     * 不真正合成语音。 */
    void write(uint8_t high, uint8_t low) {
        (void)low;
        /* 依据 high 低几位切换 DSP 模式，保证 io_new.cpp 的状态机可推进 */
        if ((high & 0x80) == 0) {
            dspMode = high & 0x0f;
        } else {
            dspMode = high & 0x0f;
        }
    }
};
#endif

#endif /* DSP_H */