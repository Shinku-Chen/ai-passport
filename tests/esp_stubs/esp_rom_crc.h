// tests/esp_stubs/esp_rom_crc.h —— CRC32 桩(与 ESP-ROM 同样的 reflected CRC-32)。
#pragma once

#include <stdint.h>
#include <stddef.h>

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
