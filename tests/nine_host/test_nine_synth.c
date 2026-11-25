// Source-engine.js differential for condition/effect/edge semantics that the
// real Episode 3 graph does not exercise by itself: ordered conditional nexts,
// filtered choices with the all-choices fallback, `all` conjunctions, additive
// effects, complete/loop1, chapter-preview suppression and the 500-hop limit.
//
// synth_cases.json is fed to both the pinned source engine (synth_trace.cjs)
// and nine_synth_pack.py; this test replays the C VM and compares every step.
//
// Usage: test_nine_synth <dir with <case>.bin and <case>.trace>
#include "nine_pack.h"
#include "nine_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t digest(const nine_pack_t *pack, const nine_state_t *state)
{
    char all[4096];
    size_t used = 0;
    for (uint32_t i = 0; i < pack->key_count; ++i) {
        const char *name = nine_pack_key(pack, (uint16_t)i);
        const int value = nine_pack_key_persistent(pack, (uint16_t)i)
                              ? state->progress[i]
                              : state->flag[i];
        const int n = snprintf(all + used, sizeof(all) - used, "%s=%d;", name ? name : "?", value);
        if (n <= 0 || used + (size_t)n >= sizeof(all)) return 0;
        used += (size_t)n;
    }
    return fnv(all);
}

static uint8_t *read_file(const char *path, size_t *size)
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

typedef struct {
    const char *name;
    bool preview;
    int policy[8];
} case_t;

static int run_case(const char *dir, const case_t *c)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", dir, c->name);
    size_t pack_size = 0, trace_size = 0;
    uint8_t *pack_bytes = read_file(path, &pack_size);
    snprintf(path, sizeof(path), "%s/%s.trace", dir, c->name);
    uint8_t *trace = read_file(path, &trace_size);
    if (!pack_bytes || !trace || trace_size < 16) {
        fprintf(stderr, "%s: missing inputs\n", c->name);
        free(pack_bytes);
        free(trace);
        return 1;
    }
    static nine_pack_t pack;
    static nine_vm_t vm;
    if (!nine_pack_open(&pack, pack_bytes, pack_size)) {
        fprintf(stderr, "%s: pack rejected\n", c->name);
        return 1;
    }
    if (!nine_vm_init(&vm, &pack)) {
        fprintf(stderr, "%s: vm init failed\n", c->name);
        return 1;
    }
    if (!nine_vm_start(&vm, NINE_NODE_NONE, c->preview)) {
        // Unlocatable start (500-hop limit): the source trace records the same
        // partially-walked position and an error step.
        if (trace[16] != 9 || vm.state.node != rd16(trace + 17) ||
            vm.state.index != rd32(trace + 19)) {
            fprintf(stderr, "%s: vm start failed kind=%u node=%u/%u index=%u/%u\n",
                    c->name, (unsigned)trace[16], (unsigned)vm.state.node,
                    (unsigned)rd16(trace + 17), (unsigned)vm.state.index,
                    (unsigned)rd32(trace + 19));
            free(pack_bytes);
            free(trace);
            return 1;
        }
        printf("PASS synth %s (hop limit at node %u)\n", c->name, (unsigned)vm.state.node);
        free(pack_bytes);
        free(trace);
        return 0;
    }
    if (rd32(trace + 12) != pack.identity) {
        fprintf(stderr, "%s: trace/package mismatch\n", c->name);
        return 1;
    }
    const uint32_t count = rd32(trace + 8);
    size_t off = 16;
    int failures = 0;
    int choice_no = 0;
    for (uint32_t step = 0; step < count; ++step) {
        if (off + 13 > trace_size) { fprintf(stderr, "%s: truncated\n", c->name); return 1; }
        const uint32_t kind = trace[off];
        const uint32_t node = rd16(trace + off + 1);
        const uint32_t index = rd32(trace + off + 3);
        const uint32_t want_h = rd32(trace + off + 7);
        const uint32_t len = rd16(trace + off + 11);
        off += 13;
        if (off + len > trace_size) { fprintf(stderr, "%s: truncated text\n", c->name); return 1; }
        static char want_text[4096];
        const size_t copy = len < sizeof(want_text) - 1 ? len : sizeof(want_text) - 1;
        memcpy(want_text, trace + off, copy);
        want_text[copy] = 0;
        off += len;

        const nine_view_t *v = nine_vm_locate(&vm);
        uint32_t got_kind = 8;
        if (!v) got_kind = 8;
        else if (v->kind == NINE_VM_TEXT) got_kind = 0;
        else if (v->kind == NINE_VM_OP) got_kind = 1;
        else if (v->kind == NINE_VM_CHOICE) got_kind = 2;
        else if (v->kind == NINE_VM_END) got_kind = 3;
        else got_kind = 9;
        char got_text[4096];
        got_text[0] = 0;
        if (got_kind == 0) {
            nine_frame_t frame;
            if (nine_vm_current_frame(&vm, &frame)) {
                const char *t = nine_pack_text(&pack, &frame);
                snprintf(got_text, sizeof(got_text), "%s", t ? t : "");
            }
        } else if (got_kind == 2) {
            size_t used = 0;
            for (int i = 0; i < v->choice_count; ++i) {
                nine_edge_t e;
                if (!nine_pack_edge(&pack, true, v->choices[i], &e)) break;
                const char *label = nine_pack_string(&pack, e.label);
                const int n = snprintf(got_text + used, sizeof(got_text) - used, "%s%s",
                                       i ? "\n" : "", label ? label : "");
                if (n <= 0) break;
                used += (size_t)n;
            }
        }
        const uint32_t got_h = digest(&pack, &vm.state);
        const bool text_ok = kind == 9 || strcmp(got_text, want_text) == 0;
        const bool hash_ok = kind == 9 || got_h == want_h;
        if (got_kind != kind || vm.state.node != node || vm.state.index != index ||
            !text_ok || !hash_ok) {
            fprintf(stderr,
                    "%s step %u: kind %u/%u node %u/%u index %u/%u hash %08x/%08x "
                    "text '%s'/'%s'\n",
                    c->name, step, got_kind, kind, (unsigned)vm.state.node, node,
                    (unsigned)vm.state.index, index, got_h, want_h, got_text, want_text);
            ++failures;
        }
        if (got_kind == 3 || got_kind == 9 || got_kind == 8) break;
        if (got_kind == 2) {
            int pick = c->policy[choice_no < 8 ? choice_no : 7];
            if (pick < 0 || pick >= v->choice_count) pick = 0;
            ++choice_no;
            if (!nine_vm_choose(&vm, (uint32_t)pick)) {
                fprintf(stderr, "%s: choose failed\n", c->name);
                return 1;
            }
        } else if (!nine_vm_advance(&vm)) {
            fprintf(stderr, "%s: advance failed\n", c->name);
            return 1;
        }
    }
    free(pack_bytes);
    free(trace);
    if (failures) return 1;
    printf("PASS synth %s (%u steps)\n", c->name, count);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <synth dir>\n", argv[0]);
        return 2;
    }
    const case_t cases[] = {
        { "conditions", false, { 1, 0 } },
        { "effect_order", false, { 0 } },
        { "effect_preview", true, { 0 } },
        { "all_conditions", false, { 0 } },
        { "hop_limit", false, { 0 } },
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        failures += run_case(argv[1], &cases[i]);
    }
    return failures ? 1 : 0;
}
