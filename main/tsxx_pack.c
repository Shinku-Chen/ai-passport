// main/tsxx_pack.c —— 资源包只读解析。不依赖 ESP-IDF / LVGL,可在宿主机上测试。
#include "tsxx_pack.h"

#include <stdio.h>
#include <string.h>

// 段类型,必须与 tools/tsxx_pack.py 的 SEC_* 一致。
enum {
    SEC_SYM = 0, SEC_TEXT, SEC_TOFF, SEC_TLEN,
    SEC_PBG, SEC_PSPK, SEC_PSPR, SEC_PFLAG,
    SEC_PCGB, SEC_PCG,
    SEC_BGNAME, SEC_SPKNAME, SEC_SPRNAME, SEC_CGNAME,
    SEC_CHOICE, SEC_CHOICEOPT,
    SEC_BG, SEC_FG, SEC_EVB, SEC_EVC, SEC_CGDIR,
    SEC_META, SEC_COUNT
};

#define ENTRY_BG 12u
#define ENTRY_FG 24u
#define ENTRY_EVB 12u
#define ENTRY_EVC 12u
#define ENTRY_CGDIR 20u
#define ENTRY_CHOICE 12u
#define ENTRY_CHOICEOPT 12u
#define HEADER_SIZE 20u
#define SECTION_ENTRY 16u

// 资源包的检查点间隔,必须与 tools/tsxx_pack.py 的 CHECKPOINT_PAGES 一致。
#define CHECKPOINT_PAGES 256u

static uint16_t rd16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// 位图前 page 页里有几张事件图。
static uint32_t bitmap_rank(const uint8_t *bits, uint32_t page)
{
    uint32_t rank = 0;
    const uint32_t whole = page >> 3;
    for (uint32_t i = 0; i < whole; ++i) {
        rank += (uint32_t)__builtin_popcount((unsigned)bits[i]);
    }
    for (uint32_t bit = 0; bit < (page & 7u); ++bit) {
        rank += (bits[whole] >> bit) & 1u;
    }
    return rank;
}

// 段表项:{ type u32, offset u32, count u32, size u32 }
static bool section_view(const uint8_t *blob, uint32_t size, uint32_t type,
                         const uint8_t **out, uint32_t *count, uint32_t *bytes)
{
    const uint8_t *table = blob + HEADER_SIZE;
    const uint32_t sections = rd32(blob + 16);
    if (type >= sections) {
        return false;
    }
    const uint8_t *entry = table + (type * SECTION_ENTRY);
    const uint32_t offset = rd32(entry + 4);
    const uint32_t items = rd32(entry + 8);
    const uint32_t length = rd32(entry + 12);
    if (offset > size || length > size - offset) {
        return false;
    }
    *out = blob + offset;
    *count = items;
    *bytes = length;
    return true;
}

// 带目录的段:[目录 count*stride][数据],目录项里的 off 相对数据区。
static bool blob_section(const uint8_t *section, uint32_t count, uint32_t stride,
                         const uint8_t **dir, const uint8_t **data, uint32_t *data_size,
                         uint32_t section_size)
{
    const uint32_t header = count * stride;
    if (section_size < header) {
        return false;
    }
    *dir = section;
    *data = section + header;
    *data_size = section_size - header;
    return true;
}

static bool meta_has_art(const uint8_t *meta, uint32_t size)
{
    // META 里有一行 art=<W>x<H>;与编译期常量不符说明包和固件不是同一次生成的。
    static const char key[] = "art=";
    char expect[16];
    const int expect_len = snprintf(expect, sizeof(expect), "%ux%u", TSXX_ART_W, TSXX_ART_H);
    for (uint32_t i = 0; i + sizeof(key) - 1 <= size; ++i) {
        if (memcmp(meta + i, key, sizeof(key) - 1) != 0) {
            continue;
        }
        const uint32_t start = i + sizeof(key) - 1;
        uint32_t end = start;
        while (end < size && meta[end] != '\n') {
            ++end;
        }
        const uint32_t len = end - start;
        return len == (uint32_t)expect_len && memcmp(meta + start, expect, len) == 0;
    }
    return false;
}

