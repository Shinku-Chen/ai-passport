// tests/test_nn_script.c —— 《男娘恋爱物语》剧本运行时 + 资源包的宿主测试。
//
// 覆盖:
//   1. 真实资源包解析:段表、场景/操作/选项/结局计数与源剧本一致;
//   2. 全路线遍历:每条分支都走到结局,没有 NN_STEP_BROKEN,4 个结局全部可达,
//      1,248 句对白全部可达;
//   3. 选项不会线性落进错误结局:两个选项点的每个分支都记录其结局 id 并逐条断言;
//   4. 断点恢复:nn_rt_seek() 到任意 (scene, op) 后重跑,下一句与存档点一致;
//   5. 坏包:魔数/版本/长度/段偏移/记录越界都必须被 nn_pack_init 拒绝。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_pack.h"
#include "nn_script.h"

#define DEFAULT_PACK "assets/nn/nn_pack.bin"

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (n <= 0) { fclose(fh); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(fh); return NULL; }
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        free(buf);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

// 全路线遍历的统计
typedef struct {
    int say;
    int choice;
    int endings_seen[8];
    int endings_total;
    int broken;
    int max_depth;
    // 每个选项点的每个选项分别走到哪个结局(用于断言“不会线性落进错误结局”)
    int option_ending[4][4];
    int option_count[4];
} walk_stats_t;

static int walk(nn_rt_t *rt, walk_stats_t *st, int depth)
{
    if (depth > st->max_depth) st->max_depth = depth;
    if (depth > 64) { st->broken++; return -1; }     // 分支递归上限,防止测试自身爆栈
    nn_step_t step;
    while ((step = nn_rt_run(rt)) == NN_STEP_SAY) {
        st->say++;
        if (rt->text == NULL || rt->text_len == 0) st->broken++;
    }
    if (step == NN_STEP_END) {
        if (rt->ending_id < 8) st->endings_seen[rt->ending_id]++;
        st->endings_total++;
        return rt->ending_id;
    }
    if (step == NN_STEP_BROKEN) { st->broken++; return -1; }
    if (step != NN_STEP_CHOICE) { st->broken++; return -1; }

    const int idx = st->choice;
    if (idx > 3) { st->broken++; return -1; }
    st->choice++;
    const uint8_t count = rt->choice_count;
    st->option_count[idx] = count;
    for (uint8_t i = 0; i < count; ++i) {
        nn_rt_t branch = *rt;
        const uint16_t before_scene = branch.scene;
        const uint16_t before_op = branch.op;
        if (!nn_rt_choose(&branch, i)) { st->broken++; continue; }
        // 位置只能原地停留(继续选项)或向前跳,绝不能倒退
        CHECK(branch.scene != before_scene || branch.op >= before_op,
              "option %u of choice at scene %u op %u moved backwards", i, before_scene, before_op);
        uint16_t len = 0;
        const char *label = nn_rt_choice_text(rt, i, &len);
        CHECK(label != NULL && len > 0, "choice option %u has no text", i);
        const int e = walk(&branch, st, depth + 1);
        if (i < 4) st->option_ending[idx][i] = e;
    }
    return -2;   // 本分支没有直接到结局,而是又遇到一个选项点
}

