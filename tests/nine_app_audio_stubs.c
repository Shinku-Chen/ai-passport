// Host HAL stubs for the production nine-app menu regression.
// The app code is main/nine/nine_app.c; only board/decoder/audio edges are stubbed.
#include "nine_assets.h"
#include "nine_image.h"
#include "nine_media.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *nine_menu_script_path;
const char *nine_menu_images_path;
const lv_font_t nine_cjk_16 = { 0 };
const lv_font_t nine_cjk_20 = { 0 };

static uint8_t *s_script;
static size_t s_script_size;
static uint8_t *s_images;
static size_t s_images_size;

static uint8_t *read_all(const char *path, size_t *size)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

bool nine_assets_map(nine_assets_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_script) {
        s_script = read_all(nine_menu_script_path, &s_script_size);
        s_images = read_all(nine_menu_images_path, &s_images_size);
    }
    if (!s_script || !s_images) {
        out->error = "test pack missing";
        return false;
    }
    out->script = s_script;
    out->script_size = (uint32_t)s_script_size;
    out->images = s_images;
    out->images_size = (uint32_t)s_images_size;
    out->container = s_script;
    out->container_size = s_script_size;
    return true;
}

void nine_assets_unmap(nine_assets_t *out)
{
    (void)out;
}

lv_obj_t *lv_screen_active(void) { return NULL; }
bool nine_image_init(struct _lv_obj_t *parent) { (void)parent; return true; }
bool nine_image_bind_bundle(const uint8_t *script, size_t script_size,
                            const uint8_t *images, size_t images_size)
{
    (void)script; (void)script_size; (void)images; (void)images_size;
    return true;
}
bool nine_image_show_visual(uint32_t visual) { (void)visual; return true; }
bool nine_image_show_asset(uint32_t image) { (void)image; return true; }
bool nine_image_copy_scaled(uint16_t *dst, unsigned width, unsigned height)
{
    (void)dst; (void)width; (void)height;
    return true;
}
void nine_image_clear(uint16_t rgb565) { (void)rgb565; }
void nine_image_set_visible(bool visible) { (void)visible; }
void nine_image_set_op_fast(bool fast) { (void)fast; }
uint32_t nine_image_asset_count(void) { return 853; }
uint32_t nine_image_op_count(void) { return 623; }
bool nine_image_title(unsigned index, uint32_t *image)
{
    (void)index;
    if (image) *image = 0;
    return true;
}

int nine_audio_play_count;
int nine_audio_stop_count;
int nine_audio_op_count;
uint32_t nine_audio_current;
bool nine_audio_playing;

bool nine_media_init(const uint8_t *script, size_t script_size,
                     const uint8_t *images, size_t images_size)
{
    (void)script; (void)script_size; (void)images; (void)images_size;
    nine_audio_current = 0;
    nine_audio_playing = false;
    return true;
}
bool nine_media_play(uint32_t track)
{
    ++nine_audio_play_count;
    nine_audio_current = track;
    nine_audio_playing = true;
    return true;
}
bool nine_media_play_op(uint32_t track)
{
    ++nine_audio_op_count;
    nine_audio_current = track;
    nine_audio_playing = true;
    return true;
}
void nine_media_stop(void)
{
    ++nine_audio_stop_count;
    nine_audio_playing = false;
}
bool nine_media_deinit(void) { return true; }
bool nine_media_is_playing(void) { return nine_audio_playing; }
uint32_t nine_media_current_track(void) { return nine_audio_current; }
uint32_t nine_media_position_ms(void) { return 0; }
uint32_t nine_media_track_count(void) { return 2; }
bool nine_media_track_info(uint32_t track, nine_media_track_t *out)
{
    if (!out) return false;
    out->name = track ? "opening" : "title";
    out->duration_ms = 1000;
    out->sample_rate = 12000;
    out->data_offset = 0;
    out->data_size = 0;
    return true;
}
bool nine_media_failed(void) { return false; }
void nine_media_tick(void) {}

