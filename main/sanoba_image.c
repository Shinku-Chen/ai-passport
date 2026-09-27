// main/sanoba_image.c —— 画面区合成实现。
//
// 三套解码路径,共同点是"没有中间大缓冲、不用 32 KB inflate 字典":
//   1. 背景 / 事件 CG:JPEG 240x320,esp_jpeg 直接解进画布(输出缓冲就是画布);
//   2. 立绘 / SD / 特效:**分块调色板格式**(不是 PNG),每块解压到 ≤2 KB 的行缓冲,
//      逐行反滤波 -> 查调色板 -> 混进画布;
//   3. 事件 CG 补丁:先解基准 JPEG,再按 1bpp 掩码把 RGB565 像素逐行覆写上去
//      (掩码整段解压,像素按块解压,边解边写)。
//
// 为什么不用 PNG / 流式解压:本板空闲堆只有十几 KB,而 tinfl 的流式解压需要一块
// 32 KB 环形字典。分块格式把字典需求压到 0(每块单独解压,不用 TINFL 的环形模式),
// 代价是压缩率略低 —— 由 tools/sanoba_pack.py 承担。
#include "sanoba_image.h"

#include "sanoba_inflate.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "sanoba_img";

// esp_jpeg 在 C3 上走 ROM 里的 TJpgDec,输出 RGB565 时由它做 888->565 转换:
// swap=false 写 LOBYTE 在前 = 小端,正好是 LVGL 要的原生 16 位字节序。
#define SANOBA_JPEG_SWAP_BYTES 0

// 正文带颜色(源工程 text_bg 的蓝底,压深一点配和风画面)与上下透明度
#define SANOBA_BAND_R 42
#define SANOBA_BAND_G 60
#define SANOBA_BAND_B 110
#define SANOBA_BAND_ALPHA_TOP 120
#define SANOBA_BAND_ALPHA_BOTTOM 236
// 文字区浅暗帘:压在文字下面,保证白字在任何画面上都能读
#define SANOBA_SCRIM_R 6
#define SANOBA_SCRIM_G 12
#define SANOBA_SCRIM_B 20
#define SANOBA_SCRIM_ALPHA_TOP 24
#define SANOBA_SCRIM_ALPHA_BOTTOM 96

// 分块调色板格式头部:8 个 u16(见 tools/sanoba_pack.py)
#define BLOCKED_HEADER 16u
// 补丁像素块的像素数(与打包器一致):每块最多 1024 个 RGB565 = 2 KB
#define PATCH_BLOCK_PIXELS 1024u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void canvas_invalidate(sanoba_image_t *img)
{
    if (img->canvas) {
        lv_obj_invalidate(img->canvas);
    }
}

static void fill_canvas(sanoba_image_t *img, uint16_t color)
{
    for (size_t index = 0; index < (size_t)SANOBA_ART_W * SANOBA_ART_H; index++) {
        img->pixels[index] = color;
    }
}

bool sanoba_image_init(sanoba_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!img || !parent || !pixels) {
        return false;
    }
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    fill_canvas(img, 0);
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, SANOBA_ART_W, SANOBA_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    return true;
}

void sanoba_image_set_pack(sanoba_image_t *img, const sanoba_pack_t *pack)
{
    if (img) {
        img->pack = pack;
    }
}

void sanoba_image_clear(sanoba_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    fill_canvas(img, 0);
    canvas_invalidate(img);
}

uint16_t *sanoba_image_canvas(sanoba_image_t *img)
{
    return img && img->pixels ? img->pixels : NULL;
}

// --------------------------------------------------------------------------
// RGB565 混合
// --------------------------------------------------------------------------

