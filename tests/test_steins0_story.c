// tests/test_steins0_story.c —— 生产解释器遍历真实包的路线/分支/结局/入口。
#include "steins0_engine.h"
#include "steins0_pack.h"
#include "steins0_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static sg0_pack_t s_pack;
static uint8_t s_chunk[16384];
static uint8_t s_seen[41][2];
static uint8_t s_seen_union[41];
static uint8_t s_endings[6];
static uint32_t s_frames[41];
static uint32_t s_frame_total;
static uint32_t s_choice_checked;
static uint32_t s_text_bytes;
static uint32_t s_chunk_marks[1567];
static uint32_t s_chunk_last;
static uint32_t s_unique_chunks;
static int s_depth;

static void mark_chunk(uint32_t zoff)
{
    s_chunk_last = zoff;
    for (uint32_t i = 0; i < s_unique_chunks; ++i) {
        if (s_chunk_marks[i] == zoff + 1u) return;
    }
    if (s_unique_chunks < 1567) s_chunk_marks[s_unique_chunks++] = zoff + 1u;
}

static void walk(sg0_state_t state)
{
    if (++s_depth > 80) {
        CHECK(0, "recursion too deep");
        --s_depth;
        return;
    }
    uint8_t current = 0xFFu;
    uint8_t marked = 0;
    for (int guard = 0; guard < 8000; ++guard) {
        if (state.node != current) {
            current = state.node;
            marked = 0;
        }
        if (!marked) {
            if (s_seen[current][state.endings & 1u]) break;
            s_seen[current][state.endings & 1u] = 1;
            marked = 1;
        }
        sg0_choice_t choice;
        const sg0_place_t place = sg0_engine_locate(&s_pack, &state, &choice);
        if (place == SG0_PLACE_ERROR) {
            CHECK(0, "locate error at node %u cursor %u", state.node, state.cursor);
            break;
        }
        if (place == SG0_PLACE_END) {
            CHECK(state.ending_id < 6, "ending id %u", state.ending_id);
            s_endings[state.ending_id] = 1;
            break;
        }
        if (place == SG0_PLACE_CHOICE) {
            CHECK(choice.count == 2, "choice count");
            CHECK(choice.to[0] < 41 && choice.to[1] < 41, "choice target");
            sg0_node_t node;
            CHECK(sg0_pack_node(&s_pack, state.node, &node), "node");
            // 存档点:选项处保存 node+cursor,恢复后仍在同一选项。
            sg0_save_t save;
            sg0_save_init(&save, state.node, state.cursor, state.mode);
            sg0_state_t restored = state;
            CHECK(sg0_engine_locate(&s_pack, &restored, &choice) == SG0_PLACE_CHOICE,
                  "restore choice");
            CHECK(save.meta.node == state.node && save.meta.cursor == state.cursor, "save identity");
            for (uint8_t i = 0; i < 2; ++i) {
                sg0_state_t branch = state;
                CHECK(sg0_engine_choose(&s_pack, &branch, i), "choose %u", i);
                CHECK(branch.node == choice.to[i], "choice target applied");
                CHECK(branch.cursor == 0, "choice resets cursor");
                walk(branch);
            }
            ++s_choice_checked;
            break;
        }
        // text
        const uint8_t node_index = state.node;
        sg0_chunk_t chunk;
        uint32_t raw_len = 0;
        sg0_frame_t frame;
        const char *text = NULL;
        uint16_t text_len = 0;
        CHECK(sg0_engine_read(&s_pack, &state, s_chunk, sizeof(s_chunk), &chunk, &raw_len,
                              &frame, &text, &text_len), "read node %u cursor %u",
              state.node, state.cursor);
        CHECK(text && text_len > 0, "text empty node %u cursor %u", state.node, state.cursor);
        CHECK(sg0_pack_chunk_load(&s_pack, &chunk, s_chunk, sizeof(s_chunk), &raw_len),
              "chunk inflate");
        CHECK(text_len == frame.text_len, "text len");
        s_text_bytes += text_len;
        ++s_frames[node_index];
        ++s_frame_total;
        mark_chunk(chunk.zoff);
        if (!sg0_engine_next(&s_pack, &state)) {
            // next 失败只可能发生在最终帧之后;下一轮 locate 会给出 choice/end。
            continue;
        }
    }
    --s_depth;
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)size);
    CHECK(fread(blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    CHECK(sg0_pack_open(&s_pack, blob, (uint32_t)size), "pack open");

    for (uint8_t seed = 0; seed < 2; ++seed) {
        memset(s_seen, 0, sizeof(s_seen));
        sg0_state_t state;
        sg0_engine_init(&state, seed ? 1 : 0, 0);
        s_frame_total = 0;
        walk(state);
        uint32_t nodes_seen = 0;
        for (int i = 0; i < 41; ++i) {
            if (s_seen[i][seed]) {
                ++nodes_seen;
                s_seen_union[i] = 1;
            }
        }
        printf("story seed %u: nodes %u/41, frames %u, choices %u\n", seed, nodes_seen,
               s_frame_total, s_choice_checked);
        CHECK(nodes_seen >= 20, "seed nodes %u", nodes_seen);
    }
    // 非关键来电由手机接听;单独走 accept 分支,覆盖 source 的两个目标。
    uint32_t accept_nodes = 0;
    for (int node_index = 0; node_index < 41; ++node_index) {
        sg0_node_t node;
        if (!sg0_pack_node(&s_pack, (uint8_t)node_index, &node)) continue;
        if (node.choice == SG0_NONE) continue;
        sg0_choice_t choice;
        if (!sg0_pack_choice(&s_pack, node.choice, &choice) || choice.critical) continue;
        uint32_t total = 0;
        for (uint8_t i = 0; i < node.chunk_count; ++i) {
            sg0_chunk_t chunk;
            if (!sg0_pack_chunk_at(&s_pack, (uint16_t)(node.chunk_first + i), &chunk)) break;
            total += chunk.frames;
        }
        sg0_state_t st;
        sg0_engine_init(&st, 0, 0);
        st.node = (uint8_t)node_index;
        st.cursor = (uint16_t)total;
        if (sg0_engine_locate(&s_pack, &st, &choice) == SG0_PLACE_TEXT &&
            st.rine.pending_valid && sg0_engine_answer_pending(&s_pack, &st, true)) {
            walk(st);
            ++accept_nodes;
        }
    }
    fprintf(stderr, "accept_nodes=%u", accept_nodes);
    CHECK(accept_nodes >= 10, "pending accept branches %u", accept_nodes);

    // 条件跳转:bit0 决定 n8 -> n13/n14;两条分支都必须可达。
    {
        sg0_state_t state;
        sg0_engine_init(&state, 0, 0);
        state.node = 8;
        state.cursor = 65535;
        sg0_choice_t choice;
        state.node = 8;
        state.cursor = 65535;
        (void)sg0_engine_locate(&s_pack, &state, &choice);
        const uint8_t without = state.node;
        sg0_engine_init(&state, 1, 0);
        state.node = 8;
        state.cursor = 65535;
        (void)sg0_engine_locate(&s_pack, &state, &choice);
        const uint8_t with = state.node;
        CHECK(without == 14 && with == 13, "n8 condition targets %u/%u", without, with);
    }
    // 章节入口:每个 start 都能进入并读到正文。
    uint32_t starts = 0;
    for (uint16_t i = 0; i < s_pack.sec[SG0_SEC_START].count; ++i) {
        sg0_start_t start;
        CHECK(sg0_pack_start(&s_pack, i, &start), "start %u", i);
        sg0_state_t state;
        sg0_engine_init(&state, 0, 0);
        sg0_engine_enter_chapter(&state, &start);
        CHECK(state.mode == 1 && state.node == start.node && state.cursor == start.index,
              "enter chapter");
        sg0_chunk_t chunk;
        uint32_t raw_len = 0;
        sg0_frame_t frame;
        const char *text = NULL;
        uint16_t text_len = 0;
        CHECK(sg0_engine_read(&s_pack, &state, s_chunk, sizeof(s_chunk), &chunk, &raw_len,
                              &frame, &text, &text_len), "chapter start read %u", i);
        CHECK(text_len > 0, "chapter start text");
        ++starts;
    }
    // 跳章:从 n0 起点连续跳,必须前进且不越界。
    {
        sg0_state_t state;
        sg0_engine_init(&state, 0, 0);
        uint16_t last = state.cursor;
        uint8_t last_node = state.node;
        int jumps = 0;
        while (jumps < 60) {
            if (sg0_engine_locate(&s_pack, &state, NULL) != SG0_PLACE_TEXT) break;
            if (!sg0_engine_next_chapter(&s_pack, &state)) break;
            if (state.node == last_node && state.cursor <= last) break;
            last = state.cursor;
            last_node = state.node;
            ++jumps;
        }
        CHECK(jumps > 0, "next_chapter made progress");
        printf("chapter skip: %d jumps, final node %u cursor %u\n", jumps, state.node,
               state.cursor);
    }

    uint32_t union_nodes = 0;
    for (int i = 0; i < 41; ++i) if (s_seen_union[i]) ++union_nodes;
    for (int i = 0; i < 41; ++i) {
        if (s_seen[i][0] || s_seen[i][1]) s_seen_union[i] = 1;
    }
    union_nodes = 0;
    for (int i = 0; i < 41; ++i) if (s_seen_union[i]) ++union_nodes;
    CHECK(union_nodes == 41, "union reachable nodes %u/41", union_nodes);
    uint32_t reached_endings = 0;
    for (int i = 0; i < 6; ++i) if (s_endings[i]) ++reached_endings;
    CHECK(reached_endings == 6, "endings reached %u/6", reached_endings);
    CHECK(s_choice_checked >= 20, "choices checked %u", s_choice_checked);
    CHECK(starts == 104, "starts %u", starts);
    CHECK(s_unique_chunks >= 800, "unique chunk streams touched %u", s_unique_chunks);
    printf("story: frames %u, text %u bytes, choices %u, endings %u/6, starts %u, "
           "chunks %u\n", s_frame_total, s_text_bytes, s_choice_checked, reached_endings,
           starts, s_unique_chunks);
    free(blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS story traversal/routes/endings/chapters\n");
    return 0;
}
