// Host route audit: production gal_vm over the real pack.
// 1) every chapter entry starts and reaches a frame without error;
// 2) bounded DFS explores choices/objections from the initial story entry.
#include "gal_pack.h"
#include "gal_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BRANCH 8192
#define MAX_STATES 250000
#define HASH_SIZE (1u << 19)

typedef struct {
    gal_vm_t vm;
    uint16_t row;
    uint8_t kind;   // 0 choice, 1 link, 2 advance
} branch_t;

static void probe_branch(const gal_pack_t *pack, const gal_vm_t *base, int row, int kind);

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

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

static uint64_t vm_hash(const gal_vm_t *vm)
{
    uint64_t h = 1469598103934665603ull;
    h = mix(h, vm->chapter); h = mix(h, vm->root); h = mix(h, vm->cursor);
    h = mix(h, vm->focus); h = mix(h, (uint64_t)vm->ended | ((uint64_t)vm->trial << 1) |
                                   ((uint64_t)vm->initial_book << 2) |
                                   ((uint64_t)vm->debug_direct << 3) |
                                   ((uint64_t)vm->choice_block << 4) |
                                   ((uint64_t)vm->legacy_visuals << 5));
    h = mix(h, vm->block_end); h = mix(h, vm->block_continuation);
    for (uint16_t i = 0; i < vm->var_count; ++i) {
        const gal_val_t *v = &vm->vars[i];
        uint32_t bits;
        memcpy(&bits, &v->num, sizeof(bits));
        h = mix(h, ((uint64_t)v->type << 48) ^ ((uint64_t)v->str << 32) ^ bits);
    }
    for (uint16_t i = 0; i < vm->book_count; ++i) {
        const uint16_t k = vm->book_order[i];
        h = mix(h, ((uint64_t)k << 8) | vm->book[k]);
    }
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        const gal_sprite_t *s = &vm->sprites[i];
        if (!s->used) continue;
        h = mix(h, ((uint64_t)s->actor << 48) ^ ((uint64_t)s->sub << 32) ^
                   ((uint64_t)s->image << 16) ^ (uint16_t)s->x);
        h = mix(h, ((uint64_t)(uint16_t)s->y << 48) ^ ((uint64_t)(uint16_t)s->world_y << 32) ^
                   ((uint64_t)(uint16_t)s->scale100 << 16) ^ s->visible);
    }
    for (uint8_t i = 0; i < vm->layer_count; ++i) {
        const gal_layer_t *l = &vm->layers[i];
        if (!l->used) continue;
        h = mix(h, ((uint64_t)l->actor << 48) ^ ((uint64_t)l->sub << 32) ^
                   ((uint64_t)l->image << 16) ^ l->visible);
    }
    for (uint8_t i = 0; i < vm->stack_count; ++i)
        h = mix(h, ((uint64_t)vm->stack[i][0] << 16) | vm->stack[i][1]);
    for (uint8_t i = 0; i < vm->pending_count; ++i) {
        const gal_pending_t *p = &vm->pending[i];
        h = mix(h, ((uint64_t)p->text_str << 48) ^ ((uint64_t)p->to << 32) ^
                   ((uint64_t)p->end << 16) ^ p->menu);
    }
    if (vm->active_valid) {
        h = mix(h, 0xA5A5);
        h = mix(h, vm->active_chapter); h = mix(h, vm->active_cursor);
        for (uint8_t i = 0; i < vm->active_count; ++i) {
            const gal_row_t *r = &vm->active_rows[i];
            h = mix(h, ((uint64_t)r->to << 48) ^ ((uint64_t)r->end << 32) ^
                       ((uint64_t)r->kind << 16) ^ r->text_off);
        }
    }
    return h ? h : 1;
}

static uint64_t *s_seen;
static uint32_t s_seen_count;
static uint32_t s_state_budget;

static bool seen_add(uint64_t h)
{
    uint32_t i = (uint32_t)(h & (HASH_SIZE - 1));
    for (uint32_t n = 0; n < HASH_SIZE; ++n) {
        if (s_seen[i] == 0) { s_seen[i] = h; s_seen_count++; return true; }
        if (s_seen[i] == h) return false;
        i = (i + 1) & (HASH_SIZE - 1);
    }
    return false;
}

static uint8_t s_scene_seen[512];
static uint8_t s_end_seen[512];
static int s_end_kinds[64];
static int s_end_kind_ids[64];
static int s_end_kind_count;
static int s_choices;
static int s_links;
static int s_errors;
static int s_texts;
static int s_dropped;
static int s_max_stack;
static int s_max_cursor[512];

static void record_end(uint16_t kind)
{
    if (!s_end_seen[kind & 511]) s_end_seen[kind & 511] = 1;
    for (int i = 0; i < s_end_kind_count; ++i) {
        if (s_end_kind_ids[i] == kind) { s_end_kinds[i]++; return; }
    }
    if (s_end_kind_count < 64) {
        s_end_kind_ids[s_end_kind_count] = kind;
        s_end_kinds[s_end_kind_count] = 1;
        s_end_kind_count++;
    }
}

