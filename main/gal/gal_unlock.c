// main/gal/gal_unlock.c —— 剧情锁的位掩码与持久化实现。
#include "gal_unlock.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_unlock";

#define GAL_UNLOCK_NAMESPACE "galunlock"
#define GAL_UNLOCK_KEY "bits"
#define GAL_UNLOCK_MAGIC 0x4Cu
#define GAL_UNLOCK_VERSION 1u

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_unlock_t bits;
} gal_unlock_blob_t;

static nvs_handle_t s_handle;
static bool s_ready;

static bool bit_has(uint32_t mask, uint8_t index)
{
    if (index >= 32) return false;
    return (mask & (1u << index)) != 0;
}

static void bit_add(uint32_t *mask, uint8_t index)
{
    if (!mask || index >= 32) return;
    *mask |= (1u << index);
}

bool gal_unlock_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(GAL_UNLOCK_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

void gal_unlock_clear(gal_unlock_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

bool gal_unlock_load(gal_unlock_t *out)
{
    if (!out) return false;
    gal_unlock_clear(out);
    if (!s_ready) return false;

    gal_unlock_blob_t blob = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_UNLOCK_KEY, &blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读解锁状态失败: %s", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(blob) || blob.magic != GAL_UNLOCK_MAGIC ||
        blob.version != GAL_UNLOCK_VERSION) {
        ESP_LOGW(TAG, "解锁记录格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    *out = blob.bits;
    return true;
}

bool gal_unlock_store(const gal_unlock_t *in)
{
    if (!in || !s_ready) return false;
    gal_unlock_blob_t blob = { 0 };
    blob.magic = GAL_UNLOCK_MAGIC;
    blob.version = GAL_UNLOCK_VERSION;
    blob.bits = *in;
    const esp_err_t err = nvs_set_blob(s_handle, GAL_UNLOCK_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写解锁状态失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

void gal_unlock_grant_cg(gal_unlock_t *out, uint8_t index)
{
    if (out) bit_add(&out->cg, index);
}

void gal_unlock_grant_chapter(gal_unlock_t *out, uint8_t index)
{
    if (out) bit_add(&out->chapter, index);
}

void gal_unlock_grant_ending(gal_unlock_t *out, uint8_t index)
{
    if (out) bit_add(&out->ending, index);
}

void gal_unlock_set_flag(gal_unlock_t *out, uint8_t index)
{
    if (out) bit_add(&out->flag, index);
}

bool gal_unlock_has_cg(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->cg, index);
}

bool gal_unlock_has_chapter(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->chapter, index);
}

bool gal_unlock_has_ending(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->ending, index);
}

bool gal_unlock_has_flag(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->flag, index);
}
