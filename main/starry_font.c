// main/starry_font.c —— 字形包解析(纯逻辑,无 ESP-IDF 依赖)。
#include "starry_font.h"

#include <string.h>

#define FONT_HEADER_SIZE 40u
#define FONT_CMAP_ENTRY 8u
#define FONT_DSC_ENTRY 12u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool starry_font_open(starry_font_t *font, const uint8_t *data, uint32_t size)
{
    if (!font || !data || size < FONT_HEADER_SIZE) return false;
    if (memcmp(data, STARRY_FONT_MAGIC, 8) != 0) return false;
    if (rd32(data + 8) != STARRY_FONT_VERSION) return false;
    if (rd32(data + 12) != size) return false;

    const uint32_t glyph_count = rd32(data + 28);
    const uint32_t bitmap_size = rd32(data + 32);
    if (glyph_count == 0 || glyph_count > 65536u) return false;

    const uint64_t cmap_size = (uint64_t)glyph_count * FONT_CMAP_ENTRY;
    const uint64_t dsc_size = (uint64_t)glyph_count * FONT_DSC_ENTRY;
    const uint64_t tables = (uint64_t)FONT_HEADER_SIZE + cmap_size + dsc_size;
    if (tables + bitmap_size != size) return false;

    font->blob = data;
    font->blob_size = size;
    font->px = (uint16_t)rd32(data + 16);
    font->line_height = (uint16_t)rd32(data + 20);
    font->ascent = (int16_t)rd32(data + 24);
    font->glyph_count = glyph_count;
    font->cmap = data + FONT_HEADER_SIZE;
    font->dsc = data + FONT_HEADER_SIZE + cmap_size;
    font->bitmap = data + FONT_HEADER_SIZE + cmap_size + dsc_size;
    font->bitmap_size = bitmap_size;
    return font->px > 0 && font->line_height >= font->px;
}

bool starry_font_glyph(const starry_font_t *font, uint32_t codepoint, starry_glyph_t *out_gl)
{
    if (!font || !out_gl || !font->cmap) return false;

    // cmap 按码位升序,二分查找。
    uint32_t lo = 0;
    uint32_t hi = font->glyph_count;
    uint32_t found = 0xFFFFFFFFu;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t cp = rd32(font->cmap + (size_t)mid * FONT_CMAP_ENTRY);
        if (cp == codepoint) {
            found = mid;
            break;
        }
        if (cp < codepoint) lo = mid + 1u;
        else hi = mid;
    }
    if (found == 0xFFFFFFFFu) return false;

    const uint16_t index = rd16(font->cmap + (size_t)found * FONT_CMAP_ENTRY + 4);
    if (index >= font->glyph_count) return false;
    const uint8_t *dsc = font->dsc + (size_t)index * FONT_DSC_ENTRY;
    const uint32_t off = rd32(dsc);
    const uint8_t w = dsc[4];
    const uint8_t h = dsc[5];
    if (w == 0 || h == 0) return false;
    const uint32_t need = (uint32_t)h * (((uint32_t)w + 1u) / 2u);
    if (off > font->bitmap_size || need > font->bitmap_size - off) return false;

    out_gl->rows = font->bitmap + off;
    out_gl->w = w;
    out_gl->h = h;
    out_gl->ofs_x = (int8_t)dsc[6];
    out_gl->ofs_y = (int8_t)dsc[7];
    out_gl->advance = dsc[8];
    return true;
}