static int chapter_entry_count;

static int audit_chapters(const gal_pack_t *pack)
{
    int failures = 0;
    chapter_entry_count = (int)pack->count[GAL_SEC_CHAPTERS];
    for (int i = 0; i < chapter_entry_count; ++i) {
        const gal_chapter_t *c = (const gal_chapter_t *)pack->sec[GAL_SEC_CHAPTERS] + i;
        static gal_vm_t vm;
        gal_vm_init(&vm, pack);
        const char *err = NULL;
        if (!gal_vm_start(&vm, (uint16_t)c->scene, false, false, &err)) {
            fprintf(stderr, "chapter %d start failed: %s\n", i, err ? err : "?");
            failures++;
            continue;
        }
        bool got_frame = false;
        for (int step = 0; step < 400 && !vm.ended; ++step) {
            gal_frame_t f;
            const gal_frame_type_t t = gal_vm_read(&vm, &f);
            if (t == GAL_FRAME_ERROR) {
                fprintf(stderr, "chapter %d error at %u:%u: %s\n", i, vm.chapter, vm.cursor,
                        f.error ? f.error : "?");
                failures++;
                break;
            }
            if (t == GAL_FRAME_YIELD) continue;
            got_frame = true;
            if (t == GAL_FRAME_CHOICE) {
                if (f.row_count == 0) { failures++; break; }
                if (!gal_vm_choose(&vm, 0, &err)) { failures++; break; }
            } else if (t == GAL_FRAME_TEXT) {
                gal_vm_advance(&vm);
            } else if (t == GAL_FRAME_END) {
                break;
            }
        }
        if (!got_frame) { failures++; }
    }
    return failures;
}

static int audit_dfs(const gal_pack_t *pack)
{
    s_seen = calloc(HASH_SIZE, sizeof(uint64_t));
    branch_t *stack = malloc(sizeof(branch_t) * MAX_BRANCH);
    if (!s_seen || !stack) return 1;
    int failures = 0;
    static gal_vm_t vm;
    gal_vm_init(&vm, pack);
    const char *err = NULL;
    if (!gal_vm_start(&vm, (uint16_t)pack->meta[1], true, false, &err)) {
        free(stack); free(s_seen); return 1;
    }
    int stack_count = 0;
    int active = 1;
    uint32_t steps = 0;
    while (active || stack_count) {
        if (!active) {
            branch_t *b = &stack[--stack_count];
            vm = b->vm;
            if (b->kind == 0) {
                if (!gal_vm_choose(&vm, (uint8_t)b->row, &err)) { failures++; break; }
            } else if (b->kind == 1) {
                if (!gal_vm_object_to(&vm, (uint8_t)b->row, &err)) { failures++; break; }
            } else {
                gal_vm_advance(&vm);
            }
            active = 1;
        }
        if (steps++ > 20000000u || s_state_budget++ > MAX_STATES) break;
        s_scene_seen[vm.chapter & 511] = 1;
        if (vm.cursor > s_max_cursor[vm.chapter & 511]) s_max_cursor[vm.chapter & 511] = vm.cursor;
        gal_frame_t f;
        const gal_frame_type_t t = gal_vm_read(&vm, &f);
        if (t == GAL_FRAME_YIELD) continue;
        if (t == GAL_FRAME_ERROR) {
            s_errors++;
            if (s_errors < 8) {
                fprintf(stderr, "dfs error at %u:%u: %s\n", vm.chapter, vm.cursor,
                        f.error ? f.error : "?");
            }
            active = 0;
            continue;
        }
        if (t == GAL_FRAME_END) {
            s_scene_seen[vm.chapter & 511] = 1;
            record_end(f.end_kind);
            active = 0;
            continue;
        }
        if (t == GAL_FRAME_TEXT) {
            s_texts++;
            s_scene_seen[vm.chapter & 511] = 1;
            if (f.link_count) {
                s_links++;
                const uint64_t h = vm_hash(&vm);
                if (!seen_add(h)) { active = 0; continue; }
                // 备选:跳过反驳继续读,以及每个反驳词
                if (stack_count < MAX_BRANCH) {
                    branch_t *b = &stack[stack_count++];
                    memset(b, 0, sizeof(*b));
                    b->vm = vm; b->kind = 2;
                }
                for (uint16_t i = 0; i < f.link_count; ++i) {
                    if (stack_count >= MAX_BRANCH) { s_dropped++; continue; }
                    branch_t *b = &stack[stack_count++];
                    memset(b, 0, sizeof(*b));
                    b->vm = vm; b->kind = 1; b->row = i;
                }
                if (stack_count > s_max_stack) s_max_stack = stack_count;
                for (uint16_t i = 0; i < f.link_count; ++i) probe_branch(pack, &vm, i, 1);
                if (!gal_vm_object_to(&vm, 0, &err)) { failures++; active = 0; }
            } else {
                gal_vm_advance(&vm);
            }
            continue;
        }
        if (t == GAL_FRAME_CHOICE) {
            s_choices++;
            const uint64_t h = vm_hash(&vm);
            if (!seen_add(h) || f.row_count == 0) { active = 0; continue; }
            for (int r = f.row_count - 1; r >= 1; --r) {
                if (stack_count >= MAX_BRANCH) { s_dropped++; continue; }
                branch_t *b = &stack[stack_count++];
                memset(b, 0, sizeof(*b));
                b->vm = vm; b->kind = 0; b->row = (uint16_t)r;
            }
            if (stack_count > s_max_stack) s_max_stack = stack_count;
            for (int r = 0; r < f.row_count; ++r) probe_branch(pack, &vm, r, 0);
            if (!gal_vm_choose(&vm, 0, &err)) { failures++; active = 0; }
            continue;
        }
    }
    free(stack);
    free(s_seen);
    return failures;
}

