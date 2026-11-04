// tests/harness_stubs/nvs.h —— 宿主回归用的 NVS 类型/声明（实现由 harness 提供）。
//
// 真实固件用 ESP-IDF 的 nvs.h；宿主上由每个 harness 自己实现一套**内存 NVS**，
// 并可在 nvs_set_blob / nvs_commit / nvs_get_blob / nvs_flash_init 上注入失败。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE } nvs_open_mode_t;

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x1106
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110D
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);

const char *esp_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
