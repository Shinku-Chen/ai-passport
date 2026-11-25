// Shared reflected CRC32 used by the package, image, media and save readers.
#pragma once
#include <stddef.h>
#include <stdint.h>

uint32_t nine_crc32_update(uint32_t crc, const uint8_t *p, size_t n);
uint32_t nine_crc32_bytes(const uint8_t *p, size_t n);
// CRC over p[0..len) with the bytes [zero_off, zero_off+zeros) treated as zero.
// SIZE_MAX zero_off means no zeroed range.
uint32_t nine_crc32_zero(const uint8_t *p, size_t len, size_t zero_off, size_t zeros);
