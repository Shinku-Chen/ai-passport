// main/steins0_ui.h —— 320x240 横屏作品界面(标题/正文/选择/列表/鉴赏/CG/历史/结局)。
#pragma once

#include "lvgl.h"
#include "steins0_image.h"

#include <stdbool.h>
#include <stdint.h>

#define SG0_LIST_ROWS 8

typedef enum {
    SG0_PAGE_TITLE = 0,
    SG0_PAGE_GAME,
    SG0_PAGE_LIST,
    SG0_PAGE_CG,
    SG0_PAGE_HISTORY,
    SG0_PAGE_ABOUT,
    SG0_PAGE_ENDING,
    SG0_PAGE_CONFIRM,
    SG0_PAGE_PHONE,
    SG0_PAGE_PHONE_THREAD,
    SG0_PAGE_CARD,
} steins0_page_t;

typedef struct {
    lv_obj_t *root;
    steins0_image_t image;
    const lv_font_t *font;
    uint16_t page_ready_mask;

    lv_obj_t *page_title;
    lv_obj_t *title_label;
    lv_obj_t *title_rows[6];
    lv_obj_t *title_battery;
    int title_count;
    int title_selected;

    lv_obj_t *page_game;
    lv_obj_t *game_chapter;
    lv_obj_t *game_battery;
    lv_obj_t *game_rine;
    lv_obj_t *speaker_label;
    lv_obj_t *text_lines[3];
    lv_obj_t *page_label;
    lv_obj_t *choice_box;
    lv_obj_t *choice_heading;
    lv_obj_t *choice_rows[2];
    int choice_count;
    int choice_selected;

    lv_obj_t *page_list;
    lv_obj_t *list_title;
    lv_obj_t *list_rows[SG0_LIST_ROWS];
    lv_obj_t *list_values[SG0_LIST_ROWS];
    lv_obj_t *list_footer;
    int list_count;
    int list_selected;

    lv_obj_t *page_cg;
    lv_obj_t *cg_caption;
    lv_obj_t *cg_variant;

    lv_obj_t *page_history;
    lv_obj_t *history_view;
    lv_obj_t *history_label;
    lv_obj_t *history_hint;

    lv_obj_t *page_about;
    lv_obj_t *about_view;
    lv_obj_t *about_label;

    lv_obj_t *page_ending;
    lv_obj_t *ending_title;
    lv_obj_t *ending_label;
    lv_obj_t *ending_hint;

    lv_obj_t *page_confirm;
    lv_obj_t *confirm_prompt;
    lv_obj_t *confirm_rows[2];
    int confirm_selected;

    lv_obj_t *page_phone;
    lv_obj_t *phone_title;
    lv_obj_t *phone_sub;
    lv_obj_t *phone_page_label;
    lv_obj_t *phone_rows[SG0_LIST_ROWS];
    lv_obj_t *phone_values[SG0_LIST_ROWS];
    lv_obj_t *phone_footer;
    int phone_count;
    int phone_selected;

    lv_obj_t *page_phone_thread;
    lv_obj_t *phone_thread_header;
    lv_obj_t *phone_thread_sender;
    lv_obj_t *phone_lines[3];
    lv_obj_t *phone_thread_hint;
    steins0_image_t stamp;
    uint16_t *stamp_pixels;

    lv_obj_t *page_card;
    lv_obj_t *card_title;

    lv_obj_t *notice_box;
    lv_obj_t *notice_text;
} steins0_ui_t;

bool steins0_ui_create(steins0_ui_t *ui, lv_obj_t *root, uint16_t *pixels,
                       uint16_t *stamp_pixels, const lv_font_t *font);
bool steins0_ui_show(steins0_ui_t *ui, steins0_page_t page);

void steins0_ui_title(steins0_ui_t *ui, const char *const *rows, int count, int selected);
void steins0_ui_title_battery(steins0_ui_t *ui, int percent);
void steins0_ui_game(steins0_ui_t *ui, const char *chapter, int battery);
void steins0_ui_game_text(steins0_ui_t *ui, const char *speaker, const char *const lines[3],
                          int line_count);
void steins0_ui_game_page(steins0_ui_t *ui, int page, int pages);
void steins0_ui_choice(steins0_ui_t *ui, const char *heading, const char *const *rows,
                       int count, int selected);
void steins0_ui_choice_hide(steins0_ui_t *ui);
void steins0_ui_list(steins0_ui_t *ui, const char *title, const char *const *rows,
                     const char *const *values, int count, int selected,
                     const char *footer);
void steins0_ui_cg(steins0_ui_t *ui, const char *caption, const char *variant);
void steins0_ui_history(steins0_ui_t *ui, const char *text);
void steins0_ui_about(steins0_ui_t *ui, const char *text);
void steins0_ui_ending(steins0_ui_t *ui, const char *title, const char *body,
                       const char *hint);
void steins0_ui_confirm(steins0_ui_t *ui, const char *prompt, const char *yes,
                        const char *no, int selected);
void steins0_ui_phone(steins0_ui_t *ui, const char *title, const char *sub,
                      const char *page_label, const char *const *rows,
                      const char *const *values, int count, int selected, const char *footer);
void steins0_ui_phone_thread(steins0_ui_t *ui, const char *header, const char *sender,
                             const char *const lines[3], int line_count, bool stamp,
                             const char *hint);
void steins0_ui_card(steins0_ui_t *ui, const char *title);
void steins0_ui_game_rine(steins0_ui_t *ui, const char *text);
void steins0_ui_notice(steins0_ui_t *ui, const char *text);
void steins0_ui_notice_hide(steins0_ui_t *ui);
void steins0_ui_battery(steins0_ui_t *ui, int percent);

// 滚动类页面(历史/关于)的上下移动,单位像素。
void steins0_ui_scroll(steins0_ui_t *ui, steins0_page_t page, int delta);
