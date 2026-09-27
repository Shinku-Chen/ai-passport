// main/main.c —— limelight lemonade jam 阅读器(AI Passport,竖屏 240x214 画面区)。
//
// 界面与页面流程来自 ATRI 阅读器(main/limelight_ui.c、limelight_image.c、
// limelight_app.c),剧本/素材/存档由 limelight 数据层驱动。按键语义:
//   标题/列表页 上、下移动光标,确定进入,长按确定返回
//   正文页      上/下短按推进(打字中=显示全文),长按上快进(松手停,靠 BSP 抬起事件),
//               长按下自动阅读开关,确定打开菜单
//   选项页      上、下选择,确定确认
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动状态机;LVGL 对象只在持有
// bsp_lvgl_lock() 时改写(由本文件负责加锁)。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "limelight_app.h"
#include "limelight_inflate.h"

#include "driver/usb_serial_jtag_vfs.h"

#include <fcntl.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "main";

// 两个包由 main/CMakeLists.txt 的 EMBED_FILES 注入(见 tools/limelight_*_pack.py)。
extern const uint8_t limelight_pack_bin_start[] asm("_binary_limelight_pack_bin_start");
extern const uint8_t limelight_pack_bin_end[] asm("_binary_limelight_pack_bin_end");
extern const uint8_t limelight_script_bin_start[] asm("_binary_limelight_script_bin_start");
extern const uint8_t limelight_script_bin_end[] asm("_binary_limelight_script_bin_end");
// LVGL 中文字体子集(assets/fonts/limelight_cjk_16.c)。
LV_FONT_DECLARE(limelight_cjk_16);

// 开机直接进入的对白 id(-1 = 正常走标题页)。由 CMake 的 LIMELIGHT_BOOT_ID 传入,
// 真机验收某一章时单独构建,正式固件用默认值。
#ifndef LIMELIGHT_BOOT_ID
#define LIMELIGHT_BOOT_ID (-1)
#endif

#define INPUT_QUEUE_DEPTH 8
#define CONSOLE_QUEUE_DEPTH 4
#define INPUT_TICK_MS 40

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到标题页继续读)。
#define LIME_DIM_MS 60000u
#define LIME_SCREEN_OFF_MS 180000u
#define LIME_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

// 静态缓冲(RAM 预算见交付说明;DRAM 只有 313.8 KiB,所以每块都卡着尺寸给):
//   画面区画布 240x214  = 100.3 KiB —— 背景/CG 直接解码进它
//   立绘解码缓冲         = 46.0 KiB —— 打包器把立绘夹到 ≤23,000 像素
//   立绘遮罩 1bpp        =  3.1 KiB —— 实测最大遮罩 2,607 B(init 会自检)
//   剧本块缓存           = 20.5 KiB —— 实测单块解压上限 20,835 B
static uint16_t s_art_pixels[LIME_ART_W * LIME_ART_H] __attribute__((aligned(4)));
static uint8_t s_sprite_buffer[LIME_SPRITE_MAX_PIXELS * 2] __attribute__((aligned(4)));
static uint8_t s_mask_buffer[3200] __attribute__((aligned(4)));
static uint8_t s_script_cache[20 * 1024 + 512] __attribute__((aligned(4)));
static lime_app_t s_app;

// 验收截图:LVGL 按 40 行一条把合成结果刷进绘制缓冲,把每条抄回画布缓冲,就得到
// "画面区当前完整画面"(不额外占 RAM)。注意只覆盖画面区 214 行 —— 正文带与文字是
// LVGL 自己画的,要验证文字请看串口日志/实际屏幕。
static volatile bool s_capture_frame;
static volatile int s_capture_row_offset;   // 只要 [offset, offset+LIME_ART_H) 这段行

static QueueHandle_t s_input_queue;
static QueueHandle_t s_console_queue;      // 串口命令行(由 console_task 阻塞读入)
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    // 亮度变化记录下来:验收"自动阅读不调暗/不息屏"时直接看日志。
    ESP_LOGI(TAG, "亮度 %u%%(空闲 %u ms)", (unsigned)percent,
             (unsigned)lime_app_idle_ms(&s_app));
    bsp_display_backlight(percent);
}

