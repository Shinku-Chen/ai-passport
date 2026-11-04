#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

// Test hooks.
void stub_nvs_reset(void);
void stub_nvs_set_available(int available);
void stub_nvs_set_fail_set(int fail);
void stub_nvs_set_fail_commit(int fail);
void stub_nvs_set_fail_get(int fail);
int stub_nvs_key_count(void);
