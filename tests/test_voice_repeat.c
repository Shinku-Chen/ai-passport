#ifdef NDEBUG
#undef NDEBUG
#endif
#include "voice_repeat.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    voice_repeat_t state = {0};
    uint32_t first, next;
    assert(voice_repeat_offer(&state, &first));
    for (int i = 0; i < 100; ++i) assert(!voice_repeat_offer(&state, &next));
    assert(voice_repeat_valid(&state, first));

    // Release invalidates both a queued repeat and one waiting for the UI lock.
    voice_repeat_cancel(&state);
    assert(!voice_repeat_valid(&state, first));
    voice_repeat_cancel(&state); // A new physical press owns a new generation.
    assert(voice_repeat_offer(&state, &next));
    assert(next != first);
    voice_repeat_done(&state, first); // Old completion must not cancel new work.
    assert(voice_repeat_valid(&state, next));
    assert(!voice_repeat_offer(&state, &first));

    // Completion (or failed queue send) frees the slot for the next held repeat.
    voice_repeat_done(&state, next);
    assert(voice_repeat_offer(&state, &first));
    assert(voice_repeat_valid(&state, first));
    voice_repeat_done(&state, first);
    assert(!voice_repeat_valid(&state, first));

    // Independent key slots do not accidentally clear a newer direction.
    voice_repeat_t other = {0};
    assert(voice_repeat_offer(&other, &next));
    voice_repeat_cancel(&state);
    assert(voice_repeat_valid(&other, next));
    puts("voice_repeat: PASS");
    return 0;
}
