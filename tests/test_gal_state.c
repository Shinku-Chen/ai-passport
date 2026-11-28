#include "gal_progress.h"
#include "gal_save.h"
#include "gal_unlock.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

typedef struct {
    uint8_t occupied;
    uint16_t event_index;
    uint16_t page_index;
    char chapter[GAL_SAVE_CHAPTER_LEN];
    uint8_t reserved;
} slot_v1_t;
typedef struct {
    uint8_t magic, version, reserved[2];
    slot_v1_t slots[GAL_SAVE_SLOTS];
} save_v1_t;
typedef struct {
    uint8_t valid;
    uint16_t event_index;
    uint16_t page_index;
} progress_v1_t;
typedef struct {
    uint8_t magic, version, reserved[2];
    progress_v1_t progress;
} progress_blob_v1_t;

_Static_assert(sizeof(slot_v1_t) == 36, "v1 save slot layout");
_Static_assert(sizeof(save_v1_t) == 112, "v1 save blob layout");
_Static_assert(sizeof(progress_v1_t) == 6, "v1 progress layout");
_Static_assert(sizeof(progress_blob_v1_t) == 10, "v1 progress blob layout");

static void seed_v1_save(void)
{
    save_v1_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x53; old.version = 1;
    old.slots[0].occupied = 1;
    old.slots[0].event_index = 7;
    old.slots[0].page_index = 2;
    snprintf(old.slots[0].chapter, sizeof(old.slots[0].chapter), "%s", "legacy");
    nvs_handle_t h;
    CHECK(nvs_open("galsave", NVS_READWRITE, &h) == 0, "open galsave");
    CHECK(nvs_set_blob(h, "slots", &old, sizeof(old)) == 0, "seed v1 save");
    CHECK(nvs_commit(h) == 0, "commit v1 save");
}

static void seed_v1_progress(void)
{
    progress_blob_v1_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x50; old.version = 1;
    old.progress.valid = 1;
    old.progress.event_index = 11;
    old.progress.page_index = 3;
    nvs_handle_t h;
    CHECK(nvs_open("galprog", NVS_READWRITE, &h) == 0, "open galprog");
    CHECK(nvs_set_blob(h, "pos", &old, sizeof(old)) == 0, "seed v1 progress");
    CHECK(nvs_commit(h) == 0, "commit v1 progress");
}

int main(void)
{
    CHECK(gal_save_init(), "save init");
    CHECK(gal_progress_init(), "progress init");
    CHECK(gal_unlock_init(), "unlock init");

    const uint32_t FLAG_A = 1u << 0;
    const uint32_t FLAG_B = 1u << 1;
    CHECK(gal_flags_has(FLAG_A | FLAG_B, 0), "flags has A");
    CHECK(gal_flags_has(FLAG_A | FLAG_B, 1), "flags has B");
    CHECK(!gal_flags_has(FLAG_A, 1), "flags lacks B");
    CHECK(gal_flags_set(FLAG_A, 2) == (FLAG_A | (1u << 2)), "flags set C");

    // v2 save: flags travel with the slot, permanent unlock does not.
    gal_save_t save;
    gal_save_clear(&save);
    save.slots[1].occupied = 1;
    save.slots[1].event_index = 21;
    save.slots[1].page_index = 4;
    save.slots[1].flags = FLAG_A;
    snprintf(save.slots[1].chapter, sizeof(save.slots[1].chapter), "%s", "route A");
    CHECK(gal_save_store(&save), "v2 save store");
    gal_save_t loaded;
    CHECK(gal_save_load(&loaded), "v2 save load");
    CHECK(loaded.slots[1].occupied && loaded.slots[1].event_index == 21 &&
              loaded.slots[1].page_index == 4, "v2 save fields");
    CHECK(loaded.slots[1].flags == FLAG_A, "v2 save flags");

    // v1 save is accepted with flags reset to zero.
    seed_v1_save();
    gal_save_t legacy;
    CHECK(gal_save_load(&legacy), "v1 save load");
    CHECK(legacy.slots[0].occupied && legacy.slots[0].event_index == 7 &&
              strcmp(legacy.slots[0].chapter, "legacy") == 0 &&
              legacy.slots[0].flags == 0, "v1 save converted");

    // v2 progress: flags travel with the auto/continue snapshot.
    gal_progress_t progress;
    gal_progress_clear(&progress);
    progress.valid = 1;
    progress.event_index = 31;
    progress.page_index = 1;
    progress.flags = FLAG_B;
    CHECK(gal_progress_store(&progress), "v2 progress store");
    gal_progress_t pload;
    CHECK(gal_progress_load(&pload), "v2 progress load");
    CHECK(pload.valid && pload.event_index == 31 && pload.page_index == 1 &&
              pload.flags == FLAG_B, "v2 progress fields");

    // v1 progress is accepted with flags reset to zero.
    seed_v1_progress();
    gal_progress_t plegacy;
    CHECK(gal_progress_load(&plegacy), "v1 progress load");
    CHECK(plegacy.valid && plegacy.event_index == 11 && plegacy.page_index == 3 &&
              plegacy.flags == 0, "v1 progress converted");

    // Permanent chapter/CG unlock stays global and independent from run flags.
    gal_unlock_t unlock;
    gal_unlock_clear(&unlock);
    CHECK(!gal_unlock_has_any_ending(&unlock), "no ending before grant");
    gal_unlock_grant_chapter(&unlock, 2);
    gal_unlock_grant_cg(&unlock, 4);
    gal_unlock_grant_ending(&unlock, 0);
    CHECK(gal_unlock_has_any_ending(&unlock), "ending grants CLEARED qualification");
    CHECK(gal_unlock_store(&unlock), "unlock store");
    gal_unlock_t uload;
    CHECK(gal_unlock_load(&uload), "unlock load");
    CHECK(gal_unlock_has_chapter(&uload, 2) && gal_unlock_has_cg(&uload, 4),
          "permanent unlock retained");

    // Cross-playthrough simulation: A then B do not leak into each other.
    uint32_t current = loaded.slots[1].flags;
    CHECK(gal_flags_has(current, 0) && !gal_flags_has(current, 1),
          "slot A flags only");
    current = pload.flags;
    CHECK(gal_flags_has(current, 1) && !gal_flags_has(current, 0),
          "progress B flags only");
    CHECK(gal_unlock_has_chapter(&uload, 2), "permanent unlock survives run flags");

    printf("%s: failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
