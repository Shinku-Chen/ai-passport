// Host stub: the app regressions exercise text/state/UI, not JPEG decoding.
#include "steins0_image.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static lv_obj_t *s_canvas;
int host_image_fail_validate;
void host_image_reset(void) { host_image_fail_validate = 0; }

bool steins0_image_init(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!image || !parent || !pixels) return false;
    memset(image, 0, sizeof(*image));
    image->pixels = pixels;
    s_canvas = lv_canvas_create(parent);
    image->canvas = s_canvas;
    return s_canvas != NULL;
}

void steins0_image_clear(steins0_image_t *image) { (void)image; }
bool steins0_image_show_scene(steins0_image_t *image, const sg0_pack_t *pack, uint16_t view,
                              bool draw) { (void)image; (void)pack; (void)view; (void)draw; return true; }
bool steins0_image_show_cg(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                           bool draw) { (void)image; (void)pack; (void)asset; (void)draw; return true; }
bool steins0_image_show_title(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw) { (void)image; (void)pack; (void)asset; (void)draw; return true; }
bool steins0_image_show_card(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                             bool draw) { (void)image; (void)pack; (void)asset; (void)draw; return true; }
bool steins0_image_show_stamp(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw) { (void)image; (void)pack; (void)asset; (void)draw; return true; }
bool steins0_image_init_ex(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels,
                           uint16_t width, uint16_t height)
{
    if (!image || !parent || !pixels) return false;
    memset(image, 0, sizeof(*image));
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->canvas = lv_canvas_create(parent);
    return image->canvas != NULL;
}
bool steins0_image_validate_asset(const sg0_pack_t *pack, uint16_t asset)
{ (void)pack; (void)asset; return !host_image_fail_validate; }
bool steins0_image_validate_view(const sg0_pack_t *pack, uint16_t view)
{ (void)pack; (void)view; return !host_image_fail_validate; }
void steins0_image_layer_box(const sg0_layer_t *layer, int *x, int *y, int *w, int *h)
{ (void)layer; if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; }
