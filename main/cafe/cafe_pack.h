// main/cafe/cafe_pack.h —— Cafe Stella 二进制资源包读取层。
//
// 包由 tools/cafe_pack.py 生成，固定源 xuanyide01/cafestella-rw5 @ 90ddb9e0。
// 所有多字节字段为小端；本层只做边界/CRC 校验和按需解压，不解释剧情语义。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAFE_PACK_MAGIC "CSPK1\0\0\0"
#define CAFE_PACK_VERSION 1
#define CAFE_TEXT_CACHE_SIZE 12288

// 包内 section 类型（与 tools/cafe_pack.py 的 SEC_* 一致）。
enum {
    CAFE_SEC_META = 1,
    CAFE_SEC_NODES = 2,
    CAFE_SEC_NEXT = 3,
    CAFE_SEC_WHEN = 4,
    CAFE_SEC_CHOICE = 5,
    CAFE_SEC_COMPLETE = 6,
    CAFE_SEC_RULE = 7,
    CAFE_SEC_FLAGS = 8,
    CAFE_SEC_CHAPTERS = 9,
    CAFE_SEC_AFTER = 10,
    CAFE_SEC_ENDINGS = 11,
    CAFE_SEC_GALLERY = 12,
    CAFE_SEC_GALLERY_VAR = 13,
    CAFE_SEC_ASSETS = 14,
    CAFE_SEC_STRINGS = 15,
    CAFE_SEC_TEXT_BLOCKS = 16,
    CAFE_SEC_TEXT_DATA = 17,
    CAFE_SEC_IMAGES = 18,
    CAFE_SEC_IMAGE_DATA = 19,
    CAFE_SEC_ROUTES = 20,
    CAFE_SEC_PROGRESS = 21,
    CAFE_SEC_UI = 22,
    CAFE_SEC_GALLERY_ALIAS = 23,
    CAFE_SEC_AUDIO = 24,
    CAFE_SEC_AUDIO_DATA = 25,
    CAFE_SEC_OP = 26,
    CAFE_SEC_JPEG_POOL = 27,
    CAFE_SEC_JPEG_DESC = 28,
};

enum {
    CAFE_NODE_ID_NONE = 0xFFFF,
    CAFE_FLAG_NONE = 0xFF,
    CAFE_WHEN_NONE = 0xFFFF,
    CAFE_IMAGE_NONE = 0xFFFF,
};

typedef struct {
    uint8_t id;
    uint8_t kind;       // 0 title, 1 opening
    uint16_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint16_t block_align;
    uint32_t samples;   // exact decoded sample count
    uint32_t data_off;
    uint32_t data_len;
    uint32_t crc;
} cafe_audio_track_t;

typedef struct {
    uint16_t first_image;
    uint16_t frames;
    uint16_t fps;
    uint8_t audio_id;
    uint8_t flags;
    uint32_t duration_ms;
    uint32_t audio_guard_ms;
    uint32_t audio_content_ms;
    uint32_t audio_samples;
} cafe_op_t;

typedef struct {
    uint16_t id;
    uint16_t frames;
    uint16_t block_first;
    uint16_t block_count;
    uint16_t next_first;
    uint16_t choice_first;
    uint16_t complete_first;
    uint8_t next_count;
    uint8_t choice_count;
    uint8_t complete_count;
    uint8_t route;
    uint8_t adult;
    uint8_t after;
    uint32_t title_off;
    uint32_t chapter_off;
    uint32_t source_off;
} cafe_node_t;

typedef struct {
    uint16_t image_id;
    uint16_t patch_image_id;
    uint16_t width;
    uint16_t height;
    uint16_t patch_x, patch_y, patch_w, patch_h;
    uint16_t atlas_x, atlas_y, atlas_w, atlas_h;
    uint8_t kind;      // 0 = 背景, 1 = 立绘
    uint8_t reserved;
} cafe_asset_t;

typedef struct {
    uint16_t id;
    uint8_t kind;       // 0 = 无 mask, 1 = JPEG + zlib 1bpp mask, 2 = 2bpp
    uint8_t flags;
    uint16_t w, h;
    uint32_t off, len;
    uint32_t mask_off, mask_len, mask_raw;
    uint16_t gallery;
    uint32_t crc;
} cafe_image_t;

typedef struct {
    uint16_t node;
    uint16_t first;
    uint16_t count;
    uint16_t raw;
    uint32_t off;
    uint32_t comp;
    uint32_t crc;
} cafe_text_block_t;

typedef struct {
    uint16_t to;       // CAFE_NODE_ID_NONE = @end
    uint16_t when;     // CAFE_WHEN_NONE = 无条件
} cafe_next_t;

typedef struct {
    uint32_t text_off;
    uint16_t to;
    uint8_t flag;
    uint8_t value;
} cafe_choice_t;

typedef struct {
    uint8_t count;
    uint8_t negate;
    uint8_t check_any;
    uint8_t comp[7][4]; // flag, op(0==,1!=), value, pad
} cafe_when_t;

