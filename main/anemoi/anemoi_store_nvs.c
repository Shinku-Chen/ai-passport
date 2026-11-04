// main/anemoi/anemoi_store_nvs.c —— NVS 适配器（只在固件里编译）。
//
// 约束：只打开命名空间读写；初始化失败就把持久化降级为“本次运行内有效”，
// 绝不 nvs_flash_erase()。写入成功必须 nvs_set_blob 与 nvs_commit 都成功。
#include "anemoi_store.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "anemoi_nvs";
static nvs_handle_t s_handle;
static bool s_ready;

static bool nvs_read(const char *key, void *out, size_t size, size_t *out_size)
{
    if (!s_ready) {
        return false;
    }
    size_t length = size;
    const esp_err_t err = nvs_get_blob(s_handle, key, out, &length);
    if (err != ESP_OK) {
        return false;
    }
    if (out_size) {
        *out_size = length;
    }
    return length == size;
}

static bool nvs_write(const char *key, const void *data, size_t size)
{
    if (!s_ready) {
        return false;
    }
    if (nvs_set_blob(s_handle, key, data, size) != ESP_OK) {
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

static const anem_store_io_t s_nvs_io = { .read = nvs_read, .write = nvs_write };

bool anem_store_init(void)
{
    (void)anem_store_workspace_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 不擦除：只报告并降级。
        ESP_LOGW(TAG, "NVS 不可用 (0x%x)，本次运行不落盘", (unsigned)err);
        return false;
    }
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_open("anemoi", NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败 (0x%x)", (unsigned)err);
        return false;
    }
    s_ready = true;
    anem_store_bind(&s_nvs_io);
    return true;
}
