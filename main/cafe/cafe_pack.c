// main/cafe/cafe_pack.c —— Cafe Stella 资源包读取/校验实现。
#include "cafe_pack.h"

#include "esp_log.h"

#include <string.h>
#include "cafe_compat.h"

static const char *TAG = "cafe_pack";

#define HEADER_SIZE 128
#define SECTION_STRIDE 24
#define NODE_SIZE 36
#define ASSET_SIZE 26
#define IMAGE_SIZE 36
#define BLOCK_SIZE 20
#define CHOICE_SIZE 12
#define WHEN_SIZE 32
#define CHAPTER_SIZE 12
#define AFTER_SIZE 12
#define ENDING_SIZE 16
#define GALLERY_SIZE 16
#define RULE_SIZE 4
#define ROUTE_SIZE 8
#define UI_IMAGE_SIZE 8
#define ALIAS_SIZE 4
#define AUDIO_SIZE 24
#define OP_SIZE 24

typedef struct {
    uint32_t type;
    uint32_t off;
    uint32_t size;
    uint32_t count;
    uint32_t crc;
} section_t;

static const uint8_t *s_data;
static size_t s_size;
static section_t s_sections[32];
static int s_section_count;
static bool s_ready;
static uint32_t s_source_sha_prefix;
static uint8_t s_text_cache[CAFE_TEXT_CACHE_SIZE];
static uint16_t s_text_cache_block = 0xFFFF;
static bool s_text_cache_valid;

#if defined(ESP_PLATFORM)
extern const uint8_t cafe_pak_blob_start[] asm("_binary_cafe_pak_start");
extern const uint8_t cafe_pak_blob_end[] asm("_binary_cafe_pak_end");
#endif

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint16_t cafe_meta_u16(const uint8_t *meta, unsigned index)
{
    if (!meta) return 0;
    return rd16(meta + index * 2);
}

static const section_t *find_section(uint32_t type)
{
    for (int i = 0; i < s_section_count; ++i) {
        if (s_sections[i].type == type) return &s_sections[i];
    }
    return NULL;
}

static bool section_ok(const section_t *sec)
{
    return sec && sec->off <= s_size && sec->size <= s_size - sec->off;
}

static bool check_section_crc(const section_t *sec)
{
    if (!section_ok(sec)) return false;
    const uint32_t got = (uint32_t)cafe_crc32(0, s_data + sec->off, sec->size);
    return got == sec->crc;
}

static bool validate_header(void)
{
    if (!s_data || s_size < HEADER_SIZE) return false;
    if (memcmp(s_data, CAFE_PACK_MAGIC, 8) != 0) return false;
    if (rd32(s_data + 8) != CAFE_PACK_VERSION) return false;
    if (rd32(s_data + 12) != HEADER_SIZE) return false;
    const uint32_t file_size = rd32(s_data + 16);
    const uint32_t section_count = rd32(s_data + 20);
    if (file_size > s_size || section_count > 32) return false;
    s_size = file_size;   // 只信任包头声明的长度，尾随字节不参与读取
    s_source_sha_prefix = rd32(s_data + 24);
    s_section_count = (int)section_count;
    const size_t table_end = HEADER_SIZE + (size_t)section_count * SECTION_STRIDE;
    if (table_end > s_size) return false;
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *p = s_data + HEADER_SIZE + i * SECTION_STRIDE;
        s_sections[i].type = rd32(p + 0);
        s_sections[i].off = rd32(p + 4);
        s_sections[i].size = rd32(p + 8);
        s_sections[i].count = rd32(p + 12);
        s_sections[i].crc = rd32(p + 16);
        if (s_sections[i].off < table_end || !section_ok(&s_sections[i])) {
            ESP_LOGE(TAG, "section %u 越界", (unsigned)i);
            return false;
        }
    }
    // 小表在启动时校验；正文/图片数据在各自读取时校验，避免启动延迟。
    static const uint32_t eager[] = {
        CAFE_SEC_META, CAFE_SEC_NODES, CAFE_SEC_NEXT, CAFE_SEC_WHEN, CAFE_SEC_CHOICE,
        CAFE_SEC_COMPLETE, CAFE_SEC_RULE, CAFE_SEC_FLAGS, CAFE_SEC_CHAPTERS,
        CAFE_SEC_AFTER, CAFE_SEC_ENDINGS, CAFE_SEC_GALLERY, CAFE_SEC_GALLERY_VAR,
        CAFE_SEC_ASSETS, CAFE_SEC_STRINGS, CAFE_SEC_TEXT_BLOCKS, CAFE_SEC_IMAGES,
        CAFE_SEC_ROUTES, CAFE_SEC_PROGRESS, CAFE_SEC_UI, CAFE_SEC_GALLERY_ALIAS,
    };
    for (size_t i = 0; i < sizeof(eager) / sizeof(eager[0]); ++i) {
        const section_t *sec = find_section(eager[i]);
        if (!sec || !check_section_crc(sec)) {
            ESP_LOGE(TAG, "section %u 缺失或 CRC 错误", (unsigned)eager[i]);
            return false;
        }
    }
    // New JPEG descriptor sections are optional for old packs, but when
    // present their boundary/CRC checks are mandatory.
    static const uint32_t optional[] = { CAFE_SEC_JPEG_POOL, CAFE_SEC_JPEG_DESC };
    for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); ++i) {
        const section_t *sec = find_section(optional[i]);
        if (sec && !check_section_crc(sec)) {
            ESP_LOGE(TAG, "section %u CRC 错误", (unsigned)optional[i]);
            return false;
        }
    }
    return true;
}

