// main/main.c —— FoloToy AI Passport DLNA 投屏播放器:开机直启,无主菜单。
//
// 按键语义(开机直达 DLNA 播放器):
//   上短按/下短按  音量 + / −(见 demo_dlna_key)
//   上长按/下长按  切换播放(本地;对手机 App 的"上一首/下一首"无效)
//   确定  短按    播放 / 暂停
//   确定  长按    (本文件不再拦截返回菜单,交给 demo 页处理)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 各演示页(开机直达其中的 DLNA Player)。保留完整数组便于定位/扩展。
static const demo_entry_t DEMOS[] = {
    { "Display", demo_display_enter, demo_display_exit, demo_display_key },
    { "Button",  demo_button_enter,  demo_button_exit,  demo_button_key  },
    { "Audio",   demo_audio_enter,   demo_audio_exit,   demo_audio_key   },
    { "Battery", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "Wi-Fi",   demo_wifi_enter,    demo_wifi_exit,    demo_wifi_key    },
    { "BLE",     demo_ble_enter,     demo_ble_exit,     demo_ble_key     },
    { "Low Power", demo_low_power_enter, demo_low_power_exit, demo_low_power_key },
    { "DLNA Player", demo_dlna_enter, demo_dlna_exit, demo_dlna_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))
// 开机直达的功能页索引(DLNA Player 在 DEMOS[] 的位置)。
#define DLNA_DEMO_INDEX 7

static bool s_audio_ok;      // 音频初始化结果
static int  s_active = -1;   // 当前所在演示页(开机即 = DLNA_DEMO_INDEX)

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    if (s_active >= 0) {
        DEMOS[s_active].key(btn, ev);
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "DLNA 投屏播放器启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是 UI 载体,失败就没有界面可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 其余外设单项失败不阻塞:音频失败时播放页会显示错误。
    bool btn_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_audio_ok = (bsp_audio_init() == ESP_OK);
    bool batt_ok = (bsp_battery_init() == ESP_OK);

    // 开机直接进入 DLNA 播放器功能页,跳过主菜单。
    if (s_audio_ok && btn_ok && bsp_lvgl_lock(1000)) {
        s_active = DLNA_DEMO_INDEX;
        DEMOS[s_active].enter();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "Button=%d Audio=%d 初始化失败,无法进入 DLNA 播放页",
                 btn_ok, s_audio_ok);
    }

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             1, btn_ok, s_audio_ok, batt_ok);
}
