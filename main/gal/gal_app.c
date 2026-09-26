#include "gal_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_pins.h"

#include "gal_assets.h"
#include "gal_layout.h"
#include "gal_save.h"
#include "gal_script.h"
#include "gal_strings.h"

LV_FONT_DECLARE(gal_font_16);
LV_FONT_DECLARE(gal_font_20);

static const char *TAG = "gal_app";

/* The palette lives in gal_layout.h so the reader, the panels and the Saya
 * reference port all draw from one set of colours. */

/* --- panels ------------------------------------------------------------------ */

typedef enum {
    GAL_PANEL_NONE = 0,
    GAL_PANEL_MENU,
    GAL_PANEL_SLOTS,
    GAL_PANEL_CHAPTERS,
    GAL_PANEL_SETTINGS,
} gal_panel_t;

/* Max rows a panel shows at once; the chapter list scrolls a window over 30. */
#define GAL_LIST_VISIBLE 6

typedef enum {
    GAL_VIEW_TITLE = 0,
    GAL_VIEW_READER,
    GAL_VIEW_ENDING,
    GAL_VIEW_ERROR,
} gal_view_t;


/* --- state ------------------------------------------------------------------- */

static gal_assets_t s_assets;
static gal_save_data_t *s_save;
static lv_obj_t *s_screen;
static gal_view_t s_view;

static gal_reader_pos_t s_pos;
static gal_chapter_t s_chapter;

static lv_obj_t *s_bg_image;
static lv_obj_t *s_overlay_image;
static lv_obj_t *s_body_image;
static lv_obj_t *s_face_image;
static lv_obj_t *s_textbox;
static lv_obj_t *s_name_label;
static lv_obj_t *s_text_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_chapter_label;
static bool s_textbox_visible = true;

static lv_timer_t *s_type_timer;
static lv_timer_t *s_auto_timer;
static lv_timer_t *s_status_timer;
static lv_timer_t *s_hint_timer;

/* Typewriter: the packed text lives in flash, so the revealed prefix is copied
 * into this buffer as it appears. The target label is a variable because the
 * settings panel previews the current speed and size with the same machinery. */
static char s_reveal[GAL_REVEAL_CAPACITY];
static size_t s_reveal_len;
static size_t s_text_len;
static const char *s_text;
static bool s_revealing;
static lv_obj_t *s_reveal_target;

/* Fast forward: active only while the up key is physically held. */
static lv_timer_t *s_ff_timer;
static bool s_ff_active;

static lv_obj_t *s_panel;
static lv_obj_t *s_panel_title;
static lv_obj_t *s_panel_rows[GAL_LIST_VISIBLE];
static lv_obj_t *s_preview_label;
static gal_panel_t s_panel_kind;
static uint16_t s_panel_count;
static uint16_t s_panel_cursor;
static uint16_t s_panel_top;
static bool s_slot_is_save;
static lv_obj_t *s_title_items[4];
static uint8_t s_title_cursor;

/*
 * Everything the reader draws depends on the chosen text size, because the panel
 * is sized from the line height: four lines of a page must always fit whole, and
 * the panel grows with the font instead of clipping the last line.
 */
typedef struct {
    const lv_font_t *font;
    int line_h;    /* one line of body text */
    int panel_h;
    int panel_top;
    int name_y;
    int text_h;    /* four lines */
    gal_pagination_t pagination;
} gal_reader_layout_t;

static gal_reader_layout_t reader_layout(void)
{
    const bool large = s_save->text_size == GAL_TEXT_SIZE_LARGE;
    gal_reader_layout_t layout = {
        .font = large ? &gal_font_20 : &gal_font_16,
        .line_h = large ? GAL_LINE_H_LARGE : GAL_LINE_H_SMALL,
        .panel_h = large ? GAL_PANEL_H_LARGE : GAL_PANEL_H_SMALL,
        .panel_top = large ? GAL_PANEL_TOP_LARGE : GAL_PANEL_TOP_SMALL,
        .name_y = large ? GAL_NAME_Y_LARGE : GAL_NAME_Y_SMALL,
        .text_h = large ? GAL_TEXT_H_LARGE : GAL_TEXT_H_SMALL,
        .pagination = {
            .units_per_line = large ? GAL_UNITS_PER_LINE_LARGE : GAL_UNITS_PER_LINE_SMALL,
            .lines_per_page = GAL_LINES_PER_PAGE,
        },
    };
    return layout;
}

/* Text size the reader widgets were built for, so a change can be detected. */
static uint16_t s_reader_text_size;

static uint16_t s_committed_chapter;
static bool s_have_committed_chapter;

static const lv_font_t *body_font(void)
{
    return s_save->text_size == GAL_TEXT_SIZE_LARGE ? &gal_font_20 : &gal_font_16;
}

/* --- small helpers ------------------------------------------------------------ */

static void style_hidden(lv_obj_t *object, bool hidden)
{
    if (object == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static void make_opaque(lv_obj_t *object, lv_color_t color)
{
    lv_obj_set_style_bg_color(object, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(object, 0, LV_PART_MAIN);
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, BSP_LCD_W, BSP_LCD_H);
    make_opaque(screen, GAL_COLOR_PANEL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

/* Battery in the top-right corner, per the repository's UI convention. */
static void make_status_bar(lv_obj_t *parent)
{
    s_battery_label = make_label(parent, &lv_font_montserrat_14, GAL_COLOR_DIM);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -6, GAL_STATUS_Y);

    s_chapter_label = make_label(parent, &gal_font_16, GAL_COLOR_DIM);
    lv_label_set_text(s_chapter_label, "");
    lv_obj_align(s_chapter_label, LV_ALIGN_TOP_LEFT, 6, GAL_STATUS_Y);
}

static void status_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_battery_label == NULL) {
        return;
    }
    int soc = bsp_battery_soc();
    if (soc < 0) {
        /* No gauge reading: show no number rather than a wrong one. */
        lv_label_set_text(s_battery_label, "--%");
    } else {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%d%%", soc);
        lv_label_set_text(s_battery_label, buffer);
    }
}

static void show_hint(const char *text)
{
    if (s_hint_label == NULL) {
        return;
    }
    lv_label_set_text(s_hint_label, text);
    lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    if (s_hint_timer != NULL) {
        lv_timer_reset(s_hint_timer);
        lv_timer_resume(s_hint_timer);
    }
}

static void hint_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_hint_label != NULL) {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_hint_timer != NULL) {
        lv_timer_pause(s_hint_timer);
    }
}