static inline uint16_t blend565(uint16_t dst, uint16_t src, unsigned alpha)
{
    if (alpha == 0) {
        return dst;
    }
    if (alpha >= 255) {
        return src;
    }
    const unsigned inverse = 255u - alpha;
    const unsigned r = (((src >> 11) & 0x1Fu) * alpha + ((dst >> 11) & 0x1Fu) * inverse) / 255u;
    const unsigned g = (((src >> 5) & 0x3Fu) * alpha + ((dst >> 5) & 0x3Fu) * inverse) / 255u;
    const unsigned b = ((src & 0x1Fu) * alpha + (dst & 0x1Fu) * inverse) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// --------------------------------------------------------------------------
// JPEG(背景 / 事件 CG)
// --------------------------------------------------------------------------

static bool decode_jpeg(sanoba_image_t *img, const sanoba_asset_t *asset)
{
    if (asset->w != SANOBA_ART_W || asset->h != SANOBA_ART_H) {
        ESP_LOGE(TAG, "%s 尺寸与画布不符: %ux%u", asset->name, (unsigned)asset->w, (unsigned)asset->h);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)asset->data,
        .indata_size = asset->data_len,
        .outbuf = (uint8_t *)img->pixels,
        .outbuf_size = (uint32_t)SANOBA_ART_W * SANOBA_ART_H * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = SANOBA_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    img->last_bg_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG %s 解码失败: %s", asset->name, esp_err_to_name(err));
        return false;
    }
    if (out.width != SANOBA_ART_W || out.height != SANOBA_ART_H) {
        ESP_LOGE(TAG, "JPEG %s 解码尺寸不符: %ux%u", asset->name, (unsigned)out.width,
                 (unsigned)out.height);
        return false;
    }
    ESP_LOGI(TAG, "背景/CG %s %u ms,空闲堆 %u", asset->name, (unsigned)img->last_bg_ms,
             (unsigned)esp_get_free_heap_size());
    return true;
}

// SD 装饰是 JPEG 横条(源工程这 292 张本来就是 240x144 JPEG,量化成调色板 PNG 反而大 4 倍)。
// 宽 == 画布宽且 dx == 0,所以解码目标就是画布上的那一段 —— 不需要额外缓冲。
static bool decode_jpeg_strip(sanoba_image_t *img, const sanoba_asset_t *asset)
{
    if (asset->dx != 0 || asset->w != SANOBA_ART_W || asset->h == 0 ||
        (uint32_t)asset->dy + asset->h > SANOBA_ART_H) {
        ESP_LOGE(TAG, "%s 不是可整宽解码的横条: %ux%u @(%u,%u)", asset->name, (unsigned)asset->w,
                 (unsigned)asset->h, (unsigned)asset->dx, (unsigned)asset->dy);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)asset->data,
        .indata_size = asset->data_len,
        .outbuf = (uint8_t *)(img->pixels + (size_t)asset->dy * SANOBA_ART_W),
        .outbuf_size = (uint32_t)asset->w * asset->h * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = SANOBA_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG SD %s 解码失败: %s", asset->name, esp_err_to_name(err));
        return false;
    }
    if (out.width != asset->w || out.height != asset->h) {
        ESP_LOGE(TAG, "JPEG SD %s 解码尺寸不符: %ux%u", asset->name, (unsigned)out.width,
                 (unsigned)out.height);
        return false;
    }
    ESP_LOGI(TAG, "SD %s %ux%u @y=%u, %u ms", asset->name, (unsigned)asset->w, (unsigned)asset->h,
             (unsigned)asset->dy, (unsigned)ms);
    return true;
}

static bool payload_is_jpeg(const sanoba_asset_t *asset)
{
    return asset->data_len >= 2 && asset->data[0] == 0xFF && asset->data[1] == 0xD8;
}

// --------------------------------------------------------------------------
// 分块调色板图像(立绘 / SD / 特效)
// --------------------------------------------------------------------------

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t palette_count;
    uint16_t block_rows;
    uint16_t block_count;
    uint32_t raw_block_bytes;
    const uint8_t *palette;     // u16 RGB565 × palette_count
    const uint8_t *alpha;       // u8 × palette_count
    const uint8_t *block_lens;  // u32 × block_count
    const uint8_t *blocks;      // 各块 zlib 流
} blocked_info_t;

