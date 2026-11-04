// Host stub for the NVS blob API used by steins0_app.c.
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
