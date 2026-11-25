// VM persistence / preview / corruption regression on the real package.
// Covers: save roundtrip, byte-level CRC rejection, permanent-record merge on
// load, chapter preview suppressing persistent writes, and 24-step history.
//
// Usage: test_nine_vm_state <nine_script.bin>
#include "nine_pack.h"
#include "nine_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { ++failures; fprintf(stderr, "FAIL %d %s\n", __LINE__, m); } } while (0)

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static bool frames_equal(const nine_state_t *a, const nine_state_t *b)
{
    return a->node == b->node && a->index == b->index && a->preview == b->preview &&
           !memcmp(a->flag, b->flag, sizeof(a->flag)) &&
           !memcmp(a->progress, b->progress, sizeof(a->progress)) &&
           !memcmp(a->flag_set, b->flag_set, sizeof(a->flag_set)) &&
           !memcmp(a->progress_set, b->progress_set, sizeof(a->progress_set));
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <script.bin>\n", argv[0]); return 2; }
    size_t size = 0;
    uint8_t *script = slurp(argv[1], &size);
    static nine_pack_t pack;
    static nine_vm_t vm, vm2;
    CHECK(script && nine_pack_open(&pack, script, size), "pack opens");
    CHECK(nine_vm_init(&vm, &pack), "vm init");
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, false), "start");

    // Move 100 frames, then roundtrip a save.
    for (int i = 0; i < 100; ++i) CHECK(nine_vm_advance(&vm) != NULL, "advance");
    nine_state_t at100 = vm.state;
    uint8_t save[512];
    const size_t saved = nine_vm_save_encode(&vm, 3, save, sizeof(save));
    CHECK(saved > 0, "save encodes");
    for (int i = 0; i < 25; ++i) nine_vm_advance(&vm);
    CHECK(!frames_equal(&vm.state, &at100), "state changed after save");
    uint16_t page = 0;
    CHECK(nine_vm_save_decode(&vm, save, saved, &page), "save decodes");
    CHECK(page == 3, "page restored");
    CHECK(frames_equal(&vm.state, &at100), "save roundtrip restores the exact state");

    // Any single-byte corruption must be rejected without touching live state.
    nine_state_t live = vm.state;
    for (size_t i = 0; i < saved; i += 7) {
        uint8_t bad[512];
        memcpy(bad, save, saved);
        bad[i] ^= 0x5a;
        CHECK(!nine_vm_save_decode(&vm, bad, saved, &page), "tampered save rejected");
        CHECK(frames_equal(&vm.state, &live), "tampered save leaves live state intact");
    }
    uint8_t short_blob[16] = { 0 };
    CHECK(!nine_vm_save_decode(&vm, short_blob, sizeof(short_blob), &page), "short blob rejected");

    // Chapter preview: persistent effects and completion are suppressed.
    const int title_bg = nine_pack_find_key(&pack, "titleBg");
    const int loop1 = nine_pack_find_key(&pack, "loop1");
    const int loop_hk = nine_pack_find_key(&pack, "loop_hk");
    CHECK(title_bg >= 0 && loop1 >= 0 && loop_hk >= 0, "story keys");

    CHECK(nine_vm_init(&vm2, &pack), "vm2 init");
    CHECK(nine_vm_start(&vm2, 40, true), "preview start at n40");
    const nine_view_t *v = nine_vm_locate(&vm2);
    CHECK(v && v->kind == NINE_VM_CHOICE && v->choice_count == 1, "n40 choice");
    CHECK(vm2.state.progress[title_bg] == 0, "preview skips titleBg");
    CHECK(vm2.state.progress[loop1] == 0 && vm2.state.progress[loop_hk] == 0,
          "preview skips loop effects");
    CHECK(nine_vm_choose(&vm2, 0) != NULL, "preview choose");
    CHECK(vm2.state.progress[title_bg] == 0 && vm2.state.progress[loop1] == 0 &&
          vm2.state.progress[loop_hk] == 0, "preview writes no permanent state");
    CHECK(vm2.state.preview, "preview flag survives");

    // Normal play applies the same effects and records completion.
    CHECK(nine_vm_start(&vm2, 40, false), "normal start at n40");
    v = nine_vm_locate(&vm2);
    CHECK(v && v->kind == NINE_VM_CHOICE, "n40 choice normal");
    CHECK(vm2.state.progress[title_bg] == 1, "normal applies titleBg");
    uint8_t before_choice[512];
    const size_t before_size = nine_vm_save_encode(&vm2, 0, before_choice, sizeof(before_choice));
    CHECK(before_size > 0, "save at the choice");
    CHECK(nine_vm_choose(&vm2, 0) != NULL, "normal choose");
    CHECK(vm2.state.progress[loop1] == 1 && vm2.state.progress[loop_hk] == 1,
          "normal choice records loop flags");
    CHECK(vm2.state.progress[title_bg] == 1, "titleBg retained");

    // Loading the older snapshot keeps the newest permanent record (engine.js
    // merges _progress over the saved state) but restores position and flags.
    uint16_t pos_node = 0;
    uint32_t pos_index = 0;
    CHECK(nine_vm_save_encode(&vm2, 2, save, sizeof(save)) > 0, "encode newest");
    CHECK(nine_vm_save_decode(&vm2, before_choice, before_size, &page), "load old snapshot");
    CHECK(vm2.state.node == 40, "old snapshot position");
    CHECK(vm2.state.progress[loop1] == 1 && vm2.state.progress[loop_hk] == 1,
          "newest permanent record survives an old load");
    CHECK(vm2.state.progress[title_bg] == 1, "titleBg survives");
    (void)pos_node; (void)pos_index;

    // Progress record roundtrip.
    uint8_t perm[512];
    const size_t perm_size = nine_vm_progress_encode(&vm2, perm, sizeof(perm));
    CHECK(perm_size > 0, "progress encodes");
    nine_state_t progress_before = vm2.state;
    CHECK(nine_vm_progress_decode(&vm2, perm, perm_size), "progress decodes");
    CHECK(vm2.state.progress[loop1] == 1, "progress loop1");
    CHECK(frames_equal(&vm2.state, &progress_before) ||
          (vm2.state.node == progress_before.node), "progress decode keeps position");
    uint8_t bad_perm[512];
    memcpy(bad_perm, perm, perm_size);
    bad_perm[0] ^= 0xff;
    CHECK(!nine_vm_progress_decode(&vm2, bad_perm, perm_size), "tampered progress rejected");

    // History keeps 24 steps; previous walks them back.
    CHECK(nine_vm_start(&vm2, NINE_NODE_NONE, false), "restart for history");
    for (int i = 0; i < 40; ++i) nine_vm_advance(&vm2);
    int back = 0;
    while (vm2.history_count > 0) { nine_vm_previous(&vm2); ++back; }
    CHECK(back == 24, "history is capped at 24");

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS vm state: save/load, corruption, preview, permanent merge, history\n");
    free(script);
    return 0;
}
