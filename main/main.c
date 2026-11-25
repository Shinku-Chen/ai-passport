// main/main.c —— 9-nine- 春色春恋春熙风:开机直接进启动页(横屏 320x240)。
//
// 按键语义:
//   启动页    短按任意键进入标题页
//   标题/列表 上、下移动光标,确定进入,长按确定返回
//   正文页    短按 OK/上/下=推进正文(打字中先完整显示本页);长按 OK=游戏内菜单;
//             长按上=快进(按住持续,松手停);长按下=自动阅读开关
//   设置页    上、下移动,确定切换/执行,长按确定返回
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动应用状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写(由 nine_app 内部负责)。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "nine/nine_app.h"
#include "nine/nine_input.h"
#include "nine/nine_media.h"
#include "key_guard.h"
#include "lvgl.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

LV_FONT_DECLARE(nine_cjk_16);

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到启动页)。
#define GAL_DIM_MS 60000u
#define GAL_SCREEN_OFF_MS 180000u
#define GAL_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

static nine_app_t s_app;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static nine_input_t s_input;
static volatile bool s_input_ready;
static volatile bool s_input_overflow;
static uint8_t s_backlight = BACKLIGHT_FULL;

// 深睡按键唤醒的按键守卫(见 key_guard.h):唤醒键松手前吞掉按键动作。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 启动失败也必须给用户一个看得见的、使用真实中文字库的错误页。
static void show_boot_error(const char *text)
{
    bool shown = false;
    if (bsp_lvgl_lock(-1)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_t *label = lv_label_create(scr);
        lv_obj_set_style_text_font(label, &nine_cjk_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        lv_refr_now(NULL);
        bsp_lvgl_unlock();
        shown = true;
    }
    if (shown) bsp_display_backlight(BACKLIGHT_FULL);
    else ESP_LOGE(TAG, "启动失败且无法显示错误页: %s", text);
}

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
    // 先消费守卫再查就绪位:迟到的 release 若在这里被丢掉,守卫会一直吞下一次真实按键。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const nine_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    if (xQueueSend(s_input_queue, &key, 0) != pdTRUE) {
        // Callback stays nonblocking; latch the loss for safe recovery in task.
        s_input_overflow = true;
    }
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)nine_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        // 配不上唤醒源就绝不睡下去,否则醒不过来。
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 睡前把自动进度落盘(平时是 2 秒节流写)。
    nine_app_flush(&s_app);
    // 正在放的音乐/OP 先停掉,等播放任务停写 I2S 再让 codec 休眠。
    nine_media_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

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
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        if (!s_input.have_pending) {
            nine_key_t key;
            if (xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE) {
                s_input.pending = key;
                s_input.have_pending = true;
                set_backlight(BACKLIGHT_FULL);
            }
        }
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (s_input.have_pending) {
            if (!nine_input_process(&s_app, &s_input)) {
                // The key is already consumed from the queue. Retain it and
                // yield; no tick may advance FF while a release is queued.
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            // Drain already queued events (notably RELEASE) before any tick.
            if (uxQueueMessagesWaiting(s_input_queue) > 0) continue;
        }

        if (s_input_overflow) {
            s_input_overflow = false;
            nine_app_input_recover(&s_app);
            ESP_LOGW(TAG, "输入队列满,事件未入队;已停止快进/清除长按锁存");
        }
        nine_app_tick(&s_app, elapsed_ms);

        const uint32_t idle = nine_app_idle_ms(&s_app);
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
    ESP_LOGI(TAG, "AI Passport GalGame 框架骨架启动");

    // deep sleep 唤醒会重启整个应用,把原因打出来便于确认“按键真能唤醒”。
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到启动页");
        // 唤醒键此刻还按着:先挡住它的动作,直到那颗键松手(见 key_guard.h)。
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d)", (int)wakeup);
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建(%s);保留现有数据,本次以易失状态运行",
                 esp_err_to_name(nvs_err));
        // Never erase: a version mismatch must not destroy saves from a running title.
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
    // 横屏必须在 LVGL 启动后、建界面之前切换(面板 MADCTL 旋转,不占 CPU)。
    // 锁或旋转失败都不能当作横屏就绪继续跑 320x240 界面。
    bool landscape_ok = false;
    if (bsp_lvgl_lock(-1)) {
        const esp_err_t lerr = bsp_lvgl_set_landscape(true);
        if (lerr == ESP_OK) landscape_ok = true;
        else ESP_LOGE(TAG, "横屏切换失败: %s", esp_err_to_name(lerr));
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "横屏切换拿不到 LVGL 锁");
    }
    if (!landscape_ok) {
        show_boot_error("横屏初始化失败");
        return;
    }

    if (!nine_app_init(&s_app)) {
        ESP_LOGE(TAG, "阅读器初始化失败");
        show_boot_error("阅读器初始化失败");
        return;
    }
    // 先提交首帧,再开背光;避免上电白屏或背光先于画面。
    bool frame_ok = false;
    if (bsp_lvgl_lock(-1)) {
        lv_refr_now(NULL);
        frame_ok = true;
        bsp_lvgl_unlock();
    }
    if (!frame_ok) {
        ESP_LOGE(TAG, "首帧刷新拿不到 LVGL 锁");
        return;
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    nine_input_init(&s_input);
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(nine_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        nine_media_stop();
        show_boot_error("输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "nine_input", 8192, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        nine_media_stop();
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        show_boot_error("输入任务创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法操作阅读器");
        s_input_ready = false;
        vTaskDelete(s_input_task);
        s_input_task = NULL;
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        nine_media_stop();
        show_boot_error("按键初始化失败");
        return;
    }
    if (s_key_guard.armed) {
        // 唤醒键若在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:9-nine- 春色春恋春熙风;短按 OK/上/下=下一句,长按 OK=菜单,"
                  "长按上=快进,长按下=自动;空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(GAL_DIM_MS / 1000), (unsigned)(GAL_SCREEN_OFF_MS / 1000),
             (unsigned)(GAL_SLEEP_MS / 1000));
}
