/*
 * Host tests for the galgame script reader.
 *
 * Builds synthetic packs in memory using the generated layout, then drives the
 * real parser: a blank partition, a truncated pack, a bad magic, out-of-range
 * offsets and multi-chapter advance are all covered without a device.
 *
 * main/gal/gal_script.c deliberately has no ESP-IDF or LVGL dependency, so it
 * links into this test unchanged.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gal/gal_format.h"
#include "gal/gal_save.h"
#include "gal/gal_script.h"

/* --- synthetic pack builder -------------------------------------------------- */

#define BUF_BYTES 65536

/* File scope, not stack: the buffer is 64 KiB and several tests need two. */
typedef struct {
    uint8_t bytes[BUF_BYTES];
    uint32_t len;
    uint16_t image_count;
    uint16_t chapter_count;
    uint32_t image_index_off;
    uint32_t name_blob_off;
    uint32_t name_blob_len;
    uint32_t script_index_off;
} builder_t;

static builder_t g_pack;
static builder_t g_corrupt;

static uint32_t align4(uint32_t value) { return (value + 3u) & ~3u; }

static void emit(builder_t *b, const void *src, uint32_t len)
{
    assert(b->len + len <= BUF_BYTES);
    memcpy(b->bytes + b->len, src, len);
    b->len += len;
}

static void emit_u8(builder_t *b, uint8_t v) { emit(b, &v, 1); }
static void emit_u16(builder_t *b, uint16_t v) { emit(b, &v, 2); }
static void emit_u32(builder_t *b, uint32_t v) { emit(b, &v, 4); }
static void pad_to(builder_t *b, uint32_t target)
{
    while (b->len < target) {
        emit_u8(b, 0);
    }
}

/* One chapter: three scenes, the last of which closes the chapter. */
typedef struct {
    const char *names[6];
    const char *texts[6];
    uint16_t body[6];
    uint16_t face[6];
} chapter_spec_t;

static const char *const IMAGE_NAMES[] = {"bg/a.png", "fg/body.png", "fg/face.png"};

static void build_chapter(builder_t *b, uint16_t index, const chapter_spec_t *spec,
                          bool finale, uint32_t *out_off, uint32_t *out_len)
{
    *out_off = b->len;

    /* scene/line layout: scene0 has 1 line, scene1 has 2, scene2 has 3 */
    static const uint16_t scene_lines[3] = {1, 2, 3};

    uint32_t scenes_off = GAL_CHAPTER_HEADER_SIZE;
    uint32_t dialogues_off = scenes_off + 3 * GAL_SCENE_REC_SIZE;
    uint32_t text_pool_off = dialogues_off + 6 * GAL_DLG_REC_SIZE;

    /* text pool */
    uint8_t pool[512];
    uint32_t pool_len = 0;
    uint32_t text_off[6];
    uint32_t name_off[6];
    for (int i = 0; i < 6; i++) {
        text_off[i] = pool_len;
        size_t n = strlen(spec->texts[i]) + 1;
        memcpy(pool + pool_len, spec->texts[i], n);
        pool_len += (uint32_t)n;
    }
    uint32_t names_off = pool_len;
    for (int i = 0; i < 6; i++) {
        /* Offsets into a pool are relative to that pool's own start. */
        name_off[i] = pool_len - names_off;
        size_t n = strlen(spec->names[i]) + 1;
        memcpy(pool + pool_len, spec->names[i], n);
        pool_len += (uint32_t)n;
    }

    emit_u32(b, GAL_CHAPTER_MAGIC);
    emit_u16(b, 3);
    emit_u16(b, 6);
    emit_u32(b, scenes_off);
    emit_u32(b, dialogues_off);
    /* `text_pool_off`/`names_off` are record offsets inside the chapter, while
     * `text_off[]`/`name_off[]` are offsets inside their own pool. */
    emit_u32(b, text_pool_off);
    emit_u32(b, names_off);
    emit_u32(b, text_pool_off + names_off);
    emit_u32(b, pool_len - names_off);
    emit_u32(b, index);
    emit_u16(b, finale ? GAL_CHAPTER_FLAG_FINALE : 0);
    emit_u16(b, 0);

    uint16_t cursor = 0;
    for (uint16_t s = 0; s < 3; s++) {
        emit_u16(b, s == 0 ? 0 : GAL_IMG_NONE);   /* background */
        emit_u16(b, GAL_IMG_NONE);                /* overlay */
        emit_u16(b, 0);
        emit_u16(b, 0);
        emit_u16(b, cursor);
        emit_u16(b, scene_lines[s]);
        emit_u32(b, 0);
        cursor = (uint16_t)(cursor + scene_lines[s]);
    }
    for (int i = 0; i < 6; i++) {
        emit_u32(b, text_off[i]);
        emit_u16(b, (uint16_t)strlen(spec->texts[i]));
        emit_u16(b, (uint16_t)name_off[i]);
        emit_u16(b, (uint16_t)strlen(spec->names[i]));
        emit_u16(b, spec->body[i]);
        emit_u16(b, spec->face[i]);
        emit_u16(b, 0);
    }
    emit(b, pool, pool_len);
    *out_len = b->len - *out_off;
}

