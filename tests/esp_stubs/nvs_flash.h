// tests/esp_stubs/nvs_flash.h —— 只保留本应用用到的一次性初始化。
#pragma once

#include "esp_err.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
