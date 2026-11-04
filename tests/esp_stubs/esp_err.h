// tests/esp_stubs/esp_err.h —— 宿主机测试用的最小 esp_err_t。
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)

const char *esp_err_to_name(esp_err_t err);
