// tests/summer_walk.c —— 用生产 C 解释器遍历真实资源包。
//
// 覆盖：主线（首选项/末选项两条分支）、ALKA/POCKET 入口、章节模式、
// 地图预览与选择、文本停点存档/恢复。失败返回 1，供 gate 使用。
#include "summer_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SUMMER_PACK_PATH
#define SUMMER_PACK_PATH "main/summer_data/summer_pack.bin"
#endif

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
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

typedef struct {
    long texts;
    long choices;
    long maps;
    long yields;
    int ending;
    int error;
    int map_empty;
    uint16_t map_scene;
    uint32_t map_op;
} walk_stats_t;

static walk_stats_t walk(summer_pack_t *pack, summer_vm_t *vm, int choice_mode,
                         int map_mode, long text_limit)
{
    walk_stats_t st = { 0 };
    for (long guard = 0; guard < text_limit; ++guard) {
        const summer_step_t step = summer_vm_step(vm, pack, 256);
        if (step.kind == SUMMER_STEP_TEXT) {
            st.texts++;
            if (!summer_vm_advance(vm, pack)) {
                st.error = 1;
                break;
            }
        } else if (step.kind == SUMMER_STEP_CHOICE) {
            st.choices++;
            int items[32];
            const int count = summer_vm_visible_choices(vm, pack, step.group, items, 32);
            if (count <= 0) {
                st.error = 2;
                break;
            }
            const int pick = choice_mode == 0 ? items[0] : items[count - 1];
            if (!summer_vm_choose(vm, pack, step.group, pick)) {
                st.error = 3;
                break;
            }
        } else if (step.kind == SUMMER_STEP_MAP) {
            st.maps++;
            summer_map_choice_t map[40];
            const int count = summer_vm_preview_map(vm, pack, step.group, map, 40);
            if (count <= 0) {
                // The source itself can exhaust all destinations for a map
                // stage; the app shows its documented error page.  Treat it
                // as a source-terminal state, not an interpreter failure.
                st.map_empty = 1;
                st.map_scene = vm->scene_id;
                st.map_op = vm->op_index;
                return st;
            }
            const int index = map_mode == 0 ? 0 : count - 1;
            if (!summer_vm_choose_map(vm, pack, step.group, map[index].item_index)) {
                st.error = 5;
                break;
            }
        } else if (step.kind == SUMMER_STEP_YIELD) {
            st.yields++;
        } else if (step.kind == SUMMER_STEP_END) {
            st.ending = summer_vm_ending(vm, pack);
            return st;
        } else {
            st.error = 6;
            break;
        }
    }
    if (st.error == 0) st.error = 7;
    return st;
}

static void test_story(summer_pack_t *pack, int choice_mode, int map_mode,
                       summer_progress_t *progress, int action, int expected)
{
    summer_vm_t vm;
    summer_vm_init(&vm);
    if (action == 0) {
        CHECK(summer_vm_start_story(&vm, pack), "start story");
    } else {
        CHECK(summer_vm_start_flow(&vm, pack, action), "start flow");
    }
    summer_progress_apply(&vm, pack, progress);
    const walk_stats_t st = walk(pack, &vm, choice_mode, map_mode, 400000);
    printf("  story action=%d choice=%s map=%s expected=%d: texts=%ld choices=%ld maps=%ld "
           "yields=%ld ending=%d error=%d map_empty=%d(%u:%u)",
           action, choice_mode == 0 ? "first" : "last", map_mode == 0 ? "first" : "last",
           expected, st.texts, st.choices, st.maps, st.yields, st.ending, st.error,
           st.map_empty, st.map_scene, st.map_op);
    putchar(10);
    CHECK(st.error == 0, "story walk error %d", st.error);
    if (expected < 0) {
        // Source and C both exhaust this map stage under last-choice/first-map:
        // 281:1404 has no remaining destination (verified by the source trace).
        CHECK(st.map_empty && st.map_scene == 281 && st.map_op == 1404,
              "expected source MEXHAUST at 281:1404, got empty=%d %u:%u ending=%d",
              st.map_empty, st.map_scene, st.map_op, st.ending);
        return;
    }
    if (st.map_empty) {
        CHECK(false, "unexpected map exhaustion at %u:%u", st.map_scene, st.map_op);
        return;
    }
    CHECK(st.ending == expected, "story ending %d expected %d", st.ending, expected);
    if (st.map_empty) {
        CHECK(st.ending == 0, "map-empty walk has ending %d", st.ending);
        printf("  (map stage exhausted all destinations; source error page)\n");
        return;
    }
    CHECK(st.ending >= 1 && st.ending <= 14, "story ending %d", st.ending);
    if (st.ending > 0) {
        (void)summer_progress_sync(&vm, pack, progress);
        CHECK((progress->cleared & (uint16_t)(1u << (st.ending - 1))) != 0,
              "ending %d not synced", st.ending);
    }
}

