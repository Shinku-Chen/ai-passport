/*
 * tests/test_eden_store.c —— 存档/进度/解锁持久化的宿主机测试。
 *
 * eden_store.c 里没有 NVS 调用:读写走 eden_store_io_t。本测试绑定一个假适配器,
 * 覆盖生产逻辑本身(编解码 + 校验 + 位操作),并注入"提交失败""记录损坏"这类
 * 真机上难复现的路径:
 *   - 写失败必须冒泡到调用方(不能显示"已保存");
 *   - 读失败/损坏/版本不符必须返回 false 且**不改动**调用方结构体;
 *   - 页码/场景/字符串边界越界的记录整体拒绝。
 *
 * 构建:cc -std=c11 -Wall -Wextra -Werror -Imain \
 *         tests/test_eden_store.c main/eden_store.c main/eden_script.c
 */
#include "eden_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------- 假适配器
#define FAKE_SLOTS 512
#define FAKE_BLOBS 512

typedef struct {
    char ns[16];
    char key[16];
    uint8_t data[FAKE_BLOBS];
    size_t size;
    bool present;
} fake_entry_t;

static fake_entry_t s_entries[8];
static int s_entry_count;
static bool s_write_fails;
static int s_write_calls;
static int s_write_success;

static fake_entry_t *find(const char *ns, const char *key, bool create)
{
    for (int index = 0; index < s_entry_count; index++) {
        if (strcmp(s_entries[index].ns, ns) == 0 && strcmp(s_entries[index].key, key) == 0) {
            return &s_entries[index];
        }
    }
    if (!create || s_entry_count >= 8) {
        return NULL;
    }
    fake_entry_t *entry = &s_entries[s_entry_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ns, sizeof(entry->ns), "%s", ns);
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    return entry;
}

static bool fake_read(const char *ns, const char *key, void *out, size_t size)
{
    fake_entry_t *entry = find(ns, key, false);
    if (entry == NULL || !entry->present || entry->size != size) {
        return false;
    }
    memcpy(out, entry->data, size);
    return true;
}

static bool fake_write(const char *ns, const char *key, const void *data, size_t size)
{
    s_write_calls++;
    if (s_write_fails || size > FAKE_BLOBS) {
        return false;   // 模拟 nvs_commit 失败:上层必须看到 false
    }
    fake_entry_t *entry = find(ns, key, true);
    if (entry == NULL) {
        return false;
    }
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->present = true;
    s_write_success++;
    return true;
}

static const eden_store_io_t fake_io = {
    .read = fake_read,
    .write = fake_write,
};

// 生产代码默认指向 NVS 适配器(eden_store_nvs.c)。宿主机测试用同一个符号名提供假实现,
// 这样 eden_store.c 一行都不用改,也不需要 #ifdef。
const eden_store_io_t eden_store_nvs_io = {
    .read = fake_read,
    .write = fake_write,
};

static void reset_fake(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_entry_count = 0;
    s_write_fails = false;
    s_write_calls = 0;
    s_write_success = 0;
    eden_store_bind(&fake_io);
}

// ---------------------------------------------------------------- 用例
static void make_slot(eden_slot_t *slot, int index)
{
    eden_slot_clear(slot);
    slot->valid = 1;
    slot->scene = (uint16_t)(index + 1);
    slot->op = (uint32_t)(100 + index);
    slot->page = (uint16_t)index;
    slot->chapter = (uint16_t)index;
    slot->vars[0] = index * 7;
    snprintf(slot->chapter_text, sizeof(slot->chapter_text), "第 %d 章", index + 1);
    snprintf(slot->preview, sizeof(slot->preview), "预览 %d", index + 1);
}

