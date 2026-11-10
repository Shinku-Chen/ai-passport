#ifdef NDEBUG
#undef NDEBUG // Keep pixel/bounds assertions enabled in optimized host builds.
#endif
#include "voice_marquee.h"
#include "fonts/voice_cjk.h"
// Test-only access to the exact pinned LVGL's animation offset, never firmware API.
#include "src/widgets/label/lv_label_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 240
#define HEIGHT 320
#define STEPS 4000
static uint8_t draw_buffer[WIDTH * 20 * 2];
static uint16_t framebuffer[WIDTH * HEIGHT];
static uint32_t baseline_frames[STEPS];
static lv_area_t expected;
static bool enforce_bounds;
static unsigned flushes, pixels, largest;

typedef struct {
    unsigned pixels, flushes, largest;
    int max_step;
} result_t;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    assert(w > 0 && h > 0);
    assert(area->x1 >= 0 && area->x2 < WIDTH && area->y1 >= 0 && area->y2 < HEIGHT);
    if (enforce_bounds) {
        assert(area->x1 >= expected.x1 && area->x2 <= expected.x2);
        assert(area->y1 >= expected.y1 && area->y2 <= expected.y2);
    }
    for (int y = 0; y < h; ++y)
        memcpy(&framebuffer[(area->y1 + y) * WIDTH + area->x1],
               data + y * w * 2, (size_t)w * 2);
    unsigned size = (unsigned)(w * h);
    pixels += size;
    ++flushes;
    if (size > largest) largest = size;
    lv_display_flush_ready(display);
}

static uint32_t frame_hash(void)
{
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
        hash = (hash ^ framebuffer[i]) * 16777619u;
    return hash;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, unsigned color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static result_t run(bool header, int variant, const char *text)
{
    // Fresh LVGL clocks make frame-by-frame before/after replays deterministic.
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *old_screen = lv_screen_active();
    lv_obj_t *screen = box(NULL, 0, 0, WIDTH, HEIGHT, 0x17203A);
    int x = header ? 8 : 10;
    int y = header ? 0 : 1;
    int w = header ? 178 : 222;
    int h = header ? 24 : 21;
    int parent_y = header ? 0 : 129;
    int parent_x = header ? 0 : 4;
    lv_obj_t *parent = box(screen, parent_x, parent_y, header ? WIDTH : 232,
                           h, header ? 0x1B2444 : 0x2E4C9B);
    lv_obj_t *label;
    if (variant == 0) {
        // Previous firmware geometry and fixed 2000ms circular animation.
        label = lv_label_create(parent);
        lv_label_set_text_static(label, text);
        lv_obj_set_style_text_font(label, &voice_ttf, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xEDF1FF), 0);
        lv_obj_set_style_pad_all(label, 0, 0);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_size(label, w, h);
        lv_obj_set_style_anim_duration(label, 2000, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else {
        label = voice_marquee_create(parent, x, y, w, text, &voice_ttf,
                                      lv_color_hex(0xEDF1FF), true);
        // Isolate clipping savings from the change to constant pixel speed.
        if (variant == 1) lv_obj_set_style_anim_duration(label, 2000, 0);
    }
    lv_screen_load(screen);
    if (old_screen) lv_obj_delete(old_screen);
    lv_obj_update_layout(screen);
    enforce_bounds = false;
    lv_refr_now(display); // Initial page draw is intentionally full-screen.
    pixels = flushes = largest = 0;
    expected = (lv_area_t){parent_x + x, parent_y + y,
                          parent_x + x + w - 1,
                          parent_y + y + voice_ttf.line_height - 1};
    enforce_bounds = variant != 0;
    int previous_offset = ((lv_label_t *)label)->offset.x;
    int max_step = 0;
    uint32_t hash = frame_hash();
    unsigned previous_flushes = 0;
    for (int i = 0; i < STEPS; ++i) {
        lv_tick_inc(5);
        lv_timer_handler();
        int offset = ((lv_label_t *)label)->offset.x;
        int step = previous_offset - offset;
        if (step > max_step) max_step = step; // Exclude circular wrap to origin.
        previous_offset = offset;
        if (previous_flushes != flushes) {
            hash = frame_hash();
            previous_flushes = flushes;
        }
        if (variant == 0) baseline_frames[i] = hash;
        if (variant == 1) assert(baseline_frames[i] == hash);
    }
    result_t result = {pixels, flushes, largest, max_step};
    assert(pixels > 0 && largest < WIDTH * HEIGHT);
    if (variant == 2) assert(max_step <= 2);
    // No timer redraws once the label is static and outstanding invalidation drains.
    enforce_bounds = false;
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_refr_now(display);
    unsigned static_flushes = flushes;
    for (int i = 0; i < 200; ++i) { lv_tick_inc(5); lv_timer_handler(); }
    assert(flushes == static_flushes);
    lv_display_delete(display);
    lv_deinit();
    return result;
}

int main(void)
{
    const char *text = "音效钥匙扣音效钥匙扣音效钥匙扣音效钥匙扣音效钥匙扣音效钥匙扣";
    for (int header = 0; header <= 1; ++header) {
        result_t before = run(header, 0, text);
        result_t clipped = run(header, 1, text);
        result_t after = run(header, 2, text);
        assert(clipped.pixels < before.pixels);
        printf("%s: before_pixels=%u clipped_pixels=%u optimized_pixels=%u "
               "before_flushes=%u clipped_flushes=%u optimized_flushes=%u "
               "before_max_step=%d optimized_max_step=%d\n",
               header ? "header" : "row", before.pixels, clipped.pixels, after.pixels,
               before.flushes, clipped.flushes, after.flushes, before.max_step, after.max_step);
    }
    puts("voice_marquee: PASS (20s replay per variant; clipping is pixel-identical)");
    return 0;
}
