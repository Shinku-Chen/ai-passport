/*
 * tests/test_eden_pack.c —— 资源包解析与剧本执行器的 C 契约测试(宿主机运行)。
 *
 * 只断言"包自身必须成立"的不变量与坏包拒绝;源⇄目标逐条等价由
 * tests/test_eden_source_audit.py 用独立实现审计(需要源仓库路径)。
 * 这里不写死 21/7830/2161 之类的绝对数字——那是审计的结论,不是包的契约。
 *
 * 用法: test_eden_pack [pack 路径]   缺省 main/eden_data/eden_pack.bin
 */
#include "eden_pack.h"
#include "eden_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

#define CANVAS_W 264u
#define CANVAS_H 168u
#define PATCH_W_MAX 104u

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        return NULL;
    }
    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return NULL;
    }
    const long length = ftell(handle);
    if (length <= 0 || fseek(handle, 0, SEEK_SET) != 0) {
        fclose(handle);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (data == NULL) {
        fclose(handle);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, handle) != (size_t)length) {
        free(data);
        fclose(handle);
        return NULL;
    }
    fclose(handle);
    *size = (uint32_t)length;
    return data;
}

static void check_layout(const eden_pack_t *pack)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    uint32_t size = 0;

    CHECK(eden_pack_section(pack, EDEN_SEC_OPS, &data, &count, &size), "ops 段缺失");
    CHECK(count == size / 12u, "ops 记录尺寸不符");
    CHECK(eden_pack_scene_count(pack) > 0, "没有场景");
    CHECK(eden_pack_chapter_count(pack) == eden_pack_scene_count(pack),
          "章节数 %u != 场景数 %u", (unsigned)eden_pack_chapter_count(pack),
          (unsigned)eden_pack_scene_count(pack));
    CHECK(eden_pack_gallery_count(pack) > 0, "鉴赏表为空");

    // 场景表必须连续、有序、覆盖全部指令(跳转的越界检查才有意义)。
    uint32_t expect = 0;
    for (uint16_t scene = 1; scene <= eden_pack_scene_count(pack); scene++) {
        uint32_t first = 0;
        uint32_t length = 0;
        CHECK(eden_pack_scene_range(pack, scene, &first, &length), "场景 %u 区间非法",
              (unsigned)scene);
        CHECK(first == expect, "场景 %u 起点 %u,期望 %u", (unsigned)scene, (unsigned)first,
              (unsigned)expect);
        CHECK(length > 0, "场景 %u 为空", (unsigned)scene);
        expect = first + length;
    }
    CHECK(expect == count, "场景表覆盖 %u 条指令,实际 %u", (unsigned)expect, (unsigned)count);

    // 场景号 -> 章节下标必须一一对应(跨场景推进依赖它)。
    for (uint16_t scene = 1; scene <= eden_pack_scene_count(pack); scene++) {
        CHECK(eden_script_chapter_of(pack, scene) >= 0, "场景 %u 没有对应章节", (unsigned)scene);
    }
}

