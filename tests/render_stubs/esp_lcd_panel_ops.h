// Host-test stub: 假面板把每条条带拷进一块 240x320 缓冲(按大端输入解码)。
#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end,
                              int y_end, const void *color_data);
#ifdef __cplusplus
}
#endif
