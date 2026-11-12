#include "atri_app.h"
#include "bsp_button.h"
#include "nvs.h"
#include "fake_ui.h"
#include "senren_afterstory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return NULL; }
    fclose(f); *len = (uint32_t)n; return data;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void key(atri_app_t *app, uint8_t btn, uint8_t ev)
{
    atri_key_t k = { btn, ev };
    atri_app_key(app, &k);
}

static void click(atri_app_t *app, uint8_t btn) { key(app, btn, BSP_BTN_CLICK); }
static void long_press(atri_app_t *app, uint8_t btn) { key(app, btn, BSP_BTN_LONG); }

static void dismiss_warning(atri_app_t *app)
{
    if (atri_app_test_page(app) == ATRI_PAGE_WARNING) click(app, BSP_BTN_OK);
}

static void goto_title(atri_app_t *app)
{
    for (int guard = 0; guard < 10 && atri_app_test_page(app) != ATRI_PAGE_TITLE; ++guard) {
        if (atri_app_test_page(app) == ATRI_PAGE_ENDING ||
            atri_app_test_page(app) == ATRI_PAGE_WARNING) {
            click(app, BSP_BTN_OK);
        } else {
            break;
        }
    }
}

static int flag_index(const senren_scn_t *scn, uint32_t page)
{
    for (uint32_t i = 0; i < scn->flag_count; ++i) {
        if (read_u32(scn->flag_pages + 4u * i) == page) return (int)i;
    }
    return -1;
}

typedef struct { const char *ending; uint8_t values[8]; } route_t;

static const uint32_t route_flag_pages[8] = {109, 5504, 5912, 5926, 6390, 7337, 8418, 51359};
static const route_t routes[6] = {
    {"gameend_YoshinoEnd",  {1, 2, 1, 1, 1, 1, 1, 1}},
    {"gameend_MakoEnd",     {1, 2, 2, 1, 1, 1, 1, 1}},
    {"gameend_MurasameEnd", {1, 2, 3, 1, 2, 1, 1, 1}},
    {"gameend_RenaEnd",     {1, 1, 1, 2, 1, 1, 1, 1}},
    {"gameend_KoharuEnd",   {1, 2, 1, 1, 1, 2, 2, 1}},
    {"gameend_RokaEnd",     {1, 2, 1, 1, 1, 2, 2, 2}},
};

static int choose_for_route(const senren_player_t *p, const int idx[8], const uint8_t values[8])
{
    for (int option = 0; option < p->choice_count; ++option) {
        for (int f = 0; f < 8; ++f) {
            if (p->choice_flag[option] == idx[f] && p->choice_value[option] == values[f]) {
                return option;
            }
        }
    }
    return -1;
}

static bool play_route(atri_app_t *app, const senren_scn_t *scn, int route_no, bool require_auto)
{
    goto_title(app);
    while (atri_app_test_title_sel(app) != 0) click(app, BSP_BTN_UP);
    click(app, BSP_BTN_OK);
    CHECK(atri_app_test_page(app) == ATRI_PAGE_GAME);

    int idx[8];
    for (int i = 0; i < 8; ++i) idx[i] = flag_index(scn, route_flag_pages[i]);
    if (idx[0] < 0) return false;

    bool auto_seen = false;
    for (unsigned guard = 0; guard < 500000; ++guard) {
        atri_page_t page = atri_app_test_page(app);
        const senren_player_t *p = atri_app_test_player(app);
        if (page == ATRI_PAGE_ENDING) {
            char ending[SENREN_NAME_MAX];
            senren_scn_ending(scn, p->ending_index, ending, sizeof(ending));
            CHECK(strcmp(ending, routes[route_no].ending) == 0);
            if (require_auto) CHECK(auto_seen);
            click(app, BSP_BTN_OK);
            goto_title(app);
            printf("route %s: PASS\n", routes[route_no].ending);
            return true;
        }
        if (page == ATRI_PAGE_WARNING) { click(app, BSP_BTN_OK); continue; }
        if (page != ATRI_PAGE_GAME) {
            fprintf(stderr, "route %d unexpected page=%d title_sel=%d notice=%s\n",
                    route_no, (int)page, atri_app_test_title_sel(app), g_fake_ui.notice);
            CHECK(false); return false;
        }
        if (atri_app_test_at_choice(app)) {
            int pick = choose_for_route(p, idx, routes[route_no].values);
            if (pick < 0) pick = 0;   // common choices outside the eight route flags
            for (int i = 0; i < pick; ++i) click(app, BSP_BTN_DOWN);
            click(app, BSP_BTN_OK);
            if (!auto_seen) { senren_save_t save; auto_seen = senren_auto_load(&save); }
        } else {
            click(app, BSP_BTN_DOWN);
        }
    }
    CHECK(false);
    return false;
}

