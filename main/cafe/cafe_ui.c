// main/cafe/cafe_ui.c —— 320x240 横屏界面实现。
#include "cafe_ui.h"

#include "cafe_image.h"
#include "esp_log.h"
#include "misc/lv_text_private.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

#define COL_BG 0x0B0B10
#define COL_PANEL 0x241A18
#define COL_PANEL_EDGE 0x6B4A3C
#define COL_TEXT 0xF6EFE8
#define COL_DIM 0xA89990
#define COL_ACCENT 0xE7B98F
#define COL_ROW 0x1B1512
#define COL_ROW_SEL 0x5A3B2B
#define COL_ROW_DIS 0x6C625C
#define COL_BOX 0x17110F
#define COL_BOX_EDGE 0xB98A63
#define COL_NAME 0xF1C9A5

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *mk_page(cafe_ui_t *ui, cafe_page_t page, uint32_t bg, bool opaque)
{
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, CAFE_UI_W, CAFE_UI_H);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (opaque) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    set_hidden(obj, true);
    ui->pages[page] = obj;
    return obj;
}

static void mk_page_title(cafe_ui_t *ui, cafe_page_t page, const char *text)
{
    ui->titles[page] = mk_label(ui->pages[page], ui->font_small, COL_ACCENT);
    lv_label_set_text(ui->titles[page], text ? text : "");
    lv_obj_set_pos(ui->titles[page], 10, 7);
}

static bool list_make_row(cafe_ui_t *ui, cafe_list_t id, int i)
{
    cafe_list_ui_t *list = &ui->lists[id];
    if (i < 0 || i >= list->max_rows || list->rows[i]) return true;
    lv_obj_t *row = lv_obj_create(list->page);
    if (!row) return false;
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, list->w - 8, list->row_h - 4);
    lv_obj_set_pos(row, list->x + 4, list->top + i * list->row_h);
    lv_obj_set_style_radius(row, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = mk_label(row, ui->font_small, COL_TEXT);
    const bool wants_value = id != CAFE_LIST_HISTORY && id != CAFE_LIST_RECALL;
    lv_obj_t *value = wants_value ? mk_label(row, ui->font_small, COL_ACCENT) : NULL;
    if (!label || (wants_value && !value)) {
        lv_obj_del(row);
        return false;
    }
    lv_obj_set_width(label, list->w - 100);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
    if (value) lv_obj_align(value, LV_ALIGN_RIGHT_MID, -8, 0);

    list->rows[i] = row;
    list->labels[i] = label;
    list->values[i] = value;
    list->enabled[i] = true;
    set_hidden(row, true);
    return true;
}

static void list_clear(cafe_ui_t *ui, cafe_list_t id)
{
    cafe_list_ui_t *list = &ui->lists[id];
    for (int i = 0; i < CAFE_LIST_MAX; ++i) {
        if (list->rows[i]) {
            lv_obj_del(list->rows[i]);
            list->rows[i] = NULL;
            list->labels[i] = NULL;
            list->values[i] = NULL;
        }
        list->enabled[i] = true;
    }
    list->count = 0;
    list->selected = 0;
}

static void list_build_parent(cafe_ui_t *ui, cafe_list_t id, lv_obj_t *parent,
                              cafe_page_t page_id, int x, int y, int w, int row_h,
                              int max_rows)
{
    cafe_list_ui_t *list = &ui->lists[id];
    memset(list, 0, sizeof(*list));
    list->page = parent;
    list->page_id = page_id;
    list->x = x;
    list->top = y;
    list->w = w;
    list->row_h = row_h;
    list->max_rows = max_rows > CAFE_LIST_MAX ? CAFE_LIST_MAX : max_rows;
}

static void list_build(cafe_ui_t *ui, cafe_list_t id, cafe_page_t page, int x, int y,
                       int w, int row_h, int max_rows)
{
    list_build_parent(ui, id, ui->pages[page], page, x, y, w, row_h, max_rows);
}

