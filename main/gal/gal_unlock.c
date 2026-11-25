// main/gal/gal_unlock.c —— 剧情锁的位掩码与持久化实现。
#include "gal_unlock.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_unlock";

#define GAL_UNLOCK_NAMESPACE "galunlock"
#define GAL_UNLOCK_KEY "bits"
#define GAL_UNLOCK_MAGIC 0x4Cu
#define GAL_UNLOCK_VERSION 2u

// 版本 2：CG 位图 64 位（两条 32 位字），其余字段不变。
typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint32_t cg_lo;
    uint32_t cg_hi;
    uint32_t chapter;
    uint32_t flag;
    uint32_t ending;
} gal_unlock_blob_t;

// 版本 1：CG 位图 32 位。
typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint32_t cg;
    uint32_t chapter;
    uint32_t flag;
    uint32_t ending;
} gal_unlock_blob_v1_t;

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
    uint8_t raw[sizeof(gal_unlock_blob_t)] = { 0 };
    size_t len = sizeof(raw);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_UNLOCK_KEY, raw, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读解锁状态失败: %s", esp_err_to_name(err));
        return false;
    }
    memcpy(&blob, raw, sizeof(blob));
    if (len == sizeof(gal_unlock_blob_v1_t) && raw[0] == GAL_UNLOCK_MAGIC &&
        raw[1] == 1u) {
        gal_unlock_blob_v1_t old;
        memcpy(&old, raw, sizeof(old));
        out->cg = old.cg;
        out->chapter = old.chapter;
        out->flag = old.flag;
        out->ending = old.ending;
        return true;
    }
    if (len != sizeof(gal_unlock_blob_t) || blob.magic != GAL_UNLOCK_MAGIC ||
        blob.version != GAL_UNLOCK_VERSION) {
        ESP_LOGW(TAG, "解锁记录格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    out->cg = (uint64_t)blob.cg_lo | ((uint64_t)blob.cg_hi << 32);
    out->chapter = blob.chapter;
    out->flag = blob.flag;
    out->ending = blob.ending;
    return true;
}

bool gal_unlock_store(const gal_unlock_t *in)
{
    if (!in || !s_ready) return false;
    gal_unlock_blob_t blob = { 0 };
    blob.magic = GAL_UNLOCK_MAGIC;
    blob.version = GAL_UNLOCK_VERSION;
    blob.cg_lo = (uint32_t)(in->cg & 0xFFFFFFFFu);
    blob.cg_hi = (uint32_t)(in->cg >> 32);
    blob.chapter = in->chapter;
    blob.flag = in->flag;
    blob.ending = in->ending;
    const esp_err_t err = nvs_set_blob(s_handle, GAL_UNLOCK_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写解锁状态失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

void gal_unlock_grant_cg(gal_unlock_t *out, uint16_t index)
{
    if (!out || index >= 64) return;
    out->cg |= (uint64_t)1u << index;
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

bool gal_unlock_has_cg(const gal_unlock_t *in, uint16_t index)
{
    return in && index < 64 && (in->cg & ((uint64_t)1u << index)) != 0;
}

bool gal_unlock_has_chapter(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->chapter, index);
}

bool gal_unlock_has_ending(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->ending, index);
}

bool gal_unlock_has_any_ending(const gal_unlock_t *in)
{
    return in && in->ending != 0;
}

bool gal_unlock_has_flag(const gal_unlock_t *in, uint8_t index)
{
    return in && bit_has(in->flag, index);
}

bool gal_flags_has(uint32_t flags, uint8_t index)
{
    return bit_has(flags, index);
}

uint32_t gal_flags_set(uint32_t flags, uint8_t index)
{
    bit_add(&flags, index);
    return flags;
}
