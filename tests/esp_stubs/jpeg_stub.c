// tests/esp_stubs/jpeg_stub.c —— 用输入首字节决定"解码后是什么图"的桩。
//
//   'T' 320x150  'B' 160x45   'S' 90x150   's' 90x90   'F' 失败   'W' 尺寸错误
#include "jpeg_decoder.h"

#include <string.h>

#include "esp_timer.h"

static void fill(uint16_t *dst, int w, int h, uint16_t value)
{
    for (int i = 0; i < w * h; ++i) dst[i] = value;
}

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *out)
{
    if (!cfg || !out || !cfg->indata || cfg->indata_size == 0) return ESP_FAIL;
    uint16_t w = 0, h = 0, value = 0;
    switch (cfg->indata[0]) {
    case 'T': w = 320; h = 150; value = 0x1234; break;
    case 'B': w = 160; h = 45; value = 0x5678; break;
    case 'S': w = 90; h = 150; value = 0x9ABC; break;
    case 's': w = 90; h = 90; value = 0xDEF0; break;
    case 'W': w = 321; h = 150; value = 0xFFFF; break;
    case 'F':
        return ESP_FAIL;
    default:
        // 真实 pack 的 JPEG:按调用方给的输出缓冲大小推断目标尺寸(测试桩专用)。
        switch (cfg->outbuf_size) {
        case 320u * 150u * 2u: w = 320; h = 150; break;
        case 160u * 45u * 2u: w = 160; h = 45; break;
        case 96u * 150u * 2u: w = 90; h = 150; break;
        case 96u * 90u * 2u: w = 90; h = 90; break;
        default: return ESP_FAIL;
        }
        value = 0x0101;
        break;
    }
    if (cfg->out_format != JPEG_IMAGE_FORMAT_RGB565) return ESP_FAIL;
    const uint32_t need = (uint32_t)w * h * 2u;
    if (cfg->outbuf_size < need || !cfg->outbuf) return ESP_FAIL;
    fill((uint16_t *)cfg->outbuf, w, h, value);
    out->width = w;
    out->height = h;
    out->output_len = need;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}