static void list_refresh(cafe_ui_t *ui, cafe_list_t id)
{
    cafe_list_ui_t *list = &ui->lists[id];
    for (int i = 0; i < list->max_rows; ++i) {
        const bool show = i < list->count && list->rows[i];
        set_hidden(list->rows[i], !show);
        if (!show) continue;
        const bool sel = i == list->selected;
        lv_obj_set_style_bg_color(list->rows[i], lv_color_hex(sel ? COL_ROW_SEL : COL_ROW),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(list->rows[i], sel ? 2 : 0, LV_PART_MAIN);
        const uint32_t color = list->enabled[i] ? COL_TEXT : COL_ROW_DIS;
        lv_obj_set_style_text_color(list->labels[i], lv_color_hex(color), LV_PART_MAIN);
        if (list->values[i]) {
            lv_obj_set_style_text_color(list->values[i],
                                        lv_color_hex(list->enabled[i] ? COL_ACCENT : COL_ROW_DIS),
                                        LV_PART_MAIN);
        }
    }
    if (list->count > 0 && list->rows[list->selected]) {
        lv_obj_scroll_to_view(list->rows[list->selected], LV_ANIM_OFF);
    }
}

static void typing_stop(cafe_ui_t *ui)
{
    ui->typing_active = false;
    if (ui->typer) lv_timer_pause(ui->typer);
}

static void typing_cb(lv_timer_t *timer)
{
    cafe_ui_t *ui = (cafe_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    for (int n = 0; n < 2 && ui->typing_index < ui->typing_total; ++n) {
        const uint8_t *src = (const uint8_t *)ui->typing_target;
        size_t step = 1;
        const uint8_t lead = src[ui->typing_index];
        if ((lead & 0xE0u) == 0xC0u) step = 2;
        else if ((lead & 0xF0u) == 0xE0u) step = 3;
        else if ((lead & 0xF8u) == 0xF0u) step = 4;
        if (ui->typing_index + step > ui->typing_total) step = 1;
        memcpy(ui->typing_shown + ui->typing_index, src + ui->typing_index, step);
        ui->typing_index += (uint32_t)step;
    }
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    if (ui->typing_index >= ui->typing_total) typing_stop(ui);
}

void cafe_ui_show_full_text(cafe_ui_t *ui)
{
    if (!ui || !ui->typing_active) return;
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total);
    ui->typing_shown[ui->typing_total] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    typing_stop(ui);
}

bool cafe_ui_typing(const cafe_ui_t *ui)
{
    return ui && ui->typing_active;
}

static void cafe_ui_fit_name(cafe_ui_t *ui)
{
    if (!ui || !ui->name) return;
    const char *name = lv_label_get_text(ui->name);
    if (!name || !name[0]) {
        set_hidden(ui->name, true);
        return;
    }
    // Measure the actual selected font's glyph run, then cap the content width
    // so background + 8px side padding never exceeds 230px.  Long names keep
    // DOT ellipsis; short names do not retain the old 230px dark strip.
    const lv_font_t *font = lv_obj_get_style_text_font(ui->name, LV_PART_MAIN);
    lv_text_attributes_t attrs;
    lv_text_attributes_init(&attrs);
    int32_t content_w = lv_text_get_width(name, (uint32_t)strlen(name), font, &attrs);
    if (content_w < 1) content_w = 1;
    content_w += 2 * CAFE_NAME_PAD_H + 8;   // rounding/line-space margin
    if (content_w > CAFE_NAME_MAX_W) content_w = CAFE_NAME_MAX_W;
    lv_obj_set_width(ui->name, content_w);
    lv_obj_align_to(ui->name, ui->panel, LV_ALIGN_OUT_TOP_LEFT, 8, -1);
    set_hidden(ui->name, false);
}

void cafe_ui_set_text(cafe_ui_t *ui, const char *speaker, const char *text,
                      uint32_t ms_per_char)
{
    if (!ui) return;
    const char *sp = speaker ? speaker : "";
    lv_label_set_text(ui->name, sp);
    if (sp[0] == '\0') set_hidden(ui->name, true);
    else cafe_ui_fit_name(ui);
    const char *src = text ? text : "";
    size_t len = strlen(src);
    if (len >= sizeof(ui->typing_target)) len = sizeof(ui->typing_target) - 1;
    memcpy(ui->typing_target, src, len);
    ui->typing_target[len] = '\0';
    ui->typing_total = (uint32_t)len;
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    ui->typing_step_ms = ms_per_char;
    lv_label_set_text(ui->text, "");
    if (len == 0 || ms_per_char == 0 || !ui->typer) {
        memcpy(ui->typing_shown, ui->typing_target, len);
        ui->typing_shown[len] = '\0';
        ui->typing_index = ui->typing_total;
        lv_label_set_text(ui->text, ui->typing_shown);
        typing_stop(ui);
        return;
    }
    ui->typing_active = true;
    lv_timer_set_period(ui->typer, ms_per_char < 10 ? 10 : ms_per_char);
    lv_timer_reset(ui->typer);
    lv_timer_resume(ui->typer);
}

void cafe_ui_set_brand(cafe_ui_t *ui, const char *title, const char *subtitle)
{
    if (!ui) return;
    lv_label_set_text(ui->brand, title ? title : "");
    lv_label_set_text(ui->brand_sub, subtitle ? subtitle : "");
}

void cafe_ui_set_chapter(cafe_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->chapter, text ? text : "");
}

void cafe_ui_set_battery(cafe_ui_t *ui, int percent)
{
    if (!ui) return;
    char b[16];
    if (percent < 0) snprintf(b, sizeof(b), "--");
    else snprintf(b, sizeof(b), "%d%%", percent);
    lv_label_set_text(ui->battery, b);
}

void cafe_ui_set_notice(cafe_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->notice, text ? text : "");
    set_hidden(ui->notice, !text || !text[0]);
}

void cafe_ui_set_mode(cafe_ui_t *ui, bool auto_play)
{
    if (!ui) return;
    lv_label_set_text(ui->auto_label, auto_play ? "自动" : "");
    set_hidden(ui->auto_label, !auto_play);
}

