// main/gal/maitetsu_pack.h —— 媒体包解析与有界条带解码（不依赖 LVGL）。
//
// 媒体包把每张图像切成独立的 30 行 JPEG 条带：量化表与 Huffman 表按质量档
// 共享一次，条带本身只有 35 字节扫描头加熵编码数据。解码时只需要一块
// 320x30 的 RGB565 缓冲，不会为整幅图像分配 115KB 的帧缓冲。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAITETSU_BAND_ROWS 30
#define MAITETSU_MAX_WIDTH 320
#define MAITETSU_MAX_IMAGES 768

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t first_band;
    uint8_t bands;
    uint8_t prefix;
    uint32_t crc;
} maitetsu_image_t;

typedef struct {
    const uint16_t *pixels;   // width * rows 的 RGB565，行距 = width
    uint16_t width;
    uint16_t rows;
} maitetsu_band_t;

// 校验包头与表结构。失败时写 error 并保持未就绪；不会继续使用任何旧包。
bool maitetsu_pack_init(const uint8_t *data, size_t size, char *error, size_t error_size);

bool maitetsu_pack_ready(void);

// 包内图像数量（未就绪时 0）。
uint16_t maitetsu_pack_image_count(void);

// 图像描述；下标非法返回 NULL。
const maitetsu_image_t *maitetsu_pack_image(uint16_t index);

// 解码第 band 条带到内部缓冲。首次使用某图像时校验它的 CRC32；
// 失败返回 false，且不会留下半解码的条带。
bool maitetsu_pack_band(uint16_t image, uint8_t band, maitetsu_band_t *out);

// 自检统计（host 测试用）：成功解码的条带数、CRC 校验过的图像数。
uint32_t maitetsu_pack_decoded_bands(void);
uint32_t maitetsu_pack_validated_images(void);

// CRC32（与包内算法一致，语义同 zlib.crc32 的续算）。
uint32_t maitetsu_crc32(const uint8_t *data, size_t len);
uint32_t maitetsu_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
