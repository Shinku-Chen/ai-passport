// tests/test_steins0_event_save.c —— app-level save/load/continue/auto regression
// for checkpoint event positions and folded RINE migration.
#include "steins0_app.h"
#include "steins0_engine.h"
#include "steins0_pack.h"
#include "steins0_text.h"

#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

extern int host_nvs_writes;
extern void host_nvs_reset(void);
extern void host_lvgl_reset(void);
extern void host_lvgl_set_locked(int locked);
extern void host_image_reset(void);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static steins0_app_t s_app;
static uint8_t *s_blob;
static uint32_t s_blob_size;

static void key(uint8_t btn, uint8_t ev)
{
    steins0_key_t k = {.btn = btn, .ev = ev};
    steins0_app_key(&s_app, &k);
}
static void click(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK);
}
static void longp(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_LONG); key(btn, BSP_BTN_RELEASE);
}

static void boot(void)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_image_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "app init");
    host_lvgl_set_locked(1);
}

static void menu_row(int row);

static uint16_t find_reply(uint16_t base)
{
    for (uint16_t i = 0; i < s_app.pack.sec[SG0_SEC_RREPLY].count; ++i) {
        uint16_t b = 0;
        if (sg0_pack_rine_reply(&s_app.pack, i, &b, NULL, NULL, NULL, NULL) && b == base) {
            return i;
        }
    }
    return 0xFFFFu;
}

static void install_reply_state(uint8_t node, uint16_t cursor, uint16_t base)
{
    sg0_state_t st;
    sg0_engine_init(&st, s_app.unlock.endings, s_app.unlock.flags);
    st.node = node;
    st.cursor = cursor;
    (void)sg0_engine_locate(&s_app.pack, &st, NULL);
    const uint16_t reply = find_reply(base);
    CHECK(reply != 0xFFFFu, "reply record base %u", base);
    CHECK(sg0_engine_send_reply(&s_app.pack, &st, reply, 0), "send reply base %u", base);
    s_app.state = st;
    s_app.game_started = true;
    s_app.page = SG0_PAGE_GAME;
    s_app.page_index = 0;
    s_app.force_scene = true;
    s_app.last_view = 0xFFFFu;
    s_app.last_history_node = 0xFFu;
    s_app.last_history_cursor = 0xFFFFu;
    steins0_ui_show(&s_app.ui, SG0_PAGE_GAME);
}

static void install_r9_state(void)
{
    sg0_state_t st;
    sg0_engine_init(&st, s_app.unlock.endings, s_app.unlock.flags);
    st.node = 0;
    st.cursor = 841;
    (void)sg0_engine_locate(&s_app.pack, &st, NULL);
    bool marked = false;
    for (uint16_t i = 0; i < st.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_app.pack, st.rine.refs[i], &msg)) continue;
        if (!(msg.flags & 1u) && msg.base == 3072) {
            st.rine.read[i] = 1;
            marked = true;
            break;
        }
    }
    CHECK(marked, "R9 base3072 parent read");
    st.cursor = 842;
    (void)sg0_engine_locate(&s_app.pack, &st, NULL);
    bool fallback = false;
    for (uint16_t i = 0; i < st.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&s_app.pack, st.rine.refs[i], &msg)) continue;
        if (msg.id == 15 && msg.base == 3072) fallback = true;
    }
    CHECK(fallback, "R9 fallback 15 present");
    s_app.state = st;
    s_app.game_started = true;
    s_app.page = SG0_PAGE_GAME;
    s_app.force_scene = true;
    s_app.last_view = 0xFFFFu;
    steins0_ui_show(&s_app.ui, SG0_PAGE_GAME);
}

