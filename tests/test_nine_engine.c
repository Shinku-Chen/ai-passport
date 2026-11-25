// Production reader/VM contracts: save, permanent progress, preview isolation,
// previous, chapter skip, corrupt records, filtered-choice fallback, hop limit.
#include "nine_pack.h"
#include "nine_vm.h"
#include "nine_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *size = (size_t)n; return b;
}
static bool same_state(const nine_state_t *a, const nine_state_t *b)
{
    return a->node == b->node && a->index == b->index && a->preview == b->preview &&
           memcmp(a->flag, b->flag, sizeof(a->flag)) == 0 &&
           memcmp(a->progress, b->progress, sizeof(a->progress)) == 0 &&
           memcmp(a->flag_set, b->flag_set, sizeof(a->flag_set)) == 0 &&
           memcmp(a->progress_set, b->progress_set, sizeof(a->progress_set)) == 0;
}
static void set_progress(nine_vm_t *vm, const char *key, int value)
{
    int k = nine_pack_find_key(vm->pack, key);
    if (k < 0) { printf("missing key %s\n", key); fails++; return; }
    vm->state.progress[k] = value;
    vm->state.progress_set[k / 8] |= (uint8_t)(1u << (k % 8));
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script.bin filter-fallback.bin\n", argv[0]); return 2; }
    size_t n, sn;
    uint8_t *pack_bytes = load(argv[1], &n);
    static nine_pack_t pack;
    CHECK(nine_pack_open(&pack, pack_bytes, n), "open real pack");
    CHECK(pack.node_count == 74, "74 nodes");
    CHECK(pack.frame_count == 8050, "8050 frames");
    CHECK(pack.chapter_count == 17, "17 chapters");
    CHECK(pack.ending_count == 2, "2 endings");
    CHECK(pack.key_count == 15, "15 variables");

    static nine_vm_t vm;
    CHECK(nine_vm_init(&vm, &pack), "vm init");
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, false), "start");
    CHECK(vm.view.kind == NINE_VM_TEXT, "first text");
    for (int i = 0; i < 20; ++i) nine_vm_advance(&vm);
    uint16_t page = 3;
    uint8_t save[NINE_SAVE_RECORD_MAX];
    size_t save_n = nine_vm_save_encode(&vm, page, save, sizeof(save));
    CHECK(save_n >= 40 && save_n < sizeof(save), "save encode");
    nine_state_t saved = vm.state;
    uint16_t saved_page = 0;
    for (int i = 0; i < 10; ++i) nine_vm_advance(&vm);
    CHECK(!same_state(&vm.state, &saved), "state moved");
    CHECK(nine_vm_save_decode(&vm, save, save_n, &saved_page), "save decode");
    CHECK(same_state(&vm.state, &saved), "save state restored");
    CHECK(saved_page == page, "page restored");

    nine_state_t before_bad = vm.state;
    save[20] ^= 0x40;
    CHECK(!nine_vm_save_decode(&vm, save, save_n, &saved_page), "corrupt save rejected");
    CHECK(same_state(&vm.state, &before_bad), "corrupt save does not touch state");
    save[20] ^= 0x40;

    /* Permanent progress is a separate record and never rolls back on load. */
    set_progress(&vm, "loop_my", 0);
    size_t pn = nine_vm_progress_encode(&vm, save, sizeof(save));
    CHECK(pn > 0 && pn < 160, "progress encode");
    CHECK(nine_vm_progress_decode(&vm, save, pn), "progress decode");
    set_progress(&vm, "loop_my", 1);
    save_n = nine_vm_save_encode(&vm, page, save, sizeof(save));
    nine_vm_t fresh;
    CHECK(nine_vm_init(&fresh, &pack), "fresh init");
    nine_vm_start(&fresh, NINE_NODE_NONE, false);
    CHECK(nine_vm_save_decode(&fresh, save, save_n, &saved_page), "decode into fresh");
    CHECK(nine_vm_condition_value(&fresh, (uint16_t)nine_pack_find_key(&pack, "loop_my")) == 1,
          "loop condition from progress");

    /* Preview writes run flags but never permanent progress. */
    uint8_t before_preview[NINE_PROGRESS_RECORD_MAX];
    size_t bp = nine_vm_progress_encode(&vm, before_preview, sizeof(before_preview));
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, true), "preview start");
    for (int i = 0; i < 500; ++i) {
        if (vm.view.kind != NINE_VM_TEXT && vm.view.kind != NINE_VM_OP) break;
        nine_vm_advance(&vm);
    }
    CHECK(vm.state.preview, "preview remains marked");
    uint8_t after_preview[NINE_PROGRESS_RECORD_MAX];
    size_t ap = nine_vm_progress_encode(&vm, after_preview, sizeof(after_preview));
    CHECK(bp == ap && memcmp(before_preview, after_preview, bp) == 0,
          "preview does not pollute permanent progress");

    /* previous restores a full checkpoint. */
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, false), "start for previous");
    for (int i = 0; i < 10; ++i) nine_vm_advance(&vm);
    nine_state_t mark = vm.state;
    for (int i = 0; i < 5; ++i) nine_vm_advance(&vm);
    for (int i = 0; i < 5; ++i) nine_vm_previous(&vm);
    CHECK(same_state(&vm.state, &mark), "previous restores checkpoint");

    /* Save/load at a real choice in the second-playthrough graph. */
    set_progress(&vm, "loop_my", 1);
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, false), "second run start");
    for (int i = 0; i < 101; ++i) nine_vm_advance(&vm);
    CHECK(vm.view.kind == NINE_VM_CHOICE, "reached n1 choice");
    CHECK(vm.view.choice_count == 2, "n1 has two choices");
    CHECK(nine_vm_choose(&vm, 0) != NULL, "choose");
    save_n = nine_vm_save_encode(&vm, 0, save, sizeof(save));
    nine_state_t after_choice = vm.state;
    nine_vm_advance(&vm);
    CHECK(nine_vm_save_decode(&vm, save, save_n, &saved_page), "choice save decode");
    CHECK(same_state(&vm.state, &after_choice), "choice save restores flags");

    /* Chapter skip stops at the next chapter boundary. */
    CHECK(nine_vm_start(&vm, NINE_NODE_NONE, false), "start for skip");
    for (int i = 0; i < 50; ++i) nine_vm_advance(&vm);
    const nine_view_t *v = nine_vm_skip_chapter(&vm);
    CHECK(v != NULL && v->kind != NINE_VM_ERROR, "skip chapter");

    /* Corrupt the script CRC and reject the pack before any state is applied. */
    pack_bytes[300] ^= 0x01;
    static nine_pack_t bad;
    CHECK(!nine_pack_open(&bad, pack_bytes, n), "corrupt pack rejected");
    pack_bytes[300] ^= 0x01;

    /* Synthetic graph: all choices filtered -> fallback, and the hop limit. */
    uint8_t *fixture = load(argv[2], &sn);
    static nine_pack_t fp;
    static nine_vm_t fv;
    CHECK(nine_pack_open(&fp, fixture, sn), "open fixture");
    CHECK(nine_vm_init(&fv, &fp), "fixture vm");
    CHECK(nine_vm_start(&fv, NINE_NODE_NONE, false), "fixture start");
    CHECK(fv.view.kind == NINE_VM_CHOICE && fv.view.choice_count == 2,
          "filtered choices fall back to all options");
    CHECK(nine_vm_choose(&fv, 1) != NULL && fv.view.kind == NINE_VM_END,
          "fallback choice target");
    int loop = nine_pack_find_node(&fp, "n3");
    CHECK(loop >= 0, "loop node");
    (void)nine_vm_start(&fv, (uint16_t)loop, false);   // a self-loop cannot locate
    CHECK(fv.view.kind == NINE_VM_ERROR && fv.view.error &&
          strcmp(fv.view.error, "graph hop limit") == 0, "hop limit");

    printf("fails=%d\n", fails);
    free(pack_bytes); free(fixture);
    return fails ? 1 : 0;
}
