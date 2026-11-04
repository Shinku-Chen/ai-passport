// tests/lvgl_stubs/lvgl_stub.c —— 最小 LVGL 对象模型实现。
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJS 512

static lv_obj_t g_objs[MAX_OBJS];
static int g_obj_count;
static lv_obj_t *g_free_list[MAX_OBJS];
static int g_free_count;
static lv_obj_t *g_screen;

static lv_obj_t *alloc_obj(lv_obj_t *parent, int kind)
{
    lv_obj_t *o = NULL;
    if (g_free_count > 0) {
        o = g_free_list[--g_free_count];      // 回收 lv_obj_clean 释放的槽位
    } else if (g_obj_count < MAX_OBJS) {
        o = &g_objs[g_obj_count++];
    }
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->parent = parent;
    o->bg_opa = LV_OPA_COVER;
    if (parent) {
        if (!parent->first_child) parent->first_child = o;
        if (parent->last_child) parent->last_child->next_sibling = o;
        parent->last_child = o;
    }
    return o;
}

lv_obj_t *lv_screen_active(void)
{
    if (!g_screen) {
        g_screen = alloc_obj(NULL, 0);
        if (g_screen) {
            g_screen->w = 320;
            g_screen->h = 240;
        }
    }
    return g_screen;
}

void lv_stub_reset(void)
{
    for (int i = 0; i < g_obj_count; ++i) free(g_objs[i].text);
    memset(g_objs, 0, sizeof(g_objs));
    g_obj_count = 0;
    g_free_count = 0;
    g_screen = NULL;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    return alloc_obj(parent, 0);
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    return alloc_obj(parent, 1);
}

lv_obj_t *lv_canvas_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    return alloc_obj(parent, 2);
}

void lv_obj_remove_style_all(lv_obj_t *obj) { (void)obj; }

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h)
{
    if (!obj) return;
    obj->w = w;
    obj->h = h;
}

void lv_obj_set_width(lv_obj_t *obj, int32_t w)
{
    if (obj) obj->w = w;
}

void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y)
{
    if (!obj) return;
    obj->x = x;
    obj->y = y;
}

void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    if (obj) obj->flags |= flag;
}

void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    if (obj) obj->flags &= ~flag;
}

bool lv_obj_has_flag(const lv_obj_t *obj, lv_obj_flag_t flag)
{
    return obj && (obj->flags & flag) != 0;
}

bool lv_stub_visible(const lv_obj_t *obj)
{
    for (const lv_obj_t *o = obj; o; o = o->parent) {
        if (o->flags & LV_OBJ_FLAG_HIDDEN) return false;
    }
    return obj != NULL;
}

int lv_stub_child_count(const lv_obj_t *obj)
{
    int n = 0;
    if (!obj) return 0;
    for (const lv_obj_t *c = obj->first_child; c; c = c->next_sibling) ++n;
    return n;
}

lv_obj_t *lv_stub_child(const lv_obj_t *obj, int index)
{
    if (!obj || index < 0) return NULL;
    lv_obj_t *c = obj->first_child;
    while (c && index-- > 0) c = c->next_sibling;
    return c;
}

// 递归释放子树(否则嵌套的 label 会泄漏对象槽位,长时间测试会耗尽 512 个槽)。
static void free_subtree(lv_obj_t *node)
{
    lv_obj_t *c = node->first_child;
    while (c) {
        lv_obj_t *next = c->next_sibling;
        free_subtree(c);
        free(c->text);
        memset(c, 0, sizeof(*c));
        if (g_free_count < MAX_OBJS) g_free_list[g_free_count++] = c;
        c = next;
    }
}

void lv_obj_clean(lv_obj_t *obj)
{
    if (!obj) return;
    free_subtree(obj);
    obj->first_child = obj->last_child = NULL;
    obj->scroll_y = 0;
}

void lv_obj_move_foreground(lv_obj_t *obj)
{
    if (!obj || !obj->parent) return;
    lv_obj_t *parent = obj->parent;
    // 从链表中摘下
    lv_obj_t *prev = NULL;
    for (lv_obj_t *c = parent->first_child; c; prev = c, c = c->next_sibling) {
        if (c != obj) continue;
        if (prev) prev->next_sibling = c->next_sibling;
        else parent->first_child = c->next_sibling;
        if (parent->last_child == c) parent->last_child = prev;
        break;
    }
    obj->next_sibling = NULL;
    if (!parent->first_child) parent->first_child = obj;
    if (parent->last_child) parent->last_child->next_sibling = obj;
    parent->last_child = obj;
}