static void test_r9_thread_navigation(void)
{
    install_r9_state();
    const uint16_t refs = s_app.state.rine.count;
    const uint16_t replied = s_app.state.rine.replied;
    const uint16_t windows = s_app.state.rine.windows;
    menu_row(1);                 // phone
    click(BSP_BTN_OK);           // inbox
    CHECK(s_app.phone_view == SG0_PHONE_INBOX, "R9 inbox");
    int thread_index = -1;
    for (int i = 0; i < SG0_PHONE_KEY_MAX; ++i) {
        if (s_app.phone_thread_keys[i].base == 3072) { thread_index = i; break; }
    }
    CHECK(thread_index >= 0, "R9 fallback thread index");
    s_app.phone_sel = thread_index;
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_PHONE_THREAD, "R9 thread");
    const uint16_t count = s_app.phone_thread_count;
    CHECK(count > 0, "R9 thread parts %u", count);
    for (int k = 0; k < count; ++k) {
        click(BSP_BTN_DOWN);
        CHECK(s_app.phone_thread_count == count, "R9 count stable %d", k);
        CHECK(s_app.state.rine.count == refs && s_app.state.rine.replied == replied &&
              s_app.state.rine.windows == windows, "R9 state stable %d", k);
    }
    longp(BSP_BTN_OK); longp(BSP_BTN_OK); longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "R9 back game");
}

static void menu_row(int row)
{
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "menu");
    for (int i = 0; i < 8 && s_app.list_sel != row; ++i) click(BSP_BTN_DOWN);
    CHECK(s_app.list_sel == row, "menu row %d (got %d)", row, s_app.list_sel);
    click(BSP_BTN_OK);
}

static void save_slot0(void)
{
    s_app.slot_sel = 0;
    menu_row(2);                  // opens save list and selects slot_sel
    click(BSP_BTN_OK);            // slot 0
    CHECK(s_app.slot_valid[0], "slot0 valid");
}

static void load_slot0(void)
{
    menu_row(3);
    click(BSP_BTN_OK);            // slot 0
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "load lands in game (page %d)", s_app.page);
}

static void load_slot0_must_fail(void)
{
    if (s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_SLOTS_LOAD) {
        longp(BSP_BTN_OK);        // failed load left the load list; return to game
        CHECK(s_app.page == SG0_PAGE_GAME, "back to game after invalid save");
    }
    menu_row(3);
    click(BSP_BTN_OK);            // slot 0
    CHECK(s_app.page == SG0_PAGE_LIST, "invalid save stays in list (page %d)", s_app.page);
}

static void check_state_same(const sg0_state_t *a, const sg0_state_t *b, const char *label)
{
    CHECK(a->node == b->node && a->cursor == b->cursor, "%s node/cursor", label);
    CHECK(a->mode == b->mode && a->endings == b->endings && a->flags == b->flags,
          "%s mode/endings/flags", label);
    CHECK(a->event_node == b->event_node && a->event_cursor == b->event_cursor,
          "%s event position %u/%u vs %u/%u", label, a->event_node, a->event_cursor,
          b->event_node, b->event_cursor);
    CHECK(a->rine.count == b->rine.count, "%s refs count %u/%u", label,
          a->rine.count, b->rine.count);
    const uint16_t n = a->rine.count < b->rine.count ? a->rine.count : b->rine.count;
    CHECK(memcmp(a->rine.refs, b->rine.refs, n * sizeof(a->rine.refs[0])) == 0,
          "%s refs content", label);
    CHECK(memcmp(a->rine.read, b->rine.read, n) == 0, "%s read bits", label);
    CHECK(a->rine.replied == b->rine.replied && a->rine.windows == b->rine.windows,
          "%s replied/windows", label);
    CHECK(a->rine.pending_valid == b->rine.pending_valid &&
          a->rine.pending_node == b->rine.pending_node, "%s pending", label);
    CHECK(sg0_engine_unread(&s_app.pack, a) == sg0_engine_unread(&s_app.pack, b),
          "%s unread", label);
    for (uint16_t i = 0; i < n; ++i) {
        if (a->rine.refs[i] != b->rine.refs[i]) break;
        const uint16_t da = sg0_engine_rine_date(&s_app.pack, a, a->rine.refs[i]);
        const uint16_t db = sg0_engine_rine_date(&s_app.pack, b, b->rine.refs[i]);
        CHECK(da == db, "%s effective date ref %u", label, a->rine.refs[i]);
    }
}

