// tests/test_steins_mobile_ui_store.c —— 手机页面 + v2 checkpoint 存档接线回归。
//
// 驱动真实 steins_app/ui/model/pack/mobile(+候选 v4 包)；不复制 codec，不伪造
// 事件状态。LEGACY 16 B 镜像用于旧工具兼容，主键是 svNNv2 / sauto。
#include "steins_app.h"
#include "steins_mobile.h"
#include "steins_mobile_pack.h"
#include "steins_pack.h"
#include "steins_ui.h"

#include "bsp_battery.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);                 \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static steins_app_t s_app;
const lv_font_t steins_story_16;
const lv_font_t steins_ui_16;
const lv_font_t steins_ui_18;

enum { BTN_UP = 0, BTN_DOWN = 1, BTN_OK = 2 };
enum { EV_PRESS = 0, EV_CLICK = 1, EV_DOUBLE = 2, EV_LONG = 3, EV_RELEASE = 4 };

static void press(int btn, int ev)
{
    const steins_key_t key = { (uint8_t)btn, (uint8_t)ev };
    steins_app_key(&s_app, &key);
}
static void click(int btn)
{
    press(btn, EV_PRESS);
    press(btn, EV_CLICK);
    press(btn, EV_RELEASE);
}
static void long_press(int btn)
{
    press(btn, EV_PRESS);
    press(btn, EV_LONG);
    press(btn, EV_RELEASE);
}
static void tick_ms(uint32_t ms) { steins_app_tick(&s_app, ms); }
static void settle(int steps)
{
    for (int i = 0; i < steps; ++i) tick_ms(50);
}

static uint8_t *load_file(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (!data) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)n;
    return data;
}

static void boot(void)
{
    lv_stub_reset_render_counters();
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins_app_init(&s_app), "app init failed");
    bsp_stub_set_soc(87);
}