static void test_chapters(summer_pack_t *pack)
{
    const summer_scene_t *flows = summer_scene_by_id(pack, pack->meta.flow_scene);
    CHECK(flows != NULL, "flow scene");
    int tested = 0;
    for (uint32_t i = 0; i < pack->chapter_count; i += 17) {
        const summer_chapter_t *chapter = &pack->chapters[i];
        summer_vm_t vm;
        CHECK(summer_vm_start_chapter(&vm, pack, chapter->scene), "chapter %u start", chapter->scene);
        CHECK(vm.mode == SUMMER_MODE_CHAPTER, "chapter mode");
        const walk_stats_t st = walk(pack, &vm, 0, 0, 20000);
        CHECK(st.error == 0, "chapter %u walk error %d", chapter->scene, st.error);
        tested++;
    }
    printf("  chapters: tested=%d\n", tested);
    CHECK(tested > 0, "no chapters tested");
}

static void test_save_roundtrip(summer_pack_t *pack)
{
    summer_vm_t vm;
    CHECK(summer_vm_start_story(&vm, pack), "save start");
    for (int i = 0; i < 30; ++i) {
        const summer_step_t step = summer_vm_step(&vm, pack, 256);
        if (step.kind == SUMMER_STEP_TEXT) break;
        if (step.kind == SUMMER_STEP_CHOICE || step.kind == SUMMER_STEP_MAP ||
            step.kind == SUMMER_STEP_END || step.kind == SUMMER_STEP_ERROR) {
            CHECK(false, "save fixture stopped early");
            return;
        }
    }
    summer_progress_t progress;
    summer_progress_init(&progress);
    uint8_t buf[SUMMER_SAVE_CAP];
    const size_t len = summer_save_encode(&vm, 0, &progress, buf, sizeof(buf));
    CHECK(len > 0, "save encode");
    summer_save_t save;
    char err[64];
    CHECK(summer_save_decode(pack, buf, len, &save, err, sizeof(err)), "save decode: %s", err);
    CHECK(save.scene_id == vm.scene_id && save.op_index == vm.op_index, "save position");
    CHECK(save.bg == vm.bg && save.body == vm.body, "save images");
    summer_vm_t restored;
    CHECK(summer_save_to_vm(&save, &restored, pack), "save to vm");
    const summer_step_t a = summer_vm_step(&vm, pack, 256);
    const summer_step_t b = summer_vm_step(&restored, pack, 256);
    CHECK(a.kind == b.kind && a.text == b.text && a.speaker == b.speaker, "restored text");
    // Corrupting the CRC must fail without touching a valid VM.
    buf[len - 1] ^= 0xFF;
    summer_save_t bad;
    CHECK(!summer_save_decode(pack, buf, len, &bad, err, sizeof(err)), "corrupt save accepted");
}

static void test_progress(void)
{
    summer_progress_t p;
    summer_progress_init(&p);
    p.cleared = (uint16_t)((1u << (4 - 1)) | (1u << (5 - 1)) | (1u << (6 - 1)) |
                           (1u << (7 - 1)) | (1u << (13 - 1)));
    p.restart_stage = 1;
    summer_progress_reconcile(&p);
    CHECK(p.restart_stage == 1, "restart stage %u", p.restart_stage);
    unsigned actions = summer_progress_title_actions(&p);
    CHECK((actions & SUMMER_TITLE_RESTART) != 0, "ReSTART not offered after 4/5 heroines");
    p.restart_stage = 4;
    actions = summer_progress_title_actions(&p);
    CHECK((actions & SUMMER_TITLE_ALKA) != 0, "ALKA not offered after second ReSTART");
    p.cleared |= (uint16_t)(1u << (8 - 1));
    actions = summer_progress_title_actions(&p);
    CHECK((actions & SUMMER_TITLE_POCKET) != 0, "Pocket not offered");
    p.cleared |= (uint16_t)(1u << (9 - 1));
    actions = summer_progress_title_actions(&p);
    CHECK((actions & SUMMER_TITLE_START) != 0 && (actions & SUMMER_TITLE_ALKA) != 0 &&
          (actions & SUMMER_TITLE_POCKET) != 0, "after Pocket actions %u", actions);
    summer_progress_t q;
    summer_progress_init(&q);
    CHECK(summer_progress_merge(&q, 0x0003, NULL, 0), "merge clear");
    CHECK(!summer_progress_merge(&q, 0x0003, NULL, 0), "merge duplicate");
    uint8_t seen[4] = { 0x05, 0, 0, 0 };
    CHECK(summer_progress_merge(&q, 0, seen, sizeof(seen)), "merge seen");
    CHECK(summer_progress_cg_seen(&q, 0) && summer_progress_cg_seen(&q, 2), "seen bits");
    CHECK(!summer_progress_cg_seen(&q, 1), "seen bit 1");
    uint8_t enc[96];
    size_t len = 0;
    CHECK(summer_progress_encode(&q, enc, sizeof(enc), &len), "progress encode");
    summer_progress_t back;
    CHECK(summer_progress_decode(&back, enc, len), "progress decode");
    CHECK(back.cleared == q.cleared && summer_progress_cg_seen(&back, 2), "progress roundtrip");
    enc[len - 1] ^= 0xFF;
    CHECK(!summer_progress_decode(&back, enc, len), "corrupt progress accepted");
}

