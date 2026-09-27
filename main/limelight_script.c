// main/limelight_script.c —— 剧本包解析与分块解压(纯逻辑,不依赖 ESP-IDF)。
#include "limelight_script.h"

#include <string.h>

#define HEADER_SIZE 32u
#define SECTION_SIZE 16u
#define SECTION_COUNT 7u

#define SEC_TEXT 0u
#define SEC_CHUNK 1u
#define SEC_NAME 2u
#define SEC_NAMETEXT 3u
#define SEC_CHAPTER 4u
#define SEC_CHOICE 5u
#define SEC_META 6u

#define CHUNK_ENTRY_SIZE 16u     // { off u32, size u32, first_id u32, count u16, flags u16 }
#define NAME_ENTRY_SIZE 8u       // { off u32, len u16, pad u16 }
#define CHAPTER_ENTRY_SIZE 6u    // { first_id u32, name u16 }
#define CHOICE_ENTRY_SIZE 6u     // { id u32, count u8, pad u8 }
#define CHOICE_OPTION_SIZE 6u    // { name u16, target u32 }
#define CHUNK_FLAG_STORED (1u << 0)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool lime_script_open(lime_script_t *script, const uint8_t *blob, uint32_t size)
{
    if (!script || !blob || size < HEADER_SIZE + SECTION_SIZE * SECTION_COUNT) return false;
    if (memcmp(blob, LIME_SCRIPT_MAGIC, 8) != 0) return false;
    if (rd32(blob + 8) != LIME_SCRIPT_VERSION) return false;
    if (rd32(blob + 12) != size) return false;
    if (rd32(blob + 16) != SECTION_COUNT) return false;

    lime_script_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.blob = blob;
    parsed.size = size;
    parsed.max_chunk_raw = rd32(blob + 20);

    uint32_t seen = 0;
    for (uint32_t i = 0; i < SECTION_COUNT; i++) {
        const uint8_t *section = blob + HEADER_SIZE + SECTION_SIZE * i;
        uint32_t type = rd32(section);
        uint32_t off = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t len = rd32(section + 12);
        if (type >= SECTION_COUNT || off + len > size || off < HEADER_SIZE) return false;
        seen |= 1u << type;
        switch (type) {
        case SEC_TEXT:
            parsed.text_sec = blob + off;
            parsed.text_sec_len = len;
            break;
        case SEC_CHUNK:
            if (count == 0 || len != count * CHUNK_ENTRY_SIZE) return false;
            parsed.chunk_tab = blob + off;
            parsed.chunks = count;
            parsed.chunk_tab_len = len;
            parsed.chunk_entries = rd16(blob + off + 12);
            break;
        case SEC_NAME:
            if (len != count * NAME_ENTRY_SIZE) return false;
            parsed.name_tab = blob + off;
            parsed.name_count = count;
            break;
        case SEC_NAMETEXT:
            parsed.name_text = blob + off;
            parsed.name_text_len = len;
            break;
        case SEC_CHAPTER:
            if (len != count * CHAPTER_ENTRY_SIZE) return false;
            parsed.chapter_tab = blob + off;
            parsed.chapter_count = count;
            break;
        case SEC_CHOICE:
            parsed.choice_tab = blob + off;
            parsed.choice_count = count;
            parsed.choice_tab_len = len;
            break;
        default:
            break;
        }
    }
    if (seen != (1u << SECTION_COUNT) - 1u) return false;
    if (!parsed.chunk_tab || !parsed.name_tab || !parsed.name_text) return false;
    if (parsed.chunk_entries == 0 || parsed.max_chunk_raw == 0) return false;

    // 条目总数 = 最后一块的 first_id + count - 1。
    const uint8_t *last = parsed.chunk_tab + (parsed.chunks - 1) * CHUNK_ENTRY_SIZE;
    parsed.entries = rd32(last + 8) + rd16(last + 12) - 1;
    for (uint32_t i = 0; i < parsed.chunks; i++) {
        const uint8_t *chunk = parsed.chunk_tab + i * CHUNK_ENTRY_SIZE;
        uint32_t off = rd32(chunk);
        uint32_t len = rd32(chunk + 4);
        if (off + len > parsed.text_sec_len) return false;
    }
    *script = parsed;
    return true;
}

