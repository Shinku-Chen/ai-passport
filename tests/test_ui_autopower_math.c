// tests/test_ui_autopower_math.c —— 「无操作自动深睡」判定的宿主测试。
#include <assert.h>
#include "ui_autopower_math.h"

int main(void)
{
    // 阈值 = 2 分钟 = 120000 ms
    assert(AUTOPOWER_IDLE_TIMEOUT_MS == 120000);

    // 未超时
    assert(ui_autopower_idle_expired(0) == false);
    assert(ui_autopower_idle_expired(119000) == false);
    // 临界
    assert(ui_autopower_idle_expired(120000) == true);
    // 已超时
    assert(ui_autopower_idle_expired(120001) == true);
    assert(ui_autopower_idle_expired(300000) == true);

    // 计时异常(负值)保守判定不关机
    assert(ui_autopower_idle_expired(-1) == false);
    assert(ui_autopower_idle_expired(-5000) == false);

    // clamp 归一化
    assert(ui_autopower_clamp_elapsed(-5) == 0);
    assert(ui_autopower_clamp_elapsed(0) == 0);
    assert(ui_autopower_clamp_elapsed(200000) == 200000);

    return 0;
}
