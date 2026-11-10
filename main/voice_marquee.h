#pragma once

#include "lvgl.h"

// Creates a one-line label inside a transparent clipping viewport. Text must
// remain valid for the label's lifetime (the voice index and literals do).
// Call in LVGL context or with the BSP LVGL lock held.
lv_obj_t *voice_marquee_create(lv_obj_t *parent, int x, int y, int width,
                               const char *text, const lv_font_t *font,
                               lv_color_t color, bool selected);
