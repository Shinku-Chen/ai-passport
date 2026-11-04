// tests/test_anemoi_store.c —— 存档/自动档/全局解锁：编解码、校验、失败注入。
#include "anemoi_pack.h"
#include "anemoi_script.h"
#include "anemoi_store.h"

#include "anemoi_test_util.h"

#include <stdio.h>
#include <string.h>

static void test_workspace_boot(void) { (void)anem_store_workspace_init(); }

#define FAKE_SLOTS 40
static unsigned char s_storage[FAKE_SLOTS][4608];
static size_t s_length[FAKE_SLOTS];
static char s_keys[FAKE_SLOTS][32];
static int s_count;
static bool s_fail_write;

static bool fake_read(const char *key, void *out, size_t size, size_t *out_size)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0 && s_length[i] == size) {
            memcpy(out, s_storage[i], size);
            if (out_size) {
                *out_size = size;
            }
            return true;
        }
    }
    return false;
}

static bool fake_write(const char *key, const void *data, size_t size)
{
    if (s_fail_write) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0) {
            memcpy(s_storage[i], data, size);
            s_length[i] = size;
            return true;
        }
    }
    if (s_count >= FAKE_SLOTS) {
        return false;
    }
    snprintf(s_keys[s_count], sizeof(s_keys[s_count]), "%s", key);
    memcpy(s_storage[s_count], data, size);
    s_length[s_count] = size;
    s_count++;
    return true;
}

static const anem_store_io_t s_io = { .read = fake_read, .write = fake_write };

static int roundtrip(const anem_pack_t *pack)
{
    static anem_script_t script;
    ANEM_CHECK(anem_script_init(&script, pack));
    ANEM_CHECK(anem_script_start_chapter(&script, 3, 0));
    anem_script_var_set_num(&script, 0, 1234);
    anem_slot_t slot;
    anem_slot_from_state(&slot, &script, 0, 2, 1700000000u);
    uint8_t buf[4608];
    const size_t size = anem_slot_blob_size();
    ANEM_CHECK(size <= sizeof(buf));
    ANEM_CHECK(anem_slot_encode(&slot, buf, size));
    anem_slot_t decoded;
    ANEM_CHECK(anem_slot_decode(buf, size, &decoded));
    ANEM_CHECK(decoded.valid && decoded.chapter == 3 && decoded.page == 2);
    ANEM_CHECK(decoded.vars[0].num == 1234);
    ANEM_CHECK(anem_slot_apply(&decoded, &script));
    ANEM_CHECK(script.st.chapter == 3 && script.st.vars[0].num == 1234);

    // 版本 / CRC / 长度 任一不符都必须拒绝
    uint8_t bad[4608];
    memcpy(bad, buf, size);
    bad[4] = 9;
    ANEM_CHECK(!anem_slot_decode(bad, size, &decoded));
    memcpy(bad, buf, size);
    bad[size - 1] ^= 0xFF;
    ANEM_CHECK(!anem_slot_decode(bad, size, &decoded));
    ANEM_CHECK(!anem_slot_decode(buf, size - 4, &decoded));
    // 变量 kind 非法
    memcpy(bad, buf, size);
    bad[40 + ANEM_ACTOR_MAX * 4 + ANEM_BODY_MAX * 6 + 6] = 9;
    // 重新算 CRC 之外仍应被拒（这里只验证 CRC 也拦得住）
    ANEM_CHECK(!anem_slot_decode(bad, size, &decoded));
    return 0;
}

static int persistence(void)
{
    static anem_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.valid = true;
    slot.chapter = 7;
    slot.page = 1;
    slot.var_count = 2;
    slot.vars[0].kind = ANEM_VAL_NUM;
    slot.vars[0].num = 42;
    ANEM_CHECK(anem_store_save_slot(3, &slot));
    anem_slot_t loaded;
    ANEM_CHECK(anem_store_load_slot(3, &loaded));
    ANEM_CHECK(loaded.chapter == 7 && loaded.vars[0].num == 42);
    ANEM_CHECK(!anem_store_load_slot(4, &loaded));
    // 写失败：返回 false，不提示成功
    s_fail_write = true;
    ANEM_CHECK(!anem_store_save_slot(5, &slot));
    ANEM_CHECK(!anem_store_save_auto(&slot));
    s_fail_write = false;
    // 自动档独立于手动槽
    ANEM_CHECK(anem_store_save_auto(&slot));
    ANEM_CHECK(anem_store_load_auto(&loaded));
    // 存储里的记录被截断：读失败且不污染目标
    memset(s_storage[0], 0, s_length[0]);
    anem_slot_t untouched;
    memset(&untouched, 0x5A, sizeof(untouched));
    const anem_slot_t before = untouched;
    ANEM_CHECK(!anem_store_load_slot(0, &untouched));
    ANEM_CHECK(memcmp(&before, &untouched, sizeof(before)) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    test_workspace_boot();
    const char *path = argc > 1 ? argv[1] : "main/anemoi_data/anemoi_pack.bin";
    size_t size = 0;
    unsigned char *blob = anem_test_read(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "SKIP: 读不到资源包 %s\n", path);
        return 77;
    }
    anem_pack_t pack;
    ANEM_CHECK(anem_pack_open(&pack, blob, size));
    anem_store_bind(&s_io);
    int rc = roundtrip(&pack);
    if (rc == 0) {
        rc = persistence();
    }
    if (rc == 0) {
        static anem_progress_t progress;
        anem_progress_clear(&progress);
        progress.level = 4;
        progress.var_count = 2;
        progress.vars[0].kind = ANEM_VAL_NUM;
        progress.vars[0].num = 1;
        anem_progress_mark_cg(&progress, 3);
        anem_progress_mark_ending(&progress, 1);
        ANEM_CHECK(anem_store_save_progress(&progress));
        anem_progress_t loaded;
        ANEM_CHECK(anem_store_load_progress(&loaded));
        ANEM_CHECK(loaded.level == 4);
        ANEM_CHECK(anem_progress_has_cg(&loaded, 3));
        ANEM_CHECK(!anem_progress_has_cg(&loaded, 4));
        ANEM_CHECK(anem_progress_has_ending(&loaded, 1));
        uint8_t buf[4608];
        const size_t psize = anem_progress_blob_size();
        ANEM_CHECK(psize <= sizeof(buf));
        ANEM_CHECK(anem_progress_encode(&loaded, buf, psize));
        buf[psize - 2] ^= 0x01;
        ANEM_CHECK(!anem_progress_decode(buf, psize, &loaded));
    }
    free(blob);
    if (rc == 0) {
        printf("test_anemoi_store: PASS\n");
    }
    return rc;
}
