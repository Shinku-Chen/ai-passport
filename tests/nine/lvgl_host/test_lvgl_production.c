// Real 32-bit LVGL production-path test: real screen, nine_image (no
// NINE_IMAGE_HOST), real gal_ui, real 16/20px fonts, real pack, 32 KiB pool.
// Captures PPM pages and pool stats; converts to PNG with the Python helper.
#include "nine_app.h"
#include "nine_image.h"
#include "nine_media.h"
#include "nine_assets.h"
#include "nine_assets_host.h"
#include "gal_ui.h"
#include "bsp_button.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nine_nvs_host_reset(void);
void nine_store_test_clear(void);
int nine_assets_host_load(const char *script, const char *images);
void host_heap_fail_next(int count);
int host_heap_alloc_count(void);

#define W 320
#define H 240
static uint8_t s_draw[W * H * 2];
static uint16_t s_fb[W * H];
static nine_app_t s_app;
static nine_app_t *s_current = &s_app;
static int s_failures;
static int s_min_free = 1 << 30;
static int s_max_used;
static int s_largest = -1;
static int s_min_biggest = 1 << 30;
static int s_page_no;
static size_t s_canvas_bytes;
static FILE *s_manifest;

#define CHECK(c, m) do { if (!(c)) { ++s_failures; printf("FAIL: %s\n", m); } } while (0)

typedef struct { uint16_t node; uint32_t index; uint16_t page; int kind; } pos_t;
static pos_t pos(const nine_app_t *a)
{
    pos_t p = { a->vm.state.node, a->vm.state.index, a->text_page, (int)a->vm.view.kind };
    return p;
}
static bool same(pos_t a, pos_t b)
{
    return a.node == b.node && a.index == b.index && a.page == b.page && a.kind == b.kind;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static bool make_bad_images(const char *path)
{
    size_t size = 0;
    const uint8_t *src = nine_assets_images(&size);
    if (!src || size < 128) return false;
    uint8_t *bad = (uint8_t *)malloc(size);
    if (!bad) return false;
    memcpy(bad, src, size);
    const uint32_t table = rd32(bad + 24);
    const uint32_t off = rd32(bad + table + 12);
    const uint32_t len = rd32(bad + table + 16);
    if (off + len > size || len < 8) { free(bad); return false; }
    bad[off + 2] ^= 0x40;
    wr32(bad + table + 20, crc32_bytes(bad + off, len));
    wr32(bad + 16, 0);
    wr32(bad + 16, crc32_bytes(bad, size));
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(bad, 1, size, f) == size;
    if (f) fclose(f);
    free(bad);
    return ok;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    static int n;
    if (n < 8) { printf("flush %d %d,%d-%d,%d\n", n, area->x1, area->y1, area->x2, area->y2); ++n; }
    const uint16_t *src = (const uint16_t *)px;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    for (int y = 0; y < h; ++y) {
        int dy = area->y1 + y;
        if (dy < 0 || dy >= H) continue;
        for (int x = 0; x < w; ++x) {
            int dx = area->x1 + x;
            if (dx >= 0 && dx < W) s_fb[dy * W + dx] = src[y * w + x];
        }
    }
    lv_display_flush_ready(disp);
}

static void key(nine_app_t *a, int btn, int ev)
{
    nine_key_t k = { (uint8_t)btn, (uint8_t)ev };
    nine_app_key(a, &k);
}
static void click(nine_app_t *a, int btn) { key(a, btn, BSP_BTN_CLICK); }
static void new_click(nine_app_t *a, int btn) { key(a, btn, BSP_BTN_PRESS); click(a, btn); }
static void long_press(nine_app_t *a, int btn)
{
    key(a, btn, BSP_BTN_LONG);
    key(a, btn, BSP_BTN_RELEASE);
}

static void write_ppm(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "build/evidence/lvgl-pages/%03d-%s.ppm", ++s_page_no, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); ++s_failures; return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        uint16_t p = s_fb[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31),
                           (uint8_t)(((p >> 5) & 63) * 255 / 63),
                           (uint8_t)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if ((int)mon.free_size < s_min_free) s_min_free = (int)mon.free_size;
    if ((int)mon.used_pct > s_max_used) s_max_used = (int)mon.used_pct;
    if ((int)mon.free_biggest_size > s_largest) s_largest = (int)mon.free_biggest_size;
    if ((int)mon.free_biggest_size < s_min_biggest) s_min_biggest = (int)mon.free_biggest_size;
    fprintf(s_manifest, "%s page=%d node=%u idx=%u free=%u biggest=%u used=%u%%\n",
            name, (int)s_current->page, s_current->vm.state.node,
            (unsigned)s_current->vm.state.index,
            (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
            (unsigned)mon.used_pct);
}

static void capture(const char *name)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("capture %s: free=%u biggest=%u used=%u%% timer\n", name,
           (unsigned)m.free_size, (unsigned)m.free_biggest_size, (unsigned)m.used_pct);
    lv_timer_handler();
    printf("capture %s: refr\n", name);
    lv_refr_now(NULL);
    printf("capture %s: write\n", name);
    write_ppm(name);
}

