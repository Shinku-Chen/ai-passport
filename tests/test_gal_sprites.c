// Sprite state pool regression: the source reaches 25/26 live sprites on
// Adv05 cur6/19; neither Jailer nor Jailer-Middle may be silently dropped.
#include "gal_pack.h"
#include "gal_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static int scene_by_id(const gal_pack_t *p, const char *id)
{
    for (uint32_t i = 0; i < p->count[GAL_SEC_SCENES]; ++i) {
        const gal_scene_t *s = gal_pack_scene(p, (uint16_t)i);
        if (s && strcmp(gal_pack_str(p, (uint16_t)s->id), id) == 0) return (int)i;
    }
    return -1;
}

static int has_sprite(const gal_vm_t *vm, const char *actor, const char *sub)
{
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        if (!vm->sprites[i].used) continue;
        if (strcmp(gal_pack_str(vm->pack, vm->sprites[i].actor), actor) == 0 &&
            strcmp(gal_pack_str(vm->pack, vm->sprites[i].sub), sub ? sub : "") == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) { fprintf(stderr, "pack: %s\n", err); return 1; }
    CHECK(GAL_MAX_SPRITES >= 30, "GAL_MAX_SPRITES=%u", GAL_MAX_SPRITES);

    const int scene = scene_by_id(&pack, "Act01_Chapter01_Adv05");
    CHECK(scene >= 0, "Adv05 missing");
    if (scene < 0) return 1;
    gal_vm_t vm;
    gal_vm_init(&vm, &pack);
    CHECK(gal_vm_start(&vm, (uint16_t)pack.meta[1], true, false, &err),
          "start %s", err ? err : "?");
    vm.cursor = 0;
    int seen6 = 0, seen19 = 0, seen_order = 0;
    for (int i = 0; i < 400; ++i) {
        const uint16_t before = vm.cursor;
        gal_frame_t f;
        const gal_frame_type_t t = gal_vm_read(&vm, &f);
        const gal_scene_t *cur = gal_pack_scene(&pack, vm.chapter);
        const int in_adv05 = cur && strcmp(gal_pack_str(&pack, (uint16_t)cur->id),
                                             "Act01_Chapter01_Adv05") == 0;
        const int in_adv04 = cur && strcmp(gal_pack_str(&pack, (uint16_t)cur->id),
                                             "Act01_Chapter01_Adv04") == 0;
        if (in_adv04 && vm.cursor >= 5 && !seen_order) {
            seen_order = 1;
            CHECK(vm.body_count == 2, "Adv04 body_count=%u", vm.body_count);
            if (vm.body_count == 2) {
                CHECK(strcmp(gal_pack_str(&pack, vm.bodies[0].actor), "Sherry") == 0 &&
                      strcmp(gal_pack_str(&pack, vm.bodies[1].actor), "Ema") == 0,
                      "Adv04 body order %s,%s", gal_pack_str(&pack, vm.bodies[0].actor),
                      gal_pack_str(&pack, vm.bodies[1].actor));
            }
        }
        if (in_adv05 && vm.cursor >= 6 && !seen6) {
            seen6 = 1;
            CHECK(vm.sprite_count >= 25, "cur6 sprite_count=%u (want >=25)", vm.sprite_count);
            CHECK(has_sprite(&vm, "Jailer", ""), "cur6 Jailer missing");
        }
        if (in_adv05 && vm.cursor >= 19 && !seen19) {
            seen19 = 1;
            CHECK(vm.sprite_count >= 26, "cur19 sprite_count=%u (want >=26)", vm.sprite_count);
            CHECK(has_sprite(&vm, "Jailer", ""), "cur19 Jailer missing");
            CHECK(has_sprite(&vm, "Jailer", "Middle"), "cur19 Jailer-Middle missing");
        }
        if (t == GAL_FRAME_TEXT) {
            if (in_adv05 && vm.cursor >= 24) break;
            gal_vm_advance(&vm);
        } else if (t == GAL_FRAME_CHOICE) {
            if (!gal_vm_choose(&vm, 0, &err)) break;
        } else if (t == GAL_FRAME_END || t == GAL_FRAME_ERROR) {
            break;
        }
        (void)before;
    }
    CHECK(seen6 && seen19, "did not reach cur6/19 (%d/%d)", seen6, seen19);
    CHECK(seen_order, "did not reach Adv04 cur5");
    printf("test_gal_sprites: %s max_sprite_count_seen=%u\n",
           failures ? "FAIL" : "PASS", (unsigned)vm.sprite_count);
    free(data);
    return failures ? 1 : 0;
}
