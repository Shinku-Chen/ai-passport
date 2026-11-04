// tests/host/stub_lvgl.c —— LVGL 极薄实现：只维护对象树/文本/坐标，供 UI 回归断言。
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OBJ 256
static lv_obj_t s_objs[MAX_OBJ];
static int s_used[MAX_OBJ];
static int s_next_id;
static lv_obj_t s_screen;

const lv_font_t *lv_stub_font_ptr;

static lv_obj_t *alloc(void)
{
    for (int i = 0; i < MAX_OBJ; i++) {
        if (!s_used[i]) {
            s_used[i] = 1;
            memset(&s_objs[i], 0, sizeof(s_objs[i]));
            s_objs[i].id = ++s_next_id;
            return &s_objs[i];
        }
    }
    return NULL;
}

void lv_stub_reset(void)
{
    for (int i = 0; i < MAX_OBJ; i++) {
        if (s_used[i] && &s_objs[i] != &s_screen) {
            s_used[i] = 0;
        }
    }
    memset(&s_screen, 0, sizeof(s_screen));
    s_used[0] = 1;
    s_screen.id = 0;
    s_screen.w = 320;
    s_screen.h = 240;
    s_next_id = 0;
}

void lv_stub_free_all(void) { lv_stub_reset(); }

int lv_stub_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_OBJ; i++) {
        n += s_used[i] ? 1 : 0;
    }
    return n;
}

const lv_obj_t *lv_stub_at(int i)
{
    int k = 0;
    for (int j = 0; j < MAX_OBJ; j++) {
        if (!s_used[j]) {
            continue;
        }
        if (k++ == i) {
            return &s_objs[j];
        }
    }
    return NULL;
}

const lv_obj_t *lv_stub_find_text(const char *text)
{
    for (int j = 0; j < MAX_OBJ; j++) {
        if (s_used[j] && strcmp(s_objs[j].text, text) == 0) {
            return &s_objs[j];
        }
    }
    return NULL;
}

int lv_stub_rows_under(lv_obj_t *parent)
{
    int n = 0;
    for (int j = 0; j < MAX_OBJ; j++) {
        if (s_used[j] && s_objs[j].parent == parent) {
            n++;
        }
    }
    return n;
}

static void unlink_from_parent(lv_obj_t *o)
{
    if (!o->parent) {
        return;
    }
    lv_obj_t *c = o->parent->first_child;
    lv_obj_t *prev = NULL;
    while (c) {
        if (c == o) {
            if (prev) {
                prev->next_sibling = c->next_sibling;
            } else {
                o->parent->first_child = c->next_sibling;
            }
            return;
        }
        prev = c;
        c = c->next_sibling;
    }
}

static void free_tree(lv_obj_t *o)
{
    for (int i = 0; i < MAX_OBJ; i++) {
        if (s_used[i] && s_objs[i].parent == o) {
            free_tree(&s_objs[i]);
        }
    }
    unlink_from_parent(o);
    o->parent = NULL;
    int idx = (int)(o - s_objs);
    if (idx >= 0 && idx < MAX_OBJ && o != &s_screen) {
        s_used[idx] = 0;
    }
}

lv_obj_t *lv_screen_active(void) { return &s_screen; }

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    lv_obj_t *o = alloc();
    if (!o) {
        return NULL;
    }
    o->parent = parent ? parent : &s_screen;
    o->next_sibling = o->parent->first_child;
    o->parent->first_child = o;
    return o;
}

lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
lv_obj_t *lv_image_create(lv_obj_t *parent) { return lv_obj_create(parent); }

void lv_obj_del(lv_obj_t *obj)
{
    if (obj && obj != &s_screen) {
        free_tree(obj);
    }
}

void lv_obj_set_size(lv_obj_t *o, int w, int h)
{
    o->w = w;
    o->h = h;
}
void lv_obj_set_pos(lv_obj_t *o, int x, int y)
{
    o->x = x;
    o->y = y;
}
void lv_obj_set_width(lv_obj_t *o, int w) { o->w = w; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int part)
{
    (void)part;
    o->bg = c.v;
}
void lv_obj_set_style_bg_opa(lv_obj_t *o, lv_opa_t opa, int part)
{
    (void)part;
    o->bg_opa = opa;
}
void lv_obj_set_style_border_width(lv_obj_t *o, int w, int part) { (void)o; (void)w; (void)part; }
void lv_obj_set_style_border_color(lv_obj_t *o, lv_color_t c, int part) { (void)o; (void)c; (void)part; }
void lv_obj_set_style_radius(lv_obj_t *o, int r, int part) { (void)o; (void)r; (void)part; }
void lv_obj_set_style_pad_all(lv_obj_t *o, int p, int part) { (void)o; (void)p; (void)part; }
void lv_obj_set_scrollbar_mode(lv_obj_t *o, lv_scrollbar_mode_t m) { (void)o; (void)m; }
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int part) { (void)o; (void)f; (void)part; }
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int part)
{
    (void)part;
    o->fg = c.v;
}
void lv_obj_set_style_text_align(lv_obj_t *o, lv_text_align_t a, int part)
{
    (void)part;
    o->align = (int)a;
}
void lv_obj_set_style_text_line_space(lv_obj_t *o, int px, int part) { (void)o; (void)px; (void)part; }

void lv_label_set_text(lv_obj_t *o, const char *text)
{
    snprintf(o->text, sizeof(o->text), "%s", text ? text : "");
}
void lv_label_set_long_mode(lv_obj_t *o, lv_label_long_mode_t m) { (void)o; (void)m; }

lv_obj_t *lv_obj_get_child(lv_obj_t *o, int idx)
{
    lv_obj_t *c = o->first_child;
    while (c && --idx >= 0) {
        c = c->next_sibling;
    }
    return idx < 0 ? c : NULL;
}

void lv_image_set_src(lv_obj_t *o, const void *src) { (void)o; (void)src; }
void lv_obj_invalidate(lv_obj_t *o) { (void)o; }
lv_color_t lv_color_hex(uint32_t hex)
{
    lv_color_t c;
    c.v = hex;
    return c;
}

void lv_obj_set_style_pad_hor(lv_obj_t *o, int pad, int part)
{
    (void)o; (void)pad; (void)part;
}

void lv_image_set_scale(lv_obj_t *o, uint32_t scale)
{
    (void)o; (void)scale;
}

void lv_image_set_inner_align(lv_obj_t *o, int align)
{
    (void)o; (void)align;
}

void lv_obj_move_to_index(lv_obj_t *o, int idx)
{
    (void)o; (void)idx;
}

int32_t lv_obj_get_x(const lv_obj_t *o) { return o ? o->x : 0; }
int32_t lv_obj_get_y(const lv_obj_t *o) { return o ? o->y : 0; }
