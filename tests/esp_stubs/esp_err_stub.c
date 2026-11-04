// tests/esp_stubs/esp_err_stub.c —— esp_err_t 名称表。
#include "esp_err.h"

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case 0x1102: return "ESP_ERR_NVS_NOT_FOUND";
    case 0x1105: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case 0x1110: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case 0x110C: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "ESP_ERR_UNKNOWN";
    }
}
