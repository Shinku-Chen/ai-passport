// NVS records for the 9-nine- reader. Layouts are little-endian and versioned.
#include "nine_store.h"

#include <string.h>

#include "esp_err.h"
#include "nine_vm.h"

#define CFG_MAGIC 0x31474643u    // "CFG1"
#define UNLOCK_MAGIC 0x314b4e55u // "UNL1"
#define CFG_BYTES 16u
#define UNLOCK_RECORD 24u
#define NS "nine"

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void nine_cfg_defaults(nine_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->text_speed = 1;
    cfg->font_large = 0;
    cfg->auto_play = 0;
    cfg->transparency = 30;
    cfg->sound = 1;
}

bool nine_store_init(nine_store_t *s, bool nvs_available)
{
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    s->handle = 0;
    if (!nvs_available) return false;
    if (nvs_open(NS, NVS_READWRITE, &s->handle) != ESP_OK) {
        s->handle = 0;
        return false;
    }
    s->ok = true;
    return true;
}

static bool get_record(nine_store_t *s, const char *key, uint8_t *buf, size_t cap,
                       size_t *size)
{
    if (!s || !s->ok || !buf || !cap) return false;
    size_t len = cap;
    if (nvs_get_blob(s->handle, key, buf, &len) != ESP_OK) return false;
    if (size) *size = len;
    return true;
}

static bool put_record(nine_store_t *s, const char *key, const uint8_t *buf, size_t size)
{
    if (!s || !s->ok || !buf || !size) return false;
    if (nvs_set_blob(s->handle, key, buf, size) != ESP_OK) return false;
    if (nvs_commit(s->handle) != ESP_OK) return false;
    s->writes++;
    return true;
}

bool nine_store_cfg_load(nine_store_t *s, nine_cfg_t *out)
{
    uint8_t b[CFG_BYTES];
    size_t n = 0;
    if (!out || !get_record(s, "cfg", b, sizeof(b), &n) || n != CFG_BYTES ||
        rd32(b) != CFG_MAGIC || rd32(b + 12) != crc32_bytes(b, 12)) return false;
    nine_cfg_t c;
    c.text_speed = b[4];
    c.font_large = b[5];
    c.auto_play = b[6];
    c.transparency = b[7];
    c.sound = b[8];
    c.title_bg = b[9];
    if (c.text_speed > 3 || c.font_large > 1 || c.auto_play > 1 || c.sound > 1 ||
        c.title_bg > 2 || c.transparency < 10 || c.transparency > 55 ||
        (c.transparency % 5) != 0) return false;
    *out = c;
    return true;
}

bool nine_store_cfg_save(nine_store_t *s, const nine_cfg_t *cfg)
{
    if (!cfg) return false;
    uint8_t b[CFG_BYTES];
    memset(b, 0, sizeof(b));
    wr32(b, CFG_MAGIC);
    b[4] = cfg->text_speed;
    b[5] = cfg->font_large;
    b[6] = cfg->auto_play;
    b[7] = cfg->transparency;
    b[8] = cfg->sound;
    b[9] = cfg->title_bg;
    wr32(b + 12, crc32_bytes(b, 12));
    return put_record(s, "cfg", b, sizeof(b));
}

bool nine_store_unlock_load(nine_store_t *s, uint8_t bits[NINE_UNLOCK_BYTES])
{
    uint8_t b[UNLOCK_RECORD];
    size_t n = 0;
    if (!bits || !get_record(s, "unl", b, sizeof(b), &n) || n != UNLOCK_RECORD ||
        rd32(b) != UNLOCK_MAGIC || rd32(b + 20) != crc32_bytes(b, 20)) return false;
    memcpy(bits, b + 4, NINE_UNLOCK_BYTES);
    return true;
}

bool nine_store_unlock_save(nine_store_t *s, const uint8_t bits[NINE_UNLOCK_BYTES])
{
    if (!bits) return false;
    uint8_t b[UNLOCK_RECORD];
    wr32(b, UNLOCK_MAGIC);
    memcpy(b + 4, bits, NINE_UNLOCK_BYTES);
    wr32(b + 20, crc32_bytes(b, 20));
    return put_record(s, "unl", b, sizeof(b));
}

