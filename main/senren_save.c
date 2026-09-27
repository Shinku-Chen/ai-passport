// main/senren_save.c —— NVS 持久化实现。
#include "senren_save.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "senren_save";

#define SENREN_NVS_NAMESPACE "senren"
#define KEY_AUTO "auto"
#define KEY_CFG "cfg"
#define CFG_MAGIC 0x53u
#define CFG_VERSION 1u
#define SAVE_BLOB_MAX 32u

static nvs_handle_t s_handle;
static bool s_ready;

static void slot_key(uint8_t slot, char out[4])
{
    out[0] = 's';
    out[1] = (char)('0' + (slot % 10));
    out[2] = '\0';
}

void senren_settings_default(senren_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;   // 中速
    settings->font_large = 0;
    settings->seen_tips = 0;
}

bool senren_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(SENREN_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

bool senren_settings_load(senren_settings_t *out)
{
    if (!out) return false;
    senren_settings_default(out);
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

bool senren_settings_store(const senren_settings_t *settings)
{
    if (!settings || !s_ready) return false;
    const uint8_t blob[5] = { CFG_MAGIC, CFG_VERSION, settings->text_speed,
                              settings->font_large, settings->seen_tips };
    if (nvs_set_blob(s_handle, KEY_CFG, blob, sizeof(blob)) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool senren_slot_load(uint8_t slot, senren_save_t *out)
{
    if (!out || !s_ready || slot >= SENREN_SAVE_SLOTS) return false;
    char key[4];
    slot_key(slot, key);
    uint8_t blob[SAVE_BLOB_MAX] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, key, blob, &len) != ESP_OK) return false;
    return senren_save_decode(out, blob, len);
}

bool senren_slot_store(uint8_t slot, const senren_save_t *save)
{
    if (!save || !s_ready || slot >= SENREN_SAVE_SLOTS) return false;
    uint8_t blob[SAVE_BLOB_MAX];
    const size_t len = senren_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    char key[4];
    slot_key(slot, key);
    if (nvs_set_blob(s_handle, key, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool senren_slot_clear(uint8_t slot)
{
    if (!s_ready || slot >= SENREN_SAVE_SLOTS) return false;
    char key[4];
    slot_key(slot, key);
    const esp_err_t err = nvs_erase_key(s_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool senren_auto_load(senren_save_t *out)
{
    if (!out || !s_ready) return false;
    uint8_t blob[SAVE_BLOB_MAX] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, KEY_AUTO, blob, &len) != ESP_OK) return false;
    return senren_save_decode(out, blob, len);
}

bool senren_auto_store(const senren_save_t *save)
{
    if (!save || !s_ready) return false;
    uint8_t blob[SAVE_BLOB_MAX];
    const size_t len = senren_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    if (nvs_set_blob(s_handle, KEY_AUTO, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool senren_auto_clear(void)
{
    if (!s_ready) return false;
    const esp_err_t err = nvs_erase_key(s_handle, KEY_AUTO);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}
