#include "gal_script.h"

#include <string.h>

/* Every accessor re-validates offsets against the mapped size so that a
 * truncated, stale or blank partition degrades into "nothing to read" instead of
 * faulting on an out-of-range pointer. */

/* The packer aligns every offset that the firmware casts to a struct. A pack that
 * violates this is rejected: on RISC-V a misaligned 32-bit load is not something
 * to rely on, and the compiler is entitled to assume the alignment the type
 * requires. */
#define GAL_ALIGN 4u

static bool aligned(uint32_t value)
{
    return (value % GAL_ALIGN) == 0u;
}

static bool range_ok(const gal_pack_t *pack, uint32_t offset, uint32_t length)
{
    if (pack == NULL || pack->data == NULL) {
        return false;
    }
    if (offset > pack->size) {
        return false;
    }
    return length <= pack->size - offset;
}

bool gal_pack_open(gal_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (pack == NULL) {
        return false;
    }
    pack->data = data;
    pack->size = size;
    pack->header = NULL;
    if (data == NULL || size < GAL_PACK_HEADER_SIZE) {
        return false;
    }
    const gal_pack_header_t *header = (const gal_pack_header_t *)(const void *)data;
    if (header->magic != GAL_PACK_MAGIC || header->version != GAL_PACK_VERSION ||
        header->header_size != GAL_PACK_HEADER_SIZE) {
        return false;
    }
    pack->header = header;
    return true;
}

bool gal_pack_usable(const gal_pack_t *pack)
{
    if (pack == NULL || pack->header == NULL) {
        return false;
    }
    const gal_pack_header_t *header = pack->header;
    if (header->image_count == 0 || header->chapter_count == 0) {
        return false;
    }
    if (!aligned(header->image_index_off) || !aligned(header->script_index_off)) {
        return false;
    }
    if (!aligned(header->name_blob_off)) {
        return false;
    }
    if (!range_ok(pack, header->image_index_off,
                  (uint32_t)header->image_count * GAL_IMAGE_ENTRY_SIZE)) {
        return false;
    }
    if (!range_ok(pack, header->name_blob_off, header->name_blob_len)) {
        return false;
    }
    return range_ok(pack, header->script_index_off,
                    (uint32_t)header->chapter_count * GAL_CHAPTER_ENTRY_SIZE);
}

uint16_t gal_pack_max_text_len(const gal_pack_t *pack)
{
    return pack != NULL && pack->header != NULL ? pack->header->max_text_len : 0;
}

/* --- images ---------------------------------------------------------------- */

uint16_t gal_pack_image_count(const gal_pack_t *pack)
{
    return pack != NULL && pack->header != NULL ? pack->header->image_count : 0;
}

const gal_image_entry_t *gal_pack_image(const gal_pack_t *pack, uint16_t index)
{
    if (!gal_pack_usable(pack) || index >= pack->header->image_count) {
        return NULL;
    }
    uint32_t offset = pack->header->image_index_off +
                      (uint32_t)index * GAL_IMAGE_ENTRY_SIZE;
    if (!range_ok(pack, offset, GAL_IMAGE_ENTRY_SIZE)) {
        return NULL;
    }
    return (const gal_image_entry_t *)(const void *)(pack->data + offset);
}

const char *gal_pack_image_name(const gal_pack_t *pack, const gal_image_entry_t *image)
{
    if (image == NULL || !gal_pack_usable(pack)) {
        return NULL;
    }
    uint32_t offset = pack->header->name_blob_off + image->name_off;
    if (!range_ok(pack, offset, 1)) {
        return NULL;
    }
    return (const char *)(const void *)(pack->data + offset);
}

int32_t gal_pack_find_image(const gal_pack_t *pack, const char *name)
{
    if (name == NULL || !gal_pack_usable(pack)) {
        return -1;
    }
    for (uint16_t index = 0; index < pack->header->image_count; index++) {
        const gal_image_entry_t *image = gal_pack_image(pack, index);
        if (image == NULL) {
            return -1;
        }
        const char *candidate = gal_pack_image_name(pack, image);
        if (candidate != NULL && strcmp(candidate, name) == 0) {
            return (int32_t)index;
        }
    }
    return -1;
}

const uint8_t *gal_pack_image_payload(const gal_pack_t *pack, const gal_image_entry_t *image)
{
    if (image == NULL || image->kind == GAL_IMG_KIND_SOLID || !gal_pack_usable(pack)) {
        return NULL;
    }
    if (!range_ok(pack, image->data_off, image->data_len)) {
        return NULL;
    }
    /* An indexed image carries its palette first; LVGL reads the palette from the
     * same pointer, and the packer lays it out exactly that way. */
    uint32_t needed = GAL_PALETTE_BYTES + (uint32_t)image->width * image->height;
    if (image->data_len < needed) {
        return NULL;
    }
    return pack->data + image->data_off;
}

