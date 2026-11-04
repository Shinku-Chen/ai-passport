// tests/lvgl_stubs/lvgl.h —— 供宿主机测试用的最小 LVGL 对象模型(不是渲染器)。
//
// 只实现 gal_ui.c / gal_image.c 用到的 API:对象树、可见性(含祖先)、位置/尺寸、
// 标签文本、滚动位置、定时器、画布缓冲。用于断言"对象是否真的可见/排布"。
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
typedef struct lv_timer_t lv_timer_t;
struct lv_font_t { int size; };
typedef struct lv_font_t lv_font_t;

typedef struct {
    uint32_t v;
} lv_color_t;

typedef uint8_t lv_opa_t;
typedef int lv_color_format_t;
typedef int lv_image_align_t;
typedef uint32_t lv_obj_flag_t;
typedef int lv_part_t;

#define LV_PART_MAIN 0
#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_OPA_60 153
#define LV_OPA_80 204
#define LV_OPA_90 230
#define LV_COLOR_FORMAT_RGB565 1
#define LV_IMAGE_ALIGN_TOP_LEFT 0
#define LV_LABEL_LONG_MODE_WRAP 0
#define LV_LABEL_LONG_MODE_DOTS 1
#define LV_LABEL_LONG_MODE_CLIP 4
#define LV_OBJ_FLAG_SCROLLABLE 0x00000001u
#define LV_OBJ_FLAG_HIDDEN 0x00000004u
#define LV_BORDER_SIDE_TOP 1
#define LV_DIR_VER 2
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_ANIM_OFF 0
#define LV_COORD_MAX 32767

typedef struct {
    int32_t x, y, w, h;
} lv_area_t;

struct _lv_obj_t {
    lv_obj_t *parent;
    lv_obj_t *first_child;
    lv_obj_t *last_child;
    lv_obj_t *next_sibling;
    int32_t x, y, w, h;
    uint32_t flags;
    int kind;              // 0 = obj, 1 = label, 2 = canvas
    char *text;            // label
    void *buffer;          // canvas
    int32_t buf_w, buf_h;
    int32_t scroll_y;
    int32_t scroll_max;
    uint32_t bg_opa;
    uint32_t bg_color;
};

// ---- 对象 ----
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_obj_remove_style_all(lv_obj_t *obj);
void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h);
void lv_obj_set_width(lv_obj_t *obj, int32_t w);
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y);
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag);
bool lv_obj_has_flag(const lv_obj_t *obj, lv_obj_flag_t flag);
void lv_obj_clean(lv_obj_t *obj);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_obj_invalidate(lv_obj_t *obj);
void lv_obj_update_layout(lv_obj_t *obj);
void lv_obj_set_scroll_dir(lv_obj_t *obj, int dir);
void lv_obj_scroll_to_y(lv_obj_t *obj, int32_t y, int anim);
int32_t lv_obj_get_scroll_y(const lv_obj_t *obj);
lv_obj_t *lv_screen_active(void);

// ---- 样式(记录少量数值供断言)----
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, lv_part_t part);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, lv_part_t part);
void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t w, lv_part_t part);
void lv_obj_set_style_border_side(lv_obj_t *obj, int side, lv_part_t part);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, lv_part_t part);
void lv_obj_set_style_radius(lv_obj_t *obj, int32_t r, lv_part_t part);
void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t p, lv_part_t part);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, lv_part_t part);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, lv_part_t part);
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, lv_part_t part);
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int32_t space, lv_part_t part);
lv_color_t lv_color_hex(uint32_t rgb);

// ---- 标签 ----
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...);
void lv_label_set_long_mode(lv_obj_t *obj, int mode);

// ---- 画布 ----
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int32_t w, int32_t h, lv_color_format_t cf);
void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align);
#define LV_SIZE_CONTENT 0x7FFFFFFF
void lv_obj_set_height(lv_obj_t *obj, int h);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int part);
int32_t lv_obj_get_child_cnt(const lv_obj_t *obj);
void lv_image_set_scale(lv_obj_t *obj, uint32_t scale);
void lv_image_set_pivot(lv_obj_t *obj, int x, int y);

// ---- 定时器(测试里手动推进)----
lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t period, void *user);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void lv_timer_reset(lv_timer_t *timer);
void *lv_timer_get_user_data(const lv_timer_t *timer);
void lv_timer_delete(lv_timer_t *timer);

// ---- 测试辅助(不属于 LVGL)----
bool lv_stub_visible(const lv_obj_t *obj);      // 自身+所有祖先都没有 HIDDEN
int lv_stub_child_count(const lv_obj_t *obj);
lv_obj_t *lv_stub_child(const lv_obj_t *obj, int index);
void lv_stub_reset(void);
