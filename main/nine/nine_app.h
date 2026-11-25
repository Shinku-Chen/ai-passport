// 9-nine- 九次九日九重色 reader application state.
//
// Keys (identical across the six ports):
//   reading: short OK/UP/DOWN all advance (a press while auto is on also advances);
//            long OK opens the in-game menu; hold UP fast-forwards without waiting
//            for the typewriter or the auto timer; long DOWN toggles auto reading.
//   lists:   short UP/DOWN move, short OK selects, long OK returns.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gal_text.h"
#include "gal_ui.h"
#include "nine_media.h"
#include "nine_pack.h"
#include "nine_store.h"
#include "nine_vm.h"

#define NINE_HISTORY_ROWS 16
#define NINE_PAGE_ROWS 5
#define NINE_CHAPTER_ROWS 5
#define NINE_HISTORY_WINDOW 5

typedef struct { uint8_t btn, ev; } nine_key_t;

// Same numeric order as bsp_btn_t / bsp_btn_ev_t; kept local so the state
// machine can be compiled and driven on the host with a controlled clock.
enum { NINE_BTN_UP = 0, NINE_BTN_DOWN = 1, NINE_BTN_OK = 2 };
enum { NINE_EV_PRESS = 0, NINE_EV_CLICK = 1, NINE_EV_DOUBLE = 2,
       NINE_EV_LONG = 3, NINE_EV_RELEASE = 4 };

typedef struct {
    char speaker[24];
    char text[160];
} nine_history_row_t;

typedef struct {
    gal_ui_t ui;
    nine_pack_t pack;
    nine_vm_t vm;
    nine_store_t store;
    nine_cfg_t cfg;
    uint8_t unlock[NINE_UNLOCK_BYTES];
    nine_slot_meta_t slots[NINE_SLOTS];

    bool ready;          // UI built and a valid pack bound
    bool fatal;          // pack/image validation failed; show the error screen
    char fatal_text[96];
    bool media_ok;

    gal_page_t page;
    gal_page_t return_page;
    gal_page_t menu_return_page;   // GAME for text, CHOICE when the menu came from options

    int title_sel, menu_sel, settings_sel, gallery_sel;
    int save_sel, load_sel, slot_page;
    int chapter_sel, cg_sel, music_sel, choice_sel, history_sel;
    int history_off;
    int cg_variant, cg_page;
    int modal_sel;
    bool modal_open;
    bool overwrite_confirm;

    uint16_t text_page;
    int page_count;
    uint32_t page_offsets[GAL_TEXT_MAX_PAGES + 1];
    char text[NINE_TEXT_MAX];
    char speaker[96];
    char page_text[GAL_TEXT_CAPACITY];
    char notice[40];
    uint32_t notice_ms;

    uint32_t visual;
    uint32_t hist_frame;
    nine_history_row_t history[NINE_HISTORY_ROWS];
    int history_count, history_head;

    bool auto_mode, ff_hold;
    uint32_t auto_accum, held_ms;
    uint8_t held_btn;
    bool suppress_click;
    uint32_t idle_ms, battery_ms, auto_write_ms;
    bool auto_dirty;
    uint32_t clock_ms;

    uint8_t auto_blob[NINE_SAVE_RECORD_MAX];
    size_t auto_size;

    bool op_active, op_extra;
    gal_page_t op_return;
    uint32_t op_frame;

    lv_image_dsc_t title_dsc[3];
    lv_image_dsc_t thumb_dsc[8];
    gal_cg_t cg_items[8];
    char cg_labels[8][24];
} nine_app_t;

bool nine_app_init(nine_app_t *app, const uint8_t *script, size_t script_size,
                   const uint8_t *images, size_t image_size);
void nine_app_key(nine_app_t *app, const nine_key_t *key);
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms);
uint32_t nine_app_idle_ms(const nine_app_t *app);
void nine_app_flush(nine_app_t *app);
void nine_app_show_sleeping(nine_app_t *app);

// Startup gate: never report a 320x240-ready UI unless the BSP lock succeeded,
// the landscape call returned OK, and LVGL reports 320x240.
bool nine_app_landscape_ok(bool lock_ok, bool set_ok, int width, int height);
// Backlight gate: only true when the arm/invalidate critical section succeeded
// and LVGL actually submitted a flush; a timeout must leave the panel dark.
bool nine_app_first_frame_ready(bool lock_ok, bool submitted);

// Host-regression helpers: the production state machine drives these, tests only
// observe them. No alternate host behavior is compiled in.
const char *nine_app_current_text(const nine_app_t *app);
int nine_app_current_page(const nine_app_t *app);
bool nine_app_auto_on(const nine_app_t *app);
bool nine_app_fast_forwarding(const nine_app_t *app);
int nine_app_history_count(const nine_app_t *app);
