#include "lvgl.h"

#include <string.h>

#define MAX_OBJECTS 256
static lv_obj_t s_objects[MAX_OBJECTS];
static int s_used;
static lv_obj_t s_screen;
int lv_stub_invalidate_count;

static lv_obj_t *new_object(void)
{
    if (s_used >= MAX_OBJECTS) return NULL;
    lv_obj_t *obj = &s_objects[s_used++];
    memset(obj, 0, sizeof(*obj));
    obj->x = obj->y = 0;
    obj->w = obj->h = 0;
    return obj;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { (void)parent; return new_object(); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; return new_object(); }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { (void)parent; return new_object(); }
lv_obj_t *lv_screen_active(void) { return &s_screen; }

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

void lv_obj_set_height(lv_obj_t *obj, int h)
{
    if (obj) obj->h = h;
}

void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector)
{ (void)obj; (void)pad; (void)selector; }

void lv_obj_set_style_text_outline_stroke_width(lv_obj_t *obj, int w, int selector)
{ (void)obj; (void)w; (void)selector; }

void lv_obj_set_style_text_outline_stroke_color(lv_obj_t *obj, lv_color_t c, int selector)
{ (void)obj; (void)c; (void)selector; }

void lv_obj_set_size(lv_obj_t *obj, int w, int h)
{
    if (!obj) return;
    obj->w = w;
    obj->h = h;
}
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
void lv_obj_set_style_pad_all(lv_obj_t *obj, int pad, int selector)
{ (void)obj; (void)pad; (void)selector; }
void lv_obj_set_style_radius(lv_obj_t *obj, int radius, int selector)
{ (void)obj; (void)radius; (void)selector; }
void lv_obj_set_style_border_width(lv_obj_t *obj, int width, int selector)
{ (void)obj; (void)width; (void)selector; }
void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int mode) { (void)obj; (void)mode; }
void lv_obj_move_foreground(lv_obj_t *obj) { (void)obj; }
void lv_label_set_long_mode(lv_obj_t *obj, int mode) { (void)obj; (void)mode; }

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    size_t n = text ? strlen(text) : 0;
    if (n >= sizeof(obj->text)) n = sizeof(obj->text) - 1;
    memcpy(obj->text, text ? text : "", n);
    obj->text[n] = '\0';
}

lv_color_t lv_color_hex(uint32_t color) { return color; }
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format)
{
    (void)buf;
    (void)format;
    if (!obj) return;
    obj->w = w;
    obj->h = h;
}
void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; lv_stub_invalidate_count++; }

void lv_stub_reset(void)
{
    memset(s_objects, 0, sizeof(s_objects));
    memset(&s_screen, 0, sizeof(s_screen));
    s_used = 0;
    lv_stub_invalidate_count = 0;
}
