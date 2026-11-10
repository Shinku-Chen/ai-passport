#include "voice_marquee.h"

lv_obj_t *voice_marquee_create(lv_obj_t *parent, int x, int y, int width,
                               const char *text, const lv_font_t *font,
                               lv_color_t color, bool selected)
{
    // LVGL labels invalidate an extra font-height/4 margin. Parent clipping
    // confines native marquee invalidation to this line, including at wrap.
    lv_obj_t *viewport = lv_obj_create(parent);
    lv_obj_remove_style_all(viewport);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(viewport, x, y);
    lv_obj_set_size(viewport, width, lv_font_get_line_height(font));

    lv_obj_t *label = lv_label_create(viewport);
    lv_label_set_text_static(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_pos(label, 0, 0);
    lv_obj_set_size(label, width, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    // Pixels/second, not a fixed total duration: long names must not race by.
    lv_obj_set_style_anim_duration(label, lv_anim_speed(36), 0);
    lv_label_set_long_mode(label, selected ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
    return label;
}
