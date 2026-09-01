// main/tuner_engine.c —— 音高检测引擎实现。
// 用归一化平方差函数 (NSDF) 检测基频:对麦克风采集的 PCM 窗口求自相关,
// NSDF 峰值对应的滞后即基频周期。NSDF 对幅度归一化,且在半周期处为负,
// 天然抑制高八度误判,适合乐器调音。
#include "tuner_engine.h"

#include <math.h>
#include <string.h>

// 信号强度门限(16-bit 采样,以满量程 32767 归一)。低于此值认为无有效信号。
#define TUNER_RMS_THRESHOLD 300.0f

// 抛物线插值的最大滞后步长,避免插值跨过相邻峰。
#define TUNER_PARABOLA_RADIUS 3.0f

float tuner_detect_pitch(const int16_t *pcm, int n, int sample_rate)
{
    return tuner_detect_pitch_diag(pcm, n, sample_rate, NULL);
}

float tuner_detect_pitch_diag(const int16_t *pcm, int n, int sample_rate,
                              tuner_diag_t *diag)
{
    if (diag) {
        diag->rms = 0.0f;
        diag->nsdf_peak = 0.0f;
        diag->lag = -1;
        diag->raw_freq = 0.0f;
    }
    if (!pcm || n < 64 || sample_rate <= 0) {
        return 0.0f;
    }

    // 1. 去直流 + RMS 门限。麦克风有 DC 偏置会影响 NSDF,先减均值。
    float mean = 0.0f;
    for (int i = 0; i < n; i++) {
        mean += pcm[i];
    }
    mean /= (float)n;

    float energy = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = pcm[i] - mean;
        energy += x * x;
    }
    float rms = sqrtf(energy / (float)n);
    if (diag) diag->rms = rms / 32767.0f;
    if (rms < TUNER_RMS_THRESHOLD) {
        return 0.0f;   // 太弱,当作无输入
    }

    // 2. 计算 NSDF 序列。
    //    nsdf[lag] = 2*sum(x[i]*x[i+lag]) / sum(x[i]^2 + x[i+lag]^2), 范围 [-1,1]。
    //    lag 范围由目标频率范围决定:lag_min 对应最高频,lag_max 对应最低频。
    int lag_min = (int)((float)sample_rate / TUNER_MAX_FREQ);
    int lag_max = (int)((float)sample_rate / TUNER_MIN_FREQ);
    if (lag_min < 2) lag_min = 2;
    if (lag_max > n / 2) lag_max = n / 2;   // 需要至少半个窗口做相关
    // 静态 NSDF 缓冲(产品窗 16 kHz -> 266 个 float ≈ 1 KB,不占任务栈)。
    static float nsdf[512];
    if (lag_max >= (int)(sizeof(nsdf) / sizeof(nsdf[0]))) {
        lag_max = (int)(sizeof(nsdf) / sizeof(nsdf[0])) - 1;
    }
    if (lag_max <= lag_min) {
        return 0.0f;
    }

    int best_lag = -1;
    float best_val = -1.0f;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float sum = 0.0f, sum_sq = 0.0f;
        const int end = n - lag;
        for (int i = 0; i < end; i++) {
            float x = pcm[i] - mean;
            float y = pcm[i + lag] - mean;
            sum += x * y;
            sum_sq += x * x + y * y;
        }
        float v = (sum_sq > 1e-6f) ? (2.0f * sum / sum_sq) : 0.0f;
        nsdf[lag] = v;
        if (v > best_val) {
            best_val = v;
            best_lag = lag;
        }
    }

    // 3. 可信度门限 + 基频选择。
    //    纯正弦在周期整数倍滞后处 NSDF 都接近 1,全局最大可能是 2×/3× 周期 → 低八度误判。
    //    故先取全局最大作参考,再从最小滞后扫描,选第一个接近它且为局部峰的滞后 = 基频。
    if (best_val < TUNER_NSDF_THRESHOLD || best_lag < 0) {
        return 0.0f;
    }
    // 从 lag_min+1 起,保证局部峰判断的两侧邻居都可用。
    // 纯扫描上升沿会把半周期以下的斜坡相关误当基频(如 E2 在 lag=13 处值已
    // 接近 1 但只是 cos 下降坡),故必须同时要求左右都不高于当前值。
    int final_lag = best_lag;
    for (int lag = lag_min + 1; lag <= best_lag; lag++) {
        if (nsdf[lag] < 0.9f * best_val) continue;
        if (nsdf[lag] < nsdf[lag - 1]) continue;   // 下降坡
        if (lag < lag_max && nsdf[lag] < nsdf[lag + 1]) continue;  // 上升坡
        final_lag = lag;
        break;
    }

    // 4. 抛物线插值,把滞后精度提到亚采样。
    float peak_lag = (float)final_lag;
    if (final_lag > lag_min && final_lag < lag_max) {
        float y0 = nsdf[final_lag - 1];
        float y1 = nsdf[final_lag];
        float y2 = nsdf[final_lag + 1];
        float denom = y0 - 2.0f * y1 + y2;
        if (fabsf(denom) > 1e-6f) {
            // 抛物线顶点偏离整数滞后 < 0.5,越界时截断保护。
            float delta = 0.5f * (y0 - y2) / denom;
            if (delta < -TUNER_PARABOLA_RADIUS) delta = -TUNER_PARABOLA_RADIUS;
            if (delta > TUNER_PARABOLA_RADIUS) delta = TUNER_PARABOLA_RADIUS;
            peak_lag += delta;
        }
    }

    if (peak_lag < 1.0f) {
        return 0.0f;
    }

    float freq = (float)sample_rate / peak_lag;
    if (diag) {
        diag->nsdf_peak = nsdf[final_lag];
        diag->lag = final_lag;
        diag->raw_freq = freq;
    }
    return freq;
}

tuner_note_t tuner_quantize(float freq_hz)
{
    tuner_note_t out;
    out.freq_hz = freq_hz;

    if (freq_hz <= 0.0f || freq_hz < TUNER_MIN_FREQ || freq_hz > TUNER_MAX_FREQ) {
        out.note_idx = -1;
        out.octave = -1;
        out.cents = 0.0f;
        out.note_hz = 0.0f;
        return out;
    }

    // MIDI note 号:69 = A4。取整到最近半音。
    float semis = 12.0f * log2f(freq_hz / TUNER_A4_HZ);
    int midi = (int)lroundf(69.0f + semis);

    out.note_idx = ((midi % 12) + 12) % 12;   // 0=C
    out.octave = midi / 12 - 1;               // MIDI 60=C4
    out.note_hz = TUNER_A4_HZ * powf(2.0f, (float)(midi - 69) / 12.0f);
    out.cents = 1200.0f * log2f(freq_hz / out.note_hz);
    return out;
}
