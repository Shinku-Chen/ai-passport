// tests/test_cafe_trace.c —— 源 engine.js 轨迹 vs C VM 的逐步差分。
//
// 用法: test_cafe_trace <cafe.pak> <cafe_trace.txt>
// 轨迹由 tools/cafe_trace.js 生成（141 选项 + 7 结局 + 5 后日谈 + skip/previous）。
#include "cafe_pack.h"
#include "cafe_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static uint32_t flag_hash(const cafe_vm_t *vm)
{
    uint32_t h = 2166136261u;
    const uint8_t n = (uint8_t)cafe_meta_u32(1);
    for (uint8_t i = 0; i < n; ++i) {
        h = (h ^ cafe_vm_flag(vm, i)) * 16777619u;
    }
    h = (h ^ (cafe_vm_check_any_clear(vm) ? 1u : 0u)) * 16777619u;
    return h;
}

static uint32_t when_mask(const cafe_vm_t *vm)
{
    uint32_t mask = 0;
    const uint16_t n = cafe_when_count();
    for (uint16_t i = 0; i < n && i < 32; ++i) {
        if (cafe_vm_matches(vm, i)) mask |= 1u << i;
    }
    return mask;
}

static int parse_node(const char *s)
{
    if (!s || s[0] != 'n') return -1;
    return atoi(s + 1);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cafe.pak> <cafe_trace.txt>\n", argv[0]);
        return 2;
    }
    size_t pack_size = 0;
    unsigned char *pack = read_file(argv[1], &pack_size);
    if (!pack || !cafe_pack_set_data(pack, pack_size)) {
        fprintf(stderr, "pack load failed\n");
        return 2;
    }
    size_t trace_size = 0;
    unsigned char *trace = read_file(argv[2], &trace_size);
    if (!trace) {
        fprintf(stderr, "trace load failed\n");
        return 2;
    }
    char *save = NULL;
    char *line = strtok_r((char *)trace, "\n", &save);
    cafe_vm_t vm;
    bool in_case = false;
    unsigned long lines = 0;
    unsigned cases = 0;
    unsigned commands = 0;
    while (line) {
        ++lines;
        if (strncmp(line, "CASE ", 5) == 0) {
            char name[96] = {0};
            unsigned bits = 0;
            char node_s[16] = {0};
            unsigned progression = 0;
            if (sscanf(line, "CASE %95s %u %15s %u", name, &bits, node_s, &progression) != 4) {
                fprintf(stderr, "bad CASE line %lu: %s\n", lines, line);
                return 1;
            }
            const int node = parse_node(node_s);
            if (node < 0) {
                fprintf(stderr, "bad node in %s\n", name);
                return 1;
            }
            cafe_vm_init(&vm, bits);
            vm.node = (uint16_t)node;
            vm.index = 0;
            vm.progression = (uint8_t)progression;
            in_case = true;
            cases++;
        } else if (in_case && (strcmp(line, "A") == 0 || strcmp(line, "S") == 0 ||
                               strcmp(line, "P") == 0 || (line[0] == 'C' && line[1]))) {
            if (line[0] == 'A') cafe_vm_advance(&vm);
            else if (line[0] == 'S') cafe_vm_skip_chapter(&vm);
            else if (line[0] == 'P') cafe_vm_previous(&vm);
            else cafe_vm_choose(&vm, (uint8_t)atoi(line + 1));
            commands++;
        } else if (in_case && strncmp(line, "EXP ", 4) == 0) {
            unsigned type = 0, exp_bits = 0, exp_hash = 0, exp_when = 0;
            char exp_node[16] = {0};
            unsigned exp_index = 0;
            if (sscanf(line, "EXP %u %15s %u %u %u %u", &type, exp_node, &exp_index,
                       &exp_bits, &exp_hash, &exp_when) != 6) {
                fprintf(stderr, "bad EXP line %lu\n", lines);
                return 1;
            }
            const int want_node = atoi(exp_node);
            uint16_t node = 0, index = 0, first = 0;
            uint8_t count = 0;
            const int loc = cafe_vm_locate(&vm, &node, &index, &first, &count);
            const int got_node = vm.node == CAFE_NODE_ID_NONE ? 65535 : vm.node;
            const uint32_t got_hash = flag_hash(&vm);
            const uint32_t got_when = when_mask(&vm);
            if ((unsigned)loc != type || got_node != want_node || vm.index != exp_index ||
                vm.progress != exp_bits || got_hash != exp_hash || got_when != exp_when) {
                fprintf(stderr,
                        "MISMATCH line %lu: loc %d/%u node %d/%d idx %u/%u prog %08x/%08x "
                        "flags %08x/%08x when %08x/%08x\n",
                        lines, loc, type, got_node, want_node, vm.index, exp_index,
                        vm.progress, exp_bits, got_hash, exp_hash, got_when, exp_when);
                return 1;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    printf("cafe trace: %u cases, %u commands, %lu lines PASS\n", cases, commands, lines);
    free(trace);
    free(pack);
    return 0;
}
