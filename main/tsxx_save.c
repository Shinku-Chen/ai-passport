// main/tsxx_save.c —— NVS 持久化实现。
#include "tsxx_save.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tsxx_save";

#define TSXX_NVS_NAMESPACE "tsxx"
#define KEY_AUTO "auto"
#define KEY_CFG "cfg"
#define CFG_MAGIC 0x5Bu
#define CFG_VERSION 1u

static nvs_handle_t s_handle;
static bool s_ready;

void tsxx_settings_default(tsxx_settings_t *settings)
{
    if (!settings) return;
    memset(settings, 0, sizeof(*settings));
    settings->text_speed = 1;   // 中速
    settings->auto_play = 0;
    settings->seen_warning = 0;
}

bool tsxx_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(TSXX_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

bool tsxx_settings_load(tsxx_settings_t *out)
{
    if (!out) return false;
    tsxx_settings_default(out);
    if (!s_ready) return false;

    uint8_t blob[8] = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, KEY_CFG, blob, &len);
    if (err != ESP_OK || len < 4 || blob[0] != CFG_MAGIC || blob[1] != CFG_VERSION) {
        return false;
    }
    if (blob[2] > 2) {
        out->text_speed = 1;   // 越界值(旧固件/写坏的 NVS)退回中速,不让排版失控
    } else {
        out->text_speed = blob[2];
    }
    out->auto_play = blob[3] ? 1 : 0;
    return true;
}

bool tsxx_settings_store(const tsxx_settings_t *settings)
{
    if (!settings || !s_ready) return false;
    const uint8_t blob[5] = { CFG_MAGIC, CFG_VERSION, settings->text_speed,
                              settings->auto_play, settings->seen_warning };
    if (nvs_set_blob(s_handle, KEY_CFG, blob, sizeof(blob)) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

// 槽位 -> NVS key。自动槽用固定的 "auto",手动槽是 "s0".."s4"。
static bool slot_key(uint8_t slot, char out[8])
{
    if (slot == TSXX_AUTO_SLOT) {
        snprintf(out, 8, "%s", KEY_AUTO);
        return true;
    }
    if (slot >= TSXX_SAVE_SLOTS) return false;
    out[0] = 's';
    out[1] = (char)('0' + slot);
    out[2] = '\0';
    return true;
}

bool tsxx_slot_load(uint8_t slot, tsxx_save_t *out)
{
    char key[8];
    if (!out || !s_ready || !slot_key(slot, key)) return false;
    uint8_t blob[16] = { 0 };
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_handle, key, blob, &len) != ESP_OK) return false;
    return tsxx_save_decode(out, blob, len);
}

bool tsxx_slot_store(uint8_t slot, const tsxx_save_t *save)
{
    char key[8];
    if (!save || !s_ready || !slot_key(slot, key)) return false;
    uint8_t blob[16];
    const size_t len = tsxx_save_encode(save, blob, sizeof(blob));
    if (len == 0) return false;
    if (nvs_set_blob(s_handle, key, blob, len) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

bool tsxx_slot_clear(uint8_t slot)
{
    char key[8];
    if (!s_ready || !slot_key(slot, key)) return false;
    const esp_err_t err = nvs_erase_key(s_handle, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_handle) == ESP_OK;
}

void tsxx_save_from_player(const tsxx_player_t *player, tsxx_save_t *out)
{
    if (!player || !out) return;
    out->page = player->page;
    out->screen = player->screen;
}
