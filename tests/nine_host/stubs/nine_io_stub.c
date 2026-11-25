// Host data/image/media/bsp stubs for the reader application test. Image and
// audio decoding are covered by their own tests; here they only need to expose
// deterministic success/failure and an OP that ends on the next tick.
#include "bsp_battery.h"
#include "bsp_display.h"
#include "nine_image.h"
#include "nine_media.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nine_test_lock_fail_once = 0;
bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    if (nine_test_lock_fail_once > 0) {
        --nine_test_lock_fail_once;
        return false;
    }
    return true;
}
void bsp_lvgl_unlock(void) {}
int bsp_battery_soc(void) { return 88; }

const lv_font_t nine_cjk_16 = { 0 };
const lv_font_t nine_cjk_20 = { 0 };
int nine_host_fail_visual;
static bool s_bound;
static bool s_media_playing;
static uint32_t s_track = 0xffffffffu;

bool nine_image_init(struct _lv_obj_t *parent) { (void)parent; return true; }
void nine_image_deinit(void) {}
bool nine_image_bind_source(const nine_image_source_t *source) { return source != NULL; }
bool nine_image_bind_bundle(const uint8_t *script, size_t script_size,
                            const uint8_t *images, size_t image_size)
{
    (void)script; (void)script_size; (void)images; (void)image_size;
    s_bound = true;
    return true;
}
bool nine_image_show_visual(uint32_t visual) { (void)visual; return s_bound && !nine_host_fail_visual; }
bool nine_image_show_asset(uint32_t image) { (void)image; return s_bound; }
bool nine_image_show_asset_full(uint32_t image) { (void)image; return s_bound; }
bool nine_image_validate_asset(uint32_t image) { (void)image; return s_bound; }
void nine_image_clear(uint16_t rgb565) { (void)rgb565; }
void nine_image_set_visible(bool visible) { (void)visible; }
size_t nine_image_canvas_bytes(void) { return 0; }
size_t nine_image_scratch_bytes(void) { return 0; }
uint32_t nine_image_asset_count(void) { return s_bound ? 100 : 0; }
bool nine_image_title(unsigned index, uint32_t *image)
{
    if (!image || index > 2) return false;
    *image = index;
    return s_bound;
}
static const uint8_t s_pkg[1] = { 0 };
const uint8_t *nine_image_package(size_t *size)
{
    if (size) *size = sizeof(s_pkg);
    return s_pkg;
}
bool nine_image_op_frame(uint32_t index, uint32_t *image, uint32_t *timestamp,
                         uint32_t *duration)
{
    (void)index; (void)image; (void)timestamp; (void)duration;
    return false;
}
uint32_t nine_image_op_count(void) { return 0; }

bool nine_media_init(const uint8_t *script, size_t script_size,
                     const uint8_t *images, size_t image_size)
{
    (void)script; (void)script_size; (void)images; (void)image_size;
    return true;
}
bool nine_media_play(uint32_t track)
{
    if (track > 1) return false;
    s_track = track;
    s_media_playing = true;
    return true;
}
bool nine_media_play_op(uint32_t track)
{
    if (!nine_media_play(track)) return false;
    return true;
}
void nine_media_stop(void)
{
    s_media_playing = false;
    s_track = 0xffffffffu;
}
bool nine_media_deinit(void) { nine_media_stop(); return true; }
bool nine_media_is_playing(void) { return s_media_playing; }
uint32_t nine_media_current_track(void) { return s_track; }
uint32_t nine_media_position_ms(void) { return 0; }
uint32_t nine_media_track_count(void) { return 2; }
bool nine_media_track_info(uint32_t track, nine_media_track_t *out)
{
    if (track > 1 || !out) return false;
    memset(out, 0, sizeof(*out));
    out->name = track == 0 ? "title" : "opening";
    out->duration_ms = 1000;
    out->sample_rate = 8000;
    return true;
}
bool nine_media_failed(void) { return false; }
void nine_media_tick(void)
{
    // The OP visual sequence is irrelevant here; finish it on the first tick.
    if (s_media_playing && s_track == 1) s_media_playing = false;
}
