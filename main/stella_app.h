// main/stella_app.h —— 应用状态机：标题/正文/菜单/存档/设置/章节/CG/历史/结局。
#pragma once

#include "stella_model.h"
#include "stella_pack.h"
#include "stella_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;
    uint8_t ev;
} stella_key_t;

typedef struct {
    uint8_t large;      // 0 = 16px, 1 = 20px
    uint8_t speed;      // 0 slow, 1 medium, 2 fast, 3 instant
    uint8_t auto_wait;  // 0 slow, 1 normal, 2 fast
} stella_settings_t;

#define STELLA_SLOT_COUNT 7
#define STELLA_HISTORY_COUNT 8
#define STELLA_HISTORY_TEXT 192
#define STELLA_OPS_CAP 28672
#define STELLA_NOTICE_BYTES 64

typedef struct {
    char speaker[32];
    char text[STELLA_HISTORY_TEXT];
} stella_history_t;

typedef struct {
    stella_pack_t pack;
    stella_ui_t ui;
    uint16_t ui_pixels[STELLA_SCREEN_W * STELLA_SCREEN_H];
    stella_settings_t settings;

    stella_page_t page;
    stella_page_t return_page;
    stella_page_t confirm_return;
    stella_page_t slots_return;
    stella_page_t history_return;

    uint16_t scene_index;
    stella_scene_t scene;
    const uint8_t *ops;  // 直接指向 Flash 内的场景 op 流（24 B 定长）
    uint16_t op_count;
    uint16_t op;
    uint8_t mode;       // STELLA_MODE_STORY / STELLA_MODE_CHAPTER
    uint16_t page_no;
    uint16_t page_count;
    uint32_t page_off[STELLA_MAX_PAGES + 1];
    char op_text[STELLA_TEXT_BYTES];
    char page_text[STELLA_TEXT_BYTES];
    char speaker[64];
    char chapter[64];

    bool at_choice;
    bool ended;
    bool scene_image_failed;
    uint16_t choice_target;
    uint16_t scene_asset_bg;
    uint16_t scene_bodies[3];
    uint8_t scene_body_count;

    bool typing;
    uint32_t typing_pos;
    bool fast_forward;
    bool suppress_click;   // 长按后驱动补发的 CLICK 要吞掉
    uint8_t long_btn;
    bool auto_mode;
    uint32_t ff_accum;
    uint32_t auto_accum;

    uint32_t idle_ms;
    uint32_t battery_ms;
    uint32_t progress_ms;
    bool progress_dirty;
    bool unlock_dirty;
    uint32_t unlock_retry_ms;
    uint8_t unlock_retry_count;
    int battery;

    stella_save_t checkpoint;
    bool checkpoint_valid;
    stella_unlock_t unlock;
    stella_save_t autosave;
    bool have_auto;
    stella_save_t slots[STELLA_SLOT_COUNT];
    bool slot_valid[STELLA_SLOT_COUNT];

    int title_sel;
    int menu_sel;
    int settings_sel;
    int slot_sel;
    int chapter_sel;
    int gallery_sel;
    int confirm_sel;
    int chapter_page;
    int gallery_page;
    uint16_t cg_group;
    uint16_t cg_variant;
    bool cg_decode_failed;

    int slot_action;   // 0 save, 1 load
    int slot_pending;

    stella_history_t history[STELLA_HISTORY_COUNT];
    int history_count;
    int history_next;
    uint16_t last_hist_scene;
    uint16_t last_hist_op;

    char row_text[STELLA_LIST_ROWS][72];
    char row_value[STELLA_LIST_ROWS][48];
    const char *row_ptrs[STELLA_LIST_ROWS];
    const char *value_ptrs[STELLA_LIST_ROWS];

    char notice[STELLA_NOTICE_BYTES];
    uint32_t notice_ms;

    char shown[768];
} stella_app_t;

bool stella_app_init(stella_app_t *app, const uint8_t *pack_data, uint32_t pack_size);
void stella_app_key(stella_app_t *app, const stella_key_t *key);
void stella_app_tick(stella_app_t *app, uint32_t elapsed_ms);
uint32_t stella_app_idle_ms(const stella_app_t *app);
void stella_app_flush(stella_app_t *app);
void stella_app_show_sleeping(stella_app_t *app);