static void capture_flush_event(lv_event_t *event)
{
    if (!s_capture_frame) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data) return;
    if (lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) return;

    const int32_t row_offset = s_capture_row_offset;
    const int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    const int32_t x2 = area->x2 >= LIME_ART_W ? LIME_ART_W - 1 : area->x2;
    if (x2 < x1) return;
    const size_t row_bytes = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        const int32_t target = y - row_offset;
        if (target < 0 || target >= LIME_ART_H) continue;   // 只抄这一段
        const uint8_t *src = draw_buf->data +
                             (size_t)(y - area->y1) * draw_buf->header.stride +
                             (size_t)x1 * sizeof(uint16_t);
        memcpy(s_art_pixels + (size_t)target * LIME_ART_W + x1, src, row_bytes);
    }
}

// 强制一次整屏重画,把画面区抄进画布缓冲供回传。必须在持有 LVGL 锁时调用。
// 抓一段:把屏幕上 [row_offset, row_offset+LIME_ART_H) 的行抄进画布缓冲。
// 画布只有 214 行,抓整屏 320 行要分两次(0 与 214),由上位机拼起来。
static void capture_full_frame(int row_offset)
{
    // 先把画布缓冲清零:LVGL 的刷新是分块的,某些行这次没被覆盖的话
    // 会留下上一段的旧内容,看起来就像"屏幕上出现了两遍文字"。
    memset(s_art_pixels, 0, sizeof(s_art_pixels));
    s_capture_row_offset = row_offset;
    s_capture_frame = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    s_capture_frame = false;
}

// 开机后的这段时间里丢弃按键事件。
// 原因:三个按键共用一个 ADC 阶梯,上电瞬间分压还没稳(读数约 0mV),正好落在"上"键
// 窗口里 —— 真机上表现为设备自己一路从提示页点进正文页(幽灵按键)。
#define KEY_IGNORE_BOOT_MS 1500

static int64_t s_boot_us;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    if (esp_timer_get_time() - s_boot_us < (int64_t)KEY_IGNORE_BOOT_MS * 1000) return;
    const lime_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)lime_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    if (bsp_lvgl_lock(300)) {
        lime_app_before_sleep(&s_app);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(400));

    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();

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

static void handle_key(const lime_key_t *key)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,丢弃本次按键");
        return;
    }
    lime_app_key(&s_app, key);
    bsp_lvgl_unlock();
}

static void handle_tick(uint32_t elapsed_ms)
{
    if (!bsp_lvgl_lock(500)) return;
    lime_app_tick(&s_app, elapsed_ms);
    const bool want_sleep = lime_app_take_sleep_request(&s_app);
    bsp_lvgl_unlock();
    if (want_sleep) enter_sleep();
}

