// tests/test_ui_eatwhat_math.c —— 「今天吃啥」抽签/播放纯逻辑的宿主测试。
#include <assert.h>
#include <stdint.h>
#include "ui_eatwhat_math.h"

int main(void)
{
    // 抽类型:两种类型等概率,按 rng 取模。
    assert(ui_eatwhat_pick_type(0) == 0);
    assert(ui_eatwhat_pick_type(1) == 1);
    assert(ui_eatwhat_pick_type(2) == 0);
    assert(ui_eatwhat_pick_type(3) == 1);
    assert(ui_eatwhat_pick_type(0xFFFFFFFFu) == 1);
    // 任意 rng 都必须落在 [0, EATWHAT_TYPE_COUNT) 内(索引 s_anims[] 的合法范围)。
    for (uint32_t i = 0; i < 1000; i++) {
        int t = ui_eatwhat_pick_type(i * 2654435761u);
        assert(t >= 0 && t < EATWHAT_TYPE_COUNT);
    }

    // 抽起始帧:落在 [0, frames) 内;frames==0 时保护为 0。
    assert(ui_eatwhat_start_frame(0, 21) == 0);
    assert(ui_eatwhat_start_frame(5, 21) == 5);
    assert(ui_eatwhat_start_frame(21, 21) == 0);
    assert(ui_eatwhat_start_frame(100, 20) == 0);
    assert(ui_eatwhat_start_frame(12345, 0) == 0);
    for (uint32_t i = 0; i < 1000; i++) {
        assert(ui_eatwhat_start_frame(i * 2654435761u, 20) < 20);
        assert(ui_eatwhat_start_frame(i * 2654435761u, 21) < 21);
    }

    // 帧推进回绕。
    assert(ui_eatwhat_next_frame(0, 5) == 1);
    assert(ui_eatwhat_next_frame(4, 5) == 0);   // 回绕到首帧
    assert(ui_eatwhat_next_frame(0, 0) == 0);   // 空序列保护
    assert(ui_eatwhat_next_frame(19, 20) == 0);
    assert(ui_eatwhat_next_frame(20, 21) == 0);

    // 松开判定。
    assert(ui_eatwhat_is_released(3300) == true);   // 松开态(约 3.3V)
    assert(ui_eatwhat_is_released(300) == false);    // 按住 UP(约 0V)
    assert(ui_eatwhat_is_released(1500) == false);   // 按住某键(<2000)
    assert(ui_eatwhat_is_released(2500) == true);    // 高于阈值
    assert(ui_eatwhat_is_released(-1) == false);     // 读失败,保守仍按住

    return 0;
}
