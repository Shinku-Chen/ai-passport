// tests/test_gal_tone.c —— 方波合成的纯逻辑测试(宿主运行,不需要设备)。
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gal/gal_tone.h"

int main(void)
{
    int16_t buf[256];

    // 休止:全零,且不会留下上一次的数据。
    memset(buf, 0x7F, sizeof(buf));
    gal_tone_fill(buf, 256, 16000, 0, 6000);
    for (int i = 0; i < 256; ++i) assert(buf[i] == 0);

    // 500Hz @16kHz:周期 32 采样、半周期 16;斜坡约 4ms(64 采样)之后是干净的方波。
    gal_tone_fill(buf, 256, 16000, 500, 6000);
    for (int i = 100; i < 256; ++i) {
        const bool high = ((i % 32) < 16);
        assert(buf[i] == (high ? 6000 : -6000));
    }
    // 一个完整周期里正负各一半。
    int positive = 0;
    for (int i = 128; i < 160; ++i) {
        if (buf[i] > 0) positive++;
    }
    assert(positive == 16);

    // 斜坡:块首从 0 起往上走,不会一步跳到满幅(避免爆音)。
    gal_tone_fill(buf, 64, 16000, 500, 6000);
    assert(buf[0] == 0);
    assert(buf[1] > 0 && buf[1] < 6000);
    for (int i = 0; i < 64; ++i) {
        assert(buf[i] >= -6000 && buf[i] <= 6000);
    }
    // 斜坡只在块首:这 64 个采样都还在 4ms 斜坡内,幅度未到满幅。
    assert(buf[63] > -6000 && buf[63] < 6000);

    // 频率高到半周期不足一个采样:退化成静音而不是除零/噪声。
    gal_tone_fill(buf, 32, 16000, 32000, 6000);
    for (int i = 0; i < 32; ++i) assert(buf[i] == 0);

    // 非法参数不写越界、不崩。
    gal_tone_fill(NULL, 8, 16000, 440, 6000);
    gal_tone_fill(buf, 0, 16000, 440, 6000);

    puts("gal_tone square wave: PASS");
    return 0;
}