// 串口调试命令(验收用;在输入任务里非阻塞处理,不再单独开一个 8KB 栈的任务):
//   LIMEPAGE [title | chapters | gallery | menu | settings | about | <对白id>]
//                                回标题页/按名字切页/指定对白并渲染
//   LIMEIMAGE                    回传画面区原始 RGB565(240x214,末尾 LIMEIMAGE-END)
//   LIMEJUMP <对白id>            直接拨进度(正常落盘)
//   LIMEAUTO [on|off]            开关自动阅读(验收不调暗/不息屏)
static void handle_console_line(char *line)
{
    if (strncmp(line, "LIMEIMAGE", 9) == 0) {
        int row_offset = 0;
        (void)sscanf(line, "LIMEIMAGE %d", &row_offset);
        // 允许 0..319 的起点:抄的时候只取 [offset, offset+LIME_ART_H) 里落在屏幕内的
        // 那部分(所以 offset=214 就是抓底部 106 行,填在缓冲的前 106 行)。
        if (row_offset < 0 || row_offset > LIME_SCREEN_H - 1) row_offset = 0;
        if (bsp_lvgl_lock(2000)) {
            capture_full_frame(row_offset);
            const uint32_t total = sizeof(s_art_pixels);
            printf("LIMEIMAGE %d %d %d %u\n", row_offset, LIME_ART_W, LIME_ART_H,
                   (unsigned)total);
            fflush(stdout);
            usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
            const uint8_t *bytes = (const uint8_t *)s_art_pixels;
            for (uint32_t off = 0; off < total; off += 2048) {
                const uint32_t chunk = (total - off) < 2048u ? (total - off) : 2048u;
                fwrite(bytes + off, 1, chunk, stdout);
            }
            fflush(stdout);
            usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
            printf("\nLIMEIMAGE-END\n");
            fflush(stdout);
            bsp_lvgl_unlock();
        } else {
            printf("LIMEIMAGE-ERR\n");
            fflush(stdout);
        }
        return;
    }
    if (strncmp(line, "LIMEPAGE", 8) == 0) {
        int id = -1;
        const bool is_title = strstr(line, "title") != NULL;
        const bool has_id = sscanf(line, "LIMEPAGE %d", &id) == 1;
        // LIMEPAGE chapters / gallery / menu / settings / about:抓图用的按名字切页。
        char page_name[16] = { 0 };
        const bool has_page = !has_id && !is_title &&
                              sscanf(line, "LIMEPAGE %15s", page_name) == 1;
        if (bsp_lvgl_lock(2000)) {
            if (has_page) {
                (void)lime_app_debug_page(&s_app, page_name);
            } else if (is_title) {
                (void)lime_app_debug_title(&s_app);
            } else if (has_id) {
                (void)lime_app_debug_start(&s_app, (uint32_t)id);
            }
            lv_obj_update_layout(lv_screen_active());
            bsp_lvgl_unlock();
        }
        printf("LIMEPAGE-OK %s %d\n",
               has_page ? page_name : (is_title ? "title" : "id"),
               has_page || is_title ? -1 : id);
        fflush(stdout);
        return;
    }
    if (strncmp(line, "LIMEAUTO", 8) == 0) {
        // 开关自动阅读,顺便打印当前空闲毫秒数(验收自动模式不熄屏用)。
        const bool on = strstr(line, "off") == NULL;
        if (bsp_lvgl_lock(2000)) {
            lime_app_debug_set_auto(&s_app, on);
            bsp_lvgl_unlock();
        }
        printf("LIMEAUTO-OK %s\n", on ? "on" : "off");
        fflush(stdout);
        return;
    }
    if (strncmp(line, "LIMEBTN", 7) == 0) {
        // 诊断用:打印按键 ADC 的当前电压与状态(幽灵按键通常就是这里读数贴边)。
        const int mv = bsp_button_read_mv();
        printf("LIMEBTN mv=%d state=%s\n", mv,
               mv < 0 ? "读失败" : (mv > 1900 ? "松开" : "按下"));
        fflush(stdout);
        return;
    }
    int id = -1;
    if (sscanf(line, "LIMEJUMP %d", &id) == 1 && id > 0) {
        bool jumped = false;
        if (bsp_lvgl_lock(1000)) {
            jumped = lime_app_debug_start(&s_app, (uint32_t)id);
            bsp_lvgl_unlock();
        }
        printf("LIMEJUMP-%s %d\n", jumped ? "OK" : "ERR", id);
        fflush(stdout);
    }
}

// 串口命令只在验收/调试时需要。分两个上下文跑:
//   console_task 只做"阻塞读一行 + 入队"(小栈),
//   input_task(8KB 栈)取出后执行 —— 抓帧要整屏重画,必须在大栈里跑。
// 不用 O_NONBLOCK 单任务轮询:USB-Serial-JTAG 的 VFS 不遵守它,read 会把任务卡死
// (真机现象:设备活着,但按键与命令全无响应)。
static void console_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        (void)xQueueSend(s_console_queue, line, 0);   // 队列满就丢弃,不阻塞
    }
}

