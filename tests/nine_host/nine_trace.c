// Host trace CLI: production nine_vm on the production pack. The Node harness
// runs src/common/engine.js + chunk-codec.js over the same scenario and the
// comparator diffs the two transcripts.
#include "nine_pack.h"
#include "nine_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nine_pack_t pack;
static nine_vm_t vm;
static uint8_t *script;
static size_t script_size;
static int auto_choice[16];
static int auto_choice_n;

static void put_hex(const char *s)
{
    if (!s) { putchar('-'); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) printf("%02x", *p);
}
static int cmp_pair(const void *a, const void *b)
{
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}
static void emit_state(const nine_state_t *s)
{
    char pairs[NINE_KEY_MAX * 2][96];
    const char *ptrs[NINE_KEY_MAX * 2];
    int n = 0;
    for (uint16_t k = 0; k < pack.key_count; ++k) {
        if (s->flag_set[k / 8] & (1u << (k % 8))) {
            snprintf(pairs[n], sizeof(pairs[n]), "%s=%d", nine_pack_key(&pack, k), s->flag[k]);
            ptrs[n] = pairs[n]; n++;
        }
    }
    qsort(ptrs, (size_t)n, sizeof(ptrs[0]), cmp_pair);
    for (int i = 0; i < n; ++i) printf("%s%s", i ? ";" : "", ptrs[i]);
    printf("|");
    n = 0;
    for (uint16_t k = 0; k < pack.key_count; ++k) {
        if (s->progress_set[k / 8] & (1u << (k % 8))) {
            snprintf(pairs[n], sizeof(pairs[n]), "%s=%d", nine_pack_key(&pack, k), s->progress[k]);
            ptrs[n] = pairs[n]; n++;
        }
    }
    qsort(ptrs, (size_t)n, sizeof(ptrs[0]), cmp_pair);
    for (int i = 0; i < n; ++i) printf("%s%s", i ? ";" : "", ptrs[i]);
}
static void emit_view(void)
{
    const nine_view_t *v = &vm.view;
    nine_node_t node;
    const char *nid = "";
    if (nine_pack_node(&pack, v->node, &node)) nid = nine_pack_string(&pack, node.name);
    switch (v->kind) {
    case NINE_VM_TEXT: {
        nine_frame_t f;
        nine_vm_current_frame(&vm, &f);
        const char *text = nine_pack_text(&pack, &f);
        const char *sp = nine_pack_string(&pack, f.speaker);
        printf("S text %s %u ", nid, v->index);
        put_hex(sp); printf(" "); put_hex(text); printf(" ");
        break;
    }
    case NINE_VM_OP:
        printf("S op %s %u - - ", nid, v->index);
        break;
    case NINE_VM_CHOICE:
        printf("S choice %s %u - ", nid, v->index);
        for (int i = 0; i < v->choice_count; ++i) {
            nine_edge_t e;
            if (nine_pack_edge(&pack, true, v->choices[i], &e)) {
                if (i) putchar(',');
                put_hex(nine_pack_string(&pack, e.label));
            }
        }
        printf(" ");
        break;
    case NINE_VM_END:
        printf("S end @end %u - - ", v->index);
        break;
    default:
        printf("S error %s %u - ", nid, v->index);
        put_hex(v->error);
        printf(" ");
        break;
    }
    emit_state(&vm.state);
    putchar('\n');
}
static int parse_int(const char *s) { return s ? atoi(s) : 0; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s script.bin scenario\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    script = malloc((size_t)n);
    if (!script || fread(script, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f); script_size = (size_t)n;
    if (!nine_pack_open(&pack, script, script_size)) { fprintf(stderr, "bad pack\n"); return 2; }
    if (!nine_vm_init(&vm, &pack)) return 2;
    FILE *sc = fopen(argv[2], "r");
    if (!sc) return 2;
    char line[512];
    int progress_before = 0;
    while (fgets(line, sizeof(line), sc)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        if (!strcmp(cmd, "progress")) {
            /* progress key=value [key=value ...] */
            char *kv;
            while ((kv = strtok(NULL, " "))) {
                char *eq = strchr(kv, '=');
                if (!eq) continue;
                *eq = 0;
                int k = nine_pack_find_key(&pack, kv);
                if (k >= 0) {
                    vm.state.progress[k] = atoi(eq + 1);
                    vm.state.progress_set[k / 8] |= (uint8_t)(1u << (k % 8));
                }
            }
            progress_before = 1;
        } else if (!strcmp(cmd, "start")) {
            nine_vm_start(&vm, NINE_NODE_NONE, false);
            emit_view();
        } else if (!strcmp(cmd, "newgame")) {
            nine_vm_start(&vm, NINE_NODE_NONE, false);
            emit_view();
        } else if (!strcmp(cmd, "preview")) {
            char *id = strtok(NULL, " ");
            int node = nine_pack_find_node(&pack, id);
            if (node < 0) { printf("E unknown chapter\n"); continue; }
            nine_vm_start(&vm, (uint16_t)node, true);
            emit_view();
        } else if (!strcmp(cmd, "advance")) {
            int count = parse_int(strtok(NULL, " "));
            for (int i = 0; i < count; ++i) {
                nine_vm_advance(&vm);
                emit_view();
                if (vm.view.kind != NINE_VM_TEXT && vm.view.kind != NINE_VM_OP) break;
            }
        } else if (!strcmp(cmd, "choose")) {
            nine_vm_choose(&vm, (uint32_t)parse_int(strtok(NULL, " ")));
            emit_view();
        } else if (!strcmp(cmd, "previous")) {
            nine_vm_previous(&vm);
            emit_view();
        } else if (!strcmp(cmd, "skip")) {
            nine_vm_skip_chapter(&vm);
            emit_view();
        } else if (!strcmp(cmd, "auto")) {
            int count = parse_int(strtok(NULL, " "));
            char *policy = strtok(NULL, " ");
            auto_choice_n = 0;
            if (policy) {
                char *tok = strtok(policy, ",");
                while (tok && auto_choice_n < 16) { auto_choice[auto_choice_n++] = atoi(tok); tok = strtok(NULL, ","); }
            }
            int pick = 0;
            for (int i = 0; i < count; ++i) {
                nine_vm_locate(&vm);
                emit_view();
                if (vm.view.kind == NINE_VM_TEXT || vm.view.kind == NINE_VM_OP) {
                    nine_vm_advance(&vm);
                } else if (vm.view.kind == NINE_VM_CHOICE) {
                    int sel = auto_choice_n ? auto_choice[pick % auto_choice_n] : 0;
                    pick++;
                    if (sel >= vm.view.choice_count) sel = 0;
                    nine_vm_choose(&vm, (uint32_t)sel);
                } else break;
            }
        } else {
            printf("E unknown command\n");
        }
    }
    fclose(sc);
    nine_vm_deinit(&vm);
    free(script);
    (void)progress_before;
    return 0;
}
