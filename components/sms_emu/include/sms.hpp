#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shared_memory.h"

#ifdef __cplusplus
extern "C" {
#endif
void reset_sms();
void init_sms(uint8_t *romdata, size_t rom_data_size);
void init_gg(uint8_t *romdata, size_t rom_data_size);
void run_sms_rom();
void deinit_sms();
#ifdef __cplusplus
}
#endif
void load_sms(std::string_view save_path);
void save_sms(std::string_view save_path);
std::span<uint8_t> get_sms_video_buffer();
