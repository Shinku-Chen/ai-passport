/*
 * Persistent reader state: the resume point, six manual save slots and the
 * display settings.
 *
 * The whole structure is one NVS blob, so a save is a single atomic write and a
 * partially written state cannot be read back. Defaults and validation live in
 * the header as pure functions so they can be exercised by a host test without
 * NVS, and so a corrupt or older blob is repaired rather than trusted.
 */
#ifndef GAL_SAVE_H
#define GAL_SAVE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "gal_script.h"

#define GAL_SAVE_SLOTS 6
#define GAL_SAVE_MAGIC 0x56415347u /* "GSAV" */
/* Bumped when the slot layout changes; an older blob is reset rather than read. */
#define GAL_SAVE_VERSION 2

/* Character reveal interval in milliseconds. Zero means the whole line appears at
 * once; the settings row cycles through these four presets, fastest last. */
#define GAL_SPEED_INSTANT 0
#define GAL_SPEED_FAST    20
#define GAL_SPEED_MEDIUM  40
#define GAL_SPEED_SLOW    80
#define GAL_SPEED_MIN     GAL_SPEED_INSTANT
#define GAL_SPEED_MAX     GAL_SPEED_SLOW
#define GAL_SPEED_DEFAULT GAL_SPEED_MEDIUM

/* Body text size; both are generated font subsets. */
#define GAL_TEXT_SIZE_SMALL 16
#define GAL_TEXT_SIZE_LARGE 20
#define GAL_TEXT_SIZE_DEFAULT GAL_TEXT_SIZE_SMALL

typedef struct {
    uint16_t chapter;
    uint16_t scene;
    uint16_t dialogue;
    uint16_t page; /* which page of that line the reader was on */
    uint8_t valid;
    uint8_t reserved;
} gal_slot_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t text_speed;
    uint16_t text_size;
    uint8_t auto_play;
    uint8_t auto_delay; /* tenths of a second after a line finishes */
    gal_slot_t last;    /* auto-resume point */
    gal_slot_t slots[GAL_SAVE_SLOTS];
} gal_save_data_t;

static inline void gal_save_defaults(gal_save_data_t *data)
{
    uint8_t *raw = (uint8_t *)data;
    for (uint32_t i = 0; i < sizeof(*data); i++) {
        raw[i] = 0;
    }
    data->magic = GAL_SAVE_MAGIC;
    data->version = GAL_SAVE_VERSION;
    data->text_speed = GAL_SPEED_DEFAULT;
    data->text_size = GAL_TEXT_SIZE_DEFAULT;
    data->auto_play = 0;
    data->auto_delay = 12;
}

/*
 * True when the blob is usable as-is. A blob from a different version, or one
 * whose settings are out of range, is rejected and replaced with defaults by the
 * caller instead of being partially applied.
 */
static inline bool gal_save_valid(const gal_save_data_t *data)
{
    if (data->magic != GAL_SAVE_MAGIC || data->version != GAL_SAVE_VERSION) {
        return false;
    }
    /* Zero (instant) is the lowest preset, and an unsigned field cannot go below
     * it, so only the upper bound needs checking. */
    if (data->text_speed > GAL_SPEED_MAX) {
        return false;
    }
    if (data->text_size != GAL_TEXT_SIZE_SMALL && data->text_size != GAL_TEXT_SIZE_LARGE) {
        return false;
    }
    if (data->auto_delay == 0 || data->auto_delay > 100) {
        return false;
    }
    return true;
}

static inline void gal_slot_from_pos(gal_slot_t *slot, const gal_reader_pos_t *pos)
{
    slot->chapter = pos->chapter;
    slot->scene = pos->scene;
    slot->dialogue = pos->dialogue;
    slot->page = pos->page;
    slot->valid = 1;
    slot->reserved = 0;
}

static inline bool gal_slot_to_pos(const gal_slot_t *slot, gal_reader_pos_t *pos)
{
    if (!slot->valid) {
        return false;
    }
    pos->chapter = slot->chapter;
    pos->scene = slot->scene;
    pos->dialogue = slot->dialogue;
    pos->page = slot->page;
    pos->page_count = 1; /* recomputed against the current typesetting when read */
    return true;
}

/* One press faster each time, wrapping from the fastest back to the slowest. */
static inline uint16_t gal_speed_next(uint16_t current)
{
    if (current >= GAL_SPEED_SLOW) {
        return GAL_SPEED_MEDIUM;
    }
    if (current > GAL_SPEED_FAST) {
        return GAL_SPEED_FAST;
    }
    if (current > GAL_SPEED_INSTANT) {
        return GAL_SPEED_INSTANT;
    }
    return GAL_SPEED_SLOW;
}

/* --- NVS backed operations -------------------------------------------------- */

esp_err_t gal_save_init(void);
/* Never NULL once gal_save_init() has been called. */
gal_save_data_t *gal_save_data(void);
/* Write the current structure to NVS. */
esp_err_t gal_save_commit(void);
/* Drop every slot, keeping the settings. */
void gal_save_clear_slots(void);

#endif /* GAL_SAVE_H */
