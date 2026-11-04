// tests/test_nn_save.c —— 存档记录编解码 + 恢复路径的宿主测试。
//
// 覆盖（NVS 本身在宿主上不可用，所以测的是可纯逻辑化的那一半）：
//   1. 编解码往返：所有字段原样回来；
//   2. 坏记录：长度不对 / 魔数不对 / 版本不对 / 载荷长度不对，一律拒收；
//   3. 字符串安全：chapter/preview 没有 NUL 结尾时，解码后必须补上；
//   4. 与资源包自洽：scene/op/图像 id 越界的记录不能拿去恢复；
//   5. 恢复路径：真实位置编码 → 解码 → nn_rt_seek + nn_rt_run，落回同一句。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_pack.h"
#include "nn_save.h"
#include "nn_save_codec.h"
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
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) { free(buf); fclose(fh); return NULL; }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

static void make_save(nn_save_t *s)
{
    memset(s, 0, sizeof(*s));
    s->scene = 5;
    s->op = 42;
    s->bg = 3;
    s->cg = NN_NONE16;
    s->sprite = 60;
    s->stamp = 12345;
    s->cg_mask = 0x1234;
    s->ending_mask = 0x0A;
    snprintf(s->chapter, sizeof(s->chapter), "chunk006");
    snprintf(s->preview, sizeof(s->preview), "测试预览");
}

