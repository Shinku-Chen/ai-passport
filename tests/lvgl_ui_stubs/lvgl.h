// Minimal LVGL object/flag/canvas model for the host image/UI regressions.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _lv_obj_t {
    bool hidden;
    bool scrollable;
    bool clickable;
    int x, y, w, h;
    int bg_opa;
    char text[2048];
} lv_obj_t;

typedef struct _lv_font_t { int unused; } lv_font_t;
#define LV_FONT_DECLARE(name) extern const lv_font_t name
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;

#define LV_SIZE_CONTENT 0x7FFFFFFF   // 桩：内容自适应哨兵
#define LV_OBJ_FLAG_HIDDEN 0x0001u
#define LV_OBJ_FLAG_SCROLLABLE 0x0002u
#define LV_OBJ_FLAG_CLICKABLE 0x0004u

#define LV_LABEL_LONG_WRAP 0
#define LV_LABEL_LONG_CLIP 1
#define LV_TEXT_ALIGN_LEFT 0
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_OPA_80 204
#define LV_OPA_90 230
#define LV_PART_MAIN 0
#define LV_COLOR_FORMAT_RGB565 1
#define LV_SCROLLBAR_MODE_OFF 0

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
lv_obj_t *lv_screen_active(void);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_style_all(lv_obj_t *obj);
void lv_obj_set_pos(lv_obj_t *obj, int x, int y);
void lv_obj_set_size(lv_obj_t *obj, int w, int h);
void lv_obj_set_width(lv_obj_t *obj, int w);
void lv_obj_set_height(lv_obj_t *obj, int h);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector);
void lv_obj_set_style_text_outline_stroke_width(lv_obj_t *obj, int w, int selector);
void lv_obj_set_style_text_outline_stroke_color(lv_obj_t *obj, lv_color_t c, int selector);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector);
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector);
void lv_obj_set_style_pad_all(lv_obj_t *obj, int pad, int selector);
void lv_obj_set_style_radius(lv_obj_t *obj, int radius, int selector);
void lv_obj_set_style_border_width(lv_obj_t *obj, int width, int selector);
void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int mode);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_obj_invalidate(lv_obj_t *obj);
extern int lv_stub_invalidate_count;
void lv_stub_reset(void);
void lv_label_set_long_mode(lv_obj_t *obj, int mode);
void lv_label_set_text(lv_obj_t *obj, const char *text);
lv_color_t lv_color_hex(uint32_t color);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format);
