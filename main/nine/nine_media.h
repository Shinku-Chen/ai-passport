// Offline two-track IMA ADPCM playback. No network or Opus runtime is required.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NINE_MEDIA_BLOCK_BYTES 132u
#define NINE_MEDIA_BLOCK_SAMPLES 256u
typedef struct {
    const char *name;
    uint32_t duration_ms, sample_rate;
    uint32_t data_offset, data_size;
} nine_media_track_t;

bool nine_media_init(const uint8_t *script, size_t script_size,
                     const uint8_t *images, size_t image_size);
bool nine_media_play(uint32_t track);
// Also starts the opening's timestamped frame sequence. Call tick under the
// LVGL lock; the audio worker never accesses LVGL or the image decoder.
bool nine_media_play_op(uint32_t track);
void nine_media_stop(void);
// Stops worker before releasing its data. False means it has not exited yet;
// callers must preserve source packages and retry rather than freeing live data.
bool nine_media_deinit(void);
bool nine_media_is_playing(void);
uint32_t nine_media_current_track(void);
uint32_t nine_media_position_ms(void);
uint32_t nine_media_track_count(void);
bool nine_media_track_info(uint32_t track, nine_media_track_t *out);
bool nine_media_failed(void);
void nine_media_tick(void);
typedef struct {
    uint32_t position_ms;
    uint32_t wall_ms;
    uint32_t duration_ms;
    uint32_t frames;
    uint32_t crc_checks;
    uint32_t jpeg_decodes;
    uint32_t late_skips;
    uint32_t decode_max_us;
    uint32_t decode_total_us;
    uint32_t frame_render_max_us;
    uint32_t frame_gap_max_ms;
    uint32_t audio_writes;
    uint32_t audio_write_max_us;
    uint32_t audio_write_total_us;
    uint32_t audio_gap_max_us;
} nine_media_op_stats_t;
// Bounded per-play OP statistics; updated by the real media tick path.
bool nine_media_get_op_stats(nine_media_op_stats_t *out);

// Shared production block decoder, independent of ESP-IDF and host-testable.
bool nine_media_decode_block(const uint8_t *src, size_t len,
                             int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES]);
#ifdef NINE_MEDIA_HOST
// Consume the next real ADPCM block without a hardware I2S sink.
size_t nine_media_test_pump(int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES]);
#endif