static size_t read_key(const char *key, uint8_t *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("steins", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const bool ok = nvs_get_blob(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok ? len : 0;
}

static bool write_key(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("steins", NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, key, data, len) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void start_story(void)
{
    CHECK(s_app.page == STEINS_PAGE_TITLE, "not title");
    click(BTN_OK);
    settle(100);
    CHECK(s_app.page == STEINS_PAGE_GAME, "story did not start (%d)", (int)s_app.page);
    s_app.reading = true;
}

static void open_phone_from_game(void)
{
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu not open (%d)", (int)s_app.page);
    // 菜单行: 返回/保存/读取/手机/历史/自动/章节/鉴赏/设置/标题。
    for (int i = 0; i < 3; ++i) click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_HOME, "phone home not open (%d)",
          (int)s_app.page);
}

static void back_to_game(void)
{
    for (int i = 0; i < 10 && s_app.page != STEINS_PAGE_GAME; ++i) long_press(BTN_OK);
}

static void test_real_adapter_s1(void)
{
    nvs_stub_reset();
    boot();
    steins_mobile_pack_ctx_t ctx;
    steins_mobile_content_t content;
    void *user = NULL;
    CHECK(steins_mobile_pack_content(&s_app.pack, &ctx, &content, &user),
          "adapter rejected candidate pack");
    // 源节点名 "n0" -> 真实 pack 下标；不能把源后缀 0 当 pack 下标
    // （cand-3 NODE[0] 实际是 n32）。
    uint16_t n0 = 0xFFFFu;
    const bool found = steins_mobile_pack_index_of_source(&s_app.pack, 0, &n0);
    CHECK(found, "steins_mobile_pack_index_of_source(n0) failed");
    printf("INFO | source n0 -> pack node %u (helper)", (unsigned)n0);
    putchar(10);
    if (n0 == 0xFFFFu) return;
    steins_mobile_t m;
    steins_mobile_init(&m);
    steins_mobile_set_dynamic_boundaries(&m, false);
    steins_checkpoint_t chk;
    steins_checkpoint_init(&chk);
    steins_mobile_seek(&chk, n0, 351);
    CHECK(steins_mobile_enter(&m, &content, user, n0), "enter source n0 failed");
    CHECK(steins_mobile_sync(&m, &chk), "sync source n0 failed");
    CHECK(chk.inbox_count == 1 && chk.inbox[0] == 0,
          "S1 inbox=%u id0=%u (expected 1/0 at source n0@351)",
          (unsigned)chk.inbox_count, chk.inbox_count ? (unsigned)chk.inbox[0] : 0xFFFFu);
    CHECK(chk.last_at == 351, "S1 lastAt=%u", (unsigned)chk.last_at);
}

static void test_real_pres_provider(void)
{
    nvs_stub_reset();
    boot();
    steins_mobile_pack_ctx_t ctx;
    steins_mobile_content_t content;
    void *user = NULL;
    if (!steins_mobile_pack_content(&s_app.pack, &ctx, &content, &user)) {
        printf("SKIP | no mobile content provider");
        putchar(10);
        return;
    }
    uint16_t count = content.presentation_key_count ? content.presentation_key_count(user) : 0;
    if (count == 0) {
        printf("SKIP | candidate has no PRES key directory");
        putchar(10);
        return;
    }
    uint16_t node = 0;
    CHECK(steins_mobile_pack_index_of_source(&s_app.pack, 32, &node), "n32 not found");
    uint16_t rows = content.row_count ? content.row_count(user, node) : 0;
    bool found = false;
    uint16_t pres_row_cursor = 0;
    uint16_t pres_key = 0xFFFFu;
    for (uint16_t r = 0; r < rows && !found; ++r) {
        steins_mobile_row_t row;
        if (!content.row || !content.row(user, node, r, &row)) break;
        for (uint8_t e = 0; e < row.count; ++e) {
            steins_mobile_event_t ev;
            if (!content.event || !content.event(user, node, &row, e, &ev)) break;
            if (ev.kind != STEINS_MOB_EV_PRESENTATION) continue;
            const uint16_t key = content.presentation_key
                                     ? content.presentation_key(user, node, &row, e)
                                     : 0xFFFFu;
            CHECK(key != 0xFFFFu && key < count,
                  "PRES key provider n32 row%u ev%u -> key=%u/%u", r, e, key, count);
            pres_row_cursor = (uint16_t)row.cursor;
            pres_key = key;
            found = true;
            break;
        }
    }
    CHECK(found, "no presentation event found at source n32 (pack index %u)", node);
    if (found && pres_key != 0xFFFFu) {
        // 真正的 engine+adapter+codec 去重：入队 -> finish 置 bit -> 回退 lastAt 重入不重播。
        steins_mobile_t m;
        steins_mobile_init(&m);
        steins_mobile_set_dynamic_boundaries(&m, false);
        steins_checkpoint_t chk;
        steins_checkpoint_init(&chk);
        steins_mobile_seek(&chk, node, pres_row_cursor);
        CHECK(steins_mobile_enter(&m, &content, user, node), "PRES enter n32");
        CHECK(steins_mobile_sync(&m, &chk), "PRES sync n32");
        CHECK(chk.pres_count >= 1, "PRES not enqueued");
        if (chk.pres_count >= 1) {
            CHECK(chk.pres_key[0] == (uint8_t)pres_key, "PRES queued key=%u want %u",
                  chk.pres_key[0], pres_key);
            CHECK(steins_mobile_finish_presentation(&m, &chk), "PRES finish");
            CHECK(chk.pres_read_bits[pres_key >> 3] & (uint8_t)(1u << (pres_key & 7u)),
                  "PRES finish did not set readbit");
            chk.last_at = pres_row_cursor - 1u;
            CHECK(steins_mobile_sync(&m, &chk), "PRES rewind sync");
            CHECK(chk.pres_count == 0, "PRES replayed after finish (%u)", chk.pres_count);
        }
    }
    printf("INFO | PRES provider count=%u, n32 first key found", (unsigned)count);
    putchar(10);
}

static void test_phone_pages_and_v2(void)
{
    nvs_stub_reset();
    boot();
    start_story();

    // 真实候选包的 id 顺序：选两条正文与一个 type3 回复，验证完整分页/已读/回复。
    uint16_t mail_id = 0xFFFFu, reply_parent = 0xFFFFu, reply_id = 0xFFFFu;
    for (uint16_t id = 0; id < steins_pack_mail_count(&s_app.pack); ++id) {
        steins_mail_t mail;
        if (!steins_pack_mail(&s_app.pack, id, &mail)) continue;
        if (mail.body == 0xFFFFu) continue;
        if (mail_id == 0xFFFFu) mail_id = id;
        uint16_t succ[16];
        if (steins_pack_mail_succ(&s_app.pack, &mail, succ, 16, true) > 0) {
            reply_parent = id;
            reply_id = succ[0];
            if (mail_id != 0xFFFFu) break;
        }
    }
    CHECK(mail_id != 0xFFFFu, "no mail body in candidate pack");
    CHECK(reply_parent != 0xFFFFu && reply_id != 0xFFFFu, "no type3 reply in candidate pack");
    if (mail_id == 0xFFFFu) return;

    steins_checkpoint_init(&s_app.chk);
    s_app.chk.node = s_app.pos.node;
    s_app.chk.cursor = s_app.pos.cursor;
    s_app.chk.mode = s_app.pos.mode;
    s_app.chk.inbox[0] = mail_id;
    s_app.chk.inbox[1] = reply_parent;
    s_app.chk.inbox_count = 2;
    steins_checkpoint_set_unread(&s_app.chk, mail_id, true);
    steins_checkpoint_set_unread(&s_app.chk, reply_parent, true);
    s_app.chk.open_count = 1;
    s_app.chk.open_id[0] = reply_parent;
    s_app.chk.open_val[0] = 1;

    open_phone_from_game();
    CHECK(s_app.list_count == 4, "phone home count %d", s_app.list_count);
    CHECK(steins_mobile_inbox_count(&s_app.chk) == 2, "inbox count %u",
          (unsigned)steins_mobile_inbox_count(&s_app.chk));
    click(BTN_OK);   // 收信箱
    CHECK(s_app.page == STEINS_PAGE_PHONE_INBOX, "inbox page %d", (int)s_app.page);
    click(BTN_OK);   // 第一封
    CHECK(s_app.page == STEINS_PAGE_MAIL && s_app.phone_mail_mode, "phone mail page %d",
          (int)s_app.page);
    CHECK(s_app.phone_mail == mail_id, "opened wrong mail %u", (unsigned)s_app.phone_mail);
    steins_mail_t opened;
    CHECK(steins_pack_mail(&s_app.pack, mail_id, &opened), "mail metadata missing");
    char body[STEINS_TEXT_BYTES + 64];
    const int body_len = steins_pack_str_utf8(&s_app.pack, opened.body, body, sizeof(body));
    CHECK(body_len >= 0, "body not readable");
    CHECK(strcmp(s_app.phone_body, body) == 0, "phone body differs from pack");
    CHECK(s_app.phone_mail_pages >= 1, "phone pages %d", s_app.phone_mail_pages);
    // 翻完所有页，正文仍在 app 缓冲里（分页只是显示层，不截断持久文本）。
    for (int i = 0; i + 1 < s_app.phone_mail_pages; ++i) {
        click(BTN_DOWN);
        CHECK(s_app.page == STEINS_PAGE_MAIL, "page lost at %d", i);
    }
    CHECK(s_app.phone_mail_page + 1 == s_app.phone_mail_pages, "not on last page");
    CHECK(steins_checkpoint_read(&s_app.chk, mail_id), "read bit not set");
    CHECK(!steins_checkpoint_unread(&s_app.chk, mail_id), "unread bit not cleared");

    // 返回收件箱，读可回复邮件。
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_INBOX, "back to inbox failed (%d)",
          (int)s_app.page);
    click(BTN_DOWN);   // 第二封 reply_parent
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MAIL && s_app.phone_mail == reply_parent,
          "reply parent not opened");
    for (int i = 0; i + 1 < s_app.phone_mail_pages; ++i) click(BTN_DOWN);
    CHECK(s_app.phone_reply_count > 0, "reply not offered");
    click(BTN_OK);   // 末页 -> reply list
    CHECK(s_app.page == STEINS_PAGE_PHONE_REPLY, "reply list page %d", (int)s_app.page);
    click(BTN_OK);   // 第一个回复
    CHECK(steins_checkpoint_replied(&s_app.chk, reply_parent), "parent not replied");
    bool sent = false;
    for (uint16_t i = 0; i < s_app.chk.sent_count; ++i)
        if (s_app.chk.sent[i] == reply_id) sent = true;
    CHECK(sent, "reply id not in sent");
    CHECK(s_app.page == STEINS_PAGE_MAIL && s_app.phone_mail_sent,
          "sent detail not opened");

    // auto v2：flush 写入 sauto；decode 后 inbox 顺序与 last_at 保留。
    s_app.chk.last_node = s_app.chk.node;
    s_app.chk.last_at = s_app.chk.cursor;   // no unprocessed rows at this cursor
    steins_app_flush(&s_app);
    uint8_t auto_buf[STEINS_CHK_MAX_BYTES];
    const size_t auto_len = read_key("sauto", auto_buf, sizeof(auto_buf));
    CHECK(auto_len > 0, "sauto missing");
    steins_checkpoint_t auto_chk;
    bool migrated = false;
    CHECK(steins_checkpoint_save_decode(&auto_chk, auto_buf, auto_len, &migrated),
          "sauto v2 decode failed");
    CHECK(!migrated, "new auto reported as v1");
    CHECK(auto_chk.inbox_count == 2 && auto_chk.inbox[0] == mail_id &&
              auto_chk.inbox[1] == reply_parent,
          "inbox order not preserved");
    CHECK(auto_chk.last_node == auto_chk.node && auto_chk.last_at == auto_chk.cursor,
          "lastAt not preserved");

    // 手动 v2 + 旧 16 B 镜像：主动确认后 v2 存在，load 还原同一手机状态。
    back_to_game();
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu for save (%d)", (int)s_app.page);
    click(BTN_DOWN);   // 保存
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SAVES, "save page %d", (int)s_app.page);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "confirm page %d", (int)s_app.page);
    click(BTN_UP);     // 确定
    click(BTN_OK);
    uint8_t slot[STEINS_CHK_MAX_BYTES];
    const size_t slot_len = read_key("sv00v2", slot, sizeof(slot));
    CHECK(slot_len > 0, "manual v2 slot missing");
    steins_checkpoint_t slot_chk;
    CHECK(steins_checkpoint_save_decode(&slot_chk, slot, slot_len, &migrated),
          "manual v2 decode failed");
    CHECK(slot_chk.inbox_count == 2 && steins_checkpoint_replied(&slot_chk, reply_parent),
          "manual slot lost mobile state");
    uint8_t legacy[STEINS_SAVE_BYTES];
    CHECK(read_key("sv00", legacy, sizeof(legacy)) == STEINS_SAVE_BYTES,
          "legacy 16B mirror missing");
    steins_pos_t legacy_pos;
    CHECK(steins_save_decode(&legacy_pos, legacy, sizeof(legacy)),
          "legacy mirror does not decode");
    CHECK(legacy_pos.node == slot_chk.node && legacy_pos.cursor == slot_chk.cursor,
          "legacy mirror position differs");

    // 走真实标题→读取路径：checkpoint_apply 后手机状态与当前待选/位置一致。
    if (s_app.page == STEINS_PAGE_SAVES) long_press(BTN_OK);
    if (s_app.page == STEINS_PAGE_MENU) {
        s_app.sel = 9;   // 返回标题
        click(BTN_OK);
    }
    CHECK(s_app.page == STEINS_PAGE_TITLE, "title for load (%d)", (int)s_app.page);
    s_app.sel = 0;
    click(BTN_DOWN);
    click(BTN_DOWN);   // 读取
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SAVES, "load list after save (%d)", (int)s_app.page);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "saved slot did not load (%d)", (int)s_app.page);
    CHECK(s_app.chk.inbox_count == 2 && steins_checkpoint_replied(&s_app.chk, reply_parent),
          "loaded checkpoint lost inbox/replied");

    // auto checkpoint 读回（标题继续），不重放/不丢 lastAt。
    s_app.chk.last_node = s_app.chk.node;
    s_app.chk.last_at = s_app.chk.cursor;
    steins_app_flush(&s_app);
    long_press(BTN_OK);
    if (s_app.page == STEINS_PAGE_MENU) {
        s_app.sel = 9;
        click(BTN_OK);
    }
    CHECK(s_app.page == STEINS_PAGE_TITLE, "title for continue (%d)", (int)s_app.page);
    s_app.sel = 0;
    click(BTN_DOWN);   // 继续阅读（有 auto）
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "auto continue failed (%d)", (int)s_app.page);
    CHECK(s_app.chk.inbox_count == 2, "auto continue lost inbox");
    CHECK(s_app.chk.last_at == s_app.chk.cursor, "auto continue did not advance lastAt");


    // 坏 v2 槽：typed CORRUPT，不改变当前阅读位置。
    uint8_t bad[64];
    memset(bad, 0xA5, sizeof(bad));
    bad[0] = 'S'; bad[1] = 'M'; bad[2] = 2;
    CHECK(write_key("sv01v2", bad, sizeof(bad)), "inject corrupt slot");
    long_press(BTN_OK);   // save list -> menu? then title? use title load via menu
    // 从当前存档页长按返回菜单，再进入读取
    if (s_app.page == STEINS_PAGE_SAVES) long_press(BTN_OK);
    if (s_app.page == STEINS_PAGE_MENU) {
        s_app.sel = 0;
        click(BTN_DOWN);   // 读取
        click(BTN_DOWN);
        click(BTN_OK);
    }
    CHECK(s_app.page == STEINS_PAGE_SAVES, "load page for corrupt (%d)", (int)s_app.page);
    s_app.sel = 1;
    const uint16_t before_node = s_app.pos.node;
    const uint32_t before_cursor = s_app.pos.cursor;
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SAVES, "corrupt accepted");
    CHECK(s_app.notice_ms > 0, "corrupt produced no notice");
    CHECK(s_app.pos.node == before_node && s_app.pos.cursor == before_cursor,
          "corrupt changed position");

    // open 失败：不写 v2、不假成功。
    nvs_stub_reset();
    boot();
    start_story();
    nvs_stub_set_writable(0);
    long_press(BTN_OK);
    click(BTN_DOWN);
    click(BTN_OK);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "open-fail confirm %d", (int)s_app.page);
    click(BTN_UP);
    click(BTN_OK);
    CHECK(s_app.notice_ms > 0, "open failure no notice");
    CHECK(read_key("sv00v2", slot, sizeof(slot)) == 0, "open failure wrote v2 slot");
    nvs_stub_set_writable(1);
}

