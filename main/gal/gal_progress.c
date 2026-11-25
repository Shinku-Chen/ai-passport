// main/gal/gal_progress.c —— 阅读进度的 NVS 持久化实现。
//
// 与手动存档槽位(gal_save)相互独立：
//   进度 = 每次推进剧情自动更新，「继续游戏」直接从这里接上；
//   存档 = 玩家在存档页手动写入的 3 个节点。
// 版本 3 增加 run_mask（本局实际走过的章节）。
// 版本 4 增加 preview（本局从章节 / 回想试读开始，永不授正式结局）。
#include "gal_progress.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_prog";

#define GAL_PROGRESS_NAMESPACE "galprog"
#define GAL_PROGRESS_KEY "pos"
#define GAL_PROGRESS_MAGIC 0x50u
#define GAL_PROGRESS_VERSION 4u

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint8_t valid;
    uint8_t run_mask;
    uint8_t preview;
    uint8_t reserved2;
    uint16_t event_index;
    uint16_t page_index;
    uint16_t reserved3;
    uint16_t reserved4;
    uint32_t flags;
} gal_progress_blob_t;

typedef struct {
    uint8_t valid;
    uint16_t event_index;
    uint16_t page_index;
} gal_progress_v1_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_progress_v1_t progress;
} gal_progress_blob_v1_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint8_t valid;
    uint8_t run_mask;
    uint16_t event_index;
    uint16_t page_index;
    uint16_t reserved2;
    uint32_t flags;
} gal_progress_blob_v3_t;

typedef struct {
    uint8_t valid;
    uint16_t event_index;
    uint16_t page_index;
    uint32_t flags;
} gal_progress_v2_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_progress_v2_t progress;
} gal_progress_blob_v2_t;

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

    uint8_t raw[sizeof(gal_progress_blob_t)] = { 0 };
    size_t len = sizeof(raw);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_PROGRESS_KEY, raw, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读进度失败: %s", esp_err_to_name(err));
        return false;
    }
    if (len == sizeof(gal_progress_blob_v1_t) && raw[0] == GAL_PROGRESS_MAGIC && raw[1] == 1u) {
        gal_progress_blob_v1_t old;
        memcpy(&old, raw, sizeof(old));
        out->valid = old.progress.valid;
        out->event_index = old.progress.event_index;
        out->page_index = old.progress.page_index;
        out->flags = 0;
        out->run_mask = 0;
        out->preview = 1;   // 旧记录保守处理：不视为正式周目
        return out->valid != 0;
    }
    if (len == sizeof(gal_progress_blob_v2_t) && raw[0] == GAL_PROGRESS_MAGIC && raw[1] == 2u) {
        gal_progress_blob_v2_t old;
        memcpy(&old, raw, sizeof(old));
        out->valid = old.progress.valid;
        out->event_index = old.progress.event_index;
        out->page_index = old.progress.page_index;
        out->flags = old.progress.flags;
        out->run_mask = 0;
        out->preview = 1;
        return out->valid != 0;
    }
    if (len == sizeof(gal_progress_blob_v3_t) && raw[0] == GAL_PROGRESS_MAGIC &&
        raw[1] == 3u) {
        gal_progress_blob_v3_t old;
        memcpy(&old, raw, sizeof(old));
        out->valid = old.valid;
        out->event_index = old.event_index;
        out->page_index = old.page_index;
        out->flags = old.flags;
        out->run_mask = old.run_mask;
        out->preview = 1;
        return out->valid != 0;
    }
    if (len != sizeof(gal_progress_blob_t) || raw[0] != GAL_PROGRESS_MAGIC ||
        raw[1] != GAL_PROGRESS_VERSION) {
        ESP_LOGW(TAG, "进度记录格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    gal_progress_blob_t blob;
    memcpy(&blob, raw, sizeof(blob));
    out->valid = blob.valid;
    out->event_index = blob.event_index;
    out->page_index = blob.page_index;
    out->flags = blob.flags;
    out->run_mask = blob.run_mask;
    out->preview = blob.preview;
    return out->valid != 0;
}

bool gal_progress_store(const gal_progress_t *in)
{
    if (!in || !s_ready) return false;
    gal_progress_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = GAL_PROGRESS_MAGIC;
    blob.version = GAL_PROGRESS_VERSION;
    blob.valid = in->valid;
    blob.run_mask = in->run_mask;
    blob.preview = in->preview;
    blob.event_index = in->event_index;
    blob.page_index = in->page_index;
    blob.flags = in->flags;
    const esp_err_t err = nvs_set_blob(s_handle, GAL_PROGRESS_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写进度失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}
