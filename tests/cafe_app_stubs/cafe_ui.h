#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cafe_image.h"
#define CAFE_UI_W 320
#define CAFE_UI_H 240
#define CAFE_PANEL_H 60
#define CAFE_PANEL_Y 180
#define CAFE_TEXT_SMALL_UNITS 36
#define CAFE_TEXT_SMALL_LINES 3
#define CAFE_TEXT_LARGE_UNITS 28
#define CAFE_TEXT_LARGE_LINES 2
#define CAFE_TEXT_X 5
#define CAFE_TEXT_W 310
#define CAFE_TEXT_SMALL_Y 0
#define CAFE_TEXT_LARGE_Y 6
#define CAFE_AUTO_X 264
#define CAFE_AUTO_Y 154
#define CAFE_AUTO_W 47
#define CAFE_AUTO_H 23
#define CAFE_LIST_MAX 16
typedef enum { CAFE_PAGE_START=0,CAFE_PAGE_TITLE,CAFE_PAGE_GAME,CAFE_PAGE_CHOICE,CAFE_PAGE_MENU,
 CAFE_PAGE_SAVE,CAFE_PAGE_LOAD,CAFE_PAGE_SETTINGS,CAFE_PAGE_HISTORY,CAFE_PAGE_GALLERY,
 CAFE_PAGE_CG_LIST,CAFE_PAGE_RECALL,CAFE_PAGE_MUSIC,CAFE_PAGE_CHAPTER,CAFE_PAGE_TRANSITION,
 CAFE_PAGE_ENDING,CAFE_PAGE_ABOUT,CAFE_PAGE_CG,CAFE_PAGE_COUNT } cafe_page_t;
typedef enum { CAFE_LIST_TITLE=0,CAFE_LIST_CHOICE,CAFE_LIST_MENU,CAFE_LIST_SAVE,CAFE_LIST_LOAD,
 CAFE_LIST_SETTINGS,CAFE_LIST_HISTORY,CAFE_LIST_GALLERY,CAFE_LIST_RECALL,CAFE_LIST_MUSIC,
 CAFE_LIST_CHAPTER,CAFE_LIST_ROUTE,CAFE_LIST_MODAL,CAFE_LIST_COUNT } cafe_list_t;
typedef struct cafe_ui_t {
    int page;
    char speaker[64];
    char text[1024];
    bool typing;
    bool auto_mode;
    int rows_count;
    int rows_selected;
    int choice_count;
    int choice_selected;
    int notice_seq;
    int art_visible;
    bool thumb_failed;
} cafe_ui_t;
bool cafe_ui_create(cafe_ui_t *ui);
void cafe_ui_show_page(cafe_ui_t *ui, cafe_page_t page);
void cafe_ui_set_brand(cafe_ui_t *ui, const char *title, const char *subtitle);
void cafe_ui_set_text(cafe_ui_t *ui, const char *speaker, const char *text, uint32_t ms);
void cafe_ui_show_full_text(cafe_ui_t *ui);
bool cafe_ui_typing(const cafe_ui_t *ui);
void cafe_ui_set_chapter(cafe_ui_t *ui, const char *text);
void cafe_ui_set_battery(cafe_ui_t *ui, int percent);
void cafe_ui_set_notice(cafe_ui_t *ui, const char *text);
void cafe_ui_set_mode(cafe_ui_t *ui, bool auto_play);
void cafe_ui_set_font_large(cafe_ui_t *ui, bool large);
void cafe_ui_set_panel_transparency(cafe_ui_t *ui, uint8_t percent);
void cafe_ui_set_rows(cafe_ui_t *ui, cafe_list_t list, const char *const *labels,
                      const char *const *values, int count, int selected);
void cafe_ui_set_row_enabled(cafe_ui_t *ui, cafe_list_t list, int row, bool enabled);
void cafe_ui_select_row(cafe_ui_t *ui, cafe_list_t list, int selected);
void cafe_ui_set_page_title(cafe_ui_t *ui, cafe_page_t page, const char *title);
void cafe_ui_set_footer(cafe_ui_t *ui, cafe_page_t page, const char *text);
void cafe_ui_set_save_preview(cafe_ui_t *ui, const char *text);
void cafe_ui_set_chapter_info(cafe_ui_t *ui, const char *text);
void cafe_ui_set_choice(cafe_ui_t *ui, const char *const *labels, const bool *enabled,
                        int count, int selected);
void cafe_ui_set_gallery_tiles(cafe_ui_t *ui, const char *const *labels,
                               const uint16_t *image_ids, const bool *unlocked,
                               int count, int selected);
void cafe_ui_set_cg(cafe_ui_t *ui, const char *index_text, const char *name,
                    const char *caption);
void cafe_ui_set_transition(cafe_ui_t *ui, const char *eyebrow, const char *title,
                            const char *subtitle);
void cafe_ui_set_transition_op(cafe_ui_t *ui, const char *hint);
void cafe_ui_set_ending(cafe_ui_t *ui, const char *eyebrow, const char *title,
                        const char *body);
void cafe_ui_set_about(cafe_ui_t *ui, const char *text);
void cafe_ui_scroll_about(cafe_ui_t *ui, int dy);
void cafe_ui_show_modal(cafe_ui_t *ui, const char *title, const char *body,
                        const char *const *actions, int count, int selected);
void cafe_ui_hide_modal(cafe_ui_t *ui);
void cafe_ui_bind_art(cafe_ui_t *ui, cafe_bands_t *bands);
void cafe_ui_art_visible(cafe_ui_t *ui, bool visible);
void cafe_ui_art_invalidate(cafe_ui_t *ui);
