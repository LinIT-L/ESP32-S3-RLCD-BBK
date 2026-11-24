#pragma once

#include <cstdint>

/* 统计桩: esp-box-emu 的 statistics 组件在本工程不需要, 提供空实现. */
inline void update_frame_time(uint64_t) {}
inline void reset_frame_time() {}
inline float get_fps() { return 0.0f; }
inline uint64_t get_frame_time() { return 0; }
inline uint64_t get_frame_time_max() { return 0; }
inline uint64_t get_frame_time_min() { return 0; }
inline float get_frame_time_avg() { return 0.0f; }
inline void print_statistics() {}
