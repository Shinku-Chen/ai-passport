// tests/app_stubs/lvgl_stub.c —— lvgl.h 的实现:最小对象图 + 调用记录。
//
// 关键职责不是"画得像",而是让测试能问出真实 UI 的状态:
//   * 对象可见性(HIDDEN 标志按祖先链传递);
//   * 标签文本(用于断言正文/名牌/提示);
//   * UI 写操作是否发生在 bsp_lvgl_lock 之外(eden_stub_ui_violations)。
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJECTS 1024

static lv_obj_stub_t s_objects[MAX_OBJECTS];
static int s_object_count;
static lv_obj_t *s_screen;
static int s_lock_depth;
static int s_ui_violations;
static int s_page_containers[16];
static int s_page_container_count;

// UI 写操作统一走这里:未持锁时计数,便于断言"所有 UI 变更都在锁内"。
static void note_ui_write(void)
{
    if (s_lock_depth == 0) {
        s_ui_violations++;
    }
}

static lv_obj_t *new_object(lv_obj_t *parent, int kind)
{
    if (s_object_count >= MAX_OBJECTS) {
        return NULL;
    }
    lv_obj_stub_t *obj = &s_objects[s_object_count++];
    memset(obj, 0, sizeof(*obj));
    obj->kind = kind;
    obj->parent = parent;
    if (parent != NULL && parent->child_count < 64) {
        parent->children[parent->child_count++] = obj;
    }
    return obj;
}

void eden_stub_reset(void)
{
    s_object_count = 0;
    s_screen = NULL;
    s_lock_depth = 0;
    s_ui_violations = 0;
    s_page_container_count = 0;
}

void eden_stub_set_lock_depth(int depth)
{
    s_lock_depth = depth;
}

int eden_stub_lock_depth(void)
{
    return s_lock_depth;
}

int eden_stub_ui_violations(void)
{
    return s_ui_violations;
}

lv_obj_stub_t *eden_stub_object_count(int *count)
{
    *count = s_object_count;
    return s_objects;
}

const char *eden_stub_text_of(lv_obj_stub_t *obj)
{
    return obj != NULL ? obj->text : "";
}

bool eden_stub_visible(const lv_obj_stub_t *obj)
{
    for (const lv_obj_stub_t *cursor = obj; cursor != NULL; cursor = cursor->parent) {
        if ((cursor->flags & LV_OBJ_FLAG_HIDDEN) != 0) {
            return false;
        }
    }
    return true;
}

bool eden_stub_visible_container(int index)
{
    if (index < 0 || index >= s_page_container_count) {
        return false;
    }
    return eden_stub_visible(&s_objects[s_page_containers[index]]);
}

// 页面容器:eden_ui_create 里按枚举顺序为每个页面建一个直接挂在 bg 上的容器。
// 这里用"父对象的父对象是 screen"来识别,不依赖调用顺序猜测。
void lv_stub_mark_page_container(lv_obj_t *obj);

lv_obj_t *lv_screen_active(void)
{
    if (s_screen == NULL) {
        s_screen = new_object(NULL, 4);
    }
    return s_screen;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    note_ui_write();
    return new_object(parent != NULL ? parent : lv_screen_active(), 0);
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    note_ui_write();
    return new_object(parent != NULL ? parent : lv_screen_active(), 1);
}

lv_obj_t *lv_image_create(lv_obj_t *parent)
{
    note_ui_write();
    return new_object(parent != NULL ? parent : lv_screen_active(), 2);
}

lv_obj_t *lv_canvas_create(lv_obj_t *parent)
{
    note_ui_write();
    return new_object(parent != NULL ? parent : lv_screen_active(), 3);
}

lv_obj_t *lv_obj_get_child(const lv_obj_t *obj, int32_t index)
{
    if (obj == NULL || index < 0 || index >= obj->child_count) {
        return NULL;
    }
    return (lv_obj_t *)obj->children[index];
}

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    obj->w = w;
    obj->h = h;
}

void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    obj->x = x;
    obj->y = y;
}

void lv_obj_set_height(lv_obj_t *obj, int32_t h)
{
    if (obj) obj->h = h;
}

void lv_obj_set_width(lv_obj_t *obj, int32_t w)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    obj->w = w;
}

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    obj->flags |= flag;
}

void lv_obj_set_style_pad_hor(lv_obj_t *obj, int32_t pad, int part)
{
    (void)obj;
    (void)pad;
    (void)part;
}

void lv_obj_clear_flag(lv_obj_t *obj, uint32_t flag) { lv_obj_remove_flag(obj, flag); }

void lv_refr_now(void *disp)
{
    (void)disp;
    note_ui_write();
}

void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    obj->flags &= ~flag;
}

void lv_obj_set_parent(lv_obj_t *obj, lv_obj_t *parent)
{
    if (obj == NULL || parent == NULL) {
        return;
    }
    note_ui_write();
    if (obj->parent != NULL) {
        for (int index = 0; index < obj->parent->child_count; index++) {
            if (obj->parent->children[index] == obj) {
                for (int move = index; move + 1 < obj->parent->child_count; move++) {
                    obj->parent->children[move] = obj->parent->children[move + 1];
                }
                obj->parent->child_count--;
                break;
            }
        }
    }
    obj->parent = parent;
    if (parent->child_count < 64) {
        parent->children[parent->child_count++] = obj;
    }
}

