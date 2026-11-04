// main/main.c —— 《命运石之门》阅读器:开机直接进作品标题页(320x240 横屏)。
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动应用状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "steins_app.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到标题页)。
#define DIM_MS 60000u
#define SCREEN_OFF_MS 300000u
#define SLEEP_MS 1800000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

static steins_app_t s_app;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;

// 深睡按键唤醒的按键守卫(见 key_guard.h):唤醒键松手前吞掉按键动作。
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
    // 先消费守卫再查就绪位:迟到的 release 若在这里被丢掉,守卫会一直吞下一次真实按键。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const steins_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)steins_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(500));

    steins_app_flush(&s_app);
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
    steins_key_t key;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (got) {
            set_backlight(BACKLIGHT_FULL);
            if (bsp_lvgl_lock(-1)) {
                steins_app_key(&s_app, &key);
                bsp_lvgl_unlock();
            }
        }
        if (bsp_lvgl_lock(-1)) {
            steins_app_tick(&s_app, elapsed_ms);
            bsp_lvgl_unlock();
        }

        const uint32_t idle = steins_app_idle_ms(&s_app);
        if (idle >= SLEEP_MS) {
            enter_sleep();
        } else if (idle >= SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 《命运石之门》阅读器启动");

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到标题页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d)", (int)wakeup);
    }

    // NVS 失败只降级:存档与设置不落盘,但阅读本身照常。绝不在这里擦 NVS。
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s):存档/设置本次不落盘", esp_err_to_name(nvs_err));
    }

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    // 横屏必须在建 UI 之前生效：本作 UI 全部按 320 宽布局，漏了这句就会停在 240x320 竖屏。
    // 必须持 LVGL 锁：lv_display_set_rotation() 会触发 RESOLUTION_CHANGED 回调，不持锁会卡死。
    if (bsp_lvgl_lock(-1)) {
        (void)bsp_lvgl_set_landscape(true);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "无法获取 LVGL 锁");
        return;
    }
    const bool ok = steins_app_init(&s_app);
    bsp_lvgl_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "阅读器初始化失败");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(steins_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "steins_input", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法操作阅读器");
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:短按=推进/确定,长按上=快进,长按下=自动,长按确定=菜单;"
                  "空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(DIM_MS / 1000), (unsigned)(SCREEN_OFF_MS / 1000),
             (unsigned)(SLEEP_MS / 1000));
}
