// tests/test_gal_state.c —— 存档 / 进度 / 永久解锁的格式与兼容回归。
//
// 覆盖：周目标记与永久解锁分离；run_mask 随存档与自动进度保存；v1 / v2 旧记录
// 仍可读（新字段按 0 处理）；CG 位图 64 位，索引 32..34 可授予、持久化并读回。
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
    uint8_t occupied;
    uint16_t event_index;
    uint16_t page_index;
    char chapter[GAL_SAVE_CHAPTER_LEN];
    uint8_t reserved;
    uint32_t flags;
} slot_v2_t;
typedef struct {
    uint8_t magic, version, reserved[2];
    slot_v2_t slots[GAL_SAVE_SLOTS];
} save_v2_t;
typedef struct {
    uint8_t valid;
    uint16_t event_index;
    uint16_t page_index;
} progress_v1_t;
typedef struct {
    uint8_t magic, version, reserved[2];
    progress_v1_t progress;
} progress_blob_v1_t;
typedef struct {
    uint8_t magic, version, reserved[2];
    uint32_t cg, chapter, flag, ending;
} unlock_v1_t;

// v3 存档（有 run_mask、没有 preview）：迁移后 preview 必须保守置 1。
typedef struct {
    uint8_t magic, version, reserved[2];
    uint8_t occupied[GAL_SAVE_SLOTS];
    uint8_t run_mask[GAL_SAVE_SLOTS];
    uint16_t event_index[GAL_SAVE_SLOTS];
    uint16_t page_index[GAL_SAVE_SLOTS];
    uint32_t flags[GAL_SAVE_SLOTS];
    char chapter[GAL_SAVE_SLOTS][GAL_SAVE_CHAPTER_LEN];
} save_v3_t;

// v3 进度（有 run_mask、没有 preview）。
typedef struct {
    uint8_t magic, version, reserved[2];
    uint8_t valid, run_mask;
    uint16_t event_index, page_index, reserved2;
    uint32_t flags;
} progress_v3_t;

_Static_assert(sizeof(slot_v1_t) == 36, "v1 save slot layout");
_Static_assert(sizeof(save_v1_t) == 112, "v1 save blob layout");
_Static_assert(sizeof(progress_v1_t) == 6, "v1 progress layout");
_Static_assert(sizeof(progress_blob_v1_t) == 10, "v1 progress blob layout");
_Static_assert(sizeof(unlock_v1_t) == 20, "v1 unlock blob layout");

static void seed_blob(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    CHECK(nvs_open(ns, NVS_READWRITE, &h) == 0, "open namespace");
    CHECK(nvs_set_blob(h, key, data, len) == 0, "seed blob");
    CHECK(nvs_commit(h) == 0, "commit blob");
}

static void seed_v1_save(void)
{
    save_v1_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x53;
    old.version = 1;
    old.slots[0].occupied = 1;
    old.slots[0].event_index = 7;
    old.slots[0].page_index = 2;
    snprintf(old.slots[0].chapter, sizeof(old.slots[0].chapter), "%s", "legacy");
    seed_blob("galsave", "slots", &old, sizeof(old));
}

static void seed_v2_save(void)
{
    save_v2_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x53;
    old.version = 2;
    old.slots[1].occupied = 1;
    old.slots[1].event_index = 9;
    old.slots[1].page_index = 1;
    old.slots[1].flags = 0x5u;
    snprintf(old.slots[1].chapter, sizeof(old.slots[1].chapter), "%s", "v2 route");
    seed_blob("galsave", "slots", &old, sizeof(old));
}

static void seed_v1_progress(void)
{
    progress_blob_v1_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x50;
    old.version = 1;
    old.progress.valid = 1;
    old.progress.event_index = 11;
    old.progress.page_index = 3;
    seed_blob("galprog", "pos", &old, sizeof(old));
}

static void seed_v3_save(void)
{
    save_v3_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x53;
    old.version = 3;
    old.occupied[2] = 1;
    old.run_mask[2] = 0xFu;
    old.event_index[2] = 33;
    old.page_index[2] = 0;
    old.flags[2] = 0x2u;
    snprintf(old.chapter[2], GAL_SAVE_CHAPTER_LEN, "%s", "v3 route");
    seed_blob("galsave", "slots", &old, sizeof(old));
}

static void seed_v3_progress(void)
{
    progress_v3_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x50;
    old.version = 3;
    old.valid = 1;
    old.run_mask = 0xFu;
    old.event_index = 44;
    old.page_index = 1;
    old.flags = 0x3u;
    seed_blob("galprog", "pos", &old, sizeof(old));
}

