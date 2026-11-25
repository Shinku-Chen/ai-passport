// main/gal/maitetsu_pack.c —— 媒体包解析、CRC 校验与条带解码。
//
// 包结构（小端）：
//   0   char magic[8]        "MTMEDIA\0"
//   8   u16 version          1
//   10  u16 header_bytes     48
//   12  u16 image_count
//   14  u16 band_count
//   16  u16 prefix_count
//   18  u16 band_rows        30
//   20  u32 total_bytes
//   24  u32 tables_crc32     覆盖 [0, payload_off)，本字段按 0 计算
//   28  u32 prefix_table_off 48
//   32  u32 image_table_off
//   36  u32 band_table_off
//   40  u32 payload_off
//   44  u16 max_width
//   46  u16 max_height
// 表：
//   prefix: {u32 off, u32 len}                 off 相对 payload_off
//   image:  {u16 w, u16 h, u16 first_band, u8 bands, u8 prefix, u8 pad[4], u32 crc}
//   band:   {u32 off, u32 len, u16 y0, u16 y1, u32 pad}    off 相对 payload_off
//
// 条带 = SOF + SOS + 熵编码数据 + EOI；解码时由共享 prefix（SOI+DQT+DHT）拼出
// 完整 JPEG，不需要把整幅 JPEG 复制到 RAM。
#include "maitetsu_pack.h"

#include "esp_log.h"

// TJpgDec R0.03 keeps a pre-C99 Windows typedef block that conflicts with
// stdint.h; the portable branch is what the device compiles anyway.
#ifdef _WIN32
#undef _WIN32
#include "tjpgd.h"
#define _WIN32 1
#else
#include "tjpgd.h"
#endif

#include <stdio.h>
#include <string.h>

static const char *TAG = "maitetsu_pack";

// 生成文件里的按图描述表（Flash 常量）。它是 maitetsu_pack_image() 的唯一
// 返回对象：指针在整个进程内稳定，调用方可以长期保存，不会互相别名。
// init 会逐项与媒体包表核对，包与生成表不一致时直接拒绝。
extern const maitetsu_image_t maitetsu_pack_images[];
extern const int maitetsu_pack_images_count;

#define PACK_HEADER_BYTES 48u
#define PACK_VERSION 1u
#define IMAGE_ENTRY_BYTES 16u
#define BAND_ENTRY_BYTES 16u
#define PREFIX_ENTRY_BYTES 8u

// 4 位查表 CRC32（多项式 0xEDB88320，与 zlib.crc32 一致）。
static const uint32_t CRC_NIBBLE[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static uint8_t s_band_pixels[MAITETSU_MAX_WIDTH * MAITETSU_BAND_ROWS * 2];
static uint8_t s_jpeg_pool[4096] __attribute__((aligned(8)));

static const uint8_t *s_pack;
static size_t s_size;
static const uint8_t *s_prefixes;
static const uint8_t *s_images;
static const uint8_t *s_bands;
static const uint8_t *s_payload;
static uint16_t s_image_count;
static uint16_t s_band_count;
static uint16_t s_prefix_count;
static uint16_t s_band_rows;
static bool s_ready;
static uint8_t s_validated[(MAITETSU_MAX_IMAGES + 7) / 8];
static uint32_t s_decoded_bands;
static uint32_t s_validated_images;
// 单条带解码缓冲是共享的：记录当前内容，命中时不必重解码。
static uint16_t s_cache_image;
static int16_t s_cache_band;
static uint16_t s_cache_rows;

typedef struct {
    const uint8_t *first;
    size_t first_len;
    size_t first_pos;
    const uint8_t *second;
    size_t second_len;
    size_t second_pos;
    uint16_t *pixels;
    uint16_t width;
    uint16_t strip_h;
    uint16_t rows_written;
} band_ctx_t;

uint32_t maitetsu_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
        crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
    }
    return ~crc;
}

