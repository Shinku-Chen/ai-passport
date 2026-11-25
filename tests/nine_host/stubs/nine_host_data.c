// File-backed replacement for the embedded package symbols in host tests.
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint8_t *s_script;
static size_t s_script_size;
static uint8_t *s_images;
static size_t s_images_size;

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

bool nine_host_load(const char *script_path, const char *images_path)
{
    free(s_script);
    free(s_images);
    s_script = slurp(script_path, &s_script_size);
    s_images = slurp(images_path, &s_images_size);
    return s_script && s_images;
}

void nine_host_flip_script(size_t offset)
{
    if (s_script && offset < s_script_size) s_script[offset] ^= 0x5a;
}

void nine_host_flip_images(size_t offset)
{
    if (s_images && offset < s_images_size) s_images[offset] ^= 0x5a;
}

const uint8_t *nine_host_script(size_t *size)
{
    *size = s_script_size;
    return s_script;
}

const uint8_t *nine_host_images(size_t *size)
{
    *size = s_images_size;
    return s_images;
}
