// main/main.c —— 精简入口:开机直接进入「今天吃啥」,不再有主菜单。
//
// 本固件只保留一个应用(demo_eat_what),其余 BSP 演示/外设模块已移除。
// 按键语义(应用内):
//   按住上/下   循环播放对应动画,松开停住(由 demo_eat_what_key 处理)
//   长按 OK     本固件无菜单,不再返回(仅用于重置自动关机倒计时)
//
// 其他外设(音频/电量/Wi-Fi/BLE)未初始化:开机只点亮显示 + 按键,立即进吃啥。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "ui_pixel.h"
#include "autopower.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 唯一应用:开机即进入。demo_entry_t 接口保留,便于日后扩展时复用。
static const demo_entry_t APP = {
    .name = "Eat What",
    .enter = demo_eat_what_enter,
    .exit  = demo_eat_what_exit,
    .key   = demo_eat_what_key,
};

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    autopower_notify_activity();      // 任何按键都算活动,重置关机倒计时
    if (!bsp_lvgl_lock(500)) return;

    // 无菜单,长按 OK 不再返回;其余按键全部交给应用处理。
    APP.key(btn, ev);

    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "今天吃啥 精简固件启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本应用的 UI 载体,失败就没有界面可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键是唯一输入,失败则应用无法交互。
    bool btn_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    if (!btn_ok) {
        ESP_LOGW(TAG, "按键初始化失败,应用仍进入,但无法交互");
    }

    // 开机直接进入应用。
    if (bsp_lvgl_lock(1000)) { APP.enter(); bsp_lvgl_unlock(); }

    // 全局无操作自动关机(2 分钟无按键 → 深睡,GPIO0 低电平唤醒)。非致命,失败仅不启用。
    autopower_init();

    ESP_LOGI(TAG, "就绪:Display=1 Button=%d", btn_ok);
}
