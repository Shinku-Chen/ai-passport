// main/limelight_assets.c —— 素材包解析(纯逻辑,不依赖 ESP-IDF)。
#include "limelight_assets.h"

#include <string.h>

#define HEADER_SIZE 32u
#define INDEX_SIZE 14u
#define SPELL_JPEG_LEN 4u          // 立绘条目后面追加的 JPEG 长度字段

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool lime_assets_open(lime_assets_t *assets, const uint8_t *blob, uint32_t size)
{
    if (!assets || !blob || size < HEADER_SIZE) return false;
    if (memcmp(blob, LIME_ASSET_MAGIC, 8) != 0) return false;
    if (rd32(blob + 8) != LIME_ASSET_VERSION) return false;

    lime_assets_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.blob = blob;
    parsed.size = size;
    parsed.entries = (uint16_t)rd32(blob + 12);
    parsed.blob_count = (uint16_t)rd32(blob + 16);
    parsed.quality = rd16(blob + 20);
    parsed.screen_w = rd16(blob + 22);
    parsed.screen_h = rd16(blob + 24);
    parsed.bg_rows = rd16(blob + 26);
    parsed.flags = rd32(blob + 28);
    if (parsed.entries == 0 || parsed.entries > 4096) return false;

    // 索引:逐条推进,顺便找出 meta 条目与名字表起点。
    uint32_t pos = HEADER_SIZE;
    uint32_t first_blob = 0;
    for (uint16_t i = 0; i < parsed.entries; i++) {
        if (pos + INDEX_SIZE > size) return false;
        const uint8_t *entry = blob + pos;
        uint32_t off = rd32(entry);
        uint32_t len = rd32(entry + 4);
        uint8_t kind = entry[12];
        pos += INDEX_SIZE;
        if (kind == LIME_ASSET_KIND_META) {
            parsed.meta_off = off;
            parsed.meta_len = len;
        } else if (kind != LIME_ASSET_KIND_IMAGE && kind != LIME_ASSET_KIND_SPRITE) {
            return false;
        } else if (kind == LIME_ASSET_KIND_SPRITE) {
            pos += SPELL_JPEG_LEN;
        }
        if (off < HEADER_SIZE || len == 0 || off + len > size) return false;
        if (first_blob == 0 || off < first_blob) first_blob = off;
    }
    if (parsed.meta_len == 0) return false;
    parsed.index_end = first_blob;

    // 名字表:meta 文本里 "[names]" 之后的每一行对应一个条目(顺序与索引一致)。
    const char *meta = (const char *)(blob + parsed.meta_off);
    uint32_t meta_len = parsed.meta_len;
    uint32_t at = 0;
    while (at + 8 <= meta_len) {
        if (memcmp(meta + at, "[names]\n", 8) == 0) {
            parsed.names_off = parsed.meta_off + at + 8;
            break;
        }
        at++;
    }
    if (parsed.names_off == 0) return false;
    parsed.names_end = meta_len > 0 && meta[meta_len - 1] == '\n'
                           ? parsed.meta_off + meta_len : parsed.meta_off + meta_len;
    *assets = parsed;
    return true;
}

uint16_t lime_assets_count(const lime_assets_t *assets)
{
    return assets ? assets->entries : 0;
}

bool lime_assets_get(const lime_assets_t *assets, uint16_t index, lime_asset_t *out)
{
    if (!assets || !out || index >= assets->entries) return false;
    uint32_t pos = HEADER_SIZE;
    for (uint16_t i = 0; i <= index; i++) {
        if (pos + INDEX_SIZE > assets->size) return false;
        const uint8_t *entry = assets->blob + pos;
        uint32_t off = rd32(entry);
        uint32_t len = rd32(entry + 4);
        uint16_t w = rd16(entry + 8);
        uint16_t h = rd16(entry + 10);
        uint8_t kind = entry[12];
        uint8_t flags = entry[13];
        pos += INDEX_SIZE;
        uint32_t jpeg_len = 0;
        if (kind == LIME_ASSET_KIND_SPRITE) {
            if (pos + SPELL_JPEG_LEN > assets->size) return false;
            jpeg_len = rd32(assets->blob + pos);
            pos += SPELL_JPEG_LEN;
        }
        if (i != index) continue;
        out->off = off;
        out->len = len;
        out->w = w;
        out->h = h;
        out->kind = kind;
        out->flags = flags;
        out->jpeg_len = jpeg_len;
        return true;
    }
    return false;
}

