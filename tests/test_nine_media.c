// Offline audio and OP contracts: real IMA blocks from the pack, track metadata,
// invalid blocks, OP frame timing and audio CRC rejection.
#include "nine_format.h"
#include "nine_image.h"
#include "nine_media.h"
#include "nine_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *size = (size_t)n; return b;
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static uint32_t canvas_hash(void)
{
    uint32_t h = 0;
    for (unsigned y = 0; y < NINE_IMAGE_H; y += 7)
        for (unsigned x = 0; x < NINE_IMAGE_W; x += 7)
            h = h * 31u + nine_image_test_pixel(x, y);
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn);
    uint8_t *images = load(argv[2], &imn);
    static nine_pack_t pack;
    CHECK(nine_pack_open(&pack, script, sn), "pack");
    CHECK(nine_image_init(NULL), "image init");
    CHECK(nine_image_bind_bundle(script, sn, images, imn), "image bind");
    CHECK(nine_media_init(script, sn, images, imn), "media init");
    CHECK(nine_media_track_count() == 2, "two tracks");
    nine_media_track_t t0, t1;
    CHECK(nine_media_track_info(0, &t0) && t0.sample_rate == 8000 && t0.duration_ms > 60000,
          "title track");
    CHECK(nine_media_track_info(1, &t1) && t1.duration_ms > 90000, "opening track");

    uint8_t block[NINE_MEDIA_BLOCK_BYTES];
    memset(block, 0, sizeof(block));
    block[0] = 0xE8; block[1] = 0x03;   // predictor 1000
    block[2] = 0;                        // step index
    int16_t pcm[NINE_MEDIA_BLOCK_SAMPLES];
    CHECK(nine_media_decode_block(block, sizeof(block), pcm), "decode flat block");
    CHECK(pcm[0] == 1000 && pcm[255] == 1000, "flat block holds predictor");
    CHECK(!nine_media_decode_block(block, sizeof(block) - 1, pcm), "short block rejected");
    block[2] = 89;
    CHECK(!nine_media_decode_block(block, sizeof(block), pcm), "bad index rejected");
    block[2] = 0; block[3] = 1;
    CHECK(!nine_media_decode_block(block, sizeof(block), pcm), "reserved byte rejected");
    block[3] = 0;

    CHECK(nine_media_play(0), "play title");
    size_t total = 0;
    int nonzero = 0;
    for (int i = 0; i < 200; ++i) {
        size_t n = nine_media_test_pump(pcm);
        if (!n) break;
        total += n;
        for (size_t k = 0; k < n; ++k) if (pcm[k] != 0) nonzero++;
    }
    CHECK(total > 0 && nonzero > 0, "real blocks decode to samples");
    CHECK(nine_media_position_ms() > 0 && nine_media_position_ms() <= t0.duration_ms,
          "position advances");

    /* OP slideshow: pumping audio moves the displayed frame on the real canvas. */
    CHECK(nine_media_play_op(1), "play op");
    nine_media_tick();
    uint32_t h0 = canvas_hash();
    for (int i = 0; i < 120; ++i) (void)nine_media_test_pump(pcm);
    nine_media_tick();
    uint32_t h1 = canvas_hash();
    CHECK(h0 != h1, "op frame advanced");
    CHECK(nine_media_is_playing(), "op still playing");
    nine_media_stop();
    CHECK(!nine_media_is_playing(), "stop");

    /* A corrupt audio payload is rejected before playback. */
    uint32_t acount = rd32(images + 36), aoff = rd32(images + 40);
    CHECK(acount == 2 && aoff + 32 <= imn, "audio table");
    uint32_t data_off = rd32(images + aoff + 8);
    uint8_t *bad_images = malloc(imn);
    CHECK(bad_images != NULL, "bad audio copy");
    memcpy(bad_images, images, imn);
    bad_images[data_off + 10] ^= 0xff;
    CHECK(nine_media_deinit(), "media deinit");
    CHECK(!nine_media_init(script, sn, bad_images, imn), "corrupt audio rejected");
    free(bad_images);

    printf("fails=%d\n", fails);
    free(script); free(images);
    return fails ? 1 : 0;
}
