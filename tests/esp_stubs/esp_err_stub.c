#include "esp_err.h"
const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "OK";
    case ESP_FAIL: return "FAIL";
    case ESP_ERR_NVS_NOT_FOUND: return "NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH: return "INVALID_LENGTH";
    default: return "ERR";
    }
}
