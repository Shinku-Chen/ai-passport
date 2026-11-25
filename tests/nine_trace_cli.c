// C side of the source-JS/C VM differential trace. Reads the real packed script
// and replays an explicit visible-choice sequence.
#include "nine_pack.h"
#include "nine_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct {
    const char *name;
    int32_t flag;
    int32_t progress;
} pair_t;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const pair_t *)a)->name, ((const pair_t *)b)->name);
}

static uint32_t fnv_bytes(uint32_t h, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_text(const char *text)
{
    return fnv_bytes(2166136261u, text ? text : "", text ? strlen(text) : 0);
}

static uint32_t hash_map(const nine_pack_t *pack, const nine_vm_t *vm)
{
    pair_t pairs[NINE_KEY_MAX];
    for (uint32_t i = 0; i < pack->key_count; ++i) {
        pairs[i].name = nine_pack_key(pack, (uint16_t)i);
        pairs[i].flag = vm->state.flag[i];
        pairs[i].progress = vm->state.progress[i];
    }
    qsort(pairs, pack->key_count, sizeof(pairs[0]), by_name);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < pack->key_count; ++i) {
        h = fnv_bytes(h, pairs[i].name, strlen(pairs[i].name));
        h = fnv_bytes(h, &pairs[i].flag, sizeof(pairs[i].flag));
        h = fnv_bytes(h, &pairs[i].progress, sizeof(pairs[i].progress));
    }
    return h;
}

static uint32_t hash_labels(const nine_pack_t *pack, const nine_view_t *view)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < view->choice_count; ++i) {
        nine_edge_t edge;
        if (!nine_pack_edge(pack, true, view->choices[i], &edge)) continue;
        const char *label = nine_pack_string(pack, edge.label);
        h = fnv_bytes(h, label ? label : "", label ? strlen(label) : 0);
        if (i + 1 < view->choice_count) {
            const uint8_t sep = 0;
            h = fnv_bytes(h, &sep, 1);
        }
    }
    return h;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) return 2;
    fclose(file);
    nine_pack_t pack;
    if (!nine_pack_open(&pack, data, (size_t)length)) return 2;
    int choices[64] = { 0 };
    int choice_count = 0;
    if (argc >= 3) {
        const char *p = argv[2];
        while (*p && choice_count < 64) {
            char *end = NULL;
            const long value = strtol(p, &end, 10);
            if (end == p) break;
            choices[choice_count++] = (int)value;
            p = end;
            if (*p == ',') p++;
        }
    }
    const int limit = argc >= 4 ? atoi(argv[3]) : 20000;
    nine_vm_t vm;
    if (!nine_vm_init(&vm, &pack)) return 2;
    if (!nine_vm_start(&vm, NINE_NODE_NONE, false)) return 2;
    int choice_pos = 0;
    for (int step = 0; step < limit; ++step) {
        const nine_view_t *view = nine_vm_locate(&vm);
        if (!view) return 2;
        int kind = 4;
        const char *text = "";
        const char *speaker = "";
        if (view->kind == NINE_VM_END) kind = 3;
        else if (view->kind == NINE_VM_CHOICE) kind = 2;
        else if (view->kind == NINE_VM_OP) kind = 1;
        else if (view->kind == NINE_VM_TEXT) {
            kind = 0;
            nine_frame_t frame;
            if (nine_pack_frame(&pack, view->frame, &frame)) {
                text = nine_pack_text(&pack, &frame);
                if (!text) text = "";
                speaker = nine_pack_string(&pack, frame.speaker);
                if (!speaker) speaker = "";
            }
        }
        const char *node_name = "@end";
        nine_node_t node;
        if (vm.state.node != NINE_NODE_NONE && nine_pack_node(&pack, vm.state.node, &node))
            node_name = nine_pack_string(&pack, node.name);
        printf("%d %s %u %08x %08x %08x %08x %u\n", kind, node_name, vm.state.index,
               hash_text(text), hash_text(speaker), hash_map(&pack, &vm),
               hash_labels(&pack, view), view->choice_count);
        if (kind == 3) break;
        if (kind == 2) {
            const int pick = choice_pos < choice_count ? choices[choice_pos++] : 0;
            if (!nine_vm_choose(&vm, (uint32_t)pick)) break;
        } else if (kind == 0 || kind == 1) {
            if (nine_vm_advance(&vm)->kind == NINE_VM_ERROR) break;
        } else {
            break;
        }
    }
    nine_vm_deinit(&vm);
    free(data);
    return 0;
}
