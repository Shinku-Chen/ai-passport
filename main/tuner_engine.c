// main/tuner_engine.c —— 音高检测引擎实现。
// 用归一化平方差函数 (NSDF) 检测基频:对麦克风采集的 PCM 窗口求自相关,
// NSDF 峰值对应的滞后即基频周期。NSDF 对幅度归一化,且在半周期处为负,
// 天然抑制高八度误判,适合乐器调音。
//
// 性能说明:ESP32-C3 无 FPU,float 运算走软浮点库,很慢。这里把 NSDF 热循环
// 全部改成 int64 整数累加(乘积 int32 不溢出,累加用 int64),RMS 门限也改成
// 整数比较免 sqrtf。单窗(1024 采样)检测从秒级降到毫秒级,避免 task_wdt 超时。
#include "tuner_engine.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// 信号强度门限(16-bit 采样,以满量程 32767 归一)。低于此值认为无有效信号。
// 整数化后用于 energy < THRESHOLD^2 * n 的等价比较。
#define TUNER_RMS_THRESHOLD 300.0f
#define TUNER_RMS_THRESHOLD_SQ ((int64_t)(TUNER_RMS_THRESHOLD * TUNER_RMS_THRESHOLD))

// 抛物线插值的最大滞后步长,避免插值跨过相邻峰。
#define TUNER_PARABOLA_RADIUS 3.0f

// 静态去直流缓冲上限(产品窗 1024,留余量;宿主测试同 TUNER_WINDOW)。
#define TUNER_ENGINE_MAX_N 1024
// 静态 NSDF 缓冲上限(16 kHz / 60 Hz -> lag_max 约 266)。
#define TUNER_ENGINE_MAX_LAG 512

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
    if (n > TUNER_ENGINE_MAX_N) n = TUNER_ENGINE_MAX_N;

    // 1. 去直流 + RMS 门限。麦克风有 DC 偏置会影响 NSDF,先减均值。
    //    均值用整数(四舍五入),再一次性算出去直流数组 xc[],避免 NSDF 循环里
    //    反复做 pcm[i]-mean 的浮点减法。
    int64_t mean_acc = 0;
    for (int i = 0; i < n; i++) {
        mean_acc += pcm[i];
    }
    int32_t mean = (int32_t)(mean_acc / n);

    static int32_t xc[TUNER_ENGINE_MAX_N];
    int64_t energy = 0;
    for (int i = 0; i < n; i++) {
        int32_t x = (int32_t)pcm[i] - mean;
        xc[i] = x;
        energy += (int64_t)x * x;
    }
    // 整数 RMS 门限:energy < THRESHOLD^2 * n ⇔ rms < THRESHOLD(免 sqrt)。
    if (energy < TUNER_RMS_THRESHOLD_SQ * n) {
        if (diag) {
            diag->rms = sqrtf((float)((double)energy / n)) / 32767.0f;
        }
        return 0.0f;   // 太弱,当作无输入
    }
    if (diag) diag->rms = sqrtf((float)((double)energy / n)) / 32767.0f;

    // 2. 计算 NSDF 序列。
    //    nsdf[lag] = 2*sum(x[i]*x[i+lag]) / sum(x[i]^2 + x[i+lag]^2), 范围 [-1,1]。
    //    lag 范围由目标频率范围决定:lag_min 对应最高频,lag_max 对应最低频。
    int lag_min = (int)((float)sample_rate / TUNER_MAX_FREQ);
    int lag_max = (int)((float)sample_rate / TUNER_MIN_FREQ);
    if (lag_min < 2) lag_min = 2;
    if (lag_max > n / 2) lag_max = n / 2;   // 需要至少半个窗口做相关
    // 静态 NSDF 缓冲(产品窗 16 kHz -> 266 个 float ≈ 1 KB,不占任务栈)。
    static float nsdf[TUNER_ENGINE_MAX_LAG];
    if (lag_max >= TUNER_ENGINE_MAX_LAG) {
        lag_max = TUNER_ENGINE_MAX_LAG - 1;
    }
    if (lag_max <= lag_min) {
        return 0.0f;
    }

    // 单次乘积用 int32(RV32 上 32 位乘是单指令;32767^2≈1.07e9 < 2^31 不溢出),
    // 累加提升到 int64(≈2e12,远不溢出),避免每次做 int64 软乘法。
    int best_lag = -1;
    float best_val = -1.0f;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        int64_t sum = 0, sum_sq = 0;
        const int end = n - lag;
        for (int i = 0; i < end; i++) {
            int32_t a = xc[i];
            int32_t b = xc[i + lag];
            sum += (int64_t)(a * b);
            sum_sq += (int64_t)(a * a + b * b);
        }
        float v = (sum_sq > 0) ? (float)((double)(2 * sum) / (double)sum_sq) : 0.0f;
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
