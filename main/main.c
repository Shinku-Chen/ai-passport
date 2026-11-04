// main/main.c —— 《想成为Galgame领域大神！！！》移植版:开机直接进标题页(横屏 320x240)。
//
// 按键语义(三键,与 gal_app.h 一致):
//   正文页   UP 短按 = 下一句(打字中先整页显示);UP 长按 = 快进(松开停)
//            DOWN 短按 = 历史文本;OK 短按/长按 = 游戏内菜单
//   选项页   UP/DOWN 选择,OK 确认
//   列表页   UP/DOWN 选择,OK 执行,长按 OK 返回
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写(由 gal_app 内部负责)。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "gal_app.h"
#include "key_guard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "main";

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回标题页)。
#define GAL_DIM_MS 90000u
#define GAL_SCREEN_OFF_MS 300000u
#define GAL_SLEEP_MS 1800000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 40

// 资源包(EMBED_FILES 生成的符号)。
extern const uint8_t gal_pack_bin_start[] asm("_binary_gal_pack_bin_start");
extern const uint8_t gal_pack_bin_end[] asm("_binary_gal_pack_bin_end");

// 画面缓冲(静态区 .bss,不占堆):
//   art     320x150 RGB565 = 96,000 B(画面上半)
//   strip   160x45  RGB565 = 14,400 B(文本框背后,放大 2 倍显示)
//   scratch 96x150  RGB565 = 28,800 B(立绘解码暂存,上/下半段复用)
// 另外 BSP 在 LVGL 初始化时从堆上分配 240x24x2 = 11,520 B 刷屏缓冲;
// LVGL 用 CLIB malloc,对象/标签文本与上面这些静态缓冲区共享同一块 DRAM。
static uint16_t s_art_px[320 * 150];
static uint16_t s_strip_px[160 * 45];
static uint8_t s_sprite_scratch[96 * 150 * 2];

static gal_app_t s_app;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
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
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const gal_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)gal_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(400));

    // 睡前把进度落盘。
    gal_app_flush(&s_app);
    vTaskDelay(pdMS_TO_TICKS(100));

    (void)bsp_battery_sleep();

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
    gal_key_t key;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;
        if (got) {
            set_backlight(BACKLIGHT_FULL);
            gal_app_key(&s_app, &key);
        }
        gal_app_tick(&s_app, elapsed_ms);
        const uint32_t idle = gal_app_idle_ms(&s_app);
        if (idle >= GAL_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= GAL_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= GAL_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "GalGod 移植版启动");

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到标题页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d)", (int)wakeup);
    }

    // NVS 只做一次 init。失败时**不自动擦除**——擦掉会把用户全部存档/设置一起丢掉;
    // 本次运行退化为"不落盘",保存操作会明确失败并在界面上提示。
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s):本次运行不保存设置与存档", esp_err_to_name(nvs_err));
    }

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    // 横屏：UI 全部按 320x240 布局，漏了这句会停在 240x320 竖屏（背景/菜单全错位）。
    // 必须持 LVGL 锁：lv_display_set_rotation() 触发 RESOLUTION_CHANGED，裸调会卡在 layout。
    if (bsp_lvgl_lock(-1)) {
        (void)bsp_lvgl_set_landscape(true);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "横屏切换失败（LVGL 锁超时）");
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    const uint32_t pack_size = (uint32_t)(gal_pack_bin_end - gal_pack_bin_start);
    ESP_LOGI(TAG, "资源包 %u B,空闲堆 %u B", (unsigned)pack_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    const gal_app_buffers_t buffers = {
        .art_px = s_art_px,
        .strip_px = s_strip_px,
        .scratch = s_sprite_scratch,
        .scratch_size = sizeof(s_sprite_scratch),
    };
    if (!gal_app_init(&s_app, gal_pack_bin_start, pack_size, &buffers)) {
        ESP_LOGE(TAG, "应用初始化失败");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(gal_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "gal_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法操作");
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "启动完成,空闲堆 %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
