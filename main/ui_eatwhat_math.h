// main/ui_eatwhat_math.h —— 「今天吃啥」的抽签/播放纯逻辑,与 LVGL/ESP-IDF 隔离,
// 便于使用宿主测试覆盖(见 tests/test_ui_eatwhat_math.c)。
//
// 交互模型:
//   按下任意键 → 抽签(随机选一种类型)→ 从该类型的随机一帧开始滚动
//   松开       → 停在最后一次显示的帧上(即结果)
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 松开阈值:按住某键时 ADC 约 <2000mV,松开约 3300mV。与 demo_eat_what.c 一致。
#define EATWHAT_RELEASE_MV_THRESHOLD 2000

// 抽签类型数量:0=去处盘(餐馆/店家),1=菜品盘(具体吃什么)。与 demo_eat_what.c 的
// s_anims[] 索引一致。新增类型时同时补齐 s_anims[] 与 README 的类型说明。
#define EATWHAT_TYPE_COUNT 2

// 从随机数 rng 抽一种类型(两种类型等概率)。rng 由调用方从硬件 RNG 取;
// 这里只取模,不做额外混合,便于宿主测试用固定输入复现结果。
int ui_eatwhat_pick_type(uint32_t rng);

// 从随机数 rng 抽该类型的起始帧(0..frames-1),让每次按下都从不同画面开始,
// 避免「每次都从第一张滚起」的规律感。frames==0 时返回 0(空序列保护)。
uint32_t ui_eatwhat_start_frame(uint32_t rng, uint32_t frames);

// 把当前帧推进一帧(自动回绕到 0)。
//   速度由调用方的定时器间隔控制;这里只算下一帧号。
uint32_t ui_eatwhat_next_frame(uint32_t cur_frame, uint32_t frames);

// 判定一个按钮电压值(mV)是否表示「已松开」。
//   松开态约 3300mV 明显高于阈值;按住 <阈值。mv<0(读失败)按未松开处理。
bool ui_eatwhat_is_released(int mv);
