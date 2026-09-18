// main/c4_screenshot.c —— FAP_SCREENSHOT_V1 串口截图实现。
#include "c4_screenshot.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#if C4_ENABLE_SCREENSHOT
static const char *TAG = "c4_shot";
#endif

#define C4_SHOT_CMD        "FAP_SCREENSHOT_V1"
#define C4_SHOT_CMD_LEN    (sizeof(C4_SHOT_CMD) - 1)
// 一帧就是面板的全部像素:横屏 320x240 与竖屏 240x320 像素数相同,故这一个尺寸通用。
#define C4_SHOT_FRAME_MAX  ((size_t)BSP_LCD_W * (size_t)BSP_LCD_H * 2u)
#define C4_SHOT_CHUNK      512                  // 远小于 1024B tx 环形缓冲
#define C4_SHOT_TASK_STACK 8192                 // 全屏软件渲染需要余量
#define C4_SHOT_TASK_PRIO  3                    // 低于 LVGL 任务(4)
#define C4_SHOT_IO_WAIT_MS 200

#if C4_ENABLE_SCREENSHOT
// 编译期静态预留:运行时堆里拿不到 150KB 连续块(见参考文档)。
static uint8_t s_frame[C4_SHOT_FRAME_MAX] __attribute__((aligned(64)));
static bool s_started;

static void write_all(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        const size_t n = (len > C4_SHOT_CHUNK) ? C4_SHOT_CHUNK : len;
        const int written = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(500));
        if (written <= 0) {
            ESP_LOGW(TAG, "串口写入失败,放弃本帧(%u 字节未发)", (unsigned)len);
            return;
        }
        p += written;
        len -= (size_t)written;
    }
}

static void capture_and_send(void)
{
    const int w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());
    const int h = (int)lv_display_get_vertical_resolution(lv_display_get_default());
    const size_t bytes = (size_t)w * (size_t)h * 2u;

    if (w <= 0 || h <= 0 || bytes > sizeof(s_frame)) {
        ESP_LOGE(TAG, "分辨率 %dx%d 超出截图缓冲", w, h);
        return;
    }

    bool rendered = false;
    if (bsp_lvgl_lock(1000)) {
        lv_draw_buf_t draw_buf;
        if (lv_draw_buf_init(&draw_buf, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565,
                             (uint32_t)w * 2u, s_frame, sizeof(s_frame)) == LV_RESULT_OK &&
            lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                        &draw_buf) == LV_RESULT_OK) {
            rendered = true;
        }
        bsp_lvgl_unlock();
    }

    if (!rendered) {
        ESP_LOGE(TAG, "快照渲染失败(拿不到 LVGL 锁或缓冲不匹配)");
        return;
    }

    // 二进制窗口内静音日志:日志与图像共用同一条 USB-CDC 流,插进一个字节就会错位。
    esp_log_level_set("*", ESP_LOG_NONE);
    char header[64];
    const int header_len = snprintf(header, sizeof(header),
                                    C4_SHOT_CMD " %d %d RGB565LE %u\n", w, h, (unsigned)bytes);
    write_all(header, (size_t)header_len);
    write_all(s_frame, bytes);
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "已发送截图 %dx%d(%u 字节)", w, h, (unsigned)bytes);
}

// 命令匹配:滑窗 + 行结束符复位,避免终端吞掉换行时永远匹配不上(见参考文档)。
static void handle_rx(const uint8_t *data, size_t len, size_t *matched)
{
    for (size_t i = 0; i < len; i++) {
        const char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            *matched = 0;
            continue;
        }
        if (*matched < C4_SHOT_CMD_LEN && c == C4_SHOT_CMD[*matched]) {
            (*matched)++;
            if (*matched == C4_SHOT_CMD_LEN) {
                *matched = 0;          // 复位,残留字节不会重复触发
                capture_and_send();
            }
        } else {
            *matched = (c == C4_SHOT_CMD[0]) ? 1u : 0u;
        }
    }
}

static void screenshot_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    size_t matched = 0;

    for (;;) {
        const int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(C4_SHOT_IO_WAIT_MS));
        if (n > 0) {
            handle_rx(rx, (size_t)n, &matched);
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(C4_SHOT_IO_WAIT_MS));   // 出错要退避,不能空转饿死 idle
        }
    }
}

#endif  // C4_ENABLE_SCREENSHOT

esp_err_t c4_screenshot_start(void)
{
#if !C4_ENABLE_SCREENSHOT
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_started) return ESP_OK;

    // 控制台默认走寄存器级 VFS 路径,不装驱动会在读命令时踩空驱动对象。
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB-serial-JTAG 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(screenshot_task, "c4_shot", C4_SHOT_TASK_STACK, NULL,
                    C4_SHOT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "截图任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "截图就绪:发 " C4_SHOT_CMD " 取一帧 %u 字节 RGB565LE",
             (unsigned)C4_SHOT_FRAME_MAX);
    return ESP_OK;
#endif  // C4_ENABLE_SCREENSHOT
}