void lv_obj_invalidate(lv_obj_t *obj) { (void)obj; }

void lv_obj_update_layout(lv_obj_t *obj)
{
    // 简化:把内容高度当作最后子对象的底边,供滚动上限使用。
    if (!obj) return;
    int32_t bottom = 0;
    int32_t y = 0;
    for (lv_obj_t *c = obj->first_child; c; c = c->next_sibling) {
        const int32_t b = c->y + c->h;
        if (b > bottom) bottom = b;
        y = b;
    }
    (void)y;
    obj->scroll_max = bottom > obj->h ? bottom - obj->h : 0;
}

void lv_obj_set_scroll_dir(lv_obj_t *obj, int dir) { (void)obj; (void)dir; }

void lv_obj_scroll_to_y(lv_obj_t *obj, int32_t y, int anim)
{
    (void)anim;
    if (!obj) return;
    if (y > obj->scroll_max) y = obj->scroll_max;
    if (y < 0) y = 0;
    obj->scroll_y = y;
}

int32_t lv_obj_get_scroll_y(const lv_obj_t *obj)
{
    return obj ? obj->scroll_y : 0;
}

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, lv_part_t part)
{
    (void)part;
    if (obj) obj->bg_color = color.v;
}

void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, lv_part_t part)
{
    (void)part;
    if (obj) obj->bg_opa = opa;
}

void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t w, lv_part_t part)
{
    (void)obj; (void)w; (void)part;
}
void lv_obj_set_style_border_side(lv_obj_t *obj, int side, lv_part_t part)
{
    (void)obj; (void)side; (void)part;
}
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, lv_part_t part)
{
    (void)obj; (void)color; (void)part;
}
void lv_obj_set_style_radius(lv_obj_t *obj, int32_t r, lv_part_t part)
{
    (void)obj; (void)r; (void)part;
}
void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t p, lv_part_t part)
{
    (void)obj; (void)p; (void)part;
}
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, lv_part_t part)
{
    (void)obj; (void)color; (void)part;
}
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, lv_part_t part)
{
    (void)obj; (void)font; (void)part;
}
void lv_obj_set_style_text_align(lv_obj_t *obj, int align, lv_part_t part)
{
    (void)obj; (void)align; (void)part;
}
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int32_t space, lv_part_t part)
{
    (void)obj; (void)space; (void)part;
}

lv_color_t lv_color_hex(uint32_t rgb)
{
    lv_color_t c = { rgb };
    return c;
}

static void set_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    free(obj->text);
    obj->text = strdup(text ? text : "");
}

void lv_label_set_text(lv_obj_t *obj, const char *text) { set_text(obj, text); }

void lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_text(obj, buf);
}

void lv_label_set_long_mode(lv_obj_t *obj, int mode) { (void)obj; (void)mode; }

void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int32_t w, int32_t h, lv_color_format_t cf)
{
    (void)cf;
    if (!obj) return;
    obj->buffer = buf;
    obj->buf_w = w;
    obj->buf_h = h;
}

void lv_image_set_align(lv_obj_t *obj, lv_image_align_t align) { (void)obj; (void)align; }
void lv_image_set_scale(lv_obj_t *obj, uint32_t scale) { (void)obj; (void)scale; }

// ---- 定时器 ----
struct lv_timer_t {
    void (*cb)(lv_timer_t *);
    uint32_t period;
    void *user;
    bool paused;
};

static lv_timer_t g_timers[16];
static int g_timer_count;

lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t period, void *user)
{
    if (g_timer_count >= 16) return NULL;
    lv_timer_t *t = &g_timers[g_timer_count++];
    t->cb = cb;
    t->period = period;
    t->user = user;
    t->paused = false;
    return t;
}

void lv_timer_pause(lv_timer_t *timer) { if (timer) timer->paused = true; }
void lv_timer_resume(lv_timer_t *timer) { if (timer) timer->paused = false; }
void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { if (timer) timer->period = period; }
void lv_timer_reset(lv_timer_t *timer) { (void)timer; }
void *lv_timer_get_user_data(const lv_timer_t *timer) { return timer ? timer->user : NULL; }
void lv_timer_delete(lv_timer_t *timer) { (void)timer; }

void lv_image_set_pivot(lv_obj_t *obj, int x, int y)
{
    (void)obj; (void)x; (void)y;
}

void lv_obj_set_height(lv_obj_t *obj, int h) { if (obj) obj->h = h; }
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int pad, int part) { (void)obj; (void)pad; (void)part; }
int32_t lv_obj_get_child_cnt(const lv_obj_t *obj) { return lv_stub_child_count(obj); }
