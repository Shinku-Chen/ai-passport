#include "senren_afterstory.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static const struct {
    const char *name;
    const char *ending;
    uint16_t scn;
    uint32_t from;
    uint8_t selects;
} expected[6] = {
    { "芳乃", "gameend_YoshinoEnd", 37, 553, 2 },
    { "茉子", "gameend_MakoEnd", 56, 57, 2 },
    { "丛雨", "gameend_MurasameEnd", 77, 498, 2 },
    { "蕾娜", "gameend_RenaEnd", 97, 321, 2 },
    { "小春", "gameend_KoharuEnd", 107, 728, 1 },
    { "芦花", "gameend_RokaEnd", 111, 789, 1 },
};

int main(void)
{
    CHECK(senren_afterstory_count() == 6);
    for (int i = 0; i < 6; ++i) {
        const senren_afterstory_t *s = senren_afterstory_get(i);
        CHECK(s != NULL);
        if (!s) continue;
        CHECK(strcmp(s->name, expected[i].name) == 0);
        CHECK(strcmp(s->ending, expected[i].ending) == 0);
        CHECK(s->source_scn == expected[i].scn);
        CHECK(s->chunk + 1 == s->source_scn);
        CHECK(s->node == expected[i].from);
        CHECK(s->selects == expected[i].selects);
        CHECK(senren_afterstory_index_for_ending(s->ending) == i);
    }
    CHECK(senren_afterstory_get(-1) == NULL);
    CHECK(senren_afterstory_get(6) == NULL);
    CHECK(senren_afterstory_index_for_ending("gameend_end") == -1);
    CHECK(senren_afterstory_index_for_ending("gameend_END") == -1);
    CHECK(senren_afterstory_index_for_ending(NULL) == -1);

    uint8_t clears = 0;
    for (int i = 0; i < 6; ++i) {
        CHECK(!senren_afterstory_unlocked(clears, i));
    }
    clears = senren_afterstory_set_clear(clears, 0);
    clears = senren_afterstory_set_clear(clears, 5);
    CHECK(clears == 0x21);
    CHECK(senren_afterstory_unlocked(clears, 0));
    CHECK(!senren_afterstory_unlocked(clears, 1));
    CHECK(senren_afterstory_unlocked(clears, 5));
    CHECK(senren_afterstory_set_clear(clears, -1) == clears);
    CHECK(senren_afterstory_set_clear(clears, 6) == clears);

    printf("senren_afterstory: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