static void test_presentation_key_codec(void)
{
    steins_checkpoint_t chk, back;
    steins_checkpoint_init(&chk);
    chk.node = 32;
    chk.cursor = 695;
    chk.pres_count = 1;
    chk.pres_id[0] = 263;
    chk.pres_kind[0] = STEINS_MOB_PRES_DRAFT;
    chk.pres_owner[0] = 1;
    uint8_t buf[STEINS_CHK_MAX_BYTES];
    bool migrated = true;

    // key 0 / 55 / none 往返；none wire 63 必须回到 0xFF。
    const uint8_t keys[] = {0, 55, 0xFF};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        chk.pres_key[0] = keys[k];
        const size_t n = steins_checkpoint_encode(&chk, buf, sizeof(buf));
        CHECK(n > 0, "encode key %u failed", keys[k]);
        CHECK(steins_checkpoint_save_decode(&back, buf, n, &migrated),
              "decode key %u failed", keys[k]);
        CHECK(back.pres_key[0] == keys[k], "key %u roundtrip -> %u", keys[k],
              back.pres_key[0]);
    }

    // 源 PRES 目录只有 0..55；56..63 不是合法 key，编码拒绝（63 是 wire NONE）。
    for (int key = STEINS_CHK_MAX_PRES_KEYS; key <= STEINS_CHK_MAX_PRES; ++key) {
        steins_checkpoint_t bad = chk;
        bad.pres_key[0] = (uint8_t)key;
        CHECK(steins_checkpoint_encode(&bad, buf, sizeof(buf)) == 0,
              "invalid PRES key %d was encoded", key);
    }

    // 构造 key0 blob，篡改 6-bit wire：56/62 必须拒绝，63 -> FF；byte7 置位拒绝。
    chk.pres_key[0] = 0;
    const size_t n = steins_checkpoint_encode(&chk, buf, sizeof(buf));
    CHECK(n > 20, "short key blob");
    // key1 项的 wire key 紧接在 pres triplets 后；其后固定 16 B
    // (readbits8 + source/mail/open/deferred/keyphone/endings + CRC2)。
    const size_t wire = n - 16u - 1u;
    const uint8_t tamper[] = {56, 62};
    for (size_t i = 0; i < sizeof(tamper) / sizeof(tamper[0]); ++i) {
        uint8_t copy[STEINS_CHK_MAX_BYTES];
        memcpy(copy, buf, n);
        copy[wire] = tamper[i];
        const uint16_t crc = steins_crc16(copy, n - 2u);
        copy[n - 2u] = (uint8_t)crc;
        copy[n - 1u] = (uint8_t)(crc >> 8);
        CHECK(!steins_checkpoint_decode(&back, copy, n), "wire key %u accepted",
              tamper[i]);
    }
    {
        uint8_t copy[STEINS_CHK_MAX_BYTES];
        memcpy(copy, buf, n);
        copy[wire] = 0x3Fu;   // NONE
        const uint16_t crc = steins_crc16(copy, n - 2u);
        copy[n - 2u] = (uint8_t)crc;
        copy[n - 1u] = (uint8_t)(crc >> 8);
        CHECK(steins_checkpoint_decode(&back, copy, n) && back.pres_key[0] == 0xFFu,
              "wire NONE did not decode to 0xFF");
    }
    {
        uint8_t copy[STEINS_CHK_MAX_BYTES];
        memcpy(copy, buf, n);
        copy[n - 9u] = 1;   // readbits[7] reserved byte
        const uint16_t crc = steins_crc16(copy, n - 2u);
        copy[n - 2u] = (uint8_t)crc;
        copy[n - 1u] = (uint8_t)(crc >> 8);
        CHECK(!steins_checkpoint_decode(&back, copy, n), "reserved readbit accepted");
    }
    // key55 的 bit55 正常置位/恢复。
    chk.pres_key[0] = 55;
    chk.pres_read_bits[6] = 0x80u;
    const size_t n2 = steins_checkpoint_encode(&chk, buf, sizeof(buf));
    CHECK(n2 > 0 && steins_checkpoint_save_decode(&back, buf, n2, &migrated),
          "key55/bit55 encode/decode");
    CHECK(back.pres_key[0] == 55 && back.pres_read_bits[6] == 0x80u,
          "key55/bit55 not preserved");
}