uint16_t gal_pack_image_color(const gal_pack_t *pack, const gal_image_entry_t *image)
{
    if (image == NULL || image->kind != GAL_IMG_KIND_SOLID || !gal_pack_usable(pack)) {
        return 0;
    }
    if (!range_ok(pack, image->data_off, 2)) {
        return 0;
    }
    return (uint16_t)(pack->data[image->data_off] |
                      ((uint16_t)pack->data[image->data_off + 1] << 8));
}

/* --- chapters -------------------------------------------------------------- */

uint16_t gal_pack_chapter_count(const gal_pack_t *pack)
{
    return pack != NULL && pack->header != NULL ? pack->header->chapter_count : 0;
}

static const gal_chapter_entry_t *chapter_entry(const gal_pack_t *pack, uint16_t index)
{
    if (!gal_pack_usable(pack) || index >= pack->header->chapter_count) {
        return NULL;
    }
    uint32_t offset = pack->header->script_index_off +
                      (uint32_t)index * GAL_CHAPTER_ENTRY_SIZE;
    if (!range_ok(pack, offset, GAL_CHAPTER_ENTRY_SIZE)) {
        return NULL;
    }
    return (const gal_chapter_entry_t *)(const void *)(pack->data + offset);
}

bool gal_pack_chapter_open(const gal_pack_t *pack, uint16_t index, gal_chapter_t *chapter)
{
    if (chapter == NULL) {
        return false;
    }
    chapter->blob = NULL;
    chapter->header = NULL;

    const gal_chapter_entry_t *entry = chapter_entry(pack, index);
    if (entry == NULL || entry->data_len < GAL_CHAPTER_HEADER_SIZE) {
        return false;
    }
    if (!aligned(entry->data_off)) {
        return false;
    }
    if (!range_ok(pack, entry->data_off, entry->data_len)) {
        return false;
    }
    const gal_chapter_header_t *header =
        (const gal_chapter_header_t *)(const void *)(pack->data + entry->data_off);
    if (header->magic != GAL_CHAPTER_MAGIC) {
        return false;
    }
    uint32_t scenes_bytes = (uint32_t)header->scene_count * GAL_SCENE_REC_SIZE;
    uint32_t dialogues_bytes = (uint32_t)header->dialogue_count * GAL_DLG_REC_SIZE;
    if (!range_ok(pack, entry->data_off + header->scenes_off, scenes_bytes) ||
        !range_ok(pack, entry->data_off + header->dialogues_off, dialogues_bytes) ||
        !range_ok(pack, entry->data_off + header->text_pool_off, header->text_pool_len) ||
        !range_ok(pack, entry->data_off + header->names_off, header->names_len)) {
        return false;
    }
    chapter->blob = pack->data + entry->data_off;
    chapter->header = header;
    return true;
}

uint16_t gal_chapter_index(const gal_chapter_t *chapter)
{
    return chapter != NULL && chapter->header != NULL ? chapter->header->chapter_index : 0;
}

uint16_t gal_chapter_scene_count(const gal_chapter_t *chapter)
{
    return chapter != NULL && chapter->header != NULL ? chapter->header->scene_count : 0;
}

bool gal_chapter_is_finale(const gal_chapter_t *chapter)
{
    return chapter != NULL && chapter->header != NULL &&
           (chapter->header->flags & GAL_CHAPTER_FLAG_FINALE) != 0;
}

const gal_scene_rec_t *gal_chapter_scene(const gal_chapter_t *chapter, uint16_t index)
{
    if (chapter == NULL || chapter->header == NULL || index >= chapter->header->scene_count) {
        return NULL;
    }
    return (const gal_scene_rec_t *)(const void *)(chapter->blob +
                                                   chapter->header->scenes_off +
                                                   (uint32_t)index * GAL_SCENE_REC_SIZE);
}

const gal_dlg_rec_t *gal_chapter_dialogue(const gal_chapter_t *chapter, uint16_t index)
{
    if (chapter == NULL || chapter->header == NULL || index >= chapter->header->dialogue_count) {
        return NULL;
    }
    return (const gal_dlg_rec_t *)(const void *)(chapter->blob +
                                                 chapter->header->dialogues_off +
                                                 (uint32_t)index * GAL_DLG_REC_SIZE);
}

static const char *pool_string(const gal_chapter_t *chapter, uint32_t pool_off,
                               uint32_t pool_len, uint32_t offset)
{
    if (offset >= pool_len) {
        return NULL;
    }
    const char *start = (const char *)(const void *)(chapter->blob + pool_off + offset);
    /* The packer NUL-terminates every pooled string, so a length check is enough. */
    if (start[0] == '\0') {
        return start;
    }
    return start;
}