bool cafe_pack_set_data(const void *data, size_t size)
{
    cafe_pack_shutdown();
    s_data = (const uint8_t *)data;
    s_size = size;
    s_ready = validate_header();
    if (!s_ready) {
        s_data = NULL;
        s_size = 0;
        s_section_count = 0;
        ESP_LOGE(TAG, "资源包头/索引校验失败(大小 %u)", (unsigned)size);
    }
    return s_ready;
}

bool cafe_pack_init(void)
{
#if defined(ESP_PLATFORM)
    const size_t size = (size_t)(cafe_pak_blob_end - cafe_pak_blob_start);
    if (size < HEADER_SIZE) {
        ESP_LOGE(TAG, "cafe.pak 未链接进固件");
        return false;
    }
    const bool ok = cafe_pack_set_data(cafe_pak_blob_start, size);
    if (ok) {
        ESP_LOGI(TAG, "资源包 %u 字节, source %08x", (unsigned)s_size,
                 (unsigned)s_source_sha_prefix);
    }
    return ok;
#else
    return false;
#endif
}

void cafe_pack_shutdown(void)
{
    s_data = NULL;
    s_size = 0;
    s_section_count = 0;
    s_ready = false;
    s_text_cache_block = 0xFFFF;
    s_text_cache_valid = false;
    s_source_sha_prefix = 0;
}

bool cafe_pack_ready(void)
{
    return s_ready;
}

uint32_t cafe_pack_source_sha_prefix(void)
{
    return s_source_sha_prefix;
}

uint32_t cafe_meta_u32(unsigned index)
{
    const section_t *meta = find_section(CAFE_SEC_META);
    if (!meta || meta->size < 64 || index >= 16) return 0;
    return rd32(s_data + meta->off + index * 4);
}

const char *cafe_pack_string(uint32_t off)
{
    const section_t *strings = find_section(CAFE_SEC_STRINGS);
    if (!strings || off >= strings->size) return "";
    const uint8_t *p = s_data + strings->off + off;
    const uint8_t *end = s_data + strings->off + strings->size;
    if (memchr(p, 0, (size_t)(end - p)) == NULL) return "";
    return (const char *)p;
}

bool cafe_node_get(uint16_t id, cafe_node_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_NODES);
    if (!out || !sec || id >= sec->count || sec->size < (uint32_t)(id + 1) * NODE_SIZE) {
        return false;
    }
    const uint8_t *p = s_data + sec->off + (size_t)id * NODE_SIZE;
    out->id = rd16(p + 0);
    out->frames = rd16(p + 2);
    out->block_first = rd16(p + 4);
    out->block_count = rd16(p + 6);
    out->next_first = rd16(p + 8);
    out->choice_first = rd16(p + 10);
    out->complete_first = rd16(p + 12);
    out->next_count = p[14];
    out->choice_count = p[15];
    out->complete_count = p[16];
    out->route = p[17];
    out->adult = p[18];
    out->after = p[19];
    out->title_off = rd32(p + 24);
    out->chapter_off = rd32(p + 28);
    out->source_off = rd32(p + 32);
    return out->id == id;
}

bool cafe_next_get(uint16_t index, cafe_next_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_NEXT);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * 4;
    out->to = rd16(p);
    out->when = rd16(p + 2);
    return true;
}