static bool blocked_read_info(const sanoba_asset_t *asset, blocked_info_t *info)
{
    if (asset->data == NULL || asset->data_len < BLOCKED_HEADER) {
        return false;
    }
    const uint8_t *p = asset->data;
    info->width = rd16(p);
    info->height = rd16(p + 2);
    info->palette_count = rd16(p + 4);
    info->block_rows = rd16(p + 6);
    info->block_count = rd16(p + 8);
    info->raw_block_bytes = rd32(p + 12);
    if (info->width == 0 || info->height == 0 || info->palette_count == 0 ||
        info->palette_count > 256 || info->block_rows == 0 || info->block_count == 0) {
        ESP_LOGE(TAG, "%s: 分块格式头部不合法", asset->name);
        return false;
    }
    if (info->raw_block_bytes != (uint32_t)info->block_rows * (info->width + 1u)) {
        ESP_LOGE(TAG, "%s: raw_block_bytes 与 block_rows/width 不符", asset->name);
        return false;
    }
    const uint32_t expected_blocks = (info->height + info->block_rows - 1u) / info->block_rows;
    if (info->block_count != expected_blocks) {
        ESP_LOGE(TAG, "%s: block_count %u != %u", asset->name, (unsigned)info->block_count,
                 (unsigned)expected_blocks);
        return false;
    }
    const uint32_t palette_bytes = (uint32_t)info->palette_count * 2u;
    const uint32_t alpha_bytes = info->palette_count;
    const uint32_t lens_bytes = (uint32_t)info->block_count * 4u;
    if (BLOCKED_HEADER + palette_bytes + alpha_bytes + lens_bytes > asset->data_len) {
        ESP_LOGE(TAG, "%s: 分块格式长度不够", asset->name);
        return false;
    }
    info->palette = p + BLOCKED_HEADER;
    info->alpha = info->palette + palette_bytes;
    info->block_lens = info->alpha + alpha_bytes;
    info->blocks = info->block_lens + lens_bytes;
    uint32_t available = asset->data_len - (uint32_t)(info->blocks - asset->data);
    for (uint16_t block = 0; block < info->block_count; block++) {
        const uint32_t length = rd32(info->block_lens + (uint32_t)block * 4u);
        if (length > available) {
            ESP_LOGE(TAG, "%s: 第 %u 块越界", asset->name, (unsigned)block);
            return false;
        }
        available -= length;
    }
    return true;
}

// 把一行索引反滤波并混进画布;previous 是上一行(首行全 0)
static void blocked_emit_row(sanoba_image_t *img, const blocked_info_t *info, uint8_t *row,
                             const uint8_t *previous, int y, int origin_x, int origin_y,
                             uint32_t *written)
{
    const uint8_t filter = row[0];
    uint8_t *indices = row + 1;
    const int dst_y = origin_y + y;
    uint16_t *dst_row = NULL;
    if (dst_y >= 0 && dst_y < SANOBA_ART_H) {
        dst_row = img->pixels + (size_t)dst_y * SANOBA_ART_W;
    }
    for (uint32_t x = 0; x < info->width; x++) {
        const uint8_t left = x ? indices[x - 1] : 0;
        const uint8_t up = previous[x];
        const uint8_t up_left = x ? previous[x - 1] : 0;
        uint8_t value = indices[x];
        switch (filter) {
        case 0:
            break;
        case 1:
            value = (uint8_t)(value + left);
            break;
        case 2:
            value = (uint8_t)(value + up);
            break;
        case 3:
            value = (uint8_t)(value + ((left + up) >> 1));
            break;
        case 4: {
            const int p = (int)left + (int)up - (int)up_left;
            const int pa = p > (int)left ? p - (int)left : (int)left - p;
            const int pb = p > (int)up ? p - (int)up : (int)up - p;
            const int pc = p > (int)up_left ? p - (int)up_left : (int)up_left - p;
            const uint8_t predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : up_left);
            value = (uint8_t)(value + predictor);
            break;
        }
        default:
            value = 0;   // 未知滤波类型:当 0 处理,不让整幅画报废
            break;
        }
        indices[x] = value;
        if (dst_row == NULL) {
            continue;
        }
        const int dst_x = origin_x + (int)x;
        if (dst_x < 0 || dst_x >= SANOBA_ART_W || value >= info->palette_count) {
            continue;
        }
        const uint8_t alpha = info->alpha[value];
        if (alpha == 0) {
            continue;
        }
        const uint16_t color = rd16(info->palette + (uint32_t)value * 2u);
        uint16_t *target = dst_row + dst_x;
        *target = blend565(*target, color, alpha);
        (*written)++;
    }
}