const char *gal_chapter_dialogue_text(const gal_chapter_t *chapter, const gal_dlg_rec_t *line)
{
    if (line == NULL || chapter == NULL || chapter->header == NULL) {
        return NULL;
    }
    return pool_string(chapter, chapter->header->text_pool_off,
                       chapter->header->text_pool_len, line->text_off);
}

const char *gal_chapter_dialogue_name(const gal_chapter_t *chapter, const gal_dlg_rec_t *line)
{
    if (line == NULL || chapter == NULL || chapter->header == NULL) {
        return NULL;
    }
    return pool_string(chapter, chapter->header->names_off, chapter->header->names_len,
                       line->name_off);
}

/* --- pagination ------------------------------------------------------------- */

/* Decode one UTF-8 sequence from `text`. Returns its length in bytes; a malformed
 * lead byte is treated as a one-byte character so scanning always advances. */
static size_t utf8_decode(const char *text, uint32_t *codepoint)
{
    const unsigned char *bytes = (const unsigned char *)text;
    unsigned char lead = bytes[0];
    if (lead < 0x80) {
        *codepoint = lead;
        return 1;
    }
    size_t length;
    uint32_t value;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07u;
    } else {
        *codepoint = lead;
        return 1;
    }
    for (size_t i = 1; i < length; i++) {
        if ((bytes[i] & 0xC0) != 0x80) {
            *codepoint = lead;
            return 1;
        }
        value = (value << 6) | (bytes[i] & 0x3Fu);
    }
    *codepoint = value;
    return length;
}

