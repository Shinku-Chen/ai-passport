// Host-test stub: 宿主机测试不需要注册传输完成回调。
#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>
typedef struct { bool (*on_color_trans_done)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *); } esp_lcd_panel_io_callbacks_t;
static inline int esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                            const esp_lcd_panel_io_callbacks_t *cbs,
                                                            void *user) {
    (void)io; (void)cbs; (void)user; return 0;
}
