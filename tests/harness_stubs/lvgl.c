// tests/harness_stubs/lvgl.c —— 最小 LVGL 桩的实现（对象池 + 属性记录）。
#include "lvgl.h"

#include <stdlib.h>

#define POOL_MAX 2048
static lv_obj_t s_pool[POOL_MAX];
static int s_pool_n;
static lv_obj_t s_screen;
static bool s_screen_ready;

static lv_obj_t *alloc(lv_obj_t *parent)
{
    if (s_pool_n >= POOL_MAX) abort();
    lv_obj_t *o = &s_pool[s_pool_n++];
    memset(o, 0, sizeof(*o));
    o->parent = parent;
    o->long_mode = LV_LABEL_LONG_WRAP;
    o->text_align = LV_TEXT_ALIGN_LEFT;
    if (parent && parent->child_count < LV_OBJ_CHILDREN_MAX) {
        parent->child[parent->child_count++] = o;
    }
    return o;
}

lv_obj_t *lv_scr_act(void)
{
    if (!s_screen_ready) {
        memset(&s_screen, 0, sizeof(s_screen));
        s_screen_ready = true;
    }
    return &s_screen;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) { return alloc(parent ? parent : lv_scr_act()); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return alloc(parent ? parent : lv_scr_act()); }
lv_obj_t *lv_image_create(lv_obj_t *parent) { return alloc(parent ? parent : lv_scr_act()); }

void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, uint32_t sel) { (void)o; (void)f; (void)sel; }
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, uint32_t sel) { (void)o; (void)c; (void)sel; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, uint32_t sel) { (void)o; (void)c; (void)sel; }
void lv_obj_set_style_bg_opa(lv_obj_t *o, lv_opa_t opa, uint32_t sel) { (void)o; (void)opa; (void)sel; }
void lv_obj_set_style_border_width(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_radius(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_pad_all(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_pad_left(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_text_align(lv_obj_t *o, lv_text_align_t a, uint32_t sel) { (void)sel; if (o) o->text_align = a; }
void lv_obj_set_style_text_line_space(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_outline_width(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }
void lv_obj_set_style_outline_color(lv_obj_t *o, lv_color_t c, uint32_t sel) { (void)o; (void)c; (void)sel; }
void lv_obj_set_style_outline_pad(lv_obj_t *o, int32_t v, uint32_t sel) { (void)o; (void)v; (void)sel; }

void lv_obj_set_pos(lv_obj_t *o, int32_t x, int32_t y) { if (o) { o->x = x; o->y = y; } }
void lv_obj_set_size(lv_obj_t *o, int32_t w, int32_t h) { if (o) { o->w = w; o->h = h; } }
void lv_obj_set_width(lv_obj_t *o, int32_t w) { if (o) o->w = w; }
void lv_obj_set_height(lv_obj_t *o, int32_t h) { if (o) o->h = h; }

void lv_label_set_text(lv_obj_t *o, const char *text)
{
    if (!o) return;
    if (!text) text = "";
    // 记录完整文本：桩里不做真实换行，但断言需要能看到页脚/正文内容
    const size_t n = strlen(text);
    const size_t copy = n < sizeof(o->text) - 1 ? n : sizeof(o->text) - 1;
    memcpy(o->text, text, copy);
    o->text[copy] = '\0';
}

void lv_label_set_long_mode(lv_obj_t *o, lv_label_long_mode_t m) { if (o) o->long_mode = m; }
void lv_image_set_src(lv_obj_t *o, const void *src) { if (o) o->src = src; }
void lv_obj_add_flag(lv_obj_t *o, uint32_t flag) { if (o) o->flags |= flag; }
void lv_obj_remove_flag(lv_obj_t *o, uint32_t flag) { if (o) o->flags &= ~flag; }
void lv_obj_clear_flag(lv_obj_t *o, uint32_t flag) { if (o) o->flags &= ~flag; }
static int s_flush_count;

void lv_obj_invalidate(lv_obj_t *o)
{
    if (o) o->invalidated++;
}

// 分带绘制要求“每带画完立刻 flush”：这里只记次数，便于断言确实发生了。
void lv_refr_now(void *disp)
{
    (void)disp;
    s_flush_count++;
}

int lv_stub_flush_count(void) { return s_flush_count; }

// 宿主桩：分带刷新后的延时空实现。
void vTaskDelay(uint32_t ticks)
{
    (void)ticks;
}

// 宿主桩：默认显示句柄（分带刷新只用来取句柄，桩里给个非空值）。
void *lv_display_get_default(void)
{
    static int s_disp;
    return &s_disp;
}

// 宿主桩：单调 tick（足够用来断言“长按后 400ms 内的补发 CLICK 被吞”）。
static uint32_t s_tick;
uint32_t lv_tick_get(void) { return s_tick += 50; }
