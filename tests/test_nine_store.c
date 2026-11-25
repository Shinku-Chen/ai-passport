// Slot/progress/permanent record round-trip and corruption handling.
#include "nine_store.h"
#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(nine_store_init());
    nine_slot_t slots[NINE_SLOT_COUNT];
    assert(nine_store_load_slots(0x12345678u, slots) == NINE_STORE_MISSING);

    nine_slot_t value;
    memset(&value, 0, sizeof(value));
    value.occupied = true;
    value.page = 7;
    value.chapter = 3;
    strcpy(value.preview, "测试预览");
    value.vm_size = 196;
    for (size_t i = 0; i < value.vm_size; ++i) value.vm[i] = (uint8_t)(i * 7 + 3);
    assert(nine_store_write_slot(0x12345678u, 1, &value) == NINE_STORE_OK);

    assert(nine_store_load_slots(0x12345678u, slots) == NINE_STORE_OK);
    assert(slots[1].occupied && slots[0].occupied == false);
    assert(slots[1].page == 7 && slots[1].chapter == 3);
    assert(strcmp(slots[1].preview, "测试预览") == 0);
    assert(slots[1].vm_size == 196 && memcmp(slots[1].vm, value.vm, 196) == 0);

    // Wrong pack identity is rejected without returning stale slot data.
    assert(nine_store_load_slots(0x99999999u, slots) == NINE_STORE_IDENTITY);
    assert(slots[1].occupied && slots[1].vm_size == 0);

    // A corrupted byte fails CRC.
    assert(nvs_stub_mutate("s1", 40, 0x40));
    assert(nine_store_load_slots(0x12345678u, slots) == NINE_STORE_CRC);
    assert(nine_store_erase_slot(1) == NINE_STORE_OK);
    assert(nine_store_load_slots(0x12345678u, slots) == NINE_STORE_MISSING);

    uint8_t progress[196];
    for (size_t i = 0; i < sizeof(progress); ++i) progress[i] = (uint8_t)i;
    assert(nine_store_write_progress(0x12345678u, progress, sizeof(progress), 9) == NINE_STORE_OK);
    uint8_t readback[196];
    size_t size = 0;
    uint16_t page = 0;
    assert(nine_store_load_progress(0x12345678u, readback, sizeof(readback), &size, &page) == NINE_STORE_OK);
    assert(size == sizeof(progress) && page == 9 && memcmp(progress, readback, size) == 0);
    assert(nine_store_load_progress(0x99999999u, readback, sizeof(readback), &size, &page) == NINE_STORE_IDENTITY);

    assert(nine_store_write_perm(0x12345678u, progress, 64) == NINE_STORE_OK);
    assert(nine_store_load_perm(0x12345678u, readback, sizeof(readback), &size) == NINE_STORE_OK);
    assert(size == 64 && memcmp(progress, readback, 64) == 0);

    uint8_t background = 0;
    assert(nine_store_load_title_bg(&background) == NINE_STORE_MISSING);
    assert(nine_store_write_title_bg(2) == NINE_STORE_OK);
    assert(nine_store_load_title_bg(&background) == NINE_STORE_OK && background == 2);
    assert(nine_store_write_title_bg(3) == NINE_STORE_IO);

    puts("nine persistent slot/progress records: PASS");
    return 0;
}
