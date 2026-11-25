#pragma once
#include <stdint.h>
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_font_t { int dummy; } lv_font_t;
typedef struct lv_timer_t lv_timer_t;
#define LV_OBJ_FLAG_HIDDEN 1u
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_refr_now(void *disp);
lv_obj_t *lv_screen_active(void);
#define LV_FONT_DECLARE(name) extern const lv_font_t name
