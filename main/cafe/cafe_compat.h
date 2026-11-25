// main/cafe/cafe_compat.h —— CRC32 + zlib 解压的最小封装。
//
// 目标固件用 ESP32-C3 ROM 的 tinfl_decompress_mem_to_mem（经 esp_rom/miniz.h），
// 宿主机测试用 tests/cafe_stubs 里的同接口 C 实现。这样固件不依赖外部 zlib
// 组件，测试又读的是真实的 zlib 压缩数据。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t cafe_crc32(uint32_t crc, const void *data, size_t len);

// 解压 zlib 流。成功时 *out_len = 实际字节数；失败返回 false 且不保证 dst 内容。
bool cafe_inflate(void *dst, size_t dst_cap, size_t *out_len, const void *src, size_t src_len);
