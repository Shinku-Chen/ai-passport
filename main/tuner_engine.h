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

// 检测过程的中间量,供真机调试观测检测质量(不参与算法决策)。
// rms 以 16-bit 满量程 32767 归一,nsdf_peak 为选中的 NSDF 峰值可信度(0..1)。
typedef struct {
    float rms;        // 输入窗口 RMS 电平(0..1,归一),弱信号时趋近 0
    float nsdf_peak;  // 选中滞后处的 NSDF 值,低于 TUNER_NSDF_THRESHOLD 判为不可靠
    int   lag;        // 选中的滞后(采样数),对应周期
    float raw_freq;   // 抛物线插值后的原始频率(未量化)
} tuner_diag_t;

// 检测基频。返回检测到的频率 Hz;信号太弱或无周期返回 0.0f。
// pcm 为 n 个 16-bit 采样,sample_rate 为采样率。内部使用静态缓冲,非可重入。
// 需要观测中间量时用 tuner_detect_pitch_diag(diag 可为 NULL)。
float tuner_detect_pitch(const int16_t *pcm, int n, int sample_rate);

// 同 tuner_detect_pitch,额外把 RMS/NSDF 峰值/滞后/原始频率写入 diag(可为 NULL)。
// 有信号时 diag.raw_freq == 返回值;无信号时 diag.rms 保留检测前的电平。
float tuner_detect_pitch_diag(const int16_t *pcm, int n, int sample_rate,
                              tuner_diag_t *diag);

// 把频率量化到最近的十二平均律音高。
tuner_note_t tuner_quantize(float freq_hz);
