// main/eden_script.c —— 剧本执行器实现(纯逻辑)。
#include "eden_script.h"

#include <string.h>

// 按章节顺序找 scene 的下一章场景。源 catalog.chapters 的顺序就是 chapterOrder
// (A001..D001),跨场景自然推进必须走这张表,不能假设 scene+1 等价 —— 一旦源里
// 出现场景号与章节顺序不一致(补章、插入支线),scene+1 会读错章。
static bool next_chapter_scene(const eden_pack_t *pack, uint16_t scene, uint16_t *out)
{
    const uint16_t count = eden_pack_chapter_count(pack);
    for (uint16_t index = 0; index < count; index++) {
        eden_chapter_t chapter;
        if (!eden_pack_chapter(pack, index, &chapter) || chapter.scene != scene) {
            continue;
        }
        if (index + 1 >= count) {
            return false;   // 已经是最后一章
        }
        eden_chapter_t next;
        if (!eden_pack_chapter(pack, (uint16_t)(index + 1), &next)) {
            return false;
        }
        *out = next.scene;
        return true;
    }
    // 场景不在章节表里(理论上不会出现):退回按场景号顺序推进。
    if (scene < eden_pack_scene_count(pack)) {
        *out = (uint16_t)(scene + 1);
        return true;
    }
    return false;
}

static bool compare(int32_t left, uint8_t op, int32_t right)
{
    switch (op) {
    case EDEN_CMP_EQ:
        return left == right;
    case EDEN_CMP_LE:
        return left <= right;
    default:
        return false;
    }
}

void eden_state_reset(eden_state_t *state, uint16_t scene)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->scene = scene;
    state->op = 0;   // 由 eden_state_seek / 调用方按场景表修正
}

bool eden_state_seek(eden_state_t *state, const eden_pack_t *pack,
                     uint16_t scene, uint32_t op)
{
    if (state == NULL || pack == NULL) {
        return false;
    }
    uint32_t first = 0;
    uint32_t count = 0;
    if (!eden_pack_scene_range(pack, scene, &first, &count)) {
        return false;
    }
    if (op < first || op >= first + count) {
        return false;
    }
    state->scene = scene;
    state->op = op;
    state->ended = false;
    return true;
}

eden_step_t eden_script_step(const eden_pack_t *pack, eden_state_t *state,
                             eden_line_t *line)
{
    if (line != NULL) {
        memset(line, 0, sizeof(*line));
    }
    if (pack == NULL || state == NULL || line == NULL) {
        return EDEN_STEP_YIELD;
    }
    if (state->ended) {
        line->kind = EDEN_STEP_END;
        line->scene = state->scene;
        return EDEN_STEP_END;
    }

    for (int step = 0; step < EDEN_SCRIPT_STEP_LIMIT; step++) {
        uint32_t first = 0;
        uint32_t count = 0;
        if (!eden_pack_scene_range(pack, state->scene, &first, &count)) {
            return EDEN_STEP_YIELD;
        }
        if (state->op >= first + count) {
            // 走过场景末尾:源工程里"场景读完"由上层跳到下一章(chapterOrder),
            // 这里等价地落到下一个场景;最后一个场景读完才是剧本结束。
            uint16_t successor = 0;
            if (next_chapter_scene(pack, state->scene, &successor)) {
                state->scene = successor;
                if (!eden_pack_scene_range(pack, state->scene, &first, &count)) {
                    return EDEN_STEP_YIELD;
                }
                state->op = first;
                continue;
            }
            state->ended = true;
            line->kind = EDEN_STEP_END;
            line->scene = state->scene;
            return EDEN_STEP_END;
        }

        eden_op_t op;
        if (!eden_pack_op(pack, state->op, &op)) {
            return EDEN_STEP_YIELD;
        }
        switch (op.kind) {
        case EDEN_OP_TEXT:
            line->kind = EDEN_STEP_TEXT;
            line->scene = state->scene;
            line->speaker = op.b;   // TEXT 的 a 只有 8 位,说话人名走 u16 的 b
            line->text = (uint16_t)op.c;
            line->frame = (uint16_t)op.d;
            if (eden_pack_text(pack, op.c) == NULL || op.d >= eden_pack_frame_count(pack)) {
                return EDEN_STEP_YIELD;
            }
            return EDEN_STEP_TEXT;
        case EDEN_OP_SET:
            if (op.b < EDEN_VAR_MAX) {
                state->vars[op.b] = (int32_t)op.c;
            }
            state->op++;
            break;
        case EDEN_OP_IF: {
            const int32_t left = op.b < EDEN_VAR_MAX ? state->vars[op.b] : 0;
            state->op = compare(left, op.a, (int32_t)op.c) ? op.d : state->op + 1;
            break;
        }
        case EDEN_OP_GOTO:
            state->op = op.d;
            break;
        case EDEN_OP_JUMP: {
            uint32_t next_first = 0;
            uint32_t next_count = 0;
            if (!eden_pack_scene_range(pack, (uint16_t)op.c, &next_first, &next_count)) {
                return EDEN_STEP_YIELD;
            }
            state->scene = (uint16_t)op.c;
            state->op = next_first;
            break;
        }
        case EDEN_OP_END:
            state->ended = true;
            line->kind = EDEN_STEP_END;
            line->scene = state->scene;
            return EDEN_STEP_END;
        default:
            return EDEN_STEP_YIELD;
        }
    }
    line->kind = EDEN_STEP_YIELD;
    line->scene = state->scene;
    return EDEN_STEP_YIELD;
}

void eden_script_advance(eden_state_t *state, const eden_pack_t *pack)
{
    if (state == NULL || pack == NULL || state->ended) {
        return;
    }
    uint32_t first = 0;
    uint32_t count = 0;
    if (!eden_pack_scene_range(pack, state->scene, &first, &count)) {
        state->ended = true;
        return;
    }
    if (state->op + 1 >= first + count) {
        // 停在场景末尾:下一次 step 会落到下一个场景(或剧本结束)。
        state->op = first + count;
        return;
    }
    state->op++;
}

int eden_script_chapter_of(const eden_pack_t *pack, uint16_t scene)
{
    const uint16_t count = eden_pack_chapter_count(pack);
    for (uint16_t index = 0; index < count; index++) {
        eden_chapter_t chapter;
        if (eden_pack_chapter(pack, index, &chapter) && chapter.scene == scene) {
            return (int)index;
        }
    }
    return -1;
}
