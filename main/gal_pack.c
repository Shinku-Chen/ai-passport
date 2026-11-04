// main/gal_pack.c —— 资源包解析与访问器(纯逻辑,不依赖 ESP-IDF)。
#include "gal_pack.h"

#include <string.h>

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t count;
    uint32_t size;
} gal_raw_section_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t count;
    uint32_t size;
} section_slot_t;


static bool in_blob(const gal_pack_t *pack, uint32_t off, uint32_t len)
{
    return off <= pack->size && len <= pack->size - off;
}

bool gal_pack_open(gal_pack_t *pack, const uint8_t *blob, uint32_t size, const char **err)
{
    const char *dummy = NULL;
    if (!err) err = &dummy;
    *err = NULL;
    if (!pack || !blob) { *err = "null"; return false; }
    memset(pack, 0, sizeof(*pack));
    if (size < 20) { *err = "too small"; return false; }
    if (memcmp(blob, GAL_PACK_MAGIC, 8) != 0) { *err = "bad magic"; return false; }
    uint32_t version, total, count;
    memcpy(&version, blob + 8, 4);
    memcpy(&total, blob + 12, 4);
    memcpy(&count, blob + 16, 4);
    if (version != GAL_PACK_VERSION) { *err = "bad version"; return false; }
    if (total != size) { *err = "size mismatch"; return false; }
    if (count != GAL_SEC_COUNT) {
        *err = "section count";
        return false;
    }
    if (20u + 16u * count > size) { *err = "table out of range"; return false; }
    pack->blob = blob;
    pack->size = size;

    section_slot_t slots[GAL_SEC_COUNT];
    uint8_t seen[GAL_SEC_COUNT];
    memset(slots, 0, sizeof(slots));
    memset(seen, 0, sizeof(seen));
    const uint32_t table_end = 20u + 16u * count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t hdr[4];
        memcpy(hdr, blob + 20 + 16 * i, sizeof(hdr));
        if (hdr[0] >= GAL_SEC_COUNT) { *err = "section id"; return false; }
        if (seen[hdr[0]]) { *err = "duplicate section"; return false; }
        seen[hdr[0]] = 1;
        if (hdr[1] < table_end) { *err = "section overlaps table"; return false; }
        if ((uint64_t)hdr[1] + hdr[3] > size) { *err = "section range"; return false; }
        slots[hdr[0]].type = hdr[0];
        slots[hdr[0]].offset = hdr[1];
        slots[hdr[0]].count = hdr[2];
        slots[hdr[0]].size = hdr[3];
    }
    // 重叠检查(独立一遍,读已记录的区间)。
    for (uint32_t a = 0; a < GAL_SEC_COUNT; ++a) {
        if (!seen[a] || slots[a].size == 0) continue;
        const uint64_t a0 = slots[a].offset, a1 = a0 + slots[a].size;
        for (uint32_t b = a + 1; b < GAL_SEC_COUNT; ++b) {
            if (!seen[b] || slots[b].size == 0) continue;
            const uint64_t b0 = slots[b].offset, b1 = b0 + slots[b].size;
            if (a0 < b1 && b0 < a1) { *err = "sections overlap"; return false; }
        }
    }
    // Only the four load-bearing sections must be non-empty; a title that uses no
    // per-choice actions legitimately produces empty ACTION/COND sections.
    if (slots[GAL_SEC_TEXT].size == 0 || slots[GAL_SEC_NAME].size == 0 ||
        slots[GAL_SEC_EVENT].size == 0 || slots[GAL_SEC_LINE].size == 0) {
        *err = "missing section";
        return false;
    }

    pack->text = blob + slots[GAL_SEC_TEXT].offset;
    pack->text_size = slots[GAL_SEC_TEXT].size;
    pack->name_tbl = blob + slots[GAL_SEC_NAME].offset;
    pack->name_count = slots[GAL_SEC_NAME].count;
    pack->events = (const gal_event_t *)(const void *)(blob + slots[GAL_SEC_EVENT].offset);
    pack->event_count = slots[GAL_SEC_EVENT].count;
    pack->lines = (const gal_line_t *)(const void *)(blob + slots[GAL_SEC_LINE].offset);
    pack->line_count = slots[GAL_SEC_LINE].count;
    pack->choices = (const gal_choice_t *)(const void *)(blob + slots[GAL_SEC_CHOICE].offset);
    pack->choice_count = slots[GAL_SEC_CHOICE].count;
    pack->options = (const gal_option_t *)(const void *)(blob + slots[GAL_SEC_OPTION].offset);
    pack->option_count = slots[GAL_SEC_OPTION].count;
    pack->actions = (const gal_action_t *)(const void *)(blob + slots[GAL_SEC_ACTION].offset);
    pack->action_count = slots[GAL_SEC_ACTION].count;
    pack->conds = (const gal_action_t *)(const void *)(blob + slots[GAL_SEC_COND].offset);
    pack->cond_count = slots[GAL_SEC_COND].count;
    pack->images = (const gal_pack_image_t *)(const void *)(blob + slots[GAL_SEC_IMAGE].offset);
    pack->image_count = slots[GAL_SEC_IMAGE].count;
    pack->chapters = (const gal_chapter_t *)(const void *)(blob + slots[GAL_SEC_CHAPTER].offset);
    pack->chapter_count = slots[GAL_SEC_CHAPTER].count;
    pack->endings = (const gal_ending_t *)(const void *)(blob + slots[GAL_SEC_ENDING].offset);
    pack->ending_count = slots[GAL_SEC_ENDING].count;
    pack->gallery = (const gal_gallery_t *)(const void *)(blob + slots[GAL_SEC_GALLERY].offset);
    pack->gallery_count = slots[GAL_SEC_GALLERY].count;
    if (slots[GAL_SEC_TITLE].size >= 2 && slots[GAL_SEC_TITLE].count >= 1) {
        memcpy(&pack->title_image, blob + slots[GAL_SEC_TITLE].offset, 2);
    } else {
        pack->title_image = 0;
    }

    // Basic record-size sanity: sections are arrays of fixed-size records.
    if (pack->event_count && slots[GAL_SEC_EVENT].size != pack->event_count * sizeof(gal_event_t)) {
        *err = "event size"; return false;
    }
    if (pack->line_count && slots[GAL_SEC_LINE].size != pack->line_count * sizeof(gal_line_t)) {
        *err = "line size"; return false;
    }
    if (pack->choice_count &&
        slots[GAL_SEC_CHOICE].size != pack->choice_count * sizeof(gal_choice_t)) {
        *err = "choice size"; return false;
    }
    if (pack->option_count &&
        slots[GAL_SEC_OPTION].size != pack->option_count * sizeof(gal_option_t)) {
        *err = "option size"; return false;
    }
    if (pack->image_count &&
        slots[GAL_SEC_IMAGE].size != pack->image_count * sizeof(gal_pack_image_t)) {
        *err = "image size"; return false;
    }
    if (pack->gallery_count &&
        slots[GAL_SEC_GALLERY].size != pack->gallery_count * sizeof(gal_gallery_t)) {
        *err = "gallery size"; return false;
    }
    // 定长表:size 必须正好等于 count * 记录长度。
    if (slots[GAL_SEC_NAME].size != pack->name_count * 8u) { *err = "name size"; return false; }
    if (slots[GAL_SEC_CHAPTER].size != pack->chapter_count * 8u) { *err = "chapter size"; return false; }
    if (slots[GAL_SEC_ENDING].size != pack->ending_count * 8u) { *err = "ending size"; return false; }
    if (slots[GAL_SEC_GALLERY].size != pack->gallery_count * 8u) { *err = "gallery size"; return false; }
    if (slots[GAL_SEC_COND].size != pack->cond_count * sizeof(gal_action_t)) {
        *err = "cond size"; return false;
    }
    if (slots[GAL_SEC_ACTION].size != pack->action_count * sizeof(gal_action_t)) {
        *err = "action size"; return false;
    }
    // 字符串必须在 TEXT 段内且以 NUL 结尾(坏包不要让界面读到越界指针)。
    for (uint32_t i = 0; i < pack->line_count; ++i) {
        const uint32_t off = pack->lines[i].text_off;
        const uint32_t len = pack->lines[i].text_len;
        if (off >= pack->text_size || len >= pack->text_size - off ||
            pack->text[off + len] != 0) {
            *err = "line text";
            return false;
        }
        if (pack->lines[i].speaker != GAL_NONE && pack->lines[i].speaker >= pack->name_count) {
            *err = "line speaker";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->name_count; ++i) {
        uint32_t off;
        uint16_t len;
        memcpy(&off, pack->name_tbl + (size_t)i * 8, 4);
        memcpy(&len, pack->name_tbl + (size_t)i * 8 + 4, 2);
        if (off >= pack->text_size || len >= pack->text_size - off ||
            pack->text[off + len] != 0) {
            *err = "name text";
            return false;
        }
    }
    // Cheap cross-reference checks; the Python verifier does the exhaustive pass.
    if (pack->event_count == 0 || pack->line_count == 0) { *err = "empty"; return false; }
    if (pack->title_image >= pack->image_count) { *err = "title image"; return false; }
    return true;
}

