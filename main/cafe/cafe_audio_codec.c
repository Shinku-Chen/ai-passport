#include "cafe_audio_codec.h"

#include <string.h>

static const int16_t s_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6482, 7130, 7842, 8626, 9489, 10438,
    11482, 12630, 13892, 15280, 16808, 18488, 20336, 22368, 24600,
    27056, 29760, 32767,
};
static const int8_t s_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static int16_t clamp16(int v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

static int expand_nibble(int nibble, cafe_ima_state_t *s)
{
    const int step = s_step[s->step_index];
    const int delta = nibble & 7;
    const int diff = ((2 * delta + 1) * step) >> 3;
    s->predictor = clamp16(s->predictor + ((nibble & 8) ? -diff : diff));
    s->step_index += s_index[nibble];
    if (s->step_index < 0) s->step_index = 0;
    if (s->step_index > 88) s->step_index = 88;
    return s->predictor;
}

size_t cafe_ima_decode_block(const uint8_t *block, size_t block_len,
                             cafe_ima_state_t *state, int16_t *out, size_t out_cap)
{
    if (!block || !state || !out || block_len < 4) return 0;
    const size_t samples = (block_len - 4) * 2 + 1;
    if (samples > out_cap) return 0;
    state->predictor = (int16_t)(block[0] | ((uint16_t)block[1] << 8));
    state->step_index = block[2];
    if (state->step_index > 88) return 0;
    out[0] = (int16_t)state->predictor;
    size_t n = 1;
    for (size_t i = 4; i < block_len; ++i) {
        // IMA WAV stores the first nibble in the low half of each byte.
        out[n++] = expand_nibble(block[i] & 0x0F, state);
        if (n < samples) out[n++] = expand_nibble(block[i] >> 4, state);
    }
    return n;
}
