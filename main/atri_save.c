// main/atri_save.c —— NVS 持久化实现。
#include "atri_save.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "atri_save";

#define ATRI_NVS_NAMESPACE "atri"
#define KEY_AUTO "auto"
#define KEY_CFG "cfg"
#define CFG_MAGIC 0xA7u
#define CFG_VERSION 1u

static nvs_handle_t s_handle;
static bool s_ready;

void atri_settings_default(atri_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;   // 中速
    settings->seen_warning = 0;
    settings->seen_happy = 0;
    settings->seen_bad = 0;
    settings->seen_true = 0;
}

bool atri_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(ATRI_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

bool atri_settings_load(atri_settings_t *out)
{
    if (!out) return false;
    atri_settings_default(out);
    if (!s_ready) return false;

    uint8_t blob[8] = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, KEY_CFG, blob, &len);
    if (err != ESP_OK || len < 4 || blob[0] != CFG_MAGIC || blob[1] != CFG_VERSION) {
        return false;
    }
    out->text_speed = blob[2] > 2 ? 1 : blob[2];
    out->seen_warning = blob[3] ? 1 : 0;
    out->seen_happy = len >= 5 && blob[4] ? 1 : 0;
    out->seen_bad = len >= 6 && blob[5] ? 1 : 0;
    out->seen_true = len >= 7 && blob[6] ? 1 : 0;
    return true;
}

bool atri_settings_store(const atri_settings_t *settings)
{
    if (!settings || !s_ready) return false;
    const uint8_t blob[7] = { CFG_MAGIC, CFG_VERSION, settings->text_speed,
                              settings->seen_warning, settings->seen_happy,
                              settings->seen_bad, settings->seen_true };
    if (nvs_set_blob(s_handle, KEY_CFG, blob, sizeof(blob)) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

static bool slot_key(uint8_t slot, char out[8], bool *is_auto)
{
    if (slot == ATRI_AUTO_SLOT) {
        snprintf(out, 8, "%s", KEY_AUTO);
        if (is_auto) *is_auto = true;
        return true;
    }
    if (slot >= ATRI_SAVE_SLOTS) return false;
    out[0] = 's';
    out[1] = (char)('0' + slot);
    out[2] = '\0';
    if (is_auto) *is_auto = false;
    return true;
}

bool atri_slot_load(uint8_t slot, atri_save_t *out)
{
    char key[8];
    if (!out || !s_ready || !slot_key(slot, key, NULL)) return false;
    uint8_t blob[32] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, key, blob, &len) != ESP_OK) return false;
    return atri_save_decode(out, blob, len);
}

bool atri_slot_store(uint8_t slot, const atri_save_t *save)
{
    char key[8];
    if (!save || !s_ready || !slot_key(slot, key, NULL)) return false;
    uint8_t blob[32];
    const size_t len = atri_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    if (nvs_set_blob(s_handle, key, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool atri_slot_clear(uint8_t slot)
{
    char key[8];
    if (!s_ready || !slot_key(slot, key, NULL)) return false;
    const esp_err_t err = nvs_erase_key(s_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

void atri_save_from_player(const atri_player_t *player, atri_save_t *out)
{
    if (!player || !out) return;
    memset(out, 0, sizeof(*out));
    out->chapter = player->chapter;
    out->scene = player->scene;
    out->dialogue = player->dialogue;
    out->choice_len = player->choice_len;
    memcpy(out->choice_pick, player->choice_pick, sizeof(out->choice_pick));
}
