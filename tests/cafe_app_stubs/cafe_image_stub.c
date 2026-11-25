#include "cafe_image.h"
static uint32_t s_scene_calls, s_verify_calls;
int audit_image_op_fail;
bool cafe_image_init(void) { return true; }
bool cafe_image_bands_init(cafe_bands_t *b) { (void)b; return true; }
void cafe_image_bands_free(cafe_bands_t *b) { (void)b; }
bool cafe_image_render_scene(cafe_bands_t *b, uint16_t bg, const cafe_sprite_t *s, unsigned n)
{ s_scene_calls++; (void)b; (void)bg; (void)s; (void)n; return true; }
bool cafe_image_render_image(cafe_bands_t *b, uint16_t id) { (void)b; (void)id; return true; }
bool cafe_image_thumb(uint16_t id, uint16_t *dst, int w, int h) { (void)id; (void)dst; (void)w; (void)h; return false; }
int cafe_image_gallery_group(uint16_t asset) { (void)asset; return -1; }
bool cafe_image_gallery_alias(uint16_t asset, uint16_t index, uint16_t *group)
{ (void)asset; (void)index; (void)group; return false; }
bool cafe_image_verify(uint16_t id) { s_verify_calls++; (void)id; return true; }

bool cafe_image_render_op(cafe_bands_t *bands, uint16_t image_id) { (void)bands; (void)image_id; return !audit_image_op_fail; }

uint32_t cafe_image_render_scene_count(void) { return s_scene_calls; }
uint32_t cafe_image_verify_count(void) { return s_verify_calls; }
void cafe_image_reset_counts(void) { s_scene_calls = 0; s_verify_calls = 0; }
