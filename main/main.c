// main/main.c —— 《魔女的夜宴》AI Passport 阅读器:开机直接进阅读器(竖屏 240x320)。
//
// 界面与页面流程沿用同一套阅读器骨架(main/sanoba_ui.c、sanoba_image.c、sanoba_app.c),
// 剧情/资源包/存档由 Sanoba 数据层(main/sanoba_model.c、sanoba_pack.c、sanoba_save.c)
// 驱动。按键语义:
//   标题/列表页 上、下移动光标,确定进入,长按确定返回
//   正文页      上/下短按推进,长按上快进(松手停),长按下自动阅读,确定打开菜单
//   选项页      上、下选择,确定确认
//
// 线程模型:按键回调只入队;输入任务串行处理事件并驱动应用状态机;
// LVGL 对象只在持有 bsp_lvgl_lock() 时改写(由本文件负责加锁)。
#include "sanoba_app.h"
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

// 资源包由 main/CMakeLists.txt 的 EMBED_FILES 注入(见 tools/sanoba_pack.py、
// tools/sanoba_scn_pack.py):图片包(SANOBPK1)+ 剧本包(SANOSCN1)。
extern const uint8_t sanoba_pack_bin_start[] asm("_binary_sanoba_pack_bin_start");
extern const uint8_t sanoba_pack_bin_end[] asm("_binary_sanoba_pack_bin_end");
extern const uint8_t sanoba_scn_bin_start[] asm("_binary_sanoba_scn_bin_start");
extern const uint8_t sanoba_scn_bin_end[] asm("_binary_sanoba_scn_bin_end");
// LVGL 中文字体子集(assets/fonts/sanoba_cjk_16.c,由 tools/sanoba_lvgl_font.py 生成)。
LV_FONT_DECLARE(sanoba_cjk_16);

// 开机直接进入的章号(-1 = 正常走标题页;0 = 从剧本开头)。由 CMake 的
// SANOBA_BOOT_SCENARIO 传入,用于真机验收某一章:正式固件用默认值,验收时单独构建。
#ifndef SANOBA_BOOT_SCENARIO
#define SANOBA_BOOT_SCENARIO (-1)
#endif

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TICK_MS 40

// 空闲策略:先调暗,再熄屏,最后 deep sleep(任意键唤醒后回到标题页继续读)。
#define SANOBA_DIM_MS 60000u
#define SANOBA_SCREEN_OFF_MS 180000u
#define SANOBA_SLEEP_MS 420000u
#define BACKLIGHT_FULL 100
#define BACKLIGHT_DIM 35

// 画面区 240x320 RGB565 画布(整屏合成,背景 JPEG 直接解码进这块缓冲)。
// ⚠ 它同时是 LVGL canvas 的像素缓冲,必须与 LVGL 生命周期一致,所以静态分配。
static uint16_t s_art_pixels[SANOBA_ART_W * SANOBA_ART_H] __attribute__((aligned(4)));
static sanoba_app_t s_app;

// 整屏验收截图:LVGL 是按 40 行一条把合成结果刷进绘制缓冲的,把每条都抄回画布缓冲,
// 画布缓冲就变成了"当前屏幕上看到的完整画面"(不额外占 RAM,不需要色键)。
// 开这个开关后下一次完整重画会被整帧抄下来;抄写发生在圆角适配之后、字节交换之前,
// 所以存下来的就是 LVGL 原生小端 RGB565。
static volatile bool s_capture_frame;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_debug_ready;
static volatile bool s_input_ready;
static uint8_t s_backlight = BACKLIGHT_FULL;

static void set_backlight(uint8_t percent)
{
    if (s_backlight == percent) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

// 刷屏钩子:把每一条已合成的像素抄进画布缓冲(见 s_capture_frame 说明)。
static void capture_flush_event(lv_event_t *event)
{
    if (!s_capture_frame) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data) return;
    if (lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) return;

    const int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    const int32_t x2 = area->x2 >= SANOBA_ART_W ? SANOBA_ART_W - 1 : area->x2;
    if (x2 < x1) return;
    const size_t row_bytes = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= SANOBA_ART_H) continue;
        const uint8_t *src = draw_buf->data +
                             (size_t)(y - area->y1) * draw_buf->header.stride +
                             (size_t)x1 * sizeof(uint16_t);
        memcpy(s_art_pixels + (size_t)y * SANOBA_ART_W + x1, src, row_bytes);
    }
}

// 强制一次整屏重画,把当前屏幕画面抄进画布缓冲供回传。必须在持有 LVGL 锁时调用。
static void capture_full_frame(void)
{
    s_capture_frame = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    s_capture_frame = false;
}

// 按键回调跑在 button 组件的共享 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const sanoba_key_t key = { .btn = (uint8_t)btn, .ev = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &key, 0);
}