uint32_t lime_script_entries(const lime_script_t *script)
{
    return script ? script->entries : 0;
}

uint16_t lime_script_chunk_entries(const lime_script_t *script)
{
    return script ? script->chunk_entries : 0;
}

void lime_script_set_inflate(lime_script_t *script, lime_inflate_fn inflate)
{
    if (script) script->inflate = inflate;
}

void lime_script_set_cache(lime_script_t *script, uint8_t *buffer, uint32_t capacity)
{
    if (!script) return;
    script->cache = buffer;
    script->cache_cap = capacity;
    script->cache_len = 0;
    script->cache_first_id = 0;
}

// id 所在块的第一条 id。
static uint32_t chunk_first_id(const lime_script_t *script, uint32_t id)
{
    return id - ((id - 1) % script->chunk_entries);
}

// 载入 id 所在的块(已经是当前驻留块则直接返回)。
static bool load_chunk(lime_script_t *script, uint32_t id)
{
    if (id < 1 || id > script->entries) return false;
    if (!script->cache || script->cache_cap < script->max_chunk_raw) return false;

    uint32_t index = (id - 1) / script->chunk_entries;
    if (index >= script->chunks) return false;
    uint32_t first_id = chunk_first_id(script, id);
    if (script->cache_first_id == first_id && script->cache_first_id != 0) return true;

    const uint8_t *chunk = script->chunk_tab + index * CHUNK_ENTRY_SIZE;
    uint32_t off = rd32(chunk);
    uint32_t size = rd32(chunk + 4);
    uint32_t count = rd16(chunk + 12);
    uint16_t flags = rd16(chunk + 14);
    if (off + size > script->text_sec_len) return false;

    uint32_t out_len = size;
    if (flags & CHUNK_FLAG_STORED) {
        if (size > script->cache_cap) return false;
        memcpy(script->cache, script->text_sec + off, size);
    } else {
        if (!script->inflate) return false;
        if (!script->inflate(script->text_sec + off, size, script->cache, script->cache_cap,
                             &out_len)) {
            return false;
        }
    }
    if (out_len > script->cache_cap) return false;
    script->cache_len = out_len;
    script->cache_first_id = rd32(chunk + 8);
    // 块内条数(最后一块可能少于 chunk_entries)。
    (void)count;
    return true;
}

bool lime_script_dialogue(lime_script_t *script, uint32_t id, lime_dialogue_t *out)
{
    if (!script || !out) return false;
    uint32_t first_id = chunk_first_id(script, id);
    if (!load_chunk(script, id)) return false;

    const uint8_t *p = script->cache;
    const uint8_t *end = script->cache + script->cache_len;
    for (uint32_t current = first_id; current <= id; current++) {
        if (p >= end) return false;
        uint8_t flags = *p++;
        lime_dialogue_t record;
        memset(&record, 0, sizeof(record));
        record.flags = flags;
        record.bg = LIME_NAME_NONE;
        record.sprite = LIME_NAME_NONE;
        record.cg = LIME_NAME_NONE;
        record.speaker = LIME_NAME_NONE;
        if (flags & LIME_DLG_HAS_BG) {
            if (p + 2 > end) return false;
            record.bg = rd16(p);
            p += 2;
        }
        if (flags & LIME_DLG_HAS_SPRITE) {
            if (p + 2 > end) return false;
            record.sprite = rd16(p);
            p += 2;
        }
        if (flags & LIME_DLG_HAS_CG) {
            if (p + 2 > end) return false;
            record.cg = rd16(p);
            p += 2;
        }
        if (flags & LIME_DLG_HAS_SPEAKER) {
            if (p + 2 > end) return false;
            record.speaker = rd16(p);
            p += 2;
        }
        if (flags & LIME_DLG_HAS_TEXT) {
            if (p + 2 > end) return false;
            uint16_t len = rd16(p);
            p += 2;
            if (p + len > end) return false;
            record.text = (const char *)p;
            record.text_len = len;
            p += len;
        }
        if (current == id) {
            *out = record;
            return true;
        }
    }
    return false;
}