void lv_obj_set_user_data(lv_obj_t *obj, void *data)
{
    if (obj != NULL) {
        obj->user_data = data;
    }
}

void *lv_obj_get_user_data(lv_obj_t *obj)
{
    return obj != NULL ? obj->user_data : NULL;
}

void lv_obj_invalidate(const lv_obj_t *obj)
{
    (void)obj;
}

void lv_obj_scroll_by(lv_obj_t *obj, int32_t x, int32_t y, int32_t anim)
{
    (void)obj;
    (void)x;
    (void)y;
    (void)anim;
}

void lv_obj_scroll_to_view(lv_obj_t *obj, int32_t anim)
{
    (void)obj;
    (void)anim;
}

void lv_obj_set_scroll_dir(lv_obj_t *obj, int32_t dir)
{
    (void)obj;
    (void)dir;
}

void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int32_t mode)
{
    (void)obj;
    (void)mode;
}

void lv_canvas_set_buffer(lv_obj_t *obj, void *buffer, int32_t w, int32_t h, int32_t cf)
{
    (void)buffer;
    (void)cf;
    if (obj != NULL) {
        obj->w = w;
        obj->h = h;
    }
}

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (obj == NULL) {
        return;
    }
    note_ui_write();
    snprintf(obj->text, sizeof(obj->text), "%s", text != NULL ? text : "");
}

void lv_label_set_long_mode(lv_obj_t *obj, int32_t mode)
{
    if (obj != NULL) {
        obj->long_mode = mode;
    }
}

void lv_image_set_src(lv_obj_t *obj, const void *src)
{
    if (obj != NULL) {
        obj->image_src = src;
    }
}

#define STYLE_NOOP(name)                                                     \
    void name(lv_obj_t *obj, lv_color_t color, int32_t part)                 \
    {                                                                        \
        (void)obj;                                                           \
        (void)color;                                                         \
        (void)part;                                                          \
    }
#define STYLE_NOOP_INT(name)                                                 \
    void name(lv_obj_t *obj, int32_t value, int32_t part)                    \
    {                                                                        \
        (void)obj;                                                           \
        (void)value;                                                         \
        (void)part;                                                          \
    }

STYLE_NOOP(lv_obj_set_style_bg_color)
STYLE_NOOP(lv_obj_set_style_bg_grad_color)
STYLE_NOOP(lv_obj_set_style_border_color)
STYLE_NOOP(lv_obj_set_style_text_color)
STYLE_NOOP_INT(lv_obj_set_style_bg_grad_dir)
STYLE_NOOP_INT(lv_obj_set_style_border_side)
STYLE_NOOP_INT(lv_obj_set_style_border_width)
STYLE_NOOP_INT(lv_obj_set_style_pad_all)
STYLE_NOOP_INT(lv_obj_set_style_radius)
STYLE_NOOP_INT(lv_obj_set_style_text_align)
STYLE_NOOP_INT(lv_obj_set_style_text_line_space)

void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int32_t part)
{
    (void)obj;
    (void)opa;
    (void)part;
}

void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int32_t part)
{
    (void)obj;
    (void)font;
    (void)part;
}

// ---------------------------------------------------------------- 定时器
typedef struct _lv_timer_t {
    lv_timer_cb_t callback;
    uint32_t period;
    void *user_data;
    bool active;
} lv_timer_stub_t;

static lv_timer_stub_t s_timers[8];

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *user_data)
{
    for (int index = 0; index < 8; index++) {
        if (!s_timers[index].active) {
            s_timers[index] = (lv_timer_stub_t){ callback, period, user_data, true };
            return &s_timers[index];
        }
    }
    return NULL;
}

void lv_timer_delete(lv_timer_t *timer)
{
    if (timer != NULL) {
        timer->active = false;
    }
}

void *lv_timer_get_user_data(lv_timer_t *timer)
{
    return timer != NULL ? timer->user_data : NULL;
}

// 测试显式推进打字机:调用所有活动定时器的回调一次。
// LVGL 定时器回调运行在 LVGL 任务里(本来就不需要 bsp_lvgl_lock),这里把锁深度 +1
// 模拟这个上下文,否则会被 eden_stub_ui_violations 误记成"锁外改 UI"。
void eden_stub_timer_tick(void)
{
    s_lock_depth++;
    for (int index = 0; index < 8; index++) {
        if (s_timers[index].active && s_timers[index].callback != NULL) {
            s_timers[index].callback(&s_timers[index]);
        }
    }
    s_lock_depth--;
}

void *lv_display_get_default(void)
{
    static int s_disp;
    return &s_disp;
}

int32_t lv_obj_get_width(const lv_obj_t *obj) { return obj ? obj->w : 0; }
int32_t lv_obj_get_height(const lv_obj_t *obj) { return obj ? obj->h : 0; }
int lv_label_get_long_mode(const lv_obj_t *obj) { return obj ? obj->long_mode : 0; }

void lv_image_set_scale(lv_obj_t *obj, uint32_t scale)
{
    (void)obj;
    (void)scale;
}

void lv_image_set_inner_align(lv_obj_t *obj, int align)
{
    (void)obj;
    (void)align;
}

void lv_obj_move_to_index(lv_obj_t *obj, int32_t index)
{
    (void)obj;
    (void)index;
}
