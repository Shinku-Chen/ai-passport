// tests/test_tuner_engine.c —— 音高检测引擎的 host 测试。
// 合成标准正弦波,断言检测频率与量化结果的误差在容差内。
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tuner_engine.h"

// M_PI 在严格 C11 + Windows clang 下未定义,这里做一次可移植兜底。
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 16000
#define N  TUNER_WINDOW

// 生成振幅 amp 的正弦波 int16 PCM。
static void gen_tone(int16_t *buf, int n, int sr, double freq, int16_t amp)
{
    for (int i = 0; i < n; i++) {
        double t = 2.0 * M_PI * freq * (double)i / (double)sr;
        buf[i] = (int16_t)lround(amp * sin(t));
    }
}

// 检测单一正弦音,误差须在 rel 相对容差内。
static void expect_pitch(double freq, double rel)
{
    int16_t buf[N];
    gen_tone(buf, N, SR, freq, 8000);
    float got = tuner_detect_pitch(buf, N, SR);
    double err = fabs(got - freq) / freq;
    if (err > rel) {
        fprintf(stderr, "pitch fail: want %.2f Hz got %.2f Hz (err %.2f%%)\n",
                freq, got, err * 100.0);
        exit(1);
    }
}

// 断言 diag 中间量:有信号时 rms>0、nsdf 峰值可信、raw_freq 接近返回频率。
static void expect_diag(double freq)
{
    int16_t buf[N];
    gen_tone(buf, N, SR, freq, 8000);
    tuner_diag_t d;
    float got = tuner_detect_pitch_diag(buf, N, SR, &d);
    if (got <= 0.0f || d.rms <= 0.0f || d.nsdf_peak < TUNER_NSDF_THRESHOLD ||
        d.lag <= 0 || d.raw_freq <= 0.0f) {
        fprintf(stderr, "diag fail: %.2f Hz got %.2f rms %.3f nsdf %.2f lag %d raw %.2f\n",
                freq, got, d.rms, d.nsdf_peak, d.lag, d.raw_freq);
        exit(1);
    }
    double err = fabs(d.raw_freq - freq) / freq;
    if (err > 0.01) {
        fprintf(stderr, "diag raw freq mismatch: want %.2f got %.2f\n", freq, d.raw_freq);
        exit(1);
    }
    // diag 可传 NULL,行为与旧接口一致。
    if (tuner_detect_pitch_diag(buf, N, SR, NULL) != got) {
        fprintf(stderr, "diag NULL fail\n");
        exit(1);
    }
}

// 断言量化结果。
static void expect_note(double freq, int want_idx, int want_oct, double tol_cents)
{
    tuner_note_t n = tuner_quantize((float)freq);
    if (n.note_idx != want_idx || n.octave != want_oct ||
        fabs(n.cents) > tol_cents) {
        fprintf(stderr, "quantize fail: %.2f Hz -> %d oct%d %.1f cents "
                        "(want %d oct%d within %.1f)\n",
                freq, n.note_idx, n.octave, n.cents, want_idx, want_oct,
                tol_cents);
        exit(1);
    }
}

int main(void)
{
    // 1. 正弦音高检测:吉他标准定弦 E2/A2/D3/G3/B3/E4 及 A4 八度。
    expect_pitch(82.41, 0.01);   // E2
    expect_pitch(110.00, 0.01);  // A2
    expect_pitch(146.83, 0.01);  // D3
    expect_pitch(196.00, 0.01);  // G3
    expect_pitch(246.94, 0.01);  // B3
    expect_pitch(329.63, 0.01);  // E4
    expect_pitch(440.00, 0.01);  // A4
    expect_pitch(220.00, 0.01);  // A3
    expect_pitch(880.00, 0.01);  // A5

    // 2. 微弱信号 -> 0。
    {
        int16_t buf[N] = {0};
        assert(tuner_detect_pitch(buf, N, SR) == 0.0f);
        tuner_diag_t d;
        assert(tuner_detect_pitch_diag(buf, N, SR, &d) == 0.0f);
        assert(d.rms == 0.0f);   // 静音时归一 RMS 应为 0
        assert(d.nsdf_peak == 0.0f && d.lag == -1 && d.raw_freq == 0.0f);
    }

    // 2b. diag 中间量:有信号时各字段合理,且与主接口结果一致。
    expect_diag(440.00);
    expect_diag(196.00);
    expect_diag(82.41);

    // 3. 量化:标准音零偏差。
    expect_note(440.00, 9, 4, 0.5);    // A4
    expect_note(220.00, 9, 3, 0.5);    // A3
    expect_note(880.00, 9, 5, 0.5);    // A5
    expect_note(329.63, 4, 4, 0.5);    // E4
    expect_note(82.41, 4, 2, 0.5);     // E2
    expect_note(261.63, 0, 4, 0.5);    // C4
    expect_note(466.16, 10, 4, 0.5);   // A#4

    // 4. 量化:半音边界偏移应给出正确 cents 符号。
    {
        tuner_note_t flat = tuner_quantize(440.0f * powf(2.0f, -25.0f / 1200.0f));
        assert(flat.note_idx == 9 && flat.octave == 4);
        assert(flat.cents < -20.0f && flat.cents > -30.0f);
        tuner_note_t sharp = tuner_quantize(440.0f * powf(2.0f, 25.0f / 1200.0f));
        assert(sharp.note_idx == 9 && sharp.octave == 4);
        assert(sharp.cents > 20.0f && sharp.cents < 30.0f);
    }

    // 5. 越界/非法输入。
    {
        tuner_note_t n = tuner_quantize(0.0f);
        assert(n.note_idx == -1 && n.octave == -1);
        n = tuner_quantize(30.0f);   // 低于范围
        assert(n.note_idx == -1);
        assert(tuner_detect_pitch(NULL, N, SR) == 0.0f);
    }

    printf("test_tuner_engine: PASS\n");
    return 0;
}
