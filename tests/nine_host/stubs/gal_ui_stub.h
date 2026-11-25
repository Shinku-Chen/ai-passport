// Host stubs for the reader application test. They record the UI calls that
// the state machine is expected to make and expose a controllable typewriter
// state, so key/auto/fast-forward behaviour can be asserted without LVGL.
#pragma once
#include "gal_ui.h"
#include <stdbool.h>

extern gal_page_t host_page;
extern char host_text[2048];
extern char host_speaker[64];
extern bool host_typing;
extern bool host_auto;
extern char host_notice[64];
extern int host_choice_count;
extern char host_choice_labels[16][64];
extern int host_choice_sel;
extern int host_row_count[GAL_LIST_COUNT];
extern char host_rows[GAL_LIST_COUNT][32][64];
extern char host_values[GAL_LIST_COUNT][32][64];
extern bool host_row_enabled[GAL_LIST_COUNT][32];
extern bool host_modal;
extern char host_ending_eyebrow[32];
extern char host_ending_title[96];
extern int host_font_large;
extern uint8_t host_transparency;
extern unsigned host_visuals;
extern unsigned host_assets;
