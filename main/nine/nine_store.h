// Persistent reader state, independent from the framework's placeholder save
// tables. Manual slots, automatic progress and permanent progression are three
// separate records: loading an old manual save never rolls back a completed
// ending/title unlock.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nine_vm.h"

#define NINE_SLOT_COUNT 3
#define NINE_SLOT_CHAPTER 32
#define NINE_SLOT_PREVIEW 64
#define NINE_SLOT_MAX 1280

typedef enum {
    NINE_STORE_OK = 0,
    NINE_STORE_MISSING,
    NINE_STORE_IO,
    NINE_STORE_FORMAT,
    NINE_STORE_CRC,
    NINE_STORE_IDENTITY,
} nine_store_status_t;

typedef struct {
    bool occupied;
    uint16_t page;
    uint16_t chapter;
    char preview[NINE_SLOT_PREVIEW + 1];
    uint8_t vm[NINE_SAVE_CAPACITY];
    size_t vm_size;
} nine_slot_t;

bool nine_store_init(void);
// The pack identity binds every record to this exact pack build.
nine_store_status_t nine_store_load_slots(uint32_t identity,
                                          nine_slot_t slots[NINE_SLOT_COUNT]);
nine_store_status_t nine_store_write_slot(uint32_t identity, int slot,
                                          const nine_slot_t *value);
nine_store_status_t nine_store_erase_slot(int slot);

// Automatic progress is position + run flags + page, never permanent-only data.
nine_store_status_t nine_store_load_progress(uint32_t identity, uint8_t *data,
                                             size_t cap, size_t *size, uint16_t *page);
nine_store_status_t nine_store_write_progress(uint32_t identity, const uint8_t *data,
                                              size_t size, uint16_t page);

// Permanent progression (loop/true flags, title backgrounds, ending gates).
nine_store_status_t nine_store_load_perm(uint32_t identity, uint8_t *data, size_t cap,
                                         size_t *size);
nine_store_status_t nine_store_write_perm(uint32_t identity, const uint8_t *data,
                                          size_t size);

// Selected title key visual (0..2). Unlock eligibility is derived separately.
nine_store_status_t nine_store_load_title_bg(uint8_t *index);
nine_store_status_t nine_store_write_title_bg(uint8_t index);
