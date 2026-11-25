// Differential harness: the C reader VM must reproduce the source JS engine's
// per-step node, index, text, flag, progress and visible-choice state for every
// recorded action (tools/nine_trace.mjs).
#include "nine_pack.h"
#include "nine_vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint32_t fnv_bytes(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t fnv_u32(uint32_t h, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                           (uint8_t)(v >> 24) };
    return fnv_bytes(h, b, 4);
}

static uint32_t hash_string(const char *s)
{
    return fnv_bytes(0x811c9dc5u, (const uint8_t *)(s ? s : ""), strlen(s ? s : ""));
}

static uint32_t state_hash(const nine_vm_t *vm, bool progress)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < vm->pack->key_count; ++i)
        h = fnv_u32(h, (uint32_t)(progress ? vm->state.progress[i] : vm->state.flag[i]));
    return h;
}

static uint32_t choice_hash(nine_vm_t *vm)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < vm->view.choice_count; ++i) {
        nine_edge_t e;
        if (!nine_pack_edge(vm->pack, true, vm->view.choices[i], &e)) return 0;
        const char *label = nine_pack_string(vm->pack, e.label);
        h = fnv_u32(h, hash_string(label));
        h = fnv_u32(h, e.to == NINE_NODE_NONE ? 0xffffffffu : e.to);
        for (uint32_t k = 0; k < e.effect_count; ++k) {
            nine_effect_t ef;
            if (!nine_pack_effect(vm->pack, e.first_effect + k, &ef)) return 0;
            h = fnv_u32(h, ef.key);
            h = fnv_u32(h, ef.add ? 1u : 0u);
            h = fnv_u32(h, (uint32_t)ef.value);
        }
    }
    return h;
}

static bool read_all(const char *path, uint8_t **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (!p) { fclose(f); return false; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return false; }
    fclose(f);
    *out = p;
    *size = (size_t)n;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <nine_script.bin> <nine_trace.bin>\n", argv[0]);
        return 2;
    }
    uint8_t *script = NULL, *trace = NULL;
    size_t script_size = 0, trace_size = 0;
    if (!read_all(argv[1], &script, &script_size) || !read_all(argv[2], &trace, &trace_size)) {
        fprintf(stderr, "cannot read inputs\n");
        return 2;
    }
    nine_pack_t pack;
    if (!nine_pack_open(&pack, script, script_size)) {
        fprintf(stderr, "pack rejected\n");
        return 2;
    }
    if (trace_size < 12 || memcmp(trace, "NVT1", 4) || rd32(trace + 4) != 1) {
        fprintf(stderr, "bad trace header\n");
        return 2;
    }
    const uint32_t traces = rd32(trace + 8);
    size_t pos = 12;
    nine_vm_t vm;
    nine_vm_init(&vm, &pack);
    unsigned failures = 0, compared_steps = 0;
    uint32_t max_node = 0, max_index = 0;
    for (uint32_t t = 0; t < traces && failures < 20; ++t) {
        if (pos + 8 > trace_size) { fprintf(stderr, "truncated trace\n"); return 2; }
        const uint32_t steps = rd32(trace + pos);
        const uint32_t progress = rd32(trace + pos + 4);
        pos += 8;
        if (pos + (size_t)progress * 8 > trace_size) { fprintf(stderr, "truncated progress\n"); return 2; }
        memset(vm.state.flag, 0, sizeof(vm.state.flag));
        memset(vm.state.progress, 0, sizeof(vm.state.progress));
        memset(vm.state.flag_set, 0, sizeof(vm.state.flag_set));
        memset(vm.state.progress_set, 0, sizeof(vm.state.progress_set));
        for (uint32_t i = 0; i < progress; ++i) {
            const uint32_t key = rd32(trace + pos);
            const int32_t value = (int32_t)rd32(trace + pos + 4);
            pos += 8;
            if (key >= pack.key_count) { fprintf(stderr, "bad progress key\n"); return 2; }
            vm.state.progress[key] = value;
        }
        if (!nine_vm_start(&vm, NINE_NODE_NONE, false)) {
            fprintf(stderr, "trace %u: start failed\n", t);
            return 1;
        }
        (void)nine_vm_locate(&vm);
        for (uint32_t s = 0; s < steps; ++s) {
            if (pos + 36 > trace_size) { fprintf(stderr, "truncated step\n"); return 2; }
            const uint32_t type = rd32(trace + pos);
            const uint32_t node = rd32(trace + pos + 4);
            const uint32_t index = rd32(trace + pos + 8);
            const uint32_t text_hash = rd32(trace + pos + 12);
            const uint32_t flag_hash = rd32(trace + pos + 16);
            const uint32_t progress_hash = rd32(trace + pos + 20);
            const uint32_t choice_h = rd32(trace + pos + 24);
            const uint32_t action = rd32(trace + pos + 28);
            const uint32_t argument = rd32(trace + pos + 32);
            pos += 36;
            ++compared_steps;
            uint32_t c_type = 3;
            if (vm.view.kind == NINE_VM_TEXT) c_type = 0;
            else if (vm.view.kind == NINE_VM_OP) c_type = 1;
            else if (vm.view.kind == NINE_VM_CHOICE) c_type = 2;
            else if (vm.view.kind == NINE_VM_ERROR) c_type = 99;
            uint32_t c_text = 0;
            if (c_type == 0 || c_type == 1) {
                nine_frame_t frame;
                if (nine_vm_current_frame(&vm, &frame) && frame.kind == NINE_FRAME_TEXT) {
                    const char *txt = nine_pack_text(&pack, &frame);
                    c_text = hash_string(txt);
                }
            }
            const uint32_t c_flag = state_hash(&vm, false);
            const uint32_t c_progress = state_hash(&vm, true);
            const uint32_t c_choice = c_type == 2 ? choice_hash(&vm) : 0;
            if (c_type != type || vm.state.node != node || vm.state.index != index ||
                c_text != text_hash || c_flag != flag_hash || c_progress != progress_hash ||
                c_choice != choice_h) {
                if (failures < 20)
                    fprintf(stderr,
                            "FAIL trace %u step %u: type %u/%u node %u/%u index %u/%u "
                            "text %08x/%08x flag %08x/%08x progress %08x/%08x choice %08x/%08x\n",
                            t, s, c_type, type, (unsigned)vm.state.node, node,
                            (unsigned)vm.state.index, index, c_text, text_hash, c_flag,
                            flag_hash, c_progress, progress_hash, c_choice, choice_h);
                ++failures;
                break;
            }
            if (vm.state.node == NINE_NODE_NONE) {
                max_node = 0xffffffffu;
            } else if (vm.state.node > max_node) {
                max_node = vm.state.node;
            }
            if (vm.state.index > max_index) max_index = vm.state.index;
            if (type == 3) break;
            if (action == 1) {
                if (!nine_vm_choose(&vm, argument)) {
                    fprintf(stderr, "trace %u: choose failed\n", t);
                    ++failures;
                    break;
                }
            } else {
                if (!nine_vm_advance(&vm)) {
                    fprintf(stderr, "trace %u: advance failed\n", t);
                    ++failures;
                    break;
                }
            }
        }
    }
    printf("traces=%u steps=%u failures=%u max_node=%u max_index=%u\n", traces,
           compared_steps, failures, max_node, max_index);
    free(script);
    free(trace);
    return failures ? 1 : 0;
}
