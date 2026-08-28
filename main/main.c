// main/main.c —— FoloToy AI Passport DLNA 接收器应用入口。
// 开机直接进入 DLNA 接收器(无主菜单)。初始化 BSP 外设后把控制权交给 dlna_app。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "dlna_app.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "DLNA 接收器启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是 UI 载体,失败无法使用 DLNA 接收器 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,DLNA 应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键:直接绑定 DLNA 应用回调。运行于 button 组件任务,应用内部加锁。
    esp_err_t btn_err = bsp_button_init(dlna_app_on_key, NULL);
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(btn_err));
        return;   // 无输入无法操作
    }

    // 电量:失败不阻塞,但状态页可能看不到电量,记录日志。
    bsp_battery_init();

    // 交给 DLNA 应用
    dlna_app_start();
}
