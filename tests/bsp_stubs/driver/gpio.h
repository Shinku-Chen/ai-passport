// Host-test stub for driver/gpio.h (只提供 bsp_button_prepare_deep_sleep 用到的部分)。
#pragma once
#include "esp_err.h"

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_INPUT = 0,
    GPIO_MODE_OUTPUT = 1,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ONLY = 1,
} gpio_pullup_t;

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pullup_t pull);
int gpio_get_level(gpio_num_t gpio_num);
