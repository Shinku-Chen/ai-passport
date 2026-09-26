// No LVGL task may see the display until the rounding callback is registered.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_display_lvgl.c"

static lv_display_t display;
static int panel_present = 1, lock_depth, port_live, display_live, callback_live;
static int fail_lock, fail_port, fail_display, fail_event, init_calls, unlocked_flushes;
static int panel_token, io_token;
esp_lcd_panel_handle_t bsp_display_panel(void) { return panel_present ? &panel_token : NULL; }
esp_lcd_panel_io_handle_t bsp_display_io(void) { return &io_token; }
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg) {
    (void)cfg; ++init_calls; assert(!port_live);
    if (fail_port) return ESP_ERR_NO_MEM;
    port_live = 1; return ESP_OK;
}
esp_err_t lvgl_port_deinit(void) {
    // The real API is asynchronous: returning does not release the context.
    assert(false && "Display rollback must not deinit/reinitialize the live port");
    return ESP_OK;
}
bool lvgl_port_lock(uint32_t timeout) {
    (void)timeout; assert(port_live);
    if (fail_lock) return false;
    ++lock_depth; return true;
}
void lvgl_port_unlock(void) {
    assert(lock_depth > 0);
    --lock_depth;
    if (!lock_depth && display_live) {
        assert(callback_live); // Simulate rendering as soon as the lock is free.
        ++unlocked_flushes;
    }
}
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg) {
    assert(lock_depth > 0 && cfg->panel_handle == &panel_token && !display_live);
    assert(lvgl_port_lock(0)); // Real port takes and releases a recursive lock.
    if (!fail_display) display_live = 1;
    lvgl_port_unlock();
    return fail_display ? NULL : &display;
}
esp_err_t lvgl_port_remove_disp(lv_display_t *disp) {
    assert(disp == &display && lock_depth && display_live);
    display_live = callback_live = 0; return ESP_OK;
}
void lv_display_add_event_cb(lv_display_t *disp, void (*cb)(lv_event_t *), int code, void *user) {
    (void)user;
    assert(lock_depth && disp == &display && code == LV_EVENT_FLUSH_START && cb == rounded_flush_event);
    if (!fail_event) callback_live = 1;
}
uint32_t lv_display_get_event_count(lv_display_t *disp) {
    assert(disp == &display && lock_depth);
    return 1 + callback_live; // The display already owns an internal callback.
}
// 横屏支持:运行时请求旋转,分辨率 getter 跟着切换(与 LVGL 换逻辑分辨率一致)。
static int rotation_calls;
static lv_display_rotation_t last_rotation;
static int landscape_live;
void lv_display_set_rotation(lv_display_t *disp, lv_display_rotation_t rotation) {
    assert(disp == &display);
    ++rotation_calls;
    last_rotation = rotation;
    landscape_live = (rotation == LV_DISPLAY_ROTATION_90 || rotation == LV_DISPLAY_ROTATION_270);
}
int32_t lv_display_get_horizontal_resolution(const lv_display_t *disp) {
    assert(disp == &display);
    return landscape_live ? BSP_LCD_H : BSP_LCD_W;
}
int32_t lv_display_get_vertical_resolution(const lv_display_t *disp) {
    assert(disp == &display);
    return landscape_live ? BSP_LCD_W : BSP_LCD_H;
}

static void expect_failure(void) {
    assert(bsp_lvgl_init() == NULL);
    assert(!s_disp && !lock_depth && !display_live);
    assert(!bsp_lvgl_lock(0));
}
int main(void) {
    assert(bsp_lvgl_set_landscape(true) == ESP_FAIL);   // 未初始化时不可切换
    panel_present = 0; expect_failure(); panel_present = 1;
    fail_port = 1; expect_failure(); fail_port = 0;
    const int failed_init_calls = init_calls;
    expect_failure(); // A partially initialized port cannot safely be re-created.
    assert(init_calls == failed_init_calls);
    s_port_init_failed = false; // Simulate reboot for remaining scenarios.
    fail_lock = 1; expect_failure(); fail_lock = 0;
    const int retained_init_calls = init_calls;
    fail_display = 1; expect_failure(); fail_display = 0;
    fail_event = 1; expect_failure(); fail_event = 0;
    assert(bsp_lvgl_init() == &display);
    assert(init_calls == retained_init_calls); // Display retries reuse the port.
    assert(callback_live && !lock_depth && unlocked_flushes == 1);
    const int before = init_calls;
    assert(bsp_lvgl_init() == &display && init_calls == before);
    assert(bsp_lvgl_lock(5)); bsp_lvgl_unlock();
    uint16_t pixels[BSP_LCD_W] = {0};
    for (int x = 0; x < BSP_LCD_W; ++x) pixels[x] = 0xffff;
    display.buffer = (lv_draw_buf_t){ .data = (uint8_t *)pixels, .header.stride = sizeof(pixels) };
    lv_area_t area = { .x1 = 0, .y1 = 0, .x2 = BSP_LCD_W - 1, .y2 = 0 };
    lv_event_t ev = { .target = &display, .area = &area };
    rounded_flush_event(&ev);
    assert(pixels[0] == 0 && pixels[BSP_LCD_W - 1] == 0 && pixels[BSP_LCD_W / 2] == 0xffff);

    // 横屏:逻辑分辨率变 320x240,圆角遮罩必须跟着换 —— 写死 240x320 会把两条边涂黑。
    assert(bsp_lvgl_set_landscape(true) == ESP_OK);
    assert(rotation_calls == 1 && last_rotation == LV_DISPLAY_ROTATION_90 && landscape_live);
    uint16_t wide[BSP_LCD_H] = {0};
    for (int x = 0; x < BSP_LCD_H; ++x) wide[x] = 0xffff;
    display.buffer = (lv_draw_buf_t){ .data = (uint8_t *)wide, .header.stride = sizeof(wide) };
    lv_area_t wide_top = { .x1 = 0, .y1 = 0, .x2 = BSP_LCD_H - 1, .y2 = 0 };
    lv_event_t wide_ev = { .target = &display, .area = &wide_top };
    rounded_flush_event(&wide_ev);
    assert(wide[0] == 0 && wide[BSP_LCD_H - 1] == 0 && wide[BSP_LCD_H / 2] == 0xffff);

    // 横屏中间行离圆角足够远:整行保留。
    for (int x = 0; x < BSP_LCD_H; ++x) wide[x] = 0xffff;
    lv_area_t wide_mid = { .x1 = 0, .y1 = 120, .x2 = BSP_LCD_H - 1, .y2 = 120 };
    lv_event_t mid_ev = { .target = &display, .area = &wide_mid };
    rounded_flush_event(&mid_ev);
    assert(wide[0] == 0xffff && wide[BSP_LCD_H - 1] == 0xffff);

    // 切回竖屏后分辨率与遮罩都复原。
    assert(bsp_lvgl_set_landscape(false) == ESP_OK);
    assert(rotation_calls == 2 && last_rotation == LV_DISPLAY_ROTATION_0 && !landscape_live);
    puts("BSP LVGL initialization tests: PASS");
}