// 每条指令都要能被解释:文本/人名可解析、画面几何合法、跳转目标在场景内。
static void check_ops(const eden_pack_t *pack)
{
    unsigned text_ops = 0;
    for (uint16_t scene = 1; scene <= eden_pack_scene_count(pack); scene++) {
        uint32_t first = 0;
        uint32_t length = 0;
        if (!eden_pack_scene_range(pack, scene, &first, &length)) {
            continue;
        }
        const uint32_t end = first + length;
        for (uint32_t index = first; index < end; index++) {
            eden_op_t op;
            CHECK(eden_pack_op(pack, index, &op), "指令 %u 读取失败", (unsigned)index);
            switch (op.kind) {
            case EDEN_OP_TEXT:
                text_ops++;
                CHECK(eden_pack_text(pack, op.c) != NULL, "场景 %u 指令 %u 文本非法",
                      (unsigned)scene, (unsigned)index);
                CHECK(eden_pack_text(pack, op.a) != NULL, "场景 %u 指令 %u 人名非法",
                      (unsigned)scene, (unsigned)index);
                CHECK(op.d < eden_pack_frame_count(pack), "场景 %u 指令 %u 画面越界",
                      (unsigned)scene, (unsigned)index);
                break;
            case EDEN_OP_SET:
                break;
            case EDEN_OP_IF:
                CHECK(op.a <= EDEN_CMP_LE, "场景 %u 指令 %u 比较符非法", (unsigned)scene,
                      (unsigned)index);
                CHECK(op.d >= first && op.d <= end, "场景 %u 指令 %u if 目标 %u 越界",
                      (unsigned)scene, (unsigned)index, (unsigned)op.d);
                break;
            case EDEN_OP_GOTO:
                CHECK(op.d >= first && op.d <= end, "场景 %u 指令 %u goto 目标 %u 越界",
                      (unsigned)scene, (unsigned)index, (unsigned)op.d);
                break;
            case EDEN_OP_JUMP:
                CHECK(op.c >= 1 && op.c <= eden_pack_scene_count(pack),
                      "场景 %u 指令 %u jump 目标越界", (unsigned)scene, (unsigned)index);
                break;
            case EDEN_OP_END:
                break;
            default:
                CHECK(false, "场景 %u 指令 %u 未知种类 %u", (unsigned)scene,
                      (unsigned)index, (unsigned)op.kind);
                break;
            }
        }
    }
    CHECK(text_ops > 0, "包里一句对白都没有");
    printf("text ops: %u\n", text_ops);
}

// 每个画面都要通过几何校验:底图放得下、补丁整块在画面内且不超过暂存条。
static void check_frames(const eden_pack_t *pack)
{
    const uint16_t frames = eden_pack_frame_count(pack);
    unsigned patched = 0;
    for (uint16_t index = 0; index < frames; index++) {
        eden_frame_t frame;
        CHECK(eden_pack_frame_checked(pack, index, CANVAS_W, CANVAS_H, PATCH_W_MAX, &frame),
              "画面 %u 几何非法", (unsigned)index);
        if (frame.patch_count > 0) {
            patched++;
        }
    }
    printf("frames: %u (patched %u)\n", (unsigned)frames, patched);
}

// 走一遍完整剧情,必须有终点(不允许死循环/越界)。
static void check_playthrough(const eden_pack_t *pack)
{
    eden_state_t state;
    eden_state_reset(&state, 1);
    uint32_t first = 0;
    if (!eden_pack_scene_range(pack, 1, &first, NULL)) {
        CHECK(false, "场景 1 不可用");
        return;
    }
    state.op = first;

    unsigned lines = 0;
    for (unsigned step = 0; step < 200000; step++) {
        eden_line_t line;
        const eden_step_t kind = eden_script_step(pack, &state, &line);
        if (kind == EDEN_STEP_END) {
            printf("playthrough: %u lines, %u steps\n", lines, step);
            CHECK(lines > 0, "完整剧情一句都没有");
            return;
        }
        CHECK(kind == EDEN_STEP_TEXT, "指令 %u 处 yield(kind=%d)", (unsigned)state.op,
              (int)kind);
        if (kind != EDEN_STEP_TEXT) {
            return;
        }
        CHECK(eden_pack_text(pack, line.text) != NULL, "文本下标非法");
        lines++;
        eden_script_advance(&state, pack);
    }
    CHECK(false, "完整剧情走不完(疑似跳转环)");
}