bool cafe_when_get(uint16_t index, cafe_when_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_WHEN);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * WHEN_SIZE;
    out->count = p[0];
    out->negate = p[1];
    out->check_any = p[2];
    if (out->count > 7) return false;
    memcpy(out->comp, p + 4, 28);
    return true;
}

bool cafe_choice_get(uint16_t index, cafe_choice_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_CHOICE);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * CHOICE_SIZE;
    out->text_off = rd32(p);
    out->to = rd16(p + 4);
    out->flag = p[6];
    out->value = p[7];
    return true;
}

bool cafe_complete_get(uint16_t index, uint16_t *bit)
{
    const section_t *sec = find_section(CAFE_SEC_COMPLETE);
    if (!bit || !sec || index >= sec->count) return false;
    *bit = rd16(s_data + sec->off + (size_t)index * 2);
    return true;
}

bool cafe_rule_get(uint16_t index, uint8_t *key, uint8_t *cmp_flag,
                   uint8_t *value, uint8_t *add)
{
    const section_t *sec = find_section(CAFE_SEC_RULE);
    if (!key || !cmp_flag || !value || !add || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * RULE_SIZE;
    *key = p[0];
    *cmp_flag = p[1];
    *value = p[2];
    *add = p[3];
    return true;
}

bool cafe_chapter_get(uint16_t index, cafe_chapter_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_CHAPTERS);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * CHAPTER_SIZE;
    out->node = rd16(p);
    out->route = p[2];
    out->adult = p[3];
    out->title_off = rd32(p + 4);
    out->chapter_off = rd32(p + 8);
    return true;
}

bool cafe_after_get(uint16_t index, cafe_after_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_AFTER);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * AFTER_SIZE;
    out->node = rd16(p);
    out->requires = p[2];
    out->adult = p[3];
    out->title_off = rd32(p + 4);
    return true;
}

bool cafe_ending_get(uint16_t index, cafe_ending_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_ENDINGS);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * ENDING_SIZE;
    out->bit = p[0];
    out->kind = p[1];
    // Ending records are [bit, kind] followed immediately by title/body offsets.
    out->title_off = rd32(p + 2);
    out->body_off = rd32(p + 6);
    return true;
}

bool cafe_gallery_get(uint16_t index, cafe_gallery_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_GALLERY);
    if (!out || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * GALLERY_SIZE;
    out->first_variant = rd16(p);
    out->variant_count = rd16(p + 2);
    out->section = p[4];
    out->adult = p[5];
    // Packer layout: 2+2+1+1 then unaligned u32 label, u16 first_node,
    // then two reserved u16.  Do not use the padded C-struct offsets here.
    out->label_off = rd32(p + 6);
    out->first_node = rd16(p + 10);
    return true;
}

bool cafe_gallery_variant_get(uint16_t index, uint16_t *asset)
{
    const section_t *sec = find_section(CAFE_SEC_GALLERY_VAR);
    if (!asset || !sec || index >= sec->count) return false;
    *asset = rd16(s_data + sec->off + (size_t)index * 2);
    return true;
}

bool cafe_asset_get(uint16_t id, cafe_asset_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_ASSETS);
    if (!out || !sec || id >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)id * ASSET_SIZE;
    out->image_id = rd16(p + 0);
    out->patch_image_id = rd16(p + 2);
    out->width = rd16(p + 4);
    out->height = rd16(p + 6);
    out->patch_x = rd16(p + 8);
    out->patch_y = rd16(p + 10);
    out->patch_w = rd16(p + 12);
    out->patch_h = rd16(p + 14);
    out->atlas_x = rd16(p + 16);
    out->atlas_y = rd16(p + 18);
    out->atlas_w = rd16(p + 20);
    out->atlas_h = rd16(p + 22);
    out->kind = p[24];
    out->reserved = p[25];
    return true;
}

bool cafe_image_get(uint16_t id, cafe_image_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_IMAGES);
    if (!out || !sec || id >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)id * IMAGE_SIZE;
    out->id = rd16(p + 0);
    out->kind = p[2];
    out->flags = p[3];
    out->w = rd16(p + 4);
    out->h = rd16(p + 6);
    out->off = rd32(p + 8);
    out->len = rd32(p + 12);
    out->mask_off = rd32(p + 16);
    out->mask_len = rd32(p + 20);
    out->mask_raw = rd32(p + 24);
    out->gallery = rd16(p + 28);
    out->crc = rd32(p + 32);
    return out->id == id;
}

