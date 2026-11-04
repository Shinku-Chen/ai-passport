// main/summer_app.h —— 阅读器应用状态机（按键、页面、自动阅读、存读档、鉴赏）。
#pragma once

#include "summer_model.h"
#include "summer_store.h"
#include "summer_text.h"
#include "summer_ui.h"

#include <stdbool.h>
#include <stdint.h>

#define SUMMER_HISTORY_MAX 12
#define SUMMER_CHOICE_MAX 24
#define SUMMER_MAP_MAX 40
#define SUMMER_CHAPTER_MAX 40

typedef struct {
    uint8_t btn;
    uint8_t ev;
} summer_key_t;

typedef enum {
    APP_PAGE_TITLE = 0,
    APP_PAGE_GAME,
    APP_PAGE_CHOICE,
    APP_PAGE_MAP,
    APP_PAGE_MENU,
    APP_PAGE_SLOTS,
    APP_PAGE_SETTINGS,
    APP_PAGE_HISTORY,
    APP_PAGE_GALLERY,
    APP_PAGE_CG,
    APP_PAGE_ROUTES,
    APP_PAGE_CHAPTERS,
    APP_PAGE_MANUAL,
    APP_PAGE_ENDING,
    APP_PAGE_ERROR,
    APP_PAGE_CONFIRM,
} summer_app_page_t;

typedef enum {
    SUMMER_SLOT_SAVE = 0,
    SUMMER_SLOT_LOAD,
} summer_slot_role_t;

typedef enum {
    SUMMER_CONFIRM_NONE = 0,
    SUMMER_CONFIRM_OVERWRITE,
    SUMMER_CONFIRM_TITLE,
} summer_confirm_kind_t;

typedef enum {
    SUMMER_ACT_START = 0,
    SUMMER_ACT_RESTART,
    SUMMER_ACT_ALKA,
    SUMMER_ACT_POCKET,
    SUMMER_ACT_CONTINUE,
    SUMMER_ACT_LOAD,
    SUMMER_ACT_GALLERY,
    SUMMER_ACT_OPTIONS,
    SUMMER_ACT_MANUAL,
} summer_title_action_t;

typedef struct {
    bool occupied;
    bool valid;
    char meta[64];
    char preview[96];
} summer_slot_meta_t;

typedef struct {
    summer_pack_t pack;
    summer_ui_t ui;
    summer_store_t store;
    summer_vm_t vm;
    bool vm_valid;

    summer_app_page_t page;
    summer_app_page_t menu_return;
    summer_app_page_t slots_return;
    summer_app_page_t settings_return;
    summer_app_page_t history_return;
    summer_app_page_t gallery_return;
    summer_app_page_t routes_return;
    summer_app_page_t chapters_return;
    summer_app_page_t manual_return;
    summer_app_page_t confirm_return;
    summer_confirm_kind_t confirm_kind;
    int confirm_sel;

    uint8_t suppress_click;      // bit per bsp_btn_t

    uint16_t pixels[SUMMER_ART_W * SUMMER_ART_H];
    bool at_choice;
    bool at_map;
    bool typing;
    size_t typing_pos;
    bool auto_mode;
    bool ff;
    bool ended;
    bool image_error;

    uint32_t idle_ms;
    uint32_t battery_ms;
    uint32_t notice_ms;
    uint32_t type_accum;
    uint32_t auto_accum;
    uint32_t ff_accum;
    uint32_t save_accum;
    uint32_t progress_accum;
    bool autosave_dirty;
    int battery;
    char notice[64];

    uint32_t page_offsets[SUMMER_MAX_PAGES + 1];
    int page_count;
    int page_index;
    char full_text[SUMMER_TEXT_CAP];
    char page_text[SUMMER_PAGE_TEXT_CAP];
    char speaker[SUMMER_MAX_NAME];
    char chapter_label[SUMMER_TEXT_CAP];
    char display_buf[SUMMER_PAGE_TEXT_CAP];

    uint16_t shown_bg;
    uint16_t shown_body;

    int choice_group;
    int choice_count;
    int choice_sel;
    int choice_items[SUMMER_CHOICE_MAX];
    char choice_labels[SUMMER_CHOICE_MAX][96];
    summer_ui_row_t choice_rows[SUMMER_CHOICE_MAX];

    int map_group;
    int map_count;
    int map_sel;
    summer_map_choice_t map_choices[SUMMER_MAP_MAX];
    char map_labels[SUMMER_MAP_MAX][96];
    char map_hints[SUMMER_MAP_MAX][96];
    summer_ui_row_t map_rows[SUMMER_MAP_MAX];

    int title_sel;
    int title_count;
    uint8_t title_actions;
    bool title_has_auto;
    uint8_t title_action_ids[SUMMER_UI_ROWS];
    char title_labels[SUMMER_UI_ROWS][40];
    summer_ui_row_t title_rows[SUMMER_UI_ROWS];

    int menu_sel;
    char menu_labels[8][40];
    summer_ui_row_t menu_rows[8];

    int settings_sel;
    char settings_labels[4][40];
    char settings_values[4][32];
    summer_ui_row_t settings_rows[4];

    int gallery_sel;
    int cg_group;
    int cg_variant;
    char gallery_labels[7][40];
    char gallery_meta[7][40];
    summer_ui_row_t gallery_rows[7];

    int route_sel;
    int route_count;
    int route_ids[14];
    char route_labels[7][40];
    char route_meta[7][40];
    summer_ui_row_t route_rows[7];

    int chapter_sel;
    int chapter_count;
    int chapter_scene[SUMMER_CHAPTER_MAX];
    char chapter_labels[7][40];
    char chapter_meta[7][40];
    summer_ui_row_t chapter_rows[7];

    int history_sel;
    int history_count;
    int history_head;
    char history_text[SUMMER_HISTORY_MAX][160];
    char history_speaker[SUMMER_HISTORY_MAX][32];
    char history_rows_text[SUMMER_HISTORY_MAX][96];
    char history_rows_meta[SUMMER_HISTORY_MAX][32];
    summer_ui_row_t history_rows[SUMMER_HISTORY_MAX];

    int slot_sel;
    int slot_pending;
    summer_slot_role_t slot_role;
    summer_slot_meta_t slot_meta[SUMMER_SLOT_COUNT];
    char slot_labels[3][24];
    char slot_meta_text[3][64];
    char slot_preview[3][96];
    summer_ui_row_t slot_rows[3];

    uint16_t last_text_scene;
    uint32_t last_text_op;
    char ending_body[128];

    bool init_ok;
    bool pack_ok;
} summer_app_t;

// 在 bsp_lvgl_lock 下初始化（内部会加锁）。
bool summer_app_init(summer_app_t *app, const uint8_t *pack_blob, uint32_t pack_size);
void summer_app_key(summer_app_t *app, const summer_key_t *key);
void summer_app_tick(summer_app_t *app, uint32_t elapsed_ms);
uint32_t summer_app_idle_ms(const summer_app_t *app);
void summer_app_flush(summer_app_t *app);
void summer_app_show_sleeping(summer_app_t *app);
