// Pure reading-page input state machine (no LVGL / ESP-IDF), so the exact
// three-key contract can be host-tested with a controlled clock.
//
//   short OK / UP / DOWN : advance one complete unit (next page or next line)
//   long OK              : in-game menu
//   long DOWN            : toggle auto-play
//   hold UP              : instantly next sentence; each held tick is one more
//   release UP           : stop fast-forward immediately
//
// A long press suppresses the matching short click until that key is released,
// so a release never adds an unintended advance.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NINE_AUTO_STEP_MS 900u

typedef enum {
    NINE_BTN_UP = 0,
    NINE_BTN_OK,
    NINE_BTN_DOWN,
    NINE_BTN_COUNT,
} nine_button_t;

typedef enum {
    NINE_EV_CLICK = 0,
    NINE_EV_LONG,
    NINE_EV_RELEASE,
} nine_event_t;

// A short press always advances one complete unit: the next page of a long
// line, or the next line. It never stops at "reveal the rest of the typing".
typedef enum {
    NINE_ADVANCE_RETRY = 0,   // current page failed; do not move the cursor
    NINE_ADVANCE_PAGE,
    NINE_ADVANCE_LINE,
} nine_advance_decision_t;

typedef enum {
    NINE_ACT_NONE = 0,
    NINE_ACT_ADVANCE,       // next page or next line, regardless of typing
    NINE_ACT_ADVANCE_FF,    // one held-UP control tick: app advances one sentence
    NINE_ACT_MENU,          // open the in-game menu
    NINE_ACT_AUTO_TOGGLE,   // state.auto_mode is the new value
    NINE_ACT_START_FF,      // long-UP threshold: app must immediately next sentence
    NINE_ACT_STOP_FF,
} nine_action_t;

typedef struct {
    bool reading;
    bool auto_mode;
    bool fast_forward;
    bool typing;
    bool suppress_click[NINE_BTN_COUNT];
    bool auto_deadline_valid;
    uint32_t auto_deadline;
} nine_reader_t;

void nine_reader_reset(nine_reader_t *reader);
nine_advance_decision_t nine_reader_advance_decide(bool render_error, uint16_t page,
                                                   int page_count);
// Entering/leaving the reading page clears auto, fast-forward and timers. The
// per-key long-press suppression survives until that key is released.
void nine_reader_set_reading(nine_reader_t *reader, bool reading, uint32_t now);
void nine_reader_set_typing(nine_reader_t *reader, bool typing, uint32_t now);
void nine_reader_set_auto(nine_reader_t *reader, bool on, uint32_t now);
nine_action_t nine_reader_key(nine_reader_t *reader, nine_button_t button,
                              nine_event_t event, uint32_t now);
// Every held-UP control tick returns one ADVANCE_FF; there is no inter-sentence
// timer and no catch-up. The app must perform at most one sentence per call.
nine_action_t nine_reader_tick(nine_reader_t *reader, uint32_t now);
// Stop held fast-forward without changing reading/auto state. Used when a
// choice/ending/OP/resource error has stopped the reading page.
void nine_reader_stop_ff(nine_reader_t *reader);
