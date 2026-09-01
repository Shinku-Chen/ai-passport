// main/tuner_engine.h —— 纯 C 音高检测引擎,不依赖 ESP-IDF/LVGL,可 host 测试。
// 算法:归一化平方差函数 (NSDF, McLeod Pitch Method) + RMS 门限 + 抛物线插值。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// A4 基准频率(十二平均律)。
#define TUNER_A4_HZ 440.0f

// 检测的频率范围(Hz)。范围外返回 0。
#define TUNER_MIN_FREQ 60.0f
#define TUNER_MAX_FREQ 1200.0f

// 归一化自相关可信度阈值。NSDF 峰值低于此值视为不可靠(太弱/非周期)。
#define TUNER_NSDF_THRESHOLD 0.50f

// 单个检测窗口的采样数(产品用 16 kHz × 1024 = 64 ms)。
#define TUNER_WINDOW 1024

typedef struct {
    int   note_idx;  // 0=C, 1=C#, ..., 11=B
    int   octave;    // 科学音高记号,如 A4 -> octave=4
    float cents;     // 相对最近音高的偏差,-50..+50;负=偏低
    float freq_hz;   // 输入频率
    float note_hz;   // 最近音高的标准频率
} tuner_note_t;

// 检测基频。返回检测到的频率 Hz;信号太弱或无周期返回 0.0f。
// pcm 为 n 个 16-bit 采样,sample_rate 为采样率。内部使用静态缓冲,非可重入。
float tuner_detect_pitch(const int16_t *pcm, int n, int sample_rate);

// 把频率量化到最近的十二平均律音高。
tuner_note_t tuner_quantize(float freq_hz);
