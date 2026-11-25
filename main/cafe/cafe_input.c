// main/cafe/cafe_input.c
#include "cafe_input.h"

#include <string.h>

void cafe_input_init(cafe_input_t *in)
{
    if (in) memset(in, 0, sizeof(*in));
}

void cafe_input_abort(cafe_input_t *in)
{
    if (in) memset(in, 0, sizeof(*in));
}

cafe_input_event_t cafe_input_press(cafe_input_t *in, uint8_t key, uint32_t now_ms)
{
    if (!in || key >= CAFE_KEY_COUNT) return CAFE_IN_NONE;
    if (in->down[key]) return CAFE_IN_NONE;
    in->down[key] = true;
    in->down_ms[key] = now_ms;
    in->long_fired[key] = false;
    return CAFE_IN_NONE;
}

cafe_input_event_t cafe_input_release(cafe_input_t *in, uint8_t key, uint32_t now_ms,
                                      uint32_t long_ms)
{
    if (!in || key >= CAFE_KEY_COUNT || !in->down[key]) return CAFE_IN_NONE;
    in->down[key] = false;
    if (in->long_fired[key]) return CAFE_IN_NONE;
    const uint32_t held = now_ms - in->down_ms[key];
    if (held >= long_ms) {
        // 没有 tick 也按长按结算；仍只触发一次。
        in->long_fired[key] = true;
        return CAFE_IN_LONG;
    }
    return CAFE_IN_SHORT;
}

cafe_input_event_t cafe_input_tick(cafe_input_t *in, uint32_t now_ms, uint32_t long_ms,
                                   uint8_t *key_out)
{
    if (!in) return CAFE_IN_NONE;
    for (uint8_t i = 0; i < CAFE_KEY_COUNT; ++i) {
        if (!in->down[i] || in->long_fired[i]) continue;
        if ((uint32_t)(now_ms - in->down_ms[i]) >= long_ms) {
            in->long_fired[i] = true;
            if (key_out) *key_out = i;
            return CAFE_IN_LONG;
        }
    }
    return CAFE_IN_NONE;
}

bool cafe_input_is_down(const cafe_input_t *in, uint8_t key)
{
    return in && key < CAFE_KEY_COUNT && in->down[key];
}

bool cafe_input_long_done(const cafe_input_t *in, uint8_t key)
{
    return in && key < CAFE_KEY_COUNT && in->down[key] && in->long_fired[key];
}