/* --- typewriter --------------------------------------------------------------- */

static size_t utf8_length(const char *text, size_t remaining)
{
    if (remaining == 0) {
        return 0;
    }
    unsigned char lead = (unsigned char)text[0];
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
    }
    return length > remaining ? remaining : length;
}

static void type_stop(void)
{
    if (s_type_timer != NULL) {
        lv_timer_pause(s_type_timer);
    }
    s_revealing = false;
}

/*
 * Fast forward is active only while the up key is physically held. The BSP does
 * not deliver a key-release event, but bsp_button_read_mv() is public and
 * documented as safe to sample alongside the component's own polling, so the
 * application can tell that the finger is still there from the voltage.
 */
static void ff_stop(void)
{
    if (s_ff_timer != NULL) {
        lv_timer_pause(s_ff_timer);
    }
    s_ff_active = false;
}

static void reveal_all(void)
{
    if (s_text == NULL) {
        return;
    }
    size_t copy = s_text_len < sizeof(s_reveal) - 1 ? s_text_len : sizeof(s_reveal) - 1;
    memcpy(s_reveal, s_text, copy);
    s_reveal[copy] = '\0';
    s_reveal_len = copy;
    if (s_reveal_target != NULL) {
        lv_label_set_text(s_reveal_target, s_reveal);
    }
    type_stop();
}

static void begin_reveal(const char *text, lv_obj_t *target)
{
    type_stop();
    s_text = text != NULL ? text : "";
    s_text_len = strlen(s_text);
    s_reveal_len = 0;
    s_reveal[0] = '\0';
    s_reveal_target = target;
    if (target != NULL) {
        lv_label_set_text(target, "");
    }
    if (s_save->text_speed <= GAL_SPEED_INSTANT) {
        /* 瞬间: reveal the whole page with no animation at all. */
        reveal_all();
        return;
    }
    s_revealing = true;
    if (s_type_timer != NULL) {
        lv_timer_set_period(s_type_timer, s_save->text_speed);
        lv_timer_reset(s_type_timer);
        lv_timer_resume(s_type_timer);
    }
}

static void auto_tick(lv_timer_t *timer);

static void type_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_text == NULL || s_reveal_len >= s_text_len) {
        type_stop();
        if (s_save->auto_play && s_auto_timer != NULL) {
            lv_timer_reset(s_auto_timer);
            lv_timer_resume(s_auto_timer);
        }
        return;
    }
    size_t step = utf8_length(s_text + s_reveal_len, s_text_len - s_reveal_len);
    if (s_reveal_len + step >= sizeof(s_reveal)) {
        reveal_all();
        return;
    }
    memcpy(s_reveal + s_reveal_len, s_text + s_reveal_len, step);
    s_reveal_len += step;
    s_reveal[s_reveal_len] = '\0';
    if (s_reveal_target != NULL) {
        lv_label_set_text(s_reveal_target, s_reveal);
    }
}

static void start_line(const char *text, const char *speaker)
{
    if (s_auto_timer != NULL) {
        lv_timer_pause(s_auto_timer);
    }
    if (s_name_label != NULL) {
        lv_label_set_text(s_name_label, speaker != NULL ? speaker : "");
        /* Collapse the plate when nobody is speaking, so narration does not
         * leave an empty pill floating over the artwork. */
        style_hidden(s_name_label, speaker == NULL || speaker[0] == '\0');
    }
    if (s_text_label != NULL) {
        lv_obj_scroll_to_y(lv_obj_get_parent(s_text_label), 0, LV_ANIM_OFF);
    }
    begin_reveal(text, s_text_label);
}

static void auto_tick(lv_timer_t *timer)
{
    (void)timer;
    lv_timer_pause(s_auto_timer);
    /* Only the reading view advances on its own. Without this guard a timer left
     * running while the reader walked back to the title would activate whatever
     * the title menu happened to be highlighting. */
    if (s_view != GAL_VIEW_READER || s_panel != NULL) {
        return;
    }
    /* Re-enter through the normal advance path so auto-play obeys the same rules
     * as a key press. */
    gal_app_key(BSP_BTN_OK, BSP_BTN_CLICK);
}

/* --- reader ------------------------------------------------------------------ */

static void reader_apply_background(uint16_t index)
{
    int32_t solid = gal_assets_solid_color(&s_assets, index);
    if (solid >= 0) {
        style_hidden(s_bg_image, true);
        uint16_t rgb565 = (uint16_t)solid;
        lv_obj_set_style_bg_color(s_screen,
                                  lv_color_make((rgb565 >> 11) << 3,
                                                ((rgb565 >> 5) & 0x3F) << 2,
                                                (rgb565 & 0x1F) << 3),
                                  LV_PART_MAIN);
        return;
    }
    const lv_image_dsc_t *image = gal_assets_image(&s_assets, index);
    if (image == NULL) {
        style_hidden(s_bg_image, true);
        lv_obj_set_style_bg_color(s_screen, GAL_COLOR_PANEL, LV_PART_MAIN);
        return;
    }
    lv_image_set_src(s_bg_image, image);
    style_hidden(s_bg_image, false);
}

