// main/nine/nine_app.h — 9-nine- 春色春恋春熙风 reader application state.
//
// The pack/VM layer is purely offline: graph, text, images and both music tracks
// are compiled into the firmware. This layer owns page navigation, the three-key
// semantics, save slots, automatic progress and the permanent story record.
#pragma once

#include "gal_settings.h"
#include "gal_text.h"
#include "gal_ui.h"
#include "nine_pack.h"
#include "nine_vm.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>

// Source slot file has 30 manual slots, four per page. We keep the count and
// render a moving window of it (the device has three keys, not a touch pager).
#define NINE_SAVE_SLOTS 30
#define NINE_SLOT_BYTES 512
#define NINE_PERM_BYTES 512
#define NINE_PREVIEW_MAX 96
#define NINE_WINDOW_SLOTS 4
#define NINE_WINDOW_LIST 4
#define NINE_WINDOW_HISTORY 3
#define NINE_WINDOW_CHAPTER 4

// A failed text/image read freezes the story at the current frame. The menu is
// still reachable so the user can retry, load a slot or return to the title.
#define NINE_VIEW_OK 0
#define NINE_VIEW_TEXT_ERROR 1
#define NINE_VIEW_IMAGE_ERROR 2

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} nine_key_t;

typedef enum {
    NINE_MENU_BACK = 0,
    NINE_MENU_SAVE,
    NINE_MENU_LOAD,
    NINE_MENU_HISTORY,
    NINE_MENU_SETTINGS,
    NINE_MENU_TITLE,
} nine_menu_action_t;

typedef enum {
    NINE_MODAL_NONE = 0,
    NINE_MODAL_OVERWRITE,
} nine_modal_t;

typedef struct {
    struct {
        char speaker[32];
        char text[256];
    } rows[NINE_HISTORY_MAX];
    int count;
    int head;                 // oldest row index
} nine_history_t;

typedef struct {
    bool used;
    bool corrupt;
    bool preview;
    uint16_t node;
    uint16_t page;
    uint32_t index;
} nine_slot_t;

typedef struct {
    gal_ui_t ui;
    nine_pack_t pack;         // 8 KiB bounded text cache lives here
    nine_vm_t vm;             // exact graph state + 24 history snapshots
    gal_settings_t settings;

    nvs_handle_t nvs;
    bool nvs_ok;
    bool pack_ok;             // script package validated
    bool images_ok;           // image package validated
    bool media_ok;            // audio worker available
    bool has_session;         // an automatic progress record is loadable

    bool auto_mode;
    bool fast_forward;
    uint8_t view_error;       // NINE_VIEW_*: a decode failure blocks advancing
    bool opening;             // story OP marker is playing
    bool gallery_op;          // independent Gallery -> OP playback
    bool gallery_op_error;    // last Gallery OP failed visibly
    uint32_t title_bg_image;  // cached decoded title image id
    bool title_bg_applied;    // current title art is already on screen
    uint32_t title_bg_decodes;    // diagnostics: title decode count
    uint32_t title_music_starts;  // diagnostics: title track starts
    bool suppress_click[3];   // long press of a key consumes its release click
    bool dirty_auto;

    gal_page_t page;
    gal_page_t return_page;
    nine_modal_t modal;
    int modal_action;
    int modal_slot;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slot_sel;
    int history_sel;
    int gallery_sel;
    int cg_group;
    int cg_variant;
    int cg_page;
    int chapter_sel;
    int music_sel;
    int bg_sel;
    int choice_sel;

    int page_first;           // first visible row for moving windows
    int text_page;
    int restore_text_page;    // -1 = normal page 0; >=0 = page requested by load/continue
    int page_count;
    uint32_t page_offsets[GAL_TEXT_MAX_PAGES + 1];

    uint32_t idle_ms;
    uint32_t auto_wait_ms;
    uint32_t notice_ms;
    uint32_t progress_wait_ms;
    uint32_t battery_wait_ms;
    int battery_percent;

    char notice[48];
    char error[64];
    char text[NINE_TEXT_MAX];
    char speaker[64];
    char page_text[GAL_TEXT_CAPACITY];
    char chapter[48];
    nine_history_t history;

    uint8_t save_buffer[NINE_SLOT_BYTES];
    uint8_t perm_buffer[NINE_PERM_BYTES];
    uint32_t last_visual;
    bool have_visual;
    uint16_t last_node;
    uint32_t last_index;
    uint32_t last_frame;
    bool typing_set;
    uint16_t typing_node;
    uint32_t typing_index;
    int typing_page;
    bool typing_large;
    size_t perm_len;
    nine_slot_t slots[NINE_SAVE_SLOTS];
} nine_app_t;

bool nine_app_init(nine_app_t *app);
// Returns true when the event was consumed. A false result means the LVGL lock
// was busy; the caller must retain the same key and retry (no drop, no tick).
bool nine_app_key(nine_app_t *app, const nine_key_t *key);
// Queue-overflow latch recovery: drop hold/latch state, never touch story/save.
void nine_app_input_recover(nine_app_t *app);
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms);
uint32_t nine_app_idle_ms(const nine_app_t *app);
void nine_app_flush(nine_app_t *app);
void nine_app_show_sleeping(nine_app_t *app);
