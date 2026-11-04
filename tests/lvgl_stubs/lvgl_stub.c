#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJS 512
static lv_obj_t objs[MAX_OBJS];
static int obj_count;
static lv_obj_t root;
static bool require_lock;
static int lock_depth;
static int violations;
static int invalidations;

static void touch(void)
{
    if (require_lock && lock_depth == 0) violations++;
}

void lv_stub_reset(void)
{
    memset(objs, 0, sizeof(objs));
    memset(&root, 0, sizeof(root));
    obj_count = 0;
    require_lock = false;
    lock_depth = 0;
    violations = 0;
    invalidations = 0;
    root.id = -1;
    root.kind = 0;
    root.w = 320;
    root.h = 240;
}

void lv_stub_require_lock(bool required) { require_lock = required; }
int lv_stub_lock_violations(void) { return violations; }
int lv_stub_obj_count(void) { return obj_count; }
lv_obj_t *lv_stub_obj(int index) { return (index >= 0 && index < obj_count) ? &objs[index] : NULL; }
void lv_stub_set_locked(bool locked) { lock_depth = locked ? 1 : 0; }
bool lv_stub_locked(void) { return lock_depth > 0; }
void lv_stub_clear_invalidations(void) { invalidations = 0; }
int lv_stub_invalidations(void) { return invalidations; }

lv_obj_t *lv_screen_active(void) { return &root; }

static lv_obj_t *alloc(int kind, lv_obj_t *parent)
{
    touch();
    if (obj_count >= MAX_OBJS) return NULL;
    lv_obj_t *o = &objs[obj_count++];
    memset(o, 0, sizeof(*o));
    o->id = obj_count - 1;
    o->kind = kind;
    o->parent = parent;
    o->w = 100;
    o->h = 20;
    o->bg_opa = 0;
    return o;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { return alloc(0, parent); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return alloc(1, parent); }
lv_obj_t *lv_canvas_create(lv_obj_t *parent) { return alloc(2, parent); }

void lv_obj_remove_style_all(lv_obj_t *obj) { touch(); (void)obj; }
void lv_obj_set_pos(lv_obj_t *obj, int x, int y) { touch(); if (obj) { obj->x = x; obj->y = y; } }
void lv_obj_set_size(lv_obj_t *obj, int w, int h) { touch(); if (obj) { obj->w = w; obj->h = h; } }
void lv_obj_set_width(lv_obj_t *obj, int w) { touch(); if (obj) obj->w = w; }
void lv_obj_set_height(lv_obj_t *obj, int h) { touch(); if (obj) obj->h = h; }
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t c, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->bg_color = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->bg_opa = opa;
}
void lv_obj_set_style_border_width(lv_obj_t *obj, int w, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->border_width = w;
}
void lv_obj_set_style_border_side(lv_obj_t *obj, lv_border_side_t side, lv_part_t part)
{
    (void)obj; (void)side; (void)part; touch();
}
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t c, lv_part_t part)
{
    (void)obj; (void)c; (void)part; touch();
}
void lv_obj_set_style_radius(lv_obj_t *obj, int r, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->radius = r;
}
void lv_obj_set_style_pad_left(lv_obj_t *obj, int px, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->pad_left = px;
}
void lv_obj_set_style_pad_right(lv_obj_t *obj, int px, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->pad_right = px;
}
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->font = font;
}
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t c, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->text_color = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int px, lv_part_t part)
{
    (void)obj; (void)px; (void)part; touch();
}
void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t align, lv_part_t part)
{
    (void)part; touch(); if (obj) obj->text_align = align;
}
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    touch();
    if (!obj) return;
    if (flag == LV_OBJ_FLAG_HIDDEN) obj->hidden = 1;
    if (flag == LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = 1;
}
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    touch();
    if (!obj) return;
    if (flag == LV_OBJ_FLAG_HIDDEN) obj->hidden = 0;
    if (flag == LV_OBJ_FLAG_SCROLLABLE) obj->scrollable = 0;
}
void lv_obj_move_foreground(lv_obj_t *obj) { touch(); (void)obj; }
void lv_obj_invalidate(lv_obj_t *obj) { touch(); invalidations++; if (obj) obj->invalidated++; }
void lv_obj_set_user_data(lv_obj_t *obj, void *data) { touch(); if (obj) obj->user_data = data; }
void *lv_obj_get_user_data(lv_obj_t *obj) { return obj ? obj->user_data : NULL; }
void lv_label_set_text(lv_obj_t *label, const char *text)
{
    touch();
    if (!label) return;
    snprintf(label->text, sizeof(label->text), "%s", text ? text : "");
}
void lv_label_set_long_mode(lv_obj_t *label, lv_label_long_mode_t mode)
{
    touch(); if (label) label->long_mode = mode;
}
void *lv_malloc(size_t size) { return malloc(size); }
void lv_free(void *ptr) { free(ptr); }
void lv_canvas_set_buffer(lv_obj_t *canvas, void *buf, int w, int h, lv_color_format_t cf)
{
    (void)cf; touch();
    if (!canvas) return;
    canvas->canvas_buf = (uint16_t *)buf;
    canvas->canvas_w = w;
    canvas->canvas_h = h;
}
void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align) { (void)obj; (void)align; touch(); }
void lv_image_set_scale(lv_obj_t *obj, int scale) { touch(); if (obj) obj->scale = scale; }
void lv_image_set_pivot(lv_obj_t *obj, int x, int y) { touch(); if (obj) { obj->pivot_x = x; obj->pivot_y = y; } }
lv_color_t lv_color_hex(uint32_t hex)
{
    lv_color_t c = { (uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex };
    return c;
}

bool lv_stub_visible(const lv_obj_t *obj)
{
    for (const lv_obj_t *o = obj; o; o = o->parent) {
        if (o->hidden) return false;
    }
    return obj != NULL;
}

const char *lv_stub_text(const lv_obj_t *obj)
{
    return obj ? obj->text : "";
}
