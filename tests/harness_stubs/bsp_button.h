// tests/harness_stubs/bsp_button.h —— 与 components/bsp/include/bsp_button.h 的
// 枚举取值保持一致（宿主回归用它编译真实 nn_app.c 的 _Static_assert）。
#pragma once

typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,
    BSP_BTN_CLICK,
    BSP_BTN_DOUBLE,
    BSP_BTN_LONG,
    BSP_BTN_RELEASE,
} bsp_btn_ev_t;
