// Minimal LVGL 9 object model for host tests. It records geometry, visibility,
// text and style calls; it does not render.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _lv_obj_t {
    int id;
    struct _lv_obj_t *parent;
    int x, y, w, h;
    bool hidden;
    bool scrollable;
    bool canvas;
    char text[2048];
    const struct lv_font_t *font;
    uint32_t bg_color;
    uint32_t text_color;
    int bg_opa;
    int align;
    int line_space;
} lv_obj_t;

typedef struct lv_font_t { int unused; } lv_font_t;
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;
typedef int lv_text_align_t;
typedef int lv_label_long_mode_t;
typedef int lv_dir_t;
typedef int lv_anim_enable_t;
typedef int lv_color_format_t;
typedef int lv_image_align_t;

#define LV_OBJ_FLAG_HIDDEN 0x0001u
#define LV_OBJ_FLAG_SCROLLABLE 0x0002u
#define LV_OBJ_FLAG_CLICKABLE 0x0004u
#define LV_TEXT_ALIGN_LEFT 0
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_LABEL_LONG_WRAP 0
#define LV_LABEL_LONG_CLIP 1
#define LV_DIR_VER 1
#define LV_ANIM_OFF 0
#define LV_COLOR_FORMAT_RGB565 1
#define LV_IMAGE_ALIGN_TOP_LEFT 0
#define LV_OPA_COVER 255
#define LV_SIZE_CONTENT 0x7FFFFFFF

lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, lv_color_format_t format);
void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag);
void lv_obj_set_pos(lv_obj_t *obj, int x, int y);
void lv_obj_set_size(lv_obj_t *obj, int w, int h);
void lv_obj_set_width(lv_obj_t *obj, int w);
void lv_obj_set_height(lv_obj_t *obj, int h);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector);
void lv_obj_set_style_border_width(lv_obj_t *obj, int width, int selector);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_radius(lv_obj_t *obj, int radius, int selector);
void lv_obj_set_style_pad_all(lv_obj_t *obj, int pad, int selector);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector);
void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t align, int selector);
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int space, int selector);
void lv_obj_set_scroll_dir(lv_obj_t *obj, lv_dir_t dir);
void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, lv_anim_enable_t anim);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode);
void lv_label_set_text(lv_obj_t *obj, const char *text);
const char *lv_label_get_text(const lv_obj_t *obj);
lv_color_t lv_color_hex(uint32_t color);
void lv_obj_invalidate(lv_obj_t *obj);

// Test helpers.
lv_obj_t *stub_root_object(void);
const char *stub_text(const lv_obj_t *obj);
bool stub_visible(const lv_obj_t *obj);
int stub_x(const lv_obj_t *obj);
int stub_y(const lv_obj_t *obj);
int stub_w(const lv_obj_t *obj);
int stub_h(const lv_obj_t *obj);
int stub_lock_violations(void);
void stub_reset_violations(void);
int stub_object_count(void);