bool tsxx_pack_open(tsxx_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (pack == NULL || data == NULL || size < HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, TSXX_PACK_MAGIC, 8) != 0) {
        return false;
    }
    if (rd32(data + 8) != TSXX_PACK_VERSION) {
        return false;
    }
    if (rd32(data + 12) != size) {
        return false;
    }
    if (rd32(data + 16) != SEC_COUNT) {
        return false;
    }

    tsxx_pack_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;

    uint32_t count = 0;
    uint32_t bytes = 0;
    const uint8_t *section = NULL;

    if (!section_view(data, size, SEC_SYM, &view.syms, &view.sym_count, &bytes)) {
        return false;
    }
    if (view.sym_count == 0 || bytes != view.sym_count * 4u) {
        return false;
    }
    if (!section_view(data, size, SEC_TEXT, &view.text, &count, &view.text_size)) {
        return false;
    }
    if (!section_view(data, size, SEC_TOFF, &view.toff, &count, &bytes)) {
        return false;
    }
    if (!section_view(data, size, SEC_TLEN, &view.tlen, &view.page_count, &bytes)) {
        return false;
    }
    if (view.page_count == 0 || bytes != view.page_count) {
        return false;
    }
    if (count != (view.page_count + CHECKPOINT_PAGES - 1) / CHECKPOINT_PAGES + 1) {
        return false;
    }

    const struct {
        uint32_t type;
        const uint8_t **slot;
        uint32_t stride;
        bool bitmap;
    } arrays[] = {
        { SEC_PBG, &view.pbg, 1, false },
        { SEC_PSPK, &view.pspk, 1, false },
        { SEC_PSPR, &view.pspr, 1, false },
        { SEC_PFLAG, &view.pflag, 1, false },
        { SEC_PCGB, &view.pcgb, 1, true },
    };
    for (size_t i = 0; i < sizeof(arrays) / sizeof(arrays[0]); ++i) {
        if (!section_view(data, size, arrays[i].type, arrays[i].slot, &count, &bytes)) {
            return false;
        }
        const uint32_t expect = arrays[i].bitmap ? (view.page_count + 7) / 8
                                                 : view.page_count;
        if (count != expect || bytes != expect) {
            return false;
        }
    }
    if (!section_view(data, size, SEC_PCG, &view.pcg, &view.cg_page_count, &bytes)) {
        return false;
    }
    if (bytes != view.cg_page_count * 2u) {
        return false;
    }
    if (!section_view(data, size, SEC_BGNAME, &view.bg_names, &count, &view.bg_name_size) ||
        !section_view(data, size, SEC_SPKNAME, &view.spk_names, &count, &view.spk_name_size) ||
        !section_view(data, size, SEC_SPRNAME, &view.spr_names, &count, &view.spr_name_size) ||
        !section_view(data, size, SEC_CGNAME, &view.cg_names, &count, &view.cg_name_size)) {
        return false;
    }
    if (!section_view(data, size, SEC_CHOICE, &view.choices, &view.choice_count, &bytes)) {
        return false;
    }
    if (bytes != view.choice_count * ENTRY_CHOICE) {
        return false;
    }
    if (!section_view(data, size, SEC_CHOICEOPT, &view.choice_opts, &view.choice_opt_count,
                      &bytes)) {
        return false;
    }
    if (bytes != view.choice_opt_count * ENTRY_CHOICEOPT) {
        return false;
    }

    if (!section_view(data, size, SEC_BG, &section, &view.bg_count, &bytes) ||
        !blob_section(section, view.bg_count, ENTRY_BG, &view.bg_dir, &view.bg_data,
                      &view.bg_data_size, bytes)) {
        return false;
    }
    if (!section_view(data, size, SEC_FG, &section, &view.fg_count, &bytes) ||
        !blob_section(section, view.fg_count, ENTRY_FG, &view.fg_dir, &view.fg_data,
                      &view.fg_data_size, bytes)) {
        return false;
    }
    if (!section_view(data, size, SEC_EVB, &section, &view.evb_count, &bytes) ||
        !blob_section(section, view.evb_count, ENTRY_EVB, &view.evb_dir, &view.evb_data,
                      &view.evb_data_size, bytes)) {
        return false;
    }
    if (!section_view(data, size, SEC_EVC, &section, &view.evc_count, &bytes) ||
        !blob_section(section, view.evc_count, ENTRY_EVC, &view.evc_dir, &view.evc_data,
                      &view.evc_data_size, bytes)) {
        return false;
    }
    if (!section_view(data, size, SEC_CGDIR, &view.cg_dir, &view.cg_count, &bytes) ||
        bytes != view.cg_count * ENTRY_CGDIR) {
        return false;
    }
    if (!section_view(data, size, SEC_META, &view.meta, &count, &view.meta_size)) {
        return false;
    }
    if (!meta_has_art(view.meta, view.meta_size)) {
        return false;
    }

    *pack = view;
    return true;
}