static void reader_place_sprite(lv_obj_t *object, uint16_t index)
{
    const lv_image_dsc_t *image = gal_assets_image(&s_assets, index);
    if (image == NULL) {
        style_hidden(object, true);
        return;
    }
    const gal_image_entry_t *entry = gal_pack_image(&s_assets.script, index);
    lv_image_set_src(object, image);
    lv_obj_set_pos(object, entry->offset_x, entry->offset_y);
    style_hidden(object, false);
}

static void reader_show_scene(void)
{
    const gal_scene_rec_t *scene = gal_chapter_scene(&s_chapter, s_pos.scene);
    if (scene == NULL) {
        return;
    }
    reader_apply_background(scene->background_img);

    if (scene->overlay_img == GAL_IMG_NONE) {
        style_hidden(s_overlay_image, true);
    } else {
        const lv_image_dsc_t *image = gal_assets_image(&s_assets, scene->overlay_img);
        if (image == NULL) {
            style_hidden(s_overlay_image, true);
        } else {
            lv_image_set_src(s_overlay_image, image);
            lv_obj_set_pos(s_overlay_image, scene->overlay_x, scene->overlay_y);
            style_hidden(s_overlay_image, false);
        }
    }

    style_hidden(s_body_image, true);
    style_hidden(s_face_image, true);

    if (s_chapter_label != NULL && s_view == GAL_VIEW_READER) {
        char buffer[24];
        snprintf(buffer, sizeof(buffer), "%s%u%s", GAL_STR_CHAPTER_PREFIX,
                 (unsigned)(s_pos.chapter + 1), GAL_STR_CHAPTER_SUFFIX);
        lv_label_set_text(s_chapter_label, buffer);
    }
}

/* One page of the current dialogue line, NUL-terminated so it can be revealed in
 * place. The packer stores whole lines; a line too long for the panel is split by
 * gal_text_pages() and shown one page at a time, so nothing is ever hidden. */
static char s_page_text[GAL_REVEAL_CAPACITY];
static uint32_t s_page_offsets[GAL_PAGE_MAX_OFFSETS];

static void reader_show_line(void)
{
    const gal_scene_rec_t *scene = gal_chapter_scene(&s_chapter, s_pos.scene);
    if (scene == NULL) {
        return;
    }
    const gal_dlg_rec_t *line = gal_chapter_dialogue(
        &s_chapter, (uint16_t)(scene->first_dialogue + s_pos.dialogue));
    if (line == NULL) {
        return;
    }
    /* Sprites persist across lines, exactly as the source engine did. */
    if (line->body_img != GAL_IMG_NONE) {
        reader_place_sprite(s_body_image, line->body_img);
    }
    if (line->face_img != GAL_IMG_NONE) {
        reader_place_sprite(s_face_image, line->face_img);
    }

    const char *text = gal_chapter_dialogue_text(&s_chapter, line);
    uint32_t total = text != NULL ? (uint32_t)strlen(text) : 0;
    const gal_reader_layout_t layout = reader_layout();
    int pages = gal_text_pages(text, &layout.pagination, s_page_offsets,
                               GAL_PAGE_MAX_OFFSETS);
    if (pages < 1) {
        pages = 1;
    }
    if (s_pos.page >= (uint16_t)pages) {
        s_pos.page = (uint16_t)(pages - 1);
    }
    s_pos.page_count = (uint16_t)pages;

    uint32_t begin = s_pos.page < GAL_PAGE_MAX_OFFSETS ? s_page_offsets[s_pos.page] : 0;
    uint32_t end = total;
    if (s_pos.page + 1 < GAL_PAGE_MAX_OFFSETS && s_page_offsets[s_pos.page + 1] <= total &&
        s_page_offsets[s_pos.page + 1] >= begin) {
        end = s_page_offsets[s_pos.page + 1];
    }
    uint32_t length = end > begin ? end - begin : 0;
    if (length >= sizeof(s_page_text)) {
        length = sizeof(s_page_text) - 1;
    }
    if (length > 0) {
        memcpy(s_page_text, text + begin, length);
    }
    s_page_text[length] = '\0';

    start_line(s_page_text, gal_chapter_dialogue_name(&s_chapter, line));
}

/*
 * Resume points change on every line, but NVS has limited erase cycles. The live
 * position is kept in RAM and only flushed when it becomes worth a write: a new
 * chapter, opening the menu, or an explicit save.
 */
static void reader_save_resume(void)
{
    gal_slot_from_pos(&s_save->last, &s_pos);
    if (s_have_committed_chapter && s_committed_chapter == s_pos.chapter) {
        return;
    }
    s_committed_chapter = s_pos.chapter;
    s_have_committed_chapter = true;
    gal_save_commit();
}

static void build_ending(const char *ending_text)
{
    ff_stop();
    type_stop();
    s_reveal_target = NULL;
    lv_obj_t *screen = make_screen();
    make_status_bar(screen);

    lv_obj_t *label = make_label(screen, &gal_font_20, GAL_COLOR_TEXT);
    lv_obj_set_width(label, BSP_LCD_W - 24);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "%s\n\n%s", GAL_STR_ENDING,
             ending_text != NULL ? ending_text : "");
    lv_label_set_text(label, buffer);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *hint = make_label(screen, &gal_font_16, GAL_COLOR_ACCENT);
    lv_label_set_text(hint, GAL_STR_ENDING_HINT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_screen_load(screen);
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }
    s_screen = screen;
    s_view = GAL_VIEW_ENDING;
    s_textbox = NULL;
    s_name_label = NULL;
    s_text_label = NULL;
    s_hint_label = NULL;
}

