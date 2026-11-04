#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef uint32_t nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
const char *esp_err_to_name(esp_err_t err);
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);
void host_nvs_set_fail(int fail);
