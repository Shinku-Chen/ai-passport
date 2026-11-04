// tests/test_steins_checkpoint.c —— 手机/邮件 checkpoint 编解码与容量回归。
//
// 目标(见 Lead 对 545 B 假设的批评):
//   * 不是"字段相加",而是真实可变长编码:有序 id 列表 + 646 位集合位图 + varint;
//   * 源 oracle S2 的 inbox 115 条、以及满上限的两条列表都要能往返;
//   * 逐字段编解码相等(不是只比长度);
//   * 越界/截断/坏 CRC 一律拒绝,绝不静默截断状态;
//   * 按 IDF NVS 的 entry/blob-chunk/page 结构算 30 手动 + 1 自动能否装下。
#include "steins_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);           \
            fprintf(stderr, "\n");                  \
            ++g_failures;                           \
        }                                           \
    } while (0)

static uint8_t s_buf[STEINS_CHK_MAX_BYTES];
static uint8_t s_buf2[STEINS_CHK_MAX_BYTES];

static void fill_typical(steins_checkpoint_t *chk)
{
    steins_checkpoint_init(chk);
    chk->mode = STEINS_MODE_STORY;
    chk->node = 73;
    chk->cursor = 8123;
    chk->last_node = 73;
    chk->last_at = 8100;              // 源 S10:lastAt 必须保留,否则会重放来电
    for (int i = 0; i < 115; ++i) {   // 源 oracle S2:inbox 115 条
        CHECK(steins_checkpoint_push_inbox(chk, (uint16_t)(i * 5 + 3)), "push inbox %d", i);
    }
    for (int i = 0; i < 12; ++i) {
        CHECK(steins_checkpoint_push_sent(chk, (uint16_t)(600 + i)), "push sent %d", i);
    }
    for (uint16_t id = 0; id < STEINS_MAIL_IDS; id += 3) {
        steins_checkpoint_set_read(chk, id, true);
    }
    steins_checkpoint_set_replied(chk, 297, true);
    steins_checkpoint_set_replied(chk, 386, true);
    steins_checkpoint_set_unread(chk, 404, true);
    chk->owner_count = 3;
    chk->owner_id[0] = 297; chk->owner_val[0] = 1;
    chk->owner_id[1] = 386; chk->owner_val[1] = 4;
    chk->owner_id[2] = 405; chk->owner_val[2] = 2;
    chk->open_count = 2;
    chk->open_id[0] = 386; chk->open_val[0] = 1;
    chk->open_id[1] = 381; chk->open_val[1] = 3;
    chk->channels[0] = 999; chk->channels[1] = 405;
    chk->call_present = 1;
    chk->call_site = 15; chk->call_owner = 0; chk->call_kind = 1;
    chk->call_waiting = 40; chk->call_node = 21; chk->call_at = 583;
    chk->call_return_present = 1;
    chk->call_return_node = 21; chk->call_return_cursor = 600;
    chk->pres_count = 2;
    chk->pres_id[0] = 263; chk->pres_kind[0] = 2; chk->pres_owner[0] = 4;
    chk->pres_id[1] = 278; chk->pres_kind[1] = 1; chk->pres_owner[1] = 0;
    chk->deferred_present = 1;
    chk->deferred_root = 301; chk->deferred_id = 386; chk->deferred_read = 1;
    chk->keyphone_present = 1;
    chk->keyphone_root = 19; chk->keyphone_owner = 0;
    chk->endings = 0x0003;
}

static void fill_max(steins_checkpoint_t *chk)
{
    steins_checkpoint_init(chk);
    chk->mode = STEINS_MODE_CHAPTER;
    chk->node = 306;
    chk->cursor = 0xFFFFFF;      // 源 cursor 上限远小于此,这里压一下编码边界
    chk->last_node = 306;
    chk->last_at = 0xFFFFF;
    for (uint16_t i = 0; i < STEINS_CHK_MAX_INBOX; ++i) {
        CHECK(steins_checkpoint_push_inbox(chk, (uint16_t)(i % STEINS_MAIL_IDS)), "max inbox");
    }
    for (uint16_t i = 0; i < STEINS_CHK_MAX_SENT; ++i) {
        CHECK(steins_checkpoint_push_sent(chk, (uint16_t)((i * 7) % STEINS_MAIL_IDS)), "max sent");
    }
    memset(chk->read, 0xFF, sizeof(chk->read));
    memset(chk->replied, 0xFF, sizeof(chk->replied));
    memset(chk->unread, 0xFF, sizeof(chk->unread));
    chk->owner_count = STEINS_CHK_MAX_OWNERS;
    for (int i = 0; i < STEINS_CHK_MAX_OWNERS; ++i) {
        chk->owner_id[i] = (uint16_t)i;
        chk->owner_val[i] = (uint8_t)(i % 5);
    }
    chk->open_count = STEINS_CHK_MAX_OPEN;
    for (int i = 0; i < STEINS_CHK_MAX_OPEN; ++i) {
        chk->open_id[i] = (uint16_t)(100 + i);
        chk->open_val[i] = (uint8_t)(i % 5);
    }
    for (int i = 0; i < 5; ++i) chk->channels[i] = (uint8_t)(200 + i);
    chk->call_present = 1;
    chk->call_site = 0xFFFF; chk->call_owner = 255; chk->call_kind = 255;
    chk->call_waiting = 255; chk->call_node = 0xFFFF; chk->call_at = 0xFFFFFF;
    chk->call_return_present = 1;
    chk->call_return_node = 0xFFFF; chk->call_return_cursor = 0xFFFFFF;
    chk->pres_count = STEINS_CHK_MAX_PRES;
    for (int i = 0; i < STEINS_CHK_MAX_PRES; ++i) {
        chk->pres_id[i] = (uint16_t)(i * 9 % STEINS_MAIL_IDS);
        chk->pres_kind[i] = (uint8_t)(i % 4);
        chk->pres_owner[i] = (uint8_t)(i % 5);
    }
    chk->deferred_present = 1;
    chk->deferred_root = 0xFFFF; chk->deferred_id = STEINS_MAIL_IDS - 1; chk->deferred_read = 1;
    chk->keyphone_present = 1;
    chk->keyphone_root = 0xFFFF; chk->keyphone_owner = 255;
    chk->endings = 0xFFFF;
}

