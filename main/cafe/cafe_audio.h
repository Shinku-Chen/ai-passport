// main/cafe/cafe_audio.h —— 背景音乐 / OP 音轨播放（4 kHz IMA -> 8 kHz PCM）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool cafe_audio_init(void);
bool cafe_audio_shutdown(void);

// Non-blocking scene requests; the audio task owns the codec/I2S.
bool cafe_audio_play_title(bool loop);
bool cafe_audio_play_op(void);
bool cafe_audio_play_op_at(uint32_t position_ms);
bool cafe_audio_play_preview(uint8_t track_id, bool loop);
void cafe_audio_stop(void);
bool cafe_audio_stop_confirmed(uint32_t timeout_ms);
bool cafe_audio_stop_timed_out(void);

void cafe_audio_set_volume(uint8_t percent);
uint8_t cafe_audio_volume(void);
bool cafe_audio_failed(void);
const char *cafe_audio_failure(void);
