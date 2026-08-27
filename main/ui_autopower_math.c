// main/ui_autopower_math.c —— 「无操作自动深睡」判定逻辑实现。
#include "ui_autopower_math.h"

int64_t ui_autopower_clamp_elapsed(int64_t elapsed_ms)
{
    if (elapsed_ms < 0) return 0;
    return elapsed_ms;
}

bool ui_autopower_idle_expired(int64_t elapsed_ms)
{
    if (elapsed_ms < 0) return false;
    return elapsed_ms >= (int64_t)AUTOPOWER_IDLE_TIMEOUT_MS;
}
