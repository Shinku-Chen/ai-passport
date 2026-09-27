// main/dracu_image.c —— 画面区合成实现。
//
// 与 tools/dracu_pack.py 的三套载荷一一对应:JPEG(背景/事件 CG)、分块调色板
// (立绘/SD)、CG 掩码补丁。为了在没有 PSRAM 的板子上跑起来,这里不申请任何大缓冲:
// JPEG 直接解进画布,分块调色板用一块静态行缓冲,补丁按掩码逐行覆写。
#include "dracu_image.h"

#include "dracu_inflate.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "dracu_img";

// 分块调色板格式的每块行数(必须与 tools/dracu_pack.py 的 BLOCK_ROWS 一致)。
#define DRACU_BLOCK_ROWS 24
// 行缓冲:每块 24 行 x (宽 + 1);另留一行存上一行(反滤波跨行)。
#define DRACU_BLOCK_SCRATCH (DRACU_BLOCK_ROWS * (DRACU_ART_W + 1))
static uint8_t s_block_scratch[DRACU_BLOCK_SCRATCH];
static uint8_t s_previous_row[DRACU_ART_W];

// 正文带颜色(源工程 text_bg 的蓝底)与上下透明度
#define DRACU_BAND_R 42
#define DRACU_BAND_G 60
#define DRACU_BAND_B 110
#define DRACU_BAND_ALPHA_TOP 120
#define DRACU_BAND_ALPHA_BOTTOM 236
// 文字区浅暗帘:压在文字下面,保证白字在任何画面上都能读
#define DRACU_SCRIM_R 6
#define DRACU_SCRIM_G 12
#define DRACU_SCRIM_B 20
#define DRACU_SCRIM_ALPHA_TOP 24
#define DRACU_SCRIM_ALPHA_BOTTOM 96

// 分块调色板格式头部:8 个 u16(见 tools/dracu_pack.py)
#define BLOCKED_HEADER 16u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void canvas_invalidate(dracu_image_t *img)
{
    if (img->canvas) {
        lv_obj_invalidate(img->canvas);
    }
}

static void fill_canvas(dracu_image_t *img, uint16_t color)
{
    for (size_t index = 0; index < (size_t)DRACU_ART_W * DRACU_ART_H; index++) {
        img->pixels[index] = color;
    }
}

bool dracu_image_init(dracu_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                      const dracu_pack_t *pack)
{
    if (!img || !parent || !pixels) {
        return false;
    }
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    img->pack = pack;
    img->shown.bg = DRACU_NONE8;
    img->shown.cg = DRACU_NO_ENTRY;
    img->shown.sd = DRACU_NO_ENTRY;
    img->shown.sprite = DRACU_NO_ENTRY;
    fill_canvas(img, 0);
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, DRACU_ART_W, DRACU_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    return true;
}

uint16_t *dracu_image_pixels(dracu_image_t *img)
{
    return img && img->pixels ? img->pixels : NULL;
}

void dracu_image_clear(dracu_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    fill_canvas(img, 0);
    canvas_invalidate(img);
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

static bool decode_jpeg(dracu_image_t *img, const dracu_asset_t *asset)
{
    if (asset->w != DRACU_ART_W || asset->h != DRACU_ART_H) {
        ESP_LOGE(TAG, "%.*s 尺寸与画布不符: %ux%u", (int)asset->name_len, asset->name,
                 (unsigned)asset->w, (unsigned)asset->h);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)asset->data,
        .indata_size = asset->data_len,
        .outbuf = (uint8_t *)img->pixels,
        .outbuf_size = (uint32_t)DRACU_ART_W * DRACU_ART_H * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    img->last_bg_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG %.*s 解码失败: %s", (int)asset->name_len, asset->name,
                 esp_err_to_name(err));
        return false;
    }
    if (out.width != DRACU_ART_W || out.height != DRACU_ART_H) {
        ESP_LOGE(TAG, "JPEG %.*s 解码尺寸不符: %ux%u", (int)asset->name_len, asset->name,
                 (unsigned)out.width, (unsigned)out.height);
        return false;
    }
    ESP_LOGI(TAG, "画面 %.*s %u ms,空闲堆 %u", (int)asset->name_len, asset->name,
             (unsigned)img->last_bg_ms, (unsigned)esp_get_free_heap_size());
    return true;
}

