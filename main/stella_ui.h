// main/stella_ui.h —— 横屏 320x240 的 LVGL 界面层。
#pragma once

#include "lvgl.h"
#include "stella_image.h"

#include <stdbool.h>
#include <stdint.h>

#define STELLA_LIST_ROWS 8
#define STELLA_NAME_BYTES 64
#define STELLA_TEXT_BYTES 1024
#define STELLA_HISTORY_BYTES 2560

typedef enum {
    STELLA_PAGE_TITLE = 0,
    STELLA_PAGE_GAME,
    STELLA_PAGE_MENU,
    STELLA_PAGE_SLOTS,
    STELLA_PAGE_SETTINGS,
    STELLA_PAGE_CHAPTERS,
    STELLA_PAGE_GALLERY,
    STELLA_PAGE_CG,
    STELLA_PAGE_HISTORY,
    STELLA_PAGE_ENDING,
    STELLA_PAGE_CONFIRM,
} stella_page_t;

typedef struct {
    lv_obj_t *root;
    stella_image_t image;
    const lv_font_t *font16;
    const lv_font_t *font20;
    bool large;

    lv_obj_t *page_title;
    lv_obj_t *title_rows[8];
    int title_count;
    int title_selected;
    int title_first;            // 单列窗口首项下标

    lv_obj_t *page_game;
    lv_obj_t *name_box;
    lv_obj_t *page_label;   // 页码（面板内右上，独立 label）
    lv_obj_t *name_label;
    lv_obj_t *text_label;
    lv_obj_t *chapter_label;
    lv_obj_t *battery_label;
    lv_obj_t *choice_box;
    lv_obj_t *choice_prompt;
    lv_obj_t *choice_row;
    lv_obj_t *choice_label;

    lv_obj_t *page_list;
    lv_obj_t *list_title;
    lv_obj_t *list_rows[STELLA_LIST_ROWS];
    lv_obj_t *list_values[STELLA_LIST_ROWS];
    int list_count;
    int list_selected;

    lv_obj_t *page_cg;
    lv_obj_t *cg_caption;

    lv_obj_t *page_history;
    lv_obj_t *history_view;
    lv_obj_t *history_label;

    lv_obj_t *page_ending;
    lv_obj_t *ending_label;

    lv_obj_t *page_confirm;
    lv_obj_t *confirm_prompt;
    lv_obj_t *confirm_rows[2];
    int confirm_selected;
    lv_obj_t *notice_label;
    lv_obj_t *notice_text;

    char history_buf[STELLA_HISTORY_BYTES];
} stella_ui_t;

bool stella_ui_create(stella_ui_t *ui, lv_obj_t *root, uint16_t *canvas_pixels,
                      const lv_font_t *font16, const lv_font_t *font20);
void stella_ui_show(stella_ui_t *ui, stella_page_t page);

void stella_ui_set_title_rows(stella_ui_t *ui, const char *const *rows, int count, int selected);
void stella_ui_set_list(stella_ui_t *ui, const char *title, const char *const *rows,
                        const char *const *values, int count, int selected);
void stella_ui_list_select(stella_ui_t *ui, int selected);
void stella_ui_set_game(stella_ui_t *ui, const char *name, const char *text, bool large,
                        const char *chapter, int battery);
// 页码：独立 label；pages<=1 时隐藏。
void stella_ui_set_page(stella_ui_t *ui, int page, int pages);
void stella_ui_set_text(stella_ui_t *ui, const char *text);
void stella_ui_show_choices(stella_ui_t *ui, const char *prompt, const char *option);
void stella_ui_hide_choices(stella_ui_t *ui);
void stella_ui_set_battery(stella_ui_t *ui, int battery);
void stella_ui_set_cg_caption(stella_ui_t *ui, const char *caption);
void stella_ui_set_history(stella_ui_t *ui, const char *text);
void stella_ui_history_scroll(stella_ui_t *ui, int dy);
void stella_ui_set_ending(stella_ui_t *ui, const char *text);
void stella_ui_set_confirm(stella_ui_t *ui, const char *prompt, const char *yes,
                           const char *no, int selected);
void stella_ui_confirm_select(stella_ui_t *ui, int selected);
void stella_ui_notice(stella_ui_t *ui, const char *text);
void stella_ui_notice_hide(stella_ui_t *ui);
