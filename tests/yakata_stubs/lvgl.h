#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int unused; } lv_obj_t;
typedef struct { int unused; } lv_timer_t;
typedef struct { int unused; } lv_font_t;
#define LV_FONT_DECLARE(n) extern const lv_font_t n
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RAW 1
typedef struct { unsigned magic, cf, w, h; } lv_image_header_t;
typedef struct { lv_image_header_t header; unsigned data_size; const uint8_t *data; } lv_image_dsc_t;
