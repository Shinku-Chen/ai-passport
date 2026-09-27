// main/atri_image.c —— 画面区合成实现。
//
// 三套解码路径,共同点是"没有中间大缓冲":
//   1. 背景 / 事件 CG:JPEG 240x320,esp_jpeg 直接解进画布(输出缓冲就是画布);
//   2. 立绘 / SD / 特效:调色板 PNG,逐行解压 -> 反滤波 -> 查调色板 -> 混进画布,
//      只占一个 32 KB 环形字典(见 senren_inflate.c)加两行缓冲;
//   3. 事件 CG 补丁:先解基准 JPEG,再按 1bpp 掩码把 RGB565 像素逐行覆写上去
//      (掩码整段解进一块 bbox/8 字节的缓冲,像素流边解边写)。
#include "atri_image.h"

#include "senren_inflate.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "senren_img";

// esp_jpeg 在 C3 上走 ROM 里的 TJpgDec,输出 RGB565 时由它做 888->565 转换:
// swap=false 写 LOBYTE 在前 = 小端,正好是 LVGL 要的原生 16 位字节序。
#define SENREN_JPEG_SWAP_BYTES 0

// 正文带颜色(源工程 text_bg 的蓝底,压深一点配和风画面)与上下透明度
#define SENREN_BAND_R 42
#define SENREN_BAND_G 60
#define SENREN_BAND_B 110
#define SENREN_BAND_ALPHA_TOP 120
#define SENREN_BAND_ALPHA_BOTTOM 236
// 文字区浅暗帘:压在文字下面,保证白字在任何画面上都能读
#define SENREN_SCRIM_R 6
#define SENREN_SCRIM_G 12
#define SENREN_SCRIM_B 20
#define SENREN_SCRIM_ALPHA_TOP 24
#define SENREN_SCRIM_ALPHA_BOTTOM 96

static void canvas_invalidate(atri_image_t *img)
{
    if (img->canvas) {
        lv_obj_invalidate(img->canvas);
    }
}

static void fill_canvas(atri_image_t *img, uint16_t color)
{
    for (size_t index = 0; index < (size_t)ATRI_ART_W * ATRI_ART_H; index++) {
        img->pixels[index] = color;
    }
}

bool atri_image_init(atri_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
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
    lv_canvas_set_buffer(img->canvas, pixels, ATRI_ART_W, ATRI_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    return true;
}

void atri_image_set_pack(atri_image_t *img, const senren_pack_t *pack)
{
    if (img) {
        img->pack = pack;
    }
}

void atri_image_clear(atri_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    fill_canvas(img, 0);
    canvas_invalidate(img);
}

uint16_t *atri_image_canvas(atri_image_t *img)
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

static bool decode_jpeg(atri_image_t *img, const senren_asset_t *asset)
{
    if (asset->w != ATRI_ART_W || asset->h != ATRI_ART_H) {
        ESP_LOGE(TAG, "%s 尺寸与画布不符: %ux%u", asset->name, (unsigned)asset->w, (unsigned)asset->h);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)asset->data,
        .indata_size = asset->data_len,
        .outbuf = (uint8_t *)img->pixels,
        .outbuf_size = (uint32_t)ATRI_ART_W * ATRI_ART_H * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = SENREN_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    img->last_bg_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG %s 解码失败: %s", asset->name, esp_err_to_name(err));
        return false;
    }
    if (out.width != ATRI_ART_W || out.height != ATRI_ART_H) {
        ESP_LOGE(TAG, "JPEG %s 解码尺寸不符: %ux%u", asset->name, (unsigned)out.width,
                 (unsigned)out.height);
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// 调色板 PNG(立绘 / SD / 特效)
// --------------------------------------------------------------------------

#define PNG_TYPE(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t offset;
} png_walker_t;

static bool png_next_chunk(png_walker_t *walker, uint32_t *type, const uint8_t **body,
                           uint32_t *body_len)
{
    // chunk = 长度 u32 + 类型 4B + 数据 + CRC 4B
    if (walker->offset + 12 > walker->len) {
        return false;
    }
    const uint8_t *header = walker->data + walker->offset;
    uint32_t length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                      ((uint32_t)header[2] << 8) | header[3];
    if (walker->offset + 12 + length > walker->len) {
        return false;
    }
    *type = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) | ((uint32_t)header[6] << 8) |
            header[7];
    *body = header + 8;
    *body_len = length;
    walker->offset += 12 + length;
    return true;
}

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t palette_count;
    uint16_t palette565[256];
    uint8_t palette_alpha[256];
    const uint8_t *idat;
    uint32_t idat_len;
} png_info_t;

