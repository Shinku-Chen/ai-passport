// main/clannad_ui.h —— 《CLANNAD》横屏 320x240 作品专属界面（LVGL 9）。
//
// 版面固定为：上方 320x168 画面（背景 224x168 居中 + 最多 3 个立绘），
// 下方 320x72 对白带（姓名/章节/页码 + 3 行 16px 正文）。列表/菜单/选项/CG
// 全屏覆盖。所有 lv_* 调用必须由调用方持有 bsp_lvgl_lock()。
#pragma once

#include "clannad_image.h"
#include "clannad_text.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_font_t lv_font_t;

typedef enum {
    CLAN_UI_TITLE = 0,
    CLAN_UI_GAME,
    CLAN_UI_CHOICE,
    CLAN_UI_LIST,
    CLAN_UI_CG,
    CLAN_UI_MESSAGE,
    CLAN_UI_PAGE_COUNT,
} clannad_ui_page_t;

#define CLAN_UI_TITLE_ROWS 7
#define CLAN_UI_CHOICE_ROWS 4
#define CLAN_UI_LIST_ROWS 5

typedef struct {
    char main[96];
    char sub[80];
    char value[32];
} clannad_ui_row_t;

typedef struct {
    clannad_image_t image;
    const lv_font_t *font;

    lv_obj_t *pages[CLAN_UI_PAGE_COUNT];

    lv_obj_t *title_panel;
    lv_obj_t *title_sel;
    lv_obj_t *title_rows[CLAN_UI_TITLE_ROWS];
    lv_obj_t *title_progress;
    lv_obj_t *title_hint;

    lv_obj_t *game_panel;
    lv_obj_t *speaker;
    lv_obj_t *chapter;
    lv_obj_t *page_mark;
    lv_obj_t *lines[CLAN_TEXT_LINES];

    lv_obj_t *choice_title;
    lv_obj_t *choice_sel;
    lv_obj_t *choice_rows[CLAN_UI_CHOICE_ROWS];
    lv_obj_t *choice_hint;

    lv_obj_t *list_title;
    lv_obj_t *list_sel;
    lv_obj_t *list_rows[CLAN_UI_LIST_ROWS][3];
    lv_obj_t *list_hint;

    lv_obj_t *cg_caption;
    lv_obj_t *cg_hint;

    lv_obj_t *msg_title;
    lv_obj_t *msg_body;
    lv_obj_t *msg_hint;

    lv_obj_t *notice;

    clannad_ui_page_t page;
    char row_main[CLAN_UI_LIST_ROWS][96];
    char row_sub[CLAN_UI_LIST_ROWS][80];
    char title_main[CLAN_UI_TITLE_ROWS][96];
} clannad_ui_t;

bool clannad_ui_create(clannad_ui_t *ui, lv_obj_t *parent, uint16_t *canvas_pixels,
                       const lv_font_t *font16);
void clannad_ui_show(clannad_ui_t *ui, clannad_ui_page_t page);

void clannad_ui_title(clannad_ui_t *ui, const char *const *labels, int count,
                      int selected, const char *progress, const char *hint);
void clannad_ui_game(clannad_ui_t *ui, const char *speaker, const char *line0,
                     const char *line1, const char *line2, const char *chapter,
                     const char *page_mark);
void clannad_ui_choice(clannad_ui_t *ui, const char *title, const char *const *rows,
                       int count, int selected, int top, const char *hint);
void clannad_ui_list(clannad_ui_t *ui, const char *title, const clannad_ui_row_t *rows,
                     int count, int selected, int top, const char *hint);
void clannad_ui_cg(clannad_ui_t *ui, const char *caption, const char *hint);
void clannad_ui_message(clannad_ui_t *ui, const char *title, const char *body,
                        const char *hint);
void clannad_ui_notice(clannad_ui_t *ui, const char *text);

// 供宿主薄桩回归：当前页面与可见行文本。
clannad_ui_page_t clannad_ui_page(const clannad_ui_t *ui);
const char *clannad_ui_row_main(const clannad_ui_t *ui, int row);
const char *clannad_ui_row_sub(const clannad_ui_t *ui, int row);