static void test_pack_totals(const nn_pack_t *pack)
{
    printf("pack: %u scenes, %u ops, %u images, %u choices, %u endings, %u speakers\n",
           pack->scene_count, pack->op_count, pack->image_count,
           pack->choice_count, pack->ending_count, pack->speaker_count);
    CHECK(pack->scene_count == 13, "expected 13 scenes, got %u", pack->scene_count);
    CHECK(pack->ending_count == 4, "expected 4 endings, got %u", pack->ending_count);

    int say = 0, choice = 0, end = 0, sprite = 0, bg = 0, ev = 0;
    for (uint32_t i = 0; i < pack->op_count; ++i) {
        switch (pack->ops[i].type) {
        case NN_OP_SAY: say++; break;
        case NN_OP_CHOICE: choice++; break;
        case NN_OP_END: end++; break;
        case NN_OP_SPRITE: sprite++; break;
        case NN_OP_BG: bg++; break;
        case NN_OP_EV: ev++; break;
        default: break;
        }
    }
    CHECK(say == 1248, "expected 1248 dialogue ops, got %d", say);
    CHECK(choice == 2, "expected 2 choice points, got %d", choice);
    CHECK(pack->choice_count == 5, "expected 5 options, got %u", pack->choice_count);
    CHECK(end == 4, "expected 4 ending ops, got %d", end);
    CHECK(sprite == 214, "expected 214 sprite ops, got %d", sprite);
    CHECK(bg == 51, "expected 51 background ops, got %d", bg);
    CHECK(ev == 23, "expected 23 CG ops, got %d", ev);
    printf("  ops: say=%d bg=%d cg=%d sprite=%d choices=%d endings=%d\n",
           say, bg, ev, sprite, choice, end);

    // 每张立绘都是 RGB JPEG + 1bpp 遮罩,且遮罩按行字节对齐
    int sprites = 0;
    for (uint32_t i = 0; i < pack->image_count; ++i) {
        const nn_image_t *img = &pack->images[i];
        CHECK(img->w > 0 && img->h > 0, "image %u has zero size", i);
        if (img->kind != NN_IMG_SPRITE) continue;
        sprites++;
        const uint32_t expect = ((uint32_t)(img->w + 7) / 8u) * img->h;
        CHECK(img->mask_len == expect, "sprite %u mask %u != %u", i, img->mask_len, expect);
        CHECK(img->w <= 168 && img->h <= 240, "sprite %u too large: %ux%u", i, img->w, img->h);
    }
    CHECK(sprites == 30, "expected 30 sprite images, got %d", sprites);
}

static void test_routes(const nn_pack_t *pack)
{
    nn_rt_t rt;
    nn_rt_start(&rt, pack);
    walk_stats_t st;
    memset(&st, 0, sizeof(st));
    walk(&rt, &st, 0);

    printf("  routes: say=%d choices=%d endings=%d (ids:", st.say, st.choice, st.endings_total);
    for (int i = 0; i < 8; ++i) {
        if (st.endings_seen[i]) printf(" %d x%d", i, st.endings_seen[i]);
    }
    printf(")\n");

    CHECK(st.broken == 0, "walk hit %d broken states", st.broken);
    CHECK(st.choice == 2, "expected 2 choice points on the walk, got %d", st.choice);
    CHECK(st.endings_total == 4, "expected 4 terminal routes, got %d", st.endings_total);
    for (int i = 0; i < 4; ++i) {
        CHECK(st.endings_seen[i] == 1, "ending %d reached %d times (want 1)", i, st.endings_seen[i]);
    }
    // 全路线覆盖 = 1,248 句:每条分支都读过,没有遗漏也没有重复
    CHECK(st.say == 1248, "expected 1248 lines across all routes, got %d", st.say);

    // 选项 → 结局的固定映射（源剧本:chunk004 三选一、chunk011 二选一）。
    // 选项 0 = 分支直达结局；最后的“继续”选项必须走真结局，绝不能线性落进 bad end。
    CHECK(st.option_count[0] == 3, "first choice should have 3 options, got %d", st.option_count[0]);
    CHECK(st.option_count[1] == 2, "second choice should have 2 options, got %d", st.option_count[1]);
    CHECK(st.option_ending[0][0] == 0, "choice1/opt0 should end at badend1, got %d", st.option_ending[0][0]);
    CHECK(st.option_ending[0][1] == 1, "choice1/opt1 should end at badend2, got %d", st.option_ending[0][1]);
    CHECK(st.option_ending[0][2] == -2,
          "choice1/opt2 (continue) should fall through to the second choice, got %d",
          st.option_ending[0][2]);
    CHECK(st.option_ending[1][0] == 2, "choice2/opt0 should end at normalend, got %d", st.option_ending[1][0]);
    CHECK(st.option_ending[1][1] == 3, "choice2/opt1 (continue) should reach the true end, got %d",
          st.option_ending[1][1]);
}

