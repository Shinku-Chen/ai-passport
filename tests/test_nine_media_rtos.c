// Production audio_worker regression, compiled WITHOUT NINE_MEDIA_HOST.
// A controlled RTOS/BSP boundary runs the real worker synchronously.
#include "nine_media.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned test_audio_format_calls;
extern uint32_t test_audio_hz;
extern uint8_t test_audio_bits;
extern uint8_t test_audio_channels;
extern unsigned test_audio_writes;
extern uint64_t test_audio_samples;
extern uint64_t test_audio_bytes;
extern bool test_audio_fail_format;
extern bool test_audio_fail_write;
extern unsigned test_audio_stop_after_writes;
extern unsigned test_audio_stop_calls;
extern unsigned test_audio_take_calls;
extern unsigned test_audio_task_priority;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n > 0 && fseek(f, 0, SEEK_SET) == 0);
    uint8_t *p = malloc((size_t)n);
    assert(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return p;
}
static void reset_audio_stats(void)
{
    test_audio_format_calls = 0;
    test_audio_hz = 0;
    test_audio_bits = 0;
    test_audio_channels = 0;
    test_audio_writes = 0;
    test_audio_samples = 0;
    test_audio_bytes = 0;
    test_audio_fail_format = false;
    test_audio_fail_write = false;
    test_audio_stop_after_writes = 0;
    test_audio_stop_calls = 0;
    test_audio_take_calls = 0;
}
static void patch_track_rate(uint8_t *images, uint32_t rate)
{
    const uint32_t table = rd32(images + 40);
    uint8_t *p = images + table;
    const uint32_t bytes = rd32(p + 12);
    assert(bytes && bytes % NINE_MEDIA_BLOCK_BYTES == 0);
    const uint32_t blocks = bytes / NINE_MEDIA_BLOCK_BYTES;
    const uint32_t duration = (uint32_t)(((uint64_t)blocks * NINE_MEDIA_BLOCK_SAMPLES * 1000) / rate);
    wr32(p + 16, duration);
    wr32(p + 20, rate);
}
static void check_track(uint8_t *script, size_t script_size,
                        uint8_t *images, size_t image_size,
                        uint32_t track, uint32_t expected_rate)
{
    assert(nine_media_init(script, script_size, images, image_size));
    assert(test_audio_task_priority == 6);
    nine_media_track_t info;
    assert(nine_media_track_info(track, &info));
    assert(info.sample_rate == expected_rate);
    reset_audio_stats();
    assert(nine_media_play(track));
    assert(test_audio_task_priority == 6); // audio feed must outrank input(5)
    assert(!nine_media_is_playing());
    assert(!nine_media_failed());
    assert(test_audio_format_calls >= 1);
    assert(test_audio_hz == expected_rate);
    assert(test_audio_bits == 16 && test_audio_channels == 1);
    const uint64_t expected_samples =
        ((uint64_t)info.duration_ms * info.sample_rate + 999) / 1000;
    assert(test_audio_writes > 0);
    assert(test_audio_samples == expected_samples);
    assert(test_audio_bytes == expected_samples * sizeof(int16_t));
    assert(nine_media_position_ms() <= info.duration_ms);
    assert(nine_media_deinit());
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    size_t script_size = 0, image_size = 0;
    uint8_t *script = load(argv[1], &script_size);
    uint8_t *images = load(argv[2], &image_size);

    // Real 12 kHz title and OP tracks.
    check_track(script, script_size, images, image_size, 0, 12000);
    check_track(script, script_size, images, image_size, 1, 12000);

    // 8/12/16 kHz metadata must select the same rate; a hardcoded 8000 fails
    // 12k and 16k. Patch only a temporary copy of the metadata; media bytes
    // and the real 12k resource remain unchanged.
    const uint32_t rates[] = { 8000, 12000, 16000 };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        uint8_t *copy = malloc(image_size);
        assert(copy);
        memcpy(copy, images, image_size);
        if (rates[r] != 12000) patch_track_rate(copy, rates[r]);
        check_track(script, script_size, copy, image_size, 0, rates[r]);
        free(copy);
    }

    // Stop requested from inside the real audio worker (after one block):
    // the worker must leave the playback loop, consume the stop generation,
    // park without spinning, and never restart the track by itself. The
    // controlled RTOS run returns only after the worker is parked/idle.
    assert(nine_media_init(script, script_size, images, image_size));
    reset_audio_stats();
    test_audio_stop_after_writes = 1;
    assert(nine_media_play(0));
    assert(test_audio_stop_calls == 1);
    assert(test_audio_writes == 1);
    assert(!nine_media_is_playing());
    assert(!nine_media_failed());
    assert(test_audio_take_calls <= 4);
    test_audio_stop_after_writes = 0;
    // A subsequent explicit stop while parked is a no-op state-wise and does
    // not start playback; this is the bounded gallery-stop acknowledgement.
    nine_media_stop();
    assert(!nine_media_is_playing());
    assert(nine_media_deinit());

    // Format failure must propagate to nine_media_failed and stop writes.
    assert(nine_media_init(script, script_size, images, image_size));
    reset_audio_stats();
    test_audio_fail_format = true;
    assert(nine_media_play(0));
    assert(!nine_media_is_playing());
    assert(nine_media_failed());
    assert(test_audio_writes == 0);
    assert(nine_media_deinit());

    // A sink write failure must become a retained media fault, not a natural end.
    assert(nine_media_init(script, script_size, images, image_size));
    reset_audio_stats();
    test_audio_fail_write = true;
    assert(nine_media_play(0));
    assert(!nine_media_is_playing());
    assert(nine_media_failed());
    assert(test_audio_writes == 0);
    test_audio_fail_write = false;
    assert(nine_media_deinit());

    // OP natural end through the same worker.
    assert(nine_media_init(script, script_size, images, image_size));
    reset_audio_stats();
    assert(nine_media_play_op(1));
    assert(test_audio_task_priority == 6);
    assert(!nine_media_is_playing());
    assert(!nine_media_failed());
    assert(test_audio_hz == 12000);
    assert(nine_media_position_ms() > 0 && nine_media_position_ms() <= 124680);
    nine_media_tick();
    assert(nine_media_deinit());

    // Track switch, replay, stop, and restart keep the metadata rate.
    assert(nine_media_init(script, script_size, images, image_size));
    reset_audio_stats();
    assert(nine_media_play(0));
    assert(test_audio_hz == 12000);
    assert(nine_media_play(1));
    assert(test_audio_hz == 12000);
    assert(nine_media_play(0));
    assert(test_audio_hz == 12000);
    nine_media_stop();
    assert(!nine_media_is_playing());
    assert(nine_media_play(0));
    assert(!nine_media_is_playing());
    assert(test_audio_hz == 12000);
    assert(nine_media_deinit());

    free(script);
    free(images);
    puts("nine media non-host RTOS/BSP audio_worker rate/channel/sample test: PASS");
    return 0;
}