bool cafe_audio_track_get(uint8_t id, cafe_audio_track_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_AUDIO);
    const section_t *data = find_section(CAFE_SEC_AUDIO_DATA);
    if (!out || !sec || !data || id >= sec->count || !section_ok(data) ||
        !check_section_crc(sec) || !check_section_crc(data)) return false;
    const uint8_t *p = s_data + sec->off + (size_t)id * AUDIO_SIZE;
    out->id = p[0];
    out->kind = p[1];
    out->sample_rate = rd16(p + 2);
    out->channels = p[4];
    out->bits = p[5];
    out->block_align = rd16(p + 6);
    out->samples = rd32(p + 8);
    out->data_off = rd32(p + 12);
    out->data_len = rd32(p + 16);
    out->crc = rd32(p + 20);
    if (out->id != id || out->data_off > data->size ||
        out->data_len > data->size - out->data_off) return false;
    if (out->sample_rate < 4000 || out->sample_rate > 48000 ||
        out->channels != 1 || out->bits != 4 || out->block_align <= 4) return false;
    const uint32_t crc = (uint32_t)cafe_crc32(
        0, s_data + data->off + out->data_off, out->data_len);
    return crc == out->crc;
}

const uint8_t *cafe_audio_data(const cafe_audio_track_t *track, uint32_t *len)
{
    const section_t *data = find_section(CAFE_SEC_AUDIO_DATA);
    if (!track || !data || !section_ok(data) || track->data_off > data->size ||
        track->data_len > data->size - track->data_off) return NULL;
    if (len) *len = track->data_len;
    return s_data + data->off + track->data_off;
}

bool cafe_op_get(cafe_op_t *out)
{
    const section_t *sec = find_section(CAFE_SEC_OP);
    if (!out || !sec || sec->count != 1 || sec->size < OP_SIZE || !check_section_crc(sec)) return false;
    const uint8_t *p = s_data + sec->off;
    out->first_image = rd16(p + 0);
    out->frames = rd16(p + 2);
    out->fps = rd16(p + 4);
    out->audio_id = p[6];
    out->flags = p[7];
    out->duration_ms = rd32(p + 8);
    out->audio_guard_ms = rd32(p + 12);
    out->audio_content_ms = rd32(p + 16);
    out->audio_samples = rd32(p + 20);
    if (out->frames == 0 || out->fps == 0 || out->first_image + out->frames > 0xFFFF) return false;
    return true;
}

uint16_t cafe_op_image_id(uint16_t frame)
{
    cafe_op_t op;
    if (!cafe_op_get(&op) || frame >= op.frames) return CAFE_IMAGE_NONE;
    return (uint16_t)(op.first_image + frame);
}

