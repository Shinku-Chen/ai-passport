// Fatal damage semantics: no cursor advance, end kind trialFail, trial_fail set.
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

static int scene_of_op(const gal_pack_t *p, uint32_t op_index)
{
    for (uint32_t i = 0; i < p->count[GAL_SEC_SCENES]; ++i) {
        const gal_scene_t *s = gal_pack_scene(p, (uint16_t)i);
        if (s && op_index >= s->op_start && op_index < s->op_start + s->op_count) return (int)i;
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

    uint32_t damage_op = UINT32_MAX;
    for (uint32_t i = 0; i < pack.count[GAL_SEC_OPS]; ++i) {
        const gal_op_t *o = gal_pack_op(&pack, i);
        if (o && o->op == GAL_OP_DAMAGE) { damage_op = i; break; }
    }
    CHECK(damage_op != UINT32_MAX, "no damage op");
    const int scene = scene_of_op(&pack, damage_op);
    CHECK(scene >= 0, "damage scene");
    const gal_scene_t *sc = gal_pack_scene(&pack, (uint16_t)scene);
    CHECK(sc && damage_op >= sc->op_start, "damage scene range");
    const uint16_t start = (uint16_t)(damage_op - sc->op_start);

    gal_vm_t vm;
    gal_vm_init(&vm, &pack);
    CHECK(gal_vm_start(&vm, (uint16_t)scene, true, false, &err), "start %s", err ? err : "?");
    vm.cursor = start;
    const uint16_t hp_id = gal_vm_var_id(&vm, "currentHp");
    CHECK(hp_id != GAL_NONE, "currentHp variable");
    if (hp_id != GAL_NONE) vm.vars[hp_id].num = 1.0f;
    gal_frame_t f;
    const gal_frame_type_t t = gal_vm_read(&vm, &f);
    CHECK(t == GAL_FRAME_END, "fatal damage type=%d", t);
    CHECK(vm.cursor == start, "fatal damage advanced cursor %u->%u", start, vm.cursor);
    CHECK(vm.trial_fail == 1, "trial_fail");
    CHECK(f.end_kind != GAL_NONE && strcmp(gal_pack_str(&pack, f.end_kind), "trialFail") == 0,
          "end kind=%s id=%u", f.end_kind == GAL_NONE ? "" : gal_pack_str(&pack, f.end_kind),
          f.end_kind);
    CHECK(vm.end_kind == f.end_kind, "vm/frame end kind");

    // Non-fatal damage consumes the op and keeps the trial alive.
    gal_vm_init(&vm, &pack);
    CHECK(gal_vm_start(&vm, (uint16_t)scene, true, false, &err), "start2");
    vm.cursor = start;
    if (hp_id != GAL_NONE) vm.vars[hp_id].num = 2.0f;
    const gal_frame_type_t nt = gal_vm_read(&vm, &f);
    CHECK(vm.cursor > start, "nonfatal cursor did not advance %u", vm.cursor);
    CHECK(!vm.ended && !vm.trial_fail, "nonfatal marked trial fail");
    CHECK(nt == GAL_FRAME_TEXT, "nonfatal type=%d", nt);

    printf("test_gal_damage: %s\n", failures ? "FAIL" : "PASS");
    free(data);
    return failures ? 1 : 0;
}
