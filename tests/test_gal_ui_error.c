// tests/test_gal_ui_error.c —— 资源错误层的真实 LVGL 可见性回归。
//
// 用完整 LVGL + 生产 gal_ui.c + 真实字体与媒体包，渲染到 320x240 帧缓冲：
//   · 标题图 / CG 图 / 缩略图页面先正常渲染（可见性基线）；
//   · 破坏一张尚未解码的缩略图条带，触发真实解码失败（maitetsu_media_faulted）；
//   · 提交错误层后，在 TITLE / CG / CG_LIST / GALLERY / GAME 与切页后检查：
//     错误层的父对象是屏幕根、祖先链全部可见、中心像素是错误底色、存在文字
//     像素、没有亮白空屏；
//   · 隐藏错误层后页面恢复（层可关闭）。
#include "lvgl.h"
#include "gal_ui.h"
#include "maitetsu_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

#define W 320
#define H 240
static uint16_t s_frame[W * H];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= H) continue;
        const int x0 = area->x1 < 0 ? 0 : area->x1;
        const int x1 = area->x2 >= W ? W - 1 : area->x2;
        if (x1 >= x0) {
            memcpy(&s_frame[y * W + x0], src + (x0 - area->x1), (size_t)(x1 - x0 + 1) * 2);
        }
        src += lv_area_get_width(area);
    }
    lv_display_flush_ready(disp);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    uint8_t *data = malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void render(lv_display_t *disp)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
}

static bool screen_has_text_pixels(void)
{
    int bright = 0;
    for (int i = 0; i < W * H; ++i) {
        const uint16_t p = s_frame[i];
        const int r = (p >> 11) & 0x1F;
        const int g = (p >> 5) & 0x3F;
        const int b = p & 0x1F;
        if (r > 18 && g > 44 && b > 18) bright++;
    }
    return bright > 20;
}

static bool screen_is_blank_white(void)
{
    int white = 0;
    for (int i = 0; i < W * H; ++i) {
        if (s_frame[i] == 0xFFFF) white++;
    }
    return white > (W * H) / 2;
}