static uint16_t expand565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

static bool png_read_info(const senren_asset_t *asset, png_info_t *info)
{
    memset(info, 0, sizeof(*info));
    png_walker_t walker = { asset->data, asset->data_len, 0 };
    bool have_header = false;
    bool have_palette = false;
    uint32_t type = 0;
    const uint8_t *body = NULL;
    uint32_t body_len = 0;
    uint32_t idat_offset = 0;
    uint32_t idat_length = 0;
    while (png_next_chunk(&walker, &type, &body, &body_len)) {
        if (type == PNG_TYPE('I', 'H', 'D', 'R')) {
            if (body_len < 13) {
                return false;
            }
            info->width = (uint16_t)(((uint16_t)body[0] << 8) | body[1]);
            info->height = (uint16_t)(((uint16_t)body[2] << 8) | body[3]);
            const uint8_t depth = body[8];
            const uint8_t color_type = body[9];
            const uint8_t interlace = body[12];
            // 只支持 8 位调色板、非隔行 —— tools/senren_pack.py 生成的就是这种
            if (depth != 8 || color_type != 3 || interlace != 0) {
                ESP_LOGE(TAG, "%s: 不支持的 PNG(深度 %u 类型 %u 隔行 %u)", asset->name, depth,
                         color_type, interlace);
                return false;
            }
            have_header = true;
        } else if (type == PNG_TYPE('P', 'L', 'T', 'E')) {
            const uint32_t entries = body_len / 3;
            if (entries == 0 || entries > 256) {
                return false;
            }
            info->palette_count = (uint16_t)entries;
            for (uint32_t index = 0; index < entries; index++) {
                info->palette565[index] = expand565(body[index * 3], body[index * 3 + 1],
                                                    body[index * 3 + 2]);
                info->palette_alpha[index] = 255;
            }
            have_palette = true;
        } else if (type == PNG_TYPE('t', 'R', 'N', 'S')) {
            for (uint32_t index = 0; index < body_len && index < 256; index++) {
                info->palette_alpha[index] = body[index];
            }
        } else if (type == PNG_TYPE('I', 'D', 'A', 'T')) {
            // IDAT 可能分成多块;它们在文件里首尾相接,所以只记起点与总长
            if (idat_length == 0) {
                idat_offset = (uint32_t)(body - asset->data);
            }
            idat_length += body_len;
        }
    }
    if (!have_header || !have_palette || idat_length == 0) {
        ESP_LOGE(TAG, "%s: PNG 结构不完整", asset->name);
        return false;
    }
    info->idat = asset->data + idat_offset;
    info->idat_len = idat_length;
    return true;
}

// 逐行解一张调色板 PNG:解压流按行喂给本结构,一行凑齐就反滤波 + 混进画布。
typedef struct {
    atri_image_t *img;
    const png_info_t *info;
    uint32_t width;
    uint8_t *row;        // width + 1(首字节是滤波类型)
    uint8_t *previous;   // width
    uint32_t have;       // 当前行已收到多少字节
    uint32_t y;
    uint32_t written;
    int origin_x;
    int origin_y;
    bool failed;
} png_stream_t;

