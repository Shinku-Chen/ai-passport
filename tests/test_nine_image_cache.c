// Host regression for the same-visual fast-forward cache. A simulated allocator
// failure is used as a sentinel: a cache hit must not attempt a decode, while
// asset/title/CG/OP/clear/error paths must invalidate it.
#include "nine_image.h"
#include "nine_pack.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv)
{
    assert(argc == 3);
    size_t script_size = 0, image_size = 0;
    uint8_t *script = load(argv[1], &script_size);
    uint8_t *images = load(argv[2], &image_size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, script, script_size));
    assert(nine_image_bind_bundle(script, script_size, images, image_size));
    assert(nine_image_init(NULL));

    // First decode fills the canvas and the same-visual cache.
    nine_image_test_reset_visual_counts();
    assert(nine_image_show_visual(11));
    assert(nine_image_test_visual_decode_count() == 1);
    assert(nine_image_test_visual_cache_hit_count() == 0);
    assert(nine_image_show_visual(11));
    assert(nine_image_test_visual_decode_count() == 1);
    assert(nine_image_test_visual_cache_hit_count() == 1);

    // Explicit invalidation forces a real decode.
    nine_image_invalidate_cache();
    assert(nine_image_show_visual(11));
    assert(nine_image_test_visual_decode_count() == 2);

    // A full asset (title/CG/OP use show_asset) must invalidate the visual.
    assert(nine_image_show_asset(12));
    assert(nine_image_show_visual(11));
    assert(nine_image_test_visual_decode_count() == 3);

    // Explicit clear also invalidates; the next visual must decode again.
    nine_image_clear(0);
    assert(nine_image_show_visual(11));
    assert(nine_image_test_visual_decode_count() == 4);

    nine_image_deinit();
    free(script);
    free(images);
    puts("nine image same-visual cache invalidation: PASS");
    return 0;
}