uint32_t maitetsu_crc32(const uint8_t *data, size_t len)
{
    return maitetsu_crc32_update(0, data, len);
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool images_table_matches_pack(uint16_t count);

static void set_error(char *error, size_t error_size, const char *text)
{
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", text);
}

bool maitetsu_pack_init(const uint8_t *data, size_t size, char *error, size_t error_size)
{
    s_pack = NULL;
    s_size = 0;
    s_ready = false;
    memset(s_validated, 0, sizeof(s_validated));
    s_decoded_bands = 0;
    s_validated_images = 0;
    s_cache_image = 0xFFFF;
    s_cache_band = -1;
    s_cache_rows = 0;
    if (error && error_size) error[0] = '\0';

    if (!data || size < PACK_HEADER_BYTES) {
        set_error(error, error_size, "媒体包为空或过短");
        return false;
    }
    if (memcmp(data, "MTMEDIA\0", 8) != 0) {
        set_error(error, error_size, "媒体包标识不匹配");
        return false;
    }
    if (read_u16(data + 8) != PACK_VERSION || read_u16(data + 10) != PACK_HEADER_BYTES) {
        set_error(error, error_size, "媒体包版本不认识");
        return false;
    }
    const uint16_t images = read_u16(data + 12);
    const uint16_t bands = read_u16(data + 14);
    const uint16_t prefixes = read_u16(data + 16);
    const uint16_t band_rows = read_u16(data + 18);
    const uint32_t total = read_u32(data + 20);
    const uint32_t prefix_off = read_u32(data + 28);
    const uint32_t image_off = read_u32(data + 32);
    const uint32_t band_off = read_u32(data + 36);
    const uint32_t payload_off = read_u32(data + 40);
    const uint16_t max_w = read_u16(data + 44);
    const uint16_t max_h = read_u16(data + 46);

    if (total != size) {
        set_error(error, error_size, "媒体包长度与包头不一致");
        return false;
    }
    if (images == 0 || images > MAITETSU_MAX_IMAGES || bands == 0 || prefixes == 0) {
        set_error(error, error_size, "媒体包表数量非法");
        return false;
    }
    if (band_rows == 0 || band_rows > MAITETSU_BAND_ROWS || max_w == 0 ||
        max_w > MAITETSU_MAX_WIDTH || max_h == 0) {
        set_error(error, error_size, "媒体包条带尺寸超出缓冲");
        return false;
    }
    const uint32_t expect_image = PACK_HEADER_BYTES + (uint32_t)prefixes * PREFIX_ENTRY_BYTES;
    const uint32_t expect_band = expect_image + (uint32_t)images * IMAGE_ENTRY_BYTES;
    const uint32_t expect_payload = expect_band + (uint32_t)bands * BAND_ENTRY_BYTES;
    if (prefix_off != PACK_HEADER_BYTES || image_off != expect_image ||
        band_off != expect_band || payload_off != expect_payload || payload_off > total) {
        set_error(error, error_size, "媒体包表偏移不一致");
        return false;
    }

    // 表区 CRC：把包头的 CRC 字段按 0 计算。
    uint8_t header_copy[PACK_HEADER_BYTES];
    memcpy(header_copy, data, sizeof(header_copy));
    const uint32_t stored = read_u32(data + 24);
    memset(header_copy + 24, 0, 4);
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *parts[2] = { header_copy, data + PACK_HEADER_BYTES };
    const size_t lengths[2] = { sizeof(header_copy), payload_off - PACK_HEADER_BYTES };
    for (int part = 0; part < 2; ++part) {
        for (size_t i = 0; i < lengths[part]; ++i) {
            crc ^= parts[part][i];
            crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
            crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
        }
    }
    if ((~crc) != stored) {
        set_error(error, error_size, "媒体包表区 CRC 校验失败");
        return false;
    }

    s_pack = data;
    s_size = size;
    s_prefixes = data + prefix_off;
    s_images = data + image_off;
    s_bands = data + band_off;
    s_payload = data + payload_off;
    s_image_count = images;
    s_band_count = bands;
    s_prefix_count = prefixes;
    s_band_rows = band_rows;
    if (!images_table_matches_pack(images)) {
        set_error(error, error_size, "生成表与媒体包不一致");
        ESP_LOGE(TAG, "生成表与媒体包不一致（需要重新生成资源）");
        s_image_count = 0;
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "媒体包就绪：%u 图像 / %u 条带 / %u KB",
             (unsigned)images, (unsigned)bands, (unsigned)(size / 1024));
    return true;
}

bool maitetsu_pack_ready(void)
{
    return s_ready;
}

uint16_t maitetsu_pack_image_count(void)
{
    return s_ready ? s_image_count : 0;
}

uint32_t maitetsu_pack_decoded_bands(void)
{
    return s_decoded_bands;
}

uint32_t maitetsu_pack_validated_images(void)
{
    return s_validated_images;
}

const maitetsu_image_t *maitetsu_pack_image(uint16_t index)
{
    if (!s_ready || index >= s_image_count) return NULL;
    const maitetsu_image_t *info = &maitetsu_pack_images[index];
    if (info->width == 0 || info->width > MAITETSU_MAX_WIDTH || info->height == 0 ||
        info->bands == 0 || info->prefix >= s_prefix_count ||
        (uint32_t)info->first_band + info->bands > s_band_count) {
        return NULL;
    }
    return info;
}

// 校验生成表的每一项与包内表一致（宽度/高度/条带位置/前缀/CRC）。
static bool images_table_matches_pack(uint16_t count)
{
    if (maitetsu_pack_images_count != (int)count) return false;
    for (uint16_t index = 0; index < count; ++index) {
        const uint8_t *entry = s_images + (size_t)index * IMAGE_ENTRY_BYTES;
        const maitetsu_image_t *generated = &maitetsu_pack_images[index];
        if (generated->width != read_u16(entry) ||
            generated->height != read_u16(entry + 2) ||
            generated->first_band != read_u16(entry + 4) ||
            generated->bands != entry[6] ||
            generated->prefix != entry[7] ||
            generated->crc != read_u32(entry + 12)) {
            return false;
        }
    }
    return true;
}

static bool image_validated(uint16_t index)
{
    return (s_validated[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

static void mark_validated(uint16_t index)
{
    s_validated[index >> 3] |= (uint8_t)(1u << (index & 7));
}

// 校验一张图像的条带表与全部条带负载。失败时不置位，下次仍会重试。
static bool verify_image(uint16_t index)
{
    if (image_validated(index)) return true;
    const maitetsu_image_t *info = maitetsu_pack_image(index);
    if (!info) return false;
    const uint8_t *entries = s_bands + (size_t)info->first_band * BAND_ENTRY_BYTES;
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *chunks[2] = { entries, NULL };
    size_t lengths[2] = { (size_t)info->bands * BAND_ENTRY_BYTES, 0 };
    uint32_t first = read_u32(entries);
    const uint8_t *last_entry = entries + (size_t)(info->bands - 1) * BAND_ENTRY_BYTES;
    uint32_t last_end = read_u32(last_entry) + read_u32(last_entry + 4);
    if (last_end < first || last_end > s_size - (size_t)(s_payload - s_pack)) return false;
    chunks[1] = s_pack + (size_t)(s_payload - s_pack) + first;
    lengths[1] = last_end - first;
    for (int part = 0; part < 2; ++part) {
        for (size_t i = 0; i < lengths[part]; ++i) {
            crc ^= chunks[part][i];
            crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
            crc = (crc >> 4) ^ CRC_NIBBLE[crc & 0x0Fu];
        }
    }
    if ((~crc) != info->crc) {
        ESP_LOGE(TAG, "图像 %u CRC 校验失败", (unsigned)index);
        return false;
    }
    mark_validated(index);
    ++s_validated_images;
    return true;
}

static size_t band_input(JDEC *jd, uint8_t *buffer, size_t requested)
{
    band_ctx_t *ctx = (band_ctx_t *)jd->device;
    size_t written = 0;
    while (written < requested) {
        if (ctx->first_pos < ctx->first_len) {
            size_t take = ctx->first_len - ctx->first_pos;
            if (take > requested - written) take = requested - written;
            if (buffer) memcpy(buffer + written, ctx->first + ctx->first_pos, take);
            ctx->first_pos += take;
            written += take;
            continue;
        }
        if (ctx->second_pos >= ctx->second_len) break;
        size_t take = ctx->second_len - ctx->second_pos;
        if (take > requested - written) take = requested - written;
        if (buffer) memcpy(buffer + written, ctx->second + ctx->second_pos, take);
        ctx->second_pos += take;
        written += take;
    }
    return written;
}

static int band_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    band_ctx_t *ctx = (band_ctx_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    if (!src || !rect) return 0;
    if (rect->left > rect->right || rect->top > rect->bottom) return 1;
    if (rect->right >= ctx->width) return 0;
    const unsigned width = (unsigned)(rect->right - rect->left + 1);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        // 每行都推进源指针：即使该行被裁掉，后续行也必须对上自己的数据。
        if (y < 0 || y >= ctx->strip_h) {
            src += width;
            continue;
        }
        uint16_t *dst = ctx->pixels + (size_t)y * ctx->width + rect->left;
        memcpy(dst, src, width * sizeof(uint16_t));
        src += width;
        if ((uint16_t)(y + 1) > ctx->rows_written) ctx->rows_written = (uint16_t)(y + 1);
    }
    return 1;
}

bool maitetsu_pack_band(uint16_t image_index, uint8_t band, maitetsu_band_t *out)
{
    if (!s_ready || !out) return false;
    const maitetsu_image_t *info = maitetsu_pack_image(image_index);
    if (!info || band >= info->bands) return false;
    if (!verify_image(image_index)) return false;

    const uint8_t *entry = s_bands + (size_t)(info->first_band + band) * BAND_ENTRY_BYTES;
    const uint32_t off = read_u32(entry);
    const uint32_t len = read_u32(entry + 4);
    const uint16_t y0 = read_u16(entry + 8);
    const uint16_t y1 = read_u16(entry + 10);
    const size_t payload_size = s_size - (size_t)(s_payload - s_pack);
    if (len < 35 || off > payload_size || len > payload_size - off) return false;
    if (y0 != (uint16_t)(band * s_band_rows) || y1 <= y0 || y1 > info->height) return false;
    if (y1 - y0 > MAITETSU_BAND_ROWS) return false;

    if (s_cache_image == image_index && s_cache_band == (int16_t)band) {
        out->pixels = (const uint16_t *)s_band_pixels;
        out->width = info->width;
        out->rows = s_cache_rows;
        return true;
    }

    const uint8_t *prefix = s_prefixes + (size_t)info->prefix * PREFIX_ENTRY_BYTES;
    const uint32_t prefix_off = read_u32(prefix);
    const uint32_t prefix_len = read_u32(prefix + 4);
    if (prefix_len < 4 || prefix_off > payload_size || prefix_len > payload_size - prefix_off) {
        return false;
    }

    band_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.first = s_payload + prefix_off;
    ctx.first_len = prefix_len;
    ctx.second = s_payload + off;
    ctx.second_len = len;
    ctx.pixels = (uint16_t *)s_band_pixels;
    ctx.width = info->width;
    ctx.strip_h = (uint16_t)(y1 - y0);

    JDEC jd;
    if (jd_prepare(&jd, band_input, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx) != JDR_OK) {
        ESP_LOGE(TAG, "图像 %u 条带 %u 头部损坏", (unsigned)image_index, (unsigned)band);
        return false;
    }
    if (jd.width != info->width || jd.height != ctx.strip_h) {
        ESP_LOGE(TAG, "图像 %u 条带 %u 尺寸不符", (unsigned)image_index, (unsigned)band);
        return false;
    }
    if (jd_decomp(&jd, band_output, 0) != JDR_OK || ctx.rows_written != ctx.strip_h) {
        ESP_LOGE(TAG, "图像 %u 条带 %u 解码失败", (unsigned)image_index, (unsigned)band);
        return false;
    }
    ++s_decoded_bands;
    s_cache_image = image_index;
    s_cache_band = (int16_t)band;
    s_cache_rows = ctx.strip_h;
    out->pixels = (const uint16_t *)s_band_pixels;
    out->width = info->width;
    out->rows = ctx.strip_h;
    return true;
}
