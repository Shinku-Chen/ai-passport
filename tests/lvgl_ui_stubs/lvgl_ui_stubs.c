#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define MAX_OBJECTS 256
static lv_obj_t s_objects[MAX_OBJECTS];
static int s_used;

static lv_obj_t *new_object(void)
{
    if (s_used >= MAX_OBJECTS) return NULL;
    lv_obj_t *obj = &s_objects[s_used++];
    memset(obj, 0, sizeof(*obj));
    return obj;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { (void)parent; return new_object(); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; return new_object(); }

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

void lv_obj_remove_style_all(lv_obj_t *obj) { (void)obj; }
void lv_obj_set_pos(lv_obj_t *obj, int x, int y) { if (obj) { obj->x = x; obj->y = y; } }
void lv_obj_set_width(lv_obj_t *obj, int w)
{
    if (obj) obj->w = w;
}

void lv_obj_set_height(lv_obj_t *obj, int h)
{
    if (obj) obj->h = h;
}

void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int part)
{
    (void)obj; (void)pad; (void)part;
}

void lv_obj_set_size(lv_obj_t *obj, int w, int h) { if (obj) { obj->w = w; obj->h = h; } }
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector)
{ (void)selector; if (obj) obj->bg_opa = opa; }
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector)
{ (void)obj; (void)font; (void)selector; }
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector)
{ (void)obj; (void)align; (void)selector; }
void lv_obj_set_scroll_dir(lv_obj_t *obj, int dir) { (void)obj; (void)dir; }
void lv_obj_scroll_to_y(lv_obj_t *obj, int y, int anim) { (void)obj; (void)y; (void)anim; }
void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, int anim)
{ (void)obj; (void)x; (void)y; (void)anim; }
void lv_obj_move_foreground(lv_obj_t *obj) { (void)obj; }
void lv_label_set_long_mode(lv_obj_t *obj, int mode)
{
    if (obj) obj->long_mode = mode;
}

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    snprintf(obj->text, sizeof(obj->text), "%s", text ? text : "");
}

lv_color_t lv_color_hex(uint32_t color) { return color; }

lv_obj_t *lv_canvas_create(lv_obj_t *parent) { (void)parent; return new_object(); }
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format)
{ (void)obj; (void)buf; (void)w; (void)h; (void)format; }
void lv_image_set_align(lv_obj_t *obj, int align) { (void)obj; (void)align; }
void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; }

void lv_obj_set_style_radius(lv_obj_t *obj, int r, int part)
{
    (void)part;
    if (obj) obj->radius = r;
}

void lv_obj_set_style_border_width(lv_obj_t *obj, int w, int part)
{
    (void)part;
    if (obj) obj->border_w = w;
}
