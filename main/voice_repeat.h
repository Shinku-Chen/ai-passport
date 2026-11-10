#pragma once

#include <stdbool.h>
#include <stdint.h>

// Caller serializes these short operations. Each physical press owns a
// generation; at most one repeat can be queued or waiting for the UI lock.
typedef struct {
    uint32_t generation;
    bool pending;
} voice_repeat_t;

void voice_repeat_cancel(voice_repeat_t *state);
bool voice_repeat_offer(voice_repeat_t *state, uint32_t *generation);
bool voice_repeat_valid(const voice_repeat_t *state, uint32_t generation);
void voice_repeat_done(voice_repeat_t *state, uint32_t generation);
