// main/main.c —— 对讲机应用入口。
//
// 这是派生应用:界面、状态机、协议都在 oc_* 模块里自有实现,不再使用基线 demo 菜单
// (基线 demo_*.c 保留在仓库里作为硬件验证页,但启动路径不再经过它们)。
//
// 启动顺序:共享 I2C → 屏幕/LVGL → 按键 → 对讲机应用。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "oc_app.h"

static const char *TAG = "main";

// 按键回调运行在共享 esp_timer 任务里:只允许轻量入队(见 oc_app_key)。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    oc_app_key((int)btn, (int)ev);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 对讲机启动(固件 %s)", OC_APP_VERSION);
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是应用唯一的交互面:初始化失败就没有可用的对讲机,打清楚接线日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);   // 之后由 oc_ui 按设置接管

    esp_err_t e = bsp_button_init(on_key, NULL);
    if (e != ESP_OK) {
        // 没有按键仍然可以显示(能看到配对码/回复),但无法说话,必须让人看出来。
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(e));
    }

    e = oc_app_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(e));
    }
}