static uint32_t build_pack(builder_t *b, uint16_t chapter_count, bool last_is_finale)
{
    memset(b, 0, sizeof(*b));
    b->image_count = 3;
    b->chapter_count = chapter_count;

    uint8_t placeholder_image[GAL_PALETTE_BYTES + 4];
    memset(placeholder_image, 0, sizeof(placeholder_image));

    b->len = GAL_PACK_HEADER_SIZE;
    b->image_index_off = b->len;
    b->len += b->image_count * GAL_IMAGE_ENTRY_SIZE;
    b->name_blob_off = b->len;
    uint16_t image_name_off[4] = {0};
    for (uint16_t i = 0; i < b->image_count; i++) {
        image_name_off[i] = (uint16_t)(b->len - b->name_blob_off);
        emit(b, IMAGE_NAMES[i], (uint32_t)strlen(IMAGE_NAMES[i]) + 1);
    }
    b->name_blob_len = b->len - b->name_blob_off;
    /* The chapter index and every chapter body are cast to structs by the reader,
     * so the format requires them to start on a 4-byte boundary. */
    pad_to(b, align4(b->len));
    b->script_index_off = b->len;
    b->len += chapter_count * GAL_CHAPTER_ENTRY_SIZE;
    uint32_t data_off = align4(b->len);

    /* image payloads */
    uint32_t image_data[3];
    for (int i = 0; i < 3; i++) {
        image_data[i] = b->len;
        emit(b, placeholder_image, sizeof(placeholder_image));
    }

    /* chapters */
    uint32_t chapter_data[8];
    uint32_t chapter_len[8];
    chapter_spec_t spec = {
        .names = {"", "飞鸟", "", "隼斗", "", ""},
        .texts = {"s0-0", "s1-0", "s1-1", "s2-0", "s2-1", "s2-2"},
        .body = {GAL_IMG_NONE, 1, GAL_IMG_NONE, 1, GAL_IMG_NONE, 0},
        .face = {GAL_IMG_NONE, 2, GAL_IMG_NONE, 2, GAL_IMG_NONE, 0},
    };
    for (uint16_t i = 0; i < chapter_count; i++) {
        build_chapter(b, i, &spec, last_is_finale && i + 1 == chapter_count,
                      &chapter_data[i], &chapter_len[i]);
        pad_to(b, align4(b->len));
    }

    pad_to(b, data_off);

    /* Write header */
    gal_pack_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = GAL_PACK_MAGIC;
    header.version = GAL_PACK_VERSION;
    header.header_size = (uint16_t)sizeof(gal_pack_header_t);
    header.image_count = b->image_count;
    header.chapter_count = b->chapter_count;
    header.image_index_off = b->image_index_off;
    header.name_blob_off = b->name_blob_off;
    header.name_blob_len = b->name_blob_len;
    header.script_index_off = b->script_index_off;
    header.max_text_len = 8;
    memcpy(b->bytes, &header, sizeof(header));

    /* Write image entries and chapter entries */
    for (uint16_t i = 0; i < b->image_count; i++) {
        gal_image_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.width = 2;
        entry.height = 2;
        entry.kind = GAL_IMG_KIND_OPAQUE;
        entry.name_off = image_name_off[i];
        entry.data_off = image_data[i];
        entry.data_len = sizeof(placeholder_image);
        memcpy(b->bytes + b->image_index_off + i * GAL_IMAGE_ENTRY_SIZE, &entry,
               sizeof(entry));
    }
    for (uint16_t i = 0; i < b->chapter_count; i++) {
        gal_chapter_entry_t entry = {.data_off = chapter_data[i], .data_len = chapter_len[i]};
        memcpy(b->bytes + b->script_index_off + i * GAL_CHAPTER_ENTRY_SIZE, &entry,
               sizeof(entry));
    }
    return b->len;
}

