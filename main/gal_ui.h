// main/gal_ui.h —— GalGod 的横屏界面层(320x240)。
//
// 版面:
//   y 0..149    画面区:背景/CG + 立绘(gal_image 的两块画布)
//   y 128..149  说话人名牌(画面区左下角)
//   y 150..239  文本框(90px,半透明;后面是画面第 150..239 行)
// 全屏页面(标题/菜单/存读档/设置/历史/章节/鉴赏/CG 浏览/结局)盖住整屏。
//
// 本模块只负责"把给定内容画出来",剧情走向在 gal_app.c。
#pragma once

#include "gal_image.h"
#include "gal_model.h"
#include "gal_pack.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_UI_W 320
#define GAL_UI_H 240
#define GAL_BOX_H 88           // 152..240（原 100：减 12px，正文 4 行仍不出框）
#define GAL_BOX_Y (GAL_UI_H - GAL_BOX_H)
#define GAL_TEXT_X 8
#define GAL_TEXT_W (GAL_UI_W - 2 * GAL_TEXT_X)
#define GAL_LINE_H_16 19
#define GAL_LINE_H_20 23
#define GAL_TEXT_LINES_16 4
#define GAL_TEXT_LINES_20 3
#define GAL_TEXT_BUFFER 512
#define GAL_SLOT_COUNT 4          // 3 个手动槽 + 1 个自动
#define GAL_GALLERY_COLS 4
#define GAL_GALLERY_ROWS 2
#define GAL_GALLERY_PER_PAGE (GAL_GALLERY_COLS * GAL_GALLERY_ROWS)
#define GAL_LIST_ROWS 6

// 中文字体子集(assets/fonts/gal_cjk_*.c)。
extern const lv_font_t gal_cjk_16;
extern const lv_font_t gal_cjk_20;

typedef enum {
    GAL_PAGE_TITLE = 0,
    GAL_PAGE_GAME,
    GAL_PAGE_MENU,
    GAL_PAGE_SLOTS,
    GAL_PAGE_SETTINGS,
    GAL_PAGE_HISTORY,
    GAL_PAGE_CHAPTERS,
    GAL_PAGE_GALLERY,
    GAL_PAGE_CGVIEW,
    GAL_PAGE_ENDING,
    GAL_PAGE_BROWSE,
    GAL_PAGE_ENDINGS,
} gal_page_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *rows[GAL_LIST_ROWS];
    lv_obj_t *values[GAL_LIST_ROWS];
    int count;
    int selected;
    int top;
    int row_h;
    int visible;
    const lv_font_t *font;
} gal_list_t;

typedef struct {
    gal_image_t image;

    lv_obj_t *root;
    lv_obj_t *pages[GAL_PAGE_ENDINGS + 1];

    // 正文页
    lv_obj_t *box;
    lv_obj_t *name;
    lv_obj_t *page_label;   // 页码（面板内右上，独立 label）
    lv_obj_t *text;
    lv_obj_t *chapter;
    lv_obj_t *battery;
    lv_obj_t *mode;
    lv_obj_t *notice;
    uint32_t notice_until_ms;

    // 选项
    lv_obj_t *choice_box;
    lv_obj_t *choice_rows[5];
    lv_obj_t *choice_labels[5];
    int choice_count;
    int choice_sel;

    // 标题
    lv_obj_t *title_label;
    lv_obj_t *title_sub;
    lv_obj_t *title_menu_text[5];
    int title_sel;

    // 通用列表页
    gal_list_t menu;        // 游戏内菜单
    gal_list_t slots;
    gal_list_t settings;
    gal_list_t browse;
    gal_list_t endings_list;

    // 历史
    lv_obj_t *history_box;
    lv_obj_t *history_rows[48];
    int history_count;

    // 章节
    lv_obj_t *chapter_box;
    lv_obj_t *chapter_rows[18];
    int chapter_scroll;

    // 鉴赏
    lv_obj_t *gallery_box;
    lv_obj_t *gallery_tiles[GAL_GALLERY_PER_PAGE];
    lv_obj_t *gallery_labels[GAL_GALLERY_PER_PAGE];
    lv_obj_t *gallery_page_label;
    int gallery_page;
    int gallery_sel;

    // CG 浏览
    lv_obj_t *cg_caption;
    lv_obj_t *cg_hint;

    // 结局
    lv_obj_t *ending_eyebrow;
    lv_obj_t *ending_title;
    lv_obj_t *ending_text;
    lv_obj_t *ending_hint;

    // 二次确认
    lv_obj_t *confirm_box;
    lv_obj_t *confirm_text;
    lv_obj_t *confirm_rows[2];
    lv_obj_t *confirm_labels[2];
    bool confirm_active;

    // 章节过场
    lv_obj_t *transition_box;
    lv_obj_t *transition_title;
    lv_obj_t *transition_sub;

    const lv_font_t *font_small;
    const lv_font_t *font_large;
    bool large_font;

    lv_timer_t *typing_timer;
    char typing_target[GAL_TEXT_BUFFER];
    char typing_shown[GAL_TEXT_BUFFER];
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_ms;
    bool typing_active;
} gal_ui_t;

