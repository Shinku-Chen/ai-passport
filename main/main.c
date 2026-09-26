// main/main.c —— 《ATRI -My Dear Moments-》AI Passport 移植:开机直接进阅读器(竖屏 240x320)。
//
// 按键语义:
//   标题/列表页 上、下移动光标,确定进入,长按确定返回上一层
//   正文页      确定/上/下 推进(打字中=立即显示全文),长按确定打开菜单
//   选项页      上、下选择,确定确认
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动应用状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写(由本文件负责加锁)。
#include "atri_app.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "main";

// 资源包由 main/CMakeLists.txt 的 EMBED_FILES 注入(见 tools/atri_pack.py)。
extern const uint8_t atri_pack_bin_start[] asm("_binary_atri_pack_bin_start");
extern const uint8_t atri_pack_bin_end[] asm("_binary_atri_pack_bin_end");
// 中文字体子集(assets/fonts/atri_cjk_16.c,由 tools/atri_font.py 生成)。
LV_FONT_DECLARE(atri_cjk_16);

// 开机直接进入的章节下标(-1 = 正常走标题页)。由 CMake 的 ATRI_BOOT_CHAPTER 传入,
// 用于真机验收某一章(例如“最后一章”):正式固件用默认值,验收时单独构建。
#ifndef ATRI_BOOT_CHAPTER
#define ATRI_BOOT_CHAPTER (-1)
#endif

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 50

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到标题页继续读)。
#define ATRI_DIM_MS 45000u
#define ATRI_SCREEN_OFF_MS 150000u
#define ATRI_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 30

// 画面区 240x210 RGB565;叠加解码缓冲由 atri_image 按需动态分配。
static uint16_t s_art_pixels[ATRI_ART_W * ATRI_ART_H];
static atri_app_t s_app;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;

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
    if (!s_input_ready || !s_input_queue) return;
    const atri_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
// 顺序与 demo/connect-four 在真机上验证过的一致。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)atri_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        // 配不上唤醒源就绝不睡下去,否则醒不过来。
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    if (bsp_lvgl_lock(300)) {
        atri_app_before_sleep(&s_app);
        atri_app_show_sleeping(&s_app);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(400));

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

static void handle_key(const atri_key_t *key)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,丢弃本次按键");
        return;
    }
    atri_app_key(&s_app, key);
    bsp_lvgl_unlock();
}

static void handle_tick(uint32_t elapsed_ms)
{
    if (!bsp_lvgl_lock(500)) return;
    atri_app_tick(&s_app, elapsed_ms);
    const bool want_sleep = atri_app_take_sleep_request(&s_app);
    bsp_lvgl_unlock();
    if (want_sleep) enter_sleep();
}

// 串口截图通道(调试用):在 USB-Serial-JTAG 控制台输入
//   ATRISHOT <章下标> <幕下标>
// 就把该幕渲染进画面区,并以 RGB565 原始字节回传:
//   ATRISHOT <w> <h> <字节数> + 原始像素 + ATRISHOT-END

// 平时它只是阻塞在一行输入上,不占 CPU;发行固件保留它便于远程验收画面。
static void debug_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int chapter = -1;
        int scene = -1;

        // ATRIJUMP <章下标> [幕下标]:把阅读进度直接拨过去(正常落盘)。
        if (sscanf(line, "ATRIJUMP %d %d", &chapter, &scene) >= 1) {
            bool jumped = false;
            if (bsp_lvgl_lock(1000)) {
                jumped = atri_app_debug_start(&s_app, (uint16_t)chapter,
                                              scene < 0 ? 0 : (uint16_t)scene);
                bsp_lvgl_unlock();
            }
            printf("ATRIJUMP-%s %d %d\n", jumped ? "OK" : "ERR", chapter, scene);
            fflush(stdout);
            continue;
        }

        if (sscanf(line, "ATRISHOT %d %d", &chapter, &scene) != 2) continue;

        bool ok = false;
        if (bsp_lvgl_lock(1000)) {
            ok = atri_app_debug_render(&s_app, (uint16_t)chapter, (uint16_t)scene);
            bsp_lvgl_unlock();
        }
        if (!ok) {
            printf("ATRISHOT-ERR %d %d\n", chapter, scene);
            fflush(stdout);
            continue;
        }
        const uint32_t total = sizeof(s_art_pixels);
        printf("ATRISHOT %d %d %u\n", ATRI_ART_W, ATRI_ART_H, (unsigned)total);
        fflush(stdout);
        // 控制台默认把输出里的 LF 翻成 CRLF,那会往原始像素里插字节、把画面打花;
        // 发像素期间关掉翻译,发完再恢复。
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
        const uint8_t *bytes = (const uint8_t *)s_art_pixels;
        for (uint32_t off = 0; off < total; off += 2048) {
            const uint32_t chunk = (total - off) < 2048u ? (total - off) : 2048u;
            fwrite(bytes + off, 1, chunk, stdout);
        }
        fflush(stdout);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
        printf("\nATRISHOT-END\n");
        fflush(stdout);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    atri_key_t key;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (got) {
            set_backlight(BACKLIGHT_FULL);
            handle_key(&key);
        }
        handle_tick(elapsed_ms);

        const uint32_t idle = atri_app_idle_ms(&s_app);
        if (idle >= ATRI_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= ATRI_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= ATRI_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport《ATRI -My Dear Moments-》阅读器启动");

    // deep sleep 唤醒会重启整个应用,把原因打出来便于确认"按键真能唤醒"。
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "按键唤醒,回到阅读器");
    } else if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "从休眠唤醒(原因 %d)", (int)wakeup);
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建(%s)", esp_err_to_name(nvs_err));
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
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
    bsp_display_backlight(BACKLIGHT_FULL);

    const uint32_t pack_size = (uint32_t)(atri_pack_bin_end - atri_pack_bin_start);
    if (bsp_lvgl_lock(-1)) {
        if (!atri_app_init(&s_app, atri_pack_bin_start, pack_size, s_art_pixels, &atri_cjk_16)) {
            ESP_LOGE(TAG, "阅读器初始化失败(资源包 %u 字节)", (unsigned)pack_size);
            bsp_lvgl_unlock();
            return;
        }
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,无法建界面");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(atri_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "atri_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,无法翻页");
        return;
    }
    s_input_ready = true;

#if ATRI_BOOT_CHAPTER >= 0
    // 真机验收用:跳过标题页,直接从指定章节开始读。
    if (bsp_lvgl_lock(1000)) {
        if (!atri_app_debug_start(&s_app, (uint16_t)ATRI_BOOT_CHAPTER, 0)) {
            ESP_LOGE(TAG, "开机进章失败:章节下标 %d 不存在", (int)ATRI_BOOT_CHAPTER);
        }
        bsp_lvgl_unlock();
    }
#endif

    if (xTaskCreate(debug_task, "atri_debug", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "串口截图任务创建失败(不影响阅读)");
    }

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL 内存池 %u 字节:已用 %u,空闲 %u(最大块 %u,碎片 %u%%)",
             (unsigned)mon.total_size, (unsigned)mon.total_size - mon.free_size,
             (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
             (unsigned)mon.frag_pct);
    ESP_LOGI(TAG, "就绪:竖屏阅读器;确定推进 / 长按确定菜单;"
                  "空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(ATRI_DIM_MS / 1000), (unsigned)(ATRI_SCREEN_OFF_MS / 1000),
             (unsigned)(ATRI_SLEEP_MS / 1000));
}
