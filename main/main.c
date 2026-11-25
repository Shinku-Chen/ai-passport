// main/main.c —— GalGame 框架骨架:开机直接进启动页(竖屏 240x320)。
//
// 按键语义(框架规范):
//   启动页    短按确定进入标题页
//   标题/列表 上、下移动光标(到顶/到底停住),确定进入,长按确定返回
//   正文页    短按(确定/上/下)=下一句(打字中先全显);长按确定=游戏内菜单;
//             长按上=快进,松手停;长按下=自动阅读开关(任意键停)
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
#include "nine/nine_media.h"
#include "lvgl.h"
#include "gal_music.h"
#include "key_guard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到启动页)。
#define GAL_DIM_MS 60000u
#define GAL_SCREEN_OFF_MS 180000u
#define GAL_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

extern const uint8_t nine_script_start[] asm("nine_script_start");
extern const uint8_t nine_script_end[] asm("nine_script_end");
extern const uint8_t nine_images_start[] asm("nine_images_start");
extern const uint8_t nine_images_end[] asm("nine_images_end");

static nine_app_t s_app;

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
    (void)xQueueSend(s_input_queue, &key, 0);
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
    if (bsp_lvgl_lock(500)) {
        nine_app_show_sleeping(&s_app);
        bsp_lvgl_unlock();
    }
    // 两套音频路径都停掉,等播放任务停写 I2S 再让 codec 休眠。
    nine_media_stop();
    gal_music_stop();
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
    ESP_LOGI(TAG, "9-nine- 九次九日九重色 阅读器启动");

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
        ESP_LOGW(TAG, "NVS 需要重建(%s);保留现有数据,本次运行使用易失状态",
                 esp_err_to_name(nvs_err));
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
    // 横屏必须在 LVGL 就绪后、建界面之前切换(硬件 MADCTL 旋转,320x240)。
    // 锁失败或分辨率仍不是 320x240 时不能继续当作横屏阅读器启动。
    const bool lock_ok = bsp_lvgl_lock(1000);
    bool landscape_set = false;
    int screen_w = 0, screen_h = 0;
    if (lock_ok) {
        landscape_set = bsp_lvgl_set_landscape(true) == ESP_OK;
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            screen_w = lv_display_get_horizontal_resolution(disp);
            screen_h = lv_display_get_vertical_resolution(disp);
        }
        bsp_lvgl_unlock();
    }
    if (!nine_app_landscape_ok(lock_ok, landscape_set, screen_w, screen_h)) {
        ESP_LOGE(TAG, "横屏启动失败(lock=%d set=%d %dx%d),拒绝以错误方向启动",
                 (int)lock_ok, (int)landscape_set, screen_w, screen_h);
        return;
    }

    if (!nine_app_init(&s_app, nine_script_start, (size_t)(nine_script_end - nine_script_start),
                       nine_images_start, (size_t)(nine_images_end - nine_images_start))) {
        ESP_LOGE(TAG, "9-nine- 阅读器初始化失败");
        return;
    }
    // 首帧背光门:持锁 arm + 请求真实界面刷新,再等 LVGL 至少提交一次刷新。
    // 超时/锁失败保持暗屏并拒绝 ready,绝不做“无条件亮屏”兜底。
    bool first_frame = false;
    for (int attempt = 0; attempt < 3 && !first_frame; ++attempt) {
        const bool lock_ok = bsp_lvgl_lock(500);
        bool submitted = false;
        if (lock_ok) {
            bsp_lvgl_arm_first_frame();
            lv_obj_invalidate(lv_screen_active());
            bsp_lvgl_unlock();
            for (int i = 0; i < 60 && !submitted; ++i) {
                submitted = bsp_lvgl_first_frame_submitted();
                if (!submitted) vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        first_frame = nine_app_first_frame_ready(lock_ok, submitted);
        if (!first_frame) ESP_LOGW(TAG, "首帧刷新未提交(attempt %d),重试", attempt + 1);
    }
    if (!first_frame) {
        ESP_LOGE(TAG, "首帧刷新未提交,保持暗屏,不报告横屏 ready");
        return;
    }
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(nine_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败,保持暗屏");
        return;
    }
    // Init the button before the consumer task: a button failure then has no
    // task (possibly holding the LVGL lock) to delete. Callbacks are dropped
    // until s_input_ready is set after the task exists.
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,保持暗屏");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (xTaskCreate(input_task, "nine_input", 4608, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败,保持暗屏");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    s_input_ready = true;
    // All consumers are live; only now reveal the already-flushed first frame.
    bsp_display_backlight(BACKLIGHT_FULL);
    if (s_key_guard.armed) {
        // 唤醒键若在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }
    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:短按 OK/上/下=下一句,长按 OK=菜单,"
                  "按住上=快进,长按下=自动阅读;空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(GAL_DIM_MS / 1000), (unsigned)(GAL_SCREEN_OFF_MS / 1000),
             (unsigned)(GAL_SLEEP_MS / 1000));
}
