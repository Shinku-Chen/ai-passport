#include "jpeg_decoder.h"
#include <string.h>
static int s_fail;
static int s_success_before_fail = -1;
static int s_decodes;
void jpeg_stub_fail_next(int times) { s_fail = times; }
void jpeg_stub_fail_after(int successes) { s_success_before_fail = successes; }
int jpeg_stub_decode_count(void) { return s_decodes; }

static int find_sof(const uint8_t *d, uint32_t n, uint16_t *w, uint16_t *h)
{
    if (!d || n < 4) return 0;
    for (uint32_t i = 0; i + 9 < n; ++i) {
        if (d[i] != 0xFF) continue;
        const uint8_t m = d[i + 1];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            *h = (uint16_t)((d[i + 5] << 8) | d[i + 6]);
            *w = (uint16_t)((d[i + 7] << 8) | d[i + 8]);
            return 1;
        }
    }
    return 0;
}

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    s_decodes++;
    if (s_success_before_fail == 0) {
        s_success_before_fail = -1;
        return ESP_FAIL;
    }
    if (s_success_before_fail > 0) s_success_before_fail--;
    if (s_fail > 0) { s_fail--; return ESP_FAIL; }
    if (!cfg || !img) return ESP_ERR_INVALID_ARG;
    uint16_t w = 0, h = 0;
    if (!find_sof(cfg->indata, cfg->indata_size, &w, &h)) return ESP_FAIL;
    const size_t need = (size_t)w * h * 2;
    if (!cfg->outbuf || cfg->outbuf_size < need) return ESP_ERR_INVALID_ARG;
    uint16_t *px = (uint16_t *)cfg->outbuf;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px[y * w + x] = (uint16_t)(((x * 17 + y * 7) & 0xFF) | (((y * 11) & 0xFF) << 5) | (((x * 3) & 0xFF) << 11));
    img->width = w;
    img->height = h;
    img->output_len = need;
    return ESP_OK;
}