static bool blocked_blit(sanoba_image_t *img, const sanoba_asset_t *asset, int origin_x, int origin_y,
                         uint32_t *written_out)
{
    blocked_info_t info;
    if (!blocked_read_info(asset, &info)) {
        return false;
    }
    const uint32_t row_bytes = (uint32_t)info.width + 1u;
    uint8_t *scratch = (uint8_t *)malloc(info.raw_block_bytes);
    uint8_t *previous = (uint8_t *)malloc(info.width);
    if (scratch == NULL || previous == NULL) {
        ESP_LOGE(TAG, "%s: 行缓冲分配失败(空闲堆 %u)", asset->name,
                 (unsigned)esp_get_free_heap_size());
        free(scratch);
        free(previous);
        return false;
    }
    memset(previous, 0, info.width);

    const uint8_t *block = info.blocks;
    uint32_t written = 0;
    int y = 0;
    bool ok = true;
    for (uint16_t index = 0; index < info.block_count && ok; index++) {
        const uint32_t length = rd32(info.block_lens + (uint32_t)index * 4u);
        const uint32_t produced = sanoba_inflate(block, length, scratch, info.raw_block_bytes);
        block += length;
        if (produced == 0 || produced % row_bytes != 0) {
            ESP_LOGE(TAG, "%s: 第 %u 块解压异常(%u 字节)", asset->name, (unsigned)index,
                     (unsigned)produced);
            ok = false;
            break;
        }
        const uint32_t rows = produced / row_bytes;
        for (uint32_t row = 0; row < rows; row++) {
            uint8_t *line = scratch + (size_t)row * row_bytes;
            blocked_emit_row(img, &info, line, previous, y, origin_x, origin_y, &written);
            memcpy(previous, line + 1, info.width);
            y++;
        }
    }
    free(scratch);
    free(previous);
    if (ok && y != info.height) {
        ESP_LOGE(TAG, "%s: 行数不符(%d/%u)", asset->name, y, (unsigned)info.height);
        ok = false;
    }
    if (written_out) {
        *written_out = written;
    }
    return ok;
}

// --------------------------------------------------------------------------
// 事件 CG 掩码补丁
// --------------------------------------------------------------------------

// 像素块解压出来的字节流:掩码说哪些位置要覆写,像素按扫描序从这里取
typedef struct {
    sanoba_image_t *img;
    const sanoba_asset_t *asset;
    const uint8_t *mask;
    uint32_t row_bytes;
    const uint8_t *blocks;
    const uint8_t *block_lens;
    uint32_t block_count;
    uint32_t block_index;
    uint32_t block_left;      // 当前块剩余未消费的字节
    uint8_t *scratch;         // PATCH_BLOCK_PIXELS * 2 字节
    uint32_t scratch_used;
    uint8_t pair[2];
    uint32_t pair_have;
    uint32_t y;
    uint32_t x;
    uint32_t written;
} patch_stream_t;

static bool patch_next_block(patch_stream_t *state)
{
    if (state->block_index >= state->block_count) {
        return false;
    }
    const uint32_t length = rd32(state->block_lens + state->block_index * 4u);
    const uint32_t produced =
        sanoba_inflate(state->blocks, length, state->scratch, PATCH_BLOCK_PIXELS * 2u);
    state->blocks += length;
    state->block_index++;
    if (produced == 0) {
        return false;
    }
    state->scratch_used = produced;
    state->block_left = produced;
    return true;
}

// 取下一个像素(2 字节小端);没有更多数据返回 false
static bool patch_next_pixel_bytes(patch_stream_t *state, uint8_t out[2])
{
    for (int index = 0; index < 2; index++) {
        if (state->block_left == 0 && !patch_next_block(state)) {
            return false;
        }
        out[index] = state->scratch[state->scratch_used - state->block_left];
        state->block_left--;
    }
    return true;
}

// 生成下一个置位像素的位置(行内 x 递增,行末换行)
static bool patch_next_position(patch_stream_t *state, uint32_t *out_y, uint32_t *out_x)
{
    while (state->y < state->asset->dh) {
        while (state->x < state->asset->dw) {
            const uint8_t byte = state->mask[(size_t)state->y * state->row_bytes + (state->x >> 3)];
            const uint32_t x = state->x++;
            if (((byte >> (7 - (x & 7))) & 1u) != 0) {
                *out_y = state->y;
                *out_x = x;
                return true;
            }
        }
        state->x = 0;
        state->y++;
    }
    return false;
}