static void to_title(nine_app_t *a)
{
    if (a->page == GAL_PAGE_START) click(a, BSP_BTN_OK);
    if (a->page == GAL_PAGE_TITLE) return;
    if (a->page == GAL_PAGE_CHAPTER) long_press(a, BSP_BTN_OK);
    if (a->page == GAL_PAGE_GALLERY) long_press(a, BSP_BTN_OK);
    if (a->page == GAL_PAGE_GAME || a->page == GAL_PAGE_CHOICE) long_press(a, BSP_BTN_OK);
    if (a->page == GAL_PAGE_MENU) {
        a->menu_sel = 5;
        new_click(a, BSP_BTN_OK);
    }
}

static void new_game(nine_app_t *a)
{
    to_title(a);
    a->title_sel = 0;
    new_click(a, BSP_BTN_OK);
}

static bool ensure_text(nine_app_t *a)
{
    for (int i = 0; i < 400; ++i) {
        if (a->page == GAL_PAGE_GAME) return true;
        if (a->page == GAL_PAGE_CHOICE) { click(a, BSP_BTN_OK); continue; }
        return false;
    }
    return false;
}

static void exercise_gallery_op(nine_app_t *a)
{
    to_title(a);
    a->title_sel = 4;
    new_click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_GALLERY, "OP gallery entry");
    a->gallery_sel = 3;
    new_click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_CG && a->opening && a->op_gallery,
          "OP gallery playback starts");
    uint32_t ticks = 0;
    uint32_t first = 0, mid = 0, last = 0;
    while (a->opening && ticks++ < 1200) {
        nine_media_test_advance(100);
        nine_app_tick(a, 0);
        if (a->op_frames_drawn == 1) first = a->op_image;
        if (a->op_frames_drawn == 220) mid = a->op_image;
        if (a->op_frames_drawn == 440) last = a->op_image;
    }
    CHECK(!a->opening && a->page == GAL_PAGE_GALLERY,
          "OP gallery natural end");
    CHECK(a->op_frames_drawn > 100, "OP gallery real frames drawn");
    CHECK(first && mid && last && first != mid && mid != last,
          "OP gallery first/mid/last frames are distinct");
    CHECK(a->op_late_skips == 0, "OP gallery regular clock no skips");
    printf("OP gallery frames=%u max_decode_us=%u late=%u ticks=%u\n",
           (unsigned)a->op_frames_drawn, (unsigned)a->op_max_decode_us,
           (unsigned)a->op_late_skips, (unsigned)ticks);
    to_title(a);
}

static void capture_gallery_pages(nine_app_t *a)
{
    to_title(a);
    a->title_sel = 4;
    new_click(a, BSP_BTN_OK);
    capture("gallery");
    a->gallery_sel = 0;
    new_click(a, BSP_BTN_OK);
    capture("cg-list");
    click(a, BSP_BTN_OK);
    capture("cg-viewer");
    click(a, BSP_BTN_OK);
    long_press(a, BSP_BTN_OK);
    a->gallery_sel = 3;
    new_click(a, BSP_BTN_OK);
    capture("gallery-op");
    click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_GALLERY, "OP gallery skip returns");
    a->gallery_sel = 2;
    new_click(a, BSP_BTN_OK);
    capture("endings");
    long_press(a, BSP_BTN_OK);
    a->gallery_sel = 1;
    new_click(a, BSP_BTN_OK);
    capture("chapters");
}

static bool rects_overlap(lv_obj_t *a, lv_obj_t *b)
{
    if (!a || !b || lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) return false;
    const int ax = lv_obj_get_x(a), ay = lv_obj_get_y(a);
    const int bx = lv_obj_get_x(b), by = lv_obj_get_y(b);
    const int aw = lv_obj_get_width(a), ah = lv_obj_get_height(a);
    const int bw = lv_obj_get_width(b), bh = lv_obj_get_height(b);
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void check_name_geometry(const char *label, int expect_max)
{
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t *name = s_app.ui.name;
    CHECK(name != NULL, "name object exists");
    if (!name) return;
    const int x = lv_obj_get_x(name);
    const int y = lv_obj_get_y(name);
    const int w = lv_obj_get_width(name);
    const int h = lv_obj_get_height(name);
    CHECK(!lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN), "name visible");
    CHECK(x == 8, "name x=8");
    CHECK(y >= 0 && y + h <= 179, "name bottom <=179 and glyphs on screen");
    CHECK(h >= lv_font_get_line_height(s_app.ui.font_small) + 4,
          "name height keeps real glyph line + padding");
    CHECK(w > 0 && w <= GAL_NAME_MAX_W, "name width <=160");
    if (expect_max) CHECK(w == GAL_NAME_MAX_W, "long name is capped at 160");
    CHECK(lv_obj_get_style_bg_opa(name, LV_PART_MAIN) > 0, "name has compact background");
    printf("layout %s: x=%d y=%d w=%d h=%d\n", label, x, y, w, h);
}

