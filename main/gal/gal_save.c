// main/gal/gal_save.c —— 存档槽位的 NVS 持久化实现。
#include "gal_save.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_save";

#define GAL_SAVE_NAMESPACE "galsave"
#define GAL_SAVE_KEY "slots"
#define GAL_SAVE_MAGIC 0x53u
#define GAL_SAVE_VERSION 2u

// 落盘格式:魔数 + 版本 + 槽位,便于以后扩展。
// 版本 1 没有 flags;版本 2 在每个槽位末尾增加 uint32_t flags。
typedef struct {
    uint8_t occupied;
    uint16_t event_index;
    uint16_t page_index;
    char chapter[GAL_SAVE_CHAPTER_LEN];
    uint8_t reserved;
} gal_slot_v1_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_slot_v1_t slots[GAL_SAVE_SLOTS];
} gal_save_blob_v1_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_slot_t slots[GAL_SAVE_SLOTS];
} gal_save_blob_t;

static nvs_handle_t s_handle;
static bool s_ready;

bool gal_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(GAL_SAVE_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

void gal_save_clear(gal_save_t *save)
{
    if (!save) return;
    memset(save, 0, sizeof(*save));
}

bool gal_save_load(gal_save_t *save)
{
    if (!save) return false;
    gal_save_clear(save);
    if (!s_ready) return false;

    uint8_t raw[sizeof(gal_save_blob_t)] = { 0 };
    size_t len = sizeof(raw);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_SAVE_KEY, raw, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读存档失败: %s", esp_err_to_name(err));
        return false;
    }
    if (len == sizeof(gal_save_blob_v1_t) && raw[0] == GAL_SAVE_MAGIC &&
        raw[1] == 1u) {
        gal_save_blob_v1_t old;
        memcpy(&old, raw, sizeof(old));
        for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
            save->slots[i].occupied = old.slots[i].occupied;
            save->slots[i].event_index = old.slots[i].event_index;
            save->slots[i].page_index = old.slots[i].page_index;
            memcpy(save->slots[i].chapter, old.slots[i].chapter,
                   sizeof(save->slots[i].chapter));
            save->slots[i].reserved = 0;
            save->slots[i].flags = 0;
        }
        return gal_save_any(save);
    }
    if (len != sizeof(gal_save_blob_t) || raw[0] != GAL_SAVE_MAGIC ||
        raw[1] != GAL_SAVE_VERSION) {
        ESP_LOGW(TAG, "存档格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    memcpy(save, raw + 4, sizeof(save->slots));
    return gal_save_any(save);
}

bool gal_save_store(const gal_save_t *save)
{
    if (!save || !s_ready) return false;
    gal_save_blob_t blob = { 0 };
    blob.magic = GAL_SAVE_MAGIC;
    blob.version = GAL_SAVE_VERSION;
    memcpy(blob.slots, save->slots, sizeof(blob.slots));
    const esp_err_t err = nvs_set_blob(s_handle, GAL_SAVE_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写存档失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

bool gal_save_any(const gal_save_t *save)
{
    if (!save) return false;
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        if (save->slots[i].occupied) return true;
    }
    return false;
}

int gal_save_latest(const gal_save_t *save)
{
    if (!save) return -1;
    // 槽位下标即写入顺序:取最后一个已占用的槽位。
    for (int i = GAL_SAVE_SLOTS - 1; i >= 0; --i) {
        if (save->slots[i].occupied) return i;
    }
    return -1;
}
