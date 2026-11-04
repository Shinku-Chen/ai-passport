// tests/harness_stubs/lvgl.h —— 给宿主回归用的最小 LVGL 桩。
//
// 目的不是“假装渲染”，而是让**真实的 main/nn_ui.c** 能在宿主上编译运行，
// 从而能断言真实对象属性（可见性、文本、选中行）。这里只保存对象模型里
// 被断言到的字段：hidden 标志、label 文本、src 指针、尺寸。
//
// 所有样式设置函数都是空实现（样式不影响“页面是否可见”这类逻辑）。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_OBJ_FLAG_HIDDEN 0x0001u
#define LV_OBJ_FLAG_SCROLLABLE 0x0002u

#define LV_OPA_TRANSP 0u
#define LV_OPA_40 102u
#define LV_OPA_90 229u
#define LV_OPA_COVER 255u

typedef enum { LV_LABEL_LONG_WRAP = 0, LV_LABEL_LONG_CLIP } lv_label_long_mode_t;
typedef enum { LV_TEXT_ALIGN_LEFT = 0, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT } lv_text_align_t;
typedef enum { LV_COLOR_FORMAT_RGB565 = 0x12 } lv_color_format_t;

#define LV_IMAGE_HEADER_MAGIC 0x19u
#define LV_IMAGE_HEADER_MAGIC_ 0x19u

typedef struct { uint8_t blue, green, red; } lv_color_t;
static inline lv_color_t lv_color_hex(uint32_t hex)
{
    lv_color_t c = { (uint8_t)(hex & 0xFF), (uint8_t)((hex >> 8) & 0xFF), (uint8_t)((hex >> 16) & 0xFF) };
    return c;
}
typedef uint8_t lv_opa_t;

typedef struct lv_font_t { int line_height; } lv_font_t;
#define LV_FONT_DECLARE(name) extern const lv_font_t name

typedef struct {
    uint32_t magic: 8;
    uint32_t cf: 8;
    uint32_t flags: 16;
    uint32_t w: 16;
    uint32_t h: 16;
    uint32_t stride: 16;
    uint32_t reserved_2: 16;
} lv_image_header_t;

typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;

#define LV_OBJ_TEXT_MAX 512
#define LV_OBJ_CHILDREN_MAX 64

typedef struct lv_obj_t {
    uint32_t flags;
    char text[LV_OBJ_TEXT_MAX];
    const void *src;
    int32_t x, y, w, h;
    lv_label_long_mode_t long_mode;
    lv_text_align_t text_align;
    int invalidated;       // lv_obj_invalidate 次数（断言分带刷新区域）
    struct lv_obj_t *parent;
    struct lv_obj_t *child[LV_OBJ_CHILDREN_MAX];
    int child_count;
} lv_obj_t;

// 对象池：桩里不释放，测试进程生命周期内有效。
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
lv_obj_t *lv_scr_act(void);

void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, uint32_t sel);
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, uint32_t sel);
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, uint32_t sel);
void lv_obj_set_style_bg_opa(lv_obj_t *o, lv_opa_t opa, uint32_t sel);
void lv_obj_set_style_border_width(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_radius(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_pad_all(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_pad_left(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_text_align(lv_obj_t *o, lv_text_align_t a, uint32_t sel);
void lv_obj_set_style_text_line_space(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_outline_width(lv_obj_t *o, int32_t v, uint32_t sel);
void lv_obj_set_style_outline_color(lv_obj_t *o, lv_color_t c, uint32_t sel);
void lv_obj_set_style_outline_pad(lv_obj_t *o, int32_t v, uint32_t sel);

void lv_obj_set_pos(lv_obj_t *o, int32_t x, int32_t y);
void lv_obj_set_size(lv_obj_t *o, int32_t w, int32_t h);
void lv_obj_set_width(lv_obj_t *o, int32_t w);
void lv_obj_set_height(lv_obj_t *o, int32_t h);
void lv_label_set_text(lv_obj_t *o, const char *text);
void lv_label_set_long_mode(lv_obj_t *o, lv_label_long_mode_t m);
void lv_image_set_src(lv_obj_t *o, const void *src);
void lv_obj_add_flag(lv_obj_t *o, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *o, uint32_t flag);
void lv_obj_clear_flag(lv_obj_t *o, uint32_t flag);
void lv_obj_invalidate(lv_obj_t *o);
void lv_refr_now(void *disp);
uint32_t lv_tick_get(void);
void *lv_display_get_default(void);
int lv_stub_flush_count(void);

#ifdef __cplusplus
}
#endif
