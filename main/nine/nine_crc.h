// Shared CRC-32/ISO-HDLC update used by pack/resource validation.
// The table is const in flash; no runtime RAM buffer is added.
#pragma once
#include <stddef.h>
#include <stdint.h>

uint32_t nine_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
// CRC with a byte range treated as zero (pack headers).
uint32_t nine_crc32_zero(const uint8_t *data, size_t len,
                         size_t zero_off, size_t zeros);
