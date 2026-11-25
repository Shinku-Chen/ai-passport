// 9-nine- 天色天歌天籁音: boot, three-key input task, idle policy and deep sleep.
//
// Reading page: short OK/UP/DOWN advance; long OK menu; long DOWN auto;
// hold UP fast-forward; secondary pages: long OK returns. The pure policy lives
// in nine_reader.c and is host-tested with a controlled clock.
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "nine_app.h"
#include "nine_media.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define NINE_DIM_MS 60000u
#define NINE_SCREEN_OFF_MS 180000u
#define NINE_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35
#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

static nine_app_t s_app;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
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
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const nine_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)nine_app_idle_ms(&s_app));
    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    nine_app_flush(&s_app);
    nine_media_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();
    int wake_level = 0;
    if (bsp_button_prepare_deep_sleep(&wake_level) != ESP_OK) {
        ESP_LOGE(TAG, "按键释放失败,重启恢复外设");
        esp_restart();
    }
    (void)bsp_i2c_prepare_deep_sleep();
    if (wake_level == 0) {
        ESP_LOGW(TAG, "唤醒脚仍为低电平,放弃本次休眠并重启");
        esp_restart();
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏,重启恢复外设");
        esp_restart();
    }
    (void)bsp_display_prepare_deep_sleep();
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启恢复外设");
    esp_restart();
}

static void input_task(void *arg)
{
    (void)arg;
    nine_key_t key;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (got) {
            set_backlight(BACKLIGHT_FULL);
            nine_app_key(&s_app, &key);
        }
        nine_app_tick(&s_app, elapsed_ms);
        const uint32_t idle = nine_app_idle_ms(&s_app);
        if (idle >= NINE_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= NINE_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= NINE_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "9-nine- 天色天歌天籁音 reader boot");
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到启动页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d)", (int)wakeup);
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建(%s)", esp_err_to_name(nvs_err));
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) ESP_LOGW(TAG, "NVS 不可用: %s", esp_err_to_name(nvs_err));

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    // Landscape must be applied after LVGL exists and under its lock. A failed
    // call must not leave the firmware pretending to be a ready landscape UI.
    bool landscape_ok = false;
    if (bsp_lvgl_lock(-1)) {
        landscape_ok = bsp_lvgl_set_landscape(true) == ESP_OK;
        bsp_lvgl_unlock();
    }
    if (!landscape_ok) {
        ESP_LOGE(TAG, "横屏设置失败,保持背光关闭并停止启动");
        bsp_display_backlight(0);
        return;
    }

    // The app has created the UI (or a visible error page) before the backlight
    // is enabled.
    if (!nine_app_init(&s_app)) {
        ESP_LOGE(TAG, "阅读器界面初始化失败");
        bsp_display_backlight(0);
        return;
    }
    // The UI is built; 40 ms is only a scheduling margin before enabling the
    // backlight. It is not a verified LVGL flush-complete signal.
    vTaskDelay(pdMS_TO_TICKS(40));
    bsp_display_backlight(BACKLIGHT_FULL);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(nine_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        bsp_display_backlight(0);
        return;
    }
    if (xTaskCreate(input_task, "nine_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        bsp_display_backlight(0);
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法操作阅读器");
        s_input_ready = false;
        vTaskDelete(s_input_task);
        s_input_task = NULL;
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        bsp_display_backlight(0);
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:横屏 320x240;短 OK/上/下=下一句,长 OK=菜单,"
                  "长下=自动,按住上=快进;空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(NINE_DIM_MS / 1000), (unsigned)(NINE_SCREEN_OFF_MS / 1000),
             (unsigned)(NINE_SLEEP_MS / 1000));
}
