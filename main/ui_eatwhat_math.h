// main/ui_eatwhat_math.h —— 「今天吃啥」的播放状态机纯逻辑,与 LVGL/ESP-IDF 隔离,
// 便于使用宿主测试覆盖(见 tests/test_ui_eatwhat_math.c)。
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 松开阈值:按住某键时 ADC 约 <2000mV,松开约 3300mV。与 demo_eat_what.c 一致。
#define EATWHAT_RELEASE_MV_THRESHOLD 2000

// 按键播放入口:BSP_BTN_UP=0(引导),BSP_BTN_DOWN=1(食物)。
// 返回「该按键对应的素材序列索引」;非上/下键返回 -1。
int ui_eatwhat_anim_for_btn(uint8_t btn);

// 把当前帧推进一帧(自动回绕到 0)。
//   速度由调用方的定时器间隔控制;这里只算下一帧号。
uint32_t ui_eatwhat_next_frame(uint32_t cur_frame, uint32_t frames);

// 判定一个按钮电压值(mV)是否表示「已松开」。
//   松开态约 3300mV 明显高于阈值;按住 <阈值。mv<0(读失败)按未松开处理。
bool ui_eatwhat_is_released(int mv);