static void build_reader(void)
{
    const gal_reader_layout_t layout = reader_layout();
    s_reader_text_size = s_save->text_size;
    lv_obj_t *screen = make_screen();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

    /* Creation order is z-order. The source engine draws the scene overlay last,
     * above the character, so full-screen effects (letterbox, vignette, focus
     * lines) tint the sprite as well. */
    s_bg_image = lv_image_create(screen);
    lv_obj_set_pos(s_bg_image, 0, 0);
    s_body_image = lv_image_create(screen);
    s_face_image = lv_image_create(screen);
    s_overlay_image = lv_image_create(screen);
    lv_obj_set_pos(s_overlay_image, 0, 0);

    make_status_bar(screen);

    /* The speaker name sits outside the panel, in the lower-left of the artwork,
     * and uses the small face so it never crowds the body text. */
    s_name_label = make_label(screen, &gal_font_16, GAL_COLOR_NAME);
    lv_label_set_text(s_name_label, "");
    lv_obj_set_pos(s_name_label, GAL_NAME_X, layout.name_y);
    lv_obj_set_style_bg_color(s_name_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_name_label, GAL_NAME_OPA, LV_PART_MAIN);
    lv_obj_set_style_radius(s_name_label, GAL_NAME_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_name_label, GAL_NAME_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_name_label, GAL_NAME_PAD_Y, LV_PART_MAIN);

    /* Flat, fully opaque, full width, with a single accent rule along the top
     * edge and no rounding -- the Saya reference port's panel. The upstream
     * project's panel artwork is not used: it is a near-white plate carrying a
     * repeating ornament that fights the picture at any usable opacity. */
    s_textbox = lv_obj_create(screen);
    lv_obj_remove_style_all(s_textbox);
    lv_obj_set_size(s_textbox, GAL_PANEL_WIDTH, layout.panel_h);
    lv_obj_set_pos(s_textbox, GAL_PANEL_MARGIN_X, layout.panel_top);
    lv_obj_set_style_bg_color(s_textbox, GAL_COLOR_BOX, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_textbox, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_textbox, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_textbox, GAL_PANEL_RULE_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_textbox, GAL_COLOR_LINE, LV_PART_MAIN);
    lv_obj_remove_flag(s_textbox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *text_area = lv_obj_create(s_textbox);
    lv_obj_remove_style_all(text_area);
    lv_obj_set_size(text_area, GAL_TEXT_W, layout.text_h);
    lv_obj_set_pos(text_area, GAL_TEXT_X - GAL_PANEL_MARGIN_X, GAL_TEXT_PAD_TOP);
    /* Overflow is paginated by the reader, not scrolled; the scroll direction is
     * left enabled only as a last-resort safety valve. */
    lv_obj_set_scroll_dir(text_area, LV_DIR_VER);

    s_text_label = make_label(text_area, layout.font, GAL_COLOR_TEXT);
    lv_obj_set_width(s_text_label, GAL_TEXT_W);
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_text_label, GAL_TEXT_LINE_SPACE, LV_PART_MAIN);
    lv_label_set_text(s_text_label, "");

    s_hint_label = make_label(screen, &gal_font_16, GAL_COLOR_ACCENT);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_MID, 0, GAL_STATUS_Y + 20);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(screen);
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }
    s_screen = screen;
    s_view = GAL_VIEW_READER;
    s_panel = NULL;
    s_textbox_visible = true;
}

/* --- list panels --------------------------------------------------------------- */

static void panel_close(void);
static void reader_advance(void);

static uint16_t panel_item_count(gal_panel_t kind)
{
    switch (kind) {
        case GAL_PANEL_MENU:
            return 6;
        case GAL_PANEL_SLOTS:
            return GAL_SAVE_SLOTS;
        case GAL_PANEL_CHAPTERS:
            return gal_pack_chapter_count(&s_assets.script);
        case GAL_PANEL_SETTINGS:
            return 5;
        default:
            return 0;
    }
}

static void panel_row_text(gal_panel_t kind, uint16_t index, char *buffer, size_t size)
{
    switch (kind) {
        case GAL_PANEL_MENU: {
            static const char *const items[] = {
                GAL_STR_RESUME, GAL_STR_SAVE, GAL_STR_LOAD, GAL_STR_SKIP_CHAPTER,
                GAL_STR_SETTINGS, GAL_STR_BACK_TO_TITLE,
            };
            snprintf(buffer, size, "%s", items[index]);
            break;
        }
        case GAL_PANEL_SLOTS: {
            const gal_slot_t *slot = &s_save->slots[index];
            if (!slot->valid) {
                snprintf(buffer, size, "%s%u  %s", GAL_STR_SLOT, (unsigned)(index + 1),
                         GAL_STR_EMPTY_SLOT);
            } else {
                snprintf(buffer, size, "%s%u  %s%u%s", GAL_STR_SLOT, (unsigned)(index + 1),
                         GAL_STR_CHAPTER_PREFIX, (unsigned)(slot->chapter + 1),
                         GAL_STR_CHAPTER_SUFFIX);
            }
            break;
        }
        case GAL_PANEL_CHAPTERS: {
            uint16_t chapters = gal_pack_chapter_count(&s_assets.script);
            if (index + 1 == chapters) {
                snprintf(buffer, size, "%s%u%s  %s", GAL_STR_CHAPTER_PREFIX,
                         (unsigned)(index + 1), GAL_STR_CHAPTER_SUFFIX,
                         GAL_STR_FINALE_BADGE);
            } else {
                snprintf(buffer, size, "%s%u%s", GAL_STR_CHAPTER_PREFIX,
                         (unsigned)(index + 1), GAL_STR_CHAPTER_SUFFIX);
            }
            break;
        }
        case GAL_PANEL_SETTINGS:
            switch (index) {
                case 0: {
                    const char *label = s_save->text_speed <= GAL_SPEED_INSTANT ? GAL_STR_INSTANT
                                      : s_save->text_speed <= GAL_SPEED_FAST ? GAL_STR_FAST
                                      : s_save->text_speed <= GAL_SPEED_MEDIUM ? GAL_STR_MEDIUM
                                      : GAL_STR_SLOW;
                    snprintf(buffer, size, "%s  %s", GAL_STR_TEXT_SPEED, label);
                    break;
                }
                case 1:
                    snprintf(buffer, size, "%s  %s", GAL_STR_TEXT_SIZE,
                             s_save->text_size == GAL_TEXT_SIZE_LARGE ? GAL_STR_LARGE
                                                                      : GAL_STR_SMALL);
                    break;
                case 2:
                    snprintf(buffer, size, "%s  %s", GAL_STR_AUTO_ON,
                             s_save->auto_play ? GAL_STR_ON : GAL_STR_OFF);
                    break;
                case 3:
                    snprintf(buffer, size, "%s", GAL_STR_RESET_DEFAULTS);
                    break;
                default:
                    snprintf(buffer, size, "%s", GAL_STR_BACK);
                    break;
            }
            break;
        default:
            buffer[0] = '\0';
            break;
    }
}

