// main/cafe/cafe_ui.h —— 320x240 横屏、19 逻辑页的 LVGL 界面。
#pragma once

#include "cafe_image.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define CAFE_UI_W 320
#define CAFE_UI_H 240
#define CAFE_PANEL_H 60
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
#define CAFE_NAME_PAD_H 8
#define CAFE_NAME_MAX_W 230
#define CAFE_NAME_MAX_CONTENT (CAFE_NAME_MAX_W - 2 * CAFE_NAME_PAD_H)
#define CAFE_PANEL_Y (CAFE_UI_H - CAFE_PANEL_H)
#define CAFE_LIST_MAX 16

typedef enum {
    CAFE_PAGE_START = 0,
    CAFE_PAGE_TITLE,
    CAFE_PAGE_GAME,
    CAFE_PAGE_CHOICE,
    CAFE_PAGE_MENU,
    CAFE_PAGE_SAVE,
    CAFE_PAGE_LOAD,
    CAFE_PAGE_SETTINGS,
    CAFE_PAGE_HISTORY,
    CAFE_PAGE_GALLERY,
    CAFE_PAGE_CG_LIST,
    CAFE_PAGE_RECALL,
    CAFE_PAGE_MUSIC,
    CAFE_PAGE_CHAPTER,
    CAFE_PAGE_TRANSITION,
    CAFE_PAGE_ENDING,
    CAFE_PAGE_ABOUT,
    CAFE_PAGE_CG,
    CAFE_PAGE_COUNT,
} cafe_page_t;

typedef enum {
    CAFE_LIST_TITLE = 0,
    CAFE_LIST_CHOICE,
    CAFE_LIST_MENU,
    CAFE_LIST_SAVE,
    CAFE_LIST_LOAD,
    CAFE_LIST_SETTINGS,
    CAFE_LIST_HISTORY,
    CAFE_LIST_GALLERY,
    CAFE_LIST_RECALL,
    CAFE_LIST_MUSIC,
    CAFE_LIST_CHAPTER,
    CAFE_LIST_ROUTE,
    CAFE_LIST_MODAL,
    CAFE_LIST_COUNT,
} cafe_list_t;

typedef struct {
    lv_obj_t *page;
    cafe_page_t page_id;      // CAFE_PAGE_COUNT = modal layer
    lv_obj_t *rows[CAFE_LIST_MAX];
    lv_obj_t *labels[CAFE_LIST_MAX];
    lv_obj_t *values[CAFE_LIST_MAX];
    int max_rows;
    int count;
    int selected;
    int row_h;
    int top;
    int x;
    int w;
    bool enabled[CAFE_LIST_MAX];
} cafe_list_ui_t;

typedef struct {
    lv_obj_t *pages[CAFE_PAGE_COUNT];
    lv_obj_t *art[CAFE_BAND_COUNT];
    lv_image_dsc_t art_dsc[CAFE_BAND_COUNT];
    cafe_bands_t *bands;

    lv_obj_t *brand;
    lv_obj_t *brand_sub;
    lv_obj_t *start_hint;
    lv_obj_t *title_box;

    lv_obj_t *panel;
    lv_obj_t *name;
    lv_obj_t *text;
    lv_obj_t *chapter;
    lv_obj_t *battery;
    lv_obj_t *notice;
    lv_obj_t *auto_label;
    lv_obj_t *choice_box;

    lv_obj_t *titles[CAFE_PAGE_COUNT];
    lv_obj_t *footers[CAFE_PAGE_COUNT];
    lv_obj_t *chapter_info;
    lv_obj_t *save_preview;

    lv_obj_t *cg_tiles[6];
    lv_obj_t *cg_tile_img[6];
    lv_obj_t *cg_tile_label[6];
    uint16_t cg_thumb_buf[6][32 * 18];
    lv_image_dsc_t cg_thumb_dsc[6];
    lv_obj_t *cg_index;
    lv_obj_t *cg_name;
    lv_obj_t *cg_caption;

    lv_obj_t *transition_eyebrow;
    lv_obj_t *transition_title;
    lv_obj_t *transition_sub;
    lv_obj_t *ending_eyebrow;
    lv_obj_t *ending_title;
    lv_obj_t *ending_body;
    lv_obj_t *about_view;
    lv_obj_t *about_text;

    lv_obj_t *modal_dim;
    lv_obj_t *modal_box;
    lv_obj_t *modal_title;
    lv_obj_t *modal_body;

    cafe_list_ui_t lists[CAFE_LIST_COUNT];
    const lv_font_t *font_small;
    const lv_font_t *font_large;

    lv_timer_t *typer;
    char typing_target[1024];
    char typing_shown[1024];
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
    bool large_text;
    bool thumb_failed;      // 上一次图库缩略图读取/解码失败
} cafe_ui_t;

bool cafe_ui_create(cafe_ui_t *ui);
void cafe_ui_show_page(cafe_ui_t *ui, cafe_page_t page);

void cafe_ui_set_brand(cafe_ui_t *ui, const char *title, const char *subtitle);
void cafe_ui_set_text(cafe_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char);
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
void cafe_ui_set_choice(cafe_ui_t *ui, const char *const *labels,
                        const bool *enabled, int count, int selected);

void cafe_ui_set_gallery_tiles(cafe_ui_t *ui, const char *const *labels,
                               const uint16_t *image_ids, const bool *unlocked,
                               int count, int selected);
void cafe_ui_set_cg(cafe_ui_t *ui, const char *index_text, const char *name,
                    const char *caption);
void cafe_ui_set_transition(cafe_ui_t *ui, const char *eyebrow, const char *title,
                            const char *subtitle);
// OP appreciation/full-video mode: hide the title overlay objects and keep only
// a short control hint below the 0..179 video area. Parent page stays transparent.
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
// 启动失败错误屏（调用方必须已持 LVGL 锁；CJK 字体 + 深色底 + 居中换行）。
void cafe_ui_show_fatal(const char *message);