static bool png_emit_row(png_stream_t *state)
{
    const uint8_t filter = state->row[0];
    uint8_t *indices = state->row + 1;
    if (filter > 4) {
        return false;
    }
    for (uint32_t x = 0; x < state->width; x++) {
        const uint8_t left = x ? indices[x - 1] : 0;
        const uint8_t up = state->previous[x];
        const uint8_t up_left = x ? state->previous[x - 1] : 0;
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
        default: {
            const int p = (int)left + (int)up - (int)up_left;
            const int pa = p > (int)left ? p - (int)left : (int)left - p;
            const int pb = p > (int)up ? p - (int)up : (int)up - p;
            const int pc = p > (int)up_left ? p - (int)up_left : (int)up_left - p;
            const uint8_t predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : up_left);
            value = (uint8_t)(value + predictor);
            break;
        }
        }
        indices[x] = value;

        const int dst_y = state->origin_y + (int)state->y;
        if (dst_y < 0 || dst_y >= ATRI_ART_H) {
            continue;
        }
        const int dst_x = state->origin_x + (int)x;
        if (dst_x < 0 || dst_x >= ATRI_ART_W) {
            continue;
        }
        if (value >= state->info->palette_count) {
            continue;
        }
        const uint8_t alpha = state->info->palette_alpha[value];
        if (alpha == 0) {
            continue;
        }
        uint16_t *target = state->img->pixels + (size_t)dst_y * ATRI_ART_W + (size_t)dst_x;
        *target = blend565(*target, state->info->palette565[value], alpha);
        state->written++;
    }
    memcpy(state->previous, indices, state->width);
    return true;
}

static bool png_sink_bytes(const uint8_t *data, uint32_t length, void *context)
{
    png_stream_t *state = (png_stream_t *)context;
    while (length > 0) {
        if (state->y >= state->info->height) {
            state->failed = true;   // 数据比声明尺寸多
            return false;
        }
        const uint32_t need = state->width + 1u - state->have;
        const uint32_t take = length < need ? length : need;
        memcpy(state->row + state->have, data, take);
        state->have += take;
        data += take;
        length -= take;
        if (state->have == state->width + 1u) {
            if (!png_emit_row(state)) {
                state->failed = true;
                return false;
            }
            state->have = 0;
            state->y++;
        }
    }
    return true;
}

// 逐行解一张调色板 PNG 并混进画布。origin_x/origin_y 是左上角在画布里的位置。
static bool png_blit(atri_image_t *img, const senren_asset_t *asset, int origin_x, int origin_y,
                     uint32_t *written_out)
{
    png_info_t info;
    if (!png_read_info(asset, &info)) {
        return false;
    }
    const uint32_t width = info.width;
    if (width == 0 || width > 1024 || info.height == 0) {
        return false;
    }
    uint8_t *row = (uint8_t *)malloc(width + 1u);
    uint8_t *previous = (uint8_t *)malloc(width ? width : 1u);
    if (!row || !previous) {
        free(row);
        free(previous);
        return false;
    }
    memset(previous, 0, width);

    png_stream_t state = {
        .img = img, .info = &info, .width = width, .row = row, .previous = previous,
        .have = 0, .y = 0, .written = 0, .origin_x = origin_x, .origin_y = origin_y,
        .failed = false,
    };
    const bool ok = senren_inflate_stream(info.idat, info.idat_len, png_sink_bytes, &state);
    const uint32_t written = state.written;
    free(row);
    free(previous);
    if (!ok || state.failed || state.y != info.height) {
        ESP_LOGE(TAG, "%s: PNG 解码失败(解出 %u/%u 行)", asset->name, (unsigned)state.y,
                 (unsigned)info.height);
        return false;
    }
    if (written_out) {
        *written_out = written;
    }
    return true;
}

// --------------------------------------------------------------------------
// 事件 CG 掩码补丁
// --------------------------------------------------------------------------

typedef struct {
    atri_image_t *img;
    const senren_asset_t *asset;
    const uint8_t *mask;
    uint32_t row_bytes;
    uint32_t y;        // 下一个待填像素所在行
    uint32_t x;        // 该行的下一个待查位
    uint8_t pair[2];
    uint32_t have;
    uint32_t written;
    bool failed;
} patch_stream_t;

// 找下一个置位像素(按扫描序);找不到返回 false(像素流比掩码长)
static bool patch_next_pixel(patch_stream_t *state, uint32_t *out_y, uint32_t *out_x)
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

