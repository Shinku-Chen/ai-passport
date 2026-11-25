// Host real-LVGL renderer for review screenshots. No NINE_IMAGE_HOST: this
// exercises the production compositor, production gal_ui and real fonts at
// 320x240. Outputs PPMs; tools/nine_shots.py turns them into PNG/self-contained
// HTML. These are host renders, not device screenshots.
#include "capture.h"
#include "gal_text.h"
#include "gal_ui.h"
#include "nine_format.h"
#include "nine_image.h"
#include "nine_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

typedef struct {
    const char *name;
    char speaker[128];
    char text[2048];
    uint32_t visual;
    int layers;
} pick_t;

extern const lv_font_t nine_cjk_16;
extern const lv_font_t nine_cjk_20;
static nine_pack_t pack;
static gal_ui_t ui;
static uint32_t visual_count;

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

static int units(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p < 0x80) { ++n; ++p; }
        else if ((*p & 0xe0) == 0xc0) { n += 2; p += 2; }
        else if ((*p & 0xf0) == 0xe0) { n += 2; p += 3; }
        else { n += 2; p += 4; }
    }
    return n;
}

static int body_layers(uint32_t visual)
{
    nine_visual_record_t v;
    if (!nine_pack_visual(&pack, visual, &v)) return 0;
    int xs[8] = {0};
    int bodies = 0;
    for (uint32_t i = 0; i < v.layer_count; ++i) {
        nine_layer_record_t l;
        if (!nine_pack_layer(&pack, v.first_layer + i, &l)) continue;
        if (l.width == 256 && l.height == 144) continue;   // background
        if (l.width < 30 || l.height < 80) continue;       // not a face patch
        bool duplicate = false;
        for (int k = 0; k < bodies; ++k)
            if (abs(xs[k] - l.x) < 30) duplicate = true;
        if (!duplicate && bodies < 8) xs[bodies++] = l.x;
    }
    return bodies;
}

static int find_text_pick(const char *needle, pick_t *out)
{
    for (uint32_t n = 0; n < pack.node_count; ++n) {
        nine_node_t node;
        if (!nine_pack_node(&pack, (uint16_t)n, &node)) continue;
        for (uint32_t i = 0; i < node.frame_count; ++i) {
            nine_frame_t f;
            if (!nine_pack_frame(&pack, node.first_frame + i, &f) || f.kind != 0) continue;
            if (f.visual == NINE_NONE) continue;
            const char *text = nine_pack_text(&pack, &f);
            if (!text || !strstr(text, needle)) continue;
            const char *speaker = nine_pack_string(&pack, f.speaker);
            out->name = "frame";
            snprintf(out->speaker, sizeof(out->speaker), "%s", speaker ? speaker : "");
            snprintf(out->text, sizeof(out->text), "%s", text);
            out->visual = f.visual;
            out->layers = 0;
            return 1;
        }
    }
    return 0;
}

static int find_pick(int min_units, int max_units, int min_layers, int max_layers,
                     int min_bodies, int max_bodies, bool need_speaker, pick_t *out)
{
    for (uint32_t n = 0; n < pack.node_count; ++n) {
        nine_node_t node;
        if (!nine_pack_node(&pack, (uint16_t)n, &node)) continue;
        for (uint32_t i = 0; i < node.frame_count; ++i) {
            nine_frame_t f;
            if (!nine_pack_frame(&pack, node.first_frame + i, &f) || f.kind != 0) continue;
            if (f.visual == NINE_NONE) continue;
            nine_visual_record_t v;
            if (!nine_pack_visual(&pack, f.visual, &v)) continue;
            if ((int)v.layer_count < min_layers || (int)v.layer_count > max_layers) continue;
            const int bodies = body_layers(f.visual);
            if (bodies < min_bodies || bodies > max_bodies) continue;
            const char *text = nine_pack_text(&pack, &f);
            if (!text || !text[0]) continue;
            int u = units(text);
            if (u < min_units || u > max_units) continue;
            const char *speaker = nine_pack_string(&pack, f.speaker);
            if (need_speaker && (!speaker || !speaker[0])) continue;
            out->name = "frame";
            snprintf(out->speaker, sizeof(out->speaker), "%s", speaker ? speaker : "");
            snprintf(out->text, sizeof(out->text), "%s", text);
            out->visual = f.visual;
            out->layers = (int)v.layer_count;
            return 1;
        }
    }
    return 0;
}

