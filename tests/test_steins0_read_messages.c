// tests/test_steins0_read_messages.c -- expireRine missing/empty contract.
//
// The source engine uses JS truthiness:
//   if (e.readMessages && ...) { fallback[e.at+':'+e.base] = true; ... }
// A missing readMessages property is falsy and must NOT suppress the default
// message; an empty array is truthy and MUST suppress it without adding any
// fallback message.  pack v1 previously encoded both as b=0/first=0xFFFF.
#include "steins0_engine.h"
#include "steins0_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

#define TOTAL_BYTES 2048
#define EVENT_OFF 640

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void section(uint8_t *blob, uint32_t type, uint32_t off, uint32_t count, uint32_t size)
{
    uint8_t *p = blob + 20 + type * 16;
    wr32(p, type);
    wr32(p + 4, off);
    wr32(p + 8, count);
    wr32(p + 12, size);
}

// Build a minimal valid pack with one node, one expire event, one default
// message event, three rine messages and one rine base.
static void build_pack(uint8_t *blob, uint8_t expire_flags, uint16_t expire_count)
{
    memset(blob, 0, TOTAL_BYTES);
    memcpy(blob, SG0_PACK_MAGIC, 8);
    wr16(blob + 8, SG0_PACK_VERSION);
    wr16(blob + 10, 20);
    wr16(blob + 12, SG0_SEC_COUNT);
    wr32(blob + 16, TOTAL_BYTES);

    const uint32_t node = 600;
    const uint32_t event = EVENT_OFF;
    const uint32_t rmsg = 680;
    const uint32_t rbase = 730;
    const uint32_t date = 740;

    section(blob, SG0_SEC_NODE, node, 1, 24);
    section(blob, SG0_SEC_EVENT, event, 2, 32);
    section(blob, SG0_SEC_RMSG, rmsg, 3, 48);
    section(blob, SG0_SEC_RBASE, rbase, 1, 2);
    section(blob, SG0_SEC_DATE, date, 1, 4);
    for (uint32_t t = 0; t < SG0_SEC_COUNT; ++t) {
        if (t == SG0_SEC_NODE || t == SG0_SEC_EVENT || t == SG0_SEC_RMSG ||
            t == SG0_SEC_RBASE || t == SG0_SEC_DATE) {
            continue;
        }
        section(blob, t, 760, 0, 0);
    }

    // NODE: no chunks, no choice/condition, no ending; two events.
    uint8_t *n = blob + node;
    wr16(n + 0, 0);       // chunk_first
    wr16(n + 2, 0);       // chunk_count
    wr16(n + 4, 0xFFFF);  // choice
    wr16(n + 6, 0xFFFF);  // condition
    n[8] = 0xFF;          // ending = -1
    n[9] = 0;
    wr16(n + 10, 0);      // break_first
    n[12] = 0;            // break_count
    wr16(n + 13, 0);      // chapter_first
    n[15] = 0;            // chapter_count
    wr16(n + 16, 0);      // event_first
    wr16(n + 18, 2);      // event_count
    wr16(n + 20, 0);      // start_first
    n[22] = 0;            // start_count

    // Event 0: expireRine base 1000, one fallback message (ref 1).
    uint8_t *e = blob + event;
    wr16(e + 0, 0);                     // at
    e[2] = SG0_EVENT_EXPIRE_RINE;
    e[3] = expire_flags;
    wr16(e + 4, 0);                     // a
    wr16(e + 6, expire_count);          // b: fallback count
    wr16(e + 8, 1000);                  // c: base
    wr16(e + 10, 0);                    // d
    wr16(e + 12, expire_count ? 1 : 0xFFFF); // data: first fallback ref

    // Event 1: default message for base 1000 (ref 2), suppressed by fallback.
    e += 16;
    wr16(e + 0, 0);
    e[2] = SG0_EVENT_MESSAGE;
    e[3] = 0;
    wr16(e + 4, 0);                     // contact/sender
    wr16(e + 6, 102);                   // id
    wr16(e + 8, 1000);                  // base
    wr16(e + 10, 0);                    // date slot
    wr16(e + 12, 2);                    // rmsg index

    // RMSG: 0 = read inbox message, 1 = fallback, 2 = default.
    for (uint16_t i = 0; i < 3; ++i) {
        uint8_t *m = blob + rmsg + i * 16;
        wr16(m + 0, (uint16_t)(100 + i));
        wr16(m + 2, 1000);
        wr16(m + 4, 0);                 // date slot
        m[8] = 0;                       // incoming
        wr16(m + 10, 0xFFFF);           // text
        wr16(m + 12, 0xFFFF);           // stamp
    }
    wr16(blob + rbase, 1000);
    wr32(blob + date, 111111);
}

static sg0_pack_t *s_pack_ptr;

static int count_ref(const sg0_state_t *state, uint16_t id)
{
    int found = 0;
    for (uint16_t i = 0; i < state->rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(s_pack_ptr, state->rine.refs[i], &msg)) continue;
        if (msg.id == id) ++found;
    }
    return found;
}

static void run_case(const char *name, uint8_t flags, uint16_t count,
                     int want_101, int want_102)
{
    uint8_t blob[TOTAL_BYTES];
    build_pack(blob, flags, count);
    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, blob, sizeof(blob)), "%s: pack open", name);
    s_pack_ptr = &pack;

    sg0_state_t state;
    sg0_engine_init(&state, 0, 0);
    state.node = 0;
    state.cursor = 0;
    state.rine.count = 1;
    state.rine.refs[0] = 0;
    state.rine.read[0] = 1;
    (void)sg0_engine_locate(&pack, &state, NULL);

    const int got101 = count_ref(&state, 101);
    const int got102 = count_ref(&state, 102);
    CHECK(got101 == want_101, "%s: fallback 101 count %d != %d", name, got101, want_101);
    CHECK(got102 == want_102, "%s: default 102 count %d != %d", name, got102, want_102);
}

static void test_unknown_flags(void)
{
    uint8_t blob[TOTAL_BYTES];
    build_pack(blob, SG0_EVENT_FLAG_READ_MESSAGES_PRESENT, 0);
    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, blob, sizeof(blob)), "unknown-flags baseline opens");

    uint8_t copy[TOTAL_BYTES];
    memcpy(copy, blob, sizeof(blob));
    copy[EVENT_OFF + 3] = 4;  // bit2 is not defined for expireRine
    sg0_pack_t bad;
    CHECK(!sg0_pack_open(&bad, copy, sizeof(copy)), "reject unknown expire flag 0x04");

    memcpy(copy, blob, sizeof(blob));
    copy[EVENT_OFF + 16 + 3] = 4;  // message flags only allow bits 0..1
    CHECK(!sg0_pack_open(&bad, copy, sizeof(copy)), "reject unknown message flag 0x04");
}

int main(void)
{
    // Missing readMessages: no fallback mark, default message must arrive.
    run_case("missing", 0, 0, 0, 1);
    // Empty readMessages: fallback mark only, default message suppressed.
    run_case("empty", SG0_EVENT_FLAG_READ_MESSAGES_PRESENT, 0, 0, 0);
    // Old pack (flags=0) with a non-empty list keeps working.
    run_case("legacy-nonempty", 0, 1, 1, 0);
    // New pack marks presence and still adds the fallback entry.
    run_case("new-nonempty", SG0_EVENT_FLAG_READ_MESSAGES_PRESENT, 1, 1, 0);

    test_unknown_flags();

    if (failures) {
        printf("FAILED (%d)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
