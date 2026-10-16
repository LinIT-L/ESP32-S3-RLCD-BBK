#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gamepad_state.hpp"

/* box-emu.hpp 桩 — 替换 esp-box-emu 的 BoxEmu(依赖 espp/EspBox 板级抽象).
 * 在本工程里对接: 帧缓冲(PSRAM 双缓冲) / input_get_held_gb_joypad() / audio_player_feed_pcm(). */
class BoxEmu {
public:
  static BoxEmu &get() {
    static BoxEmu instance;
    return instance;
  }

  bool is_muted() const { return false; }
  void audio_sample_rate(int rate) { s_audio_rate = (uint32_t)rate; }
  uint32_t audio_sample_rate() const { return s_audio_rate; }
  void play_audio(const uint8_t *data, size_t size);
  void play_audio(const std::vector<uint8_t> &data);

  void native_size(size_t width, size_t height, int pitch);
  GamepadState gamepad_state();

  uint8_t *frame_buffer0();
  uint8_t *frame_buffer1();
  void push_frame(const void *frame);
  void palette(const uint16_t *palette, size_t size = 256);

  static constexpr char mount_point[] = "/sdcard";

  /* C 桥: 取"刚 push 的当前帧"(RGB565 双字节/像素) 供适配层灰度打包 */
  uint8_t *current_frame();
  void ensure_buffers(size_t width, size_t height);

private:
  BoxEmu() = default;
  ~BoxEmu();
  uint8_t *fb0_ = nullptr;
  uint8_t *fb1_ = nullptr;
  uint8_t *current_ = nullptr;
  size_t fb_w_ = 0, fb_h_ = 0;
  uint32_t s_audio_rate = 22050;
  uint16_t pal_[256];
  size_t pal_size_ = 0;
};
