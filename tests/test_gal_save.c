#include "gal_expr.h"
#include "gal_pack.h"
#include "gal_save.h"
#include "gal_store.h"
#include "gal_text.h"
#include "gal_unlock.h"
#include "gal_vm.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) { fprintf(stderr, "pack %s\n", err); return 1; }

    static gal_vm_t vm, tmp;
    gal_vm_init(&vm, &pack);
    gal_vm_init(&tmp, &pack);
    CHECK(gal_vm_start(&vm, (uint16_t)pack.meta[1], true, false, &err), "start %s", err);
    for (int i = 0; i < 40; ++i) {
        gal_frame_t f;
        const gal_frame_type_t t = gal_vm_read(&vm, &f);
        if (t == GAL_FRAME_TEXT) { gal_vm_advance(&vm); continue; }
        if (t == GAL_FRAME_CHOICE) break;
        if (t == GAL_FRAME_END || t == GAL_FRAME_ERROR) break;
    }
    gal_save_ui_t ui = { .scene_mode = 0, .page = 1, .choice_row = 2 };
    uint8_t blob[GAL_SAVE_MAX];
    size_t len = 0;
    CHECK(gal_save_encode(&vm, &ui, blob, sizeof(blob), &len), "encode");
    CHECK(len > 12 && len < GAL_SAVE_MAX, "len=%zu", len);
    gal_save_ui_t out_ui;
    CHECK(gal_save_decode(blob, len, &pack, &tmp, &out_ui, &err), "decode %s", err);
    CHECK(tmp.chapter == vm.chapter && tmp.cursor == vm.cursor && tmp.root == vm.root, "position");
    CHECK(out_ui.page == ui.page && out_ui.choice_row == ui.choice_row, "ui state");
    CHECK(tmp.vars[gal_expr_find(&pack, "currentHp")].type == GAL_VAL_NUM, "hp var");
    uint16_t max_order = 0;
    for (int i = 0; i < tmp.sprite_count; ++i) if (tmp.sprites[i].used && tmp.sprites[i].order > max_order) max_order = tmp.sprites[i].order;
    for (int i = 0; i < tmp.layer_count; ++i) if (tmp.layers[i].used && tmp.layers[i].order > max_order) max_order = tmp.layers[i].order;
    CHECK(tmp.serial >= max_order, "serial=%u max=%u", tmp.serial, max_order);

    // v2: mutable isDebugDirectJump and current scene bodies round-trip.
    {
        gal_vm_t direct;
        gal_vm_init(&direct, &pack);
        CHECK(gal_vm_start(&direct, (uint16_t)pack.meta[1], false, false, &err),
              "direct start %s", err ? err : "?");
        const uint16_t direct_id = gal_expr_find(&pack, "isDebugDirectJump");
        CHECK(direct_id != GAL_NONE, "direct var missing");
        if (direct_id != GAL_NONE) {
            direct.vars[direct_id].type = GAL_VAL_BOOL;
            direct.vars[direct_id].num = 0.0f;
        }
        direct.body_count = 2;
        for (int i = 0; i < 2; ++i) {
            direct.bodies[i].used = 1;
            direct.bodies[i].actor = 0;
            direct.bodies[i].sub = 0;
            direct.bodies[i].image = (uint16_t)(i + 1);
            direct.bodies[i].x = (int16_t)(40 + i * 20);
            direct.bodies[i].y = 50;
            direct.bodies[i].scale100 = 100;
            direct.bodies[i].order = (uint16_t)(10 + i);
            direct.bodies[i].visible = 1;
        }
        uint8_t db[GAL_SAVE_MAX];
        size_t dlen = 0;
        gal_save_ui_t dui = { .scene_mode = 1, .page = 0, .choice_row = 1 };
        CHECK(gal_save_encode(&direct, &dui, db, sizeof(db), &dlen), "direct encode");
        CHECK(db[12] == 2, "save version=%u", db[12]);
        gal_save_ui_t dui2;
        gal_vm_t dout;
        gal_vm_init(&dout, &pack);
        CHECK(gal_save_decode(db, dlen, &pack, &dout, &dui2, &err), "direct decode %s",
              err ? err : "?");
        CHECK(dout.vars[direct_id].type == GAL_VAL_BOOL && dout.vars[direct_id].num == 0.0f,
              "direct false lost: type=%u num=%f", dout.vars[direct_id].type,
              (double)dout.vars[direct_id].num);
        CHECK(dout.body_count == 2, "body_count=%u", dout.body_count);
        CHECK(dout.bodies[1].image == 2 && dout.bodies[1].x == 60 &&
              dout.bodies[1].order == 11, "body state");
        direct.vars[direct_id].num = 1.0f;
        CHECK(gal_save_encode(&direct, &dui, db, sizeof(db), &dlen), "direct true encode");
        CHECK(gal_save_decode(db, dlen, &pack, &dout, &dui2, &err), "direct true decode");
        CHECK(dout.vars[direct_id].num == 1.0f, "direct true lost");
    }

    // 存档最大尺寸:手册/立绘/返回栈/选项缓存都塞满
    gal_vm_t big = vm;
    memset(big.book, 0, sizeof(big.book));
    big.book_count = 0;
    for (int i = 0; i < 168 && i < GAL_MAX_BOOK; ++i) {
        big.book[i] = (uint8_t)(1 + (i % 13));
        big.book_order[big.book_count++] = (uint8_t)i;
    }
    big.sprite_count = 33;  // source max observed; keep worst real state in budget
    for (int i = 0; i < 33; ++i) {
        big.sprites[i].used = 1;
        big.sprites[i].actor = 0;
        big.sprites[i].sub = 0;
        big.sprites[i].image = (uint16_t)(i % 100);
        big.sprites[i].x = 50; big.sprites[i].y = 50; big.sprites[i].scale100 = 100;
        big.sprites[i].order = (uint16_t)i;
        big.sprites[i].visible = 1;
    }
    big.layer_count = GAL_MAX_LAYERS;
    for (int i = 0; i < GAL_MAX_LAYERS; ++i) {
        big.layers[i].used = 1; big.layers[i].actor = 0; big.layers[i].sub = 0;
        big.layers[i].image = (uint16_t)i; big.layers[i].order = (uint16_t)i; big.layers[i].visible = 1;
    }
    big.stack_count = GAL_MAX_STACK;
    for (int i = 0; i < GAL_MAX_STACK; ++i) { big.stack[i][0] = 0; big.stack[i][1] = 0; }
    big.pending_count = GAL_MAX_PENDING;
    for (int i = 0; i < GAL_MAX_PENDING; ++i) {
        big.pending[i].used = 1; big.pending[i].text_str = 0; big.pending[i].to = 0;
        big.pending[i].end = 1; big.pending[i].menu = (uint16_t)i;
    }
    big.active_valid = 1;
    big.active_chapter = 0; big.active_cursor = 0;
    big.active_count = GAL_MAX_ROWS;
    for (int i = 0; i < GAL_MAX_ROWS; ++i) {
        big.active_rows[i].kind = GAL_ROW_ITEM;
        big.active_rows[i].text_off = 0; big.active_rows[i].text_len = 1;
        big.active_rows[i].to = 0; big.active_rows[i].end = 1;
    }
    for (uint16_t i = 0; i < big.var_count; ++i) {
        big.vars[i].type = GAL_VAL_NUM;
        big.vars[i].num = (float)(i + 1);
    }
    size_t big_len = 0;
    CHECK(gal_save_encode(&big, &ui, blob, sizeof(blob), &big_len), "big encode");
    CHECK(big_len <= GAL_SAVE_MAX, "big len=%zu", big_len);
    CHECK(gal_save_decode(blob, big_len, &pack, &tmp, &out_ui, &err), "big decode %s", err);
    CHECK(tmp.book_count == 168 && tmp.sprite_count == 33, "big counts");

    // 损坏:CRC、版本、长度、截断;失败不改目标 VM
    const uint8_t good_chapter = tmp.chapter;
    uint8_t bad[GAL_SAVE_MAX];
    memcpy(bad, blob, big_len);
    bad[big_len / 2] ^= 0xFF;
    CHECK(!gal_save_decode(bad, big_len, &pack, &tmp, &out_ui, &err), "CRC corruption accepted");
    CHECK(tmp.chapter == good_chapter, "failed decode mutated VM");
    memcpy(bad, blob, big_len);
    bad[12] = 99;
    CHECK(!gal_save_decode(bad, big_len, &pack, &tmp, &out_ui, &err), "version accepted");
    memcpy(bad, blob, big_len);
    CHECK(!gal_save_decode(bad, big_len - 1, &pack, &tmp, &out_ui, &err), "truncation accepted");
    memcpy(bad, blob, big_len);
    bad[6] ^= 1;
    CHECK(!gal_save_decode(bad, big_len, &pack, &tmp, &out_ui, &err), "length accepted");

    // 全局解锁:版本/CRC/合并/只增不减
    gal_unlock_t a, b;
    gal_unlock_init(&a);
    uint8_t bits[GAL_UNLOCK_GALLERY_BYTES] = { 0 };
    uint8_t chbits[GAL_UNLOCK_CHAPTER_BYTES] = { 0 };
    bits[0] = 0x01; chbits[0] = 0x80;
    CHECK(gal_unlock_merge(&a, bits, chbits), "merge");
    CHECK(a.dirty && gal_unlock_gallery(&a, 0) && gal_unlock_chapter(&a, 7), "bits");
    uint8_t ub[GAL_UNLOCK_BLOB];
    CHECK(gal_unlock_encode(&a, ub, sizeof(ub)) == GAL_UNLOCK_BLOB, "unlock encode");
    gal_unlock_init(&b);
    CHECK(gal_unlock_decode(&b, ub, sizeof(ub)), "unlock decode");
    CHECK(!b.dirty && gal_unlock_gallery(&b, 0) && gal_unlock_chapter(&b, 7), "unlock decode bits");
    ub[20] ^= 0xFF;
    CHECK(!gal_unlock_decode(&b, ub, sizeof(ub)), "unlock CRC accepted");
    CHECK(!gal_unlock_decode(&b, ub, 40), "unlock length accepted");

    // NVS:设置写入 + commit 失败不改旧档
    nvs_stub_reset();
    gal_store_t store;
    CHECK(gal_store_init(&store), "store init");
    gal_settings_t set;
    gal_settings_defaults(&set);
    CHECK(gal_store_save_settings(&store, &set), "settings save");
    CHECK(gal_store_load_settings(&store, &set), "settings load");
    CHECK(gal_store_save_slot(&store, 0, blob, 64), "slot save");
    const size_t before = nvs_stub_key_size("s00");
    nvs_stub_fail_set(true);
    CHECK(!gal_store_save_slot(&store, 0, blob, 128), "failed save reported success");
    CHECK((int)nvs_stub_key_size("s00") == (int)before, "failed save replaced slot");
    nvs_stub_fail_set(false);
    nvs_stub_fail_commit(true);
    CHECK(!gal_store_save_slot(&store, 0, blob, 128), "failed commit reported success");
    nvs_stub_fail_commit(false);
    uint16_t prog = 0;
    CHECK(gal_store_save_progress(&store, 7), "progress save");
    CHECK((int)nvs_stub_key_size("prog") == 2, "progress key size=%d", (int)nvs_stub_key_size("prog"));
    CHECK(gal_store_load_progress(&store, &prog), "progress load");
    CHECK(prog == 7, "progress value=%u", prog);

    // 预览
    char preview[64];
    CHECK(gal_save_preview(&pack, blob, big_len, &tmp, preview, sizeof(preview)), "preview");
    CHECK(gal_text_page_count("测试", 304, 16, 3) == 1, "text page count");

    printf("test_gal_save: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
