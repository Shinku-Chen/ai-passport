// tests/harness_stubs/nn_canvas_stub.c —— 宿主用的分带画布桩。
//
// nn_ui/nn_app 的真实逻辑要能在宿主上跑，但它们只通过这几个接口用到画布：
// 带高/带数/dsc/条带首像素/画一带/刷黑一带。这里记录“画了哪些带、每带刷黑几次”，
// 便于断言分带流程（三次 flush 的带顺序），不涉及任何像素。
#include "nn_canvas.h"

#include "lvgl.h"

#include <string.h>

#define STUB_BAND_MAX 4
#define STUB_BAND_PX 320 * 60

static uint16_t s_band[STUB_BAND_MAX][STUB_BAND_PX];
static int s_band_h = NN_BAND_H;
static int s_band_count = NN_BAND_COUNT;
static lv_image_dsc_t s_dsc[STUB_BAND_MAX];

int stub_canvas_black_calls[STUB_BAND_MAX];
int stub_canvas_draw_calls[STUB_BAND_MAX];
int stub_canvas_cg_full_calls;   // CGVIEW contain 调用次数（断言用）
int stub_canvas_band_h(void) { return s_band_h; }

// 测试钩子：模拟回退到 320x60 / 四条带。
void stub_canvas_set_bands(int band_h, int count)
{
    s_band_h = band_h;
    s_band_count = count;
}

bool nn_canvas_init(void) { return true; }
int nn_canvas_band_h(void) { return s_band_h; }
int nn_canvas_band_count(void) { return s_band_count; }
uint16_t *nn_canvas_pixels(void) { return s_band[0]; }

void nn_canvas_clear_band(int band, uint16_t color)
{
    (void)color;
    if (band >= 0 && band < STUB_BAND_MAX) stub_canvas_black_calls[band]++;
}

void nn_canvas_black_band(int band) { nn_canvas_clear_band(band, 0); }

static lv_image_dsc_t *band_dsc(int band)
{
    if (band < 0 || band >= STUB_BAND_MAX) return NULL;
    lv_image_dsc_t *d = &s_dsc[band];
    memset(d, 0, sizeof(*d));
    d->header.w = NN_SCREEN_W;
    d->header.h = (uint32_t)s_band_h;
    d->header.stride = NN_SCREEN_W * 2;
    d->data_size = (uint32_t)NN_SCREEN_W * (uint32_t)s_band_h * 2u;
    d->data = (const uint8_t *)s_band[band];
    return d;
}

const void *nn_canvas_dsc(void) { return band_dsc(0); }
const void *nn_canvas_band_dsc(int band) { return band_dsc(band); }

bool nn_canvas_draw_image_band(const nn_pack_t *pack, uint16_t image_id, int band,
                               bool cg_full)
{
    (void)pack;
    (void)image_id;
    if (cg_full) stub_canvas_cg_full_calls++;
    if (band >= 0 && band < STUB_BAND_MAX) stub_canvas_draw_calls[band]++;
    return true;
}