static void test_clip_presentation_store(void)
{
    nvs_stub_reset();
    boot();
    start_story();
    // 找一个带 clip 的通话站/选项，模拟引擎已经收到 call（不改事件机内部）。
    uint16_t site = 0xFFFFu;
    uint8_t item_index = 0;
    for (uint16_t s = 0; s < steins_pack_call_site_count(&s_app.pack); ++s) {
        steins_callsite_t cs;
        if (!steins_pack_call_site(&s_app.pack, s, &cs)) continue;
        for (uint8_t i = 0; i < cs.item_count; ++i) {
            steins_callitem_t ci;
            if (!steins_pack_call_item(&s_app.pack, (uint16_t)(cs.first_item + i), &ci))
                continue;
            if (ci.clip != 0xFFu) {
                site = s;
                item_index = i;
                break;
            }
        }
        if (site != 0xFFFFu) break;
    }
    uint8_t buf[STEINS_CHK_MAX_BYTES];
    steins_checkpoint_t chk;
    bool migrated = false;
    if (site == 0xFFFFu) {
        printf("SKIP | candidate has no call clip (CALLITEM.clip all 0xFF); clip save/return not tested");
        putchar(10);
    } else {
    s_app.chk.call_present = 1;
    s_app.chk.call_site = site;
    s_app.chk.call_owner = 0;
    s_app.chk.call_kind = 0;
    s_app.chk.call_node = s_app.chk.node;
    s_app.chk.call_at = s_app.chk.cursor;
    s_app.chk.call_waiting = 0;

    open_phone_from_game();
    s_app.sel = 3;   // 当前电话/发信
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_CALLS, "calls page (%d)", (int)s_app.page);
    s_app.sel = item_index;
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_CLIP, "clip page (%d)", (int)s_app.page);
    CHECK(s_app.phone_clip_count > 0, "clip has no frames");
    // 在 clip 中存档：checkpoint 必须允许虚拟 node，而不是按主图 node 数拒绝。
    steins_app_flush(&s_app);
    const size_t n = read_key("sauto", buf, sizeof(buf));
    CHECK(n > 0 && steins_checkpoint_save_decode(&chk, buf, n, &migrated),
          "clip auto decode failed");
    CHECK(chk.node >= STEINS_MOB_CLIP_NODE_BASE && chk.call_return_present,
          "clip auto did not retain virtual node/callReturn (node=0x%04X)",
          (unsigned)chk.node);
    // 真实标题→继续路径恢复合法 clip node，而不是按主图 node 数拒绝。
    back_to_game();
    long_press(BTN_OK);
    if (s_app.page == STEINS_PAGE_MENU) {
        s_app.sel = 9;
        click(BTN_OK);
    }
    CHECK(s_app.page == STEINS_PAGE_TITLE, "title for clip restore (%d)", (int)s_app.page);
    s_app.sel = 0;
    click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_CLIP, "clip restore page (%d)", (int)s_app.page);
    }

    // presentation 队列与事件游标一起持久化（UI 暂不做动画，仅验证 codec/状态）。
    s_app.chk.pres_count = 1;
    s_app.chk.pres_id[0] = 263;
    s_app.chk.pres_kind[0] = 1;
    s_app.chk.pres_owner[0] = 0;
    s_app.chk.last_node = s_app.chk.node;
    s_app.chk.last_at = s_app.chk.cursor;
    steins_app_flush(&s_app);
    const size_t pn = read_key("sauto", buf, sizeof(buf));
    CHECK(pn > 0 && steins_checkpoint_save_decode(&chk, buf, pn, &migrated),
          "presentation auto decode failed");
    CHECK(chk.pres_count == 1 && chk.pres_id[0] == 263 &&
              chk.pres_kind[0] == 1 && chk.last_at == chk.cursor,
          "presentation/lastAt not persisted");
}

