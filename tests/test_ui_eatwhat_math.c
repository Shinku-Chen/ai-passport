// tests/test_ui_eatwhat_math.c —— 「今天吃啥」状态机纯逻辑的宿主测试。
#include <assert.h>
#include "ui_eatwhat_math.h"

int main(void)
{
    // 按键 -> 素材序列:UP=引导(0), DOWN=食物(1), 其它=无。
    assert(ui_eatwhat_anim_for_btn(0) == 0);
    assert(ui_eatwhat_anim_for_btn(1) == 1);
    assert(ui_eatwhat_anim_for_btn(2) == -1);

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
