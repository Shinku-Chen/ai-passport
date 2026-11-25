// 9-nine- 天色天歌天籁音 offline reader application.
//
// The framework supplies the 19 logical screens, three-key semantics, text
// pagination, settings and the save/load UI. This layer replaces its placeholder
// content with the pinned graph pack and the real art/audio/media.
#pragma once
#include "gal_settings.h"
#include "gal_text.h"
#include "gal_ui.h"
#include "nine_assets.h"
#include "nine_media.h"
#include "nine_pack.h"
#include "nine_reader.h"
#include "nine_store.h"
#include "nine_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define NINE_TILES 4
#define NINE_TILE_W 48
#define NINE_TILE_H 27
#define NINE_TILE_BYTES (NINE_TILE_W * NINE_TILE_H * 2)
#define NINE_GALLERY_GROUPS_MAX 32
#define NINE_HISTORY_TEXT 192
#define NINE_HISTORY_SPEAKER 32

typedef struct {
    uint8_t btn;
    uint8_t ev;
} nine_key_t;

typedef struct {
    char speaker[NINE_HISTORY_SPEAKER];
    char text[NINE_HISTORY_TEXT];
} nine_history_entry_t;

typedef struct {
    gal_ui_t ui;
    nine_pack_t pack;
    nine_vm_t vm;
    nine_assets_t assets;
    gal_settings_t settings;
    nine_reader_t reader;
    nine_checkpoint_t checkpoint;

    gal_page_t page;
    gal_page_t return_page;
    bool ready;
    bool error;
    bool storage_ok;
    bool has_progress;
    bool has_session;
    bool opening;
    const char *op_end_reason;
    bool gallery_op;       // current OP was started from Gallery, not story VM
    bool gallery_op_error; // Gallery OP fault: retry/return without VM changes
    bool render_error;
    bool media_ok;
    bool title_music;
    const char *error_text;
    int key_loop1;
    int key_loop_sr;
    int key_cf_sr;
    int key_clear_true;
    int key_true;

    uint32_t clock_ms;
    uint32_t idle_ms;
    uint32_t notice_ms;
    uint32_t progress_accum_ms;
    uint32_t perm_accum_ms;
    bool progress_dirty;
    bool perm_dirty;
    uint32_t perm_hash;
    int battery_percent;
    uint32_t battery_accum_ms;

    int start_sel;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int save_sel;
    int load_sel;
    int history_sel;
    int gallery_sel;
    int cg_sel;
    int cg_group;
    int cg_variant;
    int gallery_page;
    int recall_sel;
    int music_sel;
    int chapter_sel;
    int transition_chapter;
    int ending_index;
    int choice_sel;

    int modal_action;
    int modal_sel;
    bool modal_open;

    uint16_t text_page;
    int page_count;
    uint32_t page_offsets[GAL_TEXT_MAX_PAGES + 1];
    char page_text[GAL_TEXT_CAPACITY];
    char text[GAL_TEXT_CAPACITY];
    char speaker[64];
    char last_speaker[64];
    char last_text[GAL_TEXT_CAPACITY];
    char chapter_label[64];
    uint16_t history_node;
    uint16_t history_index;
    bool history_has_cursor;

    nine_history_entry_t history[NINE_HISTORY_MAX];
    int history_count;

    nine_slot_t slots[NINE_SLOT_COUNT];
    bool slot_ok[NINE_SLOT_COUNT];

    uint8_t title_bg;
    uint8_t title_bg_unlocked[3];

    int gallery_groups;
    uint16_t group_first[NINE_GALLERY_GROUPS_MAX];
    uint16_t group_count[NINE_GALLERY_GROUPS_MAX];
    uint16_t recall_node[NINE_GALLERY_GROUPS_MAX];
    uint32_t recall_index[NINE_GALLERY_GROUPS_MAX];
    uint16_t recall_chapter[NINE_GALLERY_GROUPS_MAX];


    uint16_t tile_px[NINE_TILES][NINE_TILE_W * NINE_TILE_H];
    lv_image_dsc_t tile_dsc[NINE_TILES];
    char tile_label[NINE_TILES][24];
    char tile_value[NINE_TILES][16];
    char canvas_line[64];
    char storage_note[96];
} nine_app_t;

bool nine_app_init(nine_app_t *app);
void nine_app_key(nine_app_t *app, const nine_key_t *key);
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms);
uint32_t nine_app_idle_ms(const nine_app_t *app);
void nine_app_flush(nine_app_t *app);
void nine_app_show_sleeping(nine_app_t *app);