static void save_load_coordinate(uint8_t node, uint16_t cursor, uint16_t base)
{
    install_reply_state(node, cursor, base);
    const sg0_state_t before = s_app.state;
    CHECK(before.event_node == node, "event node before %u", before.event_node);
    CHECK(before.event_cursor > 0, "event ordinal before %u", before.event_cursor);
    save_slot0();
    s_app.state.cursor++;          // force an observable live-state change
    load_slot0();
    check_state_same(&before, &s_app.state, "first load");
    load_slot0();
    check_state_same(&before, &s_app.state, "second load");
    CHECK(!sg0_engine_can_reply(&s_app.pack, &s_app.state, base),
          "loaded replied window stays closed");
    if (node == 31) {
        // Real R8 thread navigation: all parts, count/refs must not be overwritten.
        const uint16_t refs = s_app.state.rine.count;
        const uint16_t replied = s_app.state.rine.replied;
        const uint16_t windows = s_app.state.rine.windows;
        menu_row(1);                 // open phone
        CHECK(s_app.page == SG0_PAGE_PHONE, "R8 phone");
        click(BSP_BTN_OK);           // inbox
        CHECK(s_app.phone_view == SG0_PHONE_INBOX, "R8 inbox");
        int thread_index = -1;
        for (int i = 0; i < SG0_PHONE_KEY_MAX; ++i) {
            if (s_app.phone_thread_keys[i].base == base) { thread_index = i; break; }
        }
        CHECK(thread_index >= 0, "R8 thread base %u", base);
        s_app.phone_sel = thread_index;
        click(BSP_BTN_OK);
        CHECK(s_app.page == SG0_PAGE_PHONE_THREAD, "R8 thread open");
        const uint16_t count = s_app.phone_thread_count;
        CHECK(count > 0, "R8 thread parts %u", count);
        for (int k = 0; k < count; ++k) {
            click(BSP_BTN_DOWN);
            CHECK(s_app.phone_thread_count == count, "R8 count stable %d", k);
            CHECK(s_app.phone_thread_part == (uint16_t)((k + 1) % count),
                  "R8 part advances %d", k);
            CHECK(s_app.state.rine.count == refs && s_app.state.rine.replied == replied &&
                  s_app.state.rine.windows == windows, "R8 state preserved %d", k);
        }
        longp(BSP_BTN_OK);           // back to inbox
        longp(BSP_BTN_OK);           // back to phone home
        longp(BSP_BTN_OK);           // back to game
        CHECK(s_app.page == SG0_PAGE_GAME, "R8 back game");
    }
    char label[48];
    snprintf(label, sizeof(label), "coord %u@%u", node, cursor);
    printf("event-save: %s refs=%u event=%u/%u ok\n", label,
           (unsigned)before.rine.count, before.event_node, before.event_cursor);
}

static void write_raw_slot0(const uint8_t *raw, size_t len)
{
    nvs_handle_t h;
    CHECK(nvs_open("steins0", NVS_READWRITE, &h) == ESP_OK, "nvs open");
    CHECK(nvs_set_blob(h, "s00", raw, len) == ESP_OK, "nvs set s00");
    CHECK(nvs_commit(h) == ESP_OK, "nvs commit s00");
    nvs_close(h);
}

