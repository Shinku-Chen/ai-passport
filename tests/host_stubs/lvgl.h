// tests/host_stubs/lvgl.h —— 宿主机用的 LVGL 薄桩。
//
// 只建模"对象是否存在、是否隐藏、标签文本是什么",不画像素。这样 steins_ui.c /
// steins_app.c 的真实可见性与行文本逻辑可以在宿主机上直接跑回归,不需要屏幕。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LV_TEXT_CAP 2048

typedef struct _lv_obj_t {
    bool hidden;
    bool scrollable;
    int scroll_y;      // 桩：只记垂直滚动量（ABOUT 正文分页验证用）
    int scrollbar_mode;
    bool clickable;
    int x, y, w, h;
    int long_mode;
    char text[LV_TEXT_CAP];
    struct _lv_obj_t *parent;
} lv_obj_t;

typedef struct _lv_font_t {
    int unused;
} lv_font_t;

typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;

#define LV_SIZE_CONTENT 0x7FFFFFFF   // 桩：内容自适应哨兵
#define LV_LABEL_LONG_MODE_WRAP 0
#define LV_LABEL_LONG_MODE_CLIP 4
#define LV_OBJ_FLAG_HIDDEN 0x0001u
#define LV_OBJ_FLAG_SCROLLABLE 0x0002u
#define LV_OBJ_FLAG_CLICKABLE 0x0004u
#define LV_SCROLLBAR_MODE_OFF 0
#define LV_SCROLLBAR_MODE_ON 1
#define LV_ANIM_OFF 0

#define LV_LABEL_LONG_WRAP 0
#define LV_TEXT_ALIGN_CENTER 1
#define LV_TEXT_ALIGN_RIGHT 2
#define LV_OPA_COVER 255
#define LV_COLOR_FORMAT_RGB565 1
#define LV_IMAGE_ALIGN_TOP_LEFT 0

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
lv_obj_t *lv_screen_active(void);
void lv_obj_remove_style_all(lv_obj_t *obj);
void lv_obj_set_pos(lv_obj_t *obj, int x, int y);
void lv_obj_set_size(lv_obj_t *obj, int w, int h);
void lv_obj_set_width(lv_obj_t *obj, int w);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector);
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector);
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int space, int selector);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int mode);
void lv_obj_scroll_by(lv_obj_t *obj, int dx, int dy, int anim);
int lv_obj_get_scroll_y(const lv_obj_t *obj);
void lv_obj_invalidate(lv_obj_t *obj);
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag);
void lv_obj_move_foreground(lv_obj_t *obj);
void lv_obj_invalidate(lv_obj_t *obj);
void lv_label_set_long_mode(lv_obj_t *obj, int mode);
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format);
void lv_image_set_align(lv_obj_t *obj, int align);
lv_color_t lv_color_hex(uint32_t color);

// 记录:测试用
int lv_stub_object_count(void);
void lv_stub_reset_render_counters(void);
const char *lv_stub_last_text(void);
