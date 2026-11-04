/*
 * tests/test_eden_script.c —— 剧本执行器的宿主机行为测试。
 *
 * 用内存里拼出来的小资源包(不是生产包)钉住引擎语义,这样每条规则都能单独构造:
 *   set / if(true,false) / goto / jumpScene / end / 场景读完 -> 按章节表落到下一章 /
 *   非法跳转目标 / 跳转环 / seek 校验。
 *
 * 构建:cc -std=c11 -Wall -Wextra -Werror -Imain \
 *         tests/test_eden_script.c main/eden_pack.c main/eden_script.c
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

// ---------------------------------------------------------------- 包构造
#define MAX_SECTIONS 10

typedef struct {
    uint8_t bytes[8192];
    size_t size;
} pack_buf_t;

typedef struct {
    uint32_t kind;
    size_t offset;
    size_t count;
    size_t size;
} section_place_t;

static void put_u32(pack_buf_t *buf, uint32_t value)
{
    for (int index = 0; index < 4; index++) {
        buf->bytes[buf->size++] = (uint8_t)((value >> (8 * index)) & 0xFF);
    }
}

typedef struct {
    eden_op_t ops[64];
    int op_count;
    // 场景 -> [start, count]
    uint16_t scene_start[8];
    uint16_t scene_count[8];
    uint8_t scene_used[8];
    const char *texts[8];
    int text_count;
    eden_chapter_t chapters[8];
    int chapter_count;
} script_t;

static int intern_text(script_t *script, const char *value)
{
    for (int index = 0; index < script->text_count; index++) {
        if (strcmp(script->texts[index], value) == 0) {
            return index;
        }
    }
    script->texts[script->text_count] = value;
    return script->text_count++;
}


// 把 script_t 拼成一个真实格式的 EDENPK01 包(仅够引擎跑通:1 张假图 + 1 个画面)。
static void build_pack(pack_buf_t *buf, const script_t *script)
{
    memset(buf, 0, sizeof(*buf));

    // 文本池与偏移表
    uint8_t text[512];
    size_t text_size = 0;
    uint32_t offsets[9];
    for (int index = 0; index < script->text_count; index++) {
        offsets[index] = (uint32_t)text_size;
        const char *value = script->texts[index];
        while (*value != '\0' && text_size + 1 < sizeof(text)) {
            text[text_size++] = (uint8_t)*value++;
        }
        text[text_size++] = 0;
    }
    offsets[script->text_count] = (uint32_t)text_size;

    // 指令条数由场景区间推出,测试里只需要摆好每条指令和场景表。
    int op_count = 0;
    for (int index = 0; index < 8; index++) {
        if (script->scene_used[index]) {
            const int end = script->scene_start[index] + script->scene_count[index];
            if (end > op_count) {
                op_count = end;
            }
        }
    }

    // 各段内容
    uint8_t ops[64 * 12];
    size_t ops_size = (size_t)op_count * 12;
    for (int index = 0; index < op_count; index++) {
        const eden_op_t *op = &script->ops[index];
        uint8_t *dst = ops + (size_t)index * 12;
        dst[0] = op->kind;
        dst[1] = op->a;
        dst[2] = (uint8_t)(op->b & 0xFF);
        dst[3] = (uint8_t)(op->b >> 8);
        for (int byte = 0; byte < 4; byte++) {
            dst[4 + byte] = (uint8_t)((op->c >> (8 * byte)) & 0xFF);
            dst[8 + byte] = (uint8_t)((op->d >> (8 * byte)) & 0xFF);
        }
    }

    int max_scene = 0;
    for (int index = 0; index < 8; index++) {
        if (script->scene_used[index] && index > max_scene) {
            max_scene = index;
        }
    }
    uint8_t scenes[8 * 8];
    size_t scenes_size = (size_t)(max_scene + 1) * 8;
    memset(scenes, 0, sizeof(scenes));
    for (int scene = 1; scene <= max_scene; scene++) {
        if (!script->scene_used[scene]) {
            continue;
        }
        const uint32_t start = script->scene_start[scene];
        const uint32_t count = script->scene_count[scene];
        for (int byte = 0; byte < 4; byte++) {
            scenes[scene * 8 + byte] = (uint8_t)((start >> (8 * byte)) & 0xFF);
            scenes[scene * 8 + 4 + byte] = (uint8_t)(((start + count) >> (8 * byte)) & 0xFF);
        }
    }

    uint8_t images[16] = { 0 };
    images[4] = 1;    // data_len = 1
    images[8] = 8;    // width  = 8 (u16 @8..9)
    images[10] = 8;   // height = 8 (u16 @10..11)
    images[12] = 8;   // content_w = 8 (u16 @12..13)
    images[14] = 8;   // content_h = 8 (u16 @14..15)

    uint8_t frames[6] = { 0, 0, 0, 0, 0, 0 };   // base = 0, 无补丁

    uint8_t chapters[8 * 8];
    size_t chapters_size = (size_t)script->chapter_count * 8;
    for (int index = 0; index < script->chapter_count; index++) {
        const eden_chapter_t *chapter = &script->chapters[index];
        uint8_t *dst = chapters + (size_t)index * 8;
        dst[0] = (uint8_t)(chapter->scene & 0xFF);
        dst[1] = (uint8_t)(chapter->scene >> 8);
        dst[2] = (uint8_t)(chapter->name & 0xFF);
        dst[3] = (uint8_t)(chapter->name >> 8);
        dst[4] = (uint8_t)(chapter->op & 0xFF);
        dst[5] = (uint8_t)(chapter->op >> 8);
    }

    uint8_t gallery[6] = { 0, 0, 0, 0, 0, 0 };   // frame 0 / group 0 / label 0
    uint8_t blob[1] = { 0x00 };

    struct { uint32_t kind; const void *data; size_t size; size_t count; } parts[] = {
        { EDEN_SEC_TEXT, text, text_size, (size_t)script->text_count },
        { EDEN_SEC_TXIDX, offsets, (size_t)(script->text_count + 1) * 4,
          (size_t)(script->text_count + 1) },
        { EDEN_SEC_OPS, ops, ops_size, (size_t)op_count },
        { EDEN_SEC_SCENE, scenes, scenes_size, scenes_size / 8 },
        { EDEN_SEC_IMAGE, images, sizeof(images), 1 },
        { EDEN_SEC_FRAME, frames, sizeof(frames), 1 },
        { EDEN_SEC_CHAP, chapters, chapters_size, (size_t)script->chapter_count },
        { EDEN_SEC_GAL, gallery, sizeof(gallery), 1 },
        { EDEN_SEC_BLOB, blob, sizeof(blob), sizeof(blob) },
    };
    const size_t part_count = sizeof(parts) / sizeof(parts[0]);

    const size_t header = 20 + 10 * 16;
    size_t offset = header;
    size_t places[MAX_SECTIONS];
    for (size_t index = 0; index < part_count; index++) {
        places[index] = offset;
        offset += parts[index].size;
        offset += (4 - (offset % 4)) % 4;
    }
    buf->size = offset;

    memcpy(buf->bytes, EDEN_PACK_MAGIC, 8);
    uint8_t *header_ptr = buf->bytes + 8;
    header_ptr[0] = (uint8_t)(EDEN_PACK_VERSION & 0xFF);
    header_ptr[1] = (uint8_t)(EDEN_PACK_VERSION >> 8);
    header_ptr[2] = (uint8_t)(header & 0xFF);
    header_ptr[3] = (uint8_t)((header >> 8) & 0xFF);
    header_ptr[4] = 10;          // section_count
    header_ptr[6] = 0;
    for (int byte = 0; byte < 4; byte++) {
        header_ptr[8 + byte] = (uint8_t)((buf->size >> (8 * byte)) & 0xFF);
    }
    for (size_t index = 0; index < part_count; index++) {
        pack_buf_t record;
        memset(&record, 0, sizeof(record));
        put_u32(&record, parts[index].kind);
        put_u32(&record, (uint32_t)places[index]);
        put_u32(&record, (uint32_t)parts[index].count);
        put_u32(&record, (uint32_t)parts[index].size);
        memcpy(buf->bytes + 20 + index * 16, record.bytes, 16);
        memcpy(buf->bytes + places[index], parts[index].data, parts[index].size);
    }
    // 未使用的段(PATCH)在头部里补一条空记录,保持 section_count = 10。
    pack_buf_t empty;
    memset(&empty, 0, sizeof(empty));
    put_u32(&empty, EDEN_SEC_PATCH);
    put_u32(&empty, (uint32_t)buf->size);
    put_u32(&empty, 0);
    put_u32(&empty, 0);
    memcpy(buf->bytes + 20 + part_count * 16, empty.bytes, 16);
}


// ---------------------------------------------------------------- 场景 1:章节表驱动的跨场景
// 章节顺序故意设成 1 -> 3 -> 2(不是 scene+1):场景 1 读完后必须落到场景 3。
// 这条就是 P2-6 要的行为 —— 跨场景推进读 catalog.chapters 的顺序,而不是 scene+1。
static void test_chapter_order_fallthrough(void)
{
    script_t script;
    memset(&script, 0, sizeof(script));
    const int t_a = intern_text(&script, "a");
    const int t_bb = intern_text(&script, "bb");

    script.scene_used[1] = 1;
    script.scene_start[1] = 0;
    script.ops[0] = (eden_op_t){ EDEN_OP_SET, 0, 0, 1, 0 };              // var0 = 1
    script.ops[1] = (eden_op_t){ EDEN_OP_IF, EDEN_CMP_EQ, 0, 1, 3 };      // if var0==1 -> 3
    script.ops[2] = (eden_op_t){ EDEN_OP_TEXT, 0, 0, (uint32_t)t_a, 0 };  // 被 if 跳过
    script.ops[3] = (eden_op_t){ EDEN_OP_TEXT, 0, 1, (uint32_t)t_bb, 0 }; // 第一句
    script.ops[4] = (eden_op_t){ EDEN_OP_GOTO, 0, 0, 0, 5 };
    script.ops[5] = (eden_op_t){ EDEN_OP_TEXT, 0, 0, (uint32_t)t_a, 0 };  // 第二句,读完走下一章
    script.scene_count[1] = 6;

    script.scene_used[2] = 1;
    script.scene_start[2] = 6;
    script.ops[6] = (eden_op_t){ EDEN_OP_TEXT, 0, 2, (uint32_t)t_bb, 0 }; // 不该被走到
    script.scene_count[2] = 1;

    script.scene_used[3] = 1;
    script.scene_start[3] = 7;
    script.ops[7] = (eden_op_t){ EDEN_OP_TEXT, 0, 3, (uint32_t)t_a, 0 };  // 第三句
    script.ops[8] = (eden_op_t){ EDEN_OP_END, 0, 0, 0, 0 };
    script.scene_count[3] = 2;

    script.chapters[0] = (eden_chapter_t){ 1, 0, 0 };
    script.chapters[1] = (eden_chapter_t){ 3, 0, 7 };
    script.chapters[2] = (eden_chapter_t){ 2, 0, 6 };
    script.chapter_count = 3;

    pack_buf_t buf;
    build_pack(&buf, &script);
    eden_pack_t pack;
    CHECK(eden_pack_open(&pack, buf.bytes, (uint32_t)buf.size), "合成包打不开");

    eden_state_t state;
    eden_state_reset(&state, 1);
    state.op = 0;

    const char *expected_text[] = { "bb", "a", "a" };
    const uint16_t expected_speaker[] = { 1, 0, 3 };
    const uint16_t expected_scene[] = { 1, 1, 3 };
    for (int index = 0; index < 3; index++) {
        eden_line_t line;
        const eden_step_t kind = eden_script_step(&pack, &state, &line);
        CHECK(kind == EDEN_STEP_TEXT, "第 %d 句:kind=%d", index + 1, (int)kind);
        if (kind != EDEN_STEP_TEXT) {
            return;
        }
        const char *text = eden_pack_text(&pack, line.text);
        CHECK(text != NULL && strcmp(text, expected_text[index]) == 0,
              "第 %d 句文本 %s,期望 %s", index + 1, text != NULL ? text : "(null)",
              expected_text[index]);
        CHECK(line.speaker == expected_speaker[index], "第 %d 句人名 %u,期望 %u",
              index + 1, (unsigned)line.speaker, (unsigned)expected_speaker[index]);
        CHECK(line.scene == expected_scene[index], "第 %d 句场景 %u,期望 %u",
              index + 1, (unsigned)line.scene, (unsigned)expected_scene[index]);
        eden_script_advance(&state, &pack);
    }
    eden_line_t end_line;
    CHECK(eden_script_step(&pack, &state, &end_line) == EDEN_STEP_END, "第 4 步应为剧本结束");
    CHECK(state.vars[0] == 1, "变量 var0=%d,期望 1", (int)state.vars[0]);
}

// ---------------------------------------------------------------- 场景 2:jumpScene / if 假分支 / end
static void test_jump_and_end(void)
{
    script_t script;
    memset(&script, 0, sizeof(script));
    const int t_a = intern_text(&script, "a");
    const int t_bb = intern_text(&script, "bb");

    script.scene_used[1] = 1;
    script.scene_start[1] = 0;
    script.ops[0] = (eden_op_t){ EDEN_OP_TEXT, 0, 1, (uint32_t)t_bb, 0 }; // 第一句
    script.scene_count[1] = 1;

    script.scene_used[2] = 1;
    script.scene_start[2] = 1;
    script.ops[1] = (eden_op_t){ EDEN_OP_TEXT, 0, 2, (uint32_t)t_bb, 0 }; // 第二句
    script.ops[2] = (eden_op_t){ EDEN_OP_IF, EDEN_CMP_LE, 0, 0, 3 };      // var0<=0 不成立
    script.ops[3] = (eden_op_t){ EDEN_OP_JUMP, 0, 0, 3, 0 };              // 跳到场景 3
    script.scene_count[2] = 3;

    script.scene_used[3] = 1;
    script.scene_start[3] = 4;
    script.ops[4] = (eden_op_t){ EDEN_OP_TEXT, 0, 3, (uint32_t)t_a, 0 };  // 第三句
    script.ops[5] = (eden_op_t){ EDEN_OP_END, 0, 0, 0, 0 };
    script.scene_count[3] = 2;

    script.chapters[0] = (eden_chapter_t){ 1, 0, 0 };
    script.chapters[1] = (eden_chapter_t){ 2, 0, 1 };
    script.chapters[2] = (eden_chapter_t){ 3, 0, 4 };
    script.chapter_count = 3;

    pack_buf_t buf;
    build_pack(&buf, &script);
    eden_pack_t pack;
    CHECK(eden_pack_open(&pack, buf.bytes, (uint32_t)buf.size), "合成包打不开");

    eden_state_t state;
    eden_state_reset(&state, 1);
    state.op = 0;

    const char *expected_text[] = { "bb", "bb", "a" };
    const uint16_t expected_scene[] = { 1, 2, 3 };
    for (int index = 0; index < 3; index++) {
        eden_line_t line;
        const eden_step_t kind = eden_script_step(&pack, &state, &line);
        CHECK(kind == EDEN_STEP_TEXT, "第 %d 句:kind=%d", index + 1, (int)kind);
        if (kind != EDEN_STEP_TEXT) {
            return;
        }
        CHECK(strcmp(eden_pack_text(&pack, line.text), expected_text[index]) == 0,
              "第 %d 句文本不符", index + 1);
        CHECK(line.scene == expected_scene[index], "第 %d 句场景 %u,期望 %u",
              index + 1, (unsigned)line.scene, (unsigned)expected_scene[index]);
        eden_script_advance(&state, &pack);
    }
    eden_line_t end_line;
    CHECK(eden_script_step(&pack, &state, &end_line) == EDEN_STEP_END, "end 指令后应结束");
}

// ---------------------------------------------------------------- 场景 3:坏目标与死循环
static void test_bad_targets(void)
{
    script_t script;
    memset(&script, 0, sizeof(script));
    const int t_a = intern_text(&script, "a");

    script.scene_used[1] = 1;
    script.scene_start[1] = 0;
    script.ops[0] = (eden_op_t){ EDEN_OP_GOTO, 0, 0, 0, 40 };   // 越过场景范围
    script.scene_count[1] = 1;
    script.chapters[0] = (eden_chapter_t){ 1, 0, 0 };
    script.chapter_count = 1;
    (void)t_a;

    pack_buf_t buf;
    build_pack(&buf, &script);
    eden_pack_t pack;
    CHECK(eden_pack_open(&pack, buf.bytes, (uint32_t)buf.size), "合成包打不开");

    eden_state_t state;
    eden_state_reset(&state, 1);
    state.op = 0;
    eden_line_t line;
    // goto 指出场景:step 会先判定 op>=end -> 走章节后继;最后一章 -> END。
    // 这里真正要钉的是"绝不越界读指令"。
    const eden_step_t kind = eden_script_step(&pack, &state, &line);
    CHECK(kind == EDEN_STEP_END || kind == EDEN_STEP_YIELD, "越界跳转应结束或 yield,实际 %d",
          (int)kind);

    // 跳转环:goto 自己,必须被步数上限截断而不是卡死。
    script_t loop;
    memset(&loop, 0, sizeof(loop));
    (void)intern_text(&loop, "a");   // 文本池不能为空(包里 TEXT 是必备段)
    loop.scene_used[1] = 1;
    loop.scene_start[1] = 0;
    loop.ops[0] = (eden_op_t){ EDEN_OP_GOTO, 0, 0, 0, 0 };
    loop.scene_count[1] = 1;
    loop.chapters[0] = (eden_chapter_t){ 1, 0, 0 };
    loop.chapter_count = 1;
    build_pack(&buf, &loop);
    CHECK(eden_pack_open(&pack, buf.bytes, (uint32_t)buf.size), "合成包打不开");
    eden_state_reset(&state, 1);
    state.op = 0;
    CHECK(eden_script_step(&pack, &state, &line) == EDEN_STEP_YIELD, "跳转环应 yield");
}

// ---------------------------------------------------------------- 场景 4:seek 校验
static void test_seek(void)
{
    script_t script;
    memset(&script, 0, sizeof(script));
    const int t_a = intern_text(&script, "a");
    script.scene_used[1] = 1;
    script.scene_start[1] = 0;
    script.scene_count[1] = 2;
    script.ops[0] = (eden_op_t){ EDEN_OP_TEXT, 0, 0, (uint32_t)t_a, 0 };
    script.ops[1] = (eden_op_t){ EDEN_OP_END, 0, 0, 0, 0 };
    script.chapters[0] = (eden_chapter_t){ 1, 0, 0 };
    script.chapter_count = 1;

    pack_buf_t buf;
    build_pack(&buf, &script);
    eden_pack_t pack;
    CHECK(eden_pack_open(&pack, buf.bytes, (uint32_t)buf.size), "合成包打不开");

    eden_state_t state;
    eden_state_reset(&state, 1);
    CHECK(!eden_state_seek(&state, &pack, 2, 0), "场景 2 不存在,seek 应失败");
    CHECK(!eden_state_seek(&state, &pack, 1, 99), "越界 op 应被拒绝");
    CHECK(eden_state_seek(&state, &pack, 1, 0), "合法位置应通过");
    CHECK(state.scene == 1 && state.op == 0, "seek 后状态不对");
}

int main(void)
{
    test_chapter_order_fallthrough();
    test_jump_and_end();
    test_bad_targets();
    test_seek();
    if (failures != 0) {
        fprintf(stderr, "%d 项检查失败\n", failures);
        return 1;
    }
    printf("eden script engine: OK\n");
    return 0;
}