static void test_reading_layout_contract(void)
{
    gal_ui_t *ui = &s_app.ui;
    CHECK(GAL_PANEL_H == 60 && GAL_PANEL_Y == 180, "reading panel y180..239");
    CHECK(GAL_TEXT_LINES_SMALL == 3 && GAL_TEXT_LINES_LARGE == 2,
          "reading 3 small / 2 large lines");
    CHECK(NINE_IMAGE_W == 320 && NINE_IMAGE_H == 180 && NINE_IMAGE_BAND_H == 36,
          "full 320x180 art bands, no 170px crop");
    CHECK(nine_image_canvas_bytes() == 115200, "five bounded 320x36 bands");
    CHECK(lv_obj_get_y(ui->panel) == 180 && lv_obj_get_height(ui->panel) == 60,
          "panel exact bounds");
    CHECK(lv_obj_get_y(ui->text) == GAL_TEXT_Y, "body starts at panel top");
    // Geometry is stable only on the active page; the game page is built but
    // hidden at boot. Show it for the production layout assertions.
    gal_ui_show_page(ui, GAL_PAGE_GAME);

    gal_ui_set_text(ui, "小明", "短名测试", 0, NULL, NULL);
    check_name_geometry("short", 0);
    gal_ui_set_mode(ui, true);
    gal_ui_set_notice(ui, "提示");
    lv_obj_update_layout(lv_screen_active());
    CHECK(!rects_overlap(ui->name, ui->mode_auto), "name disjoint from Auto");
    CHECK(!rects_overlap(ui->name, ui->notice), "name disjoint from notice");
    gal_ui_set_mode(ui, false);
    gal_ui_set_notice(ui, NULL);

    char long_name[160];
    int n = 0;
    for (int i = 0; i < 40 && n + 3 < (int)sizeof(long_name); ++i) {
        long_name[n++] = (char)0xE9;
        long_name[n++] = (char)0x95;
        long_name[n++] = (char)0xBF;
    }
    long_name[n] = '\0';
    gal_ui_set_text(ui, long_name, "长名测试", 0, NULL, NULL);
    check_name_geometry("long", 1);

    gal_ui_set_text(ui, NULL, "旁白测试", 0, NULL, NULL);
    CHECK(lv_obj_has_flag(ui->name, LV_OBJ_FLAG_HIDDEN),
          "empty name hides label and its background");
    CHECK(!lv_obj_has_flag(ui->text, LV_OBJ_FLAG_HIDDEN), "body remains visible");

    gal_ui_set_font_size(ui, false);
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_height(ui->text) ==
              (lv_font_get_line_height(ui->font_small) + 1) * GAL_TEXT_LINES_SMALL - 1,
          "small body uses real 3-line metrics");
    gal_ui_set_font_size(ui, true);
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_height(ui->text) ==
              (lv_font_get_line_height(ui->font_large) + 1) * GAL_TEXT_LINES_LARGE - 1,
          "large body uses real 2-line metrics");
    CHECK(lv_obj_get_style_text_font(ui->name, LV_PART_MAIN) == ui->font_small,
          "name keeps 16px tier on font switch");
    gal_ui_set_font_size(ui, false);
    gal_ui_set_text(ui, NULL, "", 0, NULL, NULL);
    gal_ui_show_page(ui, GAL_PAGE_START);
}