static bool apply_patch(sanoba_image_t *img, const sanoba_asset_t *asset)
{
    if (asset->data == NULL || asset->data_len < 12 || asset->dw == 0 || asset->dh == 0) {
        ESP_LOGE(TAG, "%s: 补丁载荷不完整", asset->name);
        return false;
    }
    const uint8_t *p = asset->data;
    const uint32_t mask_len = rd32(p);
    const uint32_t block_count = rd32(p + 4);
    const uint32_t lens_bytes = block_count * 4u;
    if (block_count == 0 || 8u + lens_bytes + mask_len > asset->data_len) {
        ESP_LOGE(TAG, "%s: 补丁头长度不合法", asset->name);
        return false;
    }
    const uint8_t *mask_stream = p + 8 + lens_bytes;
    const uint8_t *block_lens = p + 8;
    const uint8_t *blocks = mask_stream + mask_len;

    const uint32_t row_bytes = (asset->dw + 7u) / 8u;
    const uint32_t mask_size = row_bytes * asset->dh;
    uint8_t *mask = (uint8_t *)malloc(mask_size);
    uint8_t *scratch = (uint8_t *)malloc(PATCH_BLOCK_PIXELS * 2u);
    if (mask == NULL || scratch == NULL) {
        ESP_LOGE(TAG, "%s: 补丁缓冲分配失败(空闲堆 %u)", asset->name,
                 (unsigned)esp_get_free_heap_size());
        free(mask);
        free(scratch);
        return false;
    }
    const uint32_t mask_produced = sanoba_inflate(mask_stream, mask_len, mask, mask_size);
    if (mask_produced != mask_size) {
        ESP_LOGE(TAG, "%s: 掩码解压失败(%u/%u)", asset->name, (unsigned)mask_produced,
                 (unsigned)mask_size);
        free(mask);
        free(scratch);
        return false;
    }

    patch_stream_t state = {
        .img = img, .asset = asset, .mask = mask, .row_bytes = row_bytes,
        .blocks = blocks, .block_lens = block_lens, .block_count = block_count,
        .block_index = 0, .block_left = 0, .scratch = scratch, .scratch_used = 0,
        .pair_have = 0, .y = 0, .x = 0, .written = 0,
    };
    bool ok = true;
    for (;;) {
        uint32_t y = 0;
        uint32_t x = 0;
        if (!patch_next_position(&state, &y, &x)) {
            break;   // 掩码走完
        }
        uint8_t pair[2];
        if (!patch_next_pixel_bytes(&state, pair)) {
            ESP_LOGE(TAG, "%s: 像素流比掩码短(已写 %u)", asset->name, (unsigned)state.written);
            ok = false;
            break;
        }
        const int dst_y = (int)asset->dy + (int)y;
        const int dst_x = (int)asset->dx + (int)x;
        if (dst_y >= 0 && dst_y < SANOBA_ART_H && dst_x >= 0 && dst_x < SANOBA_ART_W) {
            img->pixels[(size_t)dst_y * SANOBA_ART_W + (size_t)dst_x] =
                (uint16_t)(pair[0] | ((uint16_t)pair[1] << 8));
            state.written++;
        }
    }
    free(mask);
    free(scratch);
    if (ok) {
        ESP_LOGI(TAG, "CG 补丁 %s %ux%u@%u,%u -> %u 像素", asset->name, (unsigned)asset->dw,
                 (unsigned)asset->dh, (unsigned)asset->dx, (unsigned)asset->dy,
                 (unsigned)state.written);
    }
    return ok;
}

// --------------------------------------------------------------------------
// 图层
// --------------------------------------------------------------------------

static bool draw_background(sanoba_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        fill_canvas(img, 0);   // 黑屏(章节过场等)
        return true;
    }
    sanoba_asset_t asset;
    if (!sanoba_pack_find(img->pack, SANOBA_POOL_BG, name, &asset) || asset.kind != SANOBA_KIND_BG) {
        ESP_LOGW(TAG, "找不到背景: %s", name);
        return false;
    }
    return decode_jpeg(img, &asset);
}

