// tests/app_stubs/bsp_button.h —— 宿主机测试用的按键枚举。
#pragma once

#include <stdint.h>

typedef enum { BSP_BTN_UP = 0, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,
    BSP_BTN_CLICK,
    BSP_BTN_DOUBLE,
    BSP_BTN_LONG,
    BSP_BTN_RELEASE,
} bsp_btn_ev_t;

typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
