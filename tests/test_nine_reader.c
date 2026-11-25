// Controlled-clock regression for the reading-page input contract.
#include "nine_reader.h"

#include <assert.h>
#include <stdio.h>

static nine_action_t key(nine_reader_t *r, nine_button_t b, nine_event_t e, uint32_t t)
{
    return nine_reader_key(r, b, e, t);
}

typedef struct {
    int line;
    int page;
    int page_count;
} cursor_t;

static void step_cursor(cursor_t *cursor)
{
    if (nine_reader_advance_decide(false, (uint16_t)cursor->page,
                                   cursor->page_count) == NINE_ADVANCE_PAGE) {
        cursor->page++;
    } else {
        cursor->line++;
        cursor->page = 0;
    }
}

int main(void)
{
    nine_reader_t r;
    nine_reader_reset(&r);
    nine_reader_set_reading(&r, true, 0);

    // Each of the three short presses advances exactly once.
    assert(key(&r, NINE_BTN_OK, NINE_EV_CLICK, 10) == NINE_ACT_ADVANCE);
    assert(key(&r, NINE_BTN_UP, NINE_EV_CLICK, 20) == NINE_ACT_ADVANCE);
    assert(key(&r, NINE_BTN_DOWN, NINE_EV_CLICK, 30) == NINE_ACT_ADVANCE);

    // Long OK opens the menu and its release never adds a short advance.
    assert(key(&r, NINE_BTN_OK, NINE_EV_LONG, 100) == NINE_ACT_MENU);
    assert(key(&r, NINE_BTN_OK, NINE_EV_RELEASE, 200) == NINE_ACT_NONE);
    assert(nine_reader_tick(&r, 300) == NINE_ACT_NONE);

    // Leaving the page clears auto/FF and all pending state; a click on return
    // is not swallowed by the previous long press.
    nine_reader_set_reading(&r, false, 400);
    nine_reader_set_reading(&r, true, 500);
    assert(key(&r, NINE_BTN_OK, NINE_EV_CLICK, 600) == NINE_ACT_ADVANCE);

    // Auto: long DOWN toggles it, typing gates the 900 ms timer.
    nine_reader_set_reading(&r, true, 1000);
    nine_reader_set_typing(&r, true, 1000);
    assert(key(&r, NINE_BTN_DOWN, NINE_EV_LONG, 1000) == NINE_ACT_AUTO_TOGGLE);
    assert(r.auto_mode);
    assert(key(&r, NINE_BTN_DOWN, NINE_EV_RELEASE, 1100) == NINE_ACT_NONE);
    assert(nine_reader_tick(&r, 1800) == NINE_ACT_NONE); // still typing
    nine_reader_set_typing(&r, false, 1900);
    assert(nine_reader_tick(&r, 2799) == NINE_ACT_NONE);
    assert(nine_reader_tick(&r, 2800) == NINE_ACT_ADVANCE);

    // A manual press while auto is on advances and stops auto; it is not
    // consumed merely to turn auto off.
    nine_reader_set_typing(&r, true, 2900);
    assert(key(&r, NINE_BTN_OK, NINE_EV_CLICK, 3000) == NINE_ACT_ADVANCE);
    assert(!r.auto_mode);
    assert(nine_reader_tick(&r, 4000) == NINE_ACT_NONE);

    // Fast-forward is independent of the auto delay and has no 120 ms staging:
    // the threshold is START_FF for an immediate app advance, then every tick
    // is exactly one ADVANCE_FF.
    nine_reader_set_reading(&r, true, 5000);
    nine_reader_set_typing(&r, true, 5000);
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 5000) == NINE_ACT_START_FF);
    assert(r.fast_forward);
    assert(key(&r, NINE_BTN_UP, NINE_EV_RELEASE, 5001) == NINE_ACT_STOP_FF);
    assert(!r.fast_forward);
    assert(nine_reader_tick(&r, 5300) == NINE_ACT_NONE);

    nine_reader_set_reading(&r, true, 6000);
    nine_reader_set_typing(&r, true, 6000);
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 6000) == NINE_ACT_START_FF);
    assert(nine_reader_tick(&r, 6000) == NINE_ACT_ADVANCE_FF);
    assert(nine_reader_tick(&r, 6001) == NINE_ACT_ADVANCE_FF);
    assert(nine_reader_tick(&r, 6002) == NINE_ACT_ADVANCE_FF);
    assert(key(&r, NINE_BTN_UP, NINE_EV_RELEASE, 6003) == NINE_ACT_STOP_FF);
    assert(!r.fast_forward);
    assert(nine_reader_tick(&r, 6500) == NINE_ACT_NONE);
    // A programmatic stop (choice/ending/OP/resource error) is immediate too.
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 6600) == NINE_ACT_START_FF);
    nine_reader_stop_ff(&r);
    assert(!r.fast_forward);
    assert(nine_reader_tick(&r, 7001) == NINE_ACT_NONE);

    // Auto then FF: FF ignores the 900 ms auto deadline.
    assert(key(&r, NINE_BTN_DOWN, NINE_EV_LONG, 7000) == NINE_ACT_AUTO_TOGGLE);
    assert(r.auto_mode);
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 7010) == NINE_ACT_START_FF);
    assert(!r.auto_mode);
    assert(nine_reader_tick(&r, 7010) == NINE_ACT_ADVANCE_FF);

    // Choice/ending are not the reading page: entering one stops FF and auto.
    nine_reader_set_reading(&r, false, 7200);
    assert(!r.fast_forward);
    assert(!r.auto_mode);
    assert(nine_reader_tick(&r, 8000) == NINE_ACT_NONE);

    // Production advance plan: a short press always changes the visible unit,
    // even with typing active or auto-play on. No "first press only reveals".
    assert(nine_reader_advance_decide(false, 0, 2) == NINE_ADVANCE_PAGE);
    assert(nine_reader_advance_decide(false, 1, 2) == NINE_ADVANCE_LINE);
    assert(nine_reader_advance_decide(false, 0, 1) == NINE_ADVANCE_LINE);
    assert(nine_reader_advance_decide(false, 0, 0) == NINE_ADVANCE_LINE);
    // A broken text/image page must be retried and must not move the cursor.
    assert(nine_reader_advance_decide(true, 0, 3) == NINE_ADVANCE_RETRY);
    assert(nine_reader_advance_decide(true, 2, 3) == NINE_ADVANCE_RETRY);

    const nine_button_t buttons[3] = { NINE_BTN_OK, NINE_BTN_UP, NINE_BTN_DOWN };
    for (int i = 0; i < 3; ++i) {
        nine_reader_reset(&r);
        nine_reader_set_reading(&r, true, 0);
        nine_reader_set_typing(&r, true, 0);   // typing must not block advance
        cursor_t cursor = { .line = 0, .page = 0, .page_count = 2 };
        assert(key(&r, buttons[i], NINE_EV_CLICK, 10) == NINE_ACT_ADVANCE);
        step_cursor(&cursor);
        assert(cursor.line == 0 && cursor.page == 1);
        assert(key(&r, buttons[i], NINE_EV_CLICK, 20) == NINE_ACT_ADVANCE);
        step_cursor(&cursor);
        assert(cursor.line == 1 && cursor.page == 0);
    }

    // Auto on + typing true: a short press advances the cursor and turns auto
    // off; it is never consumed by the auto-off action.
    for (int i = 0; i < 3; ++i) {
        nine_reader_reset(&r);
        nine_reader_set_reading(&r, true, 0);
        nine_reader_set_auto(&r, true, 0);
        nine_reader_set_typing(&r, true, 0);
        cursor_t auto_cursor = { .line = 0, .page = 1, .page_count = 3 };
        assert(key(&r, buttons[i], NINE_EV_CLICK, 10) == NINE_ACT_ADVANCE);
        assert(!r.auto_mode);
        step_cursor(&auto_cursor);
        assert(auto_cursor.line == 0 && auto_cursor.page == 2);
    }

    // Long UP: the app receives START_FF immediately and a held tick is
    // ADVANCE_FF immediately; release stops and does not append a short click.
    // Auto's 900 ms timer is irrelevant.
    nine_reader_reset(&r);
    nine_reader_set_reading(&r, true, 0);
    nine_reader_set_typing(&r, true, 0);
    cursor_t ff_cursor = { .line = 0, .page = 0, .page_count = 2 };
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 1000) == NINE_ACT_START_FF);
    assert(nine_reader_tick(&r, 1000) == NINE_ACT_ADVANCE_FF);
    step_cursor(&ff_cursor);
    assert(ff_cursor.page == 1);
    assert(key(&r, NINE_BTN_UP, NINE_EV_RELEASE, 1001) == NINE_ACT_STOP_FF);
    assert(nine_reader_tick(&r, 2000) == NINE_ACT_NONE);
    assert(ff_cursor.page == 1);

    // Long OK / long DOWN consume the press; release adds no short advance.
    for (int i = 0; i < 2; ++i) {
        nine_reader_reset(&r);
        nine_reader_set_reading(&r, true, 0);
        nine_reader_set_typing(&r, true, 0);
        cursor_t c = { .line = 4, .page = 0, .page_count = 1 };
        const nine_action_t long_action = (i == 0)
            ? key(&r, NINE_BTN_OK, NINE_EV_LONG, 10)
            : key(&r, NINE_BTN_DOWN, NINE_EV_LONG, 10);
        assert(long_action == (i == 0 ? NINE_ACT_MENU : NINE_ACT_AUTO_TOGGLE));
        assert(key(&r, i == 0 ? NINE_BTN_OK : NINE_BTN_DOWN, NINE_EV_RELEASE, 20) ==
               NINE_ACT_NONE);
        assert(nine_reader_tick(&r, 1000) == NINE_ACT_NONE);
        assert(c.line == 4 && c.page == 0);
    }

    // Page switch clears FF/auto; returning starts without stale suppression.
    nine_reader_reset(&r);
    nine_reader_set_reading(&r, true, 0);
    assert(key(&r, NINE_BTN_UP, NINE_EV_LONG, 10) == NINE_ACT_START_FF);
    nine_reader_set_reading(&r, false, 20);
    assert(!r.fast_forward);
    assert(nine_reader_tick(&r, 500) == NINE_ACT_NONE);
    nine_reader_set_reading(&r, true, 600);
    assert(key(&r, NINE_BTN_OK, NINE_EV_CLICK, 700) == NINE_ACT_ADVANCE);

    puts("nine reader key/timing state machine: PASS");
    return 0;
}
