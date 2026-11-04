// tests/test_steins0_rine.c —— 源 phone/RINE 契约的 C 回归(真实包 + engine)。
//
// 对应 review-runtime/logs/rine-oracle.json 的 R1..R9 与 R6 补接、R9 三分支。
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

static int find_ref(const sg0_state_t *state, uint16_t msg_id_value, bool outgoing)
{
    for (uint16_t i = 0; i < state->rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_pack, state->rine.refs[i], &msg)) continue;
        if (msg.id == msg_id_value && ((msg.flags & 1u) != 0) == outgoing) return i;
    }
    return -1;
}

static int base_bit(uint16_t base)
{
    for (uint8_t i = 0; i < s_pack.sec[SG0_SEC_RBASE].count; ++i) {
        uint16_t value = 0;
        if (sg0_pack_rine_base(&s_pack, i, &value) && value == base) return i;
    }
    return -1;
}

static uint16_t reply_index_for(uint16_t base)
{
    for (uint16_t i = 0; i < s_pack.sec[SG0_SEC_RREPLY].count; ++i) {
        uint16_t v = 0;
        if (sg0_pack_rine_reply(&s_pack, i, &v, NULL, NULL, NULL, NULL) && v == base) return i;
    }
    return 0xFFFFu;
}

static void load_events(sg0_state_t *state, uint8_t node, uint16_t cursor)
{
    sg0_state_t temp;
    sg0_engine_init(&temp, 0, 0);
    temp.node = node;
    temp.cursor = cursor;
    (void)sg0_engine_locate(&s_pack, &temp, NULL);
    *state = temp;
}

static void test_r1_r2_r3(void)
{
    sg0_state_t state;
    sg0_engine_init(&state, 0, 0);
    state.node = 0;
    state.cursor = 243;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    const int ref = find_ref(&state, 2304, false);
    CHECK(ref >= 0, "R1 inbox 2304");
    if (ref >= 0) {
        sg0_rine_msg_t msg;
        CHECK(sg0_pack_rine_msg(&s_pack, state.rine.refs[ref], &msg), "R1 msg");
        CHECK(msg.base == 2304,
              "R1 base");
    }
    const int bit = base_bit(2304);
    CHECK(bit >= 0 && (state.rine.windows & (1u << bit)) != 0, "R2 window 2304");
    const uint16_t ri = reply_index_for(2304);
    CHECK(ri != 0xFFFFu, "R2 reply record");
    uint8_t choices = 0;
    uint16_t first = 0;
    (void)sg0_pack_rine_reply(&s_pack, ri, NULL, NULL, &choices, &first, NULL);
    CHECK(choices == 3, "R2 choices %u", choices);
    uint16_t ids[3] = {0};
    for (uint8_t i = 0; i < 3 && i < choices; ++i) {
        uint16_t selected = 0;
        if (sg0_pack_rine_choice(&s_pack, (uint16_t)(first + i), &selected, NULL, NULL, NULL)) {
            sg0_rine_msg_t msg;
            (void)sg0_pack_rine_msg(&s_pack, selected, &msg);
            ids[i] = msg.id;
        }
    }
    CHECK(ids[0] == 2305 && ids[1] == 2306 && ids[2] == 2307,
          "R2 reply ids %u/%u/%u", ids[0], ids[1], ids[2]);

    state.cursor = 270;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(base_bit(2304) < 0 || (state.rine.windows & (1u << base_bit(2304))) == 0,
          "R3 window removed");
    CHECK(find_ref(&state, 19, false) >= 0, "R3 stamp 19 received");
}

static void test_r4_reset(void)
{
    sg0_state_t state;
    load_events(&state, 0, 243);
    CHECK(state.rine.count > 0, "R4 seed inbox");
    state.node = 9;
    state.cursor = 52;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(state.rine.count == 0 && state.rine.replied == 0 && state.rine.windows == 0,
          "R4 full reset");
    // display reset: keep inbox/read/replied, clear windows.
    sg0_engine_init(&state, 0, 0);
    state.node = 0;
    state.cursor = 243;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    const uint16_t before = state.rine.count;
    state.rine.read[0] = 1;
    state.rine.replied = 1;
    state.node = 0;
    state.cursor = 0;
    state.event_node = 0xFFu;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(state.rine.count == before && state.rine.replied == 1 && state.rine.windows == 0,
          "R4b display reset");
}

static void test_r5_close_choice(void)
{
    sg0_state_t state;
    sg0_engine_init(&state, 0, 0);
    state.node = 0;
    state.cursor = 1507;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(state.rine.pending_valid, "R6 auto defer keeps pending");
    CHECK(state.node == 2 && state.cursor == 0, "R6 defer n2@0 (node %u cursor %u)",
          state.node, state.cursor);
    const uint16_t before = state.rine.count;
    CHECK(sg0_engine_answer_pending(&s_pack, &state, true), "R6 answer");
    CHECK(state.node == 1 && state.cursor == 0, "R6 accept n1@0");
    CHECK(before <= state.rine.count, "R6 inbox preserved");
    sg0_engine_init(&state, 0, 0);
    state.node = 1;
    state.cursor = 0;
    state.rine.pending_valid = 1;
    state.rine.pending_node = 0;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(!state.rine.pending_valid, "R5 closeChoice clears pending");
}

