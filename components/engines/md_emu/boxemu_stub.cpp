/* boxemu_stub.cpp — BoxEmu 桩实现 (对接本工程 input / audio_player / PSRAM 帧缓冲) */
#include "box-emu.hpp"

#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* 本工程 C 接口 */
extern "C" {
uint8_t input_get_held_gb_joypad(void);
size_t audio_player_feed_pcm(const int16_t *data, size_t frames, int sample_rate);
}

/* C 桥: 供 C 适配层取当前帧 */
extern "C" uint8_t *boxemu_current_frame(void)
{
    return BoxEmu::get().current_frame();
}

static const char *TAG = "boxemu_stub";
#define STUB_FB_MAX_BYTES (320 * 240 * 2)

void BoxEmu::ensure_buffers(size_t width, size_t height)
{
    size_t bytes = width * height * 2;
    if (bytes > STUB_FB_MAX_BYTES) bytes = STUB_FB_MAX_BYTES;
    if (fb0_ && fb_w_ == width && fb_h_ == height) return;
    if (fb0_) { heap_caps_free(fb0_); fb0_ = nullptr; }
    if (fb1_) { heap_caps_free(fb1_); fb1_ = nullptr; }
    fb0_ = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fb1_ = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb0_ || !fb1_) {
        ESP_LOGE(TAG, "帧缓冲分配失败 (PSRAM %u bytes)", (unsigned)bytes);
        return;
    }
    memset(fb0_, 0, bytes);
    memset(fb1_, 0, bytes);
    fb_w_ = width;
    fb_h_ = height;
    current_ = fb0_;
    ESP_LOGI(TAG, "帧缓冲 %ux%u 双缓冲已分配 (PSRAM)", (unsigned)width, (unsigned)height);
}

uint8_t *BoxEmu::frame_buffer0() { return fb0_; }
uint8_t *BoxEmu::frame_buffer1() { return fb1_; }

void BoxEmu::push_frame(const void *frame)
{
    current_ = (uint8_t *)frame;
}

uint8_t *BoxEmu::current_frame() { return current_; }

void BoxEmu::native_size(size_t width, size_t height, int pitch)
{
    (void)pitch;
    ensure_buffers(width, height);
}

void BoxEmu::palette(const uint16_t *palette, size_t size)
{
    size_t n = size > 256 ? 256 : size;
    if (palette) {
        memcpy(pal_, palette, n * sizeof(uint16_t));
        pal_size_ = n;
    }
}

GamepadState BoxEmu::gamepad_state()
{
    /* GB 位掩码 (低电平有效): bit0=A bit1=B bit2=Sel bit3=Start bit4=右 bit5=左 bit6=上 bit7=下 */
    uint8_t j = input_get_held_gb_joypad();
    GamepadState s;
    s.a      = !(j & (1u << 0));
    s.b      = !(j & (1u << 1));
    s.select = !(j & (1u << 2));
    s.start  = !(j & (1u << 3));
    s.right  = !(j & (1u << 4));
    s.left   = !(j & (1u << 5));
    s.up     = !(j & (1u << 6));
    s.down   = !(j & (1u << 7));
    return s;
}

void BoxEmu::play_audio(const uint8_t *data, size_t size)
{
    if (!data || size < 4) return;
    /* 双声道 int16 交错: 每声道帧数 = size / 4 */
    audio_player_feed_pcm((const int16_t *)data, size / 4, (int)s_audio_rate);
}

void BoxEmu::play_audio(const std::vector<uint8_t> &data)
{
    play_audio(data.data(), data.size());
}

BoxEmu::~BoxEmu()
{
    if (fb0_) { heap_caps_free(fb0_); fb0_ = nullptr; }
    if (fb1_) { heap_caps_free(fb1_); fb1_ = nullptr; }
}