// deep sleep:先摘掉会漏电的外设,再把唤醒脚交回数字输入,最后停屏入睡。
static void enter_sleep(void)
{
    ESP_LOGI(TAG, "空闲 %u ms,进入 deep sleep(任意按键唤醒)",
             (unsigned)sanoba_app_idle_ms(&s_app));

    const esp_err_t wake_err =
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        // 配不上唤醒源就绝不睡下去,否则醒不过来。
        ESP_LOGE(TAG, "按键唤醒配置失败(%s),保持唤醒", esp_err_to_name(wake_err));
        return;
    }

    if (bsp_lvgl_lock(300)) {
        sanoba_app_before_sleep(&s_app);
        sanoba_app_show_sleeping(&s_app);
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

static void handle_key(const sanoba_key_t *key)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,丢弃本次按键");
        return;
    }
    sanoba_app_key(&s_app, key);
    bsp_lvgl_unlock();
}

static void handle_tick(uint32_t elapsed_ms)
{
    if (!bsp_lvgl_lock(500)) return;
    sanoba_app_tick(&s_app, elapsed_ms);
    const bool want_sleep = sanoba_app_take_sleep_request(&s_app);
    bsp_lvgl_unlock();
    if (want_sleep) enter_sleep();
}

// 串口截图通道(调试用):在 USB-Serial-JTAG 控制台输入
//   SANOBASHOT <章号> [<句号>]
// 就把该处的画面区渲染出来,并以 RGB565 原始字节回传:
//   SANOBASHOT <w> <h> <字节数> + 原始像素 + SANOBASHOT-END
// 章号从 1 起(源数据 CHAPTERx-y 的 x),句号是该章之后的第几句正文(0 起)。
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
        // 任务在 app_main 最早创建(那时堆最空,才拿得到 8 KB 连续栈),
        // 界面还没建好时收到命令就回 ERR,不要去碰未初始化的应用状态。
        if (!s_debug_ready) {
            if (strncmp(line, "SANOBAPAGE", 10) == 0 || strncmp(line, "SANOBASHOT", 10) == 0) {
                printf("SANOBAPAGE-ERR\n");
            } else if (strncmp(line, "SANOBAJUMP", 10) == 0) {
                printf("SANOBAJUMP-ERR -1 -1\n");
            } else {
                continue;
            }
            fflush(stdout);
            continue;
        }
        int chapter = -1;
        int scene = -1;

        // SANOBAPAGE [title | <章号> <句号>]:把整屏(**画面区 + LVGL 画的文字/名牌/列表**)快照回传。
        // 做法:强制一次整屏重画,capture_flush_event 把每条合成结果抄进画布缓冲——
        // 回传的就是真正上屏的那一张画面,不需要第二块 150KB 缓冲、也不需要色键。
        // 回传期间一直持 LVGL 锁:~13s 内屏幕不会变(用户按键也不生效),否则翻页会把
        // 画布缓冲重写一遍,回传出去的字节就是两次画面的混合。
        if (strncmp(line, "SANOBAPAGE", 10) == 0) {
            const bool is_title = strstr(line, "title") != NULL;
            if (bsp_lvgl_lock(2000)) {
                if (is_title) {
                    (void)sanoba_app_debug_title(&s_app);
                } else if (sscanf(line, "SANOBAPAGE %d %d", &chapter, &scene) == 2) {
                    (void)sanoba_app_debug_start(&s_app, (uint16_t)chapter, (uint16_t)scene);
                }
                lv_obj_update_layout(lv_screen_active());
                capture_full_frame();

                const uint32_t total = sizeof(s_art_pixels);
                printf("SANOBAPAGE %d %d %u\n", SANOBA_ART_W, SANOBA_ART_H, (unsigned)total);
                fflush(stdout);
                usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
                const uint8_t *bytes = (const uint8_t *)s_art_pixels;
                for (uint32_t off = 0; off < total; off += 2048) {
                    const uint32_t chunk = (total - off) < 2048u ? (total - off) : 2048u;
                    fwrite(bytes + off, 1, chunk, stdout);
                }
                fflush(stdout);
                usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
                printf("\nSANOBAPAGE-END\n");
                fflush(stdout);

                // 画布缓冲现在装的是"带文字的整屏画面";重画一次这一幕,让它回到干净的画面内容。
                if (is_title) {
                    (void)sanoba_app_debug_title(&s_app);
                } else if (chapter >= 0) {
                    (void)sanoba_app_debug_start(&s_app, (uint16_t)chapter,
                                               scene < 0 ? 0 : (uint16_t)scene);
                }
                bsp_lvgl_unlock();
            } else {
                printf("SANOBAPAGE-ERR\n");
                fflush(stdout);
            }
            continue;
        }

        // SANOBAJUMP <章号> [<句号>]:把阅读进度直接拨过去(正常落盘)。
        if (sscanf(line, "SANOBAJUMP %d %d", &chapter, &scene) >= 1) {
            bool jumped = false;
            if (bsp_lvgl_lock(1000)) {
                jumped = sanoba_app_debug_start(&s_app, (uint16_t)chapter,
                                              scene < 0 ? 0 : (uint16_t)scene);
                bsp_lvgl_unlock();
            }
            printf("SANOBAJUMP-%s %d %d\n", jumped ? "OK" : "ERR", chapter, scene);
            fflush(stdout);
            continue;
        }

        if (sscanf(line, "SANOBASHOT %d %d", &chapter, &scene) != 2) continue;

        bool ok = false;
        if (bsp_lvgl_lock(1000)) {
            ok = sanoba_app_debug_render(&s_app, (uint16_t)chapter, (uint16_t)scene);
            bsp_lvgl_unlock();
        }
        if (!ok) {
            printf("SANOBASHOT-ERR %d %d\n", chapter, scene);
            fflush(stdout);
            continue;
        }
        const uint32_t total = sizeof(s_art_pixels);
        printf("SANOBASHOT %d %d %u\n", SANOBA_ART_W, SANOBA_ART_H, (unsigned)total);
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
        printf("\nSANOBASHOT-END\n");
        fflush(stdout);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    sanoba_key_t key;
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

        const uint32_t idle = sanoba_app_idle_ms(&s_app);
        if (idle >= SANOBA_SLEEP_MS) {
            enter_sleep();
        } else if (idle >= SANOBA_SCREEN_OFF_MS) {
            set_backlight(0);
        } else if (idle >= SANOBA_DIM_MS) {
            set_backlight(BACKLIGHT_DIM);
        }
    }
}

