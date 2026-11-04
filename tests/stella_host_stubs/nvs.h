#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef int nvs_handle_t;
esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
#define NVS_READONLY 0
#define NVS_READWRITE 1
