#include "voice_repeat.h"

void voice_repeat_cancel(voice_repeat_t *state)
{
    ++state->generation;
    state->pending = false;
}

bool voice_repeat_offer(voice_repeat_t *state, uint32_t *generation)
{
    if (state->pending) return false;
    state->pending = true;
    *generation = state->generation;
    return true;
}

bool voice_repeat_valid(const voice_repeat_t *state, uint32_t generation)
{
    return state->pending && state->generation == generation;
}

void voice_repeat_done(voice_repeat_t *state, uint32_t generation)
{
    if (state->generation == generation) state->pending = false;
}