bool gal_ui_create(gal_ui_t *ui, uint16_t *art_px, uint16_t *strip_px,
                   uint8_t *scratch, uint32_t scratch_size,
                   const lv_font_t *font_small, const lv_font_t *font_large);

void gal_ui_show_page(gal_ui_t *ui, gal_page_t page);
gal_page_t gal_ui_page(const gal_ui_t *ui);

// 正文
void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *page_text,
                     uint32_t ms_per_char);
// 页码：独立 label；pages<=1 时隐藏。
void gal_ui_set_page(gal_ui_t *ui, int page, int pages);
void gal_ui_show_full_text(gal_ui_t *ui);
bool gal_ui_typing(const gal_ui_t *ui);
void gal_ui_set_chapter(gal_ui_t *ui, const char *text);
void gal_ui_set_battery(gal_ui_t *ui, int percent);
void gal_ui_set_mode(gal_ui_t *ui, bool auto_play);
void gal_ui_toast(gal_ui_t *ui, const char *text, uint32_t now_ms);
void gal_ui_tick(gal_ui_t *ui, uint32_t now_ms);

// 选项
// options 是"可显示的选项下标"(已按条件过滤),count <= 5。
void gal_ui_set_choice(gal_ui_t *ui, const gal_pack_t *pack, const uint16_t *options,
                       int count, int selected);
void gal_ui_hide_choice(gal_ui_t *ui);

// 列表页
void gal_ui_set_list(gal_ui_t *ui, gal_list_t *list, gal_page_t page, const char *title,
                     const char *const *labels, const char *const *values, int count,
                     int selected, int row_h);
void gal_ui_list_select(gal_ui_t *ui, gal_list_t *list, int selected);
void gal_ui_list_set_value(gal_ui_t *ui, gal_list_t *list, int row, const char *value);

// 历史
void gal_ui_set_history(gal_ui_t *ui, const gal_pack_t *pack, const uint16_t *lines,
                        int count);
void gal_ui_history_scroll(gal_ui_t *ui, int delta);

// 章节
void gal_ui_set_chapters(gal_ui_t *ui, const gal_pack_t *pack, const gal_unlock_t *unlock,
                         int reachable);
void gal_ui_chapters_scroll(gal_ui_t *ui, int delta);

// 鉴赏
void gal_ui_set_gallery(gal_ui_t *ui, const gal_pack_t *pack, const gal_unlock_t *unlock,
                        int page, int selected);
void gal_ui_gallery_select(gal_ui_t *ui, int selected);
void gal_ui_set_cg_caption(gal_ui_t *ui, const char *text);

// 二次确认(覆盖存档)
void gal_ui_show_confirm(gal_ui_t *ui, const char *text, int selected);
void gal_ui_confirm_select(gal_ui_t *ui, int selected);
void gal_ui_hide_confirm(gal_ui_t *ui);

// 结局与过场
void gal_ui_set_ending(gal_ui_t *ui, const char *eyebrow, const char *title,
                       const char *text);
void gal_ui_set_transition(gal_ui_t *ui, const char *title, const char *sub);
void gal_ui_hide_transition(gal_ui_t *ui);

// 对话框不透明度(55..95%)。
void gal_ui_apply_panel_alpha(gal_ui_t *ui, uint8_t percent);

// 字号切换后重新排版当前页
void gal_ui_set_large_font(gal_ui_t *ui, bool large);
bool gal_ui_large_font(const gal_ui_t *ui);
int gal_ui_units_per_line(const gal_ui_t *ui);
int gal_ui_lines_per_page(const gal_ui_t *ui);
