// NVS-backed reader state. Every record is self-describing and validated before
// it is applied; a corrupt or foreign record is reported, never silently used.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvs.h"
#include "nine_pack.h"

#define NINE_SLOTS 30
#define NINE_SLOT_META_MAX 32
#define NINE_SAVE_RECORD_MAX 256
#define NINE_PROGRESS_RECORD_MAX 160
#define NINE_UNLOCK_BYTES 16

typedef struct {
    uint8_t text_speed;        // 0 slow .. 3 instant
    uint8_t font_large;        // 0 = 16px, 1 = 20px
    uint8_t auto_play;         // default for a new reading session
    uint8_t transparency;      // 10..55 %
    uint8_t sound;             // 0/1
    uint8_t title_bg;          // last selected title visual (0..2, gated by progress)
} nine_cfg_t;

typedef struct {
    bool occupied;
    uint16_t node;
    uint32_t index;
    uint16_t page;
    uint16_t chapter;          // pack chapter index, NINE_NODE_NONE when unknown
} nine_slot_meta_t;

typedef struct {
    nvs_handle_t handle;
    bool ok;                   // NVS opened; false means volatile-only fallback
    uint32_t writes;           // diagnostics for the host regression
} nine_store_t;

void nine_cfg_defaults(nine_cfg_t *cfg);

bool nine_store_init(nine_store_t *store, bool nvs_available);
bool nine_store_cfg_load(nine_store_t *store, nine_cfg_t *out);
bool nine_store_cfg_save(nine_store_t *store, const nine_cfg_t *cfg);

bool nine_store_unlock_load(nine_store_t *store, uint8_t bits[NINE_UNLOCK_BYTES]);
bool nine_store_unlock_save(nine_store_t *store, const uint8_t bits[NINE_UNLOCK_BYTES]);

bool nine_store_progress_load(nine_store_t *store, uint8_t *buf, size_t cap, size_t *size);
bool nine_store_progress_save(nine_store_t *store, const uint8_t *buf, size_t size);

bool nine_store_auto_load(nine_store_t *store, uint8_t *buf, size_t cap, size_t *size);
bool nine_store_auto_save(nine_store_t *store, const uint8_t *buf, size_t size);
bool nine_store_auto_clear(nine_store_t *store);

// Reads all slot headers once; a damaged header is reported as occupied but
// unreadable through the normal load path (never erased automatically).
void nine_store_slot_scan(nine_store_t *store, const nine_pack_t *pack,
                          nine_slot_meta_t meta[NINE_SLOTS]);
bool nine_store_slot_load(nine_store_t *store, int slot, uint8_t *buf, size_t cap,
                          size_t *size);
bool nine_store_slot_save(nine_store_t *store, int slot, const uint8_t *buf, size_t size);

bool nine_store_sound_save(nine_store_t *store, uint8_t sound);
uint8_t nine_store_sound_load(nine_store_t *store);