typedef struct {
    uint16_t node;
    uint8_t route;
    uint8_t adult;
    uint32_t title_off;
    uint32_t chapter_off;
} cafe_chapter_t;

typedef struct {
    uint16_t node;
    uint8_t requires;
    uint8_t adult;
    uint32_t title_off;
} cafe_after_t;

typedef struct {
    uint8_t bit;
    uint8_t kind;      // 0 = 主线, 1 = 后日谈
    uint32_t title_off;
    uint32_t body_off;
} cafe_ending_t;

typedef struct {
    uint16_t first_variant;
    uint16_t variant_count;
    uint8_t section;
    uint8_t adult;
    uint32_t label_off;
    uint16_t first_node;
} cafe_gallery_t;

typedef struct {
    uint16_t asset;
    int16_t left;
    int16_t top;
    uint8_t opacity;
    uint8_t reserved;
} cafe_sprite_t;

typedef struct {
    bool valid;
    uint8_t kind;      // 0 = 文本, 1 = OP 标记
    const char *speaker;
    const char *text;
    uint16_t bg_asset;
    uint8_t sprite_count;
    const cafe_sprite_t *sprites;
} cafe_frame_t;

// 目标固件：从链接进 app 的 cafe.pak 初始化。
bool cafe_pack_init(void);
// 宿主机测试：直接给一段已读入内存的包。
bool cafe_pack_set_data(const void *data, size_t size);
void cafe_pack_shutdown(void);
bool cafe_pack_ready(void);

uint32_t cafe_pack_source_sha_prefix(void); // 源码 manifest SHA 前 4 字节
uint32_t cafe_meta_u32(unsigned index);
uint16_t cafe_meta_u16(const uint8_t *meta, unsigned index);

const char *cafe_pack_string(uint32_t off);

bool cafe_node_get(uint16_t id, cafe_node_t *out);
bool cafe_next_get(uint16_t index, cafe_next_t *out);
bool cafe_when_get(uint16_t index, cafe_when_t *out);
bool cafe_choice_get(uint16_t index, cafe_choice_t *out);
bool cafe_complete_get(uint16_t index, uint16_t *bit);
bool cafe_rule_get(uint16_t index, uint8_t *key, uint8_t *cmp_flag,
                   uint8_t *value, uint8_t *add);
bool cafe_chapter_get(uint16_t index, cafe_chapter_t *out);
bool cafe_after_get(uint16_t index, cafe_after_t *out);
bool cafe_ending_get(uint16_t index, cafe_ending_t *out);
bool cafe_gallery_get(uint16_t index, cafe_gallery_t *out);
bool cafe_gallery_variant_get(uint16_t index, uint16_t *asset);
bool cafe_asset_get(uint16_t id, cafe_asset_t *out);
bool cafe_image_get(uint16_t id, cafe_image_t *out);
bool cafe_audio_track_get(uint8_t id, cafe_audio_track_t *out);
const uint8_t *cafe_audio_data(const cafe_audio_track_t *track, uint32_t *len);
bool cafe_op_get(cafe_op_t *out);
uint16_t cafe_op_image_id(uint16_t frame);
bool cafe_ui_image_get(uint16_t index, const char **name, uint16_t *image_id);
bool cafe_route_get(uint16_t index, const char **label, uint8_t *id, uint8_t *adult);
bool cafe_progress_label_get(uint16_t index, const char **label);

// 按帧号取文本。返回的字符串/精灵指针指向内部块缓存，下一次取帧可能失效；
// 调用方必须在显示/存档前复制需要的文本。
bool cafe_frame_get(uint16_t node, uint16_t index, cafe_frame_t *out);
bool cafe_frame_kind(uint16_t node, uint16_t index, uint8_t *kind);
bool cafe_frame_sprite(const cafe_frame_t *frame, unsigned index, cafe_sprite_t *out);
uint16_t cafe_when_count(void);
bool cafe_image_alias_get(uint16_t image_id, uint16_t index, uint16_t *group);

// JPEG stream descriptor.  "pool" is the shared marker-section pool;
// "parts" are u16 values: high bit = literal length, otherwise pool index.
// The literal bytes are in SEC_IMAGE_DATA at img->off/len.  A zero hash or a
// padded C-struct offset is never used here; all offsets are explicit.
typedef struct {
    const uint8_t *literal;
    uint32_t literal_len;
    const uint8_t *parts;
    uint16_t part_count;
    const uint8_t *pool;
    uint32_t pool_size;
    uint16_t part_index;
    uint32_t literal_pos;
} cafe_jpeg_t;

bool cafe_image_jpeg(const cafe_image_t *img, cafe_jpeg_t *out);
bool cafe_jpeg_next(cafe_jpeg_t *stream, const uint8_t **data, uint32_t *len);

// 图片数据指针（在包内，生命周期到 pack 关闭）。Dedup packs return NULL;
// use cafe_image_jpeg() for their JPEG bytes.
const uint8_t *cafe_image_data(const cafe_image_t *img, uint32_t *len);
const uint8_t *cafe_image_mask_data(const cafe_image_t *img, uint32_t *len);
