// tests/app_stubs/bsp_stub.c —— 锁与电量的空实现(单线程测试,锁为递归计数)。
#include "bsp_battery.h"
#include "bsp_display.h"

static int s_depth;

bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    s_depth++;
    return true;
}

void bsp_lvgl_unlock(void)
{
    if (s_depth > 0) s_depth--;
}

int bsp_battery_soc(void) { return 77; }
