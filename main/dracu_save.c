// main/dracu_save.c —— NVS 持久化实现。
#include "dracu_save.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "dracu_save";

#define DRACU_NVS_NAMESPACE "dracu"
#define KEY_AUTO "auto"
#define KEY_CFG "cfg"
#define CFG_MAGIC 0x53u
#define CFG_VERSION 1u
// 存档 blob 上限:10 字节头 + 64 字节标志位 + 2 字节立绘状态 + 4 个带长度前缀的名字(每个最多 64 + 1),
// 满打满算约 340 字节。之前这里是 32,导致编码直接返回 0、存读档静默失败。
#define SAVE_BLOB_MAX 384u

static nvs_handle_t s_handle;
static bool s_ready;

static void slot_key(uint8_t slot, char out[4])
{
    out[0] = 's';
    out[1] = (char)('0' + (slot % 10));
    out[2] = '\0';
}

void dracu_settings_default(dracu_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;   // 中速
    settings->font_large = 0;
    settings->seen_tips = 0;   // 兼容旧存档保留,界面不再使用
}

bool dracu_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(DRACU_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

bool dracu_settings_load(dracu_settings_t *out)
{
    if (!out) return false;
    dracu_settings_default(out);
    if (!s_ready) return false;

    uint8_t blob[8] = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, KEY_CFG, blob, &len);
    if (err != ESP_OK || len < 4 || blob[0] != CFG_MAGIC || blob[1] != CFG_VERSION) {
        return false;
    }
    out->text_speed = blob[2] > 2 ? 1 : blob[2];
    out->font_large = blob[3] ? 1 : 0;
    out->seen_tips = len >= 5 && blob[4] ? 1 : 0;
    return true;
}

bool dracu_settings_store(const dracu_settings_t *settings)
{
    if (!settings || !s_ready) return false;
    const uint8_t blob[5] = { CFG_MAGIC, CFG_VERSION, settings->text_speed,
                              settings->font_large, settings->seen_tips };
    if (nvs_set_blob(s_handle, KEY_CFG, blob, sizeof(blob)) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool dracu_slot_load(uint8_t slot, dracu_save_t *out)
{
    if (!out || !s_ready || slot >= DRACU_SAVE_SLOTS) return false;
    char key[4];
    slot_key(slot, key);
    uint8_t blob[SAVE_BLOB_MAX] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, key, blob, &len) != ESP_OK) return false;
    return dracu_save_decode(out, blob, len);
}

bool dracu_slot_store(uint8_t slot, const dracu_save_t *save)
{
    if (!save || !s_ready || slot >= DRACU_SAVE_SLOTS) return false;
    uint8_t blob[SAVE_BLOB_MAX];
    const size_t len = dracu_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    char key[4];
    slot_key(slot, key);
    if (nvs_set_blob(s_handle, key, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool dracu_slot_clear(uint8_t slot)
{
    if (!s_ready || slot >= DRACU_SAVE_SLOTS) return false;
    char key[4];
    slot_key(slot, key);
    const esp_err_t err = nvs_erase_key(s_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool dracu_auto_load(dracu_save_t *out)
{
    if (!out || !s_ready) return false;
    uint8_t blob[SAVE_BLOB_MAX] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, KEY_AUTO, blob, &len) != ESP_OK) return false;
    return dracu_save_decode(out, blob, len);
}

bool dracu_auto_store(const dracu_save_t *save)
{
    if (!save || !s_ready) return false;
    uint8_t blob[SAVE_BLOB_MAX];
    const size_t len = dracu_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    if (nvs_set_blob(s_handle, KEY_AUTO, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool dracu_auto_clear(void)
{
    if (!s_ready) return false;
    const esp_err_t err = nvs_erase_key(s_handle, KEY_AUTO);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}
