// main/gal/gal_ui.c —— 框架骨架的 LVGL 界面实现（竖屏 240x320）。
#include "gal_ui.h"

#include "esp_log.h"

#include "gal_settings.h"   // gal_panel_opa()：透明度百分比换算

#include <stdio.h>
#include <string.h>

static const char *TAG = "gal_ui";

// 骨架配色:深蓝紫渐变 + 青绿色强调。
#define COL_BG_TOP   0x1C2A4A
#define COL_BG_BOT   0x2E2440
#define COL_PANEL    0x0B1020
#define COL_TEXT     0xEDF1EE
#define COL_DIM      0x87919A
#define COL_ACCENT   0x74D3A0
#define COL_NAME     0x9FE0BE
#define COL_ROW      0x1B2026
#define COL_ROW_SEL  0x2B5F45
#define COL_PAGE_BG  0x0A0C0F

// ---------------------------------------------------------------- 打字机
static void typing_stop(gal_ui_t *ui)
{
    ui->typing_active = false;
    if (ui->typewriter_timer) lv_timer_pause(ui->typewriter_timer);
}

static void typing_cb(lv_timer_t *timer)
{
    gal_ui_t *ui = (gal_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    // 每 tick 推进 2 个字符:把定时器中断与重绘开销摊薄。
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

void gal_ui_show_full_text(gal_ui_t *ui)
{
    if (!ui || !ui->typing_active) return;
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total);
    ui->typing_shown[ui->typing_total] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    typing_stop(ui);
}

bool gal_ui_typing(const gal_ui_t *ui)
{
    return ui && ui->typing_active;
}

void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *page_text,
                     uint32_t ms_per_char)
{
    if (!ui) return;
    lv_label_set_text(ui->name, speaker ? speaker : "");
    // 旁白不显示名牌,避免一个空胶囊浮在画面上。
    if (speaker && speaker[0] != '\0') {
        lv_obj_remove_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    }

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

// ---------------------------------------------------------------- 列表
static void list_ensure_row(gal_ui_t *ui, gal_list_t id, int index)
{
    gal_list_ui_t *list = &ui->lists[id];
    if (index < 0 || index >= GAL_LIST_MAX_ROWS) return;
    if (list->rows[index]) return;

    lv_obj_t *row = lv_obj_create(list->page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, GAL_UI_W - 24, list->row_h - 4);
    lv_obj_set_pos(row, 12, list->top + index * list->row_h);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_color(value, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -10, 0);

    // 行是懒创建的:补一次字体,否则后建的行会用默认字体(没有汉字)。
    if (list->font) {
        lv_obj_set_style_text_font(label, list->font, LV_PART_MAIN);
        lv_obj_set_style_text_font(value, list->font, LV_PART_MAIN);
    }

    list->rows[index] = label;
    list->values[index] = value;
}

void gal_ui_set_rows(gal_ui_t *ui, gal_list_t id, const char *const *labels,
                     const char *const *values, int count, int selected)
{
    if (!ui) return;
    gal_list_ui_t *list = &ui->lists[id];
    if (count > GAL_LIST_MAX_ROWS) count = GAL_LIST_MAX_ROWS;
    list->count = count;
    for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) {
        if (i >= count) {
            if (list->rows[i]) {
                lv_obj_add_flag(lv_obj_get_parent(list->rows[i]), LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        list_ensure_row(ui, id, i);
        if (!list->rows[i]) continue;
        lv_label_set_text(list->rows[i], labels && labels[i] ? labels[i] : "");
        lv_label_set_text(list->values[i], values && values[i] ? values[i] : "");
        lv_obj_remove_flag(lv_obj_get_parent(list->rows[i]), LV_OBJ_FLAG_HIDDEN);
    }
    gal_ui_select_row(ui, id, selected);
}

void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t id, int row, const char *value)
{
    if (!ui || row < 0 || row >= GAL_LIST_MAX_ROWS) return;
    if (!ui->lists[id].values[row]) return;
    lv_label_set_text(ui->lists[id].values[row], value ? value : "");
}

void gal_ui_select_row(gal_ui_t *ui, gal_list_t id, int selected)
{
    if (!ui) return;
    gal_list_ui_t *list = &ui->lists[id];
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
    }
    // 光标移出可视区时把列表带过去(窗口滚动)。
    lv_obj_t *sel_row = list->rows[selected] ? lv_obj_get_parent(list->rows[selected]) : NULL;
    if (sel_row) lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF);
}

