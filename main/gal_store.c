// main/gal_store.c —— NVS 持久化实现。
#include "gal_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gal_store";

#define NS_NAME "galgod"
#define SAVE_MAGIC 0x314C5347u      // "GLS1"
#define SAVE_VERSION 1u
#define SETTINGS_VERSION 1u

static bool s_ready;

bool gal_store_init(void)
{
    // 真正探测一次命名空间:打不开就明确返回 false,而不是假装 ready,
    // 让上层能显示"本次运行不保存"而不是静默丢失。
    nvs_handle_t h;
    const esp_err_t err = nvs_open(NS_NAME, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        s_ready = false;
        ESP_LOGW(TAG, "NVS 命名空间不可用: %s", esp_err_to_name(err));
        return false;
    }
    nvs_close(h);
    s_ready = true;
    return true;
}

static uint32_t crc_of(const void *data, size_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)data, len);
}

void gal_settings_defaults(gal_settings_t *s)
{
    if (!s) return;
    s->version = SETTINGS_VERSION;
    s->text_speed = 1;
    s->font_large = 0;
    s->auto_delay = 1;
    s->panel_opa = 2;
}

bool gal_store_load_settings(gal_settings_t *s)
{
    if (!s) return false;
    gal_settings_defaults(s);
    if (!s_ready) return false;
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) return false;
    gal_settings_t tmp;
    size_t len = sizeof(tmp);
    const esp_err_t err = nvs_get_blob(h, "set", &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(tmp) || tmp.version != SETTINGS_VERSION) return false;
    if (tmp.text_speed > 2 || tmp.auto_delay > 2 || tmp.panel_opa > 3 || tmp.font_large > 1) {
        return false;
    }
    *s = tmp;
    return true;
}

bool gal_store_save_settings(const gal_settings_t *s)
{
    if (!s_ready || !s) return false;
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "set", s, sizeof(*s));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "设置写入失败: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void slot_key(int slot, char *buf, size_t n)
{
    if (slot == GAL_SLOT_AUTO) snprintf(buf, n, "auto");
    else snprintf(buf, n, "s%d", slot);
}

bool gal_store_load_slot(int slot, gal_save_t *save)
{
    if (!s_ready || !save || slot < 0 || slot > GAL_SLOT_AUTO) return false;
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) return false;
    gal_save_t tmp;
    size_t len = sizeof(tmp);
    const esp_err_t err = nvs_get_blob(h, key, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(tmp)) return false;
    if (tmp.magic != SAVE_MAGIC || tmp.version != SAVE_VERSION) return false;
    const uint32_t crc = tmp.crc;
    tmp.crc = 0;
    if (crc_of(&tmp, sizeof(tmp)) != crc) {
        ESP_LOGW(TAG, "存档 %d CRC 不符,忽略", slot);
        return false;
    }
    tmp.crc = crc;
    if (tmp.pc == 0xFFFF) return false;
    *save = tmp;
    return true;
}

bool gal_store_save_slot(int slot, const gal_save_t *save)
{
    if (!s_ready || !save || slot < 0 || slot > GAL_SLOT_AUTO) return false;
    gal_save_t tmp = *save;
    tmp.magic = SAVE_MAGIC;
    tmp.version = SAVE_VERSION;
    tmp.crc = 0;
    tmp.crc = crc_of(&tmp, sizeof(tmp));
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, &tmp, sizeof(tmp));
    if (err == ESP_OK) err = nvs_commit(h);   // 只有 commit 也成功才算保存成功
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "存档 %d 写入失败: %s", slot, esp_err_to_name(err));
    return err == ESP_OK;
}

bool gal_store_erase_slot(int slot)
{
    if (!s_ready || slot < 0 || slot > GAL_SLOT_AUTO) return false;
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    gal_unlock_t unlock;
    uint32_t crc;
} gal_unlock_blob_t;

#define UNLOCK_MAGIC 0x314C5547u    // "GUL1"
#define UNLOCK_VERSION 1u

bool gal_store_load_unlock(gal_unlock_t *unlock)
{
    if (!s_ready || !unlock) return false;
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) return false;
    gal_unlock_blob_t tmp;
    size_t len = sizeof(tmp);
    const esp_err_t err = nvs_get_blob(h, "unlock", &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(tmp)) return false;
    if (tmp.magic != UNLOCK_MAGIC || tmp.version != UNLOCK_VERSION) return false;
    const uint32_t crc = tmp.crc;
    tmp.crc = 0;
    if (crc_of(&tmp, sizeof(tmp)) != crc) {
        ESP_LOGW(TAG, "全局解锁记录 CRC 不符,忽略");
        return false;
    }
    *unlock = tmp.unlock;
    return true;
}

bool gal_store_save_unlock(const gal_unlock_t *unlock)
{
    if (!s_ready || !unlock) return false;
    gal_unlock_blob_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.magic = UNLOCK_MAGIC;
    tmp.version = UNLOCK_VERSION;
    tmp.unlock = *unlock;
    tmp.crc = 0;
    tmp.crc = crc_of(&tmp, sizeof(tmp));
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "unlock", &tmp, sizeof(tmp));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "全局解锁写入失败: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool gal_store_any_slot(void)
{
    gal_save_t tmp;
    for (int i = 0; i <= GAL_SLOT_AUTO; ++i) {
        if (gal_store_load_slot(i, &tmp)) return true;
    }
    return false;
}