static void open_afterstory(atri_app_t *app, int index)
{
    goto_title(app);
    while (atri_app_test_title_sel(app) != 0) click(app, BSP_BTN_UP);
    for (int i = 0; i < 3; ++i) click(app, BSP_BTN_DOWN);
    click(app, BSP_BTN_OK);
    CHECK(atri_app_test_page(app) == ATRI_PAGE_AFTER);
    int selected = atri_app_test_after_sel(app);
    while (selected < index) { click(app, BSP_BTN_DOWN); ++selected; }
    while (selected > index) { click(app, BSP_BTN_UP); --selected; }
    click(app, BSP_BTN_OK);
}

static bool run_afterstory(atri_app_t *app, int index, int flip)
{
    const senren_afterstory_t *story = senren_afterstory_get(index);
    open_afterstory(app, index);
    CHECK(atri_app_test_page(app) == ATRI_PAGE_GAME);
    const senren_player_t *p = atri_app_test_player(app);
    CHECK(atri_app_test_afterstory_chunk(app) == story->chunk &&
          atri_app_test_afterstory_node(app) == story->node);
    unsigned selects = 0;
    for (unsigned guard = 0; guard < 500000; ++guard) {
        atri_page_t page = atri_app_test_page(app);
        p = atri_app_test_player(app);
        if (page == ATRI_PAGE_ENDING) {
            char ending[SENREN_NAME_MAX];
            senren_scn_ending(&app->scn, p->ending_index, ending, sizeof(ending));
            CHECK(strcmp(ending, "gameend_end") == 0);
            click(app, BSP_BTN_OK);
            goto_title(app);
            CHECK(selects == story->selects);
            printf("afterstory %s flip=%d: PASS (%u selects)\n", story->name, flip, selects);
            return true;
        }
        if (page == ATRI_PAGE_WARNING) { click(app, BSP_BTN_OK); continue; }
        if (page != ATRI_PAGE_GAME) { CHECK(false); return false; }
        if (atri_app_test_at_choice(app)) {
            CHECK(p->choice_count == 2);
            int pick = (flip == (int)selects) ? 1 : 0;
            if (pick) click(app, BSP_BTN_DOWN);
            click(app, BSP_BTN_OK);
            ++selects;
        } else {
            click(app, BSP_BTN_DOWN);
        }
    }
    CHECK(false);
    return false;
}

static bool app_init(atri_app_t *app, uint8_t *pack, uint32_t pack_len, uint8_t *scn, uint32_t scn_len)
{
    memset(app, 0, sizeof(*app));
    static uint16_t art[ATRI_UI_W * ATRI_UI_H];
    if (!atri_app_init(app, pack, pack_len, scn, scn_len, art, &lv_font_montserrat_14)) {
        return false;
    }
    dismiss_warning(app);
    return true;
}