// --------------------------------------------------------------------------
// 分块调色板图像(立绘 / SD)
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
    const uint8_t *blocks;      // 各块 raw deflate 流
} blocked_info_t;

static bool blocked_read_info(const dracu_asset_t *asset, blocked_info_t *info)
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
        return false;
    }
    if (info->width > DRACU_ART_W || info->block_rows > DRACU_BLOCK_ROWS) {
        return false;
    }
    if (info->raw_block_bytes != (uint32_t)info->block_rows * (info->width + 1u)) {
        return false;
    }
    const uint32_t expected_blocks = (info->height + info->block_rows - 1u) / info->block_rows;
    if (info->block_count != expected_blocks) {
        return false;
    }
    const uint32_t palette_bytes = (uint32_t)info->palette_count * 2u;
    const uint32_t alpha_bytes = info->palette_count;
    const uint32_t lens_bytes = (uint32_t)info->block_count * 4u;
    if (BLOCKED_HEADER + palette_bytes + alpha_bytes + lens_bytes > asset->data_len) {
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
            return false;
        }
        available -= length;
    }
    return true;
}

// 把一行索引反滤波并混进画布;previous 是上一行(首行全 0)
static void blocked_emit_row(dracu_image_t *img, const blocked_info_t *info, uint8_t *row,
                             const uint8_t *previous, int y, int origin_x, int origin_y,
                             uint32_t *written)
{
    const uint8_t filter = row[0];
    uint8_t *indices = row + 1;
    const int dst_y = origin_y + y;
    uint16_t *dst_row = NULL;
    if (dst_y >= 0 && dst_y < DRACU_ART_H) {
        dst_row = img->pixels + (size_t)dst_y * DRACU_ART_W;
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
        if (dst_x < 0 || dst_x >= DRACU_ART_W || value >= info->palette_count) {
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

static bool blocked_blit(dracu_image_t *img, const dracu_asset_t *asset, int origin_x, int origin_y,
                         uint32_t *written_out)
{
    blocked_info_t info;
    if (!blocked_read_info(asset, &info)) {
        ESP_LOGW(TAG, "%.*s: 分块格式头部不合法", (int)asset->name_len, asset->name);
        return false;
    }
    const uint32_t row_bytes = (uint32_t)info.width + 1u;
    uint8_t *scratch = s_block_scratch;
    uint8_t *previous = s_previous_row;
    memset(previous, 0, info.width);

    const uint8_t *block = info.blocks;
    uint32_t written = 0;
    int y = 0;
    bool ok = true;
    for (uint16_t index = 0; index < info.block_count && ok; index++) {
        const uint32_t length = rd32(info.block_lens + (uint32_t)index * 4u);
        const uint32_t produced =
            dracu_inflate(block, length, scratch, DRACU_BLOCK_SCRATCH);
        block += length;
        if (produced == 0 || produced % row_bytes != 0) {
            ESP_LOGW(TAG, "%.*s: 第 %u 块解压异常(%u 字节): %s", (int)asset->name_len,
                     asset->name, (unsigned)index, (unsigned)produced,
                     dracu_inflate_last_error());
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
    if (ok && y != (int)info.height) {
        ESP_LOGW(TAG, "%.*s: 行数不符(%d/%u)", (int)asset->name_len, asset->name, y,
                 (unsigned)info.height);
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

typedef struct {
    dracu_image_t *img;
    uint32_t rect_w;
    uint32_t rect_h;
    uint32_t rect_x;
    uint32_t rect_y;
    const uint8_t *mask;
    uint32_t row_bytes;
    const uint8_t *blocks;
    const uint8_t *block_lens;
    uint32_t block_count;
    uint32_t block_index;
    uint32_t block_left;
    uint8_t *scratch;
    uint32_t scratch_used;
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
        dracu_inflate(state->blocks, length, state->scratch, DRACU_PATCH_BLOCK_PIXELS * 2u);
    state->blocks += length;
    state->block_index++;
    if (produced == 0) {
        return false;
    }
    state->scratch_used = produced;
    state->block_left = produced;
    return true;
}

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

static bool patch_next_position(patch_stream_t *state, uint32_t *out_y, uint32_t *out_x)
{
    while (state->y < state->rect_h) {
        while (state->x < state->rect_w) {
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

static bool apply_patch(dracu_image_t *img, const dracu_asset_t *asset)
{
    if (asset->data == NULL || asset->data_len < DRACU_PATCH_HEADER || asset->dw == 0 ||
        asset->dh == 0) {
        return false;
    }
    const uint8_t *p = asset->data;
    const uint32_t mask_len = rd32(p);
    const uint32_t block_count = rd32(p + 4);
    const uint32_t lens_bytes = block_count * 4u;
    if (block_count == 0 || DRACU_PATCH_HEADER + (uint64_t)lens_bytes + mask_len > asset->data_len) {
        return false;
    }
    const uint8_t *block_lens = p + DRACU_PATCH_HEADER;
    const uint8_t *mask_stream = block_lens + lens_bytes;
    const uint8_t *blocks = mask_stream + mask_len;
    const uint32_t row_bytes = (asset->dw + 7u) / 8u;
    const uint32_t mask_size = row_bytes * asset->dh;
    // 掩码整段解压:它很小(每行 dw/8 字节),借用分块行缓冲的前半段即可
    if (mask_size > sizeof(s_previous_row) * 8u) {
        return false;
    }
    uint8_t *mask = s_previous_row;
    const uint32_t produced = dracu_inflate(mask_stream, mask_len, mask, sizeof(s_previous_row));
    if (produced != mask_size) {
        ESP_LOGW(TAG, "%.*s: CG 补丁掩码解压失败(%u/%u): %s", (int)asset->name_len, asset->name,
                 (unsigned)produced, (unsigned)mask_size, dracu_inflate_last_error());
        return false;
    }
    patch_stream_t state = {
        .img = img,
        .rect_w = asset->dw,
        .rect_h = asset->dh,
        .rect_x = asset->dx,
        .rect_y = asset->dy,
        .mask = mask,
        .row_bytes = row_bytes,
        .blocks = blocks,
        .block_lens = block_lens,
        .block_count = block_count,
        .scratch = s_block_scratch,
        .y = 0,
        .x = 0,
    };
    bool ok = true;
    for (;;) {
        uint32_t y = 0;
        uint32_t x = 0;
        if (!patch_next_position(&state, &y, &x)) {
            break;
        }
        uint8_t pair[2];
        if (!patch_next_pixel_bytes(&state, pair)) {
            ESP_LOGW(TAG, "%.*s: 补丁像素流比掩码短(已写 %u)", (int)asset->name_len, asset->name,
                     (unsigned)state.written);
            ok = false;
            break;
        }
        const int dst_y = (int)asset->dy + (int)y;
        const int dst_x = (int)asset->dx + (int)x;
        if (dst_y >= 0 && dst_y < DRACU_ART_H && dst_x >= 0 && dst_x < DRACU_ART_W) {
            img->pixels[(size_t)dst_y * DRACU_ART_W + (size_t)dst_x] =
                (uint16_t)(pair[0] | ((uint16_t)pair[1] << 8));
            state.written++;
        }
    }
    if (ok) {
        ESP_LOGI(TAG, "CG 补丁 %.*s %ux%u@%u,%u -> %u 像素", (int)asset->name_len, asset->name,
                 (unsigned)asset->dw, (unsigned)asset->dh, (unsigned)asset->dx,
                 (unsigned)asset->dy, (unsigned)state.written);
    }
    return ok;
}

// --------------------------------------------------------------------------
// 图层
// --------------------------------------------------------------------------

static bool draw_background(dracu_image_t *img, uint8_t id)
{
    if (id == DRACU_NONE8) {
        return true;   // 黑屏由 compose 的清屏负责
    }
    dracu_asset_t asset;
    if (!dracu_pack_id(img->pack, DRACU_POOL_BG, id, &asset) || asset.kind != DRACU_KIND_BG) {
        ESP_LOGW(TAG, "找不到背景 %u", (unsigned)id);
        return false;
    }
    return decode_jpeg(img, &asset);
}

static bool draw_cg(dracu_image_t *img, uint16_t id)
{
    dracu_asset_t asset;
    if (!dracu_pack_id(img->pack, DRACU_POOL_CG, id, &asset)) {
        ESP_LOGW(TAG, "找不到事件 CG %u", (unsigned)id);
        return false;
    }
    if (asset.kind == DRACU_KIND_MISSING) {
        return true;   // 被排掉的素材:留黑屏,不报错
    }
    if (asset.kind == DRACU_KIND_CG) {
        return decode_jpeg(img, &asset);
    }
    if (asset.kind == DRACU_KIND_CG_DIFF) {
        dracu_asset_t base;
        if (!dracu_pack_at(img->pack, asset.base, &base) || !decode_jpeg(img, &base)) {
            return false;
        }
        const int64_t start = esp_timer_get_time();
        const bool patched = apply_patch(img, &asset);
        ESP_LOGI(TAG, "CG 补丁 %.*s -> %s, %u ms", (int)asset.name_len, asset.name,
                 patched ? "ok" : "失败", (unsigned)((esp_timer_get_time() - start) / 1000));
        return patched;
    }
    ESP_LOGW(TAG, "事件 CG %u 类型不支持: %u", (unsigned)id, (unsigned)asset.kind);
    return false;
}

static bool draw_sd(dracu_image_t *img, uint16_t id)
{
    dracu_asset_t asset;
    if (!dracu_pack_id(img->pack, DRACU_POOL_SD, id, &asset)) {
        ESP_LOGW(TAG, "找不到 SD %u", (unsigned)id);
        return false;
    }
    if (asset.kind == DRACU_KIND_MISSING) {
        return true;
    }
    const int64_t start = esp_timer_get_time();
    uint32_t written = 0;
    const bool ok = blocked_blit(img, &asset, (int)asset.dx, (int)asset.dy, &written);
    ESP_LOGI(TAG, "SD %.*s %ux%u @(%u,%u) -> %u 像素, %u ms", (int)asset.name_len, asset.name,
             (unsigned)asset.w, (unsigned)asset.h, (unsigned)asset.dx, (unsigned)asset.dy,
             (unsigned)written, (unsigned)((esp_timer_get_time() - start) / 1000));
    return ok;
}

// 立绘 = 身体 + 表情。表情的落点由合成表给出(相对身体左上角)。
static bool draw_sprite(dracu_image_t *img, uint16_t id)
{
    dracu_sprite_t sprite;
    if (!dracu_pack_sprite(img->pack, id, &sprite)) {
        ESP_LOGW(TAG, "立绘 %u 没有合成表条目", (unsigned)id);
        return false;
    }
    if (sprite.body == DRACU_NO_ENTRY) {
        return true;   // 这条描述解析不出身体(源数据里的残缺引用):不画
    }
    dracu_asset_t body;
    if (!dracu_pack_at(img->pack, sprite.body, &body) || body.kind != DRACU_KIND_BODY) {
        ESP_LOGW(TAG, "立绘 %u 的身体条目无效", (unsigned)id);
        return false;
    }
    const int x = (DRACU_ART_W - (int)body.w) / 2;
    const int y = DRACU_SPRITE_HEAD_Y - (int)sprite.facey;
    const int64_t start = esp_timer_get_time();
    uint32_t written = 0;
    bool ok = blocked_blit(img, &body, x, y, &written);
    if (ok && sprite.face != DRACU_NO_ENTRY) {
        dracu_asset_t face;
        if (dracu_pack_at(img->pack, sprite.face, &face) && face.kind == DRACU_KIND_FACE) {
            const int face_x = x + (int)sprite.facex + (int)face.dx;
            const int face_y = y + (int)sprite.facey + (int)face.dy;
            ok = blocked_blit(img, &face, face_x, face_y, &written) && ok;
        }
    }
    img->last_sprite_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "立绘 %u 身体 %ux%u @(%d,%d) -> %u 像素, %u ms, 空闲堆 %u", (unsigned)id,
             (unsigned)body.w, (unsigned)body.h, x, y, (unsigned)written,
             (unsigned)img->last_sprite_ms, (unsigned)esp_get_free_heap_size());
    return ok;
}

bool dracu_image_compose(dracu_image_t *img, const dracu_layers_t *layers)
{
    if (!img || !img->canvas || !img->pack || !layers) {
        return false;
    }
    dracu_layers_t want = *layers;
    const bool changed =
        !img->valid || memcmp(&img->shown, &want, sizeof(want)) != 0;
    bool ok = true;
    if (changed) {
        // 四层里任意一层变了就整屏重画:立绘/SD 叠在背景上,单独擦除需要保留底图,
        // 而本板没有第二张画布的内存。重画的代价是解一次 JPEG(约几十毫秒),
        // 只在换背景/换立绘那几页发生。
        fill_canvas(img, 0);
        if (want.bg != DRACU_NONE8) {
            ok = draw_background(img, (uint8_t)want.bg) && ok;
        }
        if (want.cg != DRACU_NO_ENTRY) {
            ok = draw_cg(img, want.cg) && ok;
        } else if (want.sd != DRACU_NO_ENTRY) {
            ok = draw_sd(img, want.sd) && ok;
        } else if (want.sprite != DRACU_NO_ENTRY) {
            ok = draw_sprite(img, want.sprite) && ok;
        }
        img->shown = want;
        img->valid = true;
    }
    dracu_image_draw_band(img);
    return ok;
}

// --------------------------------------------------------------------------
// 正文带与暗帘
// --------------------------------------------------------------------------

static void draw_gradient(dracu_image_t *img, unsigned red, unsigned green, unsigned blue,
                          unsigned alpha_top, unsigned alpha_bottom)
{
    const unsigned span = (unsigned)(DRACU_ART_H - DRACU_BAND_Y - 1);
    const uint16_t source = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    for (int y = DRACU_BAND_Y; y < DRACU_ART_H; y++) {
        const unsigned t = span ? (unsigned)(y - DRACU_BAND_Y) * 255u / span : 255u;
        const unsigned alpha = alpha_top + (alpha_bottom - alpha_top) * t / 255u;
        uint16_t *row = img->pixels + (size_t)y * DRACU_ART_W;
        for (int x = 0; x < DRACU_ART_W; x++) {
            row[x] = blend565(row[x], source, alpha);
        }
    }
}

void dracu_image_draw_band(dracu_image_t *img)
{
    if (!img || !img->canvas) {
        return;
    }
    draw_gradient(img, DRACU_BAND_R, DRACU_BAND_G, DRACU_BAND_B, DRACU_BAND_ALPHA_TOP,
                  DRACU_BAND_ALPHA_BOTTOM);
    draw_gradient(img, DRACU_SCRIM_R, DRACU_SCRIM_G, DRACU_SCRIM_B, DRACU_SCRIM_ALPHA_TOP,
                  DRACU_SCRIM_ALPHA_BOTTOM);
    canvas_invalidate(img);
}