static int s_probe_errors;
static int s_probe_rows;

static void probe_branch(const gal_pack_t *pack, const gal_vm_t *base, int row, int kind)
{
    gal_vm_t tmp = *base;
    tmp.expr.pack = pack;
    tmp.expr.vars = tmp.vars;
    tmp.expr.var_count = tmp.var_count;
    tmp.error = NULL;
    const char *err = NULL;
    s_probe_rows++;
    if (kind == 0) {
        if (!gal_vm_choose(&tmp, (uint8_t)row, &err)) { s_probe_errors++; return; }
    } else {
        if (!gal_vm_object_to(&tmp, (uint8_t)row, &err)) { s_probe_errors++; return; }
    }
    for (int i = 0; i < 32; ++i) {
        gal_frame_t f;
        const gal_frame_type_t t = gal_vm_read(&tmp, &f);
        if (t == GAL_FRAME_YIELD) continue;
        s_scene_seen[tmp.chapter & 511] = 1;
        if (t == GAL_FRAME_ERROR) { s_probe_errors++; break; }
        if (t == GAL_FRAME_END) { record_end(f.end_kind); break; }
        break;
    }
}

static uint32_t s_rng = 0x12345678u;
static uint32_t rng(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }

static int audit_random(const gal_pack_t *pack, int runs)
{
    int failures = 0;
    static gal_vm_t vm;
    for (int run = 0; run < runs; ++run) {
        gal_vm_init(&vm, pack);
        const char *err = NULL;
        if (!gal_vm_start(&vm, (uint16_t)pack->meta[1], true, false, &err)) return 1;
        for (int step = 0; step < 50000 && !vm.ended; ++step) {
            s_scene_seen[vm.chapter & 511] = 1;
            gal_frame_t f;
            const gal_frame_type_t t = gal_vm_read(&vm, &f);
            if (t == GAL_FRAME_YIELD) continue;
            if (t == GAL_FRAME_ERROR) { failures++; break; }
            if (t == GAL_FRAME_END) { s_scene_seen[vm.chapter & 511] = 1; record_end(f.end_kind); break; }
            if (t == GAL_FRAME_TEXT) {
                s_texts++;
                s_scene_seen[vm.chapter & 511] = 1;
                if (f.link_count && (rng() & 1u)) {
                    s_links++;
                    if (!gal_vm_object_to(&vm, (uint8_t)(rng() % f.link_count), &err)) { failures++; break; }
                } else {
                    gal_vm_advance(&vm);
                }
            } else if (t == GAL_FRAME_CHOICE) {
                s_choices++;
                if (f.row_count == 0) { failures++; break; }
                if (!gal_vm_choose(&vm, (uint8_t)(rng() % f.row_count), &err)) { failures++; break; }
            }
        }
    }
    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s pack\n", argv[0]); return 2; }
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) {
        fprintf(stderr, "pack open: %s\n", err);
        return 1;
    }
    int failures = audit_chapters(&pack);
    failures += audit_dfs(&pack);
    failures += audit_random(&pack, 3000);

    int scenes = 0;
    for (int i = 0; i < (int)pack.count[GAL_SEC_SCENES]; ++i) if (s_scene_seen[i]) scenes++;
    printf("chapters=%d failures=%d\n", chapter_entry_count, failures);
    printf("dfs: states=%u texts=%d choices=%d links=%d errors=%d scenes_seen=%d/%u\n",
           s_seen_count, s_texts, s_choices, s_links, s_errors, scenes,
           (unsigned)pack.count[GAL_SEC_SCENES]);
    for (int i = 0; i < s_end_kind_count; ++i) {
        printf("ending kind=%s count=%d\n", gal_pack_str(&pack, (uint16_t)s_end_kind_ids[i]),
               s_end_kinds[i]);
    }
    int unseen = 0;
    for (int i = 0; i < (int)pack.count[GAL_SEC_SCENES]; ++i) {
        if (!s_scene_seen[i]) {
            unseen++;
            if (unseen <= 40) {
                const gal_scene_t *s = gal_pack_scene(&pack, (uint16_t)i);
                printf("unseen scene %d %s\n", i, gal_pack_str(&pack, (uint16_t)s->id));
            }
        }
    }
    printf("unseen scenes=%d\n", unseen);
    printf("test_gal_routes: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
