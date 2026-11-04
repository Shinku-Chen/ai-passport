// main/main.c —— 《星之终途》阅读器启动：横屏 320x240、离线、三键。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "stella_app.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define STELLA_DIM_MS 60000u
#define STELLA_SCREEN_OFF_MS 300000u
#define STELLA_SLEEP_MS 1800000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35
#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

extern const uint8_t stella_pack_start[] asm("_binary_stella_pack_bin_start");
extern const uint8_t stella_pack_end[] asm("_binary_stella_pack_bin_end");

static stella_app_t s_app;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;
static key_guard_t s_key_guard;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "唤醒键已松手");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const stella_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms，进入 deep sleep", (unsigned)stella_app_idle_ms(&s_app));
    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s)，保持唤醒", esp_err_to_name(wake_err));
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    stella_app_flush(&s_app);
    if (bsp_lvgl_lock(-1)) {
        stella_app_show_sleeping(&s_app);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();

    int wake_level = 0;
    if (bsp_button_prepare_deep_sleep(&wake_level) != ESP_OK) {
        ESP_LOGE(TAG, "按键释放失败，重启");
        esp_restart();
    }
    (void)bsp_i2c_prepare_deep_sleep();
    if (wake_level == 0) {
        ESP_LOGW(TAG, "唤醒脚仍为低，放弃休眠");
        esp_restart();
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止刷屏");
        esp_restart();
    }
    (void)bsp_display_prepare_deep_sleep();
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回");
    esp_restart();
}

static void input_task(void *arg)
{
    (void)arg;
    stella_key_t key;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (got) {
            set_backlight(BACKLIGHT_FULL);
            if (!bsp_lvgl_lock(200)) continue;
            stella_app_key(&s_app, &key);
            bsp_lvgl_unlock();
        }
        if (!bsp_lvgl_lock(200)) continue;
        stella_app_tick(&s_app, elapsed_ms);
        bsp_lvgl_unlock();

        const uint32_t idle = stella_app_idle_ms(&s_app);
        if (idle >= STELLA_SLEEP_MS) enter_sleep();
        else if (idle >= STELLA_SCREEN_OFF_MS) set_backlight(0);
        else if (idle >= STELLA_DIM_MS) set_backlight(BACKLIGHT_DIM);
    }
}

void app_main(void)
{
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒，回到标题页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // 不自动擦除 NVS：擦除会静默丢掉用户的存档/设置。初始化失败时照常启动，
    // 存档写入会失败并在界面上提示，由用户决定是否自行重置。
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) ESP_LOGE(TAG, "NVS 不可用: %s", esp_err_to_name(nvs_err));

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败，电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(BACKLIGHT_FULL);
    if (bsp_lvgl_lock(-1)) {
        (void)bsp_lvgl_set_landscape(true);
        bsp_lvgl_unlock();
    }

    const uint32_t pack_size = (uint32_t)(stella_pack_end - stella_pack_start);
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 加锁失败，无法创建界面");
        return;
    }
    const bool app_ok = stella_app_init(&s_app, stella_pack_start, pack_size);
    bsp_lvgl_unlock();
    if (!app_ok) {
        ESP_LOGE(TAG, "应用初始化失败");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(stella_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    // 4096 不够：这个任务里会跑整套 LVGL 文案刷新 + 分页/UTF-8 处理，
    // 实机出现过 "Stack protection fault, Detected in task stella_input"（SP 越过栈底）。
    // 8192 留足余量（空闲堆 52KB，付得起）。
    if (xTaskCreate(input_task, "stella_input", 8192, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "空闲堆 %u 字节，最大连续块 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪：横屏 320x240；UP 推进/长按快进，DOWN 历史/长按自动，OK 菜单");
}
