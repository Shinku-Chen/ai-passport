// main/autopower.c —— 全局「无操作自动深睡断电」实现。
#include "autopower.h"
#include "bsp_display.h"
#include "ui_autopower_math.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include <stdint.h>

static const char *TAG = "autopower";

// 周期性检查间隔(毫秒)。比超时阈值小得多即可,这里取 30 s。
#define AUTOPOWER_CHECK_INTERVAL_MS  (30 * 1000)

// 板级按键 ADC 节点对应 GPIO0(见 bsp_pins.h:BSP_BTN_ADC_CHANNEL = ADC_CHANNEL_0)。
// 深睡唤醒必须用数字 RTC GPIO,C3 上 GPIO0 满足且非 strapping。
#define AUTOPOWER_WAKEUP_GPIO       GPIO_NUM_0
#define AUTOPOWER_WAKEUP_MASK       (1ULL << AUTOPOWER_WAKEUP_GPIO)

// 最后一次按键活动的时间(esp_timer 单调时钟,毫秒)。初始化为「现在」,
// 保证启动后立即有完整 2 分钟窗口。
static int64_t s_last_activity_ms;
static esp_timer_handle_t s_timer;

// 刷新最后活动时间(可在任意上下文调用;原子写 int64)。
void autopower_notify_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

// 配 GPIO0 为数字输入并启用低电平唤醒,随后进入深睡。
// 板级外部 10k 上拉维持常态高,按下拉低即触发唤醒。
static void do_sleep(void)
{
    ESP_LOGW(TAG, "无操作超时,进入深睡(GPIO0 低电平唤醒)");
    bsp_display_backlight(0);          // 关背光,省电且避免屏幕长亮

    const gpio_config_t io = {
        .pin_bit_mask = AUTOPOWER_WAKEUP_MASK,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // 用板级外部 10k 上拉,避免内外部叠加
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      // 数字中断非必需,唤醒走 RTC 电平
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 输入配置失败,放弃深睡");
        bsp_display_backlight(100);
        return;
    }
    if (esp_deep_sleep_enable_gpio_wakeup(AUTOPOWER_WAKEUP_MASK,
                                          ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 唤醒使能失败,放弃深睡");
        bsp_display_backlight(100);
        return;
    }

    // 该函数 noreturn;正常流程到此便不再返回(唤醒后由 ROM 引导重启到 app_main)。
    esp_deep_sleep_start();
}

void autopower_sleep_now(void)
{
    do_sleep();
}

// esp_timer 周期回调:检查无操作时长,超时则关机。
// 运行在 esp_timer 上下文,不做重活、不碰 LVGL 对象。
static void check_idle(void *arg)
{
    (void)arg;
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t idle_ms = now_ms - s_last_activity_ms;
    if (ui_autopower_idle_expired(idle_ms)) {
        do_sleep();
    }
}

bool autopower_init(void)
{
    autopower_notify_activity();          // 启动窗口从此刻起算

    const esp_timer_create_args_t args = {
        .callback = check_idle,
        .name = "autopower",
        .dispatch_method = ESP_TIMER_TASK,   // 独立任务,避免阻塞 esp_timer 主上下文
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer 创建失败,自动关机不可用");
        return false;
    }
    esp_timer_start_periodic(s_timer, AUTOPOWER_CHECK_INTERVAL_MS * 1000ULL);
    ESP_LOGI(TAG, "无操作 %u ms 自动深睡已启用",
             (unsigned)AUTOPOWER_IDLE_TIMEOUT_MS);
    return true;
}
