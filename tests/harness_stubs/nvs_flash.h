// tests/harness_stubs/nvs_flash.h —— 宿主回归用的 NVS 初始化声明。
#pragma once

#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_flash_deinit(void);

#ifdef __cplusplus
}
#endif
