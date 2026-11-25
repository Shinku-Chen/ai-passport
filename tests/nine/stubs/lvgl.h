#pragma once
#include <stdint.h>
typedef struct _lv_obj_t lv_obj_t;
typedef struct lv_font_t { int dummy; } lv_font_t;
typedef struct lv_timer_t lv_timer_t;
typedef struct lv_image_dsc_t lv_image_dsc_t;
typedef struct { uint8_t r, g, b; } lv_color_t;
#define LV_FONT_DECLARE(name) extern const lv_font_t name
#define LV_PART_MAIN 0
#define LV_OPA_COVER 255
#define LV_LABEL_LONG_WRAP 2
lv_obj_t *lv_screen_active(void);
void lv_obj_move_background(lv_obj_t *obj);
void lv_obj_clean(lv_obj_t *obj);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, uint32_t part);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, uint8_t opa, uint32_t part);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, uint32_t part);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, uint32_t part);
void lv_obj_set_width(lv_obj_t *obj, int32_t w);
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y);
void lv_label_set_long_mode(lv_obj_t *obj, int32_t mode);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_refr_now(void *disp);
lv_color_t lv_color_black(void);
lv_color_t lv_color_white(void);
typedef struct { size_t total_size, free_size, free_biggest_size; uint8_t used_pct, frag_pct; } lv_mem_monitor_t;
void lv_mem_monitor(lv_mem_monitor_t *mon);
