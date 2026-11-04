// 宿主机用 nvs.h:内存里的键值表,行为与真实 NVS 的"写成功才算成功"一致。
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len);
// 宿主计数器：用来证明“标题页上下键不再读 NVS 存档”。
extern int nvs_stub_read_count;
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);

// 测试挂钩
void nvs_stub_reset(void);
void nvs_stub_set_writable(int writable);
void nvs_stub_set_fail_commit(int fail);
int nvs_stub_key_count(void);
