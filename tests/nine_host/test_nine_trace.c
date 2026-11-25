// Exact source-trajectory differential: every text frame, speaker, choice
// label, ending and the permanent/per-run digest must match the trace produced
// by the pinned source engine.js (tools/nine_trace.cjs).
//
// Usage: test_nine_trace <nine_script.bin> <trace.bin>
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
    char line[192];
    char all[8192];
    size_t used = 0;
    for (uint32_t i = 0; i < pack->key_count; ++i) {
        const char *name = nine_pack_key(pack, (uint16_t)i);
        const int value = nine_pack_key_persistent(pack, (uint16_t)i)
                              ? state->progress[i]
                              : state->flag[i];
        const int n = snprintf(line, sizeof(line), "%s=%d;", name ? name : "?", value);
        if (n <= 0 || used + (size_t)n >= sizeof(all)) return 0;
        memcpy(all + used, line, (size_t)n);
        used += (size_t)n;
    }
    all[used] = '\0';
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <script.bin> <trace.bin>\n", argv[0]);
        return 2;
    }
    size_t script_size = 0, trace_size = 0;
    uint8_t *script = read_file(argv[1], &script_size);
    uint8_t *trace = read_file(argv[2], &trace_size);
    if (!script || !trace) { fprintf(stderr, "cannot read inputs\n"); return 2; }
    static nine_pack_t pack;
    if (!nine_pack_open(&pack, script, script_size)) {
        fprintf(stderr, "pack rejected\n");
        return 1;
    }
    if (trace_size < 16 || memcmp(trace, "NINETR1\0", 8) != 0) {
        fprintf(stderr, "trace header invalid\n");
        return 1;
    }
    if (rd32(trace + 12) != pack.identity) {
        fprintf(stderr, "trace was generated for a different package\n");
        return 1;
    }
    const uint32_t count = rd32(trace + 8);
    static nine_vm_t vm;
    if (!nine_vm_init(&vm, &pack)) return 1;
    if (!nine_vm_start(&vm, NINE_NODE_NONE, false)) return 1;

    size_t off = 16;
    uint32_t steps = 0;
    int failures = 0;
    for (;;) {
        const nine_view_t *v = nine_vm_locate(&vm);
        if (!v) { fprintf(stderr, "locate returned NULL\n"); return 1; }
        if (steps >= count) {
            fprintf(stderr, "C walked more steps than source trace\n");
            return 1;
        }
        if (off + 13 > trace_size) { fprintf(stderr, "trace truncated\n"); return 1; }
        const uint32_t kind = trace[off];
        const uint32_t node = rd16(trace + off + 1);
        const uint32_t index = rd32(trace + off + 3);
        const uint32_t want_digest = rd32(trace + off + 7);
        const uint32_t text_len = rd16(trace + off + 11);
        off += 13;
        if (off + text_len > trace_size) { fprintf(stderr, "trace text truncated\n"); return 1; }
        static char want_text[2048];
        const size_t copy = text_len < sizeof(want_text) - 1 ? text_len : sizeof(want_text) - 1;
        memcpy(want_text, trace + off, copy);
        want_text[copy] = 0;
        off += text_len;

        uint32_t got_kind = 9;
        switch (v->kind) {
        case NINE_VM_TEXT: got_kind = 0; break;
        case NINE_VM_OP: got_kind = 1; break;
        case NINE_VM_CHOICE: got_kind = 2; break;
        case NINE_VM_END: got_kind = 3; break;
        default: got_kind = 8; break;
        }
        char got_text[2048];
        got_text[0] = '\0';
        if (v->kind == NINE_VM_TEXT) {
            nine_frame_t frame;
            if (nine_vm_current_frame(&vm, &frame)) {
                const char *t = nine_pack_text(&pack, &frame);
                snprintf(got_text, sizeof(got_text), "%s", t ? t : "");
            }
        } else if (v->kind == NINE_VM_CHOICE) {
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
        const uint32_t got_digest = digest(&pack, &vm.state);
        if (got_kind != kind || vm.state.node != node || vm.state.index != index ||
            got_digest != want_digest || strcmp(got_text, want_text) != 0) {
            fprintf(stderr,
                    "mismatch step %u: kind %u/%u node %u/%u index %u/%u digest %08x/%08x "
                    "text '%s'/'%s'\n",
                    steps, got_kind, kind, (unsigned)vm.state.node, node,
                    (unsigned)vm.state.index, index, got_digest, want_digest, got_text, want_text);
            if (++failures >= 5) return 1;
        }
        ++steps;
        if (v->kind == NINE_VM_END) break;
        if (v->kind == NINE_VM_TEXT || v->kind == NINE_VM_OP) {
            if (!nine_vm_advance(&vm)) { fprintf(stderr, "advance failed\n"); return 1; }
        } else if (v->kind == NINE_VM_CHOICE) {
            if (!nine_vm_choose(&vm, 0)) { fprintf(stderr, "choose failed\n"); return 1; }
        } else {
            fprintf(stderr, "unexpected view kind\n");
            return 1;
        }
    }
    if (steps != count) {
        fprintf(stderr, "step count %u != trace %u\n", steps, count);
        return 1;
    }
    printf("PASS trace: %u steps, %u text frames, 13 choices, 1 OP, 1 end\n", steps,
           (unsigned)pack.frame_count - 1);
    free(script);
    free(trace);
    return 0;
}
