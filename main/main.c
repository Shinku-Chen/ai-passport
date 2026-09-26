// main/main.c —— 《飞鸟会长不肯认输》AI Passport 移植版。
//
// 这是一个派生应用:它自己实现全部界面,不复用参考示例的测试菜单、测试页或
// ui_pixel 外壳(BSP 驱动与并发模式仍然复用)。启动直接进入作品标题画面。
//
// 按键语义:
//   上/下 短按   列表/菜单中=移动选中项;阅读中=滚动对白文字
//   确定  短按   列表/菜单中=进入选中项;阅读中=推进对白(打字中则立即显示全句)
//   确定  长按   阅读中=呼出菜单;菜单中=返回上一层
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "gal/gal_app.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

// 按键事件只做入队。回调运行在共享的 esp_timer 任务上,不能阻塞、不能碰 LVGL。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            // gal_app_key 内部自己取 LVGL 锁,慢操作都在 LVGL 定时器里。
            gal_app_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "gal_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "飞鸟会长不肯认输 —— AI Passport 移植版启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是作品的唯一输出载体,没有它就无法继续 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (input_dispatch_init() != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败");
        return;
    }
    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        return;
    }

    // 电量计缺失只影响右上角读数,不阻塞阅读(gal_app 会降级显示 "--%")。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,电量显示降级");
    }

    // 本作品无音频,不初始化 codec,避免无谓的功耗。
    if (bsp_lvgl_lock(1000)) {
        gal_app_start();
        bsp_lvgl_unlock();
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 锁定失败,界面未建立");
    }

    ESP_LOGI(TAG, "就绪");
}
