#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define MAX_OBJ 512
static lv_obj_t s_objs[MAX_OBJ];
static int s_count;
static int s_violations;

extern int stub_lock_depth;
int stub_lock_violations(void) { return s_violations; }
void stub_reset_violations(void) { s_violations = 0; }
int stub_object_count(void) { return s_count; }

static void note_call(void) { if (stub_lock_depth <= 0) s_violations++; }

static lv_obj_t *alloc(lv_obj_t *parent)
{
    note_call();
    if (s_count >= MAX_OBJ) return NULL;
    lv_obj_t *o = &s_objs[s_count];
    memset(o, 0, sizeof(*o));
    o->id = s_count++;
    o->parent = parent;
    o->bg_opa = 255;
    return o;
}

lv_obj_t *lv_screen_active(void)
{
    if (s_count == 0) (void)alloc(NULL);
    return &s_objs[0];
}
lv_obj_t *lv_obj_create(lv_obj_t *parent) { return alloc(parent); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return alloc(parent); }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { lv_obj_t *o = alloc(parent); if (o) o->canvas = true; return o; }
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int w, int h, lv_color_format_t format)
{ (void)buf; (void)format; note_call(); if (obj) { obj->w = w; obj->h = h; } }
void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align) { note_call(); if (obj) obj->align = align; }
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag) { note_call(); if (obj && flag == LV_OBJ_FLAG_HIDDEN) obj->hidden = true; if (obj && flag == LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = true; }
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag) { note_call(); if (obj && flag == LV_OBJ_FLAG_HIDDEN) obj->hidden = false; if (obj && flag == LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = false; }
bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t flag) { if (!obj) return false; if (flag == LV_OBJ_FLAG_HIDDEN) return obj->hidden; return false; }
void lv_obj_set_pos(lv_obj_t *obj, int x, int y) { note_call(); if (obj) { obj->x = x; obj->y = y; } }
void lv_obj_set_width(lv_obj_t *obj, int w)
{
    if (obj) obj->w = w;
}

void lv_obj_set_height(lv_obj_t *obj, int h)
{
    if (obj) obj->h = h;
}

void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int selector)
{
    (void)obj; (void)pad; (void)selector;
}

void lv_obj_set_size(lv_obj_t *obj, int w, int h) { note_call(); if (obj) { obj->w = w; obj->h = h; } }
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int selector) { (void)selector; note_call(); if (obj) obj->bg_color = color; }
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int selector) { (void)selector; note_call(); if (obj) obj->bg_opa = opa; }
void lv_obj_set_style_border_width(lv_obj_t *obj, int width, int selector) { (void)obj; (void)width; (void)selector; note_call(); }
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, int selector) { (void)obj; (void)color; (void)selector; note_call(); }
void lv_obj_set_style_radius(lv_obj_t *obj, int radius, int selector) { (void)obj; (void)radius; (void)selector; note_call(); }
void lv_obj_set_style_pad_all(lv_obj_t *obj, int pad, int selector) { (void)obj; (void)pad; (void)selector; note_call(); }
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int selector) { (void)selector; note_call(); if (obj) obj->text_color = color; }
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector) { (void)selector; note_call(); if (obj) obj->font = font; }
void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t align, int selector) { (void)selector; note_call(); if (obj) obj->align = align; }
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int space, int selector) { (void)selector; note_call(); if (obj) obj->line_space = space; }
void lv_obj_set_scroll_dir(lv_obj_t *obj, lv_dir_t dir) { (void)obj; (void)dir; note_call(); }
void lv_obj_scroll_by(lv_obj_t *obj, int x, int y, lv_anim_enable_t anim) { (void)obj; (void)x; (void)y; (void)anim; note_call(); }
void lv_obj_move_foreground(lv_obj_t *obj) { (void)obj; note_call(); }
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode) { (void)obj; (void)mode; note_call(); }
void lv_label_set_text(lv_obj_t *obj, const char *text) { note_call(); if (obj) snprintf(obj->text, sizeof(obj->text), "%s", text ? text : ""); }
const char *lv_label_get_text(const lv_obj_t *obj) { return obj ? obj->text : ""; }
lv_color_t lv_color_hex(uint32_t color) { return color; }
void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; note_call(); }

lv_obj_t *stub_root_object(void) { return &s_objs[0]; }
const char *stub_text(const lv_obj_t *obj) { return obj ? obj->text : ""; }
bool stub_visible(const lv_obj_t *obj) { return obj && !obj->hidden; }
int stub_x(const lv_obj_t *obj) { return obj ? obj->x : 0; }
int stub_y(const lv_obj_t *obj) { return obj ? obj->y : 0; }
int stub_w(const lv_obj_t *obj) { return obj ? obj->w : 0; }
int stub_h(const lv_obj_t *obj) { return obj ? obj->h : 0; }