static void panel_render(void)
{
    if (s_panel == NULL) {
        return;
    }
    if (s_panel_cursor < s_panel_top) {
        s_panel_top = s_panel_cursor;
    }
    if (s_panel_cursor >= s_panel_top + GAL_LIST_VISIBLE) {
        s_panel_top = (uint16_t)(s_panel_cursor - GAL_LIST_VISIBLE + 1);
    }

    for (uint16_t row = 0; row < GAL_LIST_VISIBLE; row++) {
        lv_obj_t *label = s_panel_rows[row];
        if (label == NULL) {
            continue;
        }
        uint16_t index = (uint16_t)(s_panel_top + row);
        if (index >= s_panel_count) {
            lv_label_set_text(label, "");
            style_hidden(label, true);
            continue;
        }
        char buffer[64];
        panel_row_text(s_panel_kind, index, buffer, sizeof(buffer));
        if (index == s_panel_cursor) {
            lv_label_set_text_fmt(label, "> %s", buffer);
            lv_obj_set_style_text_color(label, GAL_COLOR_PANEL, LV_PART_MAIN);
            lv_obj_set_style_bg_color(label, GAL_COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_label_set_text_fmt(label, "  %s", buffer);
            lv_obj_set_style_text_color(label, GAL_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        style_hidden(label, false);
    }
}

static void panel_open(gal_panel_t kind, const char *title)
{
    panel_close();

    s_panel_kind = kind;
    s_panel_count = panel_item_count(kind);
    s_panel_top = 0;
    s_panel_cursor = 0;
    if (kind == GAL_PANEL_CHAPTERS) {
        /* Open next to where the reader already is. */
        s_panel_cursor = s_view == GAL_VIEW_READER ? s_pos.chapter : 0;
        if (s_panel_cursor >= s_panel_count) {
            s_panel_cursor = 0;
        }
    }
    if (kind == GAL_PANEL_SLOTS) {
        for (uint16_t i = 0; i < GAL_SAVE_SLOTS; i++) {
            if (!s_save->slots[i].valid) {
                s_panel_cursor = i;
                break;
            }
        }
    }

    lv_obj_t *parent = s_screen;
    s_panel = lv_obj_create(parent);
    lv_obj_set_size(s_panel, BSP_LCD_W, BSP_LCD_H);
    lv_obj_set_pos(s_panel, 0, 0);
    make_opaque(s_panel, GAL_COLOR_PANEL);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_panel_title = make_label(s_panel, &gal_font_20, GAL_COLOR_ACCENT);
    lv_label_set_text(s_panel_title, title);
    lv_obj_set_pos(s_panel_title, 10, 12);

    for (uint16_t row = 0; row < GAL_LIST_VISIBLE; row++) {
        lv_obj_t *label = make_label(s_panel, &gal_font_16, GAL_COLOR_TEXT);
        lv_obj_set_size(label, BSP_LCD_W - 20, 26);
        lv_obj_set_pos(label, 10, (int32_t)(48 + row * 30));
        lv_obj_set_style_radius(label, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_top(label, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(label, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(label, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_right(label, 0, LV_PART_MAIN);
        s_panel_rows[row] = label;
    }

    if (kind == GAL_PANEL_SETTINGS) {
        /* The upstream settings page previewed the chosen speed and size with a
         * live typewriter; reuse the reader's machinery so the preview is real
         * rather than a description of the setting. */
        s_preview_label = make_label(s_panel, body_font(), GAL_COLOR_DIM);
        lv_obj_set_size(s_preview_label, BSP_LCD_W - 20, 64);
        lv_obj_set_pos(s_preview_label, 10, 232);
        lv_label_set_long_mode(s_preview_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(s_preview_label, GAL_TEXT_LINE_SPACE,
                                         LV_PART_MAIN);
        begin_reveal(GAL_STR_PREVIEW, s_preview_label);
    }

    panel_render();
}

static void panel_close(void)
{
    if (s_preview_label != NULL && s_reveal_target == s_preview_label) {
        /* The preview is about to be deleted; stop first so the typewriter timer
         * cannot write through a dangling pointer. */
        type_stop();
        s_reveal_target = NULL;
    }
    s_preview_label = NULL;
    if (s_panel != NULL) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
    }
    for (uint16_t row = 0; row < GAL_LIST_VISIBLE; row++) {
        s_panel_rows[row] = NULL;
    }
    s_panel_title = NULL;
    s_panel_kind = GAL_PANEL_NONE;

    /* Changing the text size resizes the panel, so the reader is rebuilt before it
     * is shown again. Doing it here keeps the settings preview live while the
     * panel is open, and applies the new geometry the moment it closes. */
    if (s_view == GAL_VIEW_READER && s_reader_text_size != s_save->text_size) {
        build_reader();
        reader_show_scene();
        reader_show_line();
    }
}

/* --- navigation ---------------------------------------------------------------- */

static void open_chapter(uint16_t chapter)
{
    const gal_reader_layout_t layout = reader_layout();
    if (!gal_reader_begin(&s_assets.script, &s_pos, chapter, &layout.pagination)) {
        ESP_LOGW(TAG, "chapter %u is not available", (unsigned)chapter);
        return;
    }
    if (!gal_pack_chapter_open(&s_assets.script, s_pos.chapter, &s_chapter)) {
        return;
    }
    build_reader();
    reader_show_scene();
    reader_show_line();
    reader_save_resume();
    gal_save_commit();
}

static void reader_advance(void)
{
    const gal_reader_layout_t layout = reader_layout();
    gal_step_t step = gal_reader_advance(&s_assets.script, &s_pos, &s_chapter,
                                        &layout.pagination);
    switch (step) {
        case GAL_STEP_PAGE:
        case GAL_STEP_LINE:
            reader_show_line();
            break;
        case GAL_STEP_SCENE:
        case GAL_STEP_CHAPTER:
            reader_show_scene();
            reader_show_line();
            break;
        case GAL_STEP_FINALE: {
            const gal_scene_rec_t *scene = gal_chapter_scene(&s_chapter, s_pos.scene);
            const char *ending = "";
            if (scene != NULL) {
                const gal_dlg_rec_t *line = gal_chapter_dialogue(
                    &s_chapter, (uint16_t)(scene->first_dialogue + s_pos.dialogue));
                if (line != NULL) {
                    ending = gal_chapter_dialogue_text(&s_chapter, line);
                }
            }
            /* The story is over; there is nothing to resume into. */
            memset(&s_save->last, 0, sizeof(s_save->last));
            s_have_committed_chapter = false;
            gal_save_commit();
            build_ending(ending);
            return;
        }
        default:
            return;
    }
    reader_save_resume();
}

/* Advance repeatedly while the up key is held. */
static void ff_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ff_active || s_view != GAL_VIEW_READER || s_panel != NULL) {
        ff_stop();
        return;
    }
    int mv = bsp_button_read_mv();
    if (mv < 0 || mv > GAL_BTN_UP_MAX_MV) {
        /* Either the key was released or the reading failed; either way, stop. */
        ff_stop();
        return;
    }
    if (s_revealing) {
        reveal_all();
    } else {
        reader_advance();
    }
}

static void ff_start(void)
{
    s_ff_active = true;
    if (s_ff_timer != NULL) {
        lv_timer_set_period(s_ff_timer, GAL_FF_INTERVAL_MS);
        lv_timer_reset(s_ff_timer);
        lv_timer_resume(s_ff_timer);
    }
    show_hint(GAL_STR_FAST_FORWARD);
}

static void title_render(void)
{
    static const char *const items[] = {
        GAL_STR_START, GAL_STR_CONTINUE, GAL_STR_CHAPTERS, GAL_STR_SETTINGS,
    };
    for (uint8_t i = 0; i < 4; i++) {
        if (s_title_items[i] == NULL) {
            continue;
        }
        if (i == s_title_cursor) {
            lv_label_set_text_fmt(s_title_items[i], "> %s", items[i]);
            lv_obj_set_style_text_color(s_title_items[i], GAL_COLOR_ACCENT, LV_PART_MAIN);
        } else {
            lv_label_set_text_fmt(s_title_items[i], "  %s", items[i]);
            lv_obj_set_style_text_color(s_title_items[i], GAL_COLOR_TEXT, LV_PART_MAIN);
        }
    }
}

static void build_title(void)
{
    ff_stop();
    type_stop();
    s_reveal_target = NULL;
    lv_obj_t *screen = make_screen();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

    uint16_t solid = 0xFFFF;
    int32_t is_solid = -1;
    const lv_image_dsc_t *backdrop = gal_assets_find(&s_assets, "bg/index_bg.png", &is_solid);
    if (backdrop != NULL) {
        lv_obj_t *image = lv_image_create(screen);
        lv_image_set_src(image, backdrop);
        lv_obj_set_pos(image, 0, 0);
    } else if (is_solid >= 0) {
        solid = (uint16_t)is_solid;
        lv_obj_set_style_bg_color(screen, lv_color_make((solid >> 11) << 3,
                                                        ((solid >> 5) & 0x3F) << 2,
                                                        (solid & 0x1F) << 3),
                                  LV_PART_MAIN);
    }

    make_status_bar(screen);
    lv_obj_add_flag(s_chapter_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(screen, &gal_font_20, GAL_COLOR_TEXT);
    lv_label_set_text(title, GAL_STR_APP_NAME);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* Control legend, so the remapped keys are discoverable without a manual. */
    lv_obj_t *help = make_label(screen, &gal_font_16, GAL_COLOR_DIM);
    lv_obj_set_width(help, BSP_LCD_W - 24);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(help, GAL_TEXT_LINE_SPACE, LV_PART_MAIN);
    lv_label_set_text(help, GAL_STR_HELP_ADVANCE "\n" GAL_STR_HELP_HOLD "\n"
                            GAL_STR_HELP_MENU);
    lv_obj_align(help, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, BSP_LCD_W - 40, 4 * 30 + 16);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -14);
    make_opaque(panel, GAL_COLOR_PANEL);
    lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *item = make_label(panel, &gal_font_16, GAL_COLOR_TEXT);
        lv_obj_set_pos(item, 8, (int32_t)(8 + i * 30));
        s_title_items[i] = item;
    }
    title_render();

    s_hint_label = make_label(screen, &gal_font_16, GAL_COLOR_ACCENT);
    lv_label_set_text(s_hint_label, GAL_STR_READER_HINT);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    if (s_hint_timer != NULL) {
        lv_timer_resume(s_hint_timer);
    }

    lv_screen_load(screen);
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }
    s_screen = screen;
    s_view = GAL_VIEW_TITLE;
    s_panel = NULL;
    s_textbox = NULL;
    s_name_label = NULL;
    s_text_label = NULL;
    s_bg_image = NULL;
    s_overlay_image = NULL;
    s_body_image = NULL;
    s_face_image = NULL;
}

static void build_error(void)
{
    ff_stop();
    type_stop();
    s_reveal_target = NULL;
    lv_obj_t *screen = make_screen();

    lv_obj_t *title = make_label(screen, &gal_font_20, GAL_COLOR_ACCENT);
    lv_label_set_text(title, GAL_STR_NO_ASSETS);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *detail = make_label(screen, &gal_font_16, GAL_COLOR_TEXT);
    lv_obj_set_width(detail, BSP_LCD_W - 24);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_label_set_text(detail, GAL_STR_NO_ASSETS_HINT);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 24);

    lv_screen_load(screen);
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }
    s_screen = screen;
    s_view = GAL_VIEW_ERROR;
}

static void slot_action(uint16_t index)
{
    const gal_reader_layout_t layout = reader_layout();
    if (s_slot_is_save) {
        gal_slot_from_pos(&s_save->slots[index], &s_pos);
        gal_save_commit();
        show_hint(GAL_STR_SAVED);
    } else {
        gal_reader_pos_t target;
        if (!gal_slot_to_pos(&s_save->slots[index], &target)) {
            show_hint(GAL_STR_NO_SAVES);
            return;
        }
        panel_close();
        open_chapter(target.chapter);
        /* Land on the stored line rather than the start of the chapter. */
        if (gal_reader_clamp(&s_assets.script, &target, &s_chapter, &layout.pagination)) {
            s_pos = target;
            reader_show_scene();
            reader_show_line();
            reader_save_resume();
        }
        show_hint(GAL_STR_LOADED);
    }
    panel_render();
}

static void settings_adjust(uint16_t index)
{
    switch (index) {
        case 0:
            /* One press faster each time, wrapping from 瞬间 back to 慢. */
            s_save->text_speed = gal_speed_next(s_save->text_speed);
            if (s_type_timer != NULL) {
                lv_timer_set_period(s_type_timer, s_save->text_speed);
            }
            break;
        case 1:
            s_save->text_size = s_save->text_size == GAL_TEXT_SIZE_SMALL
                                    ? GAL_TEXT_SIZE_LARGE : GAL_TEXT_SIZE_SMALL;
            if (s_text_label != NULL) {
                lv_obj_set_style_text_font(s_text_label, body_font(), LV_PART_MAIN);
            }
            break;
        case 2:
            s_save->auto_play = !s_save->auto_play;
            if (!s_save->auto_play && s_auto_timer != NULL) {
                lv_timer_pause(s_auto_timer);
            }
            break;
        case 3: {
            gal_save_data_t keep = *s_save;
            gal_save_defaults(s_save);
            /* Keep the reader's place; only presentation is reset. */
            s_save->last = keep.last;
            memcpy(s_save->slots, keep.slots, sizeof(s_save->slots));
            if (s_type_timer != NULL) {
                lv_timer_set_period(s_type_timer, s_save->text_speed);
            }
            if (s_text_label != NULL) {
                lv_obj_set_style_text_font(s_text_label, body_font(), LV_PART_MAIN);
            }
            break;
        }
        default:
            panel_close();
            return;
    }
    gal_save_commit();
    if (s_preview_label != NULL) {
        lv_obj_set_style_text_font(s_preview_label, body_font(), LV_PART_MAIN);
        begin_reveal(GAL_STR_PREVIEW, s_preview_label);
    }
    panel_render();
}

static void panel_activate(void)
{
    switch (s_panel_kind) {
        case GAL_PANEL_MENU:
            switch (s_panel_cursor) {
                case 0:
                    panel_close();
                    break;
                case 1:
                    s_slot_is_save = true;
                    panel_open(GAL_PANEL_SLOTS, GAL_STR_SAVE);
                    break;
                case 2:
                    s_slot_is_save = false;
                    panel_open(GAL_PANEL_SLOTS, GAL_STR_LOAD);
                    break;
                case 3: {
                    uint16_t next = (uint16_t)(s_pos.chapter + 1);
                    uint16_t chapters = gal_pack_chapter_count(&s_assets.script);
                    if (next >= chapters) {
                        next = (uint16_t)(chapters - 1);
                    }
                    panel_close();
                    open_chapter(next);
                    break;
                }
                case 4:
                    panel_open(GAL_PANEL_SETTINGS, GAL_STR_SETTINGS);
                    break;
                default:
                    panel_close();
                    build_title();
                    break;
            }
            break;

        case GAL_PANEL_SLOTS:
            slot_action(s_panel_cursor);
            break;

        case GAL_PANEL_CHAPTERS:
            panel_close();
            open_chapter(s_panel_cursor);
            break;

        case GAL_PANEL_SETTINGS:
            settings_adjust(s_panel_cursor);
            break;

        default:
            panel_close();
            break;
    }
}

/* --- input ---------------------------------------------------------------------- */

static void panel_move(int delta)
{
    if (s_panel_count == 0) {
        return;
    }
    int next = (int)s_panel_cursor + delta;
    if (next < 0) {
        next = s_panel_count - 1;
    } else if (next >= (int)s_panel_count) {
        next = 0;
    }
    s_panel_cursor = (uint16_t)next;
    panel_render();
}

static void reader_scroll(int delta)
{
    if (s_text_label == NULL) {
        return;
    }
    lv_obj_t *area = lv_obj_get_parent(s_text_label);
    if (area == NULL) {
        return;
    }
    lv_obj_scroll_by(area, 0, delta, LV_ANIM_OFF);
}

void gal_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!bsp_lvgl_lock(100)) {
        return;
    }

    if (s_view == GAL_VIEW_ERROR) {
        bsp_lvgl_unlock();
        return;
    }

    if (s_panel != NULL) {
        if (ev == BSP_BTN_LONG) {
            panel_close();
            /* Leaving the panel is a natural point to persist the resume point. */
            gal_save_commit();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                panel_move(-1);
            } else if (btn == BSP_BTN_DOWN) {
                panel_move(1);
            } else {
                panel_activate();
            }
        }
        bsp_lvgl_unlock();
        return;
    }

    switch (s_view) {
        case GAL_VIEW_TITLE:
            if (ev == BSP_BTN_CLICK) {
                if (btn == BSP_BTN_UP) {
                    s_title_cursor = (uint8_t)((s_title_cursor + 3) % 4);
                    title_render();
                } else if (btn == BSP_BTN_DOWN) {
                    s_title_cursor = (uint8_t)((s_title_cursor + 1) % 4);
                    title_render();
                } else {
                    switch (s_title_cursor) {
                        case 0:
                        case 1: {
                            gal_reader_pos_t target;
                            uint16_t chapter = 0;
                            if (s_title_cursor == 1 &&
                                gal_slot_to_pos(&s_save->last, &target)) {
                                chapter = target.chapter;
                            }
                            open_chapter(chapter);
                            if (s_title_cursor == 1) {
                                const gal_reader_layout_t layout = reader_layout();
                                gal_reader_pos_t saved;
                                if (gal_slot_to_pos(&s_save->last, &saved) &&
                                    gal_reader_clamp(&s_assets.script, &saved, &s_chapter,
                                                     &layout.pagination)) {
                                    s_pos = saved;
                                    reader_show_scene();
                                    reader_show_line();
                                    reader_save_resume();
                                }
                            }
                            break;
                        }
                        case 2:
                            panel_open(GAL_PANEL_CHAPTERS, GAL_STR_CHAPTERS);
                            break;
                        default:
                            panel_open(GAL_PANEL_SETTINGS, GAL_STR_SETTINGS);
                            break;
                    }
                }
            }
            break;

        case GAL_VIEW_READER:
            if (ev == BSP_BTN_LONG) {
                if (btn == BSP_BTN_UP) {
                    ff_start();
                } else if (btn == BSP_BTN_DOWN) {
                    s_textbox_visible = !s_textbox_visible;
                    style_hidden(s_textbox, !s_textbox_visible);
                    show_hint(s_textbox_visible ? GAL_STR_SHOW_TEXT : GAL_STR_HIDE_TEXT);
                }
                /* Confirm-long has no action here: the menu opens on a short press. */
            } else if (ev == BSP_BTN_CLICK) {
                ff_stop();
                if (btn == BSP_BTN_OK) {
                    panel_open(GAL_PANEL_MENU, GAL_STR_MENU);
                } else if (btn == BSP_BTN_DOWN) {
                    /* Safety valve for the rare line that even pagination leaves
                     * wider than the panel; every new page resets to the top. */
                    reader_scroll(-reader_layout().line_h);
                } else if (s_revealing) {
                    reveal_all();
                } else {
                    reader_advance();
                }
            }
            break;

        case GAL_VIEW_ENDING:
            if (ev == BSP_BTN_CLICK) {
                build_title();
            }
            break;

        default:
            break;
    }

    bsp_lvgl_unlock();
}

