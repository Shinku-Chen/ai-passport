#include "nine_store.h"
#include "nine_format.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "nine_store";
static nvs_handle_t s_nvs;
static bool s_open;
#ifdef NINE_STORE_HOST
static int s_fail_writes;
#endif

#define CFG_MAGIC 0x4746434eu  // "NCFG"
#define UNLOCK_MAGIC 0x4c4e554eu  // "NUNL"
#define CFG_VERSION 1u
#define UNLOCK_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint8_t text_speed;
    uint8_t font_large;
    uint8_t auto_play;
    uint8_t transparency;
    uint8_t title_bg;
    uint8_t pad[3];
    uint32_t crc;
} cfg_record_t;

static uint32_t crc_with_zero(const void *data, size_t size, size_t offset, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        uint8_t b = i >= offset && i - offset < len ? 0 : p[i];
        crc ^= b;
        for (unsigned k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool nine_store_open(nine_store_t *store)
{
    if (!store) return false;
    memset(store, 0, sizeof(*store));
    if (s_open) {
        store->ok = true;
        return true;
    }
    const esp_err_t err = nvs_open(NINE_STORE_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace unavailable: %s", esp_err_to_name(err));
        return false;
    }
    s_open = true;
    store->ok = true;
    return true;
}

bool nine_store_get(nine_store_t *store, const char *key, void *out, size_t cap,
                    size_t *length)
{
    if (!store || !store->ok || !key || !out || !length) return false;
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(s_nvs, key, out, &len);
    if (err != ESP_OK) return false;
    *length = len;
    return true;
}

bool nine_store_put(nine_store_t *store, const char *key, const void *data, size_t length)
{
    if (!store || !store->ok || !key || !data || !length) return false;
#ifdef NINE_STORE_HOST
    if (s_fail_writes > 0) {
        --s_fail_writes;
        return false;
    }
#endif
    if (nvs_set_blob(s_nvs, key, data, length) != ESP_OK) return false;
    if (nvs_commit(s_nvs) != ESP_OK) return false;
    store->dirty = false;
    return true;
}

void nine_cfg_default(nine_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    gal_settings_default(&cfg->display);
    cfg->title_bg = 0;
}

static void cfg_to_record(const nine_cfg_t *cfg, cfg_record_t *r)
{
    memset(r, 0, sizeof(*r));
    r->magic = CFG_MAGIC;
    r->version = CFG_VERSION;
    r->text_speed = cfg->display.text_speed > 3 ? 1 : cfg->display.text_speed;
    r->font_large = cfg->display.font_large ? 1 : 0;
    r->auto_play = cfg->display.auto_play ? 1 : 0;
    r->transparency = cfg->display.panel_transparency;
    if (r->transparency < GAL_TRANSPARENCY_MIN || r->transparency > GAL_TRANSPARENCY_MAX ||
        (r->transparency - GAL_TRANSPARENCY_MIN) % GAL_TRANSPARENCY_STEP != 0)
        r->transparency = GAL_TRANSPARENCY_DEFAULT;
    r->title_bg = cfg->title_bg > 2 ? 0 : cfg->title_bg;
    r->crc = crc_with_zero(r, sizeof(*r), offsetof(cfg_record_t, crc), sizeof(r->crc));
}

static bool record_to_cfg(const cfg_record_t *r, nine_cfg_t *cfg)
{
    if (r->magic != CFG_MAGIC || r->version != CFG_VERSION) return false;
    if (r->crc != crc_with_zero(r, sizeof(*r), offsetof(cfg_record_t, crc), sizeof(r->crc)))
        return false;
    if (r->text_speed > 3 || r->font_large > 1 || r->auto_play > 1 || r->title_bg > 2)
        return false;
    if (r->transparency < GAL_TRANSPARENCY_MIN || r->transparency > GAL_TRANSPARENCY_MAX ||
        (r->transparency - GAL_TRANSPARENCY_MIN) % GAL_TRANSPARENCY_STEP != 0)
        return false;
    nine_cfg_default(cfg);
    cfg->display.text_speed = r->text_speed;
    cfg->display.font_large = r->font_large;
    cfg->display.auto_play = r->auto_play;
    cfg->display.panel_transparency = r->transparency;
    cfg->title_bg = r->title_bg;
    return true;
}

bool nine_cfg_load(nine_store_t *store, nine_cfg_t *cfg)
{
    nine_cfg_default(cfg);
    cfg_record_t r;
    size_t len = 0;
    if (!nine_store_get(store, "cfg", &r, sizeof(r), &len) || len != sizeof(r)) return false;
    return record_to_cfg(&r, cfg);
}

bool nine_cfg_save(nine_store_t *store, const nine_cfg_t *cfg)
{
    if (!store || !cfg) return false;
    cfg_record_t r;
    cfg_to_record(cfg, &r);
    return nine_store_put(store, "cfg", &r, sizeof(r));
}

void nine_unlock_clear(nine_unlock_blob_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = UNLOCK_MAGIC;
    out->version = UNLOCK_VERSION;
}

bool nine_unlock_load(nine_store_t *store, nine_unlock_blob_t *out)
{
    nine_unlock_clear(out);
    nine_unlock_blob_t r;
    size_t len = 0;
    if (!nine_store_get(store, "unlock", &r, sizeof(r), &len) || len != sizeof(r))
        return false;
    if (r.magic != UNLOCK_MAGIC || r.version != UNLOCK_VERSION) return false;
    if (r.crc != crc_with_zero(&r, sizeof(r), offsetof(nine_unlock_blob_t, crc),
                               sizeof(r.crc)))
        return false;
    if (r.endings & ~0x3u) return false;
    if (r.bg > 2) return false;
    if (r.chapters & ~0x7FFFFFu) return false;
    *out = r;
    return true;
}

bool nine_unlock_save(nine_store_t *store, const nine_unlock_blob_t *in)
{
    if (!store || !in) return false;
    nine_unlock_blob_t r = *in;
    r.magic = UNLOCK_MAGIC;
    r.version = UNLOCK_VERSION;
    r.endings &= 0x3u;
    r.bg = r.bg > 2 ? 0 : r.bg;
    r.chapters &= 0x7FFFFFu;
    r.crc = crc_with_zero(&r, sizeof(r), offsetof(nine_unlock_blob_t, crc), sizeof(r.crc));
    return nine_store_put(store, "unlock", &r, sizeof(r));
}

#ifdef NINE_STORE_HOST
void nine_store_test_clear(void)
{
    s_open = false;
    s_fail_writes = 0;
    memset(&s_nvs, 0, sizeof(s_nvs));
}

void nine_store_test_fail_writes(int fail)
{
    s_fail_writes = fail < 0 ? 0 : fail;
}
#endif
