// tests/ui_smoke/smoke.c —— 生产 cafe_ui + 真实 LVGL 软件显示的横屏 smoke。
#include "cafe_app.h"
#include "cafe_ui.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
static uint16_t s_fb[W * H];
static uint16_t s_draw[W * 30];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    for (int32_t y = 0; y < h; ++y) {
        const int32_t dy = area->y1 + y;
        if (dy < 0 || dy >= H) continue;
        const int32_t dx = area->x1;
        if (dx < 0 || dx + w > W) continue;
        memcpy(&s_fb[dy * W + dx], px + (size_t)y * w * 2, (size_t)w * 2);
    }
    lv_display_flush_ready(disp);
}

static void snapshot(const char *name)
{
    lv_refr_now(NULL);
    char path[384];
    const char *out = getenv("CAFE_UI_OUT");
    if (!out || !*out) out = "reports/cafestella-media-20261123-1130/ui-smoke";
    snprintf(path, sizeof(path), "%s/%s.ppm", out, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        const uint16_t c = s_fb[i];
        const unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 31) * 255 / 31),
            (unsigned char)(((c >> 5) & 63) * 255 / 63),
            (unsigned char)((c & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 19) & 0x1F) << 11 | ((c >> 10) & 0x3F) << 5 | ((c >> 3) & 0x1F));
}

static int count_area_color(const lv_area_t *a, uint16_t want, int tol)
{
    int n = 0;
    for (int y = a->y1; y <= a->y2; ++y) {
        if (y < 0 || y >= H) continue;
        for (int x = a->x1; x <= a->x2; ++x) {
            if (x < 0 || x >= W) continue;
            const uint16_t c = s_fb[y * W + x];
            const int dr = abs((int)((c >> 11) & 31) - (int)((want >> 11) & 31));
            const int dg = abs((int)((c >> 5) & 63) - (int)((want >> 5) & 63));
            const int db = abs((int)(c & 31) - (int)(want & 31));
            if (dr <= tol && dg <= tol && db <= tol) n++;
        }
    }
    return n;
}

