// tests/test_c4_settings.c —— 标题屏菜单逻辑的宿主测试。
#include "c4_settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults(void)
{
    c4_settings_t s;
    memset(&s, 0, sizeof(s));
    c4_settings_init(&s);

    assert(s.mode == C4_MODE_HUMAN_FIRST);
    assert(s.level == C4_LEVEL_MEDIUM);
    assert(s.preview == C4_PREVIEW_TOP);
    assert(s.row == C4_MENU_ROW_MODE);

    for (int row = 0; row < C4_MENU_ROW_COUNT; row++) {
        assert(c4_settings_row_enabled(&s, row));
        assert(strlen(c4_settings_row_label(row)) > 0);
        assert(strlen(c4_settings_row_value(&s, row)) > 0);
    }
    assert(!c4_settings_is_start(&s));

    // 边界行号不崩。
    assert(!c4_settings_row_enabled(&s, -1));
    assert(!c4_settings_row_enabled(&s, C4_MENU_ROW_COUNT));
    assert(strcmp(c4_settings_row_label(-1), "") == 0);
    assert(strcmp(c4_settings_row_value(&s, 99), "") == 0);
}

static void test_move_wraps(void)
{
    c4_settings_t s;
    c4_settings_init(&s);

    c4_settings_move(&s, 1);
    assert(s.row == C4_MENU_ROW_LEVEL);
    c4_settings_move(&s, 1);
    assert(s.row == C4_MENU_ROW_PREVIEW);
    c4_settings_move(&s, 1);
    assert(s.row == C4_MENU_ROW_START);
    assert(c4_settings_is_start(&s));

    c4_settings_move(&s, 1);                     // 环形回到第一行
    assert(s.row == C4_MENU_ROW_MODE);
    c4_settings_move(&s, -1);
    assert(s.row == C4_MENU_ROW_START);

    c4_settings_move(&s, 0);                     // 非法 step 不动
    assert(s.row == C4_MENU_ROW_START);
}

static void test_cycle_values(void)
{
    c4_settings_t s;
    c4_settings_init(&s);

    // MODE: 人先手 -> 电脑先手 -> 双人 -> 人先手
    c4_settings_cycle(&s);
    assert(s.mode == C4_MODE_AI_FIRST);
    c4_settings_cycle(&s);
    assert(s.mode == C4_MODE_TWO_PLAYER);
    c4_settings_cycle(&s);
    assert(s.mode == C4_MODE_HUMAN_FIRST);

    // LEVEL: 中 -> 高 -> 低 -> 中
    s.row = C4_MENU_ROW_LEVEL;
    c4_settings_cycle(&s);
    assert(s.level == C4_LEVEL_HARD);
    c4_settings_cycle(&s);
    assert(s.level == C4_LEVEL_EASY);
    c4_settings_cycle(&s);
    assert(s.level == C4_LEVEL_MEDIUM);

    // PREVIEW: 默认顶部 -> 落点 -> 顶部
    s.row = C4_MENU_ROW_PREVIEW;
    assert(s.preview == C4_PREVIEW_TOP);
    c4_settings_cycle(&s);
    assert(s.preview == C4_PREVIEW_LANDING);
    c4_settings_cycle(&s);
    assert(s.preview == C4_PREVIEW_TOP);

    // START 行切换取值不改变任何东西。
    s.row = C4_MENU_ROW_START;
    const c4_settings_t before = s;
    c4_settings_cycle(&s);
    assert(memcmp(&before, &s, sizeof(s)) == 0);
}

static void test_two_player_skips_level(void)
{
    c4_settings_t s;
    c4_settings_init(&s);

    // 电脑先手模式仍需要难度,不能跟着双人模式一起被禁用。
    s.mode = C4_MODE_AI_FIRST;
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_LEVEL));

    s.mode = C4_MODE_TWO_PLAYER;

    assert(!c4_settings_row_enabled(&s, C4_MENU_ROW_LEVEL));
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_MODE));
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_PREVIEW));
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_START));
    assert(strcmp(c4_settings_row_value(&s, C4_MENU_ROW_LEVEL), "-") == 0);

    // 联机模式同样没有难度可言。
    s.mode = C4_MODE_LINK;
    assert(!c4_settings_row_enabled(&s, C4_MENU_ROW_LEVEL));
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_MODE));
    assert(c4_settings_row_enabled(&s, C4_MENU_ROW_START));
    assert(strcmp(c4_settings_row_value(&s, C4_MENU_ROW_LEVEL), "-") == 0);
    assert(strcmp(c4_settings_row_value(&s, C4_MENU_ROW_MODE), "LINK PLAY") == 0);

    s.mode = C4_MODE_TWO_PLAYER;
    // 从 MODE 往下走应直接跳到 PREVIEW。
    s.row = C4_MENU_ROW_MODE;
    c4_settings_move(&s, 1);
    assert(s.row == C4_MENU_ROW_PREVIEW);

    // 从 PREVIEW 往上走也应跳过 LEVEL。
    c4_settings_move(&s, -1);
    assert(s.row == C4_MENU_ROW_MODE);

    // 从 START 往上:LEVEL 被跳过,落到 PREVIEW。
    s.row = C4_MENU_ROW_START;
    c4_settings_move(&s, -1);
    assert(s.row == C4_MENU_ROW_PREVIEW);

    // 即使有人把光标硬塞在 LEVEL 上,移动一步也能自己走出去。
    s.row = C4_MENU_ROW_LEVEL;
    c4_settings_move(&s, 1);
    assert(s.row == C4_MENU_ROW_PREVIEW);
}

static void test_rows_are_all_reachable(void)
{
    // 从任意一条取值出发,上下各走一圈都能回到自己,且不会卡死。
    const uint8_t modes[] = { C4_MODE_HUMAN_FIRST, C4_MODE_AI_FIRST, C4_MODE_TWO_PLAYER,
                              C4_MODE_LINK };

    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        c4_settings_t s;
        c4_settings_init(&s);
        s.mode = modes[m];

        for (int start = 0; start < C4_MENU_ROW_COUNT; start++) {
            if (!c4_settings_row_enabled(&s, start)) continue;

            s.row = (uint8_t)start;
            for (int i = 0; i < 8; i++) c4_settings_move(&s, 1);
            for (int i = 0; i < 8; i++) c4_settings_move(&s, -1);
            assert(s.row == start);
        }
    }
}

static void test_null_safety(void)
{
    c4_settings_init(NULL);
    c4_settings_move(NULL, 1);
    c4_settings_cycle(NULL);
    assert(!c4_settings_is_start(NULL));
    assert(!c4_settings_row_enabled(NULL, 0));
    assert(strcmp(c4_settings_row_value(NULL, 0), "") == 0);
}

int main(void)
{
    test_defaults();
    test_move_wraps();
    test_cycle_values();
    test_two_player_skips_level();
    test_rows_are_all_reachable();
    test_null_safety();

    printf("test_c4_settings: PASS\n");
    return 0;
}
