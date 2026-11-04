// main/gal_ui.h —— 320x240 横屏界面(作品专属:暗玫瑰色阅读器,不复用 demo 视觉壳)。
//
// 页面是固定的 LVGL 对象,app 只更新文本/光标/可见性;画面由 gal_image 画布承载。
// 所有函数要求在持有 bsp_lvgl_lock() 时调用。
#pragma once

#include "gal_image.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define GAL_UI_W 320
#define GAL_UI_H 240
#define GAL_UI_ART_H 150
// 阅读页最终布局：顶部 HP/页码，名字框在面板上方，面板 176..239。
#define GAL_UI_PANEL_Y 176
#define GAL_UI_PANEL_H 64
#define GAL_UI_TEXT_X 8
#define GAL_UI_TEXT_W 304
#define GAL_UI_TEXT_Y 4
#define GAL_UI_TEXT_H 57
#define GAL_UI_NAME_X 10
#define GAL_UI_NAME_Y 153
#define GAL_UI_NAME_PAD 3
#define GAL_UI_TOP_HP_X 8
#define GAL_UI_TOP_PAGE_X 232
#define GAL_UI_BATTERY_X 276
#define GAL_UI_CHOICE_NAME_X 218
// 画面区有效矩形 0..175；176 是画面底/面板顶/CG 字幕条顶。
#define GAL_UI_VIEW_BOTTOM 176
#define GAL_UI_CG_BAR_Y 176
#define GAL_UI_CG_BAR_H 64
// 阅读页硬指标（编译期可验证）：名字在框外、正文末行不出面板。
#define GAL_UI_LINES_TOP (GAL_UI_PANEL_Y + GAL_UI_TEXT_Y)
_Static_assert(GAL_UI_NAME_Y + 19 <= GAL_UI_PANEL_Y, "speaker must sit above the panel");
_Static_assert(GAL_UI_LINES_TOP + 3 * 19 <= GAL_UI_PANEL_Y + GAL_UI_PANEL_H,
               "last text line must stay inside the panel");
_Static_assert(GAL_IMAGE_BODY_BOTTOM == GAL_UI_PANEL_Y,
               "portrait bottom must match the panel top");
_Static_assert(GAL_UI_CG_BAR_Y == GAL_UI_PANEL_Y && GAL_UI_VIEW_BOTTOM == GAL_UI_PANEL_Y,
               "CG image bottom, panel top, and view bottom must match");
#define GAL_UI_LIST_ROWS 4

typedef enum {
    GAL_UI_TITLE = 0,
    GAL_UI_GAME,
    GAL_UI_LIST,
    GAL_UI_TEXT,
    GAL_UI_CG,
    GAL_UI_COUNT,
} gal_ui_page_t;

typedef struct {
    const char *text;
    uint8_t dim;      // 1 = 次要文字
} gal_ui_row_t;

typedef struct {
    struct _lv_obj_t *root;
    struct _lv_obj_t *title_box;
    struct _lv_obj_t *title_name;
    struct _lv_obj_t *title_ver;
    struct _lv_obj_t *title_rows[8];
    struct _lv_obj_t *title_labels[8];
    int title_count;

    struct _lv_obj_t *panel;
    struct _lv_obj_t *speaker;
    struct _lv_obj_t *hp;          // top bar HP
    struct _lv_obj_t *page_label;  // top bar page indicator
    struct _lv_obj_t *text;
    struct _lv_obj_t *hint;
    struct _lv_obj_t *battery;
    struct _lv_obj_t *battery_label;

    struct _lv_obj_t *list_page;
    struct _lv_obj_t *list_title;
    struct _lv_obj_t *list_sub;
    struct _lv_obj_t *list_rows[GAL_UI_LIST_ROWS];
    struct _lv_obj_t *list_labels[GAL_UI_LIST_ROWS];
    struct _lv_obj_t *list_footer;

    struct _lv_obj_t *text_page;
    struct _lv_obj_t *text_title;
    struct _lv_obj_t *text_body;
    struct _lv_obj_t *text_footer;

    struct _lv_obj_t *cg_bar;
    struct _lv_obj_t *cg_caption;
    struct _lv_obj_t *cg_hint;

    struct _lv_obj_t *notice;
    struct _lv_obj_t *notice_bg;
    gal_ui_page_t page;
    int notice_until_ms;
} gal_ui_t;

bool gal_ui_init(gal_ui_t *ui, struct _lv_obj_t *root);
void gal_ui_show(gal_ui_t *ui, gal_ui_page_t page);

void gal_ui_title(gal_ui_t *ui, const char *name, const char *sub,
                  const char *const *items, int count, int sel,
                  const char *battery);
void gal_ui_game(gal_ui_t *ui, const char *speaker, const char *text,
                 const char *hp, const char *page, const char *hint,
                 const char *battery);
void gal_ui_choice_name(gal_ui_t *ui, const char *speaker);
void gal_ui_list(gal_ui_t *ui, const char *title, const char *sub,
                 const gal_ui_row_t *rows, int count, int sel,
                 const char *footer);
void gal_ui_text(gal_ui_t *ui, const char *title, const char *body, const char *footer);
void gal_ui_cg(gal_ui_t *ui, const char *caption);
void gal_ui_notice(gal_ui_t *ui, const char *text, int now_ms, int duration_ms);
void gal_ui_tick(gal_ui_t *ui, int now_ms);
