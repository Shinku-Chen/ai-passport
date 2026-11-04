// main/main.c —— 《魔法少女的魔女审判》AI Passport 横屏阅读器入口。
//
// 按键语义(三键):
//   正文页  UP 短按 = 下一句/下一页(打字中先全显);UP 长按 = 快进,松手停
//           DOWN 短按 = 历史;DOWN 长按 = 自动播放开关
//           OK 短按 = 菜单(有反驳词时打开反驳列表);OK 长按 = 菜单
//   列表页  UP/DOWN 移动,OK 执行,长按 OK 返回来源
//   选项页  UP/DOWN 移动,OK 显式确认,长按 OK 菜单(返回后保留待选与光标)
//
// 线程模型:按键回调只入队;输入任务串行驱动 app;LVGL 对象只在持有
// bsp_lvgl_lock() 时改写(gal_app 内部负责)。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "gal_app.h"
#include "gal_pack.h"
#include "key_guard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "main";

#define DIM_MS 60000u
#define SCREEN_OFF_MS 300000u
#define SLEEP_MS 1800000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35
#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 40
#define INPUT_TASK_STACK 7168
#define BATTERY_PERIOD_MS 30000u

extern const lv_font_t manosaba_16;

// 资源包(CMake EMBED_FILES)。
extern const uint8_t manosaba_pack_start[] asm("_binary_manosaba_pack_start");
extern const uint8_t manosaba_pack_end[] asm("_binary_manosaba_pack_end");

static gal_pack_t s_pack;
static gal_app_t s_app;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;
static uint32_t s_last_activity_ms;
static uint32_t s_battery_accum_ms;

#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 画面缓冲(静态区,不占堆):
//   art 320x150 RGB565 = 96,000 B
//   strip 160x45 RGB565 = 14,400 B(518/256 显示)
//   scratch 150x150 RGB565 = 45,000 B(立绘解码暂存)
static uint16_t s_art_px[320 * 150];
static uint16_t s_strip_px[160 * 45];
static uint8_t s_sprite_scratch[150 * 150 * 2];

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

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep", (unsigned)(now_ms() - s_last_activity_ms));
    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }
    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(400));
    gal_app_flush(&s_app);

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
    uint32_t last_tick = now_ms();
    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const uint32_t now = now_ms();
        if (got) {
            set_backlight(BACKLIGHT_FULL);
            s_last_activity_ms = now;
            gal_app_key(&s_app, &key);
        }
        gal_app_tick(&s_app, now);

        s_battery_accum_ms += now - last_tick;
        last_tick = now;
        if (s_battery_accum_ms >= BATTERY_PERIOD_MS) {
            s_battery_accum_ms = 0;
            gal_app_set_battery(&s_app, bsp_battery_soc());
        }

        if (s_app.auto_enabled) s_last_activity_ms = now;   // 自动播放期间不算空闲
        const uint32_t idle = now - s_last_activity_ms;
        if (idle >= SLEEP_MS) {
            enter_sleep();
        } else if (idle >= SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

static void boot_error_screen(const char *message)
{
    if (!bsp_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1319), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, &manosaba_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFB4A2), LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, 280);
    lv_obj_set_pos(label, 20, 90);
    lv_label_set_text(label, message);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "《魔法少女的魔女审判》AI Passport 阅读器启动");
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到标题页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // NVS 失败只降级为“本次不保存”;绝不 nvs_flash_erase。
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s),本次运行不会保存", esp_err_to_name(nvs_err));
    }

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示不可用");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    if (bsp_lvgl_lock(-1)) {
        (void)bsp_lvgl_set_landscape(true);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    const uint32_t pack_size = (uint32_t)(manosaba_pack_end - manosaba_pack_start);
    const char *pack_err = NULL;
    if (!gal_pack_open(&s_pack, manosaba_pack_start, pack_size, &pack_err)) {
        ESP_LOGE(TAG, "资源包校验失败: %s", pack_err ? pack_err : "?");
        boot_error_screen("资源包校验失败，设备无法阅读。\n请重新烧录完整固件。");
        return;
    }
    ESP_LOGI(TAG, "资源包 %u B,场景 %u,指令 %u,图片 %u", (unsigned)pack_size,
             (unsigned)s_pack.count[GAL_SEC_SCENES], (unsigned)s_pack.count[GAL_SEC_OPS],
             (unsigned)s_pack.count[GAL_SEC_IMAGES]);

    if (!gal_app_init(&s_app, &s_pack, s_art_px, s_strip_px,
                      s_sprite_scratch, sizeof(s_sprite_scratch))) {
        ESP_LOGE(TAG, "阅读器界面初始化失败");
        boot_error_screen("阅读器界面初始化失败，请重新烧录完整固件。");
        return;
    }
    s_last_activity_ms = now_ms();
    s_battery_accum_ms = BATTERY_PERIOD_MS;
    gal_app_set_battery(&s_app, bsp_battery_soc());

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(gal_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "manosaba_input", INPUT_TASK_STACK, NULL, 5, &s_input_task) != pdPASS) {
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
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "就绪:空闲 %us 调暗 / %us 熄屏 / %us 休眠;空闲堆 %u,最大块 %u",
             (unsigned)(DIM_MS / 1000), (unsigned)(SCREEN_OFF_MS / 1000),
             (unsigned)(SLEEP_MS / 1000), (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}
