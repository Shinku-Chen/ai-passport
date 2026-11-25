// Host dump of the production image compositor for the reference comparison.
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

int main(int argc, char **argv)
{
    assert(argc == 6);
    size_t script_size = 0, image_size = 0;
    uint8_t *script = load(argv[1], &script_size);
    uint8_t *images = load(argv[2], &image_size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, script, script_size));
    assert(nine_image_bind_bundle(script, script_size, images, image_size));
    assert(nine_image_init(NULL));
    const int id = atoi(argv[3]);
    if (strcmp(argv[4], "visual") == 0) {
        assert(nine_image_show_visual((uint32_t)id));
    } else {
        assert(strcmp(argv[4], "asset") == 0);
        assert(nine_image_show_asset((uint32_t)id));
    }
    assert(nine_image_test_largest_allocation() < 32768);
    FILE *out = fopen(argv[5], "wb");
    assert(out);
    for (unsigned y = 0; y < NINE_IMAGE_H; ++y) {
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) {
            const uint16_t pixel = nine_image_test_pixel(x, y);
            const uint8_t bytes[2] = { (uint8_t)pixel, (uint8_t)(pixel >> 8) };
            assert(fwrite(bytes, 1, 2, out) == 2);
        }
    }
    fclose(out);
    printf("dumped %s %d largest=%zu\n", argv[4], id,
           nine_image_test_largest_allocation());
    nine_image_deinit();
    free(script);
    free(images);
    return 0;
}
