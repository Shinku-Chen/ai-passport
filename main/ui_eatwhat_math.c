// main/ui_eatwhat_math.c —— 「今天吃啥」抽签/播放状态机的纯逻辑实现。
#include "ui_eatwhat_math.h"

int ui_eatwhat_pick_type(uint32_t rng)
{
    // 两种类型等概率。使用取模而非取位,便于宿主测试用固定 rng 断言。
    return (int)(rng % EATWHAT_TYPE_COUNT);
}

uint32_t ui_eatwhat_start_frame(uint32_t rng, uint32_t frames)
{
    if (frames == 0) return 0;            // 空序列:不做除法,直接给 0
    return rng % frames;
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
