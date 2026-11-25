// NVS store contracts: validated records, 30 slots, corruption is reported but
// never erased, and a write failure is visible to the caller.
#include "nine_store.h"
#include "nine_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *size = (size_t)n; return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s script.bin\n", argv[0]); return 2; }
    size_t sn;
    uint8_t *bytes = load(argv[1], &sn);
    static nine_pack_t pack;
    static nine_vm_t vm;
    CHECK(nine_pack_open(&pack, bytes, sn), "pack");
    CHECK(nine_vm_init(&vm, &pack), "vm");

    nvs_stub_reset();
    static nine_store_t store;
    CHECK(nine_store_init(&store, true), "store open");
    nine_cfg_t cfg;
    nine_cfg_defaults(&cfg);
    CHECK(cfg.text_speed == 1 && cfg.transparency == 30 && cfg.sound == 1, "defaults");
    CHECK(nine_store_cfg_save(&store, &cfg), "cfg save");
    nine_cfg_t got;
    memset(&got, 0, sizeof(got));
    CHECK(nine_store_cfg_load(&store, &got), "cfg load");
    CHECK(got.text_speed == cfg.text_speed && got.font_large == cfg.font_large &&
          got.auto_play == cfg.auto_play && got.transparency == cfg.transparency &&
          got.sound == cfg.sound && got.title_bg == cfg.title_bg, "cfg roundtrip");
    cfg.font_large = 1; cfg.title_bg = 2; cfg.transparency = 55;
    CHECK(nine_store_cfg_save(&store, &cfg), "cfg save 2");
    CHECK(nine_store_cfg_load(&store, &got) && got.font_large == 1 && got.title_bg == 2 &&
          got.transparency == 55, "cfg values");

    uint8_t unlock[NINE_UNLOCK_BYTES];
    memset(unlock, 0, sizeof(unlock));
    unlock[0] = 0x5a; unlock[8] = 0x01;
    CHECK(nine_store_unlock_save(&store, unlock), "unlock save");
    uint8_t back[NINE_UNLOCK_BYTES];
    CHECK(nine_store_unlock_load(&store, back) && memcmp(back, unlock, sizeof(back)) == 0,
          "unlock roundtrip");

    int loop = nine_pack_find_key(&pack, "loop_my");
    CHECK(loop >= 0, "loop key");
    vm.state.progress[loop] = 1;
    vm.state.progress_set[loop / 8] |= (uint8_t)(1u << (loop % 8));
    uint8_t progress[NINE_PROGRESS_RECORD_MAX];
    size_t pn = nine_vm_progress_encode(&vm, progress, sizeof(progress));
    CHECK(pn > 0 && nine_store_progress_save(&store, progress, pn), "progress save");
    size_t rn = 0;
    uint8_t readback[NINE_PROGRESS_RECORD_MAX];
    CHECK(nine_store_progress_load(&store, readback, sizeof(readback), &rn) && rn == pn &&
          memcmp(readback, progress, pn) == 0, "progress load");

    nine_vm_start(&vm, NINE_NODE_NONE, false);
    for (int i = 0; i < 12; ++i) nine_vm_advance(&vm);
    uint8_t auto_blob[NINE_SAVE_RECORD_MAX];
    size_t an = nine_vm_save_encode(&vm, 0, auto_blob, sizeof(auto_blob));
    CHECK(an >= 40 && nine_store_auto_save(&store, auto_blob, an), "auto save");
    CHECK(nine_store_auto_load(&store, readback, sizeof(readback), &rn) && rn == an,
          "auto load");
    CHECK(nine_store_auto_clear(&store), "auto clear");
    CHECK(nine_store_auto_load(&store, readback, sizeof(readback), &rn) && rn == 1 &&
          !nine_vm_save_peek(&pack, readback, rn, NULL, NULL, NULL, NULL),
          "auto cleared record is not a save");

    /* 30 slots: meta scan and normal load/save. */
    for (int i = 0; i < NINE_SLOTS; ++i) {
        size_t z = nine_vm_save_encode(&vm, (uint16_t)i, auto_blob, sizeof(auto_blob));
        CHECK(nine_store_slot_save(&store, i, auto_blob, z), "slot save");
    }
    nine_slot_meta_t meta[NINE_SLOTS];
    nine_store_slot_scan(&store, &pack, meta);
    int occupied = 0;
    for (int i = 0; i < NINE_SLOTS; ++i) occupied += meta[i].occupied ? 1 : 0;
    CHECK(occupied == NINE_SLOTS, "30 slots occupied");
    CHECK(meta[3].page == 3 && meta[3].chapter != NINE_NODE_NONE, "slot metadata");

    /* Corrupt one slot: reported as occupied but the VM rejects it, and the
       store never erases it behind the reader's back. */
    uint8_t bad[NINE_SAVE_RECORD_MAX];
    size_t bn = 0;
    CHECK(nine_store_slot_load(&store, 7, bad, sizeof(bad), &bn), "slot load");
    bad[bn / 2] ^= 0xff;
    CHECK(nine_store_slot_save(&store, 7, bad, bn), "slot corrupt write");
    nine_store_slot_scan(&store, &pack, meta);
    CHECK(meta[7].occupied && meta[7].chapter == NINE_NODE_NONE, "corrupt slot reported");
    nine_store_slot_load(&store, 7, bad, sizeof(bad), &bn);
    CHECK(!nine_vm_save_decode(&vm, bad, bn, NULL), "corrupt slot rejected");

    /* Write failure is surfaced. */
    nvs_stub_fail_next_set();
    CHECK(!nine_store_slot_save(&store, 0, auto_blob, an), "write failure visible");

    /* A damaged cfg record is rejected without erasing anything. */
    uint8_t bad_cfg[16];
    memset(bad_cfg, 0xff, sizeof(bad_cfg));
    CHECK(nvs_set_blob(store.handle, "cfg", bad_cfg, sizeof(bad_cfg)) == ESP_OK,
          "write bad cfg");
    CHECK(!nine_store_cfg_load(&store, &got), "bad cfg rejected");
    CHECK(nvs_get_blob(store.handle, "cfg", bad_cfg, &(size_t){sizeof(bad_cfg)}) == ESP_OK,
          "bad cfg retained");

    printf("fails=%d\n", fails);
    free(bytes);
    return fails ? 1 : 0;
}
