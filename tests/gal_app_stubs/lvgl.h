#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdint.h>
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_font_t { int dummy; } lv_font_t;
typedef struct lv_timer_t lv_timer_t;
#define LV_FONT_DECLARE(name) extern const lv_font_t name

typedef struct { size_t total_size, free_cnt, free_size, free_biggest_size, used_cnt, max_used; uint8_t used_pct, frag_pct; } lv_mem_monitor_t;
void lv_mem_monitor(lv_mem_monitor_t *mon);
