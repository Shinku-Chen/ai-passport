// main/gal/gal_tone.h —— 方波合成(纯逻辑,不依赖 ESP-IDF,可宿主机测试)。
//
// 骨架没有音频资源,音乐鉴赏用方波当占位音色。
#pragma once

#include <stdint.h>

// 把前 samples 个采样写成 hz 的方波;hz = 0 表示休止(静音)。
// 块首做约 4ms 的斜坡,避免音符切换时的爆音。
// rate 为采样率(Hz),amplitude 为满幅值(<= 32767)。
void gal_tone_fill(int16_t *buf, int samples, uint32_t rate, uint16_t hz, int amplitude);
