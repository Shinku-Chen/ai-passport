// Chapter-direct resetBook regression: onlyDirect must read the mutable
// isDebugDirectJump variable, not the session debug_direct flag. After the
// System_Subroutine clears it, Miria/Nanoka must remain at v2 at Adv02 cur4.
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
static int book_version(const gal_pack_t *p, const gal_vm_t *vm, const char *id)
{
    for (uint32_t i = 0; i < p->count[GAL_SEC_BOOKKEYS]; ++i) {
        const gal_bookkey_t *k = gal_pack_bookkey(p, (uint16_t)i);
        if (k && strcmp(gal_pack_str(p, (uint16_t)k->id), id) == 0) return vm->book[i];
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) { fprintf(stderr, "pack: %s\n", err); return 1; }
    const int start = scene_by_id(&pack, "Act01_Chapter01_Adv35");
    const int target = scene_by_id(&pack, "Act01_Chapter02_Adv02");
    CHECK(start >= 0 && target >= 0, "scenes missing");
    if (start < 0 || target < 0) return 1;
    gal_vm_t vm;
    gal_vm_init(&vm, &pack);
    CHECK(gal_vm_start(&vm, (uint16_t)start, false, false, &err), "start %s", err ? err : "?");
    int reached = 0;
    for (int i = 0; i < 20000; ++i) {
        if (vm.chapter == (uint16_t)target && vm.cursor >= 4) { reached = 1; break; }
        gal_frame_t f;
        const gal_frame_type_t t = gal_vm_read(&vm, &f);
        if (t == GAL_FRAME_TEXT) gal_vm_advance(&vm);
        else if (t == GAL_FRAME_CHOICE) {
            if (!gal_vm_choose(&vm, 0, &err)) break;
        } else if (t == GAL_FRAME_END || t == GAL_FRAME_ERROR) break;
    }
    CHECK(reached, "did not reach Adv02 cur4 (chapter=%u cursor=%u err=%s)",
          vm.chapter, vm.cursor, vm.error ? vm.error : "");
    if (reached) {
        const int miria = book_version(&pack, &vm, "Miria");
        const int nanoka = book_version(&pack, &vm, "Nanoka");
        printf("direct_book Miria=%d Nanoka=%d cursor=%u\n", miria, nanoka, vm.cursor);
        CHECK(miria == 2 && nanoka == 2, "book versions Miria=%d Nanoka=%d", miria, nanoka);
    }
    printf("test_gal_direct_book: %s\n", failures ? "FAIL" : "PASS");
    free(data);
    return failures ? 1 : 0;
}
