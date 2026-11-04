// Minimal LVGL object/flag model for the host UI-visibility regression.
// It deliberately does not draw: it only models hidden flags and label text so
// the production stella_ui.c visibility logic is exercised on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _lv_obj_t {
    bool hidden;
    bool scrollable;
    bool clickable;
    // 几何记录：只看位置/尺寸，不做布局/绘制（标题页“菜单不能遮住上半屏”回归用）。
    int x, y, w, h;
    int long_mode;   // lv_label_set_long_mode 记录
    int bg_opa;      // lv_obj_set_style_bg_opa 记录（高亮断言用）
    int radius;      // 圆角
    int border_w;    // 边框宽
    char text[2048];
} lv_obj_t;

typedef struct lv_font_t { int unused; } lv_font_t;
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;

#define LV_SIZE_CONTENT 0x7FFFFFFF
#define LV_OBJ_FLAG_HIDDEN 0x0001u
#define LV_OBJ_FLAG_SCROLLABLE 0x0002u
#define LV_OBJ_FLAG_CLICKABLE 0x0004u

#define LV_LABEL_LONG_WRAP 0
#define LV_LABEL_LONG_CLIP 3
#define LV_LABEL_LONG_MODE_CLIP 3
#define LV_LABEL_LONG_MODE_DOTS 2
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_OPA_COVER 255
#define LV_DIR_VER 1
#define LV_ANIM_OFF 0

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_style_all(lv_obj_t *obj);
void lv_obj_set_pos(lv_obj_t *obj, int x, int y);
void lv_obj_set_size(lv_obj_t *obj, int w, int h);
void lv_obj_set_width(lv_obj_t *obj, int w);
void lv_obj_set_height(lv_obj_t *obj, int h);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int part);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector);
void lv_obj_set_style_radius(lv_obj_t *obj, int r, int part);
void lv_obj_set_style_border_width(lv_obj_t *obj, int w, int part);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector);
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector);
void lv_obj_set_scroll_dir(lv_obj_t *obj, int dir);
void lv_obj_scroll_to_y(lv_obj_t *obj, int y, int anim);
void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, int anim);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_label_set_long_mode(lv_obj_t *obj, int mode);
void lv_label_set_text(lv_obj_t *obj, const char *text);
lv_color_t lv_color_hex(uint32_t color);

#define LV_COLOR_FORMAT_RGB565 1
#define LV_IMAGE_ALIGN_TOP_LEFT 0
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format);
void lv_image_set_align(lv_obj_t *obj, int align);
void lv_obj_invalidate(lv_obj_t *obj);
