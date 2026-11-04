// Host stub for esp_err.h (app/NVS regressions only).
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_INVALID_LENGTH 0x110a
static inline const char *esp_err_to_name(esp_err_t err) { (void)err; return "host"; }