/* A full-width character occupies two units; anything else one. */
static int codepoint_units(uint32_t cp)
{
    if (cp < 0x80) {
        return 1;
    }
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

/* Marks that must not be left stranded at the start of a line. */
static bool no_line_start(uint32_t cp)
{
    switch (cp) {
        case 0x3001: /* closing comma */
        case 0x3002: /* full stop */
        case 0x300D:
        case 0x300F:
        case 0x3011:
        case 0x3015:
        case 0xFF01:
        case 0xFF09:
        case 0xFF0C:
        case 0xFF0E:
        case 0xFF1A:
        case 0xFF1B:
        case 0xFF1F:
        case 0xFF3D:
        case 0x2019:
        case 0x201D:
        case 0x2026:
        case 0x2014:
        case 0x30FC: /* long vowel mark */
            return true;
        default:
            return false;
    }
}

typedef struct {
    int page;  /* 0-based index of the current page */
    int line;  /* 0-based index of the line being filled */
    int units; /* units already placed on that line */
} page_cursor_t;

/* Finish the current line, closing the page once the last line is full. */
static void pagination_end_line(page_cursor_t *cursor, size_t byte, int lines_per_page,
                                uint32_t *offsets, int max_offsets)
{
    cursor->units = 0;
    cursor->line++;
    if (cursor->line < lines_per_page) {
        return;
    }
    cursor->page++;
    cursor->line = 0;
    if (offsets != NULL && cursor->page < max_offsets) {
        offsets[cursor->page] = (uint32_t)byte;
    }
}

int gal_text_pages(const char *utf8, const gal_pagination_t *layout,
                   uint32_t *offsets, int max_offsets)
{
    /* A missing layout means "everything fits on one page". */
    const int per_line = (layout != NULL && layout->units_per_line > 0)
                             ? layout->units_per_line : (1 << 28);
    const int per_page = (layout != NULL && layout->lines_per_page > 0)
                             ? layout->lines_per_page : (1 << 28);

    const char *text = utf8 != NULL ? utf8 : "";
    page_cursor_t cursor = { .page = 0, .line = 0, .units = 0 };
    if (offsets != NULL && max_offsets > 0) {
        offsets[0] = 0;
    }

    size_t byte = 0;
    while (text[byte] != '\0') {
        if (text[byte] == '\n') {
            byte++;
            pagination_end_line(&cursor, byte, per_page, offsets, max_offsets);
            continue;
        }
        uint32_t cp = 0;
        size_t length = utf8_decode(text + byte, &cp);
        int units = codepoint_units(cp);

        /* Wrap, unless that would strand a closing mark at the line start; such a
         * mark stays on the current line, and runs of them stay together. */
        if (cursor.units > 0 && cursor.units + units > per_line && !no_line_start(cp)) {
            pagination_end_line(&cursor, byte, per_page, offsets, max_offsets);
        }
        cursor.units += units;
        byte += length;
    }

    const int pages = cursor.page + 1;
    if (offsets != NULL && pages < max_offsets) {
        offsets[pages] = (uint32_t)byte;
    }
    return pages;
}

/* --- reader ---------------------------------------------------------------- */

static const gal_dlg_rec_t *current_line(const gal_chapter_t *chapter,
                                         const gal_reader_pos_t *pos)
{
    const gal_scene_rec_t *scene = gal_chapter_scene(chapter, pos->scene);
    if (scene == NULL || pos->dialogue >= scene->dialogue_count) {
        return NULL;
    }
    return gal_chapter_dialogue(chapter,
                               (uint16_t)(scene->first_dialogue + pos->dialogue));
}

/* Recompute how many pages the current line needs, and keep `page` inside it. */
static void refresh_page_count(const gal_chapter_t *chapter, gal_reader_pos_t *pos,
                               const gal_pagination_t *layout)
{
    const gal_dlg_rec_t *line = current_line(chapter, pos);
    int pages = 1;
    if (line != NULL) {
        pages = gal_text_pages(gal_chapter_dialogue_text(chapter, line), layout, NULL, 0);
        if (pages < 1) {
            pages = 1;
        }
    }
    pos->page_count = (uint16_t)pages;
    if (pos->page >= pos->page_count) {
        pos->page = (uint16_t)(pos->page_count - 1);
    }
}

bool gal_reader_clamp(const gal_pack_t *pack, gal_reader_pos_t *pos, gal_chapter_t *chapter,
                      const gal_pagination_t *layout)
{
    if (pos == NULL || !gal_pack_usable(pack)) {
        return false;
    }
    if (pos->chapter >= gal_pack_chapter_count(pack)) {
        return false;
    }
    /* Declared at function scope so the alias below never outlives its storage. */
    gal_chapter_t local;
    if (chapter == NULL) {
        chapter = &local;
    }
    if (!gal_pack_chapter_open(pack, pos->chapter, chapter)) {
        return false;
    }
    if (gal_chapter_scene_count(chapter) == 0) {
        return false;
    }
    if (pos->scene >= chapter->header->scene_count) {
        pos->scene = (uint16_t)(chapter->header->scene_count - 1);
        pos->dialogue = 0;
        pos->page = 0;
    }
    const gal_scene_rec_t *scene = gal_chapter_scene(chapter, pos->scene);
    if (scene == NULL || scene->dialogue_count == 0) {
        pos->dialogue = 0;
        pos->page = 0;
        refresh_page_count(chapter, pos, layout);
        return true;
    }
    if (pos->dialogue >= scene->dialogue_count) {
        pos->dialogue = (uint16_t)(scene->dialogue_count - 1);
        pos->page = 0;
    }
    refresh_page_count(chapter, pos, layout);
    return true;
}

bool gal_reader_begin(const gal_pack_t *pack, gal_reader_pos_t *pos, uint16_t chapter,
                      const gal_pagination_t *layout)
{
    if (pos == NULL) {
        return false;
    }
    gal_reader_pos_t candidate = { .chapter = chapter, .scene = 0, .dialogue = 0, .page = 0,
                                   .page_count = 1 };
    if (!gal_reader_clamp(pack, &candidate, NULL, layout)) {
        return false;
    }
    *pos = candidate;
    return true;
}

gal_step_t gal_reader_advance(const gal_pack_t *pack, gal_reader_pos_t *pos,
                              gal_chapter_t *chapter, const gal_pagination_t *layout)
{
    if (pos == NULL || chapter == NULL || chapter->header == NULL) {
        return GAL_STEP_INVALID;
    }
    const gal_scene_rec_t *scene = gal_chapter_scene(chapter, pos->scene);
    if (scene == NULL) {
        return GAL_STEP_INVALID;
    }

    /* A line too long for the panel continues on its next page first. */
    refresh_page_count(chapter, pos, layout);
    if (pos->page + 1 < pos->page_count) {
        pos->page++;
        return GAL_STEP_PAGE;
    }

    /* Within the scene: step one line. */
    if (pos->dialogue + 1 < scene->dialogue_count) {
        pos->dialogue++;
        pos->page = 0;
        refresh_page_count(chapter, pos, layout);
        return GAL_STEP_LINE;
    }

    /* Next scene in the same chapter. */
    if (pos->scene + 1 < chapter->header->scene_count) {
        pos->scene++;
        pos->dialogue = 0;
        pos->page = 0;
        refresh_page_count(chapter, pos, layout);
        return GAL_STEP_SCENE;
    }

    /* End of the chapter. */
    if (gal_chapter_is_finale(chapter)) {
        return GAL_STEP_FINALE;
    }
    uint16_t next = (uint16_t)(pos->chapter + 1);
    if (next >= gal_pack_chapter_count(pack)) {
        return GAL_STEP_FINALE;
    }
    gal_reader_pos_t candidate = { .chapter = next, .scene = 0, .dialogue = 0, .page = 0,
                                   .page_count = 1 };
    gal_chapter_t opened;
    if (!gal_reader_clamp(pack, &candidate, &opened, layout)) {
        return GAL_STEP_INVALID;
    }
    *pos = candidate;
    *chapter = opened;
    return GAL_STEP_CHAPTER;
}