bool lime_script_name(const lime_script_t *script, uint16_t index, const char **text,
                      uint16_t *len)
{
    if (!script || index >= script->name_count) return false;
    const uint8_t *entry = script->name_tab + index * NAME_ENTRY_SIZE;
    uint32_t off = rd32(entry);
    uint16_t size = rd16(entry + 4);
    if (off + size > script->name_text_len) return false;
    if (text) *text = (const char *)(script->name_text + off);
    if (len) *len = size;
    return true;
}

uint32_t lime_script_chapters(const lime_script_t *script)
{
    return script ? script->chapter_count : 0;
}

bool lime_script_chapter(const lime_script_t *script, uint32_t index, uint32_t *first_id,
                         uint16_t *name_index)
{
    if (!script || index >= script->chapter_count) return false;
    const uint8_t *entry = script->chapter_tab + index * CHAPTER_ENTRY_SIZE;
    if (first_id) *first_id = rd32(entry);
    if (name_index) *name_index = rd16(entry + 4);
    return true;
}

uint32_t lime_script_chapter_of(const lime_script_t *script, uint32_t id)
{
    if (!script || script->chapter_count == 0) return 0;
    uint32_t found = 0;
    for (uint32_t i = 0; i < script->chapter_count; i++) {
        const uint8_t *entry = script->chapter_tab + i * CHAPTER_ENTRY_SIZE;
        if (rd32(entry) <= id) {
            found = i;
        } else {
            break;
        }
    }
    return found;
}

uint32_t lime_script_choices(const lime_script_t *script)
{
    return script ? script->choice_count : 0;
}

// 遍历选项表;命中 wanted_id 或第 wanted_index 项时返回。
static bool find_choice(const lime_script_t *script, uint32_t wanted_index, uint32_t wanted_id,
                        bool by_id, uint32_t *id, uint8_t *count,
                        lime_choice_option_t *options, uint8_t capacity)
{
    const uint8_t *p = script->choice_tab;
    const uint8_t *end = script->choice_tab + script->choice_tab_len;
    for (uint32_t i = 0; i < script->choice_count; i++) {
        if (p + CHOICE_ENTRY_SIZE > end) return false;
        uint32_t entry_id = rd32(p);
        uint8_t options_count = p[4];
        p += CHOICE_ENTRY_SIZE;
        if (options_count > 4 || p + (uint32_t)options_count * CHOICE_OPTION_SIZE > end) return false;
        bool hit = by_id ? (entry_id == wanted_id) : (i == wanted_index);
        if (hit) {
            if (count) *count = options_count;
            if (id) *id = entry_id;
            if (options) {
                uint8_t n = options_count < capacity ? options_count : capacity;
                for (uint8_t k = 0; k < n; k++) {
                    const uint8_t *option = p + (uint32_t)k * CHOICE_OPTION_SIZE;
                    options[k].name = rd16(option);
                    options[k].target = rd32(option + 2);
                }
            }
            return true;
        }
        p += (uint32_t)options_count * CHOICE_OPTION_SIZE;
    }
    return false;
}

bool lime_script_choice(const lime_script_t *script, uint32_t index, uint32_t *id,
                        uint8_t *count, lime_choice_option_t *options, uint8_t capacity)
{
    if (!script || index >= script->choice_count) return false;
    return find_choice(script, index, 0, false, id, count, options, capacity);
}

bool lime_script_choice_for(const lime_script_t *script, uint32_t id, uint8_t *count,
                            lime_choice_option_t *options, uint8_t capacity)
{
    if (!script) return false;
    return find_choice(script, 0, id, true, NULL, count, options, capacity);
}