static bool ancestors_visible(lv_obj_t *obj)
{
    for (lv_obj_t *p = obj; p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

static void check_error_layer(gal_ui_t *ui, gal_page_t page, const char *name)
{
    gal_ui_show_page(ui, page);
    render(lv_display_get_default());
    lv_obj_t *error = ui->asset_error;
    CHECK(error != NULL, "error layer exists");
    if (!error) return;
    CHECK(lv_obj_get_parent(error) == lv_screen_active(),
          "error layer is parented to the screen, not a page");
    CHECK(!lv_obj_has_flag(error, LV_OBJ_FLAG_HIDDEN), "error layer is shown");
    CHECK(ancestors_visible(error), "error layer ancestor chain is visible");
    CHECK(lv_obj_is_visible(error), "error layer is really visible");
    const uint16_t panel = lv_color_to_u16(lv_color_hex(0x5A1E1E));
    CHECK(s_frame[6 * W + 6] == panel, "error background pixel covers the corner");
    int painted = 0;
    for (int i = 0; i < W * H; ++i) {
        if (s_frame[i] == panel) painted++;
    }
    CHECK(painted > (W * H) / 2, "error layer covers most of the screen");
    CHECK(screen_has_text_pixels(), "error message text is rendered");
    CHECK(!screen_is_blank_white(), "error layer is not a blank white screen");
    printf("error layer visible on page %d (%s): painted=%d\n", (int)page, name, painted);
}

// 在已加载的包里破坏一张图的条带（尚未解码、CRC 尚未缓存）。
static void corrupt_image(uint8_t *pack, uint16_t image)
{
    const size_t band_off = (size_t)(pack[36] | (pack[37] << 8) | (pack[38] << 16) |
                                     ((uint32_t)pack[39] << 24));
    const size_t payload_off = (size_t)(pack[40] | (pack[41] << 8) | (pack[42] << 16) |
                                        ((uint32_t)pack[43] << 24));
    const size_t image_off = (size_t)(pack[32] | (pack[33] << 8) | (pack[34] << 16) |
                                      ((uint32_t)pack[35] << 24));
    const size_t entry = image_off + (size_t)image * 16u;
    const uint16_t first_band = (uint16_t)(pack[entry + 4] | (pack[entry + 5] << 8));
    const size_t band_entry = band_off + (size_t)first_band * 16u;
    const size_t off = (size_t)(pack[band_entry] | (pack[band_entry + 1] << 8) |
                                (pack[band_entry + 2] << 16) |
                                ((uint32_t)pack[band_entry + 3] << 24));
    pack[payload_off + off + 40] ^= 0xFF;
}

int main(int argc, char **argv)
{
    const char *pack_path = argc > 1 ? argv[1] : "assets/maitetsu/maitetsu_media.bin";
    size_t size = 0;
    uint8_t *pack = read_file(pack_path, &size);
    if (!pack) {
        printf("FAIL: cannot read %s\n", pack_path);
        return 1;
    }
    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    CHECK(disp != NULL, "display created");
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static uint16_t draw_buf[W * 40];
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    CHECK(maitetsu_media_init_from(pack, size), "media init");
    gal_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    CHECK(gal_ui_create(&ui, &gal_cjk_16, &gal_cjk_20), "ui create");

    // 基线：标题页（标题图）、CG 页（CG 图）、CG 列表（缩略图）正常渲染。
    gal_ui_set_title_image(&ui, maitetsu_media_title());
    gal_ui_show_page(&ui, GAL_PAGE_TITLE);
    render(disp);
    CHECK(!screen_is_blank_white(), "title page renders before any fault");

    gal_ui_set_cg(&ui, "CG 01 / 35", "尾声 · 重逢", "", maitetsu_media_cg(0));
    gal_ui_show_page(&ui, GAL_PAGE_CG);
    render(disp);

    gal_cg_t items[GAL_CG_TILES];
    for (int i = 0; i < GAL_CG_TILES; ++i) {
        memset(&items[i], 0, sizeof(items[i]));
        items[i].label = "CG";
        items[i].unlocked = true;
        items[i].image = (const void *)maitetsu_media_cg_thumb((uint16_t)i);
    }
    gal_ui_set_gallery(&ui, items, GAL_CG_TILES, 0);
    gal_ui_show_page(&ui, GAL_PAGE_CG_LIST);
    render(disp);

    // 真实解码失败：破坏一张还没解过的缩略图（第 9 张），再渲染 CG 列表。
    CHECK(!maitetsu_media_faulted(), "no fault before corruption");
    corrupt_image(pack, maitetsu_cg_thumbs[8]);
    items[GAL_CG_TILES - 1].image = (const void *)maitetsu_media_cg_thumb(8);
    gal_ui_set_gallery(&ui, items, GAL_CG_TILES, 8);
    render(disp);
    CHECK(maitetsu_media_faulted(), "corrupt thumbnail raises a real media fault");

    // 应用收口后的错误层：所有页面都必须在最上面可见（父对象是屏幕根）。
    gal_ui_show_asset_error(&ui, "资源损坏，无法继续\n媒体条带校验或解码失败");
    check_error_layer(&ui, GAL_PAGE_CG_LIST, "cg list");
    check_error_layer(&ui, GAL_PAGE_TITLE, "title");
    check_error_layer(&ui, GAL_PAGE_CG, "cg viewer");
    check_error_layer(&ui, GAL_PAGE_GALLERY, "gallery");
    check_error_layer(&ui, GAL_PAGE_GAME, "reader");
    check_error_layer(&ui, GAL_PAGE_MENU, "menu");

    // 隐藏错误层后页面恢复（错误层可以关闭，不是永久亮屏）。
    gal_ui_show_asset_error(&ui, NULL);
    gal_ui_show_page(&ui, GAL_PAGE_TITLE);
    render(disp);
    const uint16_t panel = lv_color_to_u16(lv_color_hex(0x5A1E1E));
    CHECK(s_frame[6 * W + 6] != panel, "hidden error layer no longer covers the screen");

    free(pack);
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: gal ui error layer\n");
    return 0;
}
