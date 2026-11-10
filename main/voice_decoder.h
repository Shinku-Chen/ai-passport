#pragma once

#include "opus.h"

#define VOICE_DECODER_SAMPLE_RATE 16000

// Reserve one decoder before optional tasks fragment the system heap. The
// singleton lives until reboot; initialization must precede the player task.
int voice_decoder_init(void);

// Player-task only: reset the existing decoder between independent clips.
// Returns NULL if initialization/reset failed; never allocates per playback.
OpusDecoder *voice_decoder_begin(void);
