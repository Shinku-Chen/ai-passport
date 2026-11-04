#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define MAX_OBJECTS 16384
static lv_obj_t s_objects[MAX_OBJECTS];
static int s_used;
static lv_obj_t s_screen;
static int s_locked;
static int s_violations;
static int s_fail_next;
static int s_fail_after = -1;

static void touch(void)
{
    if (!s_locked) ++s_violations;
}

static lv_obj_t *new_object(lv_obj_t *parent)
{
    touch();
    if (s_fail_next > 0) { --s_fail_next; return NULL; }
    if (s_fail_after >= 0) {
        if (s_fail_after == 0) { s_fail_after = -1; return NULL; }
        --s_fail_after;
    }
    if (s_used >= MAX_OBJECTS) return NULL;
    lv_obj_t *obj = &s_objects[s_used++];
    memset(obj, 0, sizeof(*obj));
    obj->parent = parent;
    return obj;
}

static void delete_tree(lv_obj_t *obj)
{
    if (!obj) return;
    for (int i = 0; i < s_used; ++i) {
        lv_obj_t *child = &s_objects[i];
        if (!child->deleted && child->parent == obj) delete_tree(child);
    }
    obj->deleted = true;
}

void host_lvgl_reset(void)
{
    memset(s_objects, 0, sizeof(s_objects));
    memset(&s_screen, 0, sizeof(s_screen));
    s_used = 0;
    s_locked = 0;
    s_violations = 0;
    s_fail_next = 0;
    s_fail_after = -1;
}

void host_lvgl_set_locked(int locked) { s_locked = locked; }
void host_lvgl_fail_next(int count) { s_fail_next = count > 0 ? count : 0; }
void host_lvgl_fail_after(int successes) { s_fail_after = successes >= 0 ? successes : -1; }
int host_lvgl_violations(void) { return s_violations; }
int host_lvgl_object_count(void)
{
    int count = 0;
    for (int i = 0; i < s_used; ++i) if (!s_objects[i].deleted) ++count;
    return count;
}
const lv_obj_t *host_lvgl_object(int index)
{
    return index >= 0 && index < s_used ? &s_objects[index] : NULL;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { return new_object(parent); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return new_object(parent); }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { return new_object(parent); }
lv_obj_t *lv_screen_active(void) { return &s_screen; }

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag)
{
    touch();
    if (!obj) return;
    if (flag & LV_OBJ_FLAG_HIDDEN) obj->hidden = true;
    if (flag & LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = true;
    if (flag & LV_OBJ_FLAG_CLICKABLE) obj->clickable = true;
}

void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag)
{
    touch();
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
void lv_obj_set_pos(lv_obj_t *obj, int x, int y) { touch(); if (obj) { obj->x = x; obj->y = y; } }
void lv_obj_set_size(lv_obj_t *obj, int w, int h) { touch(); if (obj) { obj->w = w; obj->h = h; } }
void lv_obj_set_width(lv_obj_t *obj, int w) { touch(); if (obj) obj->w = w; }
void lv_obj_set_style_pad_left(lv_obj_t *obj, int pad, int selector) { (void)obj; (void)pad; (void)selector; }
void lv_obj_set_style_pad_right(lv_obj_t *obj, int pad, int selector) { (void)obj; (void)pad; (void)selector; }
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector)
{ (void)obj; (void)opa; (void)selector; }
void lv_obj_set_style_radius(lv_obj_t *obj, int radius, int selector)
{ (void)obj; (void)radius; (void)selector; }
void lv_obj_set_style_border_width(lv_obj_t *obj, int width, int selector)
{ (void)obj; (void)width; (void)selector; }
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector)
{ if (obj) obj->font = font; (void)selector; }
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, int selector)
{ if (obj) obj->align = align; (void)selector; }
void lv_obj_set_scroll_dir(lv_obj_t *obj, int dir) { (void)obj; (void)dir; }
void lv_obj_scroll_to_y(lv_obj_t *obj, int y, int anim)
{ touch(); if (obj) obj->scroll_y = y; (void)anim; }
void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, int anim)
{ touch(); if (obj) obj->scroll_y -= y; (void)x; (void)anim; }
void lv_label_set_long_mode(lv_obj_t *obj, int mode) { (void)obj; (void)mode; }

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    touch();
    if (!obj) return;
    snprintf(obj->text, sizeof(obj->text), "%s", text ? text : "");
}

void lv_label_set_text_static(lv_obj_t *obj, const char *text)
{
    lv_label_set_text(obj, text);
}

lv_color_t lv_color_hex(uint32_t color) { return color; }
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, int format)
{ (void)obj; (void)buf; (void)w; (void)h; (void)format; }
void lv_image_set_align(lv_obj_t *obj, int align) { (void)obj; (void)align; }
void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; }
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                           uint32_t letter, uint32_t letter_next)
{
    (void)font; (void)dsc; (void)letter; (void)letter_next;
    return true;
}
void lv_obj_del(lv_obj_t *obj)
{
    touch();
    delete_tree(obj);
}
void lv_obj_update_layout(lv_obj_t *obj) { (void)obj; }
void lv_obj_move_foreground(lv_obj_t *obj) { (void)obj; }