void cafe_ui_set_font_large(cafe_ui_t *ui, bool large)
{
    if (!ui) return;
    ui->large_text = large;
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    const int lines = large ? CAFE_TEXT_LARGE_LINES : CAFE_TEXT_SMALL_LINES;
    const int line_h = font ? font->line_height : (large ? 23 : 19);
    lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->name, font, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(ui->text, 1, LV_PART_MAIN);
    lv_obj_set_pos(ui->text, CAFE_TEXT_X, large ? CAFE_TEXT_LARGE_Y : CAFE_TEXT_SMALL_Y);
    lv_obj_set_width(ui->text, CAFE_TEXT_W);
    lv_obj_set_height(ui->text, line_h * lines + (lines - 1));
    // Recompute the name anchor after a font change; a long name is elided and
    // cannot grow into the fixed auto hint slot.
    lv_label_set_long_mode(ui->name, LV_LABEL_LONG_DOT);
    cafe_ui_fit_name(ui);
}

void cafe_ui_set_panel_transparency(cafe_ui_t *ui, uint8_t percent)
{
    if (!ui) return;
    if (percent > 55) percent = 55;
    if (percent < 10) percent = 10;
    const lv_opa_t opa = (lv_opa_t)(255 - (percent * 255u) / 100u);
    lv_obj_set_style_bg_opa(ui->panel, opa, LV_PART_MAIN);
}

void cafe_ui_set_rows(cafe_ui_t *ui, cafe_list_t id, const char *const *labels,
                      const char *const *values, int count, int selected)
{
    if (!ui || id >= CAFE_LIST_COUNT) return;
    cafe_list_ui_t *list = &ui->lists[id];
    if (count <= 0) {
        list_clear(ui, id);
        return;
    }
    if (count > list->max_rows) count = list->max_rows;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
    list->count = count;
    list->selected = selected;
    for (int i = 0; i < list->max_rows; ++i) {
        if (i >= count) {
            set_hidden(list->rows[i], true);
            continue;
        }
        if (!list_make_row(ui, id, i)) {
            list->count = i;
            break;
        }
        lv_label_set_text(list->labels[i], labels && labels[i] ? labels[i] : "");
        if (list->values[i]) {
            lv_label_set_text(list->values[i], values && values[i] ? values[i] : "");
        }
        list->enabled[i] = true;
    }
    list_refresh(ui, id);
}

void cafe_ui_set_row_enabled(cafe_ui_t *ui, cafe_list_t id, int row, bool enabled)
{
    if (!ui || id >= CAFE_LIST_COUNT) return;
    cafe_list_ui_t *list = &ui->lists[id];
    if (row < 0 || row >= list->max_rows) return;
    list->enabled[row] = enabled;
    list_refresh(ui, id);
}

void cafe_ui_select_row(cafe_ui_t *ui, cafe_list_t list, int selected)
{
    if (!ui || list >= CAFE_LIST_COUNT) return;
    cafe_list_ui_t *l = &ui->lists[list];
    if (l->count <= 0) return;
    if (selected < 0) selected = 0;
    if (selected >= l->count) selected = l->count - 1;
    l->selected = selected;
    list_refresh(ui, list);
}

void cafe_ui_set_page_title(cafe_ui_t *ui, cafe_page_t page, const char *title)
{
    if (!ui || page >= CAFE_PAGE_COUNT || !ui->titles[page]) return;
    lv_label_set_text(ui->titles[page], title ? title : "");
}

void cafe_ui_set_footer(cafe_ui_t *ui, cafe_page_t page, const char *text)
{
    if (!ui || page >= CAFE_PAGE_COUNT || !ui->footers[page]) return;
    lv_label_set_text(ui->footers[page], text ? text : "");
}

void cafe_ui_set_save_preview(cafe_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->save_preview, text ? text : "");
}

void cafe_ui_set_chapter_info(cafe_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->chapter_info, text ? text : "");
}

void cafe_ui_set_choice(cafe_ui_t *ui, const char *const *labels, const bool *enabled,
                        int count, int selected)
{
    if (!ui) return;
    cafe_ui_set_rows(ui, CAFE_LIST_CHOICE, labels, NULL, count, selected);
    for (int i = 0; i < count && i < ui->lists[CAFE_LIST_CHOICE].max_rows; ++i) {
        cafe_ui_set_row_enabled(ui, CAFE_LIST_CHOICE, i, !enabled || enabled[i]);
    }
    set_hidden(ui->choice_box, count <= 0);
}