int main(void)
{
    setbuf(stdout, NULL);
    nine_nvs_host_reset();
    nine_store_test_clear();
    CHECK(nine_assets_host_load("assets/nine/yukiiro/nine_script.bin",
                                "assets/nine/yukiiro/nine_images.bin"),
          "assets load");
    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    CHECK(disp != NULL, "display create");
    lv_display_set_buffers(disp, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);

    FILE *old = NULL;
    (void)old;
    s_manifest = fopen("build/evidence/lvgl-pages/manifest.txt", "w");
    CHECK(s_manifest != NULL, "manifest file");
    if (!s_manifest) return 1;

    CHECK(nine_app_init(&s_app), "production app init");
    CHECK(!s_app.fatal && s_app.image_ok, "production image layer ready");
    s_canvas_bytes = nine_image_canvas_bytes();
    test_reading_layout_contract();
    s_app.cfg.display.text_speed = 3;
    printf("init done\n");
    capture("start");
    printf("start captured\n");
    click(&s_app, BSP_BTN_OK);
    printf("title entered page=%d\n", (int)s_app.page);
    capture("title");

    new_game(&s_app);
    CHECK(s_app.page == GAL_PAGE_GAME, "new game reader");
    capture("game");

    for (int i = 0; i < 2000 && s_app.page != GAL_PAGE_CHOICE; ++i) {
        if (s_app.page != GAL_PAGE_GAME) break;
        click(&s_app, BSP_BTN_OK);
    }
    CHECK(s_app.page == GAL_PAGE_CHOICE, "choice page reached");
    capture("choice");

    long_press(&s_app, BSP_BTN_OK);
    capture("menu");
    s_app.menu_sel = 1;
    new_click(&s_app, BSP_BTN_OK);
    capture("save");
    long_press(&s_app, BSP_BTN_OK);
    s_app.menu_sel = 2;
    new_click(&s_app, BSP_BTN_OK);
    capture("load");
    long_press(&s_app, BSP_BTN_OK);
    s_app.menu_sel = 4;
    new_click(&s_app, BSP_BTN_OK);
    capture("settings");
    long_press(&s_app, BSP_BTN_OK);
    s_app.menu_sel = 3;
    new_click(&s_app, BSP_BTN_OK);
    capture("history");
    long_press(&s_app, BSP_BTN_OK);
    s_app.menu_sel = 0;
    new_click(&s_app, BSP_BTN_OK);

    capture_gallery_pages(&s_app);
    exercise_gallery_op(&s_app);

    new_game(&s_app);
    int steps = 0;
    while (s_app.page != GAL_PAGE_ENDING && steps++ < 30000) {
        if (s_app.page == GAL_PAGE_GAME || s_app.page == GAL_PAGE_CHOICE) click(&s_app, BSP_BTN_OK);
        else break;
    }
    CHECK(s_app.page == GAL_PAGE_ENDING, "ending page reached");
    capture("ending");

    // Real bad JPEG with recalculated CRC: bind succeeds, decode must fail and
    // must not allow the cursor/progress to move.
    nine_image_deinit();
    nine_media_deinit();
    lv_obj_clean(lv_screen_active());
    nine_store_test_clear();
    CHECK(make_bad_images("build/evidence/nine_images_bad.bin"), "bad jpeg generated");
    CHECK(nine_assets_host_load("assets/nine/yukiiro/nine_script.bin",
                                "build/evidence/nine_images_bad.bin"), "bad images load");
    nine_app_t bad;
    s_current = &bad;
    CHECK(nine_app_init(&bad), "bad jpeg app init (bind is CRC-valid)");
    CHECK(bad.image_ok && !bad.fatal, "bad jpeg bind accepted");
    bad.cfg.display.text_speed = 3;
    click(&bad, BSP_BTN_OK);
    new_game(&bad);
    CHECK(bad.page == GAL_PAGE_GAME, "bad jpeg reader");
    CHECK(bad.resource_error && bad.image_error, "bad jpeg visible frame error");
    CHECK(!bad.progress_dirty, "bad jpeg frame not marked dirty");
    capture("bad-jpeg");
    const pos_t badpos = pos(&bad);
    click(&bad, BSP_BTN_OK);
    CHECK(same(badpos, pos(&bad)), "bad jpeg retry does not advance");
    capture("bad-jpeg-retry");

    // Real allocation failure: must show a CJK error page, not a text reader.
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    // Fail the second band allocation: one band + its LVGL object already exist.
    host_heap_fail_next(2);
    nine_app_t err;
    s_current = &err;
    CHECK(nine_app_init(&err), "error app init returns");
    CHECK(err.fatal && err.resource_fatal && !err.image_ok, "allocation failure unreadable");
    CHECK(nine_image_canvas_bytes() == 0, "partial band allocation released");
    CHECK(strstr(err.error, "图像") != NULL, "allocation failure CJK error");
    capture("error-page");
    host_heap_fail_next(0);

    fprintf(s_manifest,
            "SUMMARY min_free=%d max_used=%d lvgl_free_biggest=%d "
            "min_lvgl_biggest=%d pool=%u band_bytes=%u canvas_bytes=%u\n",
            s_min_free, s_max_used, s_largest, s_min_biggest, (unsigned)LV_MEM_SIZE,
            (unsigned)(NINE_IMAGE_W * NINE_IMAGE_BAND_H * 2),
            (unsigned)s_canvas_bytes);
    fclose(s_manifest);
    printf("lvgl pages=%d min_free=%d max_used=%d biggest=%d min_biggest=%d failures=%d\n",
           s_page_no, s_min_free, s_max_used, s_largest, s_min_biggest, s_failures);
    nine_image_deinit();
    nine_media_deinit();
    return s_failures ? 1 : 0;
}