bool cafe_ui_image_get(uint16_t index, const char **name, uint16_t *image_id)
{
    const section_t *sec = find_section(CAFE_SEC_UI);
    if (!name || !image_id || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * UI_IMAGE_SIZE;
    *name = cafe_pack_string(rd32(p));
    *image_id = rd16(p + 4);
    return true;
}

bool cafe_route_get(uint16_t index, const char **label, uint8_t *id, uint8_t *adult)
{
    const section_t *sec = find_section(CAFE_SEC_ROUTES);
    if (!label || !id || !adult || !sec || index >= sec->count) return false;
    const uint8_t *p = s_data + sec->off + (size_t)index * ROUTE_SIZE;
    *label = cafe_pack_string(rd32(p));
    *id = p[4];
    *adult = p[5];
    return true;
}

bool cafe_progress_label_get(uint16_t index, const char **label)
{
    const section_t *sec = find_section(CAFE_SEC_PROGRESS);
    if (!label || !sec || index >= sec->count) return false;
    *label = cafe_pack_string(rd32(s_data + sec->off + (size_t)index * 4));
    return true;
}

static bool jpeg_pool_segment(const uint8_t *pool, uint32_t pool_size, uint16_t ref,
                              const uint8_t **data, uint32_t *len)
{
    if (!pool || pool_size < 4 || !data || !len) return false;
    const uint32_t count = rd32(pool);
    if (count == 0 || count > 0x7FFFu) return false;
    const size_t table = 4u + (size_t)(count + 1) * 4u;
    if (table > pool_size || ref >= count) return false;
    const uint32_t start = rd32(pool + 4 + (size_t)ref * 4);
    const uint32_t end = rd32(pool + 4 + (size_t)(ref + 1) * 4);
    if (start < table || end < start || end > pool_size) return false;
    *data = pool + start;
    *len = end - start;
    return true;
}

bool cafe_image_jpeg(const cafe_image_t *img, cafe_jpeg_t *out)
{
    if (!img || !out) return false;
    const section_t *pool = find_section(CAFE_SEC_JPEG_POOL);
    const section_t *desc = find_section(CAFE_SEC_JPEG_DESC);
    const section_t *data = find_section(CAFE_SEC_IMAGE_DATA);
    if (!pool || !desc || !data || img->id >= desc->count ||
        img->off > data->size || img->len > data->size - img->off) return false;
    if (desc->size < 4 + (size_t)desc->count * 4u) return false;
    const uint32_t off = rd32(s_data + desc->off + 4 + (size_t)img->id * 4u);
    if (off > desc->size || desc->size - off < 2) return false;
    const uint16_t parts = rd16(s_data + desc->off + off);
    if ((size_t)parts > (desc->size - off - 2) / 2) return false;
    const uint8_t *part_data = s_data + desc->off + off + 2;
    const uint8_t *literal = s_data + data->off + img->off;
    uint32_t literal_total = 0;
    for (uint16_t i = 0; i < parts; ++i) {
        const uint16_t v = rd16(part_data + (size_t)i * 2);
        if (v & 0x8000u) {
            literal_total += v & 0x7FFFu;
        } else {
            const uint8_t *seg = NULL;
            uint32_t seg_len = 0;
            if (!jpeg_pool_segment(s_data + pool->off, pool->size, v, &seg, &seg_len))
                return false;
        }
    }
    if (literal_total != img->len) return false;
    out->literal = literal;
    out->literal_len = img->len;
    out->parts = part_data;
    out->part_count = parts;
    out->pool = s_data + pool->off;
    out->pool_size = pool->size;
    out->part_index = 0;
    out->literal_pos = 0;
    return true;
}

bool cafe_jpeg_next(cafe_jpeg_t *stream, const uint8_t **data, uint32_t *len)
{
    if (!stream || !data || !len || !stream->parts) return false;
    if (stream->part_index >= stream->part_count) return false;
    const uint16_t v = rd16(stream->parts + (size_t)stream->part_index * 2);
    stream->part_index++;
    if (v & 0x8000u) {
        const uint32_t n = v & 0x7FFFu;
        if (n == 0 || stream->literal_pos > stream->literal_len ||
            n > stream->literal_len - stream->literal_pos) return false;
        *data = stream->literal + stream->literal_pos;
        *len = n;
        stream->literal_pos += n;
        return true;
    }
    return jpeg_pool_segment(stream->pool, stream->pool_size, v, data, len);
}

const uint8_t *cafe_image_data(const cafe_image_t *img, uint32_t *len)
{
    const section_t *sec = find_section(CAFE_SEC_IMAGE_DATA);
    if (!img || !len || !sec || img->off > sec->size || img->len > sec->size - img->off) {
        return NULL;
    }
    if (find_section(CAFE_SEC_JPEG_POOL) || find_section(CAFE_SEC_JPEG_DESC)) {
        return NULL;   // dedup JPEG is a segmented stream; use cafe_image_jpeg()
    }
    *len = img->len;
    return s_data + sec->off + img->off;
}

const uint8_t *cafe_image_mask_data(const cafe_image_t *img, uint32_t *len)
{
    const section_t *sec = find_section(CAFE_SEC_IMAGE_DATA);
    if (!img || !len || !sec || img->mask_off > sec->size ||
        img->mask_len > sec->size - img->mask_off) {
        return NULL;
    }
    *len = img->mask_len;
    return s_data + sec->off + img->mask_off;
}

static bool load_text_block(uint16_t block)
{
    if (s_text_cache_valid && s_text_cache_block == block) return true;
    s_text_cache_valid = false;
    const section_t *blocks = find_section(CAFE_SEC_TEXT_BLOCKS);
    const section_t *data = find_section(CAFE_SEC_TEXT_DATA);
    if (!blocks || !data || block >= blocks->count) return false;
    const uint8_t *p = s_data + blocks->off + (size_t)block * BLOCK_SIZE;
    const uint16_t raw = rd16(p + 6);
    const uint32_t off = rd32(p + 8);
    const uint32_t comp = rd32(p + 12);
    const uint32_t want_crc = rd32(p + 16);
    if (raw > CAFE_TEXT_CACHE_SIZE || off > data->size || comp > data->size - off) {
        ESP_LOGE(TAG, "文本块 %u 越界 raw=%u comp=%u", (unsigned)block,
                 (unsigned)raw, (unsigned)comp);
        return false;
    }
    const uint8_t *src = s_data + data->off + off;
    size_t dest_len = 0;
    if (!cafe_inflate(s_text_cache, sizeof(s_text_cache), &dest_len, src, comp) ||
        dest_len != raw) {
        ESP_LOGE(TAG, "文本块 %u 解压失败(%u/%u)", (unsigned)block,
                 (unsigned)dest_len, (unsigned)raw);
        return false;
    }
    if ((uint32_t)cafe_crc32(0, s_text_cache, raw) != want_crc) {
        ESP_LOGE(TAG, "文本块 %u CRC 错误", (unsigned)block);
        memset(s_text_cache, 0, sizeof(s_text_cache));
        return false;
    }
    s_text_cache_block = block;
    s_text_cache_valid = true;
    return true;
}

static bool parse_frame(uint16_t node, uint16_t index, cafe_frame_t *out)
{
    cafe_node_t n;
    if (!out || !cafe_node_get(node, &n) || index >= n.frames) return false;
    // 找到覆盖该帧的块。
    const section_t *blocks = find_section(CAFE_SEC_TEXT_BLOCKS);
    if (!blocks || n.block_first >= blocks->count) return false;
    uint16_t block = 0xFFFF;
    for (uint16_t i = 0; i < n.block_count; ++i) {
        const uint16_t bi = (uint16_t)(n.block_first + i);
        if (bi >= blocks->count) break;
        const uint8_t *bp = s_data + blocks->off + (size_t)bi * BLOCK_SIZE;
        const uint16_t first = rd16(bp + 2);
        const uint16_t count = rd16(bp + 4);
        if (index >= first && index < (uint32_t)first + count) {
            block = bi;
            break;
        }
    }
    if (block == 0xFFFF || !load_text_block(block)) return false;
    const uint8_t *bp = s_data + blocks->off + (size_t)block * BLOCK_SIZE;
    const uint16_t first = rd16(bp + 2);
    const uint16_t count = rd16(bp + 4);
    const uint8_t *p = s_text_cache;
    const uint8_t *end = s_text_cache + rd16(bp + 6);
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 8 > end) return false;
        const uint8_t kind = p[0];
        const uint16_t slen = rd16(p + 1);
        const uint16_t tlen = rd16(p + 3);
        const uint16_t bg = rd16(p + 5);
        const uint8_t sprites = p[7];
        p += 8;
        if (sprites > 8 || p + (size_t)sprites * 8 > end) return false;
        const uint8_t *sp = p;
        p += (size_t)sprites * 8;
        if (p + (size_t)slen + 1 + (size_t)tlen + 1 > end) return false;
        const char *speaker = (const char *)p;
        p += (size_t)slen + 1;
        const char *text = (const char *)p;
        p += (size_t)tlen + 1;
        if ((uint32_t)first + i == index) {
            out->valid = true;
            out->kind = kind;
            out->speaker = speaker;
            out->text = text;
            out->bg_asset = bg;
            out->sprite_count = sprites;
            out->sprites = (const cafe_sprite_t *)sp;
            return true;
        }
    }
    return false;
}

