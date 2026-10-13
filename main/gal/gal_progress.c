// main/gal/gal_progress.c —— 阅读进度的 NVS 持久化实现。
#include "gal_progress.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_prog";

#define GAL_PROGRESS_NAMESPACE "galprog"
#define GAL_PROGRESS_KEY "pos"
#define GAL_PROGRESS_MAGIC 0x50u
#define GAL_PROGRESS_VERSION 1u

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_progress_t progress;
} gal_progress_blob_t;

static nvs_handle_t s_handle;
static bool s_ready;

bool gal_progress_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(GAL_PROGRESS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

void gal_progress_clear(gal_progress_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

bool gal_progress_load(gal_progress_t *out)
{
    if (!out) return false;
    gal_progress_clear(out);
    if (!s_ready) return false;

    gal_progress_blob_t blob = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_PROGRESS_KEY, &blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读进度失败: %s", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(blob) || blob.magic != GAL_PROGRESS_MAGIC ||
        blob.version != GAL_PROGRESS_VERSION) {
        ESP_LOGW(TAG, "进度记录格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    *out = blob.progress;
    return out->valid != 0;
}

bool gal_progress_store(const gal_progress_t *in)
{
    if (!in || !s_ready) return false;
    gal_progress_blob_t blob = { 0 };
    blob.magic = GAL_PROGRESS_MAGIC;
    blob.version = GAL_PROGRESS_VERSION;
    blob.progress = *in;
    const esp_err_t err = nvs_set_blob(s_handle, GAL_PROGRESS_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写进度失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}
