/*
 * Read-only view over the packed galgame script.
 *
 * Deliberately free of ESP-IDF and LVGL so the parser, the bounds checks and the
 * advance rules can be exercised by a host test against a synthetic pack. Records
 * live in memory-mapped flash and are returned as pointers into it; nothing here
 * allocates.
 */
#ifndef GAL_SCRIPT_H
#define GAL_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#include "gal_format.h"

/* A validated view over a memory-mapped pack. */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    const gal_pack_header_t *header;
} gal_pack_t;

/* A chapter, read in place. */
typedef struct {
    const uint8_t *blob;
    const gal_chapter_header_t *header;
} gal_chapter_t;

/* Where the reader currently is. `page` splits a dialogue line that does not fit
 * the panel; see gal_pagination_t. */
typedef struct {
    uint16_t chapter;
    uint16_t scene;
    uint16_t dialogue;
    uint16_t page;
    uint16_t page_count;
} gal_reader_pos_t;

/*
 * Typesetting parameters. A full-width character counts two units and anything
 * else one, so the model can break lines without a font engine. The application
 * layer derives these from the font size and the panel width.
 */
typedef struct {
    int units_per_line;
    int lines_per_page;
} gal_pagination_t;

typedef enum {
    GAL_STEP_INVALID = 0,
    GAL_STEP_PAGE,     /* the same line continues on its next page */
    GAL_STEP_LINE,     /* advanced within the current scene */
    GAL_STEP_SCENE,    /* advanced to the next scene */
    GAL_STEP_CHAPTER,  /* advanced into the next chapter */
    GAL_STEP_FINALE,   /* the finale chapter finished */
} gal_step_t;

/* --- pack ------------------------------------------------------------------ */

bool gal_pack_open(gal_pack_t *pack, const uint8_t *data, uint32_t size);
bool gal_pack_usable(const gal_pack_t *pack);
uint16_t gal_pack_max_text_len(const gal_pack_t *pack);

/* --- images ---------------------------------------------------------------- */

uint16_t gal_pack_image_count(const gal_pack_t *pack);
const gal_image_entry_t *gal_pack_image(const gal_pack_t *pack, uint16_t index);
const char *gal_pack_image_name(const gal_pack_t *pack, const gal_image_entry_t *image);
/* Returns -1 when the name is absent. */
int32_t gal_pack_find_image(const gal_pack_t *pack, const char *name);
/* For GAL_IMG_KIND_SOLID this returns NULL; use gal_pack_image_color instead. */
const uint8_t *gal_pack_image_payload(const gal_pack_t *pack, const gal_image_entry_t *image);
/* The RGB565 fill for a solid image, or 0 for any other kind. */
uint16_t gal_pack_image_color(const gal_pack_t *pack, const gal_image_entry_t *image);

/* --- chapters -------------------------------------------------------------- */

uint16_t gal_pack_chapter_count(const gal_pack_t *pack);
bool gal_pack_chapter_open(const gal_pack_t *pack, uint16_t index, gal_chapter_t *chapter);
uint16_t gal_chapter_index(const gal_chapter_t *chapter);
uint16_t gal_chapter_scene_count(const gal_chapter_t *chapter);
bool gal_chapter_is_finale(const gal_chapter_t *chapter);
const gal_scene_rec_t *gal_chapter_scene(const gal_chapter_t *chapter, uint16_t index);
const gal_dlg_rec_t *gal_chapter_dialogue(const gal_chapter_t *chapter, uint16_t index);
/* NUL-terminated UTF-8, owned by the pack. */
const char *gal_chapter_dialogue_text(const gal_chapter_t *chapter, const gal_dlg_rec_t *line);
const char *gal_chapter_dialogue_name(const gal_chapter_t *chapter, const gal_dlg_rec_t *line);

/* --- reader ---------------------------------------------------------------- */

/* Move to the first line of `chapter`, clamped into range. */
bool gal_reader_begin(const gal_pack_t *pack, gal_reader_pos_t *pos, uint16_t chapter,
                      const gal_pagination_t *layout);
/* Clamp a position onto a dialogue that exists, and onto a page of that line. */
bool gal_reader_clamp(const gal_pack_t *pack, gal_reader_pos_t *pos, gal_chapter_t *chapter,
                      const gal_pagination_t *layout);
/*
 * Advance one confirm press. Returns what changed so the caller can decide
 * whether to reload the chapter, redraw the scene or just re-render the text.
 * When `layout` is NULL a line is treated as a single page.
 */
gal_step_t gal_reader_advance(const gal_pack_t *pack, gal_reader_pos_t *pos,
                              gal_chapter_t *chapter, const gal_pagination_t *layout);

/* --- pagination ------------------------------------------------------------- */

#define GAL_PAGE_MAX_OFFSETS 16

/*
 * Split a UTF-8 string into pages of `lines_per_page` lines of at most
 * `units_per_line` units, and return the page count. When `offsets` is non-NULL
 * it receives the byte offset of each page start, with `offsets[page_count]` set
 * to the string length; nothing is written past `max_offsets` entries.
 *
 * A line never begins with a trailing punctuation mark: such a mark is pulled
 * back onto the previous line (the same rule the Saya reference applies), so
 * closing quotes and full stops do not end up orphaned at the left edge.
 */
int gal_text_pages(const char *utf8, const gal_pagination_t *layout,
                   uint32_t *offsets, int max_offsets);

#endif /* GAL_SCRIPT_H */