// 事件图位置:CG / 特效是整屏,SD 装饰是固定位置的小图
static bool draw_event(sanoba_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        return true;
    }
    sanoba_asset_t asset;
    if (!sanoba_pack_find(img->pack, SANOBA_POOL_EV, name, &asset)) {
        ESP_LOGW(TAG, "找不到事件图: %s", name);
        return false;
    }
    if (asset.alias) {
        sanoba_asset_t base;
        if (!sanoba_pack_at(img->pack, asset.base, &base)) {
            return false;
        }
        asset = base;
    }
    switch (asset.kind) {
    case SANOBA_KIND_CG:
        return decode_jpeg(img, &asset);
    case SANOBA_KIND_CG_DIFF: {
        sanoba_asset_t base;
        if (!sanoba_pack_at(img->pack, asset.base, &base) || !decode_jpeg(img, &base)) {
            return false;
        }
        const int64_t start = esp_timer_get_time();
        const bool patched = apply_patch(img, &asset);
        ESP_LOGI(TAG, "CG 补丁 %s -> %s, %u ms", asset.name, patched ? "ok" : "失败",
                 (unsigned)((esp_timer_get_time() - start) / 1000));
        return patched;
    }
    case SANOBA_KIND_SD:
        // 本项目的 SD 是 JPEG 横条(见 tools/sanoba_pack.py);palette 模式生成的
        // 调色板 PNG 走原来的逐行路径,两种载荷靠文件头区分。
        if (payload_is_jpeg(&asset)) {
            return decode_jpeg_strip(img, &asset);
        }
        return blocked_blit(img, &asset, (int)asset.dx, (int)asset.dy, NULL);
    case SANOBA_KIND_EFFECT:
        return blocked_blit(img, &asset, 0, 0, NULL);
    case SANOBA_KIND_MISSING:
        return true;   // 被排除的素材:跳过绘制,不报错
    default:
        ESP_LOGW(TAG, "事件图 %s 类型不支持: %u", name, (unsigned)asset.kind);
        return false;
    }
}

static bool draw_sprite(sanoba_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        return true;
    }
    sanoba_asset_t asset;
    if (!sanoba_pack_find(img->pack, SANOBA_POOL_CH, name, &asset)) {
        ESP_LOGW(TAG, "找不到立绘: %s", name);
        return false;
    }
    const int x = (SANOBA_ART_W - (int)asset.w) / 2;
    const int y = SANOBA_ART_H - (int)asset.h;
    const int64_t start = esp_timer_get_time();
    uint32_t written = 0;
    const bool ok = blocked_blit(img, &asset, x, y, &written);
    img->last_sprite_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "立绘 %s %ux%u @(%d,%d) -> %u 像素, %u ms, 空闲堆 %u", name, (unsigned)asset.w,
             (unsigned)asset.h, x, y, (unsigned)written, (unsigned)img->last_sprite_ms,
             (unsigned)esp_get_free_heap_size());
    return ok;
}

void sanoba_image_compose(sanoba_image_t *img, const char *bg, const char *sd, const char *sprite,
                        const char *ev)
{
    if (!img || !img->canvas || !img->pack) {
        return;
    }
    draw_background(img, bg);
    draw_event(img, sd);
    draw_sprite(img, sprite);
    draw_event(img, ev);
    sanoba_image_draw_band(img);
    canvas_invalidate(img);
}

// --------------------------------------------------------------------------
// 正文带与暗帘
// --------------------------------------------------------------------------

static void draw_gradient(sanoba_image_t *img, unsigned red, unsigned green, unsigned blue,
                          unsigned alpha_top, unsigned alpha_bottom)
{
    const unsigned span = (unsigned)(SANOBA_ART_H - SANOBA_BAND_Y - 1);
    const uint16_t source = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    for (int y = SANOBA_BAND_Y; y < SANOBA_ART_H; y++) {
        const unsigned t = span ? (unsigned)(y - SANOBA_BAND_Y) * 255u / span : 255u;
        const unsigned alpha = alpha_top + (alpha_bottom - alpha_top) * t / 255u;
        uint16_t *row = img->pixels + (size_t)y * SANOBA_ART_W;
        for (int x = 0; x < SANOBA_ART_W; x++) {
            row[x] = blend565(row[x], source, alpha);
        }
    }
}

void sanoba_image_draw_band(sanoba_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    draw_gradient(img, SANOBA_BAND_R, SANOBA_BAND_G, SANOBA_BAND_B, SANOBA_BAND_ALPHA_TOP,
                  SANOBA_BAND_ALPHA_BOTTOM);
    draw_gradient(img, SANOBA_SCRIM_R, SANOBA_SCRIM_G, SANOBA_SCRIM_B, SANOBA_SCRIM_ALPHA_TOP,
                  SANOBA_SCRIM_ALPHA_BOTTOM);
    canvas_invalidate(img);
}
