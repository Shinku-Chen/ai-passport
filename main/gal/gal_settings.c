// main/gal/gal_settings.c —— 展示设置的 NVS 持久化实现。
#include "gal_settings.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_cfg";

#define GAL_NVS_NAMESPACE "galcfg"
#define GAL_KEY_CFG "cfg"
#define GAL_CFG_MAGIC 0x47u
#define GAL_CFG_VERSION 1u

static nvs_handle_t s_handle;
static bool s_ready;

void gal_settings_default(gal_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;   // 中速
    settings->font_large = 0;
    settings->auto_play = 0;
    settings->panel_transparency = GAL_TRANSPARENCY_DEFAULT;
}

void gal_settings_reset(gal_settings_t *settings)
{
    gal_settings_default(settings);
}

uint8_t gal_panel_opa(uint8_t transparency)
{
    if (transparency < GAL_TRANSPARENCY_MIN) transparency = GAL_TRANSPARENCY_MIN;
    if (transparency > GAL_TRANSPARENCY_MAX) transparency = GAL_TRANSPARENCY_MAX;
    return (uint8_t)(255u - (transparency * 255u + 50u) / 100u);
}

uint8_t gal_transparency_next(uint8_t current)
{
    if (current >= GAL_TRANSPARENCY_MAX) return GAL_TRANSPARENCY_MIN;
    const uint16_t next = (uint16_t)(current + GAL_TRANSPARENCY_STEP);
    return (uint8_t)(next > GAL_TRANSPARENCY_MAX ? GAL_TRANSPARENCY_MAX : next);
}

bool gal_settings_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(GAL_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

bool gal_settings_load(gal_settings_t *out)
{
    if (!out) return false;
    gal_settings_default(out);
    if (!s_ready) return false;

    uint8_t blob[8] = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_KEY_CFG, blob, &len);
    if (err != ESP_OK || len < 4 || blob[0] != GAL_CFG_MAGIC || blob[1] != GAL_CFG_VERSION) {
        return false;
    }
    out->text_speed = blob[2] > 3 ? 1 : blob[2];
    out->font_large = blob[3] ? 1 : 0;
    out->auto_play = len >= 5 ? (blob[4] ? 1 : 0) : 0;
    out->panel_transparency =
        (len >= 6 && blob[5] >= GAL_TRANSPARENCY_MIN && blob[5] <= GAL_TRANSPARENCY_MAX)
            ? blob[5]
            : GAL_TRANSPARENCY_DEFAULT;
    return true;
}

bool gal_settings_store(const gal_settings_t *settings)
{
    if (!settings || !s_ready) return false;
    const uint8_t blob[6] = { GAL_CFG_MAGIC, GAL_CFG_VERSION, settings->text_speed,
                              settings->font_large, settings->auto_play,
                              settings->panel_transparency };
    if (nvs_set_blob(s_handle, GAL_KEY_CFG, blob, sizeof(blob)) != ESP_OK) return false;
    return nvs_commit(s_handle) == ESP_OK;
}
