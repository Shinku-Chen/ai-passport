// Per-title NVS storage. Every record carries a magic, version, episode id and
// CRC32, so a record written by another episode (or a torn write) never loads.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gal_settings.h"

#define NINE_STORE_NAMESPACE "nineyuki"
#define NINE_STORE_SLOTS 3u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t endings;      // bit0 = loop1, bit1 = true
    uint8_t bg;           // preferred title background 0..2
    uint32_t chapters;    // bit per chapter visited (23 used)
    uint32_t crc;
} nine_unlock_blob_t;

typedef struct {
    bool ok;
    bool dirty;
} nine_store_t;

typedef struct {
    gal_settings_t display;
    uint8_t title_bg;
} nine_cfg_t;

bool nine_store_open(nine_store_t *store);
bool nine_store_get(nine_store_t *store, const char *key, void *out, size_t cap,
                    size_t *length);
bool nine_store_put(nine_store_t *store, const char *key, const void *data, size_t length);

// Settings: defaults on any failure, never half-applied.
void nine_cfg_default(nine_cfg_t *cfg);
bool nine_cfg_load(nine_store_t *store, nine_cfg_t *cfg);
bool nine_cfg_save(nine_store_t *store, const nine_cfg_t *cfg);

// Permanent ending / title-background record. Independent from saves.
void nine_unlock_clear(nine_unlock_blob_t *out);
bool nine_unlock_load(nine_store_t *store, nine_unlock_blob_t *out);
bool nine_unlock_save(nine_store_t *store, const nine_unlock_blob_t *in);

#ifdef NINE_STORE_HOST
// Test hooks: inspect the raw record and simulate write failures.
void nine_store_test_clear(void);
void nine_store_test_fail_writes(int fail);
#endif