void app_main(void)
{
    // 调试图任务在最早创建:它要 8 KB 连续栈,而读器初始化完以后最大连续空闲块只剩
    // 7.6 KB 左右(实测创建失败),只能抢在 LVGL/画布/UI 之前要内存。
    // 4 KB 的栈不够一次整屏重画(LVGL 绘图层会递归到绘制原语,实测触发 Stack
    // protection fault 重启),所以给 8 KB。
    if (xTaskCreate(debug_task, "sanoba_debug", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "串口截图任务创建失败(不影响阅读)");
    }

    ESP_LOGI(TAG, "AI Passport《魔女的夜宴》阅读器启动");

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

    // 截图钩子挂在圆角适配回调之后(那是 BSP 注册的),拿到的是圆角处理过、
    // 字节交换之前的 LVGL 原生 RGB565。
    lv_display_add_event_cb(lv_display_get_default(), capture_flush_event, LV_EVENT_FLUSH_START,
                            NULL);

    // 本应用不使用音频,但 ES8311 停在【上电默认状态】会有可听见的底噪(待机蜂鸣):
    // 先按 BSP 正常流程初始化,再走完整的 suspend 序列把它静音。
    // (bsp_audio_sleep() 在从未初始化时是空操作,所以必须成对调用。)
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频 codec 初始化失败,底噪可能仍在");
    } else if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "音频 codec 静音失败,底噪可能仍在");
    }

    const uint32_t pack_size = (uint32_t)(sanoba_pack_bin_end - sanoba_pack_bin_start);
    const uint32_t scn_size = (uint32_t)(sanoba_scn_bin_end - sanoba_scn_bin_start);
    if (bsp_lvgl_lock(-1)) {
        if (!sanoba_app_init(&s_app, sanoba_pack_bin_start, pack_size, sanoba_scn_bin_start, scn_size,
                           s_art_pixels, &sanoba_cjk_16)) {
            ESP_LOGE(TAG, "阅读器初始化失败(图片包 %u 字节 / 剧本包 %u 字节)", (unsigned)pack_size,
                     (unsigned)scn_size);
            bsp_lvgl_unlock();
            return;
        }
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "拿不到 LVGL 锁,无法建界面");
        return;
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(sanoba_key_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "sanoba_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
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

#if SANOBA_BOOT_SCENARIO >= 0
    // 真机验收用:跳过标题页,直接从指定章(1 起)开始读。
    if (bsp_lvgl_lock(1000)) {
        if (!sanoba_app_debug_start(&s_app, (uint16_t)SANOBA_BOOT_SCENARIO, 0)) {
            ESP_LOGE(TAG, "开机进章失败:第 %d 章不存在", (int)SANOBA_BOOT_SCENARIO);
        }
        bsp_lvgl_unlock();
    }
#endif

    // 调试图任务已在 app_main 开头创建(那时堆最空),这里只把就绪标志拉起来。
    s_debug_ready = true;

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
             (unsigned)(SANOBA_DIM_MS / 1000), (unsigned)(SANOBA_SCREEN_OFF_MS / 1000),
             (unsigned)(SANOBA_SLEEP_MS / 1000));
}
