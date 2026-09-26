// main/saya_ui.c —— LVGL 界面实现。
#include "saya_ui.h"

#include "esp_log.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "saya_ui";

#define COL_BOX 0x101317
#define COL_LINE 0x35604A
#define COL_TEXT 0xEDF1EE
#define COL_DIM 0x87919A
#define COL_ACCENT 0x74D3A0
#define COL_NAME 0x9FE0BE
#define COL_ROW 0x1B2026
#define COL_ROW_SEL 0x2B5F45
#define COL_PANEL 0x0A0C0F

static void list_set_values_font(saya_list_t *list, const lv_font_t *font);

static void list_create(saya_list_t *list, lv_obj_t *page, const char *title, int row_h, int top)
{
    memset(list, 0, sizeof(*list));
    list->page = page;
    list->row_h = row_h;
    list->top = top;
    list->title = lv_label_create(page);
    lv_label_set_text(list->title, title ? title : "");
    lv_obj_set_pos(list->title, 12, 6);
    lv_obj_set_style_text_color(list->title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
}

static lv_obj_t *list_ensure_row(saya_list_t *list, int index)
{
    if (index < 0 || index >= SAYA_UI_MAX_ROWS) return NULL;
    if (list->rows[index]) return list->rows[index];

    lv_obj_t *row = lv_obj_create(list->page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SAYA_UI_W - 24, list->row_h - 4);
    lv_obj_set_pos(row, 12, list->top + index * list->row_h);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(row, LV_OBJ_FLAG_SCROLLABLE, false);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_color(value, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -10, 0);

    list->rows[index] = label;
    list->values[index] = value;
    return label;
}

static void list_set_rows(saya_list_t *list, const char *const *labels, const char *const *values,
                          int count)
{
    if (count > SAYA_UI_MAX_ROWS) count = SAYA_UI_MAX_ROWS;
    list->count = count;
    for (int i = 0; i < SAYA_UI_MAX_ROWS; ++i) {
        if (i >= count) {
            if (list->rows[i]) lv_obj_set_flag(lv_obj_get_parent(list->rows[i]), LV_OBJ_FLAG_HIDDEN, true);
            continue;
        }
        const lv_obj_t *label = list_ensure_row(list, i);
        lv_label_set_text((lv_obj_t *)label, labels && labels[i] ? labels[i] : "");
        lv_label_set_text(list->values[i], values && values[i] ? values[i] : "");
        lv_obj_set_flag(lv_obj_get_parent(list->rows[i]), LV_OBJ_FLAG_HIDDEN, false);
    }
}

static void list_select(saya_list_t *list, int selected)
{
    if (list->count <= 0) return;
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count - 1;
    list->selected = selected;
    for (int i = 0; i < list->count; ++i) {
        lv_obj_t *row = lv_obj_get_parent(list->rows[i]);
        const bool on = i == selected;
        lv_obj_set_style_bg_color(row, lv_color_hex(on ? COL_ROW_SEL : COL_ROW), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, on ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_text_color(list->rows[i], lv_color_hex(COL_TEXT), LV_PART_MAIN);
    }
}

static void list_apply_font(saya_list_t *list, const lv_font_t *font)
{
    for (int i = 0; i < SAYA_UI_MAX_ROWS; ++i) {
        if (!list->rows[i]) continue;
        lv_obj_set_style_text_font(list->rows[i], font, LV_PART_MAIN);
        lv_obj_set_style_text_font(list->values[i], font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_font(list->title, font, LV_PART_MAIN);
}

// ---------------------------------------------------------------- 打字机
static void typing_stop(saya_ui_t *ui)
{
    ui->typing_active = false;
    if (ui->typewriter_timer) lv_timer_pause(ui->typewriter_timer);
}

static void typing_cb(lv_timer_t *timer)
{
    saya_ui_t *ui = (saya_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    // 每 tick 推进 2 个字符:15ms/字时把定时器中断和重绘的开销摊薄一半。
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

void saya_ui_show_full_text(saya_ui_t *ui)
{
    if (!ui || !ui->typing_active) return;
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total);
    ui->typing_shown[ui->typing_total] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    typing_stop(ui);
}

bool saya_ui_typing(const saya_ui_t *ui)
{
    return ui && ui->typing_active;
}

void saya_ui_set_text(saya_ui_t *ui, const char *speaker, const char *page_text,
                      uint32_t ms_per_char)
{
    if (!ui) return;
    lv_label_set_text(ui->name, speaker ? speaker : "");

    const char *src = page_text ? page_text : "";
    size_t len = strlen(src);
    if (len >= sizeof(ui->typing_target)) {
        ESP_LOGW(TAG, "文本超长被截断: %u", (unsigned)len);
        len = sizeof(ui->typing_target) - 1;
    }
    memcpy(ui->typing_target, src, len);
    ui->typing_target[len] = '\0';
    ui->typing_total = (uint32_t)len;
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    ui->typing_step_ms = ms_per_char;
    lv_label_set_text(ui->text, "");

    if (len == 0 || ms_per_char == 0 || !ui->typewriter_timer) {
        memcpy(ui->typing_shown, ui->typing_target, len);
        ui->typing_shown[len] = '\0';
        ui->typing_index = ui->typing_total;
        lv_label_set_text(ui->text, ui->typing_shown);
        typing_stop(ui);
        return;
    }

    ui->typing_active = true;
    lv_timer_set_period(ui->typewriter_timer, ms_per_char < 10 ? 10 : ms_per_char);
    lv_timer_reset(ui->typewriter_timer);
    lv_timer_resume(ui->typewriter_timer);
}

void saya_ui_set_chapter(saya_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->chapter, text ? text : "");
}

void saya_ui_set_battery(saya_ui_t *ui, int percent)
{
    if (!ui) return;
    if (percent < 0) {
        lv_label_set_text(ui->battery, "--");
        lv_obj_set_style_text_color(ui->battery, lv_color_hex(COL_DIM), LV_PART_MAIN);
        return;
    }
    lv_label_set_text_fmt(ui->battery, "%d%%", percent);
    lv_obj_set_style_text_color(ui->battery, lv_color_hex(percent < 20 ? 0xFF8A7A : COL_DIM),
                                LV_PART_MAIN);
}

void saya_ui_set_font_size(saya_ui_t *ui, bool large)
{
    if (!ui) return;
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->name, font, LV_PART_MAIN);
    list_apply_font(&ui->title_menu, font);
    list_apply_font(&ui->menu, font);
    list_apply_font(&ui->settings_menu, font);
    list_apply_font(&ui->slots_menu, font);
    list_apply_font(&ui->about_menu, font);
    lv_obj_set_style_text_font(ui->ending_title, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->ending_hint, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->warning_text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->warning_hint, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->about_text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->slots_title, large ? ui->font_large : ui->font_small,
                               LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->slots_hint, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->ending_name, ui->font_large, LV_PART_MAIN);
}

// ---------------------------------------------------------------- 页面
static void page_visibility(saya_ui_t *ui, saya_page_t page)
{
    lv_obj_set_flag(ui->page_warning, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_menu, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_settings, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_slots, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_ending, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(ui->page_about, LV_OBJ_FLAG_HIDDEN, true);

    switch (page) {
    case SAYA_PAGE_WARNING: lv_obj_set_flag(ui->page_warning, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_TITLE: lv_obj_set_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_GAME: lv_obj_set_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_MENU: lv_obj_set_flag(ui->page_menu, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_SETTINGS: lv_obj_set_flag(ui->page_settings, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_SLOTS: lv_obj_set_flag(ui->page_slots, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_ENDING: lv_obj_set_flag(ui->page_ending, LV_OBJ_FLAG_HIDDEN, false); break;
    case SAYA_PAGE_ABOUT: lv_obj_set_flag(ui->page_about, LV_OBJ_FLAG_HIDDEN, false); break;
    default: break;
    }
    if (page != SAYA_PAGE_GAME) {
        typing_stop(ui);
        lv_obj_set_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN, true);
        ui->choice_count = 0;
    }
}

void saya_ui_show_page(saya_ui_t *ui, saya_page_t page)
{
    if (!ui) return;
    page_visibility(ui, page);
}

// ---------------------------------------------------------------- 选项
void saya_ui_show_choices(saya_ui_t *ui, const char *first, const char *second)
{
    if (!ui) return;
    typing_stop(ui);
    ui->choice_count = 2;
    ui->choice_selected = 0;
    lv_label_set_text(ui->choice_texts[0], first ? first : "");
    lv_label_set_text(ui->choice_texts[1], second ? second : "");
    lv_label_set_text(ui->name, "");
    lv_label_set_text(ui->text, "");
    lv_obj_set_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN, false);
    saya_ui_set_choice_selected(ui, 0);
}

void saya_ui_hide_choices(saya_ui_t *ui)
{
    if (!ui) return;
    ui->choice_count = 0;
    lv_obj_set_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN, true);
}

void saya_ui_set_choice_selected(saya_ui_t *ui, int index)
{
    if (!ui || ui->choice_count <= 0) return;
    if (index < 0 || index >= ui->choice_count) return;
    ui->choice_selected = index;
    for (int i = 0; i < ui->choice_count; ++i) {
        lv_obj_t *row = ui->choice_rows[i];
        const bool on = i == index;
        lv_obj_set_style_bg_color(row, lv_color_hex(on ? COL_ROW_SEL : COL_ROW), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, on ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    }
}

int saya_ui_choice_selected(const saya_ui_t *ui)
{
    return ui ? ui->choice_selected : 0;
}

// ---------------------------------------------------------------- 建界面
static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, SAYA_UI_W, SAYA_BOX_H);
    lv_obj_set_pos(box, 0, SAYA_BOX_Y);
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_flag(box, LV_OBJ_FLAG_SCROLLABLE, false);
    return box;
}

static void list_set_values_font(saya_list_t *list, const lv_font_t *font)
{
    for (int i = 0; i < SAYA_UI_MAX_ROWS; ++i) {
        if (!list->values[i]) continue;
        lv_obj_set_style_text_font(list->values[i], font, LV_PART_MAIN);
    }
}

bool saya_ui_create(saya_ui_t *ui, uint16_t *art_pixels, uint8_t *sprite_scratch,
                    uint32_t sprite_scratch_size, const lv_font_t *font_small,
                    const lv_font_t *font_large)
{
    if (!ui || !art_pixels || !sprite_scratch || !font_small || !font_large) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_small = font_small;
    ui->font_large = font_large;

    lv_obj_t *root = lv_screen_active();
    lv_obj_set_flag(root, LV_OBJ_FLAG_SCROLLABLE, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), LV_PART_MAIN);

    // 画面区画布:标题页和正文页共用同一张缓冲,靠 z 序让全屏页面盖住它。
    lv_obj_t *art_holder = lv_obj_create(root);
    lv_obj_remove_style_all(art_holder);
    lv_obj_set_size(art_holder, SAYA_ART_W, SAYA_ART_H);
    lv_obj_set_pos(art_holder, 0, 0);
    lv_obj_set_flag(art_holder, LV_OBJ_FLAG_SCROLLABLE, false);
    if (!saya_image_init(&ui->art, art_holder, art_pixels, sprite_scratch,
                         sprite_scratch_size)) {
        return false;
    }

    // 画面区左上角章节提示 + 右上角电量(避开圆角遮罩)
    ui->chapter = lv_label_create(root);
    lv_obj_set_style_text_font(ui->chapter, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->chapter, lv_color_hex(0xE7EDE9), LV_PART_MAIN);
    lv_obj_set_pos(ui->chapter, 10, 8);
    lv_obj_set_style_bg_color(ui->chapter, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->chapter, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->chapter, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui->chapter, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->chapter, 6, LV_PART_MAIN);

    ui->battery = lv_label_create(root);
    lv_obj_set_style_text_font(ui->battery, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->battery, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_style_bg_color(ui->battery, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->battery, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->battery, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui->battery, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->battery, 6, LV_PART_MAIN);

    // 正文页
    ui->page_game = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_game);
    lv_obj_set_size(ui->page_game, SAYA_UI_W, SAYA_BOX_H);
    lv_obj_set_pos(ui->page_game, 0, SAYA_BOX_Y);
    lv_obj_set_flag(ui->page_game, LV_OBJ_FLAG_SCROLLABLE, false);
    ui->box = make_box(ui->page_game);

    ui->name = lv_label_create(ui->box);
    lv_obj_set_style_text_font(ui->name, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->name, lv_color_hex(COL_NAME), LV_PART_MAIN);
    lv_obj_set_pos(ui->name, 10, 2);

    ui->text = lv_label_create(ui->box);
    lv_obj_set_style_text_font(ui->text, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->text, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_pos(ui->text, 10, 22);
    lv_obj_set_size(ui->text, SAYA_UI_W - 20, 80);
    lv_label_set_long_mode(ui->text, LV_LABEL_LONG_WRAP);

    // 选项页(同一区域,盖住文本框)
    ui->choice_box = make_box(ui->page_game);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *row = lv_obj_create(ui->choice_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, SAYA_UI_W - 24, 36);
        lv_obj_set_pos(row, 12, 12 + i * 44);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_flag(row, LV_OBJ_FLAG_SCROLLABLE, false);
        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, font_small, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_width(label, SAYA_UI_W - 60);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        ui->choice_rows[i] = row;
        ui->choice_texts[i] = label;
    }
    lv_obj_set_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN, true);

    // 标题页(只放菜单,画面区显示标题图)
    ui->page_title = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_title);
    lv_obj_set_size(ui->page_title, SAYA_UI_W, SAYA_BOX_H);
    lv_obj_set_pos(ui->page_title, 0, SAYA_BOX_Y);
    lv_obj_set_flag(ui->page_title, LV_OBJ_FLAG_SCROLLABLE, false);
    // 标题页的菜单只有 40..100 的可用高度(上面是画面区),4 行以内能排下。
    list_create(&ui->title_menu, ui->page_title, "", 24, 4);

    // 全屏页面
    ui->page_menu = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_menu);
    lv_obj_set_size(ui->page_menu, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_menu, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_menu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_menu, LV_OBJ_FLAG_SCROLLABLE, false);
    list_create(&ui->menu, ui->page_menu, "菜单", 36, 40);
    lv_obj_set_style_text_font(ui->menu.title, font_large, LV_PART_MAIN);

    ui->page_settings = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_settings);
    lv_obj_set_size(ui->page_settings, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_settings, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_settings, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_settings, LV_OBJ_FLAG_SCROLLABLE, false);
    list_create(&ui->settings_menu, ui->page_settings, "设置", 40, 44);
    lv_obj_set_style_text_font(ui->settings_menu.title, font_large, LV_PART_MAIN);

    ui->page_slots = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_slots);
    lv_obj_set_size(ui->page_slots, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_slots, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_slots, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_slots, LV_OBJ_FLAG_SCROLLABLE, false);
    list_create(&ui->slots_menu, ui->page_slots, "存档", 32, 40);
    lv_obj_set_style_text_font(ui->slots_menu.title, font_large, LV_PART_MAIN);
    ui->slots_title = ui->slots_menu.title;
    ui->slots_hint = lv_label_create(ui->page_slots);
    lv_obj_set_style_text_font(ui->slots_hint, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->slots_hint, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(ui->slots_hint, LV_ALIGN_BOTTOM_LEFT, 12, -6);

    ui->page_ending = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_ending);
    lv_obj_set_size(ui->page_ending, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_ending, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_ending, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_ending, LV_OBJ_FLAG_SCROLLABLE, false);
    ui->ending_title = lv_label_create(ui->page_ending);
    lv_obj_set_style_text_font(ui->ending_title, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->ending_title, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_label_set_text(ui->ending_title, "达成结局");
    lv_obj_set_pos(ui->ending_title, 12, 88);
    ui->ending_name = lv_label_create(ui->page_ending);
    lv_obj_set_style_text_font(ui->ending_name, font_large, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->ending_name, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_pos(ui->ending_name, 12, 112);
    ui->ending_hint = lv_label_create(ui->page_ending);
    lv_obj_set_style_text_font(ui->ending_hint, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->ending_hint, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_pos(ui->ending_hint, 12, 190);

    ui->page_about = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_about);
    lv_obj_set_size(ui->page_about, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_about, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_about, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_about, LV_OBJ_FLAG_SCROLLABLE, false);
    list_create(&ui->about_menu, ui->page_about, "关于", 36, 168);
    lv_obj_set_style_text_font(ui->about_menu.title, font_large, LV_PART_MAIN);
    ui->about_text = lv_label_create(ui->page_about);
    lv_obj_set_style_text_font(ui->about_text, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->about_text, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_pos(ui->about_text, 12, 40);
    lv_obj_set_size(ui->about_text, SAYA_UI_W - 24, 120);
    lv_label_set_long_mode(ui->about_text, LV_LABEL_LONG_WRAP);

    ui->page_warning = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_warning);
    lv_obj_set_size(ui->page_warning, SAYA_UI_W, SAYA_UI_H);
    lv_obj_set_style_bg_color(ui->page_warning, lv_color_hex(0x1A0A0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->page_warning, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flag(ui->page_warning, LV_OBJ_FLAG_SCROLLABLE, false);
    ui->warning_text = lv_label_create(ui->page_warning);
    lv_obj_set_style_text_font(ui->warning_text, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->warning_text, lv_color_hex(0xFFD9D9), LV_PART_MAIN);
    lv_obj_set_pos(ui->warning_text, 12, 12);
    lv_obj_set_size(ui->warning_text, SAYA_UI_W - 24, SAYA_UI_H - 60);
    lv_label_set_long_mode(ui->warning_text, LV_LABEL_LONG_WRAP);
    ui->warning_hint = lv_label_create(ui->page_warning);
    lv_obj_set_style_text_font(ui->warning_hint, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->warning_hint, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(ui->warning_hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    ui->typewriter_timer = lv_timer_create(typing_cb, 40, ui);
    if (!ui->typewriter_timer) {
        ESP_LOGE(TAG, "打字机定时器创建失败");
        return false;
    }
    lv_timer_pause(ui->typewriter_timer);

    saya_ui_show_page(ui, SAYA_PAGE_WARNING);
    return true;
}

// ---------------------------------------------------------------- 内容设置
bool saya_ui_set_title_art(saya_ui_t *ui, const saya_pack_t *pack)
{
    if (!ui || !pack) return false;
    return saya_image_show(&ui->art, pack, SAYA_BG_TITLE, SAYA_NONE);
}

bool saya_ui_set_scene(saya_ui_t *ui, const saya_pack_t *pack, uint16_t bg, uint16_t fg)
{
    if (!ui || !pack) return false;
    return saya_image_show(&ui->art, pack, bg, fg);
}

void saya_ui_set_title_rows(saya_ui_t *ui, const char *const *labels, int count, int selected)
{
    if (!ui) return;
    list_set_rows(&ui->title_menu, labels, NULL, count);
    list_select(&ui->title_menu, selected);
    list_set_values_font(&ui->title_menu, ui->font_small);
}

void saya_ui_set_menu_rows(saya_ui_t *ui, const char *const *labels, int count, int selected)
{
    if (!ui) return;
    list_set_rows(&ui->menu, labels, NULL, count);
    list_select(&ui->menu, selected);
}

void saya_ui_set_about_rows(saya_ui_t *ui, const char *const *labels, int count, int selected)
{
    if (!ui) return;
    list_set_rows(&ui->about_menu, labels, NULL, count);
    list_select(&ui->about_menu, selected);
}

void saya_ui_set_settings_rows(saya_ui_t *ui, const char *const *labels,
                               const char *const *values, int count, int selected)
{
    if (!ui) return;
    list_set_rows(&ui->settings_menu, labels, values, count);
    list_select(&ui->settings_menu, selected);
    list_set_values_font(&ui->settings_menu, ui->font_small);
}

void saya_ui_set_setting_value(saya_ui_t *ui, int row, const char *value)
{
    if (!ui || row < 0 || row >= SAYA_UI_MAX_ROWS || !ui->settings_menu.values[row]) return;
    lv_label_set_text(ui->settings_menu.values[row], value ? value : "");
}

void saya_ui_set_settings_selected(saya_ui_t *ui, int index)
{
    if (!ui) return;
    list_select(&ui->settings_menu, index);
}

void saya_ui_slots_setup(saya_ui_t *ui, const char *title, const char *hint,
                         const char *const *labels, const char *const *values, int count)
{
    if (!ui) return;
    lv_label_set_text(ui->slots_title, title ? title : "");
    lv_label_set_text(ui->slots_hint, hint ? hint : "");
    list_set_rows(&ui->slots_menu, labels, values, count);
    list_set_values_font(&ui->slots_menu, ui->font_small);
    list_select(&ui->slots_menu, 0);
}

void saya_ui_set_slots_selected(saya_ui_t *ui, int index)
{
    if (!ui) return;
    list_select(&ui->slots_menu, index);
}

void saya_ui_set_ending(saya_ui_t *ui, const char *name, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_name, name ? name : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
}

void saya_ui_set_warning(saya_ui_t *ui, const char *text, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->warning_text, text ? text : "");
    lv_label_set_text(ui->warning_hint, hint ? hint : "");
}

void saya_ui_set_about_text(saya_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->about_text, text ? text : "");
}
