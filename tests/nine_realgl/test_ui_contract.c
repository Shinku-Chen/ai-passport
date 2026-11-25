// Native real-LVGL production UI contract. Checks actual object geometry and
// rendered pixels for the 9-nine reading page: 320x180 art, 60px body panel,
// 3/2 complete lines, speaker pill above the panel, long/empty names, and
// pagination bounds. This is a host proxy (32-bit when zig is available), not a
// device screenshot.
#include "capture.h"
#include "gal_text.h"
#include "gal_ui.h"
#include <stdio.h>
#include <string.h>

extern const lv_font_t nine_cjk_16;
extern const lv_font_t nine_cjk_20;
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint16_t art_pixels[320 * 180];
static lv_image_dsc_t art_dsc;
static gal_ui_t ui;

static void setup_art(void)
{
    for (int i = 0; i < 320 * 180; ++i) art_pixels[i] = 0x7BEF; // non-black grey
    memset(&art_dsc, 0, sizeof(art_dsc));
    art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    art_dsc.header.w = 320;
    art_dsc.header.h = 180;
    art_dsc.header.stride = 320 * 2;
    art_dsc.data_size = sizeof(art_pixels);
    art_dsc.data = (const uint8_t *)art_pixels;
}

static void show_text(const char *speaker, const char *text, bool large)
{
    gal_ui_set_font_size(&ui, large);
    gal_ui_set_text(&ui, speaker && speaker[0] ? speaker : NULL, text, 0,
                    &art_dsc, NULL);
    gal_ui_show_full_text(&ui);
    gal_ui_show_page(&ui, GAL_PAGE_GAME);
    lv_obj_update_layout(lv_screen_active());
}

static void check_name(const char *label, int max_width, bool hidden)
{
    lv_obj_update_layout(lv_screen_active());
    const bool is_hidden = lv_obj_has_flag(ui.name, LV_OBJ_FLAG_HIDDEN);
    lv_area_t na;
    lv_obj_get_coords(ui.name, &na);
    const int x = na.x1;
    const int y = na.y1;
    const int w = lv_obj_get_width(ui.name);
    const int h = lv_obj_get_height(ui.name);
    printf("NAME %s hidden=%d x=%d y=%d w=%d h=%d bottom=%d right=%d mode=%d\n",
           label, is_hidden, x, y, w, h, y + h - 1, x + w - 1,
           (int)lv_label_get_long_mode(ui.name));
    CHECK(is_hidden == hidden, "empty/visible name state");
    if (!hidden) {
        CHECK(y + h - 1 <= 179, "name pill bottom is above panel");
        CHECK(x + w - 1 <= 8 + max_width - 1, "name pill max width/right edge");
        CHECK(w > 0 && h > 0, "name pill has content size");
        CHECK(lv_obj_get_style_bg_opa(ui.name, LV_PART_MAIN) > 0, "name pill background");
        CHECK(lv_label_get_long_mode(ui.name) == LV_LABEL_LONG_MODE_DOTS,
              "long name uses DOTS");
    }
}

static void check_body(const char *label, int want_lines)
{
    lv_obj_update_layout(lv_screen_active());
    lv_area_t ta, pa;
    lv_obj_get_coords(ui.text, &ta);
    lv_obj_get_coords(ui.panel, &pa);
    const int y = ta.y1;
    const int h = lv_obj_get_height(ui.text);
    const int panel_y = pa.y1;
    const int panel_h = lv_obj_get_height(ui.panel);
    const lv_font_t *font = lv_obj_get_style_text_font(ui.text, LV_PART_MAIN);
    const int line_h = lv_font_get_line_height(font);
    printf("BODY %s y=%d h=%d panel_y=%d panel_h=%d line_h=%d want_lines=%d\n",
           label, y, h, panel_y, panel_h, line_h, want_lines);
    CHECK(panel_y == 180 && panel_h == 60, "body panel is 180..239");
    CHECK(y >= panel_y, "text starts inside panel");
    CHECK(h <= panel_y + panel_h - y, "complete text fits panel");
    CHECK(h >= want_lines * line_h - 2, "requested complete lines present");
}

