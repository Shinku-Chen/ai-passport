// main/main.c —— 《沙耶之歌》AI Passport 移植:开机直接进阅读器(横屏 320x240)。
//
// 按键语义:
//   标题/列表页 上、下移动光标(到顶/到底停住),确定进入,长按确定返回
//   正文页      确定打开菜单;上 短按=下一段(打字中先补全)、长按=快进、松手停;
//               下 短按=回看上一页
//   选项页      上、下选择,确定确认
//   关于页      上、下滚动正文,确定返回
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动应用状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写(由 saya_app 内部负责)。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "saya_app.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 资源包由 main/CMakeLists.txt 的 EMBED_FILES 注入(见 tools/saya_pack.py)。
extern const uint8_t saya_pack_bin_start[] asm("_binary_saya_pack_bin_start");
extern const uint8_t saya_pack_bin_end[] asm("_binary_saya_pack_bin_end");

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到标题页继续读)。
#define SAYA_DIM_MS 60000u
#define SAYA_SCREEN_OFF_MS 180000u
#define SAYA_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

// 画面区 320x136 + 立绘解码缓冲(宽上限与 tools/saya_pack.py 的 SPRITE_MAX_W 一致)。
static uint16_t s_art_pixels[SAYA_ART_W * SAYA_ART_H];
static uint8_t s_sprite_scratch[SAYA_SPRITE_MAX_W * SAYA_ART_H * 2];
static saya_app_t s_app;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

// 按键回调跑在 button 组件的共享 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const saya_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
// 顺序与 demo/connect-four 在真机上验证过的一致。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)saya_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        // 配不上唤醒源就绝不睡下去,否则醒不过来。
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    saya_app_before_sleep(&s_app);
    if (bsp_lvgl_lock(200)) {
        saya_app_show_sleeping(&s_app);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // CW2017 与 ES8311 共用 I2C:先让电量计收尾,再停 codec 与 I2S。
    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();

    // 按键脚交回数字输入并上拉,顺便回读电平:按着不放时宁愿重启也不要刚睡就醒。
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
    saya_key_t key;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (got) {
            set_backlight(BACKLIGHT_FULL);
            saya_app_key(&s_app, &key);
        }
        saya_app_tick(&s_app, elapsed_ms);

        const uint32_t idle = saya_app_idle_ms(&s_app);
        if (idle >= SAYA_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= SAYA_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= SAYA_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 沙耶之歌启动");

    // deep sleep 唤醒会重启整个应用,把原因打出来便于确认"按键真能唤醒"。
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到阅读器");
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

    // 电量计是软依赖:读不到就显示 "--"。
    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    // 显示是硬依赖:没有屏幕就没法阅读。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    const saya_app_buffers_t buffers = {
        .art_pixels = s_art_pixels,
        .sprite_scratch = s_sprite_scratch,
        .sprite_scratch_size = sizeof(s_sprite_scratch),
    };

    if (bsp_lvgl_lock(-1)) {
        // 先切横屏,再按 320x240 排版建屏:顺序反了会按竖屏尺寸算布局。
        (void)bsp_lvgl_set_landscape(true);
        const uint32_t pack_size = (uint32_t)(saya_pack_bin_end - saya_pack_bin_start);
        if (!saya_app_init(&s_app, saya_pack_bin_start, pack_size, &buffers)) {
            ESP_LOGE(TAG, "阅读器初始化失败(资源包 %u 字节)", (unsigned)pack_size);
            bsp_lvgl_unlock();
            return;
        }
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,无法建界面");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(saya_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "saya_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法操作阅读器");
        return;
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:横屏阅读器;确定开菜单,上(短按/长按)下一段/快进,下回看上一页;"
                  "空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(SAYA_DIM_MS / 1000), (unsigned)(SAYA_SCREEN_OFF_MS / 1000),
             (unsigned)(SAYA_SLEEP_MS / 1000));
}
