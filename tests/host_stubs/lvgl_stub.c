// tests/host_stubs/lvgl_stub.c —— LVGL 薄桩实现(对象表 + 文本记录)。
#include "lvgl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_OBJECTS 512

static lv_obj_t s_objects[MAX_OBJECTS];
static int s_used;
static lv_obj_t s_screen;
static char s_last_text[LV_TEXT_CAP];

static lv_obj_t *new_object(lv_obj_t *parent)
{
    if (s_used >= MAX_OBJECTS) return NULL;
    lv_obj_t *obj = &s_objects[s_used++];
    memset(obj, 0, sizeof(*obj));
    obj->parent = parent;
    return obj;
}

lv_obj_t *lv_screen_active(void)
{
    memset(&s_screen, 0, sizeof(s_screen));
    return &s_screen;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { return new_object(parent); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return new_object(parent); }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { return new_object(parent); }

void lv_obj_remove_style_all(lv_obj_t *obj) { (void)obj; }

void lv_obj_set_pos(lv_obj_t *obj, int x, int y)
{
    if (!obj) return;
    obj->x = x;
    obj->y = y;
}

void lv_obj_set_width(lv_obj_t *obj, int w)
{
    if (obj) obj->w = w;
}

void lv_obj_set_size(lv_obj_t *obj, int w, int h)
{
    if (!obj) return;
    obj->w = w;
    obj->h = h;
}

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector)
{
    (void)obj;
    (void)color;
    (void)selector;
}
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector)
{
    (void)obj;
    (void)opa;
    (void)selector;
}
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector)
{
    (void)obj;
    (void)color;
    (void)selector;
}
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector)
{
    (void)obj;
    (void)font;
    (void)selector;
}
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector)
{
    (void)obj;
    (void)pad;
    (void)selector;
}

void lv_obj_set_style_text_line_space(lv_obj_t *obj, int space, int selector)
{
    (void)obj;
    (void)space;
    (void)selector;
}

void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector)
{
    (void)obj;
    (void)align;
    (void)selector;
}

void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int mode)
{
    if (obj) obj->scrollbar_mode = mode;
}

void lv_obj_scroll_by(lv_obj_t *obj, int dx, int dy, int anim)
{
    (void)dx; (void)anim;
    if (obj) obj->scroll_y += dy;
}

int lv_obj_get_scroll_y(const lv_obj_t *obj)
{
    return obj ? obj->scroll_y : 0;
}

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag)
{
    if (!obj) return;
    if (flag & LV_OBJ_FLAG_HIDDEN) obj->hidden = true;
    if (flag & LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = true;
    if (flag & LV_OBJ_FLAG_CLICKABLE) obj->clickable = true;
}

void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag)
{
    if (!obj) return;
    if (flag & LV_OBJ_FLAG_HIDDEN) obj->hidden = false;
    if (flag & LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = false;
    if (flag & LV_OBJ_FLAG_CLICKABLE) obj->clickable = false;
}

bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag)
{
    if (!obj) return false;
    if (flag & LV_OBJ_FLAG_HIDDEN) return obj->hidden;
    if (flag & LV_OBJ_FLAG_SCROLLABLE) return obj->scrollable;
    if (flag & LV_OBJ_FLAG_CLICKABLE) return obj->clickable;
    return false;
}

void lv_obj_move_foreground(lv_obj_t *obj) { (void)obj; }
void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; }
void lv_label_set_long_mode(lv_obj_t *obj, int mode) { if (obj) obj->long_mode = mode; }

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    snprintf(obj->text, sizeof(obj->text), "%s", text ? text : "");
    snprintf(s_last_text, sizeof(s_last_text), "%s", obj->text);
}

void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format)
{
    (void)obj;
    (void)buf;
    (void)w;
    (void)h;
    (void)format;
}
void lv_image_set_align(lv_obj_t *obj, int align) { (void)obj; (void)align; }
lv_color_t lv_color_hex(uint32_t color) { return (lv_color_t)color; }

int lv_stub_object_count(void) { return s_used; }
void lv_stub_reset_render_counters(void) { s_used = 0; }
const char *lv_stub_last_text(void) { return s_last_text; }

// 宿主计时桩：0 号时间轴，避免引入 <time.h> 依赖。
int64_t esp_timer_get_time(void)
{
    return 0;
}
