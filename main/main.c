// main/main.c —— 星光咖啡馆与死神之蝶（Cafe Stella）离线阅读器启动。
//
// 目标：ESP32-C3 / 8MB Flash / 无 PSRAM / ESP-IDF 5.5.3，320x240 横屏。
// 三键：OK / UP / DOWN，阅读页短按三键都是下一句/下一分页，
// 长按 OK 游戏内菜单，长按 UP 快进，长按 DOWN 自动阅读。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "cafe_app.h"
#include "cafe_audio.h"
#include "cafe_image.h"
#include "cafe_pack.h"
#include "cafe_ui.h"
#include "key_guard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "main";

#define DIM_MS 60000u
#define SCREEN_OFF_MS 180000u
#define SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 64
#define INPUT_TICK_MS 50
#define BATTERY_MS 30000u

static cafe_app_t s_app;
static cafe_ui_t s_ui;
static cafe_bands_t s_bands;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static bool s_input_ready;
static uint32_t s_input_drops;
static volatile uint32_t s_input_overflow;
static uint32_t s_lock_timeouts;
static uint8_t s_backlight;   // 首帧提交后才点亮

static key_guard_t s_key_guard;
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

LV_FONT_DECLARE(gal_cjk_16);

typedef struct {
    uint8_t btn;
    uint8_t ev;
    uint32_t event_ms;   // sampled in the button callback, never at dequeue time
} ui_key_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

static const uint8_t s_key_map[BSP_BTN_COUNT] = {
    [BSP_BTN_UP] = CAFE_KEY_UP,
    [BSP_BTN_DOWN] = CAFE_KEY_DOWN,
    [BSP_BTN_OK] = CAFE_KEY_OK,
};

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_RELEASE) return;
    const uint32_t event_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const ui_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev, .event_ms = event_ms };
    // The callback is allowed only a bounded handoff. 64 entries cover a
    // render/lock stall of several seconds at human key rates; an overflow is
    // counted and reported by the input task instead of silently ignored.
    if (xQueueSend(s_input_queue, &key, 0) != pdTRUE) {
        s_input_drops++;
        (void)__atomic_add_fetch(&s_input_overflow, 1, __ATOMIC_RELAXED);
    }
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)cafe_app_idle_ms(&s_app));
    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (!cafe_audio_stop_confirmed(1200)) {
        ESP_LOGE(TAG, "音频停止未确认，保持唤醒");
        set_backlight(BACKLIGHT_FULL);
        return;
    }
    cafe_app_flush(&s_app);
    (void)bsp_battery_sleep();
    if (bsp_lvgl_lock(-1)) {
        (void)bsp_display_prepare_deep_sleep();
        bsp_lvgl_unlock();
    } else {
        (void)bsp_display_prepare_deep_sleep();
    }
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
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启");
    esp_restart();
}

// A full queue means a press or release may be missing.  Do not try to keep
// every overloaded event: discard the stale queue, abort the partial key
// cycles, stop FF/auto, and let the next complete press start cleanly.  This is
// intentionally limited to UI input state; the VM/page/cursor/save are untouched.
static bool input_recover_overflow(void)
{
    if (s_input_overflow == 0) return false;
    if (!bsp_lvgl_lock(1000)) {
        s_lock_timeouts++;
        return true;
    }
    const uint32_t count = __atomic_exchange_n(&s_input_overflow, 0, __ATOMIC_ACQ_REL);
    xQueueReset(s_input_queue);
    cafe_app_cancel_input(&s_app);
    bsp_lvgl_unlock();
    if (count) ESP_LOGW(TAG, "input overflow recovery: dropped=%u queue_cleared", (unsigned)count);
    return true;
}