void cafe_ui_set_gallery_tiles(cafe_ui_t *ui, const char *const *labels,
                               const uint16_t *image_ids, const bool *unlocked,
                               int count, int selected)
{
    if (!ui) return;
    ui->thumb_failed = false;
    for (int i = 0; i < 6; ++i) {
        const bool show = i < count;
        set_hidden(ui->cg_tiles[i], !show);
        if (!show) continue;
        const bool open = unlocked ? unlocked[i] : true;
        const uint16_t id = image_ids ? image_ids[i] : CAFE_IMAGE_NONE;
        if (open && id != CAFE_IMAGE_NONE &&
            cafe_image_thumb(id, ui->cg_thumb_buf[i], 32, 18)) {
            ui->cg_thumb_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
            ui->cg_thumb_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
            ui->cg_thumb_dsc[i].header.w = 32;
            ui->cg_thumb_dsc[i].header.h = 18;
            ui->cg_thumb_dsc[i].header.stride = 32 * 2;
            ui->cg_thumb_dsc[i].data_size = 32 * 18 * 2;
            ui->cg_thumb_dsc[i].data = (const uint8_t *)ui->cg_thumb_buf[i];
            lv_image_set_src(ui->cg_tile_img[i], &ui->cg_thumb_dsc[i]);
            set_hidden(ui->cg_tile_img[i], false);
        } else {
            set_hidden(ui->cg_tile_img[i], true);
            if (open && id != CAFE_IMAGE_NONE) ui->thumb_failed = true;
        }
        lv_label_set_text(ui->cg_tile_label[i], labels && labels[i] ? labels[i] : "");
        const bool sel = i == selected;
        lv_obj_set_style_border_width(ui->cg_tiles[i], sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(ui->cg_tiles[i],
                                      lv_color_hex(sel ? COL_ACCENT : COL_PANEL_EDGE),
                                      LV_PART_MAIN);
    }
}

void cafe_ui_set_cg(cafe_ui_t *ui, const char *index_text, const char *name,
                    const char *caption)
{
    if (!ui) return;
    lv_label_set_text(ui->cg_index, index_text ? index_text : "");
    lv_label_set_text(ui->cg_name, name ? name : "");
    lv_label_set_text(ui->cg_caption, caption ? caption : "");
}

void cafe_ui_set_transition(cafe_ui_t *ui, const char *eyebrow, const char *title,
                            const char *subtitle)
{
    if (!ui) return;
    set_hidden(ui->transition_eyebrow, false);
    set_hidden(ui->transition_title, false);
    set_hidden(ui->transition_sub, false);
    lv_obj_align(ui->transition_eyebrow, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_align(ui->transition_title, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_align(ui->transition_sub, LV_ALIGN_TOP_MID, 0, 120);
    lv_label_set_text(ui->transition_eyebrow, eyebrow ? eyebrow : "");
    lv_label_set_text(ui->transition_title, title ? title : "");
    lv_label_set_text(ui->transition_sub, subtitle ? subtitle : "");
}

void cafe_ui_set_transition_op(cafe_ui_t *ui, const char *hint)
{
    if (!ui) return;
    // Hide the actual eyebrow/title objects (not only clear their text) so no
    // app capsule remains over the 0..179 OP video. The one small control hint
    // lives in the non-video strip at the bottom and is safe to show.
    set_hidden(ui->transition_eyebrow, true);
    set_hidden(ui->transition_title, true);
    set_hidden(ui->transition_sub, false);
    lv_obj_align(ui->transition_sub, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(ui->transition_sub, hint ? hint : "短按跳过");
}

void cafe_ui_set_ending(cafe_ui_t *ui, const char *eyebrow, const char *title,
                        const char *body)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_eyebrow, eyebrow ? eyebrow : "");
    lv_label_set_text(ui->ending_title, title ? title : "");
    lv_label_set_text(ui->ending_body, body ? body : "");
}

void cafe_ui_set_about(cafe_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->about_text, text ? text : "");
}

void cafe_ui_scroll_about(cafe_ui_t *ui, int dy)
{
    if (ui && ui->about_view) lv_obj_scroll_by(ui->about_view, 0, dy, LV_ANIM_OFF);
}

void cafe_ui_show_modal(cafe_ui_t *ui, const char *title, const char *body,
                        const char *const *actions, int count, int selected)
{
    if (!ui) return;
    lv_label_set_text(ui->modal_title, title ? title : "");
    lv_label_set_text(ui->modal_body, body ? body : "");
    cafe_ui_set_rows(ui, CAFE_LIST_MODAL, actions, NULL, count, selected);
    set_hidden(ui->modal_dim, false);
    set_hidden(ui->modal_box, false);
}

void cafe_ui_hide_modal(cafe_ui_t *ui)
{
    if (!ui) return;
    list_clear(ui, CAFE_LIST_MODAL);
    set_hidden(ui->modal_dim, true);
    set_hidden(ui->modal_box, true);
}

void cafe_ui_bind_art(cafe_ui_t *ui, cafe_bands_t *bands)
{
    if (!ui || !bands) return;
    ui->bands = bands;
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        ui->art_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        ui->art_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        ui->art_dsc[i].header.w = CAFE_BAND_W;
        ui->art_dsc[i].header.h = CAFE_BAND_H;
        ui->art_dsc[i].header.stride = CAFE_BAND_W * 2;
        ui->art_dsc[i].data_size = CAFE_BAND_W * CAFE_BAND_H * 2;
        ui->art_dsc[i].data = (const uint8_t *)bands->pixels[i];
        lv_image_set_src(ui->art[i], &ui->art_dsc[i]);
    }
}

