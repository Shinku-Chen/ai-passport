// main/summer_ui.h —— LVGL 界面层：标题、正文、选项、列表、CG、结局、错误。
//
// 版面（横屏 320x240）：
//   y 0..179   画面画布（320x180）
//   y 180..239 正文带（60px，3 行 16px）
// 标题页把 logo 画在画面左上，菜单行叠在画面右侧；列表页整屏覆盖。
#pragma once

#include "summer_image.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// 对话框几何（320x240，画面区 320x180）：阅读页 176..240（64 高，原 90），
// CG 页只留 194..240（46 高）的字幕带，让 CG 尽量露出来。
#define SUMMER_PANEL_Y 176
#define SUMMER_PANEL_READ_H 64
#define SUMMER_PANEL_CG_Y 194
#define SUMMER_PANEL_CG_H 46

#define SUMMER_UI_ROWS 8
#define SUMMER_UI_ROW_TEXT 96
#define SUMMER_UI_ROW_META 96
#define SUMMER_UI_ROW_PREVIEW 96

typedef enum {
    SUMMER_PAGE_TITLE = 0,
    SUMMER_PAGE_GAME,
    SUMMER_PAGE_CHOICE,
    SUMMER_PAGE_MENU,
    SUMMER_PAGE_LIST,
    SUMMER_PAGE_CG,
    SUMMER_PAGE_ENDING,
    SUMMER_PAGE_ERROR,
} summer_page_t;

typedef enum {
    SUMMER_LIST_COMPACT = 0,   // 7 行 × 24px（菜单/设置/章节）
    SUMMER_LIST_CARD,          // 4 行 × 46px（存档/读档）
    SUMMER_LIST_HISTORY,       // 4 行 × 46px（历史）
    SUMMER_LIST_TITLE,         // 标题页右侧 8 行 × 21px
} summer_list_style_t;

typedef struct {
    const char *text;
    const char *meta;
    const char *preview;
    bool disabled;
} summer_ui_row_t;

typedef struct {
    lv_obj_t *root;
    summer_canvas_t canvas;
    const lv_font_t *font;

    lv_obj_t *chapter_label;
    lv_obj_t *battery_label;
    lv_obj_t *auto_label;
    lv_obj_t *notice_label;
    lv_obj_t *notice_text;
    lv_obj_t *title_progress_label;

    lv_obj_t *panel;
    lv_obj_t *speaker_box;
    lv_obj_t *speaker_label;
    lv_obj_t *text_label;
    lv_obj_t *page_label;

    lv_obj_t *cg_caption;
    lv_obj_t *cg_hint;

    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_sub;
    lv_obj_t *overlay_footer;
    lv_obj_t *rows[SUMMER_UI_ROWS];
    lv_obj_t *row_text[SUMMER_UI_ROWS];
    lv_obj_t *row_meta[SUMMER_UI_ROWS];
    lv_obj_t *row_preview[SUMMER_UI_ROWS];
    int row_count;
    int row_selected;
    int row_top;
    summer_list_style_t row_style;

    lv_obj_t *help_view;
    lv_obj_t *help_label;

    lv_obj_t *ending_title;
    lv_obj_t *ending_body;
    lv_obj_t *error_text;

    summer_page_t page;
    char title_progress[48];
} summer_ui_t;

// 建界面（需持有 LVGL 锁）。pixels 为 320x180 RGB565 画布缓冲。
bool summer_ui_create(summer_ui_t *ui, uint16_t *pixels, const lv_font_t *font);

void summer_ui_show_page(summer_ui_t *ui, summer_page_t page);
void summer_ui_set_chapter(summer_ui_t *ui, const char *text);
void summer_ui_set_battery(summer_ui_t *ui, int percent);
void summer_ui_set_auto(summer_ui_t *ui, bool auto_mode, bool fast_forward);
void summer_ui_set_panel_opacity(summer_ui_t *ui, int level);

// 正文页：speaker 可为空；text 已按行插入 '\n'；page 从 1 开始。
void summer_ui_set_game(summer_ui_t *ui, const char *speaker, const char *text,
                        int page, int pages);
void summer_ui_set_text(summer_ui_t *ui, const char *text);
void summer_ui_set_speaker(summer_ui_t *ui, const char *speaker);
void summer_ui_set_page(summer_ui_t *ui, int page, int pages);

// 列表/选项：rows 在调用期间有效；UI 会把文字复制进 LVGL。
void summer_ui_set_rows(summer_ui_t *ui, summer_list_style_t style, const char *title,
                        const char *subtitle, const char *footer, const summer_ui_row_t *rows,
                        int count, int selected);
// 窗口式列表：rows 只提供当前可见行（最多 7/3 行），first 是窗口在总表中的下标。
void summer_ui_set_window(summer_ui_t *ui, summer_list_style_t style, const char *title,
                          const char *subtitle, const char *footer, const summer_ui_row_t *rows,
                          int visible, int first, int total, int selected);
void summer_ui_set_choice_rows(summer_ui_t *ui, const summer_ui_row_t *rows, int count,
                               int selected);

// 标题页：logo 已由 app 画进画布；这里只更新菜单与进度文字。
void summer_ui_set_title_rows(summer_ui_t *ui, const summer_ui_row_t *rows, int count,
                              int selected, const char *progress);

// CG 页：caption 两行（标签 + 差分）。
void summer_ui_set_cg(summer_ui_t *ui, const char *caption, const char *hint);

void summer_ui_set_ending(summer_ui_t *ui, const char *title, const char *body);
void summer_ui_set_error(summer_ui_t *ui, const char *text);
void summer_ui_set_notice(summer_ui_t *ui, const char *text);
void summer_ui_hide_notice(summer_ui_t *ui);
void summer_ui_set_help(summer_ui_t *ui, const char *text);
void summer_ui_scroll(summer_ui_t *ui, int dy);

// 宿主机测试：当前可见行数/选中行。
int summer_ui_row_count(const summer_ui_t *ui);
int summer_ui_row_selected(const summer_ui_t *ui);