static void set_page(const pick_t *p, bool large)
{
    uint32_t off[GAL_TEXT_MAX_PAGES + 1] = {0};
    const int pages = gal_text_pages(p->text, large ? 28 : 36, large ? 2 : 3,
                                     off, GAL_TEXT_MAX_PAGES + 1);
    char page[2048];
    const size_t n = pages > 0 ? off[1] : strlen(p->text);
    snprintf(page, sizeof(page), "%.*s", (int)(n < sizeof(page) ? n : sizeof(page) - 1), p->text);
    gal_ui_set_font_size(&ui, large);
    gal_ui_set_text(&ui, p->speaker[0] ? p->speaker : NULL, page, 0, NULL, NULL);
    gal_ui_show_full_text(&ui);
    gal_ui_show_page(&ui, GAL_PAGE_GAME);
}

static void write_shot(const char *dir, const char *name, bool check_text, int min_h)
{
    capture_render();
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    CHECK(capture_write_ppm(path), "write shot");
    if (check_text) {
        lv_obj_update_layout(lv_screen_active());
        const int h = lv_obj_get_height(ui.text);
        const int y = lv_obj_get_y(ui.text);
        printf("SHOT %s text_h=%d text_y=%d panel_y=%d\n", name, h, y, GAL_PANEL_Y);
        CHECK(h >= min_h && h <= GAL_PANEL_H - 1, "text fits panel");
    }
    printf("SHOT %s -> %s\n", name, path);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: %s script images out_dir\n", argv[0]);
        return 2;
    }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn);
    uint8_t *images = load(argv[2], &imn);
    if (!script || !images) return 2;
    CHECK(capture_init() != NULL, "real LVGL display");
    CHECK(nine_pack_open(&pack, script, sn), "pack");
    visual_count = pack.visual_count;
    CHECK(gal_ui_create(&ui, &nine_cjk_16, &nine_cjk_20), "real gal_ui");
    CHECK(nine_image_init(lv_screen_active()), "real image parent");
    CHECK(nine_image_bind_bundle(script, sn, images, imn), "bind real pack");
    nine_image_set_base_layer(1);
    nine_image_set_visible(true);
    gal_ui_set_panel_transparency(&ui, 10);
    gal_ui_set_battery(&ui, 80);
    gal_ui_set_chapter(&ui, "序章 · 白巳津川");

    const char *dir = argv[3];
    pick_t normal, multi, large;
    memset(&normal, 0, sizeof(normal)); memset(&multi, 0, sizeof(multi));
    memset(&large, 0, sizeof(large));
    CHECK(find_pick(100, 160, 1, 12, 1, 1, true, &normal), "normal single-character frame");
    CHECK(find_text_pick("新海兄妹", &multi) ||
          find_pick(40, 200, 3, 12, 2, 9, true, &multi), "multi-character frame");
    CHECK(find_pick(80, 160, 1, 12, 0, 9, false, &large), "long large-font frame");

    /* Normal: 16 px, page 0 has three lines. */
    CHECK(nine_image_show_visual(normal.visual), "normal visual");
    set_page(&normal, false);
    write_shot(dir, "normal-16px", true, 3 * 19 - 2);

    /* Multi-character: background plus at least two character layers. */
    CHECK(nine_image_show_visual(multi.visual), "multi visual");
    set_page(&multi, false);
    write_shot(dir, "multi-character", true, 1);

    /* Full CG at the same 16:9 geometry. */
    nine_gallery_record_t g;
    CHECK(nine_pack_gallery(&pack, 0, &g), "gallery record");
    CHECK(nine_image_show_asset(g.image), "CG asset");
    gal_ui_set_cg(&ui, "CG 01", "第一章", "原作 CG · 16:9 全宽 · host render", NULL);
    gal_ui_show_page(&ui, GAL_PAGE_CG);
    capture_render();
    char cg_path[512];
    snprintf(cg_path, sizeof(cg_path), "%s/cg-16x9.ppm", dir);
    CHECK(capture_write_ppm(cg_path), "write CG shot");
    printf("SHOT cg-16x9 -> %s\n", cg_path);

    /* Large 20 px: page 0 has two lines. */
    CHECK(nine_image_show_visual(large.visual), "large visual");
    set_page(&large, true);
    write_shot(dir, "large-20px", true, 2 * 23 - 2);

    /* Game menu over a real scene. */
    nine_image_show_visual(normal.visual);
    set_page(&normal, false);
    static const char *const menu[] = {"保存进度", "读取存档", "历史文本",
                                       "跳到下一章", "设置", "返回标题"};
    gal_ui_set_rows(&ui, GAL_LIST_MENU, menu, NULL, 6, 0);
    gal_ui_show_page(&ui, GAL_PAGE_MENU);
    capture_render();
    char menu_path[512];
    snprintf(menu_path, sizeof(menu_path), "%s/menu.ppm", dir);
    CHECK(capture_write_ppm(menu_path), "write menu shot");
    printf("SHOT menu -> %s\n", menu_path);

    (void)visual_count;
    printf("fails=%d\n", fails);
    nine_image_deinit();
    free(script); free(images);
    return fails ? 1 : 0;
}