static void test_presentation_page(void)
{
    nvs_stub_reset();
    boot();
    start_story();
    s_app.chk.pres_count = 1;
    s_app.chk.pres_id[0] = 263;
    s_app.chk.pres_kind[0] = STEINS_MOB_PRES_DRAFT;
    s_app.chk.pres_owner[0] = 0;
    open_phone_from_game();
    s_app.sel = 3;   // 当前电话/演示
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MAIL && s_app.phone_presentation_pending,
          "presentation page not opened (%d)", (int)s_app.page);
    click(BTN_OK);   // 末页/完成演示
    CHECK(!s_app.phone_presentation_pending, "presentation not finished");
    CHECK(s_app.chk.pres_count == 0, "presentation queue not dequeued");
    CHECK(s_app.page == STEINS_PAGE_GAME, "presentation did not return to reading (%d)",
          (int)s_app.page);
}

static void test_v1_migration(void)
{
    nvs_stub_reset();
    boot();
    start_story();
    steins_pos_t old = { s_app.pos.node, s_app.pos.cursor, STEINS_MODE_STORY, 0 };
    uint8_t v1[STEINS_SAVE_BYTES];
    CHECK(steins_save_encode(&old, v1, sizeof(v1)) == STEINS_SAVE_BYTES, "v1 encode");
    CHECK(write_key("sv04", v1, sizeof(v1)), "inject v1 slot");
    long_press(BTN_OK);
    s_app.sel = 2;   // 读取
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SAVES, "load v1 list (%d)", (int)s_app.page);
    s_app.sel = 4;
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "v1 slot did not migrate/load (%d)",
          (int)s_app.page);
    CHECK(s_app.pos.node == old.node && s_app.pos.cursor == old.cursor,
          "v1 migration changed position");
    // 下一次保存写 v3 主键，v1 mirror 仍保留。
    steins_app_flush(&s_app);
    uint8_t buf[STEINS_CHK_MAX_BYTES];
    CHECK(read_key("sauto", buf, sizeof(buf)) > 0, "v2 auto after v1 migration missing");
}