static bool patch_sink_bytes(const uint8_t *data, uint32_t length, void *context)
{
    patch_stream_t *state = (patch_stream_t *)context;
    while (length > 0) {
        state->pair[state->have++] = *data++;
        length--;
        if (state->have < 2) {
            continue;
        }
        state->have = 0;
        uint32_t y = 0;
        uint32_t x = 0;
        if (!patch_next_pixel(state, &y, &x)) {
            state->failed = true;
            return false;   // 像素比掩码多
        }
        const int dst_y = (int)state->asset->dy + (int)y;
        const int dst_x = (int)state->asset->dx + (int)x;
        if (dst_y >= 0 && dst_y < ATRI_ART_H && dst_x >= 0 && dst_x < ATRI_ART_W) {
            state->img->pixels[(size_t)dst_y * ATRI_ART_W + (size_t)dst_x] =
                (uint16_t)(state->pair[0] | ((uint16_t)state->pair[1] << 8));
            state->written++;
        }
    }
    return true;
}

// 掩码整段解进 bbox/8 字节的缓冲,像素流按扫描序边解边覆写。
static bool apply_patch(atri_image_t *img, const senren_asset_t *asset)
{
    const uint8_t *packed_mask = NULL;
    const uint8_t *packed_pixels = NULL;
    uint32_t packed_mask_len = 0;
    uint32_t packed_pixels_len = 0;
    if (!senren_patch_parts(asset, &packed_mask, &packed_mask_len, &packed_pixels, &packed_pixels_len)) {
        ESP_LOGE(TAG, "%s: 补丁载荷不完整", asset->name);
        return false;
    }
    const uint32_t width = asset->dw;
    const uint32_t height = asset->dh;
    const uint32_t row_bytes = (width + 7u) / 8u;
    const uint32_t mask_size = row_bytes * height;
    uint8_t *mask = (uint8_t *)malloc(mask_size ? mask_size : 1u);
    if (!mask) {
        return false;
    }
    const uint32_t mask_produced = senren_inflate(packed_mask, packed_mask_len, mask, mask_size);
    if (mask_produced != mask_size) {
        ESP_LOGE(TAG, "%s: 掩码解压失败(%u/%u)", asset->name, (unsigned)mask_produced,
                 (unsigned)mask_size);
        free(mask);
        return false;
    }

    patch_stream_t state = {
        .img = img, .asset = asset, .mask = mask, .row_bytes = row_bytes,
        .y = 0, .x = 0, .have = 0, .written = 0, .failed = false,
    };
    const bool ok = senren_inflate_stream(packed_pixels, packed_pixels_len, patch_sink_bytes, &state);
    free(mask);
    if (!ok || state.failed) {
        ESP_LOGE(TAG, "%s: 像素流解压失败(已写 %u)", asset->name, (unsigned)state.written);
        return false;
    }
    ESP_LOGI(TAG, "CG 补丁 %s %ux%u@%u,%u -> %u 像素", asset->name, (unsigned)width,
             (unsigned)height, (unsigned)asset->dx, (unsigned)asset->dy, (unsigned)state.written);
    return true;
}

// --------------------------------------------------------------------------
// 图层
// --------------------------------------------------------------------------

static bool draw_background(atri_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        fill_canvas(img, 0);   // 黑屏(章节过场等)
        return true;
    }
    senren_asset_t asset;
    if (!senren_pack_find(img->pack, SENREN_POOL_BG, name, &asset) || asset.kind != SENREN_KIND_BG) {
        ESP_LOGW(TAG, "找不到背景: %s", name);
        return false;
    }
    return decode_jpeg(img, &asset);
}