/* --- tests ------------------------------------------------------------------- */

static void pack_current(gal_pack_t *pack, uint16_t chapters, bool last_is_finale);

/* A layout wide enough that no test line is ever split, so the reading-order
 * expectations stay independent of the typesetting rules. */
static const gal_pagination_t NO_PAGE = { .units_per_line = 1000, .lines_per_page = 10 };

static void test_speed_presets(void)
{
    /* One press faster each time, wrapping from the fastest back to the slowest. */
    assert(gal_speed_next(GAL_SPEED_SLOW) == GAL_SPEED_MEDIUM);
    assert(gal_speed_next(GAL_SPEED_MEDIUM) == GAL_SPEED_FAST);
    assert(gal_speed_next(GAL_SPEED_FAST) == GAL_SPEED_INSTANT);
    assert(gal_speed_next(GAL_SPEED_INSTANT) == GAL_SPEED_SLOW);

    /* Every preset stays inside the range the stored blob accepts. */
    gal_save_data_t data;
    gal_save_defaults(&data);
    assert(gal_save_valid(&data));

    uint16_t speed = GAL_SPEED_SLOW;
    for (int step = 0; step < 4; step++) {
        speed = gal_speed_next(speed);
        data.text_speed = speed;
        assert(gal_save_valid(&data));
    }
    assert(speed == GAL_SPEED_SLOW); /* four steps returns to the start */

    /* Defaults are one of the presets, and an out-of-range speed is rejected. */
    assert(gal_speed_next(GAL_SPEED_DEFAULT) == GAL_SPEED_FAST);
    data.text_speed = (uint16_t)(GAL_SPEED_MAX + 1);
    assert(!gal_save_valid(&data));

    puts("gal_script speed presets: PASS");
}

static void test_pagination(void)
{
    uint32_t offsets[GAL_PAGE_MAX_OFFSETS];
    gal_pagination_t layout = { .units_per_line = 10, .lines_per_page = 2 };

    /* Twenty ASCII characters fill exactly two lines of ten units. */
    assert(gal_text_pages("abcdefghijklmnopqrst", &layout, offsets, GAL_PAGE_MAX_OFFSETS) == 1);
    assert(offsets[1] == 20);

    /* Two more characters spill onto a second page. */
    int pages = gal_text_pages("abcdefghijklmnopqrstuvwxyz", &layout, offsets,
                              GAL_PAGE_MAX_OFFSETS);
    assert(pages == 2);
    assert(offsets[0] == 0 && offsets[1] == 20 && offsets[2] == 26);

    /* A full-width character is two units, so ten units hold five of them. */
    pages = gal_text_pages("\u4e00\u4e01\u4e02\u4e03\u4e04\u4e05\u4e06\u4e07\u4e08\u4e09"
                           "\u4e0a\u4e0b", &layout, offsets, GAL_PAGE_MAX_OFFSETS);
    assert(pages == 2);
    assert(offsets[1] == 30); /* ten characters, three bytes each */
    assert(offsets[2] == 36);

    /* A trailing full stop is pulled back rather than left at the line start. */
    gal_pagination_t tight = { .units_per_line = 6, .lines_per_page = 1 };
    pages = gal_text_pages("\u4e2d\u4e2d\u4e2d\u3002", &tight, offsets, GAL_PAGE_MAX_OFFSETS);
    assert(pages == 1);
    assert(offsets[1] == 12);

    /* Without the rule the same text would need two pages. */
    pages = gal_text_pages("\u4e2d\u4e2d\u4e2d\u4e2d", &tight, offsets, GAL_PAGE_MAX_OFFSETS);
    assert(pages == 2);

    /* An explicit newline ends the line even when there is room left. */
    gal_pagination_t one_line = { .units_per_line = 100, .lines_per_page = 1 };
    pages = gal_text_pages("ab\ncd", &one_line, offsets, GAL_PAGE_MAX_OFFSETS);
    assert(pages == 2);
    assert(offsets[1] == 3 && offsets[2] == 5);

    /* No layout means "one page"; an empty string is still one page. */
    assert(gal_text_pages("\u4efb\u610f\u957f\u5ea6", NULL, offsets, GAL_PAGE_MAX_OFFSETS) == 1);
    assert(gal_text_pages("", &layout, offsets, GAL_PAGE_MAX_OFFSETS) == 1);
    assert(offsets[0] == 0 && offsets[1] == 0);

    /* offsets may be omitted by a caller that only wants the count. */
    assert(gal_text_pages("abcdefghijklmnopqrstuvwxyz", &layout, NULL, 0) == 2);

    puts("gal_script pagination: PASS");
}

