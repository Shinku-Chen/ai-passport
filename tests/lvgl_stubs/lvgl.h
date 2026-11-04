// Host-side LVGL thin stub: records objects so production gal_ui/gal_app can be
// driven exactly like on device, including visibility, labels and lock checks.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct _lv_obj_t lv_obj_t;
typedef struct { uint8_t r, g, b; } lv_color_t;
typedef uint8_t lv_opa_t;
typedef struct { int dummy; } lv_font_t;

typedef enum { LV_PART_MAIN = 0 } lv_part_t;
typedef enum { LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_SCROLLABLE = 2 } lv_obj_flag_t;
typedef enum { LV_OPA_TRANSP = 0, LV_OPA_COVER = 255 } lv_opa_enum_t;
#define LV_SIZE_CONTENT 0x7fff
typedef enum { LV_TEXT_ALIGN_LEFT = 0, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT } lv_text_align_t;
typedef enum { LV_LABEL_LONG_MODE_WRAP = 0, LV_LABEL_LONG_MODE_DOTS } lv_label_long_mode_t;
typedef enum { LV_COLOR_FORMAT_RGB565 = 0x12 } lv_color_format_t;
typedef enum { LV_IMAGE_ALIGN_TOP_LEFT = 0 } lv_image_align_t;
typedef enum { LV_BORDER_SIDE_NONE = 0, LV_BORDER_SIDE_TOP = 1, LV_BORDER_SIDE_LEFT = 2 } lv_border_side_t;

#define LV_OPA_70 178
#define LV_OPA_60 153

typedef struct _lv_obj_t {
    int id;
    int kind;                 // 0 obj, 1 label, 2 canvas
    lv_obj_t *parent;
    lv_obj_t *next;
    char text[512];
    int x, y, w, h;
    uint32_t bg_color;
    uint32_t text_color;
    int bg_opa;
    int border_width;
    int radius;
    int pad_left, pad_right;
    int hidden;
    int scrollable;
    int text_align;
    int long_mode;
    int scale;
    int pivot_x, pivot_y;
    const lv_font_t *font;
    uint16_t *canvas_buf;
    int canvas_w, canvas_h;
    int invalidated;
    void *user_data;
} lv_obj_t;

lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_obj_remove_style_all(lv_obj_t *obj);
void lv_obj_set_pos(lv_obj_t *obj, int x, int y);
void lv_obj_set_size(lv_obj_t *obj, int w, int h);
void lv_obj_set_width(lv_obj_t *obj, int w);
void lv_obj_set_height(lv_obj_t *obj, int h);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t c, lv_part_t part);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, lv_part_t part);
void lv_obj_set_style_border_width(lv_obj_t *obj, int w, lv_part_t part);
void lv_obj_set_style_border_side(lv_obj_t *obj, lv_border_side_t side, lv_part_t part);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t c, lv_part_t part);
void lv_obj_set_style_radius(lv_obj_t *obj, int r, lv_part_t part);
void lv_obj_set_style_pad_left(lv_obj_t *obj, int px, lv_part_t part);
void lv_obj_set_style_pad_right(lv_obj_t *obj, int px, lv_part_t part);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, lv_part_t part);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t c, lv_part_t part);
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int px, lv_part_t part);
void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t align, lv_part_t part);
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_obj_invalidate(lv_obj_t *obj);
void lv_obj_set_user_data(lv_obj_t *obj, void *data);
void *lv_obj_get_user_data(lv_obj_t *obj);
void lv_label_set_text(lv_obj_t *label, const char *text);
void lv_label_set_long_mode(lv_obj_t *label, lv_label_long_mode_t mode);
void lv_canvas_set_buffer(lv_obj_t *canvas, void *buf, int w, int h, lv_color_format_t cf);
void *lv_malloc(size_t size);
void lv_free(void *ptr);
void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align);
void lv_image_set_scale(lv_obj_t *obj, int scale);
void lv_image_set_pivot(lv_obj_t *obj, int x, int y);
lv_color_t lv_color_hex(uint32_t hex);

// 测试辅助
void lv_stub_reset(void);
void lv_stub_require_lock(bool required);
int lv_stub_lock_violations(void);
int lv_stub_obj_count(void);
lv_obj_t *lv_stub_obj(int index);
bool lv_stub_visible(const lv_obj_t *obj);
const char *lv_stub_text(const lv_obj_t *obj);
void lv_stub_clear_invalidations(void);
int lv_stub_invalidations(void);
void lv_stub_set_locked(bool locked);
bool lv_stub_locked(void);
