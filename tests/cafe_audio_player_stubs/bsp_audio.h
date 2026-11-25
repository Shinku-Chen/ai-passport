#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_format(uint32_t,uint8_t,uint8_t);
void bsp_audio_set_volume(uint8_t);
esp_err_t bsp_audio_write(const void*,size_t);