static void check_rendered_art(void)
{
    capture_render();
    int dark_bottom = 0;
    for (int x = 0; x < 320; ++x) {
        const uint16_t p = capture_pixel(x, 179);
        if (p == 0 || p == 0x0841) dark_bottom++;
    }
    printf("ART row179_dark=%d pixel0=%04x pixel319=%04x\n",
           dark_bottom, capture_pixel(0, 179), capture_pixel(319, 179));
    CHECK(dark_bottom == 0, "no black strip at art bottom y=179");
    CHECK(capture_pixel(0, 0) != 0 && capture_pixel(319, 0) != 0,
          "full-width art at y=0");
    CHECK(capture_pixel(0, 90) == 0x7bef && capture_pixel(319, 90) == 0x7bef,
          "art has no horizontal seam at mid stage");
    CHECK(capture_pixel(0, 179) == 0x7bef && capture_pixel(319, 179) == 0x7bef,
          "art bottom is complete at y=179");
    CHECK(capture_pixel(0, 180) != 0 || capture_pixel(319, 180) != 0,
          "panel begins immediately at y=180");
}

int main(void)
{
    setup_art();
    CHECK(capture_init() != NULL, "real LVGL display");
    CHECK(gal_ui_create(&ui, &nine_cjk_16, &nine_cjk_20), "real gal_ui");
    gal_ui_set_panel_transparency(&ui, 10);
    gal_ui_show_page(&ui, GAL_PAGE_GAME);

    // Empty speaker hides pill.
    show_text(NULL, "旁白第一行\n旁白第二行\n旁白第三行", false);
    check_name("empty", 160, true);
    check_body("empty-small", 3);

    // Short name: fitted pill, no clipping.
    show_text("九條都", "第一行\n第二行\n第三行", false);
    check_name("short", 160, false);
    check_body("short-small", 3);

    // Long name: must cap and ellipsize, stay left of notice/auto and above panel.
    show_text("一个非常长的说话人名字超过一百六十像素宽度", "第一行\n第二行\n第三行", false);
    check_name("long", 160, false);
    check_body("long-small", 3);

    // Two complete 20px lines.
    show_text("九條都", "大字号第一行\n大字号第二行", true);
    check_name("large-font-name", 160, false);
    check_body("large", 2);

    // Real pagination: first, middle, last all fit the same panel.
    const char *long_text = "这是用于分页测试的长句，包含足够多的字符以产生多个分页。"
                            "分页必须保留所有文本，不得裁掉最后一行。"
                            "继续补充文字，确保第一页、第二页和最后一页都能覆盖真实分页边界。"
                            "再多加一些内容，避免因为测试句太短而误判为单页。"
                            "为了保证至少出现三页，这里再增加一整段用于分页的正文，"
                            "并继续保持每一行都在面板内部完整排版。";
    uint32_t off[GAL_TEXT_MAX_PAGES + 1] = {0};
    int pages = gal_text_pages(long_text, 36, 3, off, GAL_TEXT_MAX_PAGES + 1);
    printf("PAGES count=%d\n", pages);
    CHECK(pages >= 2, "long text paginates");
    gal_ui_set_font_size(&ui, false);
    for (int p = 0; p < pages; ++p) {
        char page[1024];
        uint32_t a = off[p], b = off[p + 1];
        if (b < a || b > strlen(long_text)) b = (uint32_t)strlen(long_text);
        snprintf(page, sizeof(page), "%.*s", (int)(b - a), long_text + a);
        show_text("九條都", page, false);
        check_body(p == 0 ? "page-first" : (p + 1 == pages ? "page-last" : "page-mid"), 1);
    }

    check_rendered_art();
    printf("fails=%d\n", fails);
    return fails ? 1 : 0;
}
