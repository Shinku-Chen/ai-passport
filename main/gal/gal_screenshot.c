// main/gal/gal_screenshot.c -- FAP_SCREENSHOT_V1 serial screenshot.
//
// Ported from the Connect Four port's c4_screenshot.c, which is where the protocol,
// the static-frame trick and the failure modes were worked out (see
// docs/reference/y2lin/serial-screenshot-protocol.md). Unlike that port this one has
// no BLE link competing for RAM, so the frame buffer is always compiled in: the
// community publisher requires the protocol to answer.
#include "gal/gal_screenshot.h"

#include "bsp_display.h"
#include "bsp_pins.h"          // BSP_LCD_W / BSP_LCD_H size the frame buffer
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "gal_shot";

#define GAL_SHOT_CMD        "FAP_SCREENSHOT_V1"
#define GAL_SHOT_CMD_LEN    (sizeof(GAL_SHOT_CMD) - 1)
// One frame is the panel's whole pixel count; the value is the same in either
// orientation, so one size covers both.
#define GAL_SHOT_FRAME_MAX  ((size_t)BSP_LCD_W * (size_t)BSP_LCD_H * 2u)
#define GAL_SHOT_CHUNK      512                 // well under the 1024-byte tx ring buffer
#define GAL_SHOT_TASK_STACK 8192                // full-screen software rendering needs headroom
#define GAL_SHOT_TASK_PRIO  3                   // below the LVGL task (4)
#define GAL_SHOT_IO_WAIT_MS 200

// Reserved at compile time, 64-byte aligned: the runtime heap cannot produce a
// contiguous 150 KB block once LVGL and the rest of the application are running.
static uint8_t s_frame[GAL_SHOT_FRAME_MAX] __attribute__((aligned(64)));
static bool s_started;

static void write_all(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        const size_t n = (len > GAL_SHOT_CHUNK) ? GAL_SHOT_CHUNK : len;
        const int written = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(500));
        if (written <= 0) {
            ESP_LOGW(TAG, "serial write failed, frame abandoned (%u bytes left)",
                     (unsigned)len);
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
        ESP_LOGE(TAG, "resolution %dx%d does not fit the capture buffer", w, h);
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
        ESP_LOGE(TAG, "snapshot failed (no LVGL lock or buffer mismatch)");
        return;
    }

    // The binary window and the log share one USB-CDC stream, and the host reads
    // exactly the declared byte count, so a single interleaved log byte shifts the
    // whole image.
    esp_log_level_set("*", ESP_LOG_NONE);
    char header[64];
    const int header_len = snprintf(header, sizeof(header),
                                    GAL_SHOT_CMD " %d %d RGB565LE %u\n", w, h,
                                    (unsigned)bytes);
    write_all(header, (size_t)header_len);
    write_all(s_frame, bytes);
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "sent a %dx%d frame (%u bytes)", w, h, (unsigned)bytes);
}

// Sliding-window command match: terminal tools can swallow the trailing newline, so a
// strict line match never fires. Reset the window on any line terminator and clear it
// after a match so residual bytes cannot re-trigger a capture.
static void handle_rx(const uint8_t *data, size_t len, size_t *matched)
{
    for (size_t i = 0; i < len; i++) {
        const char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            *matched = 0;
            continue;
        }
        if (*matched < GAL_SHOT_CMD_LEN && c == GAL_SHOT_CMD[*matched]) {
            (*matched)++;
            if (*matched == GAL_SHOT_CMD_LEN) {
                *matched = 0;
                capture_and_send();
            }
        } else {
            *matched = (c == GAL_SHOT_CMD[0]) ? 1u : 0u;
        }
    }
}

static void screenshot_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    size_t matched = 0;

    for (;;) {
        const int n = usb_serial_jtag_read_bytes(rx, sizeof(rx),
                                                 pdMS_TO_TICKS(GAL_SHOT_IO_WAIT_MS));
        if (n > 0) {
            handle_rx(rx, (size_t)n, &matched);
        } else if (n < 0) {
            // Back off on error: a tight read loop starves the idle task and trips
            // the task watchdog.
            vTaskDelay(pdMS_TO_TICKS(GAL_SHOT_IO_WAIT_MS));
        }
    }
}

esp_err_t gal_screenshot_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    // The console uses the register-level VFS path by default; without installing the
    // driver, reading the command dereferences a NULL driver object.
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB-serial-JTAG driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(screenshot_task, "gal_shot", GAL_SHOT_TASK_STACK, NULL,
                    GAL_SHOT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "screenshot task creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "screenshot ready: send " GAL_SHOT_CMD " for %u bytes of RGB565LE",
             (unsigned)GAL_SHOT_FRAME_MAX);
    return ESP_OK;
}
