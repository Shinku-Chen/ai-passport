// main/ui_eatwhat_math.c —— 「今天吃啥」播放状态机的纯逻辑实现。
#include "ui_eatwhat_math.h"

int ui_eatwhat_anim_for_btn(uint8_t btn)
{
    // BSP_BTN_UP=0 -> 引导, BSP_BTN_DOWN=1 -> 食物。与 s_anims[] 索引对齐。
    if (btn == 0) return 0;
    if (btn == 1) return 1;
    return -1;
}

uint32_t ui_eatwhat_next_frame(uint32_t cur_frame, uint32_t frames)
{
    if (frames == 0) return 0;
    return (cur_frame + 1) % frames;
}

bool ui_eatwhat_is_released(int mv)
{
    if (mv < 0) return false;             // 读取失败,保守认为仍按住
    return mv >= EATWHAT_RELEASE_MV_THRESHOLD;
}
