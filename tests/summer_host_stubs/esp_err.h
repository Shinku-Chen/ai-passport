#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_INVALID_LENGTH 0x1106
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1107
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1108
const char *esp_err_to_name(esp_err_t err);
