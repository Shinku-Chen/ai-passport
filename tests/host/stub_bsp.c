// tests/host/stub_bsp.c —— BSP/LVGL 锁、横屏与电池替身；锁深度用于检测未加锁的 LVGL 访问。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

const lv_font_t gal_cjk_16;
const lv_font_t gal_cjk_20;

static int s_depth;
static int s_violations;
static int s_battery = 87;
static bool s_landscape;

int host_lvgl_depth(void) { return s_depth; }
void host_lvgl_note_violation(void) { s_violations++; }
int host_lvgl_violations(void) { return s_violations; }
void host_lvgl_reset_violations(void) { s_violations = 0; }
bool host_landscape(void) { return s_landscape; }
void host_battery_set(int soc) { s_battery = soc; }

esp_err_t bsp_lvgl_set_landscape(bool landscape)
{
    if (s_depth <= 0) {
        s_violations++;
    }
    s_landscape = landscape;
    return 0;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    s_depth++;
    return true;
}

void bsp_lvgl_unlock(void)
{
    if (s_depth > 0) {
        s_depth--;
    } else {
        s_violations++;
    }
}

void bsp_display_backlight(uint8_t percent) { (void)percent; }
int bsp_battery_soc(void) { return s_battery; }
