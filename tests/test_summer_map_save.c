// tests/test_summer_map_save.c — map visited bits survive encode/decode and
// suppress the chosen item on the same map, including non-zero high bits.
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


// Walk the real first-choice/first-map story until the requested map op.
static int walk_to_map(summer_vm_t *vm, const summer_pack_t *pack,
                       uint16_t scene, uint32_t op)
{
    for (int i = 0; i < 400000; ++i) {
        const summer_step_t st = summer_vm_step(vm, pack, 256);
        if (st.kind == SUMMER_STEP_MAP) {
            if (vm->scene_id == scene && vm->op_index == op) return 1;
            summer_map_choice_t c[40];
            const int n = summer_vm_preview_map(vm, pack, st.group, c, 40);
            if (n <= 0 || !summer_vm_choose_map(vm, pack, st.group, c[0].item_index)) return 0;
        } else if (st.kind == SUMMER_STEP_TEXT) {
            if (!summer_vm_advance(vm, pack)) return 0;
        } else if (st.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int n = summer_vm_visible_choices(vm, pack, st.group, items, 32);
            if (n <= 0 || !summer_vm_choose(vm, pack, st.group, items[0])) return 0;
        } else {
            return 0;
        }
    }
    return 0;
}

// The exact reviewer scenario: 281:1468 repeats five times; after choosing a
// destination, its visited bit must survive save/load and keep the option gone.
static int test_1468(const summer_pack_t *pack, const summer_progress_t *progress)
{
    summer_vm_t vm;
    if (!summer_vm_start_story(&vm, pack)) return 0;
    if (!walk_to_map(&vm, pack, 281, 1468)) return 0;

    summer_step_t st = summer_vm_step(&vm, pack, 256);
    if (st.kind != SUMMER_STEP_MAP) return 0;
    summer_map_choice_t choices[40];
    const int n = summer_vm_preview_map(&vm, pack, st.group, choices, 40);
    CHECK(n > 1, "281:1468 first preview %d", n);
    const int chosen = choices[0].item_index;
    const int chosen_value = choices[0].value;

    uint8_t buf[SUMMER_SAVE_CAP];
    size_t len = summer_save_encode(&vm, 0, progress, buf, sizeof(buf));
    CHECK(len > 0, "281:1468 encode");
    summer_save_t save;
    char err[64];
    CHECK(summer_save_decode(pack, buf, len, &save, err, sizeof(err)),
          "281:1468 decode: %s", err);
    CHECK(memcmp(save.map_bits, vm.map_bits, sizeof(save.map_bits)) == 0,
          "281:1468 bits changed before choose");

    CHECK(summer_vm_choose_map(&vm, pack, st.group, chosen), "281:1468 choose");
    if (!walk_to_map(&vm, pack, 281, 1468)) return 0;
    st = summer_vm_step(&vm, pack, 256);
    if (st.kind != SUMMER_STEP_MAP) return 0;
    summer_map_choice_t again[40];
    const int n2 = summer_vm_preview_map(&vm, pack, st.group, again, 40);
    CHECK(n2 == n - 1, "281:1468 repeat preview %d -> %d", n, n2);
    for (int i = 0; i < n2; ++i) CHECK(again[i].value != chosen_value, "chosen 1468 value returned");

    len = summer_save_encode(&vm, 0, progress, buf, sizeof(buf));
    CHECK(len > 0, "281:1468 repeat encode");
    CHECK(summer_save_decode(pack, buf, len, &save, err, sizeof(err)),
          "281:1468 repeat decode: %s", err);
    CHECK(memcmp(save.map_bits, vm.map_bits, sizeof(save.map_bits)) == 0,
          "281:1468 repeat bits changed");
    summer_vm_t restored;
    CHECK(summer_save_to_vm(&save, &restored, pack), "281:1468 restore");
    summer_step_t rst = summer_vm_step(&restored, pack, 256);
    CHECK(rst.kind == SUMMER_STEP_MAP, "281:1468 restored step %d", rst.kind);
    summer_map_choice_t back[40];
    const int n3 = summer_vm_preview_map(&restored, pack, rst.group, back, 40);
    CHECK(n3 == n2, "281:1468 restored preview %d -> %d", n2, n3);
    printf("  281:1468 first n=%d repeat n=%d restored n=%d", n, n2, n3);
    putchar(10);
    for (int i = 0; i < n3; ++i) CHECK(back[i].value != chosen_value, "restored 1468 value returned");
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/summer_data/summer_pack.bin";
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) { fprintf(stderr, "SKIP: cannot read %s\n", path); return 77; }
    summer_pack_t pack;
    CHECK(summer_pack_open(&pack, blob, (uint32_t)size, NULL), "pack");

    summer_vm_t vm;
    CHECK(summer_vm_start_story(&vm, &pack), "start");
    summer_step_t st = { 0 };
    for (int i = 0; i < 5000; ++i) {
        st = summer_vm_step(&vm, &pack, 256);
        if (st.kind == SUMMER_STEP_MAP) break;
        if (st.kind == SUMMER_STEP_TEXT) {
            if (!summer_vm_advance(&vm, &pack)) break;
        } else if (st.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int n = summer_vm_visible_choices(&vm, &pack, st.group, items, 32);
            if (n <= 0 || !summer_vm_choose(&vm, &pack, st.group, items[0])) break;
        } else {
            break;
        }
    }
    CHECK(st.kind == SUMMER_STEP_MAP, "no map reached");
    if (st.kind != SUMMER_STEP_MAP) { free(blob); return 1; }
    summer_map_choice_t choices[40];
    const int n = summer_vm_preview_map(&vm, &pack, st.group, choices, 40);
    CHECK(n > 1, "map choices %d", n);
    const int chosen_value = choices[0].value;
    const int chosen_item = choices[0].item_index;

    // Save at the map op: no bits yet.
    summer_progress_t progress;
    summer_progress_init(&progress);
    uint8_t save_buf[SUMMER_SAVE_CAP];
    size_t len = summer_save_encode(&vm, 0, &progress, save_buf, sizeof(save_buf));
    CHECK(len > 0, "encode at map");
    summer_save_t save;
    char err[64];
    CHECK(summer_save_decode(&pack, save_buf, len, &save, err, sizeof(err)), "decode at map: %s", err);
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(save.map_bits); ++i) nonzero += save.map_bits[i] != 0;
    CHECK(nonzero == 0, "map bits not zero at map op");

    // Choose, then save: the bit must persist byte-for-byte.
    CHECK(summer_vm_choose_map(&vm, &pack, st.group, chosen_item), "choose map");
    const uint32_t bit = (uint32_t)st.group * SUMMER_MAP_VALUES + (uint32_t)chosen_item;
    CHECK(vm.map_bits[bit / 8u] & (uint8_t)(1u << (bit % 8u)), "live map bit");
    len = summer_save_encode(&vm, 0, &progress, save_buf, sizeof(save_buf));
    CHECK(len > 0, "encode after choose");
    CHECK(summer_save_decode(&pack, save_buf, len, &save, err, sizeof(err)), "decode after choose: %s", err);
    CHECK(memcmp(save.map_bits, vm.map_bits, sizeof(save.map_bits)) == 0,
          "map bits changed in codec");
    CHECK(save.map_bits[bit / 8u] & (uint8_t)(1u << (bit % 8u)), "chosen bit lost");

    // Preview the same map with the post-choice bits: the chosen value is gone.
    summer_vm_t restored;
    CHECK(summer_save_to_vm(&save, &restored, &pack), "restore after choose");
    // The save is after the map, so restore a copy at the map op and copy bits.
    // Rewind to the saved pre-choice position via the pre-choice save.
    len = summer_save_encode(&vm, 0, &progress, save_buf, sizeof(save_buf));
    (void)len;
    // Re-create the map VM from the first save (before choose) is simpler:
    summer_vm_t pre;
    CHECK(summer_vm_start_story(&pre, &pack), "pre start");
    for (int i = 0; i < 5000; ++i) {
        const summer_step_t s2 = summer_vm_step(&pre, &pack, 256);
        if (s2.kind == SUMMER_STEP_MAP) break;
        if (s2.kind == SUMMER_STEP_TEXT) summer_vm_advance(&pre, &pack);
        else if (s2.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int m = summer_vm_visible_choices(&pre, &pack, s2.group, items, 32);
            if (m <= 0 || !summer_vm_choose(&pre, &pack, s2.group, items[0])) break;
        } else break;
    }
    memcpy(pre.map_bits, vm.map_bits, sizeof(pre.map_bits));
    len = summer_save_encode(&pre, 0, &progress, save_buf, sizeof(save_buf));
    CHECK(summer_save_decode(&pack, save_buf, len, &save, err, sizeof(err)), "decode pre+bits");
    CHECK(summer_save_to_vm(&save, &restored, &pack), "restore pre+bits");
    summer_map_choice_t after[40];
    const int n2 = summer_vm_preview_map(&restored, &pack, st.group, after, 40);
    CHECK(n2 == n - 1, "preview count %d -> %d", n, n2);
    for (int i = 0; i < n2; ++i) CHECK(after[i].value != chosen_value, "chosen value reappeared");

    CHECK(test_1468(&pack, &progress), "281:1468 regression");

    // High bit (last group/item) and low bit round-trip.
    memset(pre.map_bits, 0, sizeof(pre.map_bits));
    const uint32_t high = 26u * SUMMER_MAP_VALUES + 33u;
    pre.map_bits[high / 8u] |= (uint8_t)(1u << (high % 8u));
    pre.map_bits[0] |= 1u;
    len = summer_save_encode(&pre, 0, &progress, save_buf, sizeof(save_buf));
    CHECK(summer_save_decode(&pack, save_buf, len, &save, err, sizeof(err)), "decode high bits");
    CHECK(memcmp(save.map_bits, pre.map_bits, sizeof(pre.map_bits)) == 0, "high/low bits lost");

    // Version 1 is explicitly rejected (version 2 has a u16 map length).
    uint8_t old[SUMMER_SAVE_CAP];
    memcpy(old, save_buf, len);
    old[4] = 1;
    old[5] = 0;
    summer_save_t old_save;
    CHECK(!summer_save_decode(&pack, old, len, &old_save, err, sizeof(err)), "v1 save accepted");

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_map_save: PASS (map %u:%u n=%d chosen=%d)\n", vm.scene_id,
           vm.op_index, n, chosen_value);
    return 0;
}
