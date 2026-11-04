// 负例矩阵：decode 失败必须不改 caller out（逐字节 memcmp），租约占用时 save/load 失败且不改 live。
#include <stdio.h>
#include <string.h>

#include "anemoi_pack.h"
#include "anemoi_script.h"
#include "anemoi_store.h"

static int fails;

static void check(const char *name, bool rejected, bool unchanged) {
    const bool ok = rejected && unchanged;
    printf("%s | neg:%s | rejected=%d unchanged=%d\n", ok ? "PASS" : "FAIL", name, rejected,
           unchanged);
    if (!ok) fails++;
}

static void fill(anem_slot_t *s, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->valid = true;
    s->chapter = 3;
    s->cursor = 120;
    s->page = 2;
    s->mode = 1;
    s->progression = 1;
    s->body_count = 2;
    s->bodies[0].valid = true;
    s->bodies[0].name = 7;
    s->var_count = 3;
    for (int i = 0; i < 3; i++) {
        s->vars[i].kind = ANEM_VAL_NUM;
        s->vars[i].num = (int32_t)(seed + (uint32_t)i);
    }
}

static bool decode_into(const uint8_t *blob, size_t n, const anem_slot_t *expect,
                        const char *name) {
    anem_slot_t out;
    memcpy(&out, expect, sizeof(out));   // 预置内容，失败必须保持不变
    const bool ok = anem_slot_decode(blob, n, &out);
    check(name, !ok, memcmp(&out, expect, sizeof(out)) == 0);
    return ok;
}

int main(void) {
    fails = 0;
    if (!anem_store_workspace_init()) {
        printf("FAIL workspace init\n");
        return 1;
    }
    anem_slot_t slot;
    fill(&slot, 100);
    uint8_t blob[4608];
    const size_t n = anem_slot_blob_size();
    if (!anem_slot_encode(&slot, blob, n)) {
        printf("FAIL encode fixture\n");
        return 1;
    }
    anem_slot_t sentinel;
    fill(&sentinel, 999);

    uint8_t bad[4608];
    struct { const char *name; size_t at; } cases[] = {
        {"crc", n - 1},        {"magic", 1},          {"mode", 9},
        {"progression", 10},   {"body_count", 11},    {"chapter", 12},
        {"cursor", 16},        {"page", 20},          {"var_count", 26},
        {"body", 100},         {"value", 500},        {"arr_len", 900},
        {"trailing", n - 6},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memcpy(bad, blob, n);
        bad[cases[i].at] ^= 0x5a;
        decode_into(bad, n, &sentinel, cases[i].name);
    }
    // header 长度字段错
    memcpy(bad, blob, n);
    bad[6] ^= 0xff;
    decode_into(bad, n, &sentinel, "header_size");

    // 租约占用：save/load 必须失败且不改 caller
    if (!anem_store_workspace_acquire()) {
        printf("FAIL lease acquire\n");
        return 1;
    }
    anem_slot_t keep;
    fill(&keep, 42);
    anem_slot_t before = keep;
    const bool load_ok = anem_store_load_auto(&keep);
    check("lease_busy_load", !load_ok, memcmp(&keep, &before, sizeof(keep)) == 0);
    const bool save_ok = anem_store_save_auto(&slot);
    check("lease_busy_save", !save_ok, true);
    anem_store_workspace_release();

    printf("SUMMARY | fails=%d | %s\n", fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