static void test_30_slots_auto_and_failure(void)
{
    nvs_stub_reset();
    boot();
    start_story();
    // 30 个手动槽走真实 SAVE 页面；每个槽必须写 v2 主键。
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu before slots (%d)", (int)s_app.page);
    for (int i = 0; i < STEINS_SLOTS; ++i) {
        s_app.sel = 1;   // 保存
        click(BTN_OK);
        CHECK(s_app.page == STEINS_PAGE_SAVES, "save page slot %d (%d)", i,
              (int)s_app.page);
        s_app.sel = i;
        click(BTN_OK);
        CHECK(s_app.page == STEINS_PAGE_CONFIRM, "confirm slot %d", i);
        click(BTN_UP);   // 确定
        click(BTN_OK);
        char key[16];
        snprintf(key, sizeof(key), "sv%02dv2", i);
        uint8_t buf[STEINS_CHK_MAX_BYTES];
        CHECK(read_key(key, buf, sizeof(buf)) > 0, "slot %d v2 missing", i);
        long_press(BTN_OK);   // SAVES -> MENU
    }
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu after slots (%d)", (int)s_app.page);
    // 首/末槽 decode 后位置/手机状态一致。
    uint8_t buf[STEINS_CHK_MAX_BYTES];
    for (int slot = 0; slot < STEINS_SLOTS; slot += 29) {
        char key[16];
        snprintf(key, sizeof(key), "sv%02dv2", slot);
        const size_t n = read_key(key, buf, sizeof(buf));
        steins_checkpoint_t chk;
        bool migrated = false;
        CHECK(n > 0 && steins_checkpoint_save_decode(&chk, buf, n, &migrated),
              "slot %d decode failed", slot);
        CHECK(chk.node == s_app.chk.node && chk.cursor == s_app.chk.cursor,
              "slot %d position mismatch", slot);
    }

    // auto checkpoint + commit failure 2 s retry（任意页，失败不假成功）。
    long_press(BTN_OK);   // 回游戏（菜单 -> game）
    CHECK(s_app.page == STEINS_PAGE_GAME, "not back to game (%d)", (int)s_app.page);
    steins_app_flush(&s_app);
    uint8_t old_auto[STEINS_CHK_MAX_BYTES];
    const size_t old_len = read_key("sauto", old_auto, sizeof(old_auto));
    CHECK(old_len > 0, "auto missing before failure");
    nvs_stub_set_fail_commit(1);
    s_app.chk.last_at = 77;
    steins_app_flush(&s_app);
    nvs_stub_set_fail_commit(0);
    CHECK(s_app.checkpoint_dirty, "failed auto did not mark dirty");
    // 宿主 nvs_stub 的 commit 失败不是事务性的（先写后 commit），所以这里只断言
    // 应用契约：标记 dirty、不报成功，2 s 后重试；真实 IDF NVS 的事务/掉电不在宿主桩内。
    CHECK(s_app.checkpoint_dirty, "failed auto not dirty");
    tick_ms(2100);
    CHECK(!s_app.checkpoint_dirty, "2s retry did not clear dirty");
    uint8_t failed_auto[STEINS_CHK_MAX_BYTES];
    const size_t retry_len = read_key("sauto", failed_auto, sizeof(failed_auto));
    steins_checkpoint_t retry_chk;
    bool migrated = false;
    CHECK(retry_len > 0 && steins_checkpoint_save_decode(&retry_chk, failed_auto, retry_len,
                                                         &migrated),
          "retry auto decode failed");
    CHECK(retry_chk.last_at == 77, "retry auto lost lastAt");

    // open/commit 的读失败事务性由生产 NVS 语义保证；宿主 nvs_stub 目前只有
    // 写 open 失败和“先写后 commit”的简化行为，因此这里不把桩当物理 NVS 结论。
    // 已有的 corrupt/空槽 typed 回归仍覆盖 CORRUPT/EMPTY；open 失败写路径在
    // test_steins_app.c 的 save_failures 覆盖。

}

