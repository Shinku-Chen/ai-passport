// main/eden_store_nvs.c —— eden_store_io_t 的 NVS 适配器 + 分区初始化。
//
// 两条硬规则:
//   1. 写入必须把 nvs_commit() 的返回值一起算进去 —— 提交失败就是没写进去,
//      调用方(存档页)据此提示失败,不能显示"已保存";
//   2. 初始化**不擦除**任何东西。NVS 分区里除了本应用,还可能躺着别的命名空间
//      (旧固件、别的应用的存档);擦整片是数据丢失,不是"可接受的恢复路径"。
//      分区不可用时降级:本次运行内照常阅读,写入失败如实上报。
#include "eden_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "eden_nvs";

// 状态标记:分区是否可用。不可用时读返回 false(上层按"没有记录"),写返回 false。
static bool s_ready;

static bool nvs_read(const char *ns, const char *key, void *out, size_t size)
{
    if (!s_ready) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t length = size;
    const esp_err_t err = nvs_get_blob(handle, key, out, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;   // 没有记录 / 键不存在,都按"无记录"处理
    }
    if (length != size) {
        ESP_LOGW(TAG, "%s/%s 长度 %u,期望 %u,按无记录处理", ns, key, (unsigned)length,
                 (unsigned)size);
        return false;
    }
    return true;
}

static bool nvs_write(const char *ns, const char *key, const void *data, size_t size)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "%s/%s 未写入:NVS 不可用", ns, key);
        return false;
    }
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(ns, NVS_READWRITE, &handle);
    if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "%s 打开失败 err=%d", ns, (int)open_err);
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) {
        // 提交才是真正落盘;忽略它的返回值会让"保存成功"变成谎报。
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s/%s 写入失败 err=%d", ns, key, (int)err);
        return false;
    }
    return true;
}

const eden_store_io_t eden_store_nvs_io = {
    .read = nvs_read,
    .write = nvs_write,
};

bool eden_store_init(void)
{
    const esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        s_ready = true;
        return true;
    }
    // 不擦除:分区里可能有别的命名空间的数据。降级为"不持久化",阅读功能不受影响。
    ESP_LOGW(TAG, "NVS 不可用(%s);本次运行不持久化,存档页会如实报失败",
             esp_err_to_name(err));
    s_ready = false;
    return false;
}
