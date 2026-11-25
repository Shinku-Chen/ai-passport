// tests/test_cafe_input.c —— 可控时钟的短按/长按判定。
#include "cafe_input.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main(void)
{
    cafe_input_t in;
    cafe_input_init(&in);

    // 三键各一次短按。
    for (uint8_t k = 0; k < CAFE_KEY_COUNT; ++k) {
        const uint32_t t = 1000 + k * 1000;
        CHECK(cafe_input_press(&in, k, t) == CAFE_IN_NONE);
        CHECK(cafe_input_is_down(&in, k));
        CHECK(cafe_input_tick(&in, t + 100, 500, NULL) == CAFE_IN_NONE);
        CHECK(cafe_input_release(&in, k, t + 200, 500) == CAFE_IN_SHORT);
        CHECK(!cafe_input_is_down(&in, k));
    }

    // 长按：到阈值只发一次 LONG；松手不再发 SHORT。
    CHECK(cafe_input_press(&in, CAFE_KEY_OK, 10000) == CAFE_IN_NONE);
    CHECK(cafe_input_tick(&in, 10499, 500, NULL) == CAFE_IN_NONE);
    uint8_t key = 0xFF;
    CHECK(cafe_input_tick(&in, 10500, 500, &key) == CAFE_IN_LONG);
    CHECK(key == CAFE_KEY_OK);
    CHECK(cafe_input_long_done(&in, CAFE_KEY_OK));
    CHECK(cafe_input_tick(&in, 10600, 500, NULL) == CAFE_IN_NONE);
    CHECK(cafe_input_release(&in, CAFE_KEY_OK, 10700, 500) == CAFE_IN_NONE);

    // 没有 tick 的长按在 release 结算一次 LONG，第二次 release 不再触发。
    CHECK(cafe_input_press(&in, CAFE_KEY_UP, 20000) == CAFE_IN_NONE);
    CHECK(cafe_input_release(&in, CAFE_KEY_UP, 20700, 500) == CAFE_IN_LONG);
    CHECK(cafe_input_release(&in, CAFE_KEY_UP, 20800, 500) == CAFE_IN_NONE);

    // 长按后重新按下仍是新的一次短按。
    CHECK(cafe_input_press(&in, CAFE_KEY_DOWN, 30000) == CAFE_IN_NONE);
    CHECK(cafe_input_release(&in, CAFE_KEY_DOWN, 30100, 500) == CAFE_IN_SHORT);

    // 未按下时 release 不产生事件。
    CHECK(cafe_input_release(&in, CAFE_KEY_OK, 40000, 500) == CAFE_IN_NONE);

    printf("cafe input timing PASS\n");
    return 0;
}
