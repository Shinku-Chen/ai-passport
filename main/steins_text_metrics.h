// main/steins_text_metrics.h —— 字形推进宽度表(由 tools/steins_font.py 生成)。
//
// adv16 是"16px 字号下的推进宽度,单位 1/16 px",与 LVGL 里同一字形的 adv_w
// 同源(同一份栅格化结果),所以排版层算出来的折行位置和 LVGL 实际绘制一致。
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t cp;
    uint16_t adv16;
} steins_adv_t;

extern const steins_adv_t steins_adv_table[];
extern const unsigned steins_adv_count;
