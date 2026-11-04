#pragma once
#include "esp_err.h"
typedef enum { BSP_BTN_UP = 0, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum {
    BSP_BTN_PRESS = 0,
    BSP_BTN_CLICK,
    BSP_BTN_DOUBLE,
    BSP_BTN_LONG,
    BSP_BTN_RELEASE,
} bsp_btn_ev_t;
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);
esp_err_t bsp_button_prepare_deep_sleep(int *level);
int bsp_button_read_mv(void);
