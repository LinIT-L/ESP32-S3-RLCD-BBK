/* 测试专用 sms.hpp —— 覆盖 components/engines/sms_emu/include/sms.hpp
 *
 * 只保留 sms_cbridge.cpp 编译所需的三个声明, 去掉 shared_memory.h 依赖。
 * 被 -I tests/emu_save/stub 优先命中 (原文件不在源文件同目录, 故 -I 有效)。
 */
#pragma once

#include <span>
#include <string_view>
#include <cstdint>

void load_sms(std::string_view save_path);
void save_sms(std::string_view save_path);
std::span<uint8_t> get_sms_video_buffer();
