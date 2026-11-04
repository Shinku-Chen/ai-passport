// tests/test_summer_save_preview.c — save preview = 20 UTF-8 characters + ellipsis.
#include "summer_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if ((*p & 0xC0u) != 0x80u) n++;
    }
    return n;
}

static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = (*p < 0x80) ? 1 : (*p < 0xE0) ? 2 : (*p < 0xF0) ? 3 : 4;
        if ((*p >= 0x80 && *p < 0xC2) || *p >= 0xF5) return false;
        for (int i = 1; i < n; ++i) if ((p[i] & 0xC0u) != 0x80u) return false;
        p += n;
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/summer_data/summer_pack.bin";
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) { fprintf(stderr, "SKIP: cannot read %s\n", path); return 77; }
    summer_pack_t pack;
    CHECK(summer_pack_open(&pack, blob, (uint32_t)size, NULL), "pack");

    int long_scene = -1;
    uint32_t long_op = 0;
    int short_scene = -1;
    uint32_t short_op = 0;
    for (uint32_t si = 0; si < pack.scene_count && long_scene < 0; ++si) {
        const summer_scene_t *scene = &pack.scenes[si];
        for (uint32_t opi = 0; opi < scene->op_count; ++opi) {
            uint32_t off = 0;
            summer_op_t op;
            if (!summer_op_seek(&pack, scene, opi, &off) ||
                !summer_op_decode(&pack, off, &op)) continue;
            if (op.kind != SUMMER_OP_TEXT) continue;
            char text[SUMMER_TEXT_CAP];
            if (!summer_pack_text_into(&pack, op.b, text, sizeof(text))) continue;
            const size_t chars = utf8_chars(text);
            if (chars > 20 && long_scene < 0) {
                long_scene = (int)scene->id;
                long_op = opi;
            }
            if (chars > 3 && chars <= 20 && short_scene < 0) {
                short_scene = (int)scene->id;
                short_op = opi;
            }
            if (long_scene >= 0 && short_scene >= 0) break;
        }
    }
    CHECK(long_scene >= 0 && short_scene >= 0, "no preview text fixture");

    summer_save_t save;
    memset(&save, 0, sizeof(save));
    char preview[128];
    if (long_scene >= 0) {
        save.scene_id = (uint16_t)long_scene;
        save.op_index = long_op;
        CHECK(summer_save_preview(&pack, &save, preview, sizeof(preview)), "long preview");
        CHECK(utf8_valid(preview), "preview is not valid UTF-8");
        CHECK(utf8_chars(preview) == 21, "preview chars %zu (20 + ellipsis)",
              utf8_chars(preview));
        CHECK(strstr(preview, "…") != NULL, "preview lacks ellipsis: %s", preview);
        printf("long preview: [%s]\n", preview);
    }
    if (short_scene >= 0) {
        save.scene_id = (uint16_t)short_scene;
        save.op_index = short_op;
        CHECK(summer_save_preview(&pack, &save, preview, sizeof(preview)), "short preview");
        CHECK(utf8_valid(preview), "short preview is not valid UTF-8");
        CHECK(strstr(preview, "…") == NULL, "short preview has ellipsis: %s", preview);
    }

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_save_preview: PASS\n");
    return 0;
}