bool nine_store_progress_load(nine_store_t *s, uint8_t *buf, size_t cap, size_t *size)
{
    return get_record(s, "prg", buf, cap, size);
}
bool nine_store_progress_save(nine_store_t *s, const uint8_t *buf, size_t size)
{
    return size <= NINE_PROGRESS_RECORD_MAX && put_record(s, "prg", buf, size);
}
bool nine_store_auto_load(nine_store_t *s, uint8_t *buf, size_t cap, size_t *size)
{
    return get_record(s, "auto", buf, cap, size);
}
bool nine_store_auto_save(nine_store_t *s, const uint8_t *buf, size_t size)
{
    return size <= NINE_SAVE_RECORD_MAX && put_record(s, "auto", buf, size);
}
bool nine_store_auto_clear(nine_store_t *s)
{
    if (!s || !s->ok) return false;
    // NVS has no portable delete in the host stub; an all-zero record is not a
    // valid VM record, so it reads back as "no automatic progress".
    uint8_t zero = 0;
    return put_record(s, "auto", &zero, 1);
}

static void slot_key(int slot, char key[8])
{
    key[0] = 's';
    key[1] = (char)('0' + slot / 10);
    key[2] = (char)('0' + slot % 10);
    key[3] = '\0';
}

void nine_store_slot_scan(nine_store_t *store, const nine_pack_t *pack,
                          nine_slot_meta_t meta[NINE_SLOTS])
{
    if (!meta) return;
    for (int i = 0; i < NINE_SLOTS; ++i) {
        meta[i].occupied = false;
        meta[i].node = 0;
        meta[i].index = 0;
        meta[i].page = 0;
        meta[i].chapter = NINE_NODE_NONE;
        char key[8];
        slot_key(i, key);
        uint8_t b[NINE_SAVE_RECORD_MAX];
        size_t n = 0;
        if (!get_record(store, key, b, sizeof(b), &n)) continue;
        meta[i].occupied = true;
        uint16_t node = 0, page = 0;
        uint32_t index = 0;
        bool preview = false;
        if (pack && nine_vm_save_peek(pack, b, n, &node, &index, &page, &preview) &&
            node != NINE_NODE_NONE) {
            nine_node_t nr;
            if (nine_pack_node(pack, node, &nr)) {
                meta[i].node = node;
                meta[i].index = index;
                meta[i].page = page;
                meta[i].chapter = nr.chapter;
            }
        }
    }
}

bool nine_store_slot_load(nine_store_t *s, int slot, uint8_t *buf, size_t cap, size_t *size)
{
    if (slot < 0 || slot >= NINE_SLOTS) return false;
    char key[8];
    slot_key(slot, key);
    return get_record(s, key, buf, cap, size);
}

bool nine_store_slot_save(nine_store_t *s, int slot, const uint8_t *buf, size_t size)
{
    if (slot < 0 || slot >= NINE_SLOTS || size > NINE_SAVE_RECORD_MAX) return false;
    char key[8];
    slot_key(slot, key);
    return put_record(s, key, buf, size);
}

bool nine_store_sound_save(nine_store_t *s, uint8_t sound)
{
    uint8_t b[8];
    memset(b, 0, sizeof(b));
    b[0] = 1;
    b[1] = sound ? 1 : 0;
    wr32(b + 4, crc32_bytes(b, 4));
    return put_record(s, "snd", b, sizeof(b));
}

uint8_t nine_store_sound_load(nine_store_t *s)
{
    uint8_t b[8];
    size_t n = 0;
    if (!get_record(s, "snd", b, sizeof(b), &n) || n != 8 || b[0] != 1 ||
        rd32(b + 4) != crc32_bytes(b, 4)) return 1;
    return b[1] ? 1 : 0;
}
