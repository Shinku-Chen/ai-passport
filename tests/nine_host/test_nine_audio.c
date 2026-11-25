// Real audio package test: both tracks must validate (CRC, IMA block headers,
// rate/block count) and decode to non-silent PCM through the production block
// decoder. This is not a device playback test.
//
// Usage: test_nine_audio <script.bin> <images.bin>
#include "nine_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <script.bin> <images.bin>\n", argv[0]); return 2; }
    size_t script_size = 0, image_size = 0;
    uint8_t *script = slurp(argv[1], &script_size);
    uint8_t *images = slurp(argv[2], &image_size);
    if (!script || !images) { fprintf(stderr, "cannot read packages\n"); return 2; }
    if (!nine_media_init(script, script_size, images, image_size)) {
        fprintf(stderr, "audio package rejected\n");
        return 1;
    }
    if (nine_media_track_count() != 2) { fprintf(stderr, "expected 2 tracks\n"); return 1; }
    for (uint32_t t = 0; t < 2; ++t) {
        nine_media_track_t info;
        if (!nine_media_track_info(t, &info) || !info.name || info.sample_rate != 16000 ||
            info.duration_ms < 1000 || info.data_size % 132) {
            fprintf(stderr, "track %u metadata invalid\n", (unsigned)t);
            return 1;
        }
        if (!nine_media_play(t)) { fprintf(stderr, "track %u play failed\n", (unsigned)t); return 1; }
        int16_t pcm[256];
        int blocks = 0, nonzero = 0, peak = 0;
        for (;;) {
            const size_t n = nine_media_test_pump(pcm);
            if (!n) break;
            if (n != 256) { fprintf(stderr, "short block\n"); return 1; }
            ++blocks;
            for (int i = 0; i < 256; ++i) {
                const int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v) ++nonzero;
                if (v > peak) peak = v;
            }
            if (blocks > 200) break;   // up to ~3.2s covers a fade-in intro
        }
        if (blocks < 4 || !nonzero || peak < 100) {
            fprintf(stderr, "track %u decoded silence/too little (%d blocks, peak %d)\n",
                    (unsigned)t, blocks, peak);
            return 1;
        }
        printf("track %u: %s %ums %u B %u blocks decoded, peak %d\n", (unsigned)t, info.name,
               (unsigned)info.duration_ms, (unsigned)info.data_size, (unsigned)blocks, peak);
        nine_media_stop();
    }
    printf("PASS audio: 2 tracks @16kHz mono IMA ADPCM, real decode\n");
    free(script);
    free(images);
    return 0;
}
