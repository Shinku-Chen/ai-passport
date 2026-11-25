// main/cafe/cafe_audio_codec.h —— IMA ADPCM (WAV block) decoder, DSP-free.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int predictor;
    int step_index;
} cafe_ima_state_t;

// Decode one IMA WAV block (header predictor/index/reserved + nibbles).
// `out` must hold at least ((block_len - 4) * 2 + 1) samples.
// Returns the number of decoded samples, or 0 on malformed input.
size_t cafe_ima_decode_block(const uint8_t *block, size_t block_len,
                             cafe_ima_state_t *state, int16_t *out, size_t out_cap);
