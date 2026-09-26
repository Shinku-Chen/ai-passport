// Host-test stub: 面板句柄只需要是个不透明指针。
#pragma once
#include <stdint.h>
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct { int dummy; } esp_lcd_panel_io_event_data_t;
