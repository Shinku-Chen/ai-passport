// main/main.c —— 《anemoi》AI Passport 横屏阅读器。
//
// 按键语义（与 docs/development/engineering/galgame-anemoi.md 一致）：
//   正文  UP 短按推进 / 长按快进（松手停）；DOWN 短按历史 / 长按自动；
//         OK 短按推进或补全打字 / 长按菜单
//   列表  UP/DOWN 移动，OK 短按进入，OK 长按返回来源页
//   待选  UP/DOWN 移动，OK 短按显式确认，OK 长按菜单（返回保留待选与光标）
//
// 线程模型：按键回调只入队；输入任务串行处理事件并驱动状态机；
// LVGL 对象只在持 bsp_lvgl_lock() 时改写。空闲时先调暗、再熄屏、最后 deep sleep；
// 睡前把自动档与全局解锁强制落盘。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "anemoi_app.h"
#include "anemoi_startup.h"

#include "anemoi_data.h"
#include "anemoi_render.h"
#include "lvgl.h"

LV_FONT_DECLARE(gal_cjk_16);

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"

#include <string.h>

static const char *TAG = "anemoi";

#define ANEM_DIM_MS 60000u
#define ANEM_SCREEN_OFF_MS 300000u   // 5 分钟关背光
#define ANEM_SLEEP_MS 1800000u    // 30 分钟才深睡
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35
#define INPUT_QUEUE_DEPTH 16
#define INPUT_TICK_MS 30
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static anem_app_t s_app;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;
static key_guard_t s_key_guard;
static uint32_t s_last_view_hash;

typedef struct {
    uint8_t key;
    uint8_t event;
} input_event_t;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) {
        return;
    }
    s_backlight = percent;
    bsp_display_backlight(percent);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手，按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || s_input_queue == NULL) {
        return;
    }
    input_event_t item = { 0, 0 };
    item.key = (uint8_t)(btn == BSP_BTN_UP ? ANEM_KEY_UP
                                           : (btn == BSP_BTN_DOWN ? ANEM_KEY_DOWN
                                                                  : ANEM_KEY_OK));
    switch (ev) {
    case BSP_BTN_PRESS: item.event = ANEM_EV_PRESS; break;
    case BSP_BTN_CLICK: item.event = ANEM_EV_CLICK; break;
    case BSP_BTN_LONG: item.event = ANEM_EV_LONG; break;
    case BSP_BTN_RELEASE: item.event = ANEM_EV_RELEASE; break;
    default: return;
    }
    if (xQueueSend(s_input_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "输入队列满，丢弃 key=%u ev=%u", (unsigned)item.key, (unsigned)item.event);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool view_hash(const anem_view_t *view, uint32_t *hash)
{
    // 简易指纹：屏幕/页/行数/选中/打字进度 + 所有文本内容。
    uint32_t h = 2166136261u;
    const uint8_t *raw = (const uint8_t *)view;
    for (size_t i = 0; i < sizeof(*view); i++) {
        h ^= raw[i];
        h *= 16777619u;
    }
    *hash = h;
    return true;
}

static bool present(bool force)
{
    anem_view_t view;
    anem_app_view(&s_app, &view);
    uint32_t hash = 0;
    (void)view_hash(&view, &hash);
    if (!force && hash == s_last_view_hash) {
        if (anem_app_art_dirty(&s_app)) {
            if (bsp_lvgl_lock(500)) {
                if (anem_app_draw(&s_app)) {
                    anem_render_set_art(anem_canvas_dsc(&s_app.canvas), ANEM_CANVAS_W,
                                        ANEM_CANVAS_H);
                    anem_render_invalidate_art();
                }
                bsp_lvgl_unlock();
            }
        }
        return true;   // 无变化：上一帧已经提交过
    }
    if (!bsp_lvgl_lock(1000)) {
        return false;   // 失败不提交 hash，下次仍会尝试
    }
    s_last_view_hash = hash;
    if (anem_app_art_dirty(&s_app)) {
        if (anem_app_draw(&s_app)) {
            anem_render_set_art(anem_canvas_dsc(&s_app.canvas), ANEM_CANVAS_W, ANEM_CANVAS_H);
        }
        anem_render_invalidate_art();
    }
    anem_render_apply(&view, s_app.panel_alpha);
    bsp_lvgl_unlock();
    return true;
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms，进入 deep sleep（任意按键唤醒）",
             (unsigned)ANEM_SLEEP_MS);
    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s)，保持唤醒", esp_err_to_name(wake_err));
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)anem_app_flush_auto(&s_app);
    (void)anem_app_flush_progress(&s_app);
    (void)bsp_battery_sleep();

    int wake_level = 0;
    if (bsp_button_prepare_deep_sleep(&wake_level) != ESP_OK) {
        ESP_LOGE(TAG, "按键释放失败，重启恢复外设");
        esp_restart();
    }
    (void)bsp_i2c_prepare_deep_sleep();
    if (wake_level == 0) {
        ESP_LOGW(TAG, "唤醒脚仍为低电平，放弃本次休眠并重启");
        esp_restart();
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    (void)bsp_display_prepare_deep_sleep();
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回，重启恢复外设");
    esp_restart();
}