static void test_r7_restore_and_r8(void)
{
    sg0_state_t state;
    load_events(&state, 0, 243);
    CHECK(state.rine.count > 0, "R7 seed");
    sg0_save_t save;
    memset(&save, 0, sizeof(save));
    save.meta.node = state.node;
    save.meta.cursor = state.cursor;
    for (uint16_t i = 0; i < state.rine.count; ++i) {
        sg0_save_rine_set(&save, state.rine.refs[i], state.rine.read[i] != 0);
    }
    save.rine_replied = state.rine.replied;
    save.rine_windows = state.rine.windows;
    save.pending_valid = state.rine.pending_valid;
    uint8_t raw[SG0_SAVE_BYTES];
    CHECK(sg0_save_encode(&save, raw, sizeof(raw)) == SG0_SAVE_BYTES, "R7 encode");
    sg0_save_t back;
    CHECK(sg0_save_decode(&back, raw, sizeof(raw)), "R7 decode");
    CHECK(back.rine_windows == state.rine.windows && back.rine_replied == state.rine.replied,
          "R7 windows/replied");
    uint16_t restored = 0;
    for (uint16_t i = 0; i < state.rine.count; ++i) {
        if (sg0_save_rine_active(&back, state.rine.refs[i])) ++restored;
    }
    CHECK(restored == state.rine.count, "R7 refs restored %u/%u", restored, state.rine.count);

    sg0_engine_init(&state, 0, 0);
    state.node = 0;
    state.cursor = 243;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    const uint16_t ri = reply_index_for(2304);
    const uint16_t before = state.rine.count;
    CHECK(sg0_engine_send_reply(&s_pack, &state, ri, 0), "R8 send 2305");
    CHECK(state.rine.replied & (1u << base_bit(2304)), "R8 replied 2304");
    CHECK(state.rine.count > before, "R8 sequence added");
    CHECK(find_ref(&state, 2305, true) >= 0, "R8 sent 2305");
    CHECK(find_ref(&state, 2309, true) >= 0, "R8 sent 2309");
    CHECK(find_ref(&state, 2311, true) >= 0, "R8 sent 2311");
    // Reply rows inherit the mother mail date, and unread counts incoming only.
    uint16_t mother_date = 0;
    for (uint16_t i = 0; i < state.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_pack, state.rine.refs[i], &msg)) continue;
        if (!(msg.flags & 1u) && msg.base == 2304) {
            mother_date = msg.date;
            break;
        }
    }
    CHECK(mother_date != 0 && sg0_pack_date(&s_pack, mother_date) == 101123,
          "mother date 2304");
    for (uint16_t i = 0; i < state.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_pack, state.rine.refs[i], &msg)) continue;
        if ((msg.flags & 1u) && msg.base == 2304) {
            const uint16_t effective = sg0_engine_rine_date(&s_pack, &state,
                                                             state.rine.refs[i]);
            CHECK(sg0_pack_date(&s_pack, effective) == 101123,
                  "reply date inherits mother at ref %u", state.rine.refs[i]);
        }
    }
    uint16_t incoming_unread = 0;
    for (uint16_t i = 0; i < state.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_pack, state.rine.refs[i], &msg)) continue;
        if (!(msg.flags & 1u) && !state.rine.read[i]) ++incoming_unread;
    }
    CHECK(incoming_unread == 6, "source S_reply incoming unread 6 (got %u)",
          incoming_unread);
    CHECK(sg0_engine_unread(&s_pack, &state) == incoming_unread,
          "unread excludes outgoing (%u/%u)", sg0_engine_unread(&s_pack, &state),
          incoming_unread);
}

static void test_r9_fallback(void)
{
    // read + unreplied -> fallback 15; read + replied -> no fallback;
    // unread + unreplied -> no fallback.
    for (int variant = 0; variant < 3; ++variant) {
        sg0_state_t state;
        sg0_engine_init(&state, 0, 0);
        state.node = 0;
        state.cursor = 841;
        (void)sg0_engine_locate(&s_pack, &state, NULL);
        const int ref = find_ref(&state, 3072, false);
        CHECK(ref >= 0, "R9 seed 3072 variant %d", variant);
        if (ref < 0) continue;
        if (variant != 2) state.rine.read[ref] = 1;
        const int bit = base_bit(3072);
        if (variant == 1) state.rine.replied |= (uint16_t)(1u << bit);
        state.cursor = 842;
        (void)sg0_engine_locate(&s_pack, &state, NULL);
        const bool added = find_ref(&state, 15, false) >= 0;
        CHECK(added == (variant == 0), "R9 variant %d fallback %d", variant, added);
        CHECK(base_bit(3072) < 0 || (state.rine.windows & (1u << bit)) == 0,
              "R9 window removed %d", variant);
    }
}

static void test_endings_normalize(void)
{
    sg0_state_t state;
    sg0_engine_init(&state, 0, 0);
    state.node = 20;
    state.cursor = 65535;
    (void)sg0_engine_locate(&s_pack, &state, NULL);
    CHECK(state.ended && state.ending_id == 5, "n20 ending 5");
    CHECK((state.endings & (1u << 3)) && (state.endings & (1u << 5)), "n20 bits 3/5");
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    if (!fh) return 1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)size);
    CHECK(fread(blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    CHECK(sg0_pack_open(&s_pack, blob, (uint32_t)size), "pack open");
    (void)s_chunk;
    test_r1_r2_r3();
    test_r4_reset();
    test_r5_close_choice();
    test_r7_restore_and_r8();
    test_r9_fallback();
    test_endings_normalize();
    free(blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS RINE oracle R1-R9 + pendingCall + normalizeEndings\n");
    return 0;
}