static uint32_t section_offset(const uint8_t *blob, int type)
{
    const uint32_t count = (uint32_t)blob[12] | ((uint32_t)blob[13] << 8);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *s = blob + 20 + i * 16;
        const uint32_t t = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
        if (t == (uint32_t)type) {
            return (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)s[6] << 16) |
                   ((uint32_t)s[7] << 24);
        }
    }
    return 0;
}

static void test_bad_pack(const uint8_t *blob, size_t size)
{
    uint8_t *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, blob, size);
    summer_pack_t pack;
    copy[0] ^= 0xFF;
    CHECK(!summer_pack_open(&pack, copy, (uint32_t)size, NULL), "bad magic accepted");
    memcpy(copy, blob, size);
    copy[10] ^= 0x01; // header size
    CHECK(!summer_pack_open(&pack, copy, (uint32_t)size, NULL), "bad header accepted");
    memcpy(copy, blob, size);
    CHECK(!summer_pack_open(&pack, copy, 100, NULL), "truncated pack accepted");
    // Bad op stream: first OPS byte is an unknown op kind.
    memcpy(copy, blob, size);
    copy[section_offset(copy, 1)] = 0xFF;
    CHECK(!summer_pack_open(&pack, copy, (uint32_t)size, NULL), "bad op stream accepted");
    // Bad scene next: first scene's next points past the scene table.
    memcpy(copy, blob, size);
    {
        const uint32_t scene = section_offset(copy, 3);
        copy[scene + 10] = 0xFF;
        copy[scene + 11] = 0xFF;
        CHECK(!summer_pack_open(&pack, copy, (uint32_t)size, NULL), "bad scene next accepted");
    }
    // Bad scene route: i8 field must stay in -1..13.
    memcpy(copy, blob, size);
    {
        const uint32_t scene = section_offset(copy, 3);
        copy[scene + 12] = 127;
        CHECK(!summer_pack_open(&pack, copy, (uint32_t)size, NULL), "bad scene route accepted");
    }
    free(copy);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : SUMMER_PACK_PATH;
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) {
        fprintf(stderr, "SKIP: cannot read %s\n", path);
        return 77;
    }
    summer_pack_t pack;
    const char *err = NULL;
    if (!summer_pack_open(&pack, blob, (uint32_t)size, &err)) {
        fprintf(stderr, "FAIL pack open: %s\n", err ? err : "?");
        free(blob);
        return 1;
    }
    printf("pack: scenes=%u images=%u text_codes=%u ops=%u\n",
           pack.scene_count, pack.image_count, pack.text_codes, pack.ops_size);

    summer_progress_t empty;
    summer_progress_init(&empty);
    test_story(&pack, 0, 0, &empty, 0, 4);   // ff: ending 4
    test_story(&pack, 1, 1, &empty, 0, 13);  // ll: ending 13
    test_story(&pack, 0, 1, &empty, 0, 13);  // fl: ending 13
    test_story(&pack, 1, 0, &empty, 0, -1);  // lf: source MEXHAUST at 281:1404
    test_chapters(&pack);
    test_save_roundtrip(&pack);
    test_bad_pack(blob, size);
    test_progress();

    summer_progress_t late;
    summer_progress_init(&late);
    late.cleared = (uint16_t)((1u << (4 - 1)) | (1u << (5 - 1)) | (1u << (6 - 1)) |
                              (1u << (7 - 1)) | (1u << (13 - 1)) | (1u << (8 - 1)) |
                              (1u << (9 - 1)));
    late.restart_stage = 3;
    test_story(&pack, 0, 0, &late, SUMMER_TITLE_ALKA, 8);
    test_story(&pack, 0, 0, &late, SUMMER_TITLE_POCKET, 9);

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("summer_walk: PASS\n");
    return 0;
}
