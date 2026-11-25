// main/cafe/cafe_store.c
#include "cafe_store.h"

#include "cafe_compat.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cafe_store";
static const char *NS = "cafe";
static const uint32_t MAGIC = 0x43414645u;   // "CAFE"
static const uint16_t VERSION = 1;
static nvs_handle_t s_nvs;
static bool s_open;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool read_blob(const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!s_open) return false;
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(s_nvs, key, buf, &len);
    if (err != ESP_OK) return false;
    if (len < 12 || len > cap) return false;
    if (rd32(buf) != MAGIC || rd16(buf + 4) != VERSION || rd16(buf + 6) != len) return false;
    const uint32_t want = rd32(buf + len - 4);
    if (cafe_crc32(0, buf, len - 4) != want) {
        ESP_LOGW(TAG, "%s CRC 错误，忽略", key);
        return false;
    }
    if (out_len) *out_len = len;
    return true;
}

static bool write_blob(const char *key, uint8_t *buf, size_t len)
{
    if (!s_open || len < 12 || len > 255) return false;
    wr32(buf, MAGIC);
    wr16(buf + 4, VERSION);
    wr16(buf + 6, (uint16_t)len);
    wr32(buf + len - 4, cafe_crc32(0, buf, len - 4));
    const esp_err_t err = nvs_set_blob(s_nvs, key, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入 %s 失败: %s", key, esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_nvs) == ESP_OK;
}

void cafe_store_defaults(cafe_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;
    settings->font_large = false;
    settings->auto_play = false;
    settings->panel_opacity = 30;
    settings->title_bg = 0;
    settings->sound_on = true;
    settings->volume = 60;
}

bool cafe_store_init(void)
{
    s_open = false;
    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace %s 不可用", NS);
        return false;
    }
    s_open = true;
    return true;
}

bool cafe_store_load_settings(cafe_settings_t *out)
{
    uint8_t buf[32];
    size_t len = 0;
    if (!out || !read_blob("cfg", buf, sizeof(buf), &len) || len < 17) return false;
    out->text_speed = buf[8];
    out->font_large = buf[9] != 0;
    out->auto_play = buf[10] != 0;
    out->panel_opacity = buf[11];
    out->title_bg = buf[12];
    if (out->text_speed > 3) out->text_speed = 1;
    if (out->panel_opacity < 10 || out->panel_opacity > 55) out->panel_opacity = 30;
    if (out->title_bg >= CAFE_TITLE_BG_COUNT) out->title_bg = 0;
    if (len >= 21) {
        out->sound_on = buf[13] != 0;
        out->volume = buf[14];
        if (out->volume > 100) out->volume = 60;
    } else {
        out->sound_on = true;
        out->volume = 60;
    }
    return true;
}

bool cafe_store_save_settings(const cafe_settings_t *settings)
{
    if (!settings) return false;
    uint8_t buf[21];
    memset(buf, 0, sizeof(buf));
    buf[8] = settings->text_speed;
    buf[9] = settings->font_large ? 1 : 0;
    buf[10] = settings->auto_play ? 1 : 0;
    buf[11] = settings->panel_opacity;
    buf[12] = settings->title_bg;
    buf[13] = settings->sound_on ? 1 : 0;
    buf[14] = settings->volume > 100 ? 100 : settings->volume;
    return write_blob("cfg", buf, sizeof(buf));
}

static bool load_save_key(const char *key, cafe_save_t *out)
{
    uint8_t buf[220];
    size_t len = 0;
    if (!out || !read_blob(key, buf, sizeof(buf), &len) || len < 204) return false;
    cafe_save_t s;
    memset(&s, 0, sizeof(s));
    s.node = rd16(buf + 8);
    s.index = rd16(buf + 10);
    s.progression = buf[12];
    s.last_complete = buf[13];
    s.progress = rd32(buf + 16);
    memcpy(s.flags, buf + 20, CAFE_FLAG_MAX);
    memcpy(s.chapter, buf + 148, CAFE_CHAPTER_LEN);
    s.chapter[CAFE_CHAPTER_LEN - 1] = '\0';
    s.occupied = true;
    if (s.progression > 1) return false;
    if (s.progress & ~((1u << CAFE_PROGRESS_BITS) - 1u)) return false;
    *out = s;
    return true;
}

static bool save_save_key(const char *key, const cafe_save_t *slot_data)
{
    if (!slot_data) return false;
    uint8_t buf[204];
    memset(buf, 0, sizeof(buf));
    wr16(buf + 8, slot_data->node);
    wr16(buf + 10, slot_data->index);
    buf[12] = slot_data->progression;
    buf[13] = slot_data->last_complete;
    wr32(buf + 16, slot_data->progress);
    memcpy(buf + 20, slot_data->flags, CAFE_FLAG_MAX);
    memcpy(buf + 148, slot_data->chapter, CAFE_CHAPTER_LEN);
    buf[148 + CAFE_CHAPTER_LEN - 1] = '\0';
    return write_blob(key, buf, sizeof(buf));
}

bool cafe_store_load_slot(int slot, cafe_save_t *out)
{
    if (slot < 0 || slot >= CAFE_SAVE_SLOTS) return false;
    char key[8];
    snprintf(key, sizeof(key), "s%d", slot);
    return load_save_key(key, out);
}

bool cafe_store_save_slot(int slot, const cafe_save_t *slot_data)
{
    if (slot < 0 || slot >= CAFE_SAVE_SLOTS) return false;
    char key[8];
    snprintf(key, sizeof(key), "s%d", slot);
    return save_save_key(key, slot_data);
}

bool cafe_store_load_auto(cafe_save_t *out)
{
    return load_save_key("prog", out);
}

bool cafe_store_save_auto(const cafe_save_t *slot_data)
{
    return save_save_key("prog", slot_data);
}

bool cafe_store_clear_auto(void)
{
    uint8_t buf[12];
    memset(buf, 0, sizeof(buf));
    // 写一个 CRC 正确但 node 无效的记录并不可靠；直接写成版本不匹配的零 blob，
    // 读取端会拒绝，不需要 NVS erase API。
    return write_blob("prog", buf, sizeof(buf));
}

bool cafe_store_any_slot(void)
{
    cafe_save_t s;
    for (int i = 0; i < CAFE_SAVE_SLOTS; ++i) {
        if (cafe_store_load_slot(i, &s)) return true;
    }
    return false;
}

bool cafe_store_load_unlock(cafe_unlock_t *out)
{
    uint8_t buf[48];
    size_t len = 0;
    if (!out || !read_blob("unlock", buf, sizeof(buf), &len) || len < 33) return false;
    out->progress = rd32(buf + 8);
    out->last_cleared = buf[12];
    memcpy(out->cg_seen, buf + 13, CAFE_CG_SEEN_BYTES);
    if (out->progress & ~((1u << CAFE_PROGRESS_BITS) - 1u)) return false;
    if (out->last_cleared >= CAFE_PROGRESS_BITS) out->last_cleared = 0xFF;
    return true;
}

bool cafe_store_save_unlock(const cafe_unlock_t *unlock)
{
    if (!unlock) return false;
    uint8_t buf[33];
    memset(buf, 0, sizeof(buf));
    wr32(buf + 8, unlock->progress);
    buf[12] = unlock->last_cleared;
    memcpy(buf + 13, unlock->cg_seen, CAFE_CG_SEEN_BYTES);
    return write_blob("unlock", buf, sizeof(buf));
}