static void test_locked_menu(uint8_t *pack, uint32_t pack_len, uint8_t *scn, uint32_t scn_len)
{
    fake_nvs_reset();
    atri_app_t app;
    if (!app_init(&app, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    goto_title(&app);
    while (atri_app_test_title_sel(&app) != 0) click(&app, BSP_BTN_UP);
    for (int i = 0; i < 3; ++i) click(&app, BSP_BTN_DOWN);
    click(&app, BSP_BTN_OK);
    CHECK(atri_app_test_page(&app) == ATRI_PAGE_AFTER);
    for (int i = 0; i < SENREN_AFTERSTORY_COUNT; ++i) {
        CHECK(strcmp(g_fake_ui.list_values[ATRI_LIST_AFTER][i], "未解锁") == 0);
    }
    click(&app, BSP_BTN_OK);
    CHECK(atri_app_test_page(&app) == ATRI_PAGE_AFTER);
    CHECK(strstr(g_fake_ui.notice, "尚未通关") != NULL);
    long_press(&app, BSP_BTN_OK);
}

static void retry_case(uint8_t *pack, uint32_t pack_len, uint8_t *scn, uint32_t scn_len,
                       bool fail_set, const char *label)
{
    fake_nvs_reset();
    atri_app_t app;
    if (!app_init(&app, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    if (fail_set) fake_nvs_fail_set(1); else fake_nvs_fail_commit(1);
    CHECK(play_route(&app, &app.scn, 0, false));
    CHECK((atri_app_test_clears(&app) & 1u) != 0);
    CHECK(atri_app_test_clears_dirty(&app));
    CHECK(!fake_nvs_has("clr"));
    fake_nvs_fail_set(0);
    fake_nvs_fail_commit(0);
    atri_app_tick(&app, 6000);
    CHECK(!atri_app_test_clears_dirty(&app));
    CHECK(fake_nvs_has("clr"));
    printf("clears retry after injected %s failure: PASS\n", label);

    atri_app_t reloaded;
    if (!app_init(&reloaded, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    CHECK((atri_app_test_clears(&reloaded) & 1u) != 0);
    goto_title(&reloaded);
    while (atri_app_test_title_sel(&reloaded) != 0) click(&reloaded, BSP_BTN_UP);
    for (int i = 0; i < 3; ++i) click(&reloaded, BSP_BTN_DOWN);
    click(&reloaded, BSP_BTN_OK);
    CHECK(atri_app_test_page(&reloaded) == ATRI_PAGE_AFTER);
    CHECK(strcmp(g_fake_ui.list_values[ATRI_LIST_AFTER][0], "已解锁") == 0);
}

static void test_full_six_and_afterstory(uint8_t *pack, uint32_t pack_len, uint8_t *scn, uint32_t scn_len)
{
    fake_nvs_reset();
    atri_app_t app;
    if (!app_init(&app, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    for (int i = 0; i < 6; ++i) {
        CHECK(play_route(&app, &app.scn, i, true));
        CHECK((atri_app_test_clears(&app) & (1u << i)) != 0);
    }
    CHECK(atri_app_test_clears(&app) == 0x3Fu);

    atri_app_t reloaded;
    if (!app_init(&reloaded, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    CHECK(atri_app_test_clears(&reloaded) == 0x3Fu);
    goto_title(&reloaded);
    while (atri_app_test_title_sel(&reloaded) != 0) click(&reloaded, BSP_BTN_UP);
    for (int i = 0; i < 3; ++i) click(&reloaded, BSP_BTN_DOWN);
    click(&reloaded, BSP_BTN_OK);
    CHECK(atri_app_test_page(&reloaded) == ATRI_PAGE_AFTER);
    for (int i = 0; i < SENREN_AFTERSTORY_COUNT; ++i) {
        CHECK(strcmp(g_fake_ui.list_values[ATRI_LIST_AFTER][i], "已解锁") == 0);
    }
    long_press(&reloaded, BSP_BTN_OK);

    for (int i = 0; i < SENREN_AFTERSTORY_COUNT; ++i) {
        const senren_afterstory_t *story = senren_afterstory_get(i);
        CHECK(run_afterstory(&reloaded, i, -1));
        for (unsigned ordinal = 0; ordinal < story->selects; ++ordinal) {
            CHECK(run_afterstory(&reloaded, i, (int)ordinal));
        }
    }

    senren_save_t save;
    memset(&save, 0, sizeof(save));
    save.chunk = 0;
    save.node = 0;
    save.chapter = 0;
    CHECK(senren_auto_store(&save));
    atri_app_t saved_app;
    if (!app_init(&saved_app, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    goto_title(&saved_app);
    while (atri_app_test_title_sel(&saved_app) != 0) click(&saved_app, BSP_BTN_UP);
    click(&saved_app, BSP_BTN_DOWN);
    click(&saved_app, BSP_BTN_OK);
    CHECK(atri_app_test_page(&saved_app) == ATRI_PAGE_GAME);

    senren_settings_t settings;
    CHECK(senren_settings_load(&settings));
    settings.text_speed = 2;
    CHECK(senren_settings_store(&settings));
    atri_app_t settings_app;
    if (!app_init(&settings_app, pack, pack_len, scn, scn_len)) { CHECK(false); return; }
    CHECK(atri_app_test_settings(&settings_app)->text_speed == 2);
}

int main(int argc, char **argv)
{
    const char *pack_path = argc > 1 ? argv[1] : "main/senren_data/senren_pack.bin";
    const char *scn_path = argc > 2 ? argv[2] : "main/senren_data/senren_scn.bin";
    uint32_t pack_len = 0, scn_len = 0;
    uint8_t *pack = read_file(pack_path, &pack_len);
    uint8_t *scn = read_file(scn_path, &scn_len);
    CHECK(pack != NULL && scn != NULL);
    if (!pack || !scn) return 1;

    test_locked_menu(pack, pack_len, scn, scn_len);
    if (!failures) retry_case(pack, pack_len, scn, scn_len, true, "nvs_set");
    if (!failures) retry_case(pack, pack_len, scn, scn_len, false, "nvs_commit");
    if (!failures) test_full_six_and_afterstory(pack, pack_len, scn, scn_len);

    free(pack); free(scn);
    printf("senren_app: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