static void layout_checks(cafe_ui_t *ui)
{
    lv_area_t pa;
    lv_obj_get_coords(ui->panel, &pa);
    if (pa.x1 != 0 || pa.y1 != 180 || pa.x2 != 319 || pa.y2 != 239) {
        fputs("panel bounds wrong", stderr); exit(2);
    }
    cafe_ui_show_page(ui, CAFE_PAGE_GAME);
    cafe_ui_art_visible(ui, true);
    cafe_ui_set_font_large(ui, false);
    cafe_ui_set_mode(ui, false);
    cafe_ui_set_notice(ui, "");
    cafe_ui_set_text(ui, "长名字测试超过宽度应当省略",
                     "第一行完整文字\n第二行完整文字\n第三行完整文字", 0);
    cafe_ui_set_chapter(ui, "");
    lv_refr_now(NULL);
    snapshot("layout-16");
    lv_area_t ta, na, aa;
    lv_obj_get_coords(ui->text, &ta);
    lv_obj_get_coords(ui->name, &na);
    lv_obj_get_coords(ui->auto_label, &aa);
    if (ta.y2 > 239 || ta.y1 < 180 || ta.y2 - ta.y1 + 1 > 59) {
        fprintf(stderr, "16px text out of panel y=%d..%d x=%d..%d", ta.y1, ta.y2, ta.x1, ta.x2); exit(2);
    }
    if (strcmp(lv_label_get_text(ui->text),
               "第一行完整文字\n第二行完整文字\n第三行完整文字") != 0) {
        fputs("16px text lost content", stderr); exit(2);
    }
    if (count_area_color(&ta, rgb565(0xF6EFE8), 4) < 20) {
        fputs("16px text glyph pixels missing", stderr); exit(2);
    }
    if (na.y2 >= 180 || na.x2 > 238) {
        fprintf(stderr, "name escaped %d..%d x%d..%d h=%d y=%d selfh=%d text=[%s]", na.y1, na.y2, na.x1, na.x2, lv_obj_get_height(ui->name), lv_obj_get_y(ui->name), (int)lv_obj_get_self_height(ui->name), lv_label_get_text(ui->name)); exit(2);
    }
    if (lv_label_get_long_mode(ui->name) != LV_LABEL_LONG_DOT) {
        fputs("long name is not elided", stderr); exit(2);
    }
    // Short names must shrink the background to actual glyph width + padding.
    cafe_ui_set_text(ui, "栞", "短名", 0);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->name, &na);
    const int short_w = na.x2 - na.x1 + 1;
    if (short_w >= 100 || short_w < 24 ||
        count_area_color(&na, rgb565(0xF1C9A5), 5) < 5) {
        fputs("short name background not fitted", stderr); exit(2);
    }
    cafe_ui_set_text(ui, "Kanna", "Latin", 0);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->name, &na);
    if (na.x2 - na.x1 + 1 >= 150 ||
        count_area_color(&na, rgb565(0xF1C9A5), 5) < 3) {
        fputs("Latin name background too wide", stderr); exit(2);
    }
    cafe_ui_set_text(ui, "長い名前とEnglish混在", "mixed", 0);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->name, &na);
    if (na.x2 - na.x1 + 1 > 230 || lv_label_get_long_mode(ui->name) != LV_LABEL_LONG_DOT) {
        fputs("long mixed name not capped", stderr); exit(2);
    }
    cafe_ui_set_text(ui, "", "empty", 0);
    lv_refr_now(NULL);
    if (lv_obj_is_visible(ui->name)) { fputs("empty name still visible", stderr); exit(2); }
    cafe_ui_set_text(ui, "栞", "font switch", 0);
    cafe_ui_set_font_large(ui, true);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->name, &na);
    if (na.x2 - na.x1 + 1 > 230 || na.y2 >= 180) { fputs("font switch name bounds", stderr); exit(2); }
    cafe_ui_set_font_large(ui, false);
    cafe_ui_set_text(ui, "长名字测试超过宽度应当省略", "第一行完整文字\n第二行完整文字\n第三行完整文字", 0);
    lv_refr_now(NULL);
    cafe_ui_set_mode(ui, true);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->auto_label, &aa);
    if (!lv_obj_is_visible(ui->auto_label) || aa.x1 < 264 || aa.y1 < 154 ||
        aa.x2 > 311 || aa.y2 > 177 || aa.y2 >= 180) {
        fputs("auto slot wrong/not visible", stderr); exit(2);
    }
    if (strcmp(lv_label_get_text(ui->auto_label), "自动") != 0 ||
        count_area_color(&aa, rgb565(0xE7B98F), 5) < 10 || aa.x1 <= na.x2) {
        fputs("auto glyph pixels missing/name collision", stderr); exit(2);
    }
    snapshot("layout-auto");
    cafe_ui_set_mode(ui, false);
    cafe_ui_show_page(ui, CAFE_PAGE_CHOICE);
    lv_refr_now(NULL);
    if (lv_obj_is_visible(ui->auto_label)) {
        fputs("auto badge visible on CHOICE", stderr); exit(2);
    }
    cafe_ui_show_page(ui, CAFE_PAGE_MENU);
    lv_refr_now(NULL);
    if (lv_obj_is_visible(ui->auto_label)) { fputs("auto badge visible on MENU", stderr); exit(2); }
    cafe_ui_show_page(ui, CAFE_PAGE_TITLE);
    lv_refr_now(NULL);
    if (lv_obj_is_visible(ui->auto_label)) { fputs("auto badge visible on TITLE", stderr); exit(2); }
    cafe_ui_show_page(ui, CAFE_PAGE_GAME);
    cafe_ui_set_mode(ui, false);
    cafe_ui_set_text(ui, "", "", 0);
    cafe_ui_set_font_large(ui, true);
    cafe_ui_set_text(ui, "名字", "第一行\n第二行", 0);
    lv_refr_now(NULL);
    lv_obj_get_coords(ui->text, &ta);
    if (ta.y2 > 239 || ta.y1 < 180 || ta.y2 - ta.y1 + 1 > 48) {
        fputs("20px text out of panel", stderr); exit(2);
    }
    if (count_area_color(&ta, rgb565(0xF6EFE8), 4) < 20) {
        fputs("20px text glyph pixels missing", stderr); exit(2);
    }
    snapshot("layout-20");
    const uint16_t fill = rgb565(0x2468AC);
    for (int b = 0; b < CAFE_BAND_COUNT; ++b)
        for (int i = 0; i < CAFE_BAND_W * CAFE_BAND_H; ++i)
            if (ui->bands && ui->bands->pixels[b]) ui->bands->pixels[b][i] = fill;
    // Hide all text overlays for this pure stage-color seam check; panel remains
    // in its own 180..239 region.
    lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->chapter, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->auto_label, LV_OBJ_FLAG_HIDDEN);
    cafe_ui_art_visible(ui, true);
    cafe_ui_art_invalidate(ui);
    lv_refr_now(NULL);
    snapshot("layout-seam");
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            const uint16_t c = s_fb[y * W + x];
            if (abs((int)((c >> 11) & 31) - (int)((fill >> 11) & 31)) > 2 ||
                abs((int)((c >> 5) & 63) - (int)((fill >> 5) & 63)) > 2 ||
                abs((int)(c & 31) - (int)(fill & 31)) > 2) {
                fputs("image seam/gap", stderr); exit(2);
            }
        }
}