static void list_create(gal_ui_t *ui, gal_list_t id, lv_obj_t *page, const char *title,
                        int row_h, int top)
{
    gal_list_ui_t *list = &ui->lists[id];
    memset(list, 0, sizeof(*list));
    list->page = page;
    list->row_h = row_h;
    list->top = top;
    list->font = NULL;
    // 行数超出窗口时允许整页滚动,list_select 会把选中行带进可视区。
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    if (title && title[0] != '\0') {
        list->title = lv_label_create(page);
        lv_label_set_text(list->title, title);
        lv_obj_set_pos(list->title, 12, 6);
        lv_obj_set_style_text_color(list->title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------- 建界面
static lv_obj_t *full_page(lv_obj_t *root, lv_color_t bg)
{
    lv_obj_t *page = lv_obj_create(root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static void page_visibility(gal_ui_t *ui, gal_page_t page)
{
    lv_obj_add_flag(ui->page_start, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_about, LV_OBJ_FLAG_HIDDEN);

    switch (page) {
    case GAL_PAGE_START: lv_obj_remove_flag(ui->page_start, LV_OBJ_FLAG_HIDDEN); break;
    case GAL_PAGE_TITLE: lv_obj_remove_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN); break;
    case GAL_PAGE_GAME: lv_obj_remove_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN); break;
    case GAL_PAGE_SETTINGS: lv_obj_remove_flag(ui->page_settings, LV_OBJ_FLAG_HIDDEN); break;
    case GAL_PAGE_ABOUT: lv_obj_remove_flag(ui->page_about, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }
    if (page != GAL_PAGE_GAME) typing_stop(ui);
}

void gal_ui_show_page(gal_ui_t *ui, gal_page_t page)
{
    if (!ui) return;
    page_visibility(ui, page);
}

bool gal_ui_create(gal_ui_t *ui, const lv_font_t *font_small, const lv_font_t *font_large)
{
    if (!ui || !font_small || !font_large) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_small = font_small;
    ui->font_large = font_large;

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_PAGE_BG), LV_PART_MAIN);

    // 程序绘制的渐变背景:骨架不携带任何美术资源。
    ui->bg = lv_obj_create(root);
    lv_obj_remove_style_all(ui->bg);
    lv_obj_set_size(ui->bg, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(ui->bg, 0, 0);
    lv_obj_set_style_bg_color(ui->bg, lv_color_hex(COL_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ui->bg, lv_color_hex(COL_BG_BOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui->bg, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ui->bg, LV_OBJ_FLAG_SCROLLABLE);

    // 启动页与标题页的作品名(居中偏上)。
    ui->brand_title = lv_label_create(root);
    lv_obj_set_style_text_font(ui->brand_title, font_large, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->brand_title, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_align(ui->brand_title, LV_ALIGN_TOP_MID, 0, 72);

    ui->brand_sub = lv_label_create(root);
    lv_obj_set_style_text_font(ui->brand_sub, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->brand_sub, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(ui->brand_sub, LV_ALIGN_TOP_MID, 0, 106);

    // 左上角标题与右上角电量。
    ui->chapter = lv_label_create(root);
    lv_obj_set_style_text_font(ui->chapter, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->chapter, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_pos(ui->chapter, 6, 4);

    ui->battery = lv_label_create(root);
    lv_obj_set_style_text_font(ui->battery, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->battery, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_label_set_text(ui->battery, "--%");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -6, 4);

    ui->notice = lv_label_create(root);
    lv_obj_set_style_text_font(ui->notice, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->notice, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_align(ui->notice, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);

    // --- 启动页与标题页:共用同一块下沿菜单区 ---
    ui->page_start = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_start);
    lv_obj_set_size(ui->page_start, GAL_UI_W, GAL_PANEL_H);
    lv_obj_set_pos(ui->page_start, 0, GAL_PANEL_Y);
    lv_obj_remove_flag(ui->page_start, LV_OBJ_FLAG_SCROLLABLE);
    list_create(ui, GAL_LIST_START, ui->page_start, "", 36, (GAL_PANEL_H - 36) / 2);

    ui->page_title = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_title);
    lv_obj_set_size(ui->page_title, GAL_UI_W, GAL_PANEL_H);
    lv_obj_set_pos(ui->page_title, 0, GAL_PANEL_Y);
    lv_obj_remove_flag(ui->page_title, LV_OBJ_FLAG_SCROLLABLE);
    list_create(ui, GAL_LIST_TITLE, ui->page_title, "", 30, 8);

    // --- 正文页:名牌 + 对话框面板 ---
    ui->page_game = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_game);
    lv_obj_set_size(ui->page_game, GAL_UI_W, GAL_PANEL_H);
    lv_obj_set_pos(ui->page_game, 0, GAL_PANEL_Y);
    lv_obj_remove_flag(ui->page_game, LV_OBJ_FLAG_SCROLLABLE);

    ui->panel = lv_obj_create(ui->page_game);
    lv_obj_remove_style_all(ui->panel);
    lv_obj_set_size(ui->panel, GAL_UI_W, GAL_PANEL_H);
    lv_obj_set_pos(ui->panel, 0, 0);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    // 默认 30% 透明(70% 不透明),初始化时按设置覆盖。
    lv_obj_set_style_bg_opa(ui->panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(ui->panel, LV_OBJ_FLAG_SCROLLABLE);

    // 名牌挂在面板上沿左侧。
    ui->name = lv_label_create(ui->page_game);
    lv_obj_set_style_text_font(ui->name, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->name, lv_color_hex(COL_NAME), LV_PART_MAIN);
    lv_obj_align(ui->name, LV_ALIGN_TOP_LEFT, 8, 0);
    lv_obj_set_style_bg_color(ui->name, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->name, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->name, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui->name, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->name, 6, LV_PART_MAIN);
    lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);

    ui->text = lv_label_create(ui->panel);
    lv_obj_set_style_text_font(ui->text, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->text, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_pos(ui->text, GAL_TEXT_X, 6);
    lv_obj_set_width(ui->text, GAL_TEXT_W);
    lv_label_set_long_mode(ui->text, LV_LABEL_LONG_WRAP);
    // 字体行高为 19/23,补 1px 行距后正好 20/24(框架规范)。
    lv_obj_set_style_text_line_space(ui->text, 1, LV_PART_MAIN);

    // --- 设置页(全屏) ---
    ui->page_settings = full_page(root, lv_color_hex(COL_PAGE_BG));
    list_create(ui, GAL_LIST_SETTINGS, ui->page_settings, "设置", 38, 42);

    // --- 关于页(全屏 + 可滚动正文) ---
    ui->page_about = full_page(root, lv_color_hex(COL_PAGE_BG));
    lv_obj_t *about_title = lv_label_create(ui->page_about);
    lv_obj_set_style_text_font(about_title, font_large, LV_PART_MAIN);
    lv_obj_set_style_text_color(about_title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_label_set_text(about_title, "关于");
    lv_obj_set_pos(about_title, 12, 6);

    ui->about_view = lv_obj_create(ui->page_about);
    lv_obj_remove_style_all(ui->about_view);
    lv_obj_set_pos(ui->about_view, 12, 40);
    lv_obj_set_size(ui->about_view, GAL_UI_W - 24, 200);
    lv_obj_set_style_bg_opa(ui->about_view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ui->about_view, LV_SCROLLBAR_MODE_OFF);
    ui->about_text = lv_label_create(ui->about_view);
    lv_obj_set_style_text_font(ui->about_text, font_small, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->about_text, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_width(ui->about_text, GAL_UI_W - 32);
    lv_label_set_long_mode(ui->about_text, LV_LABEL_LONG_WRAP);
    list_create(ui, GAL_LIST_ABOUT, ui->page_about, "", 30, GAL_UI_H - 40);

    ui->typewriter_timer = lv_timer_create(typing_cb, 40, ui);
    if (!ui->typewriter_timer) {
        ESP_LOGE(TAG, "打字机定时器创建失败");
        return false;
    }
    lv_timer_pause(ui->typewriter_timer);

    gal_ui_show_page(ui, GAL_PAGE_START);
    return true;
}

// ---------------------------------------------------------------- 设置与状态
void gal_ui_set_brand(gal_ui_t *ui, const char *title, const char *subtitle)
{
    if (!ui) return;
    lv_label_set_text(ui->brand_title, title ? title : "");
    lv_label_set_text(ui->brand_sub, subtitle ? subtitle : "");
}

void gal_ui_set_font_size(gal_ui_t *ui, bool large)
{
    if (!ui) return;
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->name, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->about_text, font, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->brand_title,
                               large ? ui->font_large : ui->font_small, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->brand_sub, font, LV_PART_MAIN);
    for (int i = 0; i < GAL_LIST_COUNT; ++i) {
        gal_list_ui_t *list = &ui->lists[i];
        list->font = font;
        for (int r = 0; r < GAL_LIST_MAX_ROWS; ++r) {
            if (list->rows[r]) {
                lv_obj_set_style_text_font(list->rows[r], font, LV_PART_MAIN);
                lv_obj_set_style_text_font(list->values[r], font, LV_PART_MAIN);
            }
        }
        if (list->title) lv_obj_set_style_text_font(list->title, font, LV_PART_MAIN);
    }
}

void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent)
{
    if (!ui) return;
    lv_obj_set_style_bg_opa(ui->panel, (lv_opa_t)gal_panel_opa(percent), LV_PART_MAIN);
}

void gal_ui_set_chapter(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->chapter, text ? text : "");
}

void gal_ui_set_battery(gal_ui_t *ui, int percent)
{
    if (!ui) return;
    if (percent < 0) {
        lv_label_set_text(ui->battery, "--%");
        lv_obj_set_style_text_color(ui->battery, lv_color_hex(COL_DIM), LV_PART_MAIN);
        return;
    }
    lv_label_set_text_fmt(ui->battery, "%d%%", percent);
    lv_obj_set_style_text_color(ui->battery,
                                lv_color_hex(percent < 20 ? 0xFF8A7A : COL_DIM), LV_PART_MAIN);
}

void gal_ui_set_notice(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->notice, text ? text : "");
    if (text && text[0] != '\0') {
        lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

void gal_ui_set_about_text(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->about_text, text ? text : "");
    if (ui->about_view) lv_obj_scroll_to_y(ui->about_view, 0, LV_ANIM_OFF);
}

void gal_ui_about_scroll(gal_ui_t *ui, int dy)
{
    if (!ui || !ui->about_view || dy == 0) return;
    // LVGL 的 scroll 值为负:界面层 dy > 0 表示往下看,传负值。
    lv_obj_scroll_by(ui->about_view, 0, -dy, LV_ANIM_OFF);
}