static void input_task(void *arg)
{
    (void)arg;
    lime_key_t key;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        const bool got = xQueueReceive(s_input_queue, &key, pdMS_TO_TICKS(INPUT_TICK_MS)) == pdTRUE;

        // 串口命令:由 console_task 读好放在队列里。
        char console[64];
        if (s_console_queue && xQueueReceive(s_console_queue, console, 0) == pdTRUE) {
            handle_console_line(console);
        }
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
        last_us = now_us;

        if (got) {
            // 每个按键事件记一条日志(含当时 ADC 电压):真机上排查按键问题就看这里。
            ESP_LOGI(TAG, "key btn=%u ev=%u mv=%d", (unsigned)key.btn, (unsigned)key.ev,
                     bsp_button_read_mv());
            set_backlight(BACKLIGHT_FULL);
            handle_key(&key);
        }
        handle_tick(elapsed_ms);

        const uint32_t idle = lime_app_idle_ms(&s_app);
        if (idle >= LIME_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= LIME_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= LIME_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    s_boot_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AI Passport limelight lemonade jam 阅读器启动");

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

    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败,电量显示 --");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败,电量显示 --");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(BACKLIGHT_FULL);

    lv_display_add_event_cb(lv_display_get_default(), capture_flush_event, LV_EVENT_FLUSH_START,
                            NULL);

    // 本应用不用音频,但 ES8311 停在上电默认状态会有可听底噪:正常初始化后静音。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频 codec 初始化失败,底噪可能仍在");
    } else if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "音频 codec 静音失败,底噪可能仍在");
    }

    const uint32_t pack_size = (uint32_t)(limelight_pack_bin_end - limelight_pack_bin_start);
    const uint32_t script_size =
        (uint32_t)(limelight_script_bin_end - limelight_script_bin_start);
    ESP_LOGI(TAG, "资源包 %u 字节,剧本包 %u 字节", (unsigned)pack_size, (unsigned)script_size);

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,无法建界面");
        return;
    }
    const bool ok = lime_app_init(&s_app, limelight_script_bin_start, script_size,
                                  limelight_pack_bin_start, pack_size, s_art_pixels,
                                  s_sprite_buffer, sizeof(s_sprite_buffer), s_mask_buffer,
                                  sizeof(s_mask_buffer), s_script_cache, sizeof(s_script_cache),
                                  &limelight_cjk_16);
    bsp_lvgl_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "阅读器初始化失败");
        return;
    }
    // 剧本块的解压器在这里挂上(ROM tinfl;宿主测试用 --stored 包不需要它)。
    lime_script_set_inflate(&s_app.script, lime_inflate_raw);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(lime_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    s_console_queue = xQueueCreate(CONSOLE_QUEUE_DEPTH, 64);
    if (!s_console_queue) {
        ESP_LOGW(TAG, "串口命令队列创建失败(不影响阅读)");
    } else if (xTaskCreate(console_task, "lime_con", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "串口读任务创建失败(不影响阅读)");
        vQueueDelete(s_console_queue);
        s_console_queue = NULL;
    }
    if (xTaskCreate(input_task, "lime_input", 8192, NULL, 5, &s_input_task) != pdPASS) {
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

#if LIMELIGHT_BOOT_ID >= 0
    if (bsp_lvgl_lock(1000)) {
        if (!lime_app_debug_start(&s_app, (uint32_t)LIMELIGHT_BOOT_ID)) {
            ESP_LOGE(TAG, "开机进句失败:id %d 不存在", (int)LIMELIGHT_BOOT_ID);
        }
        bsp_lvgl_unlock();
    }
#endif

    ESP_LOGI(TAG, "空闲堆 %u 字节,最大连续块 %u 字节", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL 内存池 %u 字节:已用 %u,空闲 %u(最大块 %u,碎片 %u%%)",
             (unsigned)mon.total_size, (unsigned)mon.total_size - mon.free_size,
             (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
             (unsigned)mon.frag_pct);
    ESP_LOGI(TAG, "就绪:上/下推进,长按上快进,长按下自动阅读,确定菜单;"
                  "空闲 %us 调暗,%us 熄屏,%us 休眠",
             (unsigned)(LIME_DIM_MS / 1000), (unsigned)(LIME_SCREEN_OFF_MS / 1000),
             (unsigned)(LIME_SLEEP_MS / 1000));
}