// 事件图位置:CG / 特效是整屏,SD 装饰是固定位置的小图
static bool draw_event(atri_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        return true;
    }
    senren_asset_t asset;
    if (!senren_pack_find(img->pack, SENREN_POOL_EV, name, &asset)) {
        ESP_LOGW(TAG, "找不到事件图: %s", name);
        return false;
    }
    if (asset.alias) {
        senren_asset_t base;
        if (!senren_pack_at(img->pack, asset.base, &base)) {
            return false;
        }
        asset = base;
    }
    switch (asset.kind) {
    case SENREN_KIND_CG:
        return decode_jpeg(img, &asset);
    case SENREN_KIND_CG_DIFF: {
        senren_asset_t base;
        if (!senren_pack_at(img->pack, asset.base, &base) || !decode_jpeg(img, &base)) {
            return false;
        }
        const int64_t start = esp_timer_get_time();
        const bool patched = apply_patch(img, &asset);
        ESP_LOGI(TAG, "CG 补丁 %s -> %s, %u ms", asset.name, patched ? "ok" : "失败",
                 (unsigned)((esp_timer_get_time() - start) / 1000));
        return patched;
    }
    case SENREN_KIND_SD:
        return png_blit(img, &asset, SENREN_SD_X, SENREN_SD_Y, NULL);
    case SENREN_KIND_EFFECT:
        return png_blit(img, &asset, 0, 0, NULL);
    case SENREN_KIND_MISSING:
        return true;   // 被排除的素材:跳过绘制,不报错
    default:
        ESP_LOGW(TAG, "事件图 %s 类型不支持: %u", name, (unsigned)asset.kind);
        return false;
    }
}

static bool draw_sprite(atri_image_t *img, const char *name)
{
    if (!name || name[0] == '\0') {
        return true;
    }
    senren_asset_t asset;
    if (!senren_pack_find(img->pack, SENREN_POOL_CH, name, &asset)) {
        ESP_LOGW(TAG, "找不到立绘: %s", name);
        return false;
    }
    const int x = (ATRI_ART_W - (int)asset.w) / 2;
    const int y = ATRI_ART_H - (int)asset.h;
    const int64_t start = esp_timer_get_time();
    uint32_t written = 0;
    const bool ok = png_blit(img, &asset, x, y, &written);
    img->last_sprite_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "立绘 %s %ux%u @(%d,%d) -> %u 像素, %u ms", name, (unsigned)asset.w,
             (unsigned)asset.h, x, y, (unsigned)written, (unsigned)img->last_sprite_ms);
    return ok;
}

void atri_image_compose(atri_image_t *img, const char *bg, const char *sd, const char *sprite,
                        const char *ev)
{
    if (!img || !img->canvas || !img->pack) {
        return;
    }
    draw_background(img, bg);
    draw_event(img, sd);
    draw_sprite(img, sprite);
    draw_event(img, ev);
    atri_image_draw_band(img);
    canvas_invalidate(img);
}

// --------------------------------------------------------------------------
// 正文带与暗帘
// --------------------------------------------------------------------------

static void draw_gradient(atri_image_t *img, unsigned red, unsigned green, unsigned blue,
                          unsigned alpha_top, unsigned alpha_bottom)
{
    const unsigned span = (unsigned)(ATRI_ART_H - ATRI_BAND_Y - 1);
    const uint16_t source = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    for (int y = ATRI_BAND_Y; y < ATRI_ART_H; y++) {
        const unsigned t = span ? (unsigned)(y - ATRI_BAND_Y) * 255u / span : 255u;
        const unsigned alpha = alpha_top + (alpha_bottom - alpha_top) * t / 255u;
        uint16_t *row = img->pixels + (size_t)y * ATRI_ART_W;
        for (int x = 0; x < ATRI_ART_W; x++) {
            row[x] = blend565(row[x], source, alpha);
        }
    }
}

void atri_image_draw_band(atri_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    draw_gradient(img, SENREN_BAND_R, SENREN_BAND_G, SENREN_BAND_B, SENREN_BAND_ALPHA_TOP,
                  SENREN_BAND_ALPHA_BOTTOM);
    draw_gradient(img, SENREN_SCRIM_R, SENREN_SCRIM_G, SENREN_SCRIM_B, SENREN_SCRIM_ALPHA_TOP,
                  SENREN_SCRIM_ALPHA_BOTTOM);
    canvas_invalidate(img);
}
