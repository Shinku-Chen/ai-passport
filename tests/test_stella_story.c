// tests/test_stella_story.c —— 两条故事线的宿主机遍历、选项目标与存档隔离。
#include "stella_model.h"
#include "stella_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STELLA_PACK_PATH
#define STELLA_PACK_PATH "assets/stella/stella_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); failures++; } } while (0)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

typedef struct {
    int text;
    int choices;
    int ends;
    int scenes;
    int choice_target;
} walk_result_t;

// 按固件相同的规则走：text 前进；choice 跳到 arg；end 结束；op 用尽走 next。
static bool walk(const stella_pack_t *pack, uint16_t start_id, walk_result_t *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t index;
    if (!stella_pack_find_scene(pack, start_id, &index)) return false;
    stella_scene_t scene;
    if (!stella_pack_scene(pack, index, &scene)) return false;
    uint16_t n = 0;
    const uint8_t *ops = stella_pack_ops(pack, &scene, &n);
    if (!ops || !n) return false;
    uint16_t op = 0;
    int guard = 0;
    for (;;) {
        if (++guard > 30000) return false;
        if (op >= n) {
            if (scene.next == STELLA_NONE) return false;
            if (!stella_pack_find_scene(pack, scene.next, &index)) return false;
            if (!stella_pack_scene(pack, index, &scene)) return false;
            ops = stella_pack_ops(pack, &scene, &n);
            if (!ops || !n) return false;
            op = 0;
            out->scenes++;
            continue;
        }
        stella_op_t o;
        if (!stella_pack_op(ops, n, op, &o)) return false;
        if (o.type == STELLA_OP_TEXT) {
            out->text++;
            op++;
        } else if (o.type == STELLA_OP_CHOICE) {
            out->choices++;
            out->choice_target = o.arg;
            op = o.arg;
        } else if (o.type == STELLA_OP_END) {
            out->ends++;
            return true;
        } else {
            return false;
        }
    }
}

int main(void)
{
    size_t size = 0;
    uint8_t *blob = read_file(STELLA_PACK_PATH, &size);
    CHECK(blob != NULL, "cannot read pack");
    if (!blob) return 1;
    stella_pack_t pack;
    CHECK(stella_pack_open(&pack, blob, (uint32_t)size), "pack open");
    walk_result_t r;

    CHECK(walk(&pack, 1, &r), "main walk");
    CHECK(r.text == 9425, "main text=%d", r.text);
    CHECK(r.choices == 0, "main choices=%d", r.choices);
    CHECK(r.ends == 1, "main ends=%d", r.ends);
    CHECK(r.scenes == 20, "main scenes=%d", r.scenes);

    CHECK(walk(&pack, 101, &r), "after walk");
    CHECK(r.text == 1378, "after text=%d", r.text);
    CHECK(r.choices == 1, "after choices=%d", r.choices);
    CHECK(r.choice_target == 424, "after choice target=%d", r.choice_target);
    CHECK(r.ends == 1, "after ends=%d", r.ends);
    CHECK(r.scenes == 6, "after scenes=%d", r.scenes);

    CHECK(walk(&pack, 109, &r), "bonus walk");
    CHECK(r.text == 15 && r.ends == 1, "bonus text=%d end=%d", r.text, r.ends);
    CHECK(walk(&pack, 108, &r), "legacy walk");
    CHECK(r.text == 157 && r.ends == 1, "legacy text=%d end=%d", r.text, r.ends);

    uint16_t idx;
    stella_scene_t scene;
    // 30 个数字块全部可打开；每个 next/choice 目标都落在同一场景的操作范围内。
    CHECK(pack.scene_count == 30, "scene_count=%u", (unsigned)pack.scene_count);
    for (uint16_t i = 0; i < pack.scene_count; ++i) {
        stella_scene_t sc;
        CHECK(stella_pack_scene(&pack, i, &sc), "scene %u", (unsigned)i);
        uint16_t n = 0;
        const uint8_t *ops = stella_pack_ops(&pack, &sc, &n);
        CHECK(ops != NULL && n > 0, "scene %u ops", (unsigned)sc.id);
        if (sc.next != STELLA_NONE) {
            CHECK(stella_pack_find_scene(&pack, sc.next, &idx), "next %u -> %u",
                  (unsigned)sc.id, (unsigned)sc.next);
        }
        for (uint16_t oi = 0; oi < n; ++oi) {
            stella_op_t o;
            CHECK(stella_pack_op(ops, n, oi, &o), "op %u/%u", (unsigned)sc.id, (unsigned)oi);
            if (o.type == STELLA_OP_CHOICE) {
                CHECK(o.arg < n, "choice target %u/%u", (unsigned)sc.id, (unsigned)o.arg);
            }
        }
    }
    CHECK(stella_pack_find_scene(&pack, 101, &idx) && stella_pack_scene(&pack, idx, &scene),
          "scene 101");
    CHECK((scene.flags & STELLA_SCENE_MAIN_CLEAR) != 0, "after story must require main clear");
    CHECK(stella_pack_find_scene(&pack, 109, &idx) && stella_pack_scene(&pack, idx, &scene),
          "scene 109");
    CHECK((scene.flags & STELLA_SCENE_AFTER_CLEAR) != 0, "bonus must require after clear");

    stella_save_t main_save = { .scene = 1, .op = 0, .page = 0, .flags = STELLA_FLAG_MAIN_CLEAR };
    stella_save_t after_save = { .scene = 101, .op = 0, .page = 0, .flags = 0 };
    uint8_t raw[STELLA_SAVE_BYTES];
    stella_save_t decoded;
    CHECK(stella_save_encode(&main_save, raw, sizeof(raw)) == STELLA_SAVE_BYTES, "encode main");
    CHECK(stella_save_decode(&decoded, raw, sizeof(raw)) && decoded.scene == 1, "decode main");
    CHECK(stella_save_encode(&after_save, raw, sizeof(raw)) == STELLA_SAVE_BYTES, "encode after");
    CHECK(stella_save_decode(&decoded, raw, sizeof(raw)) && decoded.scene == 101, "decode after");

    stella_unlock_t unlock;
    stella_unlock_init(&unlock);
    CHECK(!stella_unlock_has_flag(&unlock, STELLA_FLAG_MAIN_CLEAR), "initial flag");
    stella_unlock_set_flag(&unlock, STELLA_FLAG_MAIN_CLEAR);
    CHECK(stella_unlock_has_flag(&unlock, STELLA_FLAG_MAIN_CLEAR), "main clear");
    CHECK(!stella_unlock_has_flag(&unlock, STELLA_FLAG_AFTER_CLEAR), "after not cleared");
    stella_unlock_set_flag(&unlock, STELLA_FLAG_AFTER_CLEAR);
    CHECK(stella_unlock_has_flag(&unlock, STELLA_FLAG_AFTER_CLEAR), "after clear");

    free(blob);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: main=%d after=%d bonus=%d legacy=%d\n", 9425, 1378, 15, 157);
    return 0;
}