static void test_resume(const nn_pack_t *pack)
{
    // 走到第 7 句,记下位置,seek 回去必须得到同一句
    nn_rt_t rt;
    nn_rt_start(&rt, pack);
    for (int i = 0; i < 6; ++i) {
        CHECK(nn_rt_run(&rt) == NN_STEP_SAY, "line %d is not a say", i);
    }
    const uint16_t scene = rt.scene, op = rt.op;
    const nn_view_t view = rt.view;
    CHECK(nn_rt_run(&rt) == NN_STEP_SAY, "7th line is not a say");
    const char *want = rt.text;
    const uint16_t want_len = rt.text_len;

    nn_rt_t resumed;
    nn_rt_start(&resumed, pack);
    CHECK(nn_rt_seek(&resumed, scene, op, &view), "resume seek failed");
    CHECK(nn_rt_run(&resumed) == NN_STEP_SAY, "resume did not land on a say");
    CHECK(resumed.text_len == want_len && memcmp(resumed.text, want, want_len) == 0,
          "resume text mismatch");
    CHECK(resumed.view.bg == view.bg && resumed.view.sprite == view.sprite,
          "resume view mismatch");
    CHECK(!nn_rt_seek(&resumed, pack->scene_count, 0, NULL), "seek past last scene accepted");
    printf("  resume: scene %u op %u restored the same line\n", scene, op);
}

static void test_broken_packs(const uint8_t *blob, size_t size)
{
    nn_pack_t pack;
    uint8_t *copy = malloc(size);
    CHECK(copy != NULL, "alloc");
    if (!copy) return;

    memcpy(copy, blob, size);
    copy[0] = 'X';
    CHECK(!nn_pack_init(&pack, copy, size), "bad magic accepted");

    memcpy(copy, blob, size);
    copy[8] = 9;   // version
    CHECK(!nn_pack_init(&pack, copy, size), "bad version accepted");

    CHECK(!nn_pack_init(&pack, blob, size - 1), "truncated pack accepted");
    CHECK(!nn_pack_init(&pack, blob, 19), "tiny pack accepted");
    CHECK(!nn_pack_init(&pack, NULL, 0), "NULL pack accepted");

    // 段偏移越界:把 IMAGE 段的 offset 改成 size-8
    memcpy(copy, blob, size);
    const uint32_t sections = (uint32_t)copy[16] | ((uint32_t)copy[17] << 8) |
                              ((uint32_t)copy[18] << 16) | ((uint32_t)copy[19] << 24);
    int patched = 0;
    for (uint32_t i = 0; i < sections; ++i) {
        uint8_t *rec = copy + 20 + 16 * i;
        const uint32_t st = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) |
                            ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);
        if (st != NN_SEC_IMAGE) continue;
        const uint32_t bad = (uint32_t)(size - 8);
        rec[4] = (uint8_t)(bad & 0xFF);
        rec[5] = (uint8_t)((bad >> 8) & 0xFF);
        rec[6] = (uint8_t)((bad >> 16) & 0xFF);
        rec[7] = (uint8_t)((bad >> 24) & 0xFF);
        patched = 1;
    }
    CHECK(patched, "no image section to patch");
    CHECK(!nn_pack_init(&pack, copy, size), "out-of-range section accepted");

    free(copy);
    printf("  broken packs: all rejected\n");
}

static void test_ending_texts(const nn_pack_t *pack)
{
    for (uint8_t i = 0; i < pack->ending_count; ++i) {
        uint16_t nlen = 0, tlen = 0;
        const char *name = nn_rt_ending_name(pack, i, &nlen);
        const char *text = nn_rt_ending_text(pack, i, &tlen);
        CHECK(name && nlen > 0, "ending %u has no name", i);
        CHECK(text && tlen > 0, "ending %u has no text", i);
        printf("  ending %u: %.*s\n", i, (int)nlen, name);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : DEFAULT_PACK;
    size_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        printf("cannot read %s (run tools/nn_pack.py first)\n", path);
        return 1;
    }
    printf("nn_script: %s (%zu bytes)\n", path, size);

    nn_pack_t pack;
    CHECK(nn_pack_init(&pack, blob, size), "pack did not parse");

    test_pack_totals(&pack);
    test_routes(&pack);
    test_resume(&pack);
    test_ending_texts(&pack);
    test_broken_packs(blob, size);

    free(blob);
    if (failures) {
        printf("nn_script: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("nn_script: PASS\n");
    return 0;
}