static void test_paging_advance(void)
{
    gal_pack_t pack;
    pack_current(&pack, 3, true);

    /* Three units per line, one line per page: a four character line needs two
     * pages. Scene 0 holds a single line and scene 1 holds two, so the sequence
     * exercises both kinds of step. */
    gal_pagination_t tight = { .units_per_line = 3, .lines_per_page = 1 };
    gal_reader_pos_t pos;
    gal_chapter_t chapter;
    assert(gal_reader_begin(&pack, &pos, 0, &tight));
    assert(gal_pack_chapter_open(&pack, pos.chapter, &chapter));
    assert(pos.scene == 0 && pos.dialogue == 0 && pos.page == 0 && pos.page_count == 2);

    assert(gal_reader_advance(&pack, &pos, &chapter, &tight) == GAL_STEP_PAGE);
    assert(pos.dialogue == 0 && pos.page == 1);

    /* Page exhausted, and scene 0 has no further line: move to scene 1. */
    assert(gal_reader_advance(&pack, &pos, &chapter, &tight) == GAL_STEP_SCENE);
    assert(pos.scene == 1 && pos.dialogue == 0 && pos.page == 0 && pos.page_count == 2);

    /* Scene 1 has a second line, so the page step comes before the line step. */
    assert(gal_reader_advance(&pack, &pos, &chapter, &tight) == GAL_STEP_PAGE);
    assert(pos.scene == 1 && pos.dialogue == 0 && pos.page == 1);
    assert(gal_reader_advance(&pack, &pos, &chapter, &tight) == GAL_STEP_LINE);
    assert(pos.scene == 1 && pos.dialogue == 1 && pos.page == 0);

    /* A stored page survives clamping and is folded back into range. */
    gal_reader_pos_t saved = { .chapter = 0, .scene = 1, .dialogue = 0, .page = 1 };
    assert(gal_reader_clamp(&pack, &saved, &chapter, &tight));
    assert(saved.dialogue == 0 && saved.page == 1 && saved.page_count == 2);

    gal_reader_pos_t wide = { .chapter = 0, .scene = 1, .dialogue = 0, .page = 5 };
    assert(gal_reader_clamp(&pack, &wide, &chapter, &tight));
    assert(wide.page == 1); /* clamped to the last page of that line */

    puts("gal_script page advance: PASS");
}

static void test_rejects_bad_packs(void)
{
    gal_pack_t pack;

    /* A never-flashed partition reads as all 0xFF. */
    static uint8_t blank[4096];
    memset(blank, 0xFF, sizeof(blank));
    assert(!gal_pack_open(&pack, blank, sizeof(blank)));
    assert(!gal_pack_usable(&pack));

    /* An empty or short view is refused rather than read past the end. */
    assert(!gal_pack_open(&pack, blank, 4));
    assert(!gal_pack_open(&pack, NULL, 0));

    builder_t *b = &g_pack;
    uint32_t len = build_pack(b, 2, false);
    assert(gal_pack_open(&pack, b->bytes, len));
    assert(gal_pack_usable(&pack));

    /* A wrong magic is refused. */
    memcpy(&g_corrupt, b, sizeof(*b));
    g_corrupt.bytes[0] ^= 0xFF;
    assert(!gal_pack_open(&pack, g_corrupt.bytes, len));

    /* A truncated view passes open() but must not expose records. */
    assert(gal_pack_open(&pack, b->bytes, len));
    gal_pack_t short_view;
    assert(gal_pack_open(&short_view, b->bytes, GAL_PACK_HEADER_SIZE));
    assert(!gal_pack_usable(&short_view));
    assert(gal_pack_image(&short_view, 0) == NULL);
    gal_chapter_t chapter;
    assert(!gal_pack_chapter_open(&short_view, 0, &chapter));

    puts("gal_script pack validation: PASS");
}

