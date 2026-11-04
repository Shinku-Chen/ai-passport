// Host NVS stub for the CLANNAD app regression: in-memory blobs + failure injection.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t nvs_handle_t;
typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_INVALID_SIZE 0x1105
#define NVS_READONLY 0
#define NVS_READWRITE 1

esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
const char *esp_err_to_name(esp_err_t err);

// 故障注入（宿主测试专用）
extern int nvs_stub_fail_write;
extern int nvs_stub_fail_read;
extern int nvs_stub_fail_open;
void nvs_stub_reset(void);
int nvs_stub_count(const char *ns);