static size_t roundtrip(const char *label, const steins_checkpoint_t *chk)
{
    const size_t n = steins_checkpoint_encode(chk, s_buf, sizeof(s_buf));
    CHECK(n > 0, "%s: encode failed", label);
    if (n == 0) return 0;
    steins_checkpoint_t back;
    CHECK(steins_checkpoint_decode(&back, s_buf, n), "%s: decode failed", label);
    CHECK(steins_checkpoint_equal(chk, &back), "%s: field-by-field mismatch", label);
    // 编码是确定的(同状态同字节),便于判断"读档后状态一致"
    const size_t n2 = steins_checkpoint_encode(&back, s_buf2, sizeof(s_buf2));
    CHECK(n2 == n && memcmp(s_buf, s_buf2, n) == 0, "%s: encode is not deterministic", label);
    printf("  %-8s %4u B (inbox %u, sent %u, owners %u, pres %u)\n", label, (unsigned)n,
           (unsigned)chk->inbox_count, (unsigned)chk->sent_count,
           (unsigned)chk->owner_count, (unsigned)chk->pres_count);
    return n;
}

// IDF NVS:1 blob header + 每条 32 B 数据项 + 每 1984 B 一个 chunk header。
static unsigned nvs_entries_for(size_t length)
{
    const unsigned data = (unsigned)((length + 31) / 32);
    const unsigned chunks = (unsigned)((length + 1984 - 1) / 1984);
    return 1 + data + chunks;
}

int main(void)
{
    steins_checkpoint_t typical;
    fill_typical(&typical);
    const size_t typical_bytes = roundtrip("typical", &typical);

    steins_checkpoint_t maxst;
    fill_max(&maxst);
    const size_t max_bytes = roundtrip("max", &maxst);

    // S2 场景:115 条 inbox 的状态必须在 1 KiB 内(否则 31 槽的典型状态无法放下)
    CHECK(typical_bytes < 1024, "typical checkpoint %u B is too large", (unsigned)typical_bytes);

    // 保守容量：成功保存硬缓冲 cap=4096 B（不是所有 source watcher 的全上界），
    // 31 live 槽（30 manual + auto）+ 30 个 16 B legacy shadow + unlock/cfg/namespace，
    // 再加一次 checkpoint+legacy shadow 更新；usable 已留 1 GC 页 + 1 namespace 入口。
    const unsigned cap_bytes = 4096u;
    CHECK(cap_bytes == STEINS_CHK_MAX_BYTES, "capacity cap %u != source STEINS_CHK_MAX_BYTES %u",
          cap_bytes, (unsigned)STEINS_CHK_MAX_BYTES);
    const unsigned per_slot = nvs_entries_for(cap_bytes);
    const unsigned legacy = 2;
    const unsigned base_slots = 31u * per_slot + 30u * legacy + 7u;
    const unsigned shadow = per_slot + legacy;
    const unsigned total = base_slots + shadow;
    const unsigned pages38 = 38u;                  // nvs 0x26000 / 4096
    const unsigned usable38 = (pages38 - 1u) * 126u - 1u;
    const unsigned usable22 = (22u - 1u) * 126u - 1u;
    printf("  NVS conservative: cap=%u B, %u entries/slot, base=%u, shadow=%u, "
           "total=%u; 38p usable=%u, 22p usable=%u (max encode witness %u B)",
           cap_bytes, per_slot, base_slots, shadow, total, usable38, usable22,
           (unsigned)max_bytes);
    putchar(10);
    CHECK(total > usable22, "old 22-page NVS unexpectedly fits conservative total %u",
          total);
    CHECK(total <= usable38, "38-page NVS does not fit conservative total %u > %u",
          total, usable38);

    // 反向用例:截断 / 坏 CRC / 版本错 / 字段越界都必须拒绝
    {
        steins_checkpoint_t back;
        CHECK(!steins_checkpoint_decode(&back, s_buf, 4), "short buffer accepted");
        const size_t n = steins_checkpoint_encode(&typical, s_buf, sizeof(s_buf));
        s_buf[n - 3] ^= 0x5A;
        CHECK(!steins_checkpoint_decode(&back, s_buf, n), "bad CRC accepted");
        const size_t n2 = steins_checkpoint_encode(&typical, s_buf, sizeof(s_buf));
        s_buf[2] = 9;
        CHECK(!steins_checkpoint_decode(&back, s_buf, n2), "bad version accepted");
        const size_t n3 = steins_checkpoint_encode(&typical, s_buf, sizeof(s_buf));
        CHECK(!steins_checkpoint_decode(&back, s_buf, n3 - 1), "truncated body accepted");
    }
    // 缓冲区不够时必须失败(而不是写半截)
    {
        const size_t tiny = 40;
        CHECK(steins_checkpoint_encode(&maxst, s_buf, tiny) == 0,
              "encode into a tiny buffer did not fail");
        CHECK(steins_checkpoint_encode(&typical, s_buf, 8) == 0, "encode into 8 B did not fail");
    }

    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK (typical %u B, max %u B)\n", (unsigned)typical_bytes, (unsigned)max_bytes);
    return 0;
}
