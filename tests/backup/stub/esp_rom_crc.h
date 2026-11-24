/* 测试专用 esp_rom_crc.h —— 标准 CRC-32 (IEEE 802.3), 与 ESP ROM 实现一致。
 * 校验向量: esp_rom_crc32_le(0, "123456789", 9) == 0xCBF43926
 * 测试启动时会自检这个向量, 保证桩与真机同算法。 */
#pragma once
#include <stdint.h>

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