static void test_image_lookup(void)
{
    builder_t *b = &g_pack;
    uint32_t len = build_pack(b, 1, true);
    gal_pack_t pack;
    assert(gal_pack_open(&pack, b->bytes, len));

    assert(gal_pack_image_count(&pack) == 3);    assert(gal_pack_find_image(&pack, "fg/body.png") == 1);
    assert(gal_pack_find_image(&pack, "bg/a.png") == 0);
    assert(gal_pack_find_image(&pack, "nope.png") == -1);

    const gal_image_entry_t *image = gal_pack_image(&pack, 1);
    assert(image != NULL);
    assert(strcmp(gal_pack_image_name(&pack, image), "fg/body.png") == 0);
    assert(gal_pack_image_payload(&pack, image) != NULL);
    assert(gal_pack_image_color(&pack, image) == 0);

    /* Out of range reads return NULL instead of wandering off. */
    assert(gal_pack_image(&pack, 3) == NULL);

    puts("gal_script image lookup: PASS");
}

static void pack_current(gal_pack_t *pack, uint16_t chapters, bool last_is_finale)
{
    uint32_t len = build_pack(&g_pack, chapters, last_is_finale);
    const char *dump = getenv("GAL_TEST_DUMP");
    if (dump != NULL) {
        FILE *fp = fopen(dump, "wb");
        assert(fp != NULL);
        assert(fwrite(g_pack.bytes, 1, len, fp) == len);
        fclose(fp);
    }
    assert(gal_pack_open(pack, g_pack.bytes, len));
}

static void test_reading_order(void)
{
    gal_pack_t pack;
    pack_current(&pack, 3, true);
    assert(gal_pack_chapter_count(&pack) == 3);

    gal_reader_pos_t pos;
    assert(gal_reader_begin(&pack, &pos, 0, &NO_PAGE));
    assert(pos.chapter == 0 && pos.scene == 0 && pos.dialogue == 0);

    gal_chapter_t chapter;
    assert(gal_pack_chapter_open(&pack, pos.chapter, &chapter));

    const char *expected[] = {
        "s0-0",  /* scene 0, line 0 */
        "s1-0", "s1-1",
        "s2-0", "s2-1", "s2-2",
    };
    gal_step_t expected_step[] = {
        GAL_STEP_SCENE, GAL_STEP_LINE, GAL_STEP_SCENE, GAL_STEP_LINE, GAL_STEP_LINE,
    };

    for (int i = 0; i < 5; i++) {
        const gal_scene_rec_t *scene = gal_chapter_scene(&chapter, pos.scene);
        assert(scene != NULL);
        assert(pos.dialogue < scene->dialogue_count);
        const gal_dlg_rec_t *line = gal_chapter_dialogue(
            &chapter, (uint16_t)(scene->first_dialogue + pos.dialogue));
        assert(line != NULL);
        const char *text = gal_chapter_dialogue_text(&chapter, line);
        assert(text != NULL);
        assert(strcmp(text, expected[i]) == 0);
        gal_step_t step = gal_reader_advance(&pack, &pos, &chapter, &NO_PAGE);
        assert(step == expected_step[i]);
    }

    puts("gal_script reading order: PASS");
}

static void test_chapter_transition(void)
{
    gal_pack_t pack;
    pack_current(&pack, 3, true);

    gal_reader_pos_t pos;
    gal_chapter_t chapter;
    assert(gal_reader_begin(&pack, &pos, 0, &NO_PAGE));
    assert(gal_pack_chapter_open(&pack, pos.chapter, &chapter));

    /* Walk the last line of the last scene of chapter 0. */
    while (pos.scene != 2 || pos.dialogue != 2) {
        assert(gal_reader_advance(&pack, &pos, &chapter, &NO_PAGE) != GAL_STEP_INVALID);
    }
    assert(gal_reader_advance(&pack, &pos, &chapter, &NO_PAGE) == GAL_STEP_CHAPTER);
    assert(pos.chapter == 1 && pos.scene == 0 && pos.dialogue == 0);
    assert(gal_chapter_index(&chapter) == 1);

    puts("gal_script chapter transition: PASS");
}

