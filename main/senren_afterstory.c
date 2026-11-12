#include "senren_afterstory.h"

#include <string.h>

static const senren_afterstory_t s_stories[SENREN_AFTERSTORY_COUNT] = {
    { "芳乃", "gameend_YoshinoEnd", 37, 36, 553, 2 },
    { "茉子", "gameend_MakoEnd", 56, 55, 57, 2 },
    { "丛雨", "gameend_MurasameEnd", 77, 76, 498, 2 },
    { "蕾娜", "gameend_RenaEnd", 97, 96, 321, 2 },
    { "小春", "gameend_KoharuEnd", 107, 106, 728, 1 },
    { "芦花", "gameend_RokaEnd", 111, 110, 789, 1 },
};

int senren_afterstory_count(void)
{
    return SENREN_AFTERSTORY_COUNT;
}

const senren_afterstory_t *senren_afterstory_get(int index)
{
    if (index < 0 || index >= SENREN_AFTERSTORY_COUNT) return NULL;
    return &s_stories[index];
}

int senren_afterstory_index_for_ending(const char *ending)
{
    if (!ending || !ending[0]) return -1;
    for (int i = 0; i < SENREN_AFTERSTORY_COUNT; ++i) {
        if (strcmp(ending, s_stories[i].ending) == 0) return i;
    }
    return -1;
}

bool senren_afterstory_unlocked(uint8_t clears, int index)
{
    if (index < 0 || index >= SENREN_AFTERSTORY_COUNT) return false;
    return (clears & (uint8_t)(1u << index)) != 0;
}

uint8_t senren_afterstory_set_clear(uint8_t clears, int index)
{
    if (index < 0 || index >= SENREN_AFTERSTORY_COUNT) return clears;
    return (uint8_t)(clears | (uint8_t)(1u << index));
}

