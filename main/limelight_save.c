// main/limelight_save.c —— NVS 存档/设置实现(与《星空列车》版同一套做法)。
#include "limelight_save.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lime_save";
static const char *NS_NAME = "limelight";

static nvs_handle_t s_handle;
static bool s_open;

void lime_settings_default(lime_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;      // 中速
    settings->auto_delay = 1;      // 中(900ms)
    settings->show_sprite = 1;
    settings->seen_tips = 0;
}

bool lime_save_init(void)
{
    if (s_open) return true;
    esp_err_t err = nvs_open(NS_NAME, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_open = true;
    return true;
}

bool lime_settings_load(lime_settings_t *out)
{
    if (!out) return false;
    lime_settings_default(out);
    if (!s_open) return false;
    uint8_t blob[4] = { 0 };
    size_t length = sizeof(blob);
    if (nvs_get_blob(s_handle, "settings", blob, &length) != ESP_OK || length != sizeof(blob)) {
        return false;      // 用默认值
    }
    if (blob[0] > 2 || blob[1] > 2 || blob[2] > 1 || blob[3] > 1) return false;
    out->text_speed = blob[0];
    out->auto_delay = blob[1];
    out->show_sprite = blob[2];
    out->seen_tips = blob[3];
    return true;
}

bool lime_settings_store(const lime_settings_t *settings)
{
    if (!settings || !s_open) return false;
    const uint8_t blob[4] = { settings->text_speed, settings->auto_delay,
                              settings->show_sprite, settings->seen_tips };
    return nvs_set_blob(s_handle, "settings", blob, sizeof(blob)) == ESP_OK &&
           nvs_commit(s_handle) == ESP_OK;
}

static void slot_key(uint8_t slot, char *out, size_t size)
{
    snprintf(out, size, "slot%u", (unsigned)slot);
}

bool lime_slot_load(uint8_t slot, lime_save_t *out)
{
    if (!out || !s_open || slot >= LIME_SAVE_SLOTS) return false;
    char key[16];
    slot_key(slot, key, sizeof(key));
    uint8_t blob[32];
    size_t length = sizeof(blob);
    if (nvs_get_blob(s_handle, key, blob, &length) != ESP_OK) return false;
    return lime_save_decode(out, blob, length);
}

bool lime_slot_store(uint8_t slot, const lime_save_t *save)
{
    if (!save || !s_open || slot >= LIME_SAVE_SLOTS) return false;
    uint8_t blob[32];
    const size_t length = lime_save_encode(save, blob, sizeof(blob));
    if (length == 0) return false;
    char key[16];
    slot_key(slot, key, sizeof(key));
    return nvs_set_blob(s_handle, key, blob, length) == ESP_OK && nvs_commit(s_handle) == ESP_OK;
}

bool lime_slot_clear(uint8_t slot)
{
    if (!s_open || slot >= LIME_SAVE_SLOTS) return false;
    char key[16];
    slot_key(slot, key, sizeof(key));
    return nvs_erase_key(s_handle, key) == ESP_OK && nvs_commit(s_handle) == ESP_OK;
}

bool lime_auto_load(lime_save_t *out)
{
    if (!out || !s_open) return false;
    uint8_t blob[32];
    size_t length = sizeof(blob);
    if (nvs_get_blob(s_handle, "auto", blob, &length) != ESP_OK) return false;
    return lime_save_decode(out, blob, length);
}

bool lime_auto_store(const lime_save_t *save)
{
    if (!save || !s_open) return false;
    uint8_t blob[32];
    const size_t length = lime_save_encode(save, blob, sizeof(blob));
    if (length == 0) return false;
    return nvs_set_blob(s_handle, "auto", blob, length) == ESP_OK && nvs_commit(s_handle) == ESP_OK;
}

bool lime_auto_clear(void)
{
    if (!s_open) return false;
    return nvs_erase_key(s_handle, "auto") == ESP_OK && nvs_commit(s_handle) == ESP_OK;
}