void cafe_ui_art_visible(cafe_ui_t *ui, bool visible)
{
    if (!ui) return;
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        set_hidden(ui->art[i], !visible);
        if (visible && ui->art[i]) lv_obj_invalidate(ui->art[i]);
    }
}

void cafe_ui_art_invalidate(cafe_ui_t *ui)
{
    if (!ui) return;
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        if (ui->art[i]) lv_obj_invalidate(ui->art[i]);
    }
}

void cafe_ui_show_fatal(const char *message)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    lv_obj_t *bg = lv_obj_create(scr);
    if (bg) {
        lv_obj_remove_style_all(bg);
        lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(bg, lv_color_hex(0x1A0E0A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *label = lv_label_create(scr);
    if (label) {
        lv_obj_set_style_text_font(label, &gal_cjk_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(0xF6EFE8), LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_PCT(86));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(label, message ? message : "启动失败");
        lv_obj_center(label);
    }
}

void cafe_ui_show_page(cafe_ui_t *ui, cafe_page_t page)
{
    if (!ui || page >= CAFE_PAGE_COUNT) return;
    for (int i = 0; i < CAFE_PAGE_COUNT; ++i) {
        if (ui->pages[i]) set_hidden(ui->pages[i], true);
    }
    for (int id = 0; id < CAFE_LIST_COUNT; ++id) {
        cafe_list_ui_t *list = &ui->lists[id];
        if (list->page_id != CAFE_PAGE_COUNT && list->page_id != page) {
            list_clear(ui, (cafe_list_t)id);
        }
    }
    // 标题页使用原作标题背景自带 LOGO；仅在启动页显示文字品牌，避免双重标题。
    const bool brand = page == CAFE_PAGE_START;
    set_hidden(ui->brand, !brand);
    set_hidden(ui->brand_sub, !brand);
    if (brand) {
        lv_obj_align(ui->brand, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_align(ui->brand_sub, LV_ALIGN_TOP_MID, 0, 92);
    }
    if (page == CAFE_PAGE_CHOICE) {
        set_hidden(ui->pages[CAFE_PAGE_GAME], false);
        set_hidden(ui->pages[CAFE_PAGE_CHOICE], false);
        return;
    }
    set_hidden(ui->pages[page], false);
}

bool cafe_ui_create(cafe_ui_t *ui)
{
    if (!ui) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_small = &gal_cjk_16;
    ui->font_large = &gal_cjk_20;
    ui->large_text = false;

    // 背景舞台条带：先建，页面覆盖其上。
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        ui->art[i] = lv_image_create(lv_screen_active());
        lv_obj_set_pos(ui->art[i], 0, i * 60);
        lv_image_set_pivot(ui->art[i], 0, 0);
        lv_image_set_scale(ui->art[i], 367);   // cover the full 320px edge after integer scaling
        set_hidden(ui->art[i], true);
    }

    lv_obj_t *start = mk_page(ui, CAFE_PAGE_START, COL_BG, true);
    ui->brand = mk_label(start, ui->font_large, COL_TEXT);
    lv_obj_align(ui->brand, LV_ALIGN_TOP_MID, 0, 58);
    ui->brand_sub = mk_label(start, ui->font_small, COL_ACCENT);
    lv_obj_align(ui->brand_sub, LV_ALIGN_TOP_MID, 0, 92);
    ui->start_hint = mk_label(start, ui->font_small, COL_DIM);
    lv_label_set_text(ui->start_hint, "短按 OK 开始 · 长按 OK 菜单");
    lv_obj_align(ui->start_hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_t *title = mk_page(ui, CAFE_PAGE_TITLE, COL_BG, false);
    ui->title_box = lv_obj_create(title);
    lv_obj_remove_style_all(ui->title_box);
    lv_obj_set_size(ui->title_box, 208, 150);
    lv_obj_set_pos(ui->title_box, 102, 82);
    lv_obj_set_style_bg_color(ui->title_box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->title_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->title_box, 8, LV_PART_MAIN);
    lv_obj_remove_flag(ui->title_box, LV_OBJ_FLAG_SCROLLABLE);
    list_build(ui, CAFE_LIST_TITLE, CAFE_PAGE_TITLE, 104, 88, 204, 23, 6);

    lv_obj_t *game = mk_page(ui, CAFE_PAGE_GAME, COL_BG, false);
    ui->panel = lv_obj_create(game);
    lv_obj_remove_style_all(ui->panel);
    lv_obj_set_size(ui->panel, CAFE_UI_W, CAFE_PANEL_H);
    lv_obj_set_pos(ui->panel, 0, CAFE_PANEL_Y);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->panel, lv_color_hex(COL_PANEL_EDGE), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui->panel, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->panel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->panel, LV_OBJ_FLAG_SCROLLABLE);

    ui->name = mk_label(game, ui->font_small, COL_NAME);
    lv_obj_set_style_bg_color(ui->name, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->name, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->name, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui->name, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->name, 5, LV_PART_MAIN);
    lv_label_set_long_mode(ui->name, LV_LABEL_LONG_DOT);
    lv_obj_align_to(ui->name, ui->panel, LV_ALIGN_OUT_TOP_LEFT, 8, -1);
    set_hidden(ui->name, true);

    ui->text = mk_label(ui->panel, ui->font_small, COL_TEXT);
    lv_obj_set_pos(ui->text, CAFE_TEXT_X, CAFE_TEXT_SMALL_Y);
    lv_obj_set_width(ui->text, CAFE_TEXT_W);
    lv_obj_set_height(ui->text, 19 * CAFE_TEXT_SMALL_LINES + (CAFE_TEXT_SMALL_LINES - 1));
    lv_label_set_long_mode(ui->text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->text, 1, LV_PART_MAIN);

    ui->chapter = mk_label(game, ui->font_small, COL_ACCENT);
    lv_obj_set_pos(ui->chapter, 7, 4);
    lv_obj_set_style_bg_color(ui->chapter, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->chapter, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->chapter, 4, LV_PART_MAIN);
    ui->battery = mk_label(game, ui->font_small, COL_TEXT);
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -7, 4);
    ui->notice = mk_label(game, ui->font_small, COL_ACCENT);
    lv_obj_align(ui->notice, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(ui->notice, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->notice, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->notice, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->notice, 4, LV_PART_MAIN);
    set_hidden(ui->notice, true);
    ui->auto_label = mk_label(game, ui->font_small, COL_ACCENT);
    // Fixed image-area slot above the panel; never compute a position from an
    // empty label or depend on the label's text width.
    lv_obj_set_pos(ui->auto_label, CAFE_AUTO_X, CAFE_AUTO_Y);
    lv_obj_set_size(ui->auto_label, CAFE_AUTO_W, CAFE_AUTO_H);
    lv_obj_set_style_text_align(ui->auto_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    set_hidden(ui->auto_label, true);
    cafe_ui_set_font_large(ui, false);

    lv_obj_t *choice = mk_page(ui, CAFE_PAGE_CHOICE, COL_BG, false);
    ui->choice_box = lv_obj_create(choice);
    lv_obj_remove_style_all(ui->choice_box);
    lv_obj_set_size(ui->choice_box, 284, 132);
    lv_obj_set_pos(ui->choice_box, 18, 16);
    lv_obj_set_style_bg_color(ui->choice_box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->choice_box, (lv_opa_t)240, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->choice_box, lv_color_hex(COL_BOX_EDGE), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->choice_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->choice_box, 8, LV_PART_MAIN);
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_SCROLLABLE);
    list_build(ui, CAFE_LIST_CHOICE, CAFE_PAGE_CHOICE, 22, 24, 276, 30, 4);
    set_hidden(ui->choice_box, true);

    // 菜单
    lv_obj_t *menu = mk_page(ui, CAFE_PAGE_MENU, COL_BG, true);
    lv_obj_t *dim = lv_obj_create(menu);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, CAFE_UI_W, CAFE_UI_H);
    lv_obj_set_style_bg_color(dim, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dim, 150, LV_PART_MAIN);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *menu_box = lv_obj_create(menu);
    lv_obj_remove_style_all(menu_box);
    lv_obj_set_size(menu_box, 220, 200);
    lv_obj_set_pos(menu_box, 50, 20);
    lv_obj_set_style_bg_color(menu_box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_box, lv_color_hex(COL_BOX_EDGE), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(menu_box, 8, LV_PART_MAIN);
    lv_obj_remove_flag(menu_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *menu_title = mk_label(menu_box, ui->font_small, COL_ACCENT);
    lv_label_set_text(menu_title, "快捷菜单");
    lv_obj_align(menu_title, LV_ALIGN_TOP_MID, 0, 8);
    list_build(ui, CAFE_LIST_MENU, CAFE_PAGE_MENU, 54, 46, 212, 25, 6);

    // 存/读档
    for (int p = 0; p < 2; ++p) {
        cafe_page_t page = p == 0 ? CAFE_PAGE_SAVE : CAFE_PAGE_LOAD;
        lv_obj_t *root = mk_page(ui, page, COL_BG, true);
        mk_page_title(ui, page, p == 0 ? "保存进度" : "读取进度");
        list_build(ui, p == 0 ? CAFE_LIST_SAVE : CAFE_LIST_LOAD, page, 8, 36, 304, 38, 3);
        if (p == 0) {
            ui->save_preview = mk_label(root, ui->font_small, COL_DIM);
            lv_obj_set_pos(ui->save_preview, 12, 160);
            lv_obj_set_width(ui->save_preview, 296);
            lv_label_set_long_mode(ui->save_preview, LV_LABEL_LONG_WRAP);
        }
        ui->footers[page] = mk_label(root, ui->font_small, COL_DIM);
        lv_obj_align(ui->footers[page], LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // 设置
    (void)mk_page(ui, CAFE_PAGE_SETTINGS, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_SETTINGS, "设置");
    list_build(ui, CAFE_LIST_SETTINGS, CAFE_PAGE_SETTINGS, 8, 34, 304, 23, 8);

    // 历史
    lv_obj_t *history = mk_page(ui, CAFE_PAGE_HISTORY, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_HISTORY, "历史文本");
    list_build(ui, CAFE_LIST_HISTORY, CAFE_PAGE_HISTORY, 8, 34, 304, 42, 16);
    lv_obj_add_flag(history, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(history, LV_SCROLLBAR_MODE_OFF);

    // 鉴赏室
    (void)mk_page(ui, CAFE_PAGE_GALLERY, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_GALLERY, "鉴赏室");
    list_build(ui, CAFE_LIST_GALLERY, CAFE_PAGE_GALLERY, 8, 58, 304, 34, 5);

    // CG 列表 3x2
    lv_obj_t *cglist = mk_page(ui, CAFE_PAGE_CG_LIST, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_CG_LIST, "CG 鉴赏");
    for (int i = 0; i < 6; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        lv_obj_t *tile = lv_obj_create(cglist);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 96, 62);
        lv_obj_set_pos(tile, 8 + col * 102, 34 + row * 68);
        lv_obj_set_style_bg_color(tile, lv_color_hex(COL_ROW), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(COL_PANEL_EDGE), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 5, LV_PART_MAIN);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        ui->cg_tiles[i] = tile;
        ui->cg_tile_img[i] = lv_image_create(tile);
        lv_image_set_pivot(ui->cg_tile_img[i], 0, 0);
        lv_obj_set_pos(ui->cg_tile_img[i], 16, 6);
        lv_image_set_scale(ui->cg_tile_img[i], 512);   // 32x18 -> 64x36
        set_hidden(ui->cg_tile_img[i], true);
        ui->cg_tile_label[i] = mk_label(tile, ui->font_small, COL_TEXT);
        lv_obj_align(ui->cg_tile_label[i], LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    ui->footers[CAFE_PAGE_CG_LIST] = mk_label(cglist, ui->font_small, COL_DIM);
    lv_obj_align(ui->footers[CAFE_PAGE_CG_LIST], LV_ALIGN_BOTTOM_MID, 0, -3);

    // CG 回想 / 音乐
    for (int p = 0; p < 2; ++p) {
        cafe_page_t page = p == 0 ? CAFE_PAGE_RECALL : CAFE_PAGE_MUSIC;
        lv_obj_t *root = mk_page(ui, page, COL_BG, true);
        mk_page_title(ui, page, p == 0 ? "场景回想" : "音乐鉴赏");
        list_build(ui, p == 0 ? CAFE_LIST_RECALL : CAFE_LIST_MUSIC, page, 8, 40, 304, 34,
                   p == 0 ? 6 : 2);
        ui->footers[page] = mk_label(root, ui->font_small, COL_DIM);
        lv_obj_align(ui->footers[page], LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    // 章节/路线
    lv_obj_t *chapter = mk_page(ui, CAFE_PAGE_CHAPTER, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_CHAPTER, "剧情鉴赏");
    list_build(ui, CAFE_LIST_ROUTE, CAFE_PAGE_CHAPTER, 8, 34, 304, 23, 7);
    list_build(ui, CAFE_LIST_CHAPTER, CAFE_PAGE_CHAPTER, 8, 34, 304, 23, 7);
    ui->chapter_info = mk_label(chapter, ui->font_small, COL_DIM);
    lv_obj_set_pos(ui->chapter_info, 12, 198);
    lv_obj_set_width(ui->chapter_info, 296);
    lv_label_set_long_mode(ui->chapter_info, LV_LABEL_LONG_WRAP);
    ui->footers[CAFE_PAGE_CHAPTER] = mk_label(chapter, ui->font_small, COL_DIM);
    lv_obj_align(ui->footers[CAFE_PAGE_CHAPTER], LV_ALIGN_BOTTOM_MID, 0, -3);

    // 过场
    lv_obj_t *trans = mk_page(ui, CAFE_PAGE_TRANSITION, COL_BG, true);
    // OP 画面放在透明过渡页后的既有三条带上；普通过场由 app 保持隐藏。
    lv_obj_set_style_bg_opa(trans, LV_OPA_TRANSP, LV_PART_MAIN);
    ui->transition_eyebrow = mk_label(trans, ui->font_small, COL_ACCENT);
    lv_obj_align(ui->transition_eyebrow, LV_ALIGN_TOP_MID, 0, 56);
    ui->transition_title = mk_label(trans, ui->font_large, COL_TEXT);
    lv_obj_align(ui->transition_title, LV_ALIGN_TOP_MID, 0, 84);
    ui->transition_sub = mk_label(trans, ui->font_small, COL_DIM);
    lv_obj_align(ui->transition_sub, LV_ALIGN_TOP_MID, 0, 120);
    // OP 画面可能是亮白色首帧；文字必须有不透明暗底，不能只靠文字颜色。
    lv_obj_t *op_labels[] = { ui->transition_eyebrow, ui->transition_title, ui->transition_sub };
    for (size_t i = 0; i < sizeof(op_labels) / sizeof(op_labels[0]); ++i) {
        lv_obj_set_style_bg_color(op_labels[i], lv_color_hex(COL_BOX), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(op_labels[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(op_labels[i], 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(op_labels[i], 2, LV_PART_MAIN);
        lv_obj_set_style_radius(op_labels[i], 4, LV_PART_MAIN);
    }

    // 结局
    lv_obj_t *ending = mk_page(ui, CAFE_PAGE_ENDING, COL_BG, true);
    ui->ending_eyebrow = mk_label(ending, ui->font_small, COL_ACCENT);
    lv_obj_align(ui->ending_eyebrow, LV_ALIGN_TOP_MID, 0, 54);
    ui->ending_title = mk_label(ending, ui->font_large, COL_TEXT);
    lv_obj_align(ui->ending_title, LV_ALIGN_TOP_MID, 0, 82);
    ui->ending_body = mk_label(ending, ui->font_small, COL_DIM);
    lv_obj_set_width(ui->ending_body, 280);
    lv_label_set_long_mode(ui->ending_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->ending_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui->ending_body, LV_ALIGN_TOP_MID, 0, 118);

    // 关于
    lv_obj_t *about = mk_page(ui, CAFE_PAGE_ABOUT, COL_BG, true);
    mk_page_title(ui, CAFE_PAGE_ABOUT, "关于");
    ui->about_view = lv_obj_create(about);
    lv_obj_remove_style_all(ui->about_view);
    lv_obj_set_pos(ui->about_view, 10, 34);
    lv_obj_set_size(ui->about_view, 300, 198);
    lv_obj_set_scrollbar_mode(ui->about_view, LV_SCROLLBAR_MODE_OFF);
    ui->about_text = mk_label(ui->about_view, ui->font_small, COL_TEXT);
    lv_obj_set_width(ui->about_text, 292);
    lv_label_set_long_mode(ui->about_text, LV_LABEL_LONG_WRAP);

    // CG 全屏
    lv_obj_t *cg = mk_page(ui, CAFE_PAGE_CG, COL_BG, false);
    ui->cg_index = mk_label(cg, ui->font_small, COL_ACCENT);
    lv_obj_set_pos(ui->cg_index, 8, 4);
    lv_obj_t *caption = lv_obj_create(cg);
    lv_obj_remove_style_all(caption);
    lv_obj_set_pos(caption, 0, 184);
    lv_obj_set_size(caption, CAFE_UI_W, 56);
    lv_obj_set_style_bg_color(caption, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(caption, (lv_opa_t)220, LV_PART_MAIN);
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_SCROLLABLE);
    ui->cg_name = mk_label(caption, ui->font_large, COL_TEXT);
    lv_obj_set_pos(ui->cg_name, 10, 4);
    ui->cg_caption = mk_label(caption, ui->font_small, COL_DIM);
    lv_obj_set_pos(ui->cg_caption, 10, 28);

    // 弹窗
    ui->modal_dim = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(ui->modal_dim);
    lv_obj_set_size(ui->modal_dim, CAFE_UI_W, CAFE_UI_H);
    lv_obj_set_style_bg_color(ui->modal_dim, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->modal_dim, 170, LV_PART_MAIN);
    lv_obj_remove_flag(ui->modal_dim, LV_OBJ_FLAG_SCROLLABLE);
    set_hidden(ui->modal_dim, true);
    ui->modal_box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(ui->modal_box);
    lv_obj_set_size(ui->modal_box, 270, 140);
    lv_obj_set_pos(ui->modal_box, 25, 50);
    lv_obj_set_style_bg_color(ui->modal_box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->modal_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->modal_box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->modal_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->modal_box, 8, LV_PART_MAIN);
    lv_obj_remove_flag(ui->modal_box, LV_OBJ_FLAG_SCROLLABLE);
    ui->modal_title = mk_label(ui->modal_box, ui->font_small, COL_TEXT);
    lv_obj_align(ui->modal_title, LV_ALIGN_TOP_MID, 0, 10);
    ui->modal_body = mk_label(ui->modal_box, ui->font_small, COL_DIM);
    lv_obj_set_width(ui->modal_body, 246);
    lv_label_set_long_mode(ui->modal_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->modal_body, LV_ALIGN_TOP_MID, 0, 37);
    list_build_parent(ui, CAFE_LIST_MODAL, ui->modal_box, CAFE_PAGE_COUNT, 5, 82, 260, 26, 2);
    set_hidden(ui->modal_box, true);

    ui->typer = lv_timer_create(typing_cb, 20, ui);
    if (!ui->typer) {
        ESP_LOGE("cafe_ui", "LVGL timer pool exhausted");
        return false;
    }
    lv_timer_pause(ui->typer);

    // 提示条改为屏幕级，资源/存档错误在标题页、菜单页也可见。
    lv_obj_set_parent(ui->brand, lv_screen_active());
    lv_obj_set_parent(ui->brand_sub, lv_screen_active());
    lv_obj_move_foreground(ui->brand);
    lv_obj_move_foreground(ui->brand_sub);
    lv_obj_set_parent(ui->notice, lv_screen_active());
    lv_obj_align(ui->notice, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_move_foreground(ui->notice);

    cafe_ui_show_page(ui, CAFE_PAGE_START);
    return true;
}