int main(int argc, char **argv)
{
    // ---- 1. 往返
    nn_save_t a, b;
    make_save(&a);
    uint8_t rec[NN_SAVE_RECORD_BYTES];
    size_t len = 0;
    CHECK(nn_save_encode(&a, rec, sizeof(rec), &len), "encode failed");
    CHECK(len == NN_SAVE_RECORD_BYTES, "record size %zu != %u", len, (unsigned)NN_SAVE_RECORD_BYTES);
    CHECK(!nn_save_encode(&a, rec, NN_SAVE_RECORD_BYTES - 1, &len), "encode accepted a short buffer");
    CHECK(nn_save_decode(rec, len, &b), "decode failed");
    CHECK(b.scene == a.scene && b.op == a.op, "scene/op mismatch");
    CHECK(b.bg == a.bg && b.cg == a.cg && b.sprite == a.sprite, "view mismatch");
    CHECK(b.stamp == a.stamp && b.cg_mask == a.cg_mask && b.ending_mask == a.ending_mask,
          "meta mismatch");
    CHECK(strcmp(b.chapter, a.chapter) == 0 && strcmp(b.preview, a.preview) == 0, "text mismatch");
    printf("  round trip: %u-byte record preserves every field\n", (unsigned)len);

    // ---- 2. 坏记录
    CHECK(!nn_save_decode(rec, len - 1, &b), "short blob accepted");
    CHECK(!nn_save_decode(rec, len + 1, &b), "long blob accepted");
    CHECK(!nn_save_decode(NULL, len, &b), "NULL blob accepted");

    uint8_t bad[NN_SAVE_RECORD_BYTES];
    memcpy(bad, rec, sizeof(bad));
    bad[0] ^= 0xFF;
    CHECK(!nn_save_decode(bad, sizeof(bad), &b), "bad magic accepted");

    memcpy(bad, rec, sizeof(bad));
    bad[4] = (uint8_t)(NN_SAVE_RECORD_VERSION + 1);
    CHECK(!nn_save_decode(bad, sizeof(bad), &b), "unknown version accepted");

    memcpy(bad, rec, sizeof(bad));
    bad[6] = (uint8_t)(sizeof(nn_save_t) + 1);
    CHECK(!nn_save_decode(bad, sizeof(bad), &b), "payload length mismatch accepted");
    printf("  bad records: length/magic/version/payload all rejected\n");

    // ---- 3. 字符串安全：没有 NUL 结尾的 chapter/preview
    memcpy(bad, rec, sizeof(bad));
    memset(bad + 26, 'A', 12);   // chapter 12 字节全 'A'
    memset(bad + 38, 'B', 28);   // preview 28 字节全 'B'
    CHECK(nn_save_decode(bad, sizeof(bad), &b), "record with unterminated strings rejected");
    CHECK(b.chapter[sizeof(b.chapter) - 1] == '\0', "chapter not terminated");
    CHECK(b.preview[sizeof(b.preview) - 1] == '\0', "preview not terminated");
    CHECK(strlen(b.chapter) == sizeof(b.chapter) - 1, "chapter truncated unexpectedly");
    printf("  unterminated strings are clamped on decode\n");

    // ---- 4. 与资源包自洽
    const char *path = argc > 1 ? argv[1] : DEFAULT_PACK;
    size_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        printf("cannot read %s\n", path);
        return 1;
    }
    nn_pack_t pack;
    CHECK(nn_pack_init(&pack, blob, size), "pack did not parse");
    if (!pack.scenes) { free(blob); return 1; }

    nn_save_t ok;
    make_save(&ok);
    ok.scene = 0;
    ok.op = 1;
    ok.bg = NN_NONE16;
    ok.cg = NN_NONE16;
    ok.sprite = NN_NONE16;
    CHECK(nn_save_record_fits_pack(&ok, &pack), "valid record rejected by pack check");

    nn_save_t bad2 = ok;
    bad2.scene = (uint16_t)pack.scene_count;
    CHECK(!nn_save_record_fits_pack(&bad2, &pack), "scene past the last scene accepted");
    bad2 = ok;
    bad2.op = (uint16_t)(pack.scenes[0].op_count + 1);
    CHECK(!nn_save_record_fits_pack(&bad2, &pack), "op past the scene accepted");
    bad2 = ok;
    bad2.bg = (uint16_t)pack.image_count;
    CHECK(!nn_save_record_fits_pack(&bad2, &pack), "image id out of range accepted");
    bad2 = ok;
    bad2.sprite = 0xFFFE;
    CHECK(!nn_save_record_fits_pack(&bad2, &pack), "sprite id out of range accepted");
    printf("  pack consistency: %u scenes, %u images — out-of-range records rejected\n",
           (unsigned)pack.scene_count, (unsigned)pack.image_count);

    // ---- 5. 恢复路径：真实位置 -> 记录 -> 解码 -> seek -> 与存档点同一句
    nn_rt_t rt;
    nn_rt_start(&rt, &pack);
    for (int i = 0; i < 40; ++i) {
        CHECK(nn_rt_run(&rt) == NN_STEP_SAY, "line %d is not a say", i);
    }
    nn_save_t live;
    memset(&live, 0, sizeof(live));
    live.scene = rt.scene;
    live.op = (uint16_t)(rt.op - 1);      // 当前这句 SAY 的下标
    live.bg = rt.view.bg;
    live.cg = rt.view.cg;
    live.sprite = rt.view.sprite;
    snprintf(live.chapter, sizeof(live.chapter), "%s", "chunk");
    const char *want = rt.text;
    const uint16_t want_len = rt.text_len;

    uint8_t live_rec[NN_SAVE_RECORD_BYTES];
    CHECK(nn_save_encode(&live, live_rec, sizeof(live_rec), NULL), "encode live save failed");
    nn_save_t restored;
    CHECK(nn_save_decode(live_rec, sizeof(live_rec), &restored), "decode live save failed");
    CHECK(nn_save_record_fits_pack(&restored, &pack), "live save does not fit the pack");

    nn_view_t view = { .bg = restored.bg, .cg = restored.cg, .sprite = restored.sprite,
                       .chapter = NULL, .chapter_len = 0 };
    nn_rt_t resumed;
    nn_rt_start(&resumed, &pack);
    CHECK(nn_rt_seek(&resumed, restored.scene, restored.op, &view), "seek failed");
    CHECK(nn_rt_run(&resumed) == NN_STEP_SAY, "restored position is not a say");
    CHECK(resumed.text_len == want_len && memcmp(resumed.text, want, want_len) == 0,
          "restored line differs from the saved one");
    CHECK(resumed.view.bg == live.bg && resumed.view.sprite == live.sprite,
          "restored view differs");
    printf("  restore: scene %u op %u replays the same line after encode/decode\n",
           restored.scene, restored.op);

    free(blob);
    if (failures) {
        printf("nn_save: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("nn_save: PASS\n");
    return 0;
}