static void tap(cafe_app_t *app, uint8_t key, uint32_t t)
{
    cafe_app_key(app, key, true, t);
    cafe_app_key(app, key, false, t + 40);
}

static void hold(cafe_app_t *app, uint8_t key, uint32_t t)
{
    cafe_app_key(app, key, true, t);
    cafe_app_tick(app, t + 520, 520);
}

static void assert_op_no_overlay(cafe_ui_t *ui)
{
    lv_obj_update_layout(lv_screen_active());
    if (!lv_obj_has_flag(ui->transition_eyebrow, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(ui->transition_title, LV_OBJ_FLAG_HIDDEN)) {
        fputs("OP overlay title still visible", stderr); exit(2);
    }
    if (lv_obj_has_flag(ui->transition_sub, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_y(ui->transition_sub) < 180 ||
        lv_obj_get_y(ui->transition_sub) + lv_obj_get_height(ui->transition_sub) > 240) {
        fprintf(stderr, "OP hint y=%d h=%d hidden=%d ", (int)lv_obj_get_y(ui->transition_sub), (int)lv_obj_get_height(ui->transition_sub), (int)lv_obj_has_flag(ui->transition_sub, LV_OBJ_FLAG_HIDDEN));
        fputs("OP control hint outside bottom strip", stderr); exit(2);
    }
    if (lv_obj_get_style_bg_opa(ui->pages[CAFE_PAGE_TRANSITION], LV_PART_MAIN) != LV_OPA_TRANSP) {
        fputs("OP transition parent not transparent", stderr); exit(2);
    }
}

static void transition_modes_check(cafe_ui_t *ui)
{
    cafe_ui_set_transition(ui, "CHAPTER", "章节标题", "短按 OK 开始试读");
    if (lv_obj_has_flag(ui->transition_eyebrow, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(ui->transition_title, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_y(ui->transition_title) >= 180) {
        fputs("normal chapter transition hidden/misplaced", stderr); exit(2);
    }
    cafe_ui_show_page(ui, CAFE_PAGE_TRANSITION);
    snapshot("15c_chapter_transition");
    cafe_ui_set_transition_op(ui, "短按跳过");
    assert_op_no_overlay(ui);
}

static void gallery_op_checks(cafe_app_t *app, cafe_ui_t *ui, cafe_bands_t *bands)
{
    memset(app, 0, sizeof(*app));
    if (!cafe_app_init(app, ui, bands)) { fputs("gallery init failed", stderr); exit(2); }
    app->now_ms = 5000; app->last_activity_ms = 5000;
    uint32_t t = 5000;
    tap(app, CAFE_KEY_OK, t); t += 80;
    for (int i = 0; i < 4; ++i) { tap(app, CAFE_KEY_DOWN, t); t += 80; }
    tap(app, CAFE_KEY_OK, t); t += 80;
    for (int i = 0; i < 4; ++i) { tap(app, CAFE_KEY_DOWN, t); t += 80; }
    if (app->gallery_sel != 4 ||
        ui->lists[CAFE_LIST_GALLERY].count != 5 ||
        strcmp(lv_label_get_text(ui->lists[CAFE_LIST_GALLERY].labels[4]), "OP 播放") != 0) {
        fputs("gallery OP row not selected", stderr); exit(2);
    }
    const uint16_t vn = app->vm.node, vi = app->vm.index;
    const uint32_t vp = app->vm.progress;
    const uint8_t vprog = app->vm.progression;
    tap(app, CAFE_KEY_OK, t); t += 80;
    if (app->page != CAFE_PAGE_TRANSITION || !app->op_gallery || !app->op_pending) {
        fputs("gallery OP did not open", stderr); exit(2);
    }
    assert_op_no_overlay(ui);
    snapshot("gallery_op_first");
    const uint32_t start = app->op_start_ms;
    cafe_app_tick(app, start + 11000, 11000);
    if (app->page != CAFE_PAGE_TRANSITION || app->op_frame < 30) {
        fputs("gallery OP mid frame failed", stderr); exit(2);
    }
    snapshot("gallery_op_mid");
    cafe_app_tick(app, start + 92000, 81000);
    snapshot("gallery_op_last");
    cafe_app_tick(app, start + 95500, 3500);
    if (app->page != CAFE_PAGE_GALLERY || app->gallery_sel != 4 || app->op_pending ||
        app->vm.node != vn || app->vm.index != vi || app->vm.progress != vp ||
        app->vm.progression != vprog) {
        fputs("gallery OP natural return/VM changed", stderr); exit(2);
    }
    // Replay and short skip.
    tap(app, CAFE_KEY_OK, t); t += 80;
    if (app->page != CAFE_PAGE_TRANSITION || !app->op_gallery) { fputs("gallery replay failed", stderr); exit(2); }
    tap(app, CAFE_KEY_OK, t); t += 80;
    if (app->page != CAFE_PAGE_GALLERY || app->gallery_sel != 4) { fputs("gallery short skip failed", stderr); exit(2); }
    // Long OK returns to the same gallery row.
    tap(app, CAFE_KEY_OK, t); t += 80;
    cafe_app_key(app, CAFE_KEY_OK, true, t);
    cafe_app_tick(app, t + 520, 520);
    cafe_app_key(app, CAFE_KEY_OK, false, t + 560);
    t += 600;
    if (app->page != CAFE_PAGE_GALLERY || app->gallery_sel != 4 ||
        app->vm.node != vn || app->vm.index != vi || app->vm.progress != vp) {
        fputs("gallery long OK return failed", stderr); exit(2);
    }
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

int main(int argc, char **argv)
{
    (void)argc;
    size_t pack_size = 0;
    unsigned char *pack = read_file(argv[1], &pack_size);
    if (!pack || !cafe_pack_set_data(pack, pack_size)) {
        fprintf(stderr, "pack load failed\n");
        return 2;
    }

    fprintf(stderr, "smoke: pack ok\n");
    lv_init();
    fprintf(stderr, "smoke: lv_init\n");
    lv_display_t *disp = lv_display_create(W, H);
    if (!disp) { fprintf(stderr, "display create failed\n"); return 2; }
    lv_display_set_buffers(disp, s_draw, NULL, sizeof(s_draw),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);
    fprintf(stderr, "smoke: display ok\n");

    cafe_ui_t ui;
    cafe_bands_t bands;
    memset(&ui, 0, sizeof(ui));
    memset(&bands, 0, sizeof(bands));
    fprintf(stderr, "smoke: before bands\n");
    if (!cafe_image_bands_init(&bands)) { fprintf(stderr, "bands alloc failed\n"); return 2; }
    fprintf(stderr, "smoke: after bands\n");
    if (!cafe_ui_create(&ui)) { fprintf(stderr, "ui create failed\n"); return 2; }
    fprintf(stderr, "smoke: after ui\n");
    cafe_app_t app;
    // 预置 n2[49]（单立绘/非白背景）的自动进度，Continue 进入真实舞台。
    cafe_store_init();
    cafe_save_t seed;
    memset(&seed, 0, sizeof(seed));
    seed.occupied = true;
    seed.node = 8;
    seed.index = 12;
    seed.progression = 0;
    seed.last_complete = 0xFF;
    snprintf(seed.chapter, sizeof(seed.chapter), "prologue");
    if (!cafe_store_save_auto(&seed)) { fprintf(stderr, "seed auto failed\n"); return 2; }
    if (!cafe_app_init(&app, &ui, &bands)) { fprintf(stderr, "app init failed\n"); return 2; }
    app.now_ms = 1000;
    app.last_activity_ms = 1000;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("LVGL pool: size=%u free=%u biggest=%u used_pct=%u max_used=%u frag=%u\n",
           (unsigned)mon.total_size, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
           (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.frag_pct);

    snapshot("00_start");
    tap(&app, CAFE_KEY_OK, 1000);      // start -> title
    snapshot("01_title");
    app.title_sel = 1;                 // Continue -> seeded n8[12]
    tap(&app, CAFE_KEY_OK, 1100);
    cafe_ui_show_full_text(&ui);
    fprintf(stderr, "text y16=%d h16=%d\n", (int)lv_obj_get_y(ui.text), (int)lv_obj_get_height(ui.text));
    snapshot("02_game_16");

    // 无 UI 覆盖的纯舞台截图，供源图/压缩画质对照。
    lv_obj_add_flag(ui.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.chapter, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.auto_label, LV_OBJ_FLAG_HIDDEN);
    snapshot("02b_daily_art");
    lv_obj_remove_flag(ui.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.chapter, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.battery, LV_OBJ_FLAG_HIDDEN);

    tap(&app, CAFE_KEY_OK, 1200);
    cafe_ui_show_full_text(&ui);
    tap(&app, CAFE_KEY_OK, 1300);
    cafe_ui_show_full_text(&ui);
    snapshot("03_game_long_16");

    app.settings.font_large = true;
    cafe_ui_set_font_large(&ui, true);
    tap(&app, CAFE_KEY_OK, 1400);
    cafe_ui_show_full_text(&ui);
    fprintf(stderr, "text y20=%d h20=%d\n", (int)lv_obj_get_y(ui.text), (int)lv_obj_get_height(ui.text));
    snapshot("04_game_long_20");

    hold(&app, CAFE_KEY_OK, 2000);     // long OK -> menu
    cafe_app_key(&app, CAFE_KEY_OK, false, 2600);
    snapshot("05_menu");
    tap(&app, CAFE_KEY_DOWN, 2700);    // menu row 1: save
    tap(&app, CAFE_KEY_OK, 2800);
    snapshot("06_save");
    fprintf(stderr, "stage after save\n");

    hold(&app, CAFE_KEY_OK, 3000);     // save -> menu
    fprintf(stderr, "stage before save->menu\n");
    cafe_app_key(&app, CAFE_KEY_OK, false, 3600);
    app.menu_sel = 5;                  // return to title
    fprintf(stderr, "stage before title\n");
    tap(&app, CAFE_KEY_OK, 3700);
    fprintf(stderr, "stage title tapped\n");

    app.title_sel = 4;                 // gallery
    tap(&app, CAFE_KEY_OK, 3800);
    fprintf(stderr, "stage gallery tapped\n");
    snapshot("07_gallery");
    fprintf(stderr, "stage before cglist\n");
    tap(&app, CAFE_KEY_OK, 3900);      // CG list
    snapshot("08_cg_list");
    hold(&app, CAFE_KEY_OK, 4000);     // back gallery
    cafe_app_key(&app, CAFE_KEY_OK, false, 4600);
    app.gallery_sel = 2;               // story
    tap(&app, CAFE_KEY_OK, 4700);
    snapshot("09_chapter_routes");
    fprintf(stderr, "stage before chapters\n");
    tap(&app, CAFE_KEY_OK, 4800);      // common chapters
    snapshot("10_chapters");
    // 最大行数页面：历史 16 行（仅单标签）与回想 16 行。
    static char hlabels[16][48];
    const char *hptr[16];
    for (int i = 0; i < 16; ++i) {
        snprintf(hlabels[i], sizeof(hlabels[i]), "历史条目 %02d · 这是一行较长文本", i + 1);
        hptr[i] = hlabels[i];
    }
    cafe_ui_art_visible(&ui, false);
    cafe_ui_show_page(&ui, CAFE_PAGE_HISTORY);
    cafe_ui_set_rows(&ui, CAFE_LIST_HISTORY, hptr, NULL, 16, 0);
    snapshot("11_history16");
    // Real LVGL recall window: use actual pack labels, six visible rows, and
    // assert the selected row's border/text rather than only app scalar state.
    static char rlabels[20][64];
    const char *rptrs[20];
    for (int i = 0; i < 20; ++i) {
        cafe_gallery_t rg;
        const char *label = "未命名 CG";
        if (cafe_gallery_get((uint16_t)i, &rg) && cafe_pack_string(rg.label_off)[0])
            label = cafe_pack_string(rg.label_off);
        snprintf(rlabels[i], sizeof(rlabels[i]), "%s", label);
        rptrs[i] = rlabels[i];
    }
    cafe_ui_show_page(&ui, CAFE_PAGE_RECALL);
    cafe_ui_set_rows(&ui, CAFE_LIST_RECALL, rptrs, NULL, 16, 0);
    if (ui.lists[CAFE_LIST_RECALL].count != 6) { fputs("recall count != 6", stderr); return 2; }
    for (int i = 0; i < 6; ++i) {
        const char *txt = lv_label_get_text(ui.lists[CAFE_LIST_RECALL].labels[i]);
        if (!txt || !txt[0]) { fputs("recall blank label", stderr); return 2; }
    }
    if (lv_obj_get_style_border_width(ui.lists[CAFE_LIST_RECALL].rows[0], LV_PART_MAIN) != 2) {
        fputs("recall selected row not highlighted", stderr); return 2;
    }
    snapshot("12_recall16");
    // Empty state and last-page rows are also rendered by the real UI.
    cafe_ui_set_rows(&ui, CAFE_LIST_RECALL,
                     (const char *const[]){ "尚未解锁任何 CG" }, NULL, 1, 0);
    if (strcmp(lv_label_get_text(ui.lists[CAFE_LIST_RECALL].labels[0]), "尚未解锁任何 CG") != 0) {
        fputs("recall empty state missing", stderr); return 2;
    }
    cafe_ui_set_rows(&ui, CAFE_LIST_RECALL, &rptrs[18], NULL, 2, 1);
    if (strcmp(lv_label_get_text(ui.lists[CAFE_LIST_RECALL].labels[1]), rlabels[19]) != 0) {
        fputs("recall last-row label mismatch", stderr); return 2;
    }
    if (lv_obj_get_style_border_width(ui.lists[CAFE_LIST_RECALL].rows[1], LV_PART_MAIN) != 2) {
        fputs("recall last row not highlighted", stderr); return 2;
    }

    // CG 全屏（真实图集/缩放）。
    cafe_gallery_t g;
    uint16_t asset = 0xFFFF, image_id = 0xFFFF;
    cafe_asset_t ga;
    cafe_image_t gi;
    if (cafe_gallery_get(0, &g) && cafe_gallery_variant_get(g.first_variant, &asset) &&
        cafe_asset_get(asset, &ga) && cafe_image_get(ga.image_id, &gi)) {
        image_id = gi.id;
        cafe_ui_show_page(&ui, CAFE_PAGE_CG);
        cafe_ui_art_visible(&ui, true);
        if (cafe_image_render_image(&bands, image_id)) {
            cafe_ui_art_invalidate(&ui);
            cafe_ui_set_cg(&ui, "CG 001 · 1/21", "ev101", "上下切换差分 · 短按返回");
            snapshot("13_cg_view");

            lv_obj_add_flag(ui.cg_index, LV_OBJ_FLAG_HIDDEN);
            snapshot("13b_cg_art");
            lv_obj_remove_flag(ui.cg_index, LV_OBJ_FLAG_HIDDEN);
        }
    }

    fprintf(stderr, "stage after chapters\n");


    // OP 页面：真实媒体索引 + 原生 224x126 条带（首帧）。
    cafe_ui_show_page(&ui, CAFE_PAGE_TRANSITION);
    cafe_ui_art_visible(&ui, true);
    cafe_ui_set_transition_op(&ui, "短按跳过");
    assert_op_no_overlay(&ui);
    const uint16_t op_first = cafe_op_image_id(0);
    if (op_first != CAFE_IMAGE_NONE && cafe_image_render_op(&bands, op_first)) {
        cafe_ui_art_invalidate(&ui);
        snapshot("15_op");
    }
    else
        fprintf(stderr, "OP smoke frame failed\n");

    const uint16_t op_mid = cafe_op_image_id(143);
    if (op_mid != CAFE_IMAGE_NONE && cafe_image_render_op(&bands, op_mid)) {
        cafe_ui_art_invalidate(&ui);
        snapshot("15b_op_mid");
    }

    cafe_ui_show_page(&ui, CAFE_PAGE_MUSIC);
    cafe_ui_set_page_title(&ui, CAFE_PAGE_MUSIC, "音乐鉴赏");
    cafe_ui_set_rows(&ui, CAFE_LIST_MUSIC,
                     (const char *const[]){ "标题曲 Smiling-Swinging!!", "OP 曲" },
                     (const char *const[]){ "4 kHz 完整", "4 kHz 完整" }, 2, 0);
    cafe_ui_set_footer(&ui, CAFE_PAGE_MUSIC, "短按播放/重播 · 长按 OK 返回鉴赏室");
    snapshot("16_music");

    cafe_ui_show_modal(&ui, "音频输出失败", "audio write failed",
                       (const char *const[]){ "重试", "返回标题" }, 2, 0);
    snapshot("17_audio_error");

    cafe_ui_hide_modal(&ui);
    cafe_ui_show_page(&ui, CAFE_PAGE_SETTINGS);
    cafe_ui_set_page_title(&ui, CAFE_PAGE_SETTINGS, "设置");
    cafe_ui_set_rows(&ui, CAFE_LIST_SETTINGS,
                     (const char *const[]){ "文字速度", "文字大小", "自动阅读", "对话透明度",
                                            "标题背景", "声音", "音量", "返回" },
                     (const char *const[]){ "中", "16px", "关", "30%", "自动", "开", "60%", "" },
                     8, 5);
    snapshot("18_settings");

    transition_modes_check(&ui);
    gallery_op_checks(&app, &ui, &bands);
    layout_checks(&ui);
    cafe_ui_show_fatal("模拟启动失败：横屏不可用");
    snapshot("14_fatal");
    lv_mem_monitor(&mon);
    printf("LVGL pool after: free=%u biggest=%u used_pct=%u max_used=%u frag=%u\n",
           (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)mon.used_pct,
           (unsigned)mon.max_used, (unsigned)mon.frag_pct);

    cafe_image_bands_free(&bands);
    free(pack);
    printf("ui smoke PASS\n");
    return 0;
}