// 取名字表第 index 行的(家族, 基名)两个字段,返回行的起点。
static const char *name_line(const lime_assets_t *assets, uint16_t index, uint32_t *line_len)
{
    if (!assets || index >= assets->entries) return NULL;
    uint32_t at = assets->names_off;
    for (uint16_t i = 0; i < index; i++) {
        while (at < assets->names_end && assets->blob[at] != '\n') at++;
        if (at >= assets->names_end) return NULL;
        at++;
    }
    uint32_t start = at;
    while (at < assets->names_end && assets->blob[at] != '\n') at++;
    if (start >= assets->names_end) return NULL;
    if (line_len) *line_len = at - start;
    return (const char *)(assets->blob + start);
}

const char *lime_assets_family(const lime_assets_t *assets, uint16_t index, uint16_t *len)
{
    uint32_t line_len = 0;
    const char *line = name_line(assets, index, &line_len);
    if (!line) return NULL;
    uint32_t cut = 0;
    while (cut < line_len && line[cut] != '\t') cut++;
    if (len) *len = (uint16_t)cut;
    return line;
}

const char *lime_assets_name(const lime_assets_t *assets, uint16_t index, uint16_t *len)
{
    if (!assets || index >= assets->entries) return NULL;
    uint32_t at = assets->names_off;
    for (uint16_t i = 0; i < index; i++) {
        while (at < assets->names_end && assets->blob[at] != '\n') at++;
        if (at >= assets->names_end) return NULL;
        at++;
    }
    uint32_t start = at;
    while (at < assets->names_end && assets->blob[at] != '\n') at++;
    if (at >= assets->names_end && start >= assets->names_end) return NULL;
    uint32_t line_len = at - start;
    // 跳过 "家族\t" 前缀。
    uint32_t cut = 0;
    while (cut < line_len && assets->blob[start + cut] != '\t') cut++;
    if (cut < line_len) {
        start += cut + 1;
        line_len -= cut + 1;
    }
    if (len) *len = (uint16_t)line_len;
    return (const char *)(assets->blob + start);
}

static bool name_matches(const char *line, uint32_t line_len, const char *name)
{
    uint32_t i = 0;
    for (; i < line_len && name[i] && name[i] != '.'; i++) {
        if (line[i] != name[i]) return false;
    }
    if (i != line_len) return false;
    return name[i] == '\0' || name[i] == '.';
}

int lime_assets_find(const lime_assets_t *assets, const char *name)
{
    if (!assets || !name || !*name) return -1;
    for (uint16_t i = 0; i < assets->entries; i++) {
        uint16_t len = 0;
        const char *line = lime_assets_name(assets, i, &len);
        if (!line) continue;
        if (name_matches(line, len, name)) return (int)i;
    }
    return -1;
}

const uint8_t *lime_assets_jpeg(const lime_assets_t *assets, const lime_asset_t *asset,
                                uint32_t *len)
{
    if (!assets || !asset) return NULL;
    uint32_t jpeg_len = asset->kind == LIME_ASSET_KIND_SPRITE ? asset->jpeg_len : asset->len;
    if (jpeg_len == 0 || asset->off + jpeg_len > assets->size) return NULL;
    if (len) *len = jpeg_len;
    return assets->blob + asset->off;
}

const uint8_t *lime_assets_mask(const lime_assets_t *assets, const lime_asset_t *asset,
                               uint32_t *len)
{
    if (!assets || !asset || asset->kind != LIME_ASSET_KIND_SPRITE) return NULL;
    if (asset->jpeg_len >= asset->len) return NULL;
    uint32_t mask_len = asset->len - asset->jpeg_len;
    if (asset->off + asset->len > assets->size) return NULL;
    if (len) *len = mask_len;
    return assets->blob + asset->off + asset->jpeg_len;
}

uint32_t lime_mask_raw_len(uint16_t w, uint16_t h)
{
    return (uint32_t)((w + 7u) / 8u) * h;
}

bool lime_mask_decode(const uint8_t *mask, uint32_t mask_len, uint16_t w, uint16_t h,
                      uint8_t *out, uint32_t out_len)
{
    if (!mask || !out || mask_len < 4) return false;
    uint32_t raw_len = rd32(mask);
    if (raw_len != lime_mask_raw_len(w, h) || raw_len != out_len) return false;
    uint32_t written = 0;
    uint32_t at = 4;
    while (written < raw_len) {
        if (at + 1 >= mask_len) return false;
        uint32_t run = mask[at];
        uint8_t value = mask[at + 1];
        at += 2;
        if (run == 0 || written + run > raw_len) return false;
        memset(out + written, value, run);
        written += run;
    }
    return true;
}
