#pragma once
#include "nvs.h"
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1105
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1106
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
