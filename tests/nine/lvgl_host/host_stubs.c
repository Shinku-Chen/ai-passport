#include "esp_heap_caps.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include <stdlib.h>

static int s_fail;
static int s_allocs;
void host_heap_fail_next(int count) { s_fail = count; }
int host_heap_alloc_count(void) { return s_allocs; }
void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    ++s_allocs;
    if (s_fail > 0) { --s_fail; return NULL; }
    return malloc(size);
}
void heap_caps_free(void *p) { free(p); }
uint32_t esp_get_free_heap_size(void) { return 200000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 100000; }
bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
esp_err_t bsp_lvgl_set_landscape(bool landscape) { (void)landscape; return ESP_OK; }
int bsp_battery_soc(void) { return 88; }