static void test_roundtrip(void)
{
    reset_fake();
    eden_slot_t slots[EDEN_SAVE_SLOTS];
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        make_slot(&slots[index], index);
    }
    slots[2].valid = 0;   // 空槽也要能存能读

    CHECK(eden_store_save_slots(slots), "正常写入应成功");
    CHECK(s_write_success == 1, "写入次数 %d", s_write_success);

    eden_slot_t loaded[EDEN_SAVE_SLOTS];
    memset(loaded, 0, sizeof(loaded));
    CHECK(eden_store_load_slots(loaded), "正常读取应成功");
    CHECK(loaded[0].scene == 1 && loaded[1].scene == 2, "槽位内容不对");
    CHECK(loaded[1].vars[0] == 7, "变量没存进去");
    CHECK(strcmp(loaded[1].chapter_text, "第 2 章") == 0, "章节名不对");
    CHECK(loaded[2].valid == 0, "空槽应保持无效");

    eden_progress_t progress = { 1, 5, 42, 1, { 3, 0, 0, 0, 0, 0, 0, 0 } };
    CHECK(eden_store_save_progress(&progress), "进度写入应成功");
    eden_progress_t loaded_progress;
    memset(&loaded_progress, 0, sizeof(loaded_progress));
    CHECK(eden_store_load_progress(&loaded_progress), "进度读取应成功");
    CHECK(loaded_progress.scene == 5 && loaded_progress.op == 42, "进度内容不对");

    eden_unlock_t unlock;
    eden_unlock_clear(&unlock);
    eden_unlock_grant_cg(&unlock, 294);
    eden_unlock_grant_chapter(&unlock, 20);
    eden_unlock_grant_ending(&unlock, 0);
    CHECK(eden_store_save_unlock(&unlock), "解锁写入应成功");
    eden_unlock_t loaded_unlock;
    eden_unlock_clear(&loaded_unlock);
    CHECK(eden_store_load_unlock(&loaded_unlock), "解锁读取应成功");
    CHECK(eden_unlock_has_cg(&loaded_unlock, 294), "CG 组 294 应已解锁");
    CHECK(!eden_unlock_has_cg(&loaded_unlock, 293), "CG 组 293 不应解锁");
    CHECK(eden_unlock_has_chapter(&loaded_unlock, 20), "章节 20 应已解锁");
    CHECK(eden_unlock_has_ending(&loaded_unlock, 0), "结局应已解锁");
}

// 提交失败必须冒泡:调用方据此提示"保存失败",不能显示成功。
static void test_write_failure(void)
{
    reset_fake();
    s_write_fails = true;
    eden_slot_t slots[EDEN_SAVE_SLOTS];
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        make_slot(&slots[index], index);
    }
    CHECK(!eden_store_save_slots(slots), "写入失败必须返回 false");
    CHECK(s_write_calls == 1 && s_write_success == 0, "失败写入不应被计入成功");

    eden_progress_t progress = { 1, 2, 3, 0, { 0 } };
    CHECK(!eden_store_save_progress(&progress), "进度写入失败必须返回 false");
    eden_unlock_t unlock;
    eden_unlock_clear(&unlock);
    CHECK(!eden_store_save_unlock(&unlock), "解锁写入失败必须返回 false");

    // 读不到时也不能改动调用方结构体。
    eden_slot_t untouched[EDEN_SAVE_SLOTS];
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        make_slot(&untouched[index], index + 5);
    }
    eden_slot_t before[EDEN_SAVE_SLOTS];
    memcpy(before, untouched, sizeof(before));
    CHECK(!eden_store_load_slots(untouched), "没有记录时读取应失败");
    CHECK(memcmp(before, untouched, sizeof(before)) == 0, "读失败不得改动调用方数据");
}