static void input_task(void *arg)
{
    (void)arg;
    ui_key_t key;
    int64_t last_us = esp_timer_get_time();
    uint32_t battery_accum = 0;
    uint32_t last_drops = 0;
    for (;;) {
        if (s_input_overflow != 0) {
            const bool pending = input_recover_overflow();
            if (pending) {
                vTaskDelay(pdMS_TO_TICKS(5));
                last_us = esp_timer_get_time();
                continue;
            }
            continue;
        }
        const bool peeked = xQueuePeek(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t t0 = esp_timer_get_time();
        const uint32_t now = (uint32_t)(t0 / 1000);
        const uint32_t elapsed = (uint32_t)((t0 - last_us) / 1000);
        if (peeked) {
            if (!bsp_lvgl_lock(1000)) {
                s_lock_timeouts++;
                vTaskDelay(pdMS_TO_TICKS(5));
                last_us = esp_timer_get_time();
                continue;
            }
            if (s_input_overflow != 0) {
                bsp_lvgl_unlock();
                continue;
            }
            int batch = 0;
            while (batch < INPUT_QUEUE_DEPTH && xQueueReceive(s_input_queue, &key, 0) == pdTRUE) {
                const uint32_t proc = (uint32_t)(esp_timer_get_time() / 1000);
                set_backlight(BACKLIGHT_FULL);
                cafe_app_key_at(&s_app, s_key_map[key.btn], key.ev == BSP_BTN_PRESS,
                                key.event_ms, proc);
                batch++;
            }
            const uint32_t proc = (uint32_t)(esp_timer_get_time() / 1000);
            cafe_app_tick(&s_app, proc, elapsed);
            bsp_lvgl_unlock();
            if (batch > 0) {
                ESP_LOGI(TAG, "input batch=%d drops=%u lock_timeouts=%u stack_min=%u heap=%u largest=%u",
                         batch, (unsigned)s_input_drops, (unsigned)s_lock_timeouts,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL),
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            }
        } else {
            if (!bsp_lvgl_lock(1000)) {
                s_lock_timeouts++;
                vTaskDelay(pdMS_TO_TICKS(5));
                last_us = esp_timer_get_time();
                continue;
            }
            cafe_app_tick(&s_app, now, elapsed);
            bsp_lvgl_unlock();
        }
        if (s_input_drops != last_drops) last_drops = s_input_drops;
        battery_accum += elapsed;
        if (battery_accum >= BATTERY_MS) {
            battery_accum = 0;
            const int soc = bsp_battery_soc();
            if (bsp_lvgl_lock(1000)) {
                cafe_app_set_battery(&s_app, soc);
                bsp_lvgl_unlock();
            }
        }
        last_us = esp_timer_get_time();
        const uint32_t idle = cafe_app_idle_ms(&s_app);
        if (idle >= SLEEP_MS) enter_sleep();
        else if (idle >= SCREEN_OFF_MS) set_backlight(0);
        else if (idle >= DIM_MS) set_backlight(BACKLIGHT_DIM);
    }
}

static void show_fatal(const char *message)
{
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "错误屏无法获取 LVGL 锁: %s", message);
        return;
    }
    cafe_ui_show_fatal(message);
    lv_refr_now(NULL);   // 先提交错误画面，再开背光；异步 flush 完成度未单独测量
    bsp_lvgl_unlock();
    bsp_display_backlight(BACKLIGHT_FULL);
    s_backlight = BACKLIGHT_FULL;
    ESP_LOGE(TAG, "FATAL: %s", message ? message : "unknown");
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport Cafe Stella 启动 reset_reason=%d", (int)esp_reset_reason());

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
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
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 初始化后无法加锁");
        return;
    }
    const esp_err_t land_err = bsp_lvgl_set_landscape(true);
    bsp_lvgl_unlock();
    if (land_err != ESP_OK) {
        ESP_LOGE(TAG, "横屏切换失败(%s)", esp_err_to_name(land_err));
        show_fatal("横屏初始化失败，无法开始阅读");
        return;
    }
    if (!cafe_pack_init()) {
        ESP_LOGE(TAG, "资源包初始化失败");
        show_fatal("cafe.pak 资源包校验失败");
        return;
    }
    (void)cafe_image_init();
    if (!cafe_audio_init()) {
        ESP_LOGE(TAG, "音频任务初始化失败");
        show_fatal("音频任务初始化失败");
        return;
    }
    if (!cafe_image_bands_init(&s_bands)) {
        ESP_LOGE(TAG, "图像条带分配失败");
        show_fatal("内存不足：图像条带分配失败");
        return;
    }
    bool first_frame_ok = false;
    if (bsp_lvgl_lock(-1)) {
        first_frame_ok = cafe_ui_create(&s_ui) && cafe_app_init(&s_app, &s_ui, &s_bands);
        if (first_frame_ok) lv_refr_now(NULL);   // 首帧提交后才开背光
        bsp_lvgl_unlock();
    }
    if (!first_frame_ok) {
        ESP_LOGE(TAG, "界面/阅读器初始化失败");
        show_fatal("阅读器界面初始化失败");
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    s_app.now_ms = now_ms();
    s_app.last_activity_ms = s_app.now_ms;

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(ui_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        show_fatal("输入队列创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        show_fatal("按键初始化失败");
        return;
    }
    if (xTaskCreate(input_task, "cafe_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        (void)bsp_button_init(NULL, NULL);   // detach callback before fatal screen
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        show_fatal("输入任务创建失败");
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪: 320x240 横屏;短按三键下一句,长按 OK 菜单,长按 UP 快进,长按 DOWN 自动");
}
