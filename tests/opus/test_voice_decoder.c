#ifdef NDEBUG
#undef NDEBUG // Regression checks must also execute in optimized host builds.
#endif
#include "voice_decoder.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned allocations;
static bool deny_allocations;
void *test_opus_alloc(size_t size)
{
    ++allocations;
    return deny_allocations ? NULL : malloc(size);
}
void test_opus_free(void *ptr) { free(ptr); }

typedef struct { uint64_t hash; size_t samples; } pcm_result_t;
static pcm_result_t decode_clip(OpusDecoder *decoder, const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t header[2], packet[1500];
    int16_t pcm[960];
    pcm_result_t result = {UINT64_C(14695981039346656037), 0};
    while (fread(header, 1, 2, file) == 2) {
        unsigned bytes = (unsigned)header[0] | ((unsigned)header[1] << 8);
        assert(bytes > 0 && bytes <= sizeof(packet));
        assert(fread(packet, 1, bytes, file) == bytes);
        int count = opus_decode(decoder, packet, (int)bytes, pcm, 960, 0);
        assert(count > 0);
        for (int i = 0; i < count; ++i)
            result.hash = (result.hash ^ (uint16_t)pcm[i]) * UINT64_C(1099511628211);
        result.samples += (size_t)count;
    }
    assert(!ferror(file) && result.samples > 0);
    fclose(file);
    return result;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(voice_decoder_begin() == NULL);
    // Baselines use the old fresh-decoder-per-clip lifecycle and real assets.
    pcm_result_t expected[2];
    for (int i = 0; i < 2; ++i) {
        int error;
        OpusDecoder *fresh = opus_decoder_create(VOICE_DECODER_SAMPLE_RATE, 1, &error);
        assert(fresh && error == OPUS_OK);
        expected[i] = decode_clip(fresh, argv[i + 1]);
        opus_decoder_destroy(fresh);
    }
    assert(voice_decoder_init() == OPUS_OK);
    unsigned reserved_allocations = allocations;
    deny_allocations = true; // Model fragmented/exhausted heap after UI startup.
    assert(voice_decoder_init() == OPUS_OK);
    OpusDecoder *reserved = voice_decoder_begin();
    assert(reserved);
    for (int play = 0; play < 12; ++play) {
        int clip = play % 2;
        OpusDecoder *decoder = voice_decoder_begin();
        assert(decoder == reserved);
        pcm_result_t actual = decode_clip(decoder, argv[clip + 1]);
        assert(actual.samples == expected[clip].samples);
        assert(actual.hash == expected[clip].hash);
        assert(allocations == reserved_allocations);
    }
    printf("voice_decoder: PASS (12 clips, identical PCM, no Opus allocation after reserve; bytes=%d)\n",
           opus_decoder_get_size(1));
    return 0;
}
