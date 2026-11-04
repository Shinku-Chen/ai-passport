// tests/host/stubs/lvgl.h —— 极薄的 LVGL 对象 stub：只记录对象树、文本与坐标，供 UI 回归。
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { LV_COLOR_FORMAT_RGB565 = 0x12 } lv_color_format_t;
typedef struct {
    uint32_t magic : 8, cf : 8, flags : 16;
    uint32_t w : 16, h : 16, stride : 16, reserved_2 : 16;
} lv_image_header_t;
typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
} lv_image_dsc_t;
#define LV_IMAGE_HEADER_MAGIC 0x19

typedef struct lv_color_s { uint32_t v; } lv_color_t;
typedef uint8_t lv_opa_t;
typedef struct lv_font_s { int dummy; } lv_font_t;

typedef enum { LV_OPA_TRANSP = 0, LV_OPA_COVER = 255, LV_OPA_80 = 204, LV_OPA_60 = 153,
               LV_OPA_50 = 128, LV_OPA_70 = 178 } lv_opa_const_t;
#define LV_OPA_80 204
#define LV_OPA_70 178
#define LV_OPA_60 153
#define LV_OPA_50 128
#define LV_OPA_COVER 255

typedef enum { LV_TEXT_ALIGN_LEFT = 0, LV_TEXT_ALIGN_RIGHT = 1, LV_TEXT_ALIGN_CENTER = 2 } lv_text_align_t;
typedef enum { LV_LABEL_LONG_WRAP = 0, LV_LABEL_LONG_DOT = 1 } lv_label_long_mode_t;
typedef enum { LV_SCROLLBAR_MODE_OFF = 0, LV_SCROLLBAR_MODE_ON = 1 } lv_scrollbar_mode_t;

#define LV_FONT_DECLARE(name) extern const lv_font_t name

typedef struct lv_obj_s {
    int id;
    int x, y, w, h;
    char text[96];
    struct lv_obj_s *parent;
    struct lv_obj_s *first_child;
    struct lv_obj_s *next_sibling;
    uint32_t bg;
    lv_opa_t bg_opa;
    uint32_t fg;
    int align;
} lv_obj_t;

// stub 侧 API（记录 + 查询）
void lv_stub_reset(void);
int lv_stub_count(void);
const lv_obj_t *lv_stub_at(int i);
const lv_obj_t *lv_stub_find_text(const char *text);
int lv_stub_rows_under(lv_obj_t *parent);
void lv_stub_free_all(void);

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
void lv_obj_del(lv_obj_t *obj);
lv_obj_t *lv_screen_active(void);
void lv_obj_set_size(lv_obj_t *o, int w, int h);
void lv_obj_set_pos(lv_obj_t *o, int x, int y);
void lv_obj_set_width(lv_obj_t *o, int w);
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int part);
void lv_obj_set_style_bg_opa(lv_obj_t *o, lv_opa_t opa, int part);
void lv_obj_set_style_border_width(lv_obj_t *o, int w, int part);
void lv_obj_set_style_border_color(lv_obj_t *o, lv_color_t c, int part);
void lv_obj_set_style_radius(lv_obj_t *o, int r, int part);
void lv_obj_set_style_pad_all(lv_obj_t *o, int p, int part);
void lv_obj_set_scrollbar_mode(lv_obj_t *o, lv_scrollbar_mode_t m);
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int part);
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int part);
void lv_obj_set_style_text_align(lv_obj_t *o, lv_text_align_t a, int part);
void lv_obj_set_style_text_line_space(lv_obj_t *o, int px, int part);
void lv_label_set_text(lv_obj_t *o, const char *text);
void lv_label_set_long_mode(lv_obj_t *o, lv_label_long_mode_t m);
lv_obj_t *lv_obj_get_child(lv_obj_t *o, int idx);
void lv_image_set_src(lv_obj_t *o, const void *src);
void lv_obj_invalidate(lv_obj_t *o);
lv_color_t lv_color_hex(uint32_t hex);
