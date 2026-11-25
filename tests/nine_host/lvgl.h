#pragma once
#include <stdint.h>
typedef struct _lv_obj_t lv_obj_t;
lv_obj_t *lv_screen_active(void);
typedef struct lv_font_t { int dummy; } lv_font_t;
typedef struct lv_timer_t lv_timer_t;
#define LV_FONT_DECLARE(name) extern const lv_font_t name
#define LV_IMAGE_HEADER_MAGIC 0x19u
#define LV_COLOR_FORMAT_RGB565 0x12u
typedef struct {
    uint32_t magic : 8;
    uint32_t cf : 8;
    uint32_t flags : 16;
    uint32_t w : 16;
    uint32_t h : 16;
    uint32_t stride : 16;
    uint32_t reserved_2 : 16;
} lv_image_header_t;
typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
} lv_image_dsc_t;