static void test_finale(void)
{
    gal_pack_t pack;
    pack_current(&pack, 3, true);

    gal_reader_pos_t pos;
    assert(gal_reader_begin(&pack, &pos, 2, &NO_PAGE));
    gal_chapter_t chapter;
    assert(gal_pack_chapter_open(&pack, 2, &chapter));
    assert(gal_chapter_is_finale(&chapter));

    for (int i = 0; i < 5; i++) {
        assert(gal_reader_advance(&pack, &pos, &chapter, &NO_PAGE) != GAL_STEP_INVALID);
    }
    assert(gal_reader_advance(&pack, &pos, &chapter, &NO_PAGE) == GAL_STEP_FINALE);

    puts("gal_script finale: PASS");
}

static void test_speaker_names(void)
{
    gal_pack_t pack;
    pack_current(&pack, 3, true);

    gal_reader_pos_t pos;
    assert(gal_reader_begin(&pack, &pos, 0, &NO_PAGE));
    gal_chapter_t chapter;
    assert(gal_pack_chapter_open(&pack, 0, &chapter));

    /* Line 0 has no speaker, line 1 is attributed to 飞鸟. */
    const gal_dlg_rec_t *first = gal_chapter_dialogue(&chapter, 0);
    assert(first != NULL);
    const char *narrator = gal_chapter_dialogue_name(&chapter, first);
    assert(narrator != NULL);
    assert(strcmp(narrator, "") == 0);

    const gal_dlg_rec_t *spoken = gal_chapter_dialogue(&chapter, 1);
    assert(spoken != NULL);
    const char *speaker = gal_chapter_dialogue_name(&chapter, spoken);
    assert(speaker != NULL);
    assert(strcmp(speaker, "飞鸟") == 0);

    puts("gal_script speaker names: PASS");
}

static void test_clamping(void)
{
    gal_pack_t pack;
    pack_current(&pack, 2, false);

    /* A saved position from a larger pack is clamped, not trusted. */
    gal_reader_pos_t pos = {.chapter = 0, .scene = 99, .dialogue = 99};
    gal_chapter_t chapter;
    assert(gal_reader_clamp(&pack, &pos, &chapter, &NO_PAGE));
    /* Clamping the scene also resets the line, since the old line number belonged
     * to a different scene. */
    assert(pos.scene == 2);
    assert(pos.dialogue == 0);

    pos.scene = 1;
    pos.dialogue = 99;
    assert(gal_reader_clamp(&pack, &pos, &chapter, &NO_PAGE));
    assert(pos.scene == 1 && pos.dialogue == 1);

    /* A chapter beyond the pack is refused. */
    pos.chapter = 2;
    pos.scene = 0;
    pos.dialogue = 0;
    assert(!gal_reader_clamp(&pack, &pos, &chapter, &NO_PAGE));
    assert(!gal_reader_begin(&pack, &pos, 2, &NO_PAGE));

    puts("gal_script clamping: PASS");
}

static void test_alignment_guard(void)
{
    gal_pack_t pack;
    gal_pack_header_t *header = (gal_pack_header_t *)(void *)g_pack.bytes;

    /* The reader casts offsets to structs, so a pack whose index is not aligned
     * must be refused instead of read. Regression guard: a misaligned chapter
     * index previously reached a misaligned struct load. */
    pack_current(&pack, 2, false);
    assert(gal_pack_usable(&pack));

    uint32_t saved = header->script_index_off;
    header->script_index_off = saved + 2;
    assert(gal_pack_open(&pack, g_pack.bytes, g_pack.len));
    assert(!gal_pack_usable(&pack));

    header->script_index_off = saved;
    assert(gal_pack_usable(&pack));

    puts("gal_script alignment guard: PASS");
}

int main(void)
{
    /* Unbuffered: a failing assertion must not swallow the progress already
     * printed by the earlier tests. */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_rejects_bad_packs();
    test_image_lookup();
    test_pagination();
    test_paging_advance();
    test_speed_presets();
    test_reading_order();
    test_chapter_transition();
    test_finale();
    test_speaker_names();
    test_clamping();
    test_alignment_guard();
    puts("gal_script tests: PASS");
    return 0;
}