const char *gal_pack_text(const gal_pack_t *pack, uint32_t off)
{
    if (!pack || !pack->text || off >= pack->text_size) return "";
    // 防御性：pack 里的字符串必须在本段内以 NUL 结尾，否则 LVGL 会读过界
    // （steins0 的 copy_text_to_label 就栽在这类指针上）。
    const char *s = (const char *)(pack->text + off);
    const char *end = (const char *)(pack->text + pack->text_size);
    if (memchr(s, 0, (size_t)(end - s)) == NULL) return "";
    return s;
}

const char *gal_pack_name(const gal_pack_t *pack, uint16_t name_id)
{
    if (!pack || name_id == GAL_NONE || name_id >= pack->name_count) return "";
    const uint8_t *rec = pack->name_tbl + (size_t)name_id * 8;
    uint32_t off;
    uint16_t len;
    memcpy(&off, rec, 4);
    memcpy(&len, rec + 4, 2);
    (void)len;
    return gal_pack_text(pack, off);
}

const char *gal_line_text(const gal_pack_t *pack, uint16_t line_id)
{
    if (!pack || line_id >= pack->line_count) return "";
    return gal_pack_text(pack, pack->lines[line_id].text_off);
}

bool gal_image_parts(const gal_pack_t *pack, uint16_t image_id,
                     const uint8_t **top, uint32_t *top_len,
                     const uint8_t **bot, uint32_t *bot_len,
                     const uint8_t **mask_top, uint32_t *mask_top_len,
                     const uint8_t **mask_bot, uint32_t *mask_bot_len,
                     uint16_t *w, uint16_t *h)
{
    if (!pack || image_id >= pack->image_count) return false;
    const gal_pack_image_t *im = &pack->images[image_id];
    if (!in_blob(pack, im->top_off, im->top_len)) return false;
    if (!in_blob(pack, im->bot_off, im->bot_len)) return false;
    if (!in_blob(pack, im->mask_top_off, im->mask_top_len)) return false;
    if (!in_blob(pack, im->mask_bot_off, im->mask_bot_len)) return false;
    if (top) *top = pack->blob + im->top_off;
    if (top_len) *top_len = im->top_len;
    if (bot) *bot = pack->blob + im->bot_off;
    if (bot_len) *bot_len = im->bot_len;
    if (mask_top) *mask_top = pack->blob + im->mask_top_off;
    if (mask_top_len) *mask_top_len = im->mask_top_len;
    if (mask_bot) *mask_bot = pack->blob + im->mask_bot_off;
    if (mask_bot_len) *mask_bot_len = im->mask_bot_len;
    if (w) *w = im->w;
    if (h) *h = im->h;
    return true;
}