uint32_t tsxx_pack_pages(const tsxx_pack_t *pack)
{
    return pack->page_count;
}

uint8_t tsxx_pack_bg_count(const tsxx_pack_t *pack)
{
    return (uint8_t)pack->bg_count;
}

uint8_t tsxx_pack_sprite_count(const tsxx_pack_t *pack)
{
    return (uint8_t)pack->fg_count;
}

uint16_t tsxx_pack_cg_count(const tsxx_pack_t *pack)
{
    return (uint16_t)pack->evb_count;
}

// 页正文在码流里的起始字节偏移:从最近的检查点按字符数走一遍。
static uint32_t text_offset(const tsxx_pack_t *pack, uint32_t page)
{
    const uint32_t block = page / CHECKPOINT_PAGES;
    uint32_t skip = 0;
    for (uint32_t i = block * CHECKPOINT_PAGES; i < page; ++i) {
        skip += pack->tlen[i];
    }
    uint32_t pos = rd32(pack->toff + block * 4u);
    while (skip > 0) {
        pos += (pack->text[pos] == 0) ? 3u : 1u;
        --skip;
    }
    return pos;
}

bool tsxx_pack_page(const tsxx_pack_t *pack, uint32_t index, tsxx_page_t *out)
{
    if (index >= pack->page_count) {
        return false;
    }
    out->bg = pack->pbg[index];
    out->speaker = pack->pspk[index];
    out->sprite = pack->pspr[index];
    out->flags = pack->pflag[index];
    out->text_len = pack->tlen[index];
    out->text_off = out->text_len ? text_offset(pack, index) : 0;
    out->has_cg = (pack->pcgb[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
    return true;
}

uint32_t tsxx_pack_cg_rank(const tsxx_pack_t *pack, uint32_t page)
{
    if (page > pack->page_count) {
        page = pack->page_count;
    }
    return bitmap_rank(pack->pcgb, page);
}

uint16_t tsxx_pack_cg_at(const tsxx_pack_t *pack, uint32_t rank)
{
    if (rank >= pack->cg_page_count) {
        return TSXX_NONE16;
    }
    return rd16(pack->pcg + rank * 2u);
}

uint32_t tsxx_pack_cg_total(const tsxx_pack_t *pack)
{
    return pack->cg_page_count;
}

// 解一个符号(1 字节码或 0x00 + u16),返回其 UTF-8 字节数与码位。
static uint32_t decode_symbol(const tsxx_pack_t *pack, uint32_t *pos, uint32_t *codepoint)
{
    const uint8_t code = pack->text[*pos];
    uint32_t id;
    if (code == 0) {
        id = rd16(pack->text + *pos + 1);
        *pos += 3;
    } else {
        id = (uint32_t)code - 1u;
        *pos += 1;
    }
    *codepoint = (id < pack->sym_count) ? rd32(pack->syms + id * 4u) : 0xFFFDu;
    return 1;
}

static size_t emit_utf8(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xC0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xE0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

size_t tsxx_pack_text(const tsxx_pack_t *pack, const tsxx_page_t *page, uint32_t skip,
                      uint32_t max_chars, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (page == NULL || page->text_len == 0 || skip >= page->text_len) {
        return 0;
    }
    uint32_t pos = page->text_off;
    uint32_t written = 0;
    const uint32_t limit = max_chars ? (skip + max_chars) : page->text_len;
    for (uint32_t i = 0; i < page->text_len; ++i) {
        uint32_t codepoint = 0;
        decode_symbol(pack, &pos, &codepoint);
        if (i < skip || i >= limit) {
            continue;
        }
        char encoded[4];
        const size_t len = emit_utf8(codepoint, encoded);
        if (written + len >= capacity) {   // 留出结尾 NUL
            break;
        }
        memcpy(out + written, encoded, len);
        written += len;
    }
    out[written] = '\0';
    return written;
}

size_t tsxx_pack_name(const tsxx_pack_t *pack, uint8_t table, uint16_t id, char *out,
                      size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    const uint8_t *blob = NULL;
    uint32_t size = 0;
    switch (table) {
    case TSXX_TABLE_BG:
        blob = pack->bg_names;
        size = pack->bg_name_size;
        break;
    case TSXX_TABLE_SPEAKER:
        blob = pack->spk_names;
        size = pack->spk_name_size;
        break;
    case TSXX_TABLE_SPRITE:
        blob = pack->spr_names;
        size = pack->spr_name_size;
        break;
    case TSXX_TABLE_EVENT:
        blob = pack->cg_names;
        size = pack->cg_name_size;
        break;
    default:
        return 0;
    }
    uint32_t index = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= size; ++i) {
        if (i != size && blob[i] != '\n') {
            continue;
        }
        if (index == id) {
            const uint32_t len = i - start;
            const uint32_t copy = (len < capacity - 1) ? len : (uint32_t)(capacity - 1);
            memcpy(out, blob + start, copy);
            out[copy] = '\0';
            return copy;
        }
        ++index;
        start = i + 1;
    }
    return 0;
}

static bool image_at(const uint8_t *dir, uint32_t stride, const uint8_t *data,
                     uint32_t data_size, uint32_t index, tsxx_image_t *out)
{
    const uint8_t *entry = dir + index * stride;
    const uint32_t offset = rd32(entry);
    const uint32_t length = rd32(entry + 4);
    if (length == 0 || offset > data_size || length > data_size - offset) {
        return false;
    }
    out->jpeg = data + offset;
    out->jpeg_len = length;
    out->w = rd16(entry + 8);
    out->h = rd16(entry + 10);
    return true;
}

bool tsxx_pack_bg(const tsxx_pack_t *pack, uint8_t id, tsxx_image_t *out)
{
    if (id >= pack->bg_count) {
        return false;
    }
    return image_at(pack->bg_dir, ENTRY_BG, pack->bg_data, pack->bg_data_size, id, out);
}

bool tsxx_pack_sprite(const tsxx_pack_t *pack, uint8_t id, tsxx_sprite_t *out)
{
    if (id >= pack->fg_count) {
        return false;
    }
    const uint8_t *entry = pack->fg_dir + (uint32_t)id * ENTRY_FG;
    const uint32_t body_off = rd32(entry);
    const uint32_t body_len = rd32(entry + 4);
    const uint32_t mask_off = rd32(entry + 8);
    const uint32_t mask_len = rd32(entry + 12);
    if (body_len == 0 || body_off > pack->fg_data_size ||
        body_len > pack->fg_data_size - body_off) {
        return false;
    }
    out->jpeg = pack->fg_data + body_off;
    out->jpeg_len = body_len;
    out->mask = pack->fg_data + mask_off;
    out->mask_len = mask_len;
    out->w = rd16(entry + 16);
    out->h = rd16(entry + 18);
    out->x = rd16(entry + 20);
    out->y = rd16(entry + 22);
    if (mask_len != ((uint32_t)(out->w + 7u) / 8u) * out->h ||
        mask_off > pack->fg_data_size || mask_len > pack->fg_data_size - mask_off) {
        return false;
    }
    return true;
}

bool tsxx_pack_cg(const tsxx_pack_t *pack, uint16_t id, tsxx_cg_t *out)
{
    if (pack->cg_dir == NULL || id >= pack->cg_count) {
        return false;
    }
    const uint8_t *entry = pack->cg_dir + (uint32_t)id * ENTRY_CGDIR;
    out->kind = entry[0];
    out->base = rd32(entry + 4);
    out->x = rd16(entry + 8);
    out->y = rd16(entry + 10);
    out->w = rd16(entry + 12);
    out->h = rd16(entry + 14);
    out->img = rd32(entry + 16);
    if (out->kind == TSXX_CG_FRAME) {
        return out->img < pack->evb_count;
    }
    if (out->kind == TSXX_CG_PATCH) {
        return out->img < pack->evc_count && out->base != id;
    }
    return false;
}

bool tsxx_pack_cg_image(const tsxx_pack_t *pack, const tsxx_cg_t *cg, tsxx_image_t *out)
{
    if (cg->kind == TSXX_CG_FRAME) {
        return image_at(pack->evb_dir, ENTRY_EVB, pack->evb_data, pack->evb_data_size,
                        cg->img, out);
    }
    if (cg->kind == TSXX_CG_PATCH) {
        return image_at(pack->evc_dir, ENTRY_EVC, pack->evc_data, pack->evc_data_size,
                        cg->img, out);
    }
    return false;
}

const char *tsxx_pack_meta(const tsxx_pack_t *pack)
{
    return (const char *)pack->meta;
}

// 选项表按页升序排列,用二分查找。
static const uint8_t *find_choice(const tsxx_pack_t *pack, uint32_t page)
{
    uint32_t low = 0;
    uint32_t high = pack->choice_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2u;
        const uint8_t *entry = pack->choices + mid * ENTRY_CHOICE;
        const uint32_t value = rd32(entry);
        if (value == page) {
            return entry;
        }
        if (value < page) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return NULL;
}

uint8_t tsxx_pack_choice_count(const tsxx_pack_t *pack, uint32_t page)
{
    const uint8_t *entry = find_choice(pack, page);
    return entry ? entry[4] : 0u;
}

bool tsxx_pack_choice_option(const tsxx_pack_t *pack, uint32_t page, uint8_t slot,
                             char *out, size_t capacity, uint32_t *target_page)
{
    const uint8_t *entry = find_choice(pack, page);
    if (entry == NULL) {
        return false;
    }
    const uint8_t count = entry[4];
    if (slot >= count) {
        return false;
    }
    const uint32_t first = rd32(entry + 8);
    if ((uint32_t)slot >= pack->choice_opt_count || first > pack->choice_opt_count - slot) {
        return false;
    }
    const uint8_t *option = pack->choice_opts + (first + slot) * ENTRY_CHOICEOPT;
    const uint32_t offset = rd32(option);
    const uint32_t length = option[4];
    const uint32_t target = rd32(option + 8);
    if (out != NULL && capacity > 0) {
        tsxx_page_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.text_len = (uint8_t)length;
        fake.text_off = offset;
        tsxx_pack_text(pack, &fake, 0, 0, out, capacity);
    }
    if (target_page != NULL) {
        *target_page = target;
    }
    return true;
}