static void test_decode_rejects(void)
{
    eden_slot_t slots[EDEN_SAVE_SLOTS];
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        make_slot(&slots[index], index);
    }
    const size_t size = eden_store_slots_blob_size();
    uint8_t *blob = (uint8_t *)malloc(size);
    CHECK(blob != NULL, "分配失败");
    if (blob == NULL) {
        return;
    }
    CHECK(eden_store_encode_slots(slots, blob, size), "编码应成功");

    eden_slot_t out[EDEN_SAVE_SLOTS];
    CHECK(eden_store_decode_slots(blob, size, out), "正常记录应解码成功");

    // 版本不符
    blob[0] = (uint8_t)(blob[0] + 1);
    CHECK(!eden_store_decode_slots(blob, size, out), "版本不符应拒绝");
    blob[0] = (uint8_t)(blob[0] - 1);

    // 长度不符(截断)
    CHECK(!eden_store_decode_slots(blob, size - 1, out), "截断记录应拒绝");

    // 页码越界:直接改结构体再编码,模拟旧固件写坏的数据
    eden_slot_t bad[EDEN_SAVE_SLOTS];
    memcpy(bad, slots, sizeof(bad));
    bad[1].page = EDEN_SLOT_PAGE_MAX;
    CHECK(eden_store_encode_slots(bad, blob, size), "编码应成功");
    CHECK(!eden_store_decode_slots(blob, size, out), "页码越界应拒绝");

    // scene = 0
    memcpy(bad, slots, sizeof(bad));
    bad[0].scene = 0;
    CHECK(eden_store_encode_slots(bad, blob, size), "编码应成功");
    CHECK(!eden_store_decode_slots(blob, size, out), "scene=0 应拒绝");

    // 字符串没有 NUL 结尾
    memcpy(bad, slots, sizeof(bad));
    memset(bad[0].preview, 'x', sizeof(bad[0].preview));
    CHECK(eden_store_encode_slots(bad, blob, size), "编码应成功");
    CHECK(!eden_store_decode_slots(blob, size, out), "字符串无 NUL 应拒绝");

    // valid 只能是 0/1
    memcpy(bad, slots, sizeof(bad));
    bad[2].valid = 2;
    CHECK(eden_store_encode_slots(bad, blob, size), "编码应成功");
    CHECK(!eden_store_decode_slots(blob, size, out), "valid=2 应拒绝");

    free(blob);
}

static void test_progress_and_unlock_bounds(void)
{
    eden_progress_t progress = { 1, 0, 10, 0, { 0 } };
    const size_t size = eden_store_progress_blob_size();
    uint8_t blob[256];
    CHECK(eden_store_encode_progress(&progress, blob, sizeof(blob)), "编码应成功");
    eden_progress_t out;
    CHECK(!eden_store_decode_progress(blob, size, &out), "scene=0 的进度应拒绝");

    progress.scene = 3;
    progress.valid = 2;
    CHECK(eden_store_encode_progress(&progress, blob, sizeof(blob)), "编码应成功");
    CHECK(!eden_store_decode_progress(blob, size, &out), "valid=2 的进度应拒绝");

    // CG 组超出 320 时静默忽略(不越界写)
    eden_unlock_t unlock;
    eden_unlock_clear(&unlock);
    eden_unlock_grant_cg(&unlock, EDEN_CG_GROUPS);
    for (size_t index = 0; index < sizeof(unlock.cg); index++) {
        CHECK(unlock.cg[index] == 0, "越界组号不得写入第 %u 字节", (unsigned)index);
    }
    CHECK(!eden_unlock_has_cg(&unlock, EDEN_CG_GROUPS), "越界组号查询应为 false");
    eden_unlock_grant_chapter(&unlock, 31);
    CHECK(eden_unlock_has_chapter(&unlock, 31), "章节 31 应可置位");
    eden_unlock_grant_chapter(&unlock, 32);
    CHECK(!eden_unlock_has_chapter(&unlock, 32), "章节 32 越界不得置位");
}

int main(void)
{
    test_roundtrip();
    test_write_failure();
    test_decode_rejects();
    test_progress_and_unlock_bounds();
    eden_store_bind(NULL);   // 恢复默认适配器指针(固件里是 NVS,宿主机上不会被调用)
    if (failures != 0) {
        fprintf(stderr, "%d 项检查失败\n", failures);
        return 1;
    }
    printf("eden store: OK\n");
    return 0;
}
