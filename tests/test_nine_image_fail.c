// Host failure test: a corrupt background or indexed sprite must make the
// production compositor reject the visual before it overwrites the canvas. The
// app maps this false result to a visible error and a blocked cursor.
#include "nine_image.h"
#include "nine_pack.h"
#include "nine_crc.h"

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

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint32_t canvas_hash(void)
{
    uint32_t h = 2166136261u;
    for (unsigned y = 0; y < NINE_IMAGE_H; ++y) {
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) {
            h ^= nine_image_test_pixel(x, y);
            h *= 16777619u;
        }
    }
    return h;
}

static uint8_t *image_payload(uint8_t *images, uint32_t image, uint32_t *length)
{
    const uint32_t count = read32(images + 20);
    const uint32_t table = read32(images + 24);
    assert(image < count);
    const uint32_t off = read32(images + table + image * 32 + 12);
    *length = read32(images + table + image * 32 + 16);
    return images + off;
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    size_t script_size = 0, image_size = 0;
    uint8_t *script = load(argv[1], &script_size);
    uint8_t *images = load(argv[2], &image_size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, script, script_size));
    assert(nine_image_bind_bundle(script, script_size, images, image_size));

    // The whole image package and the per-JPEG record CRC must reject a
    // corrupt copy before changing the live binding.
    uint8_t *bad = malloc(image_size);
    assert(bad);
    memcpy(bad, images, image_size);
    bad[16] ^= 0x01; // package header CRC field
    assert(!nine_image_bind_bundle(script, script_size, bad, image_size));
    memcpy(bad, images, image_size);
    uint32_t bad_length = 0;
    uint8_t *bad_payload = image_payload(bad, 12, &bad_length);
    assert(bad_length > 16);
    bad_payload[bad_length - 8] ^= 0x5A;
    assert(!nine_image_bind_bundle(script, script_size, bad, image_size));
    free(bad);
    assert(nine_image_bind_bundle(script, script_size, images, image_size));
    assert(nine_image_init(NULL));
    assert(nine_image_show_visual(11));
    const uint32_t good = canvas_hash();
    // The source is immutable in production. This test mutates it in place, so
    // it must explicitly invalidate the same-visual fast-forward cache before
    // expecting a re-decode/rejection.
    nine_image_invalidate_cache();

    // Background image 12 is a JPEG record: a flipped payload byte breaks its
    // record CRC and must be rejected before the canvas is cleared.
    uint32_t length = 0;
    uint8_t *background = image_payload(images, 12, &length);
    assert(length > 16);
    const uint8_t saved = background[length - 8];
    background[length - 8] ^= 0x5A;
    assert(!nine_image_show_visual(11));
    assert(canvas_hash() == good);
    background[length - 8] = saved;

    // Sprite image 13 is indexed: corrupt its first compressed block. The raw
    // palette-index CRC catches it and the old canvas stays intact.
    uint8_t *sprite = image_payload(images, 13, &length);
    const uint32_t palette_count = read32(sprite);
    const uint32_t blocks = read32(sprite + 8);
    assert(palette_count > 0 && blocks > 0);
    const uint32_t block_table = 12 + palette_count * 4;
    const uint32_t payload_base = (uint32_t)(sprite - images);
    const uint32_t block_off = read32(sprite + block_table);
    assert(block_off >= payload_base && block_off + 2 < payload_base + length);
    const uint32_t absolute = block_off;
    const uint8_t saved_block = images[absolute + 1];
    images[absolute + 1] ^= 0x7F;
    assert(!nine_image_show_visual(11));
    assert(canvas_hash() == good);
    images[absolute + 1] = saved_block;

    // Restored bytes decode again, proving the failure was the corruption.
    assert(nine_image_show_visual(11));
    assert(canvas_hash() == good);

    // OP: exactly one CRC and one JPEG on success; a CRC/bounds failure must
    // clear every band and report zero JPEG attempts; a corrected-CRC malformed
    // JPEG must reach exactly one decoder attempt, fail, and clear.
    assert(nine_image_show_asset_op(479));
    nine_image_clear(0x07e0);
    uint32_t table = read32(images + 24);
    uint32_t record = table + 479 * 32;
    uint8_t *op_payload = image_payload(images, 479, &length);
    assert(length > 16);
    nine_image_op_result_t r;
    const uint8_t saved_op = op_payload[length - 8];
    op_payload[length - 8] ^= 0x33;
    assert(!nine_image_show_asset_op_ex(479, &r));
    assert(r.crc_checks == 1 && r.jpeg_decodes == 0 && !r.ok);
    for (unsigned y=0;y<NINE_IMAGE_H;y++) for (unsigned x=0;x<NINE_IMAGE_W;x++)
        assert(nine_image_test_pixel(x,y) == 0);
    op_payload[length - 8] = saved_op;

    // Corrected record CRC but malformed JPEG: CRC passes, decoder fails once,
    // all bands are cleared, no silent success.
    op_payload[length - 8] ^= 0x55;
    const uint32_t saved_crc = read32(images + record + 20);
    const uint32_t bad_crc = ~nine_crc32_update(UINT32_MAX, op_payload, length);
    images[record + 20] = (uint8_t)bad_crc;
    images[record + 21] = (uint8_t)(bad_crc >> 8);
    images[record + 22] = (uint8_t)(bad_crc >> 16);
    images[record + 23] = (uint8_t)(bad_crc >> 24);
    assert(!nine_image_show_asset_op_ex(479, &r));
    assert(r.crc_checks == 1 && r.jpeg_decodes == 1 && !r.ok);
    for (unsigned y=0;y<NINE_IMAGE_H;y++) for (unsigned x=0;x<NINE_IMAGE_W;x++)
        assert(nine_image_test_pixel(x,y) == 0);
    op_payload[length - 8] = saved_op;
    images[record + 20] = (uint8_t)saved_crc;
    images[record + 21] = (uint8_t)(saved_crc >> 8);
    images[record + 22] = (uint8_t)(saved_crc >> 16);
    images[record + 23] = (uint8_t)(saved_crc >> 24);

    // Bounds failure must also clear and never call JPEG decode.
    const uint32_t saved_off = read32(images + record + 12);
    images[record + 12] = 0xf0; images[record + 13] = 0xff;
    images[record + 14] = 0xff; images[record + 15] = 0xff;
    assert(!nine_image_show_asset_op_ex(479, &r));
    assert(r.crc_checks == 0 && r.jpeg_decodes == 0 && !r.ok);
    for (unsigned y=0;y<NINE_IMAGE_H;y++) for (unsigned x=0;x<NINE_IMAGE_W;x++)
        assert(nine_image_test_pixel(x,y) == 0);
    images[record + 12] = (uint8_t)saved_off;
    images[record + 13] = (uint8_t)(saved_off >> 8);
    images[record + 14] = (uint8_t)(saved_off >> 16);
    images[record + 15] = (uint8_t)(saved_off >> 24);
    assert(nine_image_show_asset_op(479));

    // A non-JPEG record (indexed) is an invalid OP kind: explicit reject with
    // no CRC and no JPEG attempt; all bands clear.
    const uint32_t saved_kind = read32(images + record);
    images[record + 0] = 1; images[record + 1] = 0;
    images[record + 2] = 0; images[record + 3] = 0;
    assert(!nine_image_show_asset_op_ex(479, &r));
    assert(r.crc_checks == 0 && r.jpeg_decodes == 0 && !r.ok);
    for (unsigned y=0;y<NINE_IMAGE_H;y++) for (unsigned x=0;x<NINE_IMAGE_W;x++)
        assert(nine_image_test_pixel(x,y) == 0);
    images[record + 0] = (uint8_t)saved_kind;
    images[record + 1] = (uint8_t)(saved_kind >> 8);
    images[record + 2] = (uint8_t)(saved_kind >> 16);
    images[record + 3] = (uint8_t)(saved_kind >> 24);

    nine_image_deinit();
    free(script);
    free(images);
    puts("nine image corrupt-frame rejection: PASS");
    return 0;
}
