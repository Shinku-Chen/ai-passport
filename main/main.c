// main/main.c —— AI Passport 四子棋:开机直接进游戏(横屏 320x240)。
//
// 按键语义:
//   设置屏   UP/DOWN 选行,OK 切换该行取值(选到 START 则开始对局)
//   对局中   UP/DOWN 左右移动落子列(横屏下 UP 在右手边,所以 UP 往右),OK 落子,
//            长按 OK 回设置菜单(本局作废)
//   分出胜负 OK 再来一局,长按 OK 回设置菜单
//
// 线程模型:按键回调只入队;输入任务串行处理事件并调用 c4_app_handle_event();
// 电脑对手在独立的低优先级 worker 里搜索,算完把结果送回同一队列。
// LVGL 对象只由输入任务在 bsp_lvgl_lock() 保护下改写。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "c4_app.h"
#include "c4_link.h"
#include "c4_screenshot.h"
#include "c4_sound.h"
#include "c4_ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg)
{
    (void)arg;
    c4_event_t event;

    for (;;) {
        if (xQueueReceive(s_input_queue, &event,
                          pdMS_TO_TICKS(C4_APP_IDLE_TICK_MS)) == pdTRUE) {
            c4_app_handle_event(&event);
        } else if (c4_app_idle_tick(C4_APP_IDLE_TICK_MS)) {
            c4_app_enter_sleep();   // 正常路径不返回
        }
    }
}

// 按键回调跑在 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;

    c4_event_t event;
    event.kind = C4_EVENT_KEY;
    event.btn = btn;
    event.ev = ev;
    event.col = 0;
    event.generation = 0;
    (void)xQueueSend(s_input_queue, &event, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 四子棋启动");

    // deep sleep 唤醒会重启应用,把唤醒原因打出来便于确认“按键真能唤醒”。
    // 再在启动末尾重复一次:串口监听在设备复活重连时会丢掉最开始几百毫秒。
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,重新开始");
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d),重新开始", (int)wakeup);
    }

    // 电池是软依赖:读不到就在界面上显示 "--"。
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C 初始化失败,电量将显示为 --");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,电量将显示为 --");
    }

    // 显示是硬依赖:没有屏幕就没有游戏。失败时打清楚引脚再退出,不做串口降级。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,游戏无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (bsp_lvgl_lock(-1)) {
        // 先切换横屏,再按 320x240 排版建屏:顺序反了会按竖屏尺寸算格子。
        (void)bsp_lvgl_set_landscape(true);
        c4_ui_build();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,无法建界面");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(c4_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "c4_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,游戏无法操作");
        return;
    }

    // 电脑对手不能就绪时游戏照样能进,只是没人陪下(界面显示 AI OFFLINE)。
    if (c4_app_start_worker(s_input_queue) != ESP_OK) {
        ESP_LOGW(TAG, "电脑对手不可用");
    }

    // 音效是软依赖:初始化失败只影响声音,不影响可玩性。
    if (!c4_sound_init()) {
        ESP_LOGW(TAG, "音效不可用,静音运行");
    }

#if C4_ENABLE_LINK
    // 联机验证构建:BLE 外设启动失败只影响联机,不影响单机玩法。
    if (c4_link_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE 联机模块启动失败");
    }
#endif

    // 串口截图也是软依赖:发布到社区时用它抓设备真实画面做封面。
    if (c4_screenshot_start() != ESP_OK) {
        ESP_LOGW(TAG, "串口截图不可用");
    }

    // 控制器就绪后再载入标题屏;最后才开闸放按键事件,避免处理到一半的初始状态。
    if (bsp_lvgl_lock(-1)) {
        c4_app_init();
        bsp_lvgl_unlock();
    }

    s_input_ready = true;

    // 唤醒原因在启动末尾再报一次:串口监听重连会丢开头。
    // RTC 记号能区分“真·deep sleep 唤醒”和“被复位/掉电打回”。
    const bool from_sleep = c4_app_take_sleep_magic();
    ESP_LOGI(TAG, "本次启动: %s,esp_sleep 原因码=%d%s",
             from_sleep ? "deep sleep 唤醒" : "冷启动/复位", (int)wakeup,
             wakeup == ESP_SLEEP_WAKEUP_GPIO ? "(GPIO)" : "");

    // 内存预算的可观测证据:C3 无 PSRAM,这里把启动后的空闲堆记进日志。
    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "就绪:横屏四子棋,上下键选列 / 确定落子 / 长按确定回设置菜单;"
                  "空闲超时自动休眠(设置屏 60s / 对局中 180s)");
}
