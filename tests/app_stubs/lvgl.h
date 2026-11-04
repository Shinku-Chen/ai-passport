// tests/app_stubs/lvgl.h —— 宿主机测试用的最小 LVGL 替身。
//
// 只实现 eden_ui.c / eden_app.c / eden_image.c 真正用到的那部分对象模型:
// 对象图、文本、可见性(HIDDEN 标志)、父子关系、定时器。样式类调用都是空操作。
//
// 测试可以通过 eden_stub_* 读回状态来断言"真实 UI 调用发生了什么",
// 而不是复制一份页面逻辑:
//   eden_stub_find_text(label_text)     找文本
//   eden_stub_obj_visible(obj)          该对象当前是否可见(自身与祖先都没有 HIDDEN)
//   eden_stub_page_visible(index)       某个页面容器是否可见
//   eden_stub_lock_depth()              LVGL 锁深度(用于断言"UI 只在锁内改")
//   eden_stub_ui_violations()           在未持锁状态下发生的 UI 写操作次数
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_PART_MAIN 0
#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_SIZE_CONTENT 0x7FFFFFFF
#define LV_OPA_60 153
#define LV_OPA_70 178
#define LV_OBJ_FLAG_HIDDEN (1u << 0)
#define LV_OBJ_FLAG_SCROLLABLE (1u << 1)
#define LV_DIR_VER 2
#define LV_ANIM_OFF 0
#define LV_SCROLLBAR_MODE_OFF 0
#define LV_LABEL_LONG_WRAP 0
#define LV_LABEL_LONG_DOT 1
#define LV_LABEL_LONG_MODE_DOTS 2
#define LV_IMAGE_ALIGN_CENTER 1
#define LV_LABEL_LONG_MODE_CLIP 4
#define LV_TEXT_ALIGN_CENTER 2
#define LV_TEXT_ALIGN_RIGHT 3
#define LV_BORDER_SIDE_LEFT 4
#define LV_GRAD_DIR_VER 1
#define LV_COLOR_FORMAT_RGB565 0x12
#define LV_COLOR_FORMAT_RGB565A8 0x14
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_FONT_DECLARE(name) extern const lv_font_t name;

typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_timer_t lv_timer_t;
typedef uint8_t lv_opa_t;
typedef struct { uint32_t blue : 8, green : 8, red : 8; } lv_color_t;
typedef struct { int dummy; } lv_font_t;

typedef struct {
    uint32_t magic;
    uint32_t cf;
    uint32_t w;
    uint32_t h;
    uint32_t stride;
} lv_image_header_t;

typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const void *data;
} lv_image_dsc_t;

typedef void (*lv_timer_cb_t)(lv_timer_t *timer);
typedef void (*lv_event_cb_t)(void *event);

static inline lv_color_t lv_color_hex(uint32_t value)
{
    lv_color_t color;
    color.red = (uint8_t)(value >> 16);
    color.green = (uint8_t)(value >> 8);
    color.blue = (uint8_t)value;
    return color;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_obj_get_child(const lv_obj_t *obj, int32_t index);

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h);
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y);
void lv_obj_set_width(lv_obj_t *obj, int32_t w);
int32_t lv_obj_get_width(const lv_obj_t *obj);
int32_t lv_obj_get_height(const lv_obj_t *obj);
int lv_label_get_long_mode(const lv_obj_t *obj);
void lv_obj_set_height(lv_obj_t *obj, int32_t h);
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int32_t pad, int part);
void lv_obj_clear_flag(lv_obj_t *obj, uint32_t flag);
void lv_refr_now(void *disp);
void *lv_display_get_default(void);
void lv_obj_set_height(lv_obj_t *obj, int32_t h);
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_set_parent(lv_obj_t *obj, lv_obj_t *parent);
void lv_obj_set_user_data(lv_obj_t *obj, void *data);
void *lv_obj_get_user_data(lv_obj_t *obj);
void lv_obj_invalidate(const lv_obj_t *obj);
void lv_obj_scroll_by(lv_obj_t *obj, int32_t x, int32_t y, int32_t anim);
void lv_obj_scroll_to_view(lv_obj_t *obj, int32_t anim);
void lv_obj_set_scroll_dir(lv_obj_t *obj, int32_t dir);
void lv_obj_set_scrollbar_mode(lv_obj_t *obj, int32_t mode);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buffer, int32_t w, int32_t h, int32_t cf);

void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_label_set_long_mode(lv_obj_t *obj, int32_t mode);
void lv_image_set_src(lv_obj_t *obj, const void *src);
void lv_image_set_scale(lv_obj_t *obj, uint32_t scale);
void lv_image_set_inner_align(lv_obj_t *obj, int align);
void lv_obj_move_to_index(lv_obj_t *obj, int32_t index);

// 样式:全部空操作(测试不关心视觉,只关心对象状态)。
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, int32_t part);
void lv_obj_set_style_bg_grad_color(lv_obj_t *obj, lv_color_t color, int32_t part);
void lv_obj_set_style_bg_grad_dir(lv_obj_t *obj, int32_t dir, int32_t part);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, int32_t part);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, int32_t part);
void lv_obj_set_style_border_side(lv_obj_t *obj, int32_t side, int32_t part);
void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t width, int32_t part);
void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t pad, int32_t part);
void lv_obj_set_style_radius(lv_obj_t *obj, int32_t radius, int32_t part);
void lv_obj_set_style_text_align(lv_obj_t *obj, int32_t align, int32_t part);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, int32_t part);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int32_t part);
void lv_obj_set_style_text_line_space(lv_obj_t *obj, int32_t space, int32_t part);

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *user_data);
void lv_timer_delete(lv_timer_t *timer);
void *lv_timer_get_user_data(lv_timer_t *timer);

// ---------------------------------------------------------------- 测试接口
// 对象结构体就是 lv_obj_t 本身,这样替换层与生产代码共用同一个类型,
// 打开 -Werror 时不会出现指针类型不兼容。
struct _lv_obj_t {
    uint32_t flags;
    char text[512];
    const void *image_src;
    void *user_data;
    struct _lv_obj_t *parent;
    struct _lv_obj_t *children[64];
    int child_count;
    int32_t w;
    int32_t h;
    int32_t x;
    int32_t y;
    int32_t long_mode;   // lv_label_set_long_mode 记录
    int kind;   // 0=obj 1=label 2=image 3=canvas 4=screen
};
typedef struct _lv_obj_t lv_obj_stub_t;

// 所有对象(按创建顺序),便于测试定位页面容器与文本。
lv_obj_stub_t *eden_stub_object_count(int *count);
const char *eden_stub_text_of(lv_obj_stub_t *obj);
bool eden_stub_visible(const lv_obj_stub_t *obj);
void eden_stub_reset(void);
void eden_stub_set_lock_depth(int depth);
int eden_stub_lock_depth(void);
int eden_stub_ui_violations(void);
// 返回第 index 个页面容器的可见性(页面容器是 ui->pages[] 里按枚举顺序创建的)。
bool eden_stub_visible_container(int index);

#ifdef __cplusplus
}
#endif
