// Host UI/app stub: audio is exercised separately with the dedicated codec test.
#include "cafe_audio.h"
#include <string.h>
static uint8_t s_vol = 60;
static uint32_t s_last_op_seek;
bool cafe_audio_init(void) { return true; }
bool cafe_audio_shutdown(void) { return true; }
bool cafe_audio_play_title(bool loop) { (void)loop; return true; }
bool cafe_audio_play_op(void) { return true; }
bool cafe_audio_play_op_at(uint32_t position_ms) { s_last_op_seek = position_ms; return true; }
bool cafe_audio_play_preview(uint8_t id, bool loop) { (void)id; (void)loop; return true; }
void cafe_audio_stop(void) {}
bool cafe_audio_stop_confirmed(uint32_t timeout_ms) { (void)timeout_ms; return true; }
bool cafe_audio_stop_timed_out(void) { return false; }
void cafe_audio_set_volume(uint8_t p) { s_vol = p; }
uint8_t cafe_audio_volume(void) { return s_vol; }
bool cafe_audio_failed(void) { return false; }
const char *cafe_audio_failure(void) { return "stub"; }

uint32_t cafe_audio_stub_op_seek(void) { return s_last_op_seek; }
