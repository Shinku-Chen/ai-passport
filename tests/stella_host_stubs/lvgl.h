#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct _lv_obj_t lv_obj_t;
typedef struct lv_font_t { int unused; } lv_font_t;
typedef struct lv_timer_t lv_timer_t;
lv_obj_t *lv_screen_active(void);
#define LV_FONT_DECLARE(name) extern const lv_font_t name