/* --- lifecycle -------------------------------------------------------------------- */

bool gal_app_start(void)
{
    s_save = gal_save_data();
    gal_save_init();

    esp_err_t err = gal_assets_init(&s_assets);
    if (err != ESP_OK || !gal_assets_ready(&s_assets)) {
        ESP_LOGE(TAG, "assets unavailable: %s", gal_assets_failure());
        build_error();
        return false;
    }

    /* The dialogue panel is styled in code, so the pack only has to be readable. */
    s_type_timer = lv_timer_create(type_tick, s_save->text_speed, NULL);
    s_auto_timer = lv_timer_create(auto_tick, (uint32_t)s_save->auto_delay * 100, NULL);
    s_status_timer = lv_timer_create(status_tick, 30000, NULL);
    s_hint_timer = lv_timer_create(hint_tick, 1600, NULL);
    s_ff_timer = lv_timer_create(ff_tick, GAL_FF_INTERVAL_MS, NULL);
    if (s_type_timer == NULL || s_auto_timer == NULL || s_status_timer == NULL ||
        s_hint_timer == NULL || s_ff_timer == NULL) {
        ESP_LOGE(TAG, "cannot create timers");
        build_error();
        return false;
    }
    lv_timer_pause(s_type_timer);
    lv_timer_pause(s_auto_timer);
    lv_timer_pause(s_hint_timer);
    lv_timer_pause(s_ff_timer);

    build_title();
    status_tick(NULL);
    ESP_LOGI(TAG, "galgame ready");
    return true;
}