static void seed_v1_unlock(void)
{
    unlock_v1_t old;
    memset(&old, 0, sizeof(old));
    old.magic = 0x4C;
    old.version = 1;
    old.cg = (1u << 2) | (1u << 31);
    old.chapter = 1u << 1;
    old.ending = 1u;
    seed_blob("galunlock", "bits", &old, sizeof(old));
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

    // v3 save: flags + run_mask travel with the slot.
    gal_save_t save;
    gal_save_clear(&save);
    save.slots[1].occupied = 1;
    save.slots[1].event_index = 21;
    save.slots[1].page_index = 4;
    save.slots[1].flags = FLAG_A;
    save.slots[1].run_mask = 0x5u;
    save.slots[1].preview = 1;
    snprintf(save.slots[1].chapter, sizeof(save.slots[1].chapter), "%s", "route A");
    CHECK(gal_save_store(&save), "v3 save store");
    gal_save_t loaded;
    CHECK(gal_save_load(&loaded), "v3 save load");
    CHECK(loaded.slots[1].occupied && loaded.slots[1].event_index == 21 &&
              loaded.slots[1].page_index == 4, "v3 save fields");
    CHECK(loaded.slots[1].flags == FLAG_A, "v3 save flags");
    CHECK(loaded.slots[1].run_mask == 0x5u, "v4 save run mask");
    CHECK(loaded.slots[1].preview == 1, "v4 save preview flag");

    // v2 save: flags kept, run_mask defaults to zero.
    seed_v2_save();
    gal_save_t v2;
    CHECK(gal_save_load(&v2), "v2 save load");
    CHECK(v2.slots[1].occupied && v2.slots[1].event_index == 9 && v2.slots[1].flags == 0x5u,
          "v2 save converted");
    CHECK(v2.slots[1].run_mask == 0, "v2 save has no run mask");
    CHECK(v2.slots[1].preview == 1, "v2 save migrates as a preview (no free clear)");

    // v3 save: run_mask kept, preview conservatively set.
    seed_v3_save();
    gal_save_t v3;
    CHECK(gal_save_load(&v3), "v3 save load");
    CHECK(v3.slots[2].occupied && v3.slots[2].event_index == 33 && v3.slots[2].run_mask == 0xFu,
          "v3 save converted");
    CHECK(v3.slots[2].preview == 1, "v3 save migrates as a preview");

    // v1 save is accepted with flags/run_mask reset.
    seed_v1_save();
    gal_save_t legacy;
    CHECK(gal_save_load(&legacy), "v1 save load");
    CHECK(legacy.slots[0].occupied && legacy.slots[0].event_index == 7 &&
              strcmp(legacy.slots[0].chapter, "legacy") == 0 &&
              legacy.slots[0].flags == 0 && legacy.slots[0].run_mask == 0 &&
              legacy.slots[0].preview == 1,
          "v1 save converted");

    // v3 progress: flags + run_mask.
    gal_progress_t progress;
    gal_progress_clear(&progress);
    progress.valid = 1;
    progress.event_index = 31;
    progress.page_index = 1;
    progress.flags = FLAG_B;
    progress.run_mask = 0xFu;
    progress.preview = 1;
    CHECK(gal_progress_store(&progress), "v3 progress store");
    gal_progress_t pload;
    CHECK(gal_progress_load(&pload), "v3 progress load");
    CHECK(pload.valid && pload.event_index == 31 && pload.page_index == 1 &&
              pload.flags == FLAG_B && pload.run_mask == 0xFu && pload.preview == 1,
          "v4 progress fields");

    // v3 progress: run_mask kept, preview conservatively set.
    seed_v3_progress();
    gal_progress_t pv3;
    CHECK(gal_progress_load(&pv3), "v3 progress load");
    CHECK(pv3.valid && pv3.event_index == 44 && pv3.run_mask == 0xFu && pv3.preview == 1,
          "v3 progress migrates as a preview");

    // v1 progress: accepted, new fields zero.
    seed_v1_progress();
    gal_progress_t plegacy;
    CHECK(gal_progress_load(&plegacy), "v1 progress load");
    CHECK(plegacy.valid && plegacy.event_index == 11 && plegacy.page_index == 3 &&
              plegacy.flags == 0 && plegacy.run_mask == 0 && plegacy.preview == 1,
          "v1 progress converted");

    // 64-bit CG bitmap: indices beyond 31 must work and persist.
    gal_unlock_t unlock;
    gal_unlock_clear(&unlock);
    CHECK(!gal_unlock_has_any_ending(&unlock), "no ending before grant");
    gal_unlock_grant_chapter(&unlock, 2);
    gal_unlock_grant_cg(&unlock, 4);
    gal_unlock_grant_cg(&unlock, 32);
    gal_unlock_grant_cg(&unlock, 34);
    CHECK(!gal_unlock_has_cg(&unlock, 33), "CG 33 not granted yet");
    gal_unlock_grant_ending(&unlock, 0);
    CHECK(gal_unlock_has_any_ending(&unlock), "ending grants CLEARED qualification");
    CHECK(gal_unlock_store(&unlock), "unlock store");
    gal_unlock_t uload;
    CHECK(gal_unlock_load(&uload), "unlock load");
    CHECK(gal_unlock_has_chapter(&uload, 2) && gal_unlock_has_cg(&uload, 4),
          "permanent unlock retained");
    CHECK(gal_unlock_has_cg(&uload, 32) && gal_unlock_has_cg(&uload, 34),
          "CG indices above 31 persist");
    CHECK(!gal_unlock_has_cg(&uload, 33), "unset high CG stays unset");

    // v1 unlock blob (32-bit CG) migrates.
    seed_v1_unlock();
    gal_unlock_t old_unlock;
    CHECK(gal_unlock_load(&old_unlock), "v1 unlock load");
    CHECK(gal_unlock_has_cg(&old_unlock, 2) && gal_unlock_has_cg(&old_unlock, 31),
          "v1 unlock CG bits migrate");
    CHECK(gal_unlock_has_ending(&old_unlock, 0), "v1 unlock ending migrates");
    CHECK(!gal_unlock_has_cg(&old_unlock, 32), "v1 unlock has no high CG bits");

    // Cross-playthrough simulation: A then B do not leak into each other.
    uint32_t current = loaded.slots[1].flags;
    CHECK(gal_flags_has(current, 0) && !gal_flags_has(current, 1), "slot A flags only");
    current = pload.flags;
    CHECK(gal_flags_has(current, 1) && !gal_flags_has(current, 0), "progress B flags only");
    CHECK(gal_unlock_has_chapter(&uload, 2), "permanent unlock survives run flags");

    printf("%s: failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