// 坏包回归:篡改真实包的字节,解析/几何校验必须拒绝。
static void check_bad_pack(uint8_t *blob, uint32_t size)
{
    eden_pack_t pack;

    CHECK(!eden_pack_open(&pack, blob, size - 1), "长度不符的包未被拒绝");

    const uint8_t original = blob[0];
    blob[0] ^= 0xFF;
    CHECK(!eden_pack_open(&pack, blob, size), "魔数错误的包未被拒绝");
    blob[0] = original;

    CHECK(eden_pack_open(&pack, blob, size), "基准包应能打开");

    // 找一个带补丁的画面,把补丁的 x 改到画面外:几何校验必须拒绝。
    uint16_t frame_with_patch = 0xFFFF;
    eden_frame_t frame;
    for (uint16_t index = 0; index < eden_pack_frame_count(&pack); index++) {
        if (eden_pack_frame(&pack, index, &frame) && frame.patch_count > 0) {
            frame_with_patch = index;
            break;
        }
    }
    CHECK(frame_with_patch != 0xFFFF, "包里没有带补丁的画面,无法做越界回归");
    if (frame_with_patch != 0xFFFF) {
        const uint32_t record = pack.sections[EDEN_SEC_PATCH].offset +
                                (uint32_t)frame.patch_first * 6u;
        const uint8_t x_low = blob[record + 2];
        const uint8_t x_high = blob[record + 3];
        blob[record + 2] = (uint8_t)(CANVAS_W & 0xFF);        // x = 264 -> 整块在外
        blob[record + 3] = (uint8_t)(CANVAS_W >> 8);
        CHECK(eden_pack_open(&pack, blob, size), "改坐标后包结构仍应可解析");
        CHECK(!eden_pack_frame_checked(&pack, frame_with_patch, CANVAS_W, CANVAS_H,
                                       PATCH_W_MAX, &frame),
              "补丁越界未被几何校验拒绝");
        blob[record + 2] = x_low;
        blob[record + 3] = x_high;

        // 把补丁宽度改到超过暂存条:同样必须拒绝。
        const uint32_t image_index = (uint32_t)blob[record] | ((uint32_t)blob[record + 1] << 8);
        const uint32_t image_record = pack.sections[EDEN_SEC_IMAGE].offset + image_index * 16u;
        const uint8_t w_low = blob[image_record + 8];
        const uint8_t w_high = blob[image_record + 9];
        blob[image_record + 8] = (uint8_t)((PATCH_W_MAX + 8) & 0xFF);
        blob[image_record + 9] = 0;
        CHECK(eden_pack_open(&pack, blob, size), "改宽度后包结构仍应可解析");
        CHECK(!eden_pack_frame_checked(&pack, frame_with_patch, CANVAS_W, CANVAS_H,
                                       PATCH_W_MAX, &frame),
              "补丁超宽未被几何校验拒绝");
        blob[image_record + 8] = w_low;
        blob[image_record + 9] = w_high;
    }

    // 鉴赏条目指向不存在的画面:必须被拒绝。
    if (eden_pack_gallery_count(&pack) > 0) {
        const uint32_t gal = pack.sections[EDEN_SEC_GAL].offset;
        const uint8_t low = blob[gal];
        const uint8_t high = blob[gal + 1];
        blob[gal] = 0xFF;
        blob[gal + 1] = 0xFF;
        CHECK(eden_pack_open(&pack, blob, size), "改鉴赏后包结构仍应可解析");
        eden_gallery_t item;
        CHECK(!eden_pack_gallery(&pack, 0, &item), "越界鉴赏画面未被拒绝");
        blob[gal] = low;
        blob[gal + 1] = high;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/eden_data/eden_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = read_file(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "FAIL 读不到资源包: %s\n", path);
        return 1;
    }

    eden_pack_t pack;
    if (!eden_pack_open(&pack, blob, size)) {
        fprintf(stderr, "FAIL 资源包校验失败: %s\n", path);
        free(blob);
        return 1;
    }
    printf("pack: %u bytes\n", (unsigned)size);

    check_layout(&pack);
    check_ops(&pack);
    check_frames(&pack);
    check_playthrough(&pack);
    check_bad_pack(blob, size);

    free(blob);
    if (failures != 0) {
        fprintf(stderr, "%d 项检查失败\n", failures);
        return 1;
    }
    printf("eden pack + script: OK\n");
    return 0;
}
