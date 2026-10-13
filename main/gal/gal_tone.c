// main/gal/gal_tone.c —— 方波合成实现。
#include "gal_tone.h"

#include <string.h>

void gal_tone_fill(int16_t *buf, int samples, uint32_t rate, uint16_t hz, int amplitude)
{
    if (!buf || samples <= 0) return;

    const int period = (hz == 0 || rate == 0) ? 0 : (int)(rate / hz);
    const int half = period / 2;
    if (half <= 0 || amplitude <= 0) {
        // 休止,或者频率高到半周期不足一个采样:退化成静音。
        memset(buf, 0, (size_t)samples * sizeof(int16_t));
        return;
    }

    const int ramp = (int)(rate / 250);   // 约 4ms
    for (int i = 0; i < samples; ++i) {
        int32_t v = ((i % period) < half) ? amplitude : -amplitude;
        if (i < ramp) v = v * i / ramp;   // 块首淡入
        buf[i] = (int16_t)v;
    }
}
