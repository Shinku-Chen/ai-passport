#include "nine_reader.h"

#include <string.h>

static bool deadline_due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void clear_timers(nine_reader_t *reader)
{
    reader->fast_forward = false;
    reader->auto_deadline_valid = false;
}

void nine_reader_stop_ff(nine_reader_t *reader)
{
    if (!reader) return;
    reader->fast_forward = false;
}

void nine_reader_reset(nine_reader_t *reader)
{
    if (reader) memset(reader, 0, sizeof(*reader));
}

nine_advance_decision_t nine_reader_advance_decide(bool render_error, uint16_t page,
                                                   int page_count)
{
    if (render_error) return NINE_ADVANCE_RETRY;
    if (page_count < 1) page_count = 1;
    return (page + 1 < (uint16_t)page_count) ? NINE_ADVANCE_PAGE : NINE_ADVANCE_LINE;
}

void nine_reader_set_reading(nine_reader_t *reader, bool reading, uint32_t now)
{
    if (!reader) return;
    (void)now;
    if (reader->reading == reading) return; /* page flips keep auto/FF state */
    reader->reading = reading;
    reader->auto_mode = false;
    clear_timers(reader);
    if (reading) {
        // A long-press suppression must not survive a menu round trip.
        for (unsigned i = 0; i < NINE_BTN_COUNT; ++i) reader->suppress_click[i] = false;
    }
}

void nine_reader_set_typing(nine_reader_t *reader, bool typing, uint32_t now)
{
    if (!reader) return;
    reader->typing = typing;
    if (typing) {
        reader->auto_deadline_valid = false;
    } else if (reader->auto_mode && !reader->auto_deadline_valid) {
        reader->auto_deadline = now + NINE_AUTO_STEP_MS;
        reader->auto_deadline_valid = true;
    }
}

void nine_reader_set_auto(nine_reader_t *reader, bool on, uint32_t now)
{
    if (!reader) return;
    reader->auto_mode = on;
    reader->auto_deadline_valid = false;
    if (on && !reader->typing) {
        reader->auto_deadline = now + NINE_AUTO_STEP_MS;
        reader->auto_deadline_valid = true;
    }
}

nine_action_t nine_reader_key(nine_reader_t *reader, nine_button_t button,
                              nine_event_t event, uint32_t now)
{
    if (!reader || button >= NINE_BTN_COUNT) return NINE_ACT_NONE;

    if (event == NINE_EV_RELEASE) {
        reader->suppress_click[button] = false;
        if (button == NINE_BTN_UP && reader->fast_forward) {
            reader->fast_forward = false;
            return NINE_ACT_STOP_FF;
        }
        return NINE_ACT_NONE;
    }

    if (event == NINE_EV_CLICK) {
        if (reader->suppress_click[button]) {
            reader->suppress_click[button] = false;
            return NINE_ACT_NONE;
        }
        if (!reader->reading) return NINE_ACT_NONE;
        // Manual input always advances (or reveals typing); stopping auto is an
        // additional effect, never a replacement for the advance.
        reader->auto_mode = false;
        clear_timers(reader);
        return NINE_ACT_ADVANCE;
    }

    // LONG: this key press has been consumed by a long action.
    reader->suppress_click[button] = true;
    if (!reader->reading) return NINE_ACT_NONE;

    switch (button) {
    case NINE_BTN_OK:
        reader->auto_mode = false;
        clear_timers(reader);
        return NINE_ACT_MENU;
    case NINE_BTN_DOWN:
        reader->fast_forward = false;
        reader->auto_mode = !reader->auto_mode;
        reader->auto_deadline_valid = false;
        if (reader->auto_mode && !reader->typing) {
            reader->auto_deadline = now + NINE_AUTO_STEP_MS;
            reader->auto_deadline_valid = true;
        }
        return NINE_ACT_AUTO_TOGGLE;
    case NINE_BTN_UP:
        reader->auto_mode = false;
        reader->auto_deadline_valid = false;
        reader->fast_forward = true;
        // No deadline: the app advances immediately for START_FF, then once
        // per input-control tick. There is deliberately no 90/120 ms staging.
        return NINE_ACT_START_FF;
    default:
        return NINE_ACT_NONE;
    }
}

nine_action_t nine_reader_tick(nine_reader_t *reader, uint32_t now)
{
    if (!reader || !reader->reading) {
        if (reader) clear_timers(reader);
        return NINE_ACT_NONE;
    }
    if (reader->fast_forward) {
        // Exactly one sentence per control tick; never a catch-up loop.
        return NINE_ACT_ADVANCE_FF;
    }
    if (!reader->auto_mode) {
        reader->auto_deadline_valid = false;
        return NINE_ACT_NONE;
    }
    if (reader->typing) {
        reader->auto_deadline_valid = false;
        return NINE_ACT_NONE;
    }
    if (!reader->auto_deadline_valid) {
        reader->auto_deadline = now + NINE_AUTO_STEP_MS;
        reader->auto_deadline_valid = true;
        return NINE_ACT_NONE;
    }
    if (!deadline_due(now, reader->auto_deadline)) return NINE_ACT_NONE;
    reader->auto_deadline = now + NINE_AUTO_STEP_MS;
    return NINE_ACT_ADVANCE;
}
