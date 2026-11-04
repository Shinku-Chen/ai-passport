// tests/host/stubs/lvgl.h —— 极薄 LVGL 对象 stub：记录对象树/文本/坐标，供 UI 回归。
// 每个 lv_* 调用都会检查 bsp_lvgl_lock 的持有状态（host_lvgl_violations）。
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

#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_SIZE_CONTENT 0x7FFFFFFF
#define LV_OPA_80 204
#define LV_OPA_70 178
#define LV_OPA_60 153
#define LV_OPA_50 128

typedef enum { LV_TEXT_ALIGN_LEFT = 0, LV_TEXT_ALIGN_RIGHT = 1, LV_TEXT_ALIGN_CENTER = 2 } lv_text_align_t;
typedef enum { LV_LABEL_LONG_WRAP = 0, LV_LABEL_LONG_DOT = 1, LV_LABEL_LONG_CLIP = 3 } lv_label_long_mode_t;
typedef enum { LV_SCROLLBAR_MODE_OFF = 0, LV_SCROLLBAR_MODE_ON = 1 } lv_scrollbar_mode_t;

#define LV_FONT_DECLARE(name) extern const lv_font_t name

typedef struct lv_obj_s {
    int id;
    int x, y, w, h;
    char text[512];
    struct lv_obj_s *parent;
    struct lv_obj_s *first_child;
    struct lv_obj_s *next_sibling;
    uint32_t bg;
    lv_opa_t bg_opa;
    uint32_t fg;
    int align;
    int is_image;
    const void *src;      // lv_image_set_src 记录，用于断言双带
    int invalidated;      // lv_obj_invalidate 次数
    const struct lv_font_s *font;
} lv_obj_t;

void lv_stub_reset(void);
int lv_stub_count(void);
const lv_obj_t *lv_stub_at(int i);
const lv_obj_t *lv_stub_find_text(const char *text);
const lv_obj_t *lv_stub_find_containing(const char *needle);
int lv_stub_rows_under(lv_obj_t *parent);
// 清空所有 invalidate 计数（测“两次 flush 区域”用）。
void lv_stub_reset_invalidations(void);
void lv_stub_free_all(void);

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
void lv_obj_del(lv_obj_t *obj);
lv_obj_t *lv_screen_active(void);
void lv_obj_set_size(lv_obj_t *o, int w, int h);
void lv_obj_set_pos(lv_obj_t *o, int x, int y);
void lv_obj_set_width(lv_obj_t *o, int w);
void lv_obj_set_height(lv_obj_t *o, int h);
void lv_obj_set_style_pad_hor(lv_obj_t *o, int pad, int part);
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
