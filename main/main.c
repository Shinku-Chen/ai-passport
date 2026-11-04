// main/main.c —— 《男娘恋爱物语》AI Passport 横屏阅读器：启动、按键任务、空闲策略。
//
// 开机直接进入作品标题页（不再经过硬件自检 demo 菜单）。三键语义：
//   正文   上=推进（打字中先全显，长按=快进，松手停）  下=历史（长按=自动阅读）
//          OK=菜单
//   列表   上/下 移动，OK 确认，长按 OK 返回
//
// 线程模型：按键回调只入队；输入任务串行处理事件并驱动 nn_app；LVGL 对象只在
// 持有 bsp_lvgl_lock() 时改写。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "nn_app.h"
#include "nn_canvas.h"
#include "nn_pack.h"
#include "nn_save.h"

static const char *TAG = "nn";

// 致命错误也要给人看见：画布/界面起不来时不能默默 return 留一块黑屏。
static void show_fatal(const char *msg)
{
    if (!bsp_lvgl_lock(2000)) return;
    lv_obj_t *screen = lv_screen_active();
    if (screen) {
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x200000), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, msg);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFE0E0), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, 300);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_center(label);
    }
    bsp_lvgl_unlock();
}

// 资源包由 main/CMakeLists.txt 的 target_add_binary_data 编进 .rodata。
extern const uint8_t nn_pack_bin_start[] asm("_binary_nn_pack_bin_start");
extern const uint8_t nn_pack_bin_end[] asm("_binary_nn_pack_bin_end");

// 空闲策略：先调暗，再熄屏，最后 deep sleep（任意键唤醒后回到标题页）。
#define NN_DIM_MS 60000u
#define NN_SCREEN_OFF_MS 300000u
#define NN_SLEEP_MS 1800000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

static nn_app_t s_app;
static nn_pack_t s_pack;
static nn_key_t s_key_shim;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;
static uint32_t s_idle_ms;

#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

// 按键回调跑在 button 组件的共享 esp_timer 任务里：只入队，立刻返回。
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
    const nn_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep：先摘掉会漏电的外设，再把唤醒脚交回数字输入，最后停屏入睡。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)", (unsigned)NN_SLEEP_MS);

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    set_backlight(BACKLIGHT_FULL);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (bsp_lvgl_lock(-1)) {
        nn_app_flush(&s_app);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();

    int wake_level = 1;
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
    nn_key_t key;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (got) {
            set_backlight(BACKLIGHT_FULL);
            if (bsp_lvgl_lock(-1)) {
                nn_app_key(&s_app, &key);
                bsp_lvgl_unlock();
            }
        }
        if (bsp_lvgl_lock(-1)) {
            nn_app_tick(&s_app, elapsed_ms);
            s_idle_ms = nn_app_idle_ms(&s_app);
            bsp_lvgl_unlock();
        }

        if (s_idle_ms >= NN_SLEEP_MS) {
            enter_sleep();
        } else if (s_idle_ms >= NN_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (s_idle_ms >= NN_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "男娘恋爱物语 · 横屏阅读器启动");

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到标题页");
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // NVS 失败只降级：不擦除共享分区（会删掉别的应用的 namespace），照常可玩。
    const nn_store_state_t store = nn_store_init();
    if (store != NN_STORE_OK) {
        ESP_LOGW(TAG, "存档不可用: %s — 本次运行可继续阅读，退出后进度不保留", nn_store_reason());
    }

    // 资源包：解析失败就不再往下走，避免用空数据“假装能读”。
    const size_t pack_size = (size_t)(nn_pack_bin_end - nn_pack_bin_start);
    if (!nn_pack_init(&s_pack, nn_pack_bin_start, pack_size)) {
        ESP_LOGE(TAG, "资源包解析失败(%u 字节)", (unsigned)pack_size);
        return;
    }
    ESP_LOGI(TAG, "资源包 %u 字节: %u 场景 / %u 操作 / %u 图像",
             (unsigned)pack_size, (unsigned)s_pack.scene_count,
             (unsigned)s_pack.op_count, (unsigned)s_pack.image_count);

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    // 竖屏：素材本身就是竖屏（336x480），保持原生 240x320，不额外旋转。
    if (bsp_lvgl_lock(-1)) {
        const esp_err_t rot = bsp_lvgl_set_landscape(false);
        if (rot != ESP_OK) ESP_LOGW(TAG, "横屏切换失败(%s)", esp_err_to_name(rot));
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    if (!nn_canvas_init()) {
        // 画布（内部堆）拿不到：给出屏幕可见提示，并把剩余内存写到日志，
        // 不再默默 return（那样屏幕是全黑的，现场无法判断原因）。
        ESP_LOGE(TAG, "画布分配失败，空闲堆 %u / 最大连续块 %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                            MALLOC_CAP_INTERNAL));
        show_fatal("内存不足：画面缓冲分配失败\n请重启设备；若反复出现请重刷固件");
        return;
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 加锁失败");
        return;
    }
    const bool ui_ok = nn_app_init(&s_app, &s_pack);
    bsp_lvgl_unlock();
    if (!ui_ok) {
        ESP_LOGE(TAG, "界面初始化失败");
        show_fatal("界面初始化失败\n请重启设备；若反复出现请重刷固件");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(nn_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "nn_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
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

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:上=推进(长按快进) 下=历史(长按自动) OK=菜单;"
                  "空闲 %us 调暗 / %us 熄屏 / %us 休眠",
             (unsigned)(NN_DIM_MS / 1000), (unsigned)(NN_SCREEN_OFF_MS / 1000),
             (unsigned)(NN_SLEEP_MS / 1000));
}
