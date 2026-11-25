// 9-nine- Episode 4 reader application: landscape 320x240, three keys.
#pragma once

#include "gal_settings.h"
#include "gal_text.h"
#include "gal_ui.h"
#include "nine_pack.h"
#include "nine_store.h"
#include "nine_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define NINE_APP_HISTORY_MAX 24
#define NINE_APP_HISTORY_WINDOW 3
#define NINE_APP_HISTORY_TEXT 384
#define NINE_APP_CHAPTER_WINDOW 5
#define NINE_APP_CG_WINDOW 8
#define NINE_APP_SAVE_CAPACITY NINE_SAVE_CAPACITY
#define NINE_APP_STEP_MS 50u
#define NINE_APP_AUTO_MS 900u
#define NINE_APP_PROGRESS_MS 2000u

typedef struct {
    uint8_t btn, ev;
} nine_key_t;

typedef struct {
    gal_ui_t ui;
    nine_pack_t pack;
    nine_vm_t vm;
    nine_store_t store;
    nine_cfg_t cfg;
    nine_unlock_blob_t unlock;

    bool fatal;             // unreadable: pack/orientation/UI/image layer failed
    bool resource_fatal;    // image layer allocation/bind failed (no text fallback)
    bool resource_error;    // current frame/OP/CG decode failed; no progress
    bool image_ok;
    bool media_ok;
    bool nvs_ok;
    bool opening;
    bool op_gallery;       // title-Gallery OP playback (no VM/progress writes)
    bool auto_mode;
    bool fast_forward;
    bool preview;
    bool modal;
    bool auto_valid;
    bool vm_error;
    bool image_error;

    gal_page_t page;
    gal_page_t return_page;   // subpage back target (MENU for SAVE/LOAD/SETTINGS/HISTORY)
    gal_page_t menu_origin;   // story page that opened the in-game menu (GAME/CHOICE)

    int title_sel;
    int menu_sel;
    int choice_sel;
    int save_sel;
    int load_sel;
    int settings_sel;
    int gallery_sel;
    int chapter_sel;
    int chapter_window;
    int cg_sel;
    int cg_window;
    int ending_sel;
    int music_sel;
    int modal_sel;
    int slot_node[NINE_STORE_SLOTS];
    int slot_page[NINE_STORE_SLOTS];
    bool slot_used[NINE_STORE_SLOTS];

    uint16_t text_page;
    int page_count;
    uint32_t page_offsets[GAL_TEXT_MAX_PAGES + 1];
    uint32_t visual;
    bool visual_valid;
    uint16_t shown_node;
    uint32_t shown_index;
    uint16_t shown_page;

    uint16_t key_loop1, key_true, key_clear_true, key_loop_na, key_cf_na;

    uint32_t auto_ms;
    uint32_t idle_ms;
    uint32_t battery_ms;
    int battery_percent;
    uint32_t notice_ms;
    uint32_t progress_ms;
    bool progress_dirty;
    uint32_t ignore_click_ms;
    uint8_t ignore_click_mask;

    uint32_t op_ms;
    uint32_t op_duration;
    uint32_t op_frame;
    uint32_t op_image;
    uint32_t op_frames_drawn;
    uint32_t op_late_skips;
    uint32_t op_max_decode_us;
    uint32_t op_max_gap_ms;
    uint32_t op_last_pos;
    uint16_t ending_kind;
    gal_page_t ending_return;

    uint8_t perm_blob[512];
    size_t perm_size;
    bool perm_valid;

    int history_count;
    int history_sel;
    int history_window;
    char history[NINE_APP_HISTORY_MAX][NINE_APP_HISTORY_TEXT];

    char text[768];
    char speaker[96];
    char page_text[GAL_TEXT_CAPACITY];
    char chapter_title[64];
    char notice[96];
    char error[96];
} nine_app_t;

bool nine_app_init(nine_app_t *app);
void nine_app_key(nine_app_t *app, const nine_key_t *key);
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms);
uint32_t nine_app_idle_ms(const nine_app_t *app);
void nine_app_flush(nine_app_t *app);
// Flush progress and stop the audio worker before deep sleep.
void nine_app_suspend(nine_app_t *app);
void nine_app_show_sleeping(nine_app_t *app);
