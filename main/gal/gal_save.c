#include "gal_save.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gal_save";

#define GAL_NVS_NAMESPACE "galgame"
#define GAL_NVS_KEY "state"

static gal_save_data_t s_data;
static bool s_ready;

gal_save_data_t *gal_save_data(void)
{
    return &s_data;
}

esp_err_t gal_save_commit(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, GAL_NVS_KEY, &s_data, sizeof(s_data));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "writing saved state failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t gal_save_init(void)
{
    gal_save_defaults(&s_data);
    s_ready = false;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The application owns this NVS partition, so reinitialising it costs
         * only the reader's own state. */
        ESP_LOGW(TAG, "reformatting NVS: %s", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        /* Keep running with defaults: losing saves must not stop the reader. */
        s_ready = false;
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(GAL_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_ready = true;
        ESP_LOGI(TAG, "no saved state yet; using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, using defaults: %s", esp_err_to_name(err));
        s_ready = true;
        return ESP_OK;
    }

    size_t length = sizeof(s_data);
    gal_save_data_t loaded;
    err = nvs_get_blob(handle, GAL_NVS_KEY, &loaded, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved state yet; using defaults");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "reading saved state failed, using defaults: %s", esp_err_to_name(err));
    } else if (length != sizeof(loaded) || !gal_save_valid(&loaded)) {
        /* An older or corrupt blob. Reset rather than apply half of it. */
        ESP_LOGW(TAG, "saved state is not usable (%u bytes); resetting", (unsigned)length);
    } else {
        s_data = loaded;
        ESP_LOGI(TAG, "restored state: speed %u, size %u, resume chapter %u",
                 s_data.text_speed, s_data.text_size, s_data.last.chapter + 1);
    }

    s_ready = true;
    return ESP_OK;
}

void gal_save_clear_slots(void)
{
    for (int i = 0; i < GAL_SAVE_SLOTS; i++) {
        memset(&s_data.slots[i], 0, sizeof(s_data.slots[i]));
    }
}
