#include "nine_assets.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *s_script;
static uint8_t *s_images;
static size_t s_script_size, s_images_size;

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

int nine_assets_host_load(const char *script, const char *images)
{
    s_script = load(script, &s_script_size);
    s_images = load(images, &s_images_size);
    return s_script && s_images && s_script_size && s_images_size;
}

const uint8_t *nine_assets_script(size_t *size)
{
    if (size) *size = s_script_size;
    return s_script;
}

const uint8_t *nine_assets_images(size_t *size)
{
    if (size) *size = s_images_size;
    return s_images;
}