static void input_task(void *arg)
{
    (void)arg;
    input_event_t item;
    uint32_t last_input = now_ms();
    for (;;) {
        const bool got =
            xQueueReceive(s_input_queue, &item, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const uint32_t now = now_ms();
        if (got) {
            set_backlight(BACKLIGHT_FULL);
            last_input = now;
            anem_app_key(&s_app, item.key, item.event, now);
            present(false);
        }
        anem_app_tick(&s_app, now);
        present(false);
        uint32_t idle = now - last_input;
        if (s_app.auto_enabled) {
            // 自动阅读期间不得调暗/熄屏/深睡，并把空闲计时持续重置。
            last_input = now;
            idle = 0;
        }
        if (idle >= ANEM_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= ANEM_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= ANEM_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}


// ---------------------------------------------------------------- 启动顺序
// 显示已经可用但资源包失败时的可见错误屏：复用现有静态字体/纸色，不引入新资源。
static bool show_error_screen(const char *msg)
{
    if (!bsp_lvgl_lock(1000)) {
        return false;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFCFAF0), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, 300);
    lv_obj_set_pos(label, 10, 90);
    lv_obj_set_style_text_font(label, &gal_cjk_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x3F3129), 0);
    lv_label_set_text(label, msg);
    bsp_lvgl_unlock();
    return true;
}

static bool startup_set_landscape(void) { return bsp_lvgl_set_landscape(true) == ESP_OK; }

static void startup_log(const char *msg)
{
    ESP_LOGE(TAG, "%s", msg);
}

static bool s_pack_ok;

static bool startup_lvgl_lock(int ms) { return bsp_lvgl_lock(ms); }
static void startup_lvgl_unlock(void) { bsp_lvgl_unlock(); }
static bool startup_render_init(void) { return anem_render_init(); }
static bool startup_app_init(void)
{
    if (!anem_app_init(&s_app, anemoi_pack_blob(), anemoi_pack_blob_size())) {
        return false;
    }
    anem_app_refresh_store(&s_app, now_ms());
    s_pack_ok = true;
    return true;
}
static bool startup_present(void)
{
    const bool ok = present(true);
    return ok;
}
static void startup_backlight(int percent) { bsp_display_backlight(percent); }
static bool startup_input_start(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) {
        return false;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return false;
    }
    // 设备实测：12288 必失败（largest 8,704），6144 可成；保留 4096 一次降级重试。
    BaseType_t created = xTaskCreate(input_task, "anemoi_input", 6144, NULL, 5, &s_input_task);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "6144 栈创建失败（空闲堆 %u，最大块 %u），回退 4096",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        created = xTaskCreate(input_task, "anemoi_input", 4096, NULL, 5, &s_input_task);
    }
    if (created != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return false;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;
    return true;
}

static const anem_startup_ops_t s_startup_ops = {
    startup_lvgl_lock, startup_lvgl_unlock, startup_render_init, startup_app_init,
    startup_present, startup_backlight, startup_input_start, show_error_screen, startup_log,
    startup_set_landscape,
};

void app_main(void)
{
    ESP_LOGI(TAG, "anemoi 阅读器启动");

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒，回到标题");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // NVS 只初始化，不擦除：失败就降级为本次运行内有效（anemoi_store 会报告）。
    (void)anem_store_init();

    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C 初始化失败，电量显示 --");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败，电量显示 --");
    }
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // 资源包/首帧/输入的顺序与失败语义统一由 anem_startup_run 负责：
    // 首帧成功提交后才点背光；坏包给出可见错误屏；任何失败都不打印“就绪”。
    const anem_start_result_t boot = anem_startup_run(&s_startup_ops);
    if (boot != ANEM_START_OK) {
        ESP_LOGE(TAG, "启动失败：%s", anem_start_result_name(boot));
        return;
    }
    if (!s_pack_ok) {
        ESP_LOGE(TAG, "内部状态错误：资源包未初始化");
        return;
    }
    ESP_LOGI(TAG, "资源包：%u 条指令 / %u 场景 / %u 张图 / 字号 16px",
             (unsigned)anem_pack_op_count(&s_app.pack),
             (unsigned)anem_pack_scene_count(&s_app.pack),
             (unsigned)anem_pack_image_count(&s_app.pack));
    ESP_LOGI(TAG, "空闲堆 %u 字节，最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    ESP_LOGI(TAG, "就绪：UP 推进/长按快进，DOWN 历史/长按自动，OK 推进/长按菜单");
}