bool cafe_frame_get(uint16_t node, uint16_t index, cafe_frame_t *out)
{
    if (!s_ready || !out) return false;
    memset(out, 0, sizeof(*out));
    return parse_frame(node, index, out);
}

bool cafe_frame_kind(uint16_t node, uint16_t index, uint8_t *kind)
{
    cafe_frame_t f;
    if (!kind || !cafe_frame_get(node, index, &f)) return false;
    *kind = f.kind;
    return true;
}

bool cafe_frame_sprite(const cafe_frame_t *frame, unsigned index, cafe_sprite_t *out)
{
    if (!frame || !out || index >= frame->sprite_count) return false;
    memcpy(out, (const uint8_t *)frame->sprites + index * 8, 8);
    return true;
}

bool cafe_image_alias_get(uint16_t image_id, uint16_t index, uint16_t *group)
{
    const section_t *sec = find_section(CAFE_SEC_GALLERY_ALIAS);
    if (!group || !sec) return false;
    for (uint16_t i = 0; i < sec->count; ++i) {
        const uint8_t *p = s_data + sec->off + (size_t)i * ALIAS_SIZE;
        if (rd16(p) == image_id) {
            if (index == 0) {
                *group = rd16(p + 2);
                return true;
            }
            index--;
        }
    }
    return false;
}

uint16_t cafe_when_count(void)
{
    const section_t *sec = find_section(CAFE_SEC_WHEN);
    return sec ? (uint16_t)sec->count : 0;
}
