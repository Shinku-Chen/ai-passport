// main/cafe/cafe_input.h —— 三键短按/长按判定（纯逻辑，可宿主测试）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAFE_KEY_COUNT 3

typedef enum {
    CAFE_KEY_OK = 0,
    CAFE_KEY_UP = 1,
    CAFE_KEY_DOWN = 2,
} cafe_key_t;

typedef enum {
    CAFE_IN_NONE = 0,
    CAFE_IN_SHORT,
    CAFE_IN_LONG,
} cafe_input_event_t;

typedef struct {
    bool down[CAFE_KEY_COUNT];
    uint32_t down_ms[CAFE_KEY_COUNT];
    bool long_fired[CAFE_KEY_COUNT];
} cafe_input_t;

void cafe_input_init(cafe_input_t *in);
// Abort any partial press/release cycle after a proven queue overflow.
// No SHORT/LONG can be emitted for the aborted cycle; the next full press starts fresh.
void cafe_input_abort(cafe_input_t *in);
cafe_input_event_t cafe_input_press(cafe_input_t *in, uint8_t key, uint32_t now_ms);
cafe_input_event_t cafe_input_release(cafe_input_t *in, uint8_t key, uint32_t now_ms,
                                      uint32_t long_ms);
cafe_input_event_t cafe_input_tick(cafe_input_t *in, uint32_t now_ms, uint32_t long_ms,
                                   uint8_t *key_out);
bool cafe_input_is_down(const cafe_input_t *in, uint8_t key);
bool cafe_input_long_done(const cafe_input_t *in, uint8_t key);
