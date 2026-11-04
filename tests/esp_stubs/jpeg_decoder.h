// tests/esp_stubs/jpeg_decoder.h —— esp_jpeg 的最小子集(宿主机测试桩)。
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    JPEG_IMAGE_SCALE_0 = 0,
} esp_jpeg_image_scale_t;

typedef enum {
    JPEG_IMAGE_FORMAT_RGB888 = 0,
    JPEG_IMAGE_FORMAT_RGB565,
} esp_jpeg_image_format_t;

typedef struct esp_jpeg_image_cfg_s {
    uint8_t *indata;
    uint32_t indata_size;
    uint8_t *outbuf;
    uint32_t outbuf_size;
    esp_jpeg_image_format_t out_format;
    esp_jpeg_image_scale_t out_scale;
    struct {
        uint8_t swap_color_bytes : 1;
    } flags;
    struct {
        void *working_buffer;
        size_t working_buffer_size;
    } advanced;
    struct {
        uint32_t read;
    } priv;
} esp_jpeg_image_cfg_t;

typedef struct esp_jpeg_image_output_s {
    uint16_t width;
    uint16_t height;
    size_t output_len;
} esp_jpeg_image_output_t;

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out);