static size_t read_raw_slot0(uint8_t *raw, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("steins0", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(h, "s00", raw, &len);
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

static void test_v2_migration_and_rejection(void)
{
    install_reply_state(0, 243, 2304);
    const sg0_state_t before = s_app.state;
    save_slot0();
    uint8_t raw[SG0_SAVE_BYTES];
    size_t raw_len = read_raw_slot0(raw, sizeof(raw));
    CHECK(raw_len == SG0_SAVE_BYTES, "v3 raw length %u", (unsigned)raw_len);
    sg0_save_t save;
    CHECK(sg0_save_decode(&save, raw, raw_len), "decode v3");
    CHECK(save.event_valid && save.event_node == 0, "v3 event node");
    CHECK(save.event_cursor == before.event_cursor, "v3 event ordinal");

    // Convert the current record to a valid v2 (428 B, no event fields), with
    // a fresh core and extension CRC. This is the migration fixture.
    uint8_t v2[SG0_SAVE_V2_BYTES];
    memset(v2, 0, sizeof(v2));
    const uint16_t v2_payload = 64 + SG0_SAVE_RINE_BYTES * 2 + 7;
    memcpy(v2, raw, v2_payload);
    v2[2] = 2;
    const uint16_t core = sg0_crc16(v2, 62);
    v2[62] = (uint8_t)(core & 0xFFu);
    v2[63] = (uint8_t)(core >> 8);
    const uint16_t ext = sg0_crc16(v2 + 64, v2_payload - 64);
    v2[SG0_SAVE_V2_BYTES - 2] = (uint8_t)(ext & 0xFFu);
    v2[SG0_SAVE_V2_BYTES - 1] = (uint8_t)(ext >> 8);
    write_raw_slot0(v2, sizeof(v2));
    s_app.state.cursor++;          // must be restored from v2, not left live
    load_slot0();
    check_state_same(&before, &s_app.state, "v2 migration");
    CHECK(s_app.state.event_node == 0, "v2 derived event node");
    CHECK(s_app.state.event_cursor == before.event_cursor, "v2 derived ordinal");

    // Bad event node / ordinal are rejected by app preflight with no live change.
    sg0_save_t bad;
    CHECK(sg0_save_decode(&bad, raw, raw_len), "decode bad base");
    bad.event_node = 0xFF;
    uint8_t bad_raw[SG0_SAVE_BYTES];
    CHECK(sg0_save_encode(&bad, bad_raw, sizeof(bad_raw)) == SG0_SAVE_BYTES, "bad encode");
    write_raw_slot0(bad_raw, sizeof(bad_raw));
    load_slot0_must_fail();
    check_state_same(&before, &s_app.state, "bad event node");

    CHECK(sg0_save_decode(&bad, raw, raw_len), "decode bad ordinal base");
    sg0_node_t node;
    CHECK(sg0_pack_node(&s_app.pack, 0, &node), "node for ordinal");
    bad.event_cursor = (uint16_t)(node.event_count + 1);
    CHECK(sg0_save_encode(&bad, bad_raw, sizeof(bad_raw)) == SG0_SAVE_BYTES, "bad ordinal encode");
    write_raw_slot0(bad_raw, sizeof(bad_raw));
    load_slot0_must_fail();
    check_state_same(&before, &s_app.state, "bad event ordinal");

    // Bad CRC and version are rejected without changing state.
    memcpy(bad_raw, raw, raw_len);
    bad_raw[20] ^= 0x01;
    write_raw_slot0(bad_raw, raw_len);
    load_slot0_must_fail();
    check_state_same(&before, &s_app.state, "bad CRC");
    memcpy(bad_raw, raw, raw_len);
    bad_raw[2] = 99;
    write_raw_slot0(bad_raw, raw_len);
    load_slot0_must_fail();
    check_state_same(&before, &s_app.state, "bad version");
}

static void return_title(void)
{
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "menu for title");
    for (int i = 0; i < 8 && s_app.list_sel != 6; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "title confirm");
    click(BSP_BTN_UP);             // default is cancel; select return
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "title");
}

static void test_auto_continue(void)
{
    install_reply_state(0, 243, 2304);
    const sg0_state_t before = s_app.state;
    steins0_app_flush(&s_app);     // force the real auto save path
    return_title();                // also captures auto + unlock
    s_app.title_sel = 1;           // continue
    click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "auto continue game");
    check_state_same(&before, &s_app.state, "auto continue");
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    if (!fh) return 1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    s_blob = malloc((size_t)size);
    s_blob_size = (uint32_t)size;
    CHECK(fread(s_blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);

    boot();
    save_load_coordinate(0, 243, 2304);
    boot();
    save_load_coordinate(31, 730, 5376);
    boot();
    test_v2_migration_and_rejection();
    boot();
    test_auto_continue();
    boot();
    test_r9_thread_navigation();
    free(s_blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS app event save/load/continue: n0@243, n31@730, v2 migration, bad event/crc/version\n");
    return 0;
}