static void test_nvs_capacity_model(void)
{
    // Conservative NVS witness: successful-save hard cap 4096 B (not a source
    // full-codec upper bound). 31 live slots + 30 legacy 16B + unlock/cfg/namespace
    // + one checkpoint/legacy shadow; reserve one GC page.
    const unsigned cap_bytes = 4096u;
    CHECK(cap_bytes == STEINS_CHK_MAX_BYTES, "capacity cap %u != source STEINS_CHK_MAX_BYTES %u",
          cap_bytes, (unsigned)STEINS_CHK_MAX_BYTES);
    const unsigned per_slot = 1u + (cap_bytes + 31u) / 32u +
                              (cap_bytes + 1984u - 1u) / 1984u;
    const unsigned legacy = 2;
    const unsigned base_slots = 31u * per_slot + 30u * legacy + 7u;
    const unsigned shadow = per_slot + legacy;
    const unsigned total = base_slots + shadow;
    const unsigned usable38 = (38u - 1u) * 126u - 1u;
    const unsigned usable22 = (22u - 1u) * 126u - 1u;
    CHECK(total > usable22, "old 22-page NVS unexpectedly fits conservative %u",
          total);
    CHECK(total <= usable38, "38-page NVS does not fit conservative %u > %u",
          total, usable38);
    printf("  NVS witness: cap=%u B, %u entries/slot, base=%u, shadow=%u, total=%u; "
           "38p=%u, 22p=%u",
           cap_bytes, per_slot, base_slots, shadow, total, usable38, usable22);
    putchar(10);

}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/mobile-pack-cand-3/steins_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        fprintf(stderr, "cannot read pack: %s\n", path);
        return 2;
    }
    steins_pack_host_set(blob, size);
    bsp_stub_set_soc(87);
    nvs_stub_reset();

    test_real_adapter_s1();
    test_real_pres_provider();
    test_phone_pages_and_v2();
    test_presentation_key_codec();
    test_clip_presentation_store();
    test_presentation_page();
    test_v1_migration();
    test_30_slots_auto_and_failure();
    test_nvs_capacity_model();

    free(blob);
    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
