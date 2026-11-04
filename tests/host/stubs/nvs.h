#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 0x1100
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1103
#define ESP_ERR_NVS_INVALID_LENGTH 0x1104
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

// ---- 测试注入 ----
enum { HOST_NVS_OK = 0, HOST_NVS_FAIL_WRITE = 1, HOST_NVS_OPEN_FAIL = 2 };
void host_nvs_reset(void);
void host_nvs_set_fail(int mode);
void host_nvs_set_init_ok(int ok);
int host_nvs_put(const char *ns, const char *key, const void *data, size_t len);
int host_nvs_get(const char *ns, const char *key, void *data, size_t cap, size_t *out_len);
int host_nvs_count(const char *ns);
void host_nvs_corrupt(const char *ns, const char *key, unsigned seed);
