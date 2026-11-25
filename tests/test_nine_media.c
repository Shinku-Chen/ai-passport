// Host playback-path regression: real IMA blocks, track metadata and bounded
// decoding. No I2S/BSP is used.
#include "nine_media.h"
#include "nine_image.h"
#include "nine_pack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)length);
    assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

// The host test does not exercise the opening video, so these two symbols can
// stay inert; the decoder under test is nine_media.c.
uint32_t nine_image_op_count(void) { return 0; }
bool nine_image_op_frame(uint32_t index, uint32_t *image, uint32_t *timestamp,
                         uint32_t *duration)
{
    (void)index; (void)image; (void)timestamp; (void)duration;
    return false;
}

bool nine_image_show_asset(uint32_t image)
{
    (void)image;
    return false;
}
bool nine_image_show_asset_op(uint32_t image)
{
    (void)image;
    return false;
}
bool nine_image_show_asset_op_ex(uint32_t image, nine_image_op_result_t *result)
{
    (void)image;
    if (result) { result->crc_checks=1; result->jpeg_decodes=1; result->ok=true; }
    return true;
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    size_t script_size = 0, image_size = 0;
    uint8_t *script = load(argv[1], &script_size);
    uint8_t *images = load(argv[2], &image_size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, script, script_size));
    assert(nine_media_init(script, script_size, images, image_size));
    assert(nine_media_track_count() == 2);
    nine_media_track_t info;
    assert(nine_media_track_info(0, &info));
    assert(strcmp(info.name, "title") == 0 && info.duration_ms == 85417 && info.sample_rate == 12000);
    assert(nine_media_track_info(1, &info));
    assert(strcmp(info.name, "opening") == 0 && info.duration_ms == 124680 && info.sample_rate == 12000);

    for (uint32_t track = 0; track < 2; ++track) {
        assert(nine_media_play(track));
        assert(nine_media_current_track() == track);
        assert(nine_media_is_playing());
        int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES];
        uint64_t total = 0;
        bool nonzero = false;
        for (int blocks = 0; blocks < 100000; ++blocks) {
            const size_t n = nine_media_test_pump(pcm);
            if (!n) break;
            assert(n <= NINE_MEDIA_BLOCK_SAMPLES);
            total += n;
            for (size_t i = 0; i < n; ++i) nonzero |= pcm[i] != 0;
        }
        assert(!nine_media_is_playing());
        assert(nonzero);
        assert(nine_media_track_info(track, &info));
        const uint64_t expected = ((uint64_t)info.duration_ms * info.sample_rate + 999) / 1000;
        assert(total >= expected - 256 && total <= expected + 256);
        assert(!nine_media_failed());
    }

    // A corrupt IMA block must be rejected, not decoded as noise.
    assert(nine_media_track_info(0, &info));
    int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES];
    uint8_t bad[NINE_MEDIA_BLOCK_BYTES];
    memcpy(bad, images + info.data_offset, sizeof(bad));
    bad[2] = 90;   // step index > 88
    assert(!nine_media_decode_block(bad, sizeof(bad), pcm));
    bad[2] = 0;
    bad[3] = 1;    // reserved byte must be zero
    assert(!nine_media_decode_block(bad, sizeof(bad), pcm));
    bad[3] = 0;
    assert(nine_media_decode_block(bad, sizeof(bad), pcm));

    assert(nine_media_deinit());
    free(script);
    free(images);
    puts("nine media IMA playback path: PASS");
    return 0;
}
