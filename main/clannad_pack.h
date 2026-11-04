// main/clannad_pack.h —— 《CLANNAD》资源包只读解析层。
//
// 包由 tools/clannad_pack.py 生成，整包嵌在应用 Flash 里。本层只做“字节 ->
// 结构”的纯映射与逐块解压，不依赖 ESP-IDF / LVGL，宿主机测试直接编译。
//
// 字节布局（小端）必须与 tools/clannad_pack.py 一致：
//   header  magic "CLANPK01", u32 version, u32 total, u16 section_count,
//           u16 flags, u32 reserved (24 B)
//   section { u32 type, u32 offset, u32 count, u32 size } (16 B)
//     TEXT      UTF-8 字符串池（场景名/章节名/路线名/鉴赏标签/资源名）
//     SPEAKER   u16 说话人名字偏移表
//     CATALOG   场景 14 B + 块 16 B + 路线 8 B + 章节 8 B
//     ENTRY     每场景 33 个 u16 入口游标
//     ASSETNAME 每资源一个 u16 源素材名偏移（审计/调试）
//     ASSET     每资源 36 B（JPEG/遮罩偏移、尺寸、原始尺寸、裁边锚点）
//     GALLERY   组 8 B + 条目 2 B
//     BLOB      JPEG 与 1bpp 遮罩，4 字节对齐
//     OPS       每个 512-op 块一条独立 raw-DEFLATE 流
//     META      key=value 文本
//     ALIAS     源素材 id -> 资源下标 (u16,u16)
//
// 块解压后布局：
//   u16 op_count, u16 text_size, u16 extra_size, u16 reserved
//   op_count × 16 B 记录
//   extra_size 字节（表达式/选项条目）
//   text_size 字节（本块 UTF-8 文本池）
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLAN_PACK_MAGIC "CLANPK01"
#define CLAN_PACK_VERSION 1u
#define CLAN_BLOCK_SIZE 512u
#define CLAN_ENTRY_KEEP 33u
#define CLAN_NONE 0xFFFFu
#define CLAN_NO_INDEX 0xFFFFFu

// section types
enum {
    CLAN_SEC_TEXT = 0,
    CLAN_SEC_CATALOG = 1,
    CLAN_SEC_ENTRY = 2,
    CLAN_SEC_ASSETNAME = 3,
    CLAN_SEC_ASSET = 4,
    CLAN_SEC_GALLERY = 5,
    CLAN_SEC_BLOB = 6,
    CLAN_SEC_OPS = 7,
    CLAN_SEC_META = 8,
    CLAN_SEC_ALIAS = 9,
    CLAN_SEC_SPEAKER = 10,
    CLAN_SEC_COUNT = 11,
};

// asset kinds
enum {
    CLAN_ASSET_BG = 0,
    CLAN_ASSET_SPRITE = 1,
    CLAN_ASSET_GALLERY = 2,
    CLAN_ASSET_TITLE = 3,
    CLAN_ASSET_UI = 4,
};

// op codes
enum {
    CLAN_OP_TEXT = 0,
    CLAN_OP_CHOICE = 1,
    CLAN_OP_DYNAMIC_CHOICE = 2,
    CLAN_OP_SET = 3,
    CLAN_OP_NATIVE = 4,
    CLAN_OP_GOTO = 5,
    CLAN_OP_IF_FALSE = 6,
    CLAN_OP_IF_TRUE = 7,
    CLAN_OP_CALL = 8,
    CLAN_OP_JUMP_SCENE = 9,
    CLAN_OP_CALL_SCENE = 10,
    CLAN_OP_RETURN = 11,
    CLAN_OP_END = 12,
    CLAN_OP_BACKGROUND = 13,
    CLAN_OP_BODIES = 14,
    CLAN_OP_BODY_ADD = 15,
};

#define CLAN_OP_BYTES 14u
#define CLAN_SCENE_BYTES 16u
#define CLAN_BLOCK_REC_BYTES 16u
#define CLAN_ASSET_BYTES 36u
#define CLAN_GALLERY_GROUP_BYTES 8u

typedef struct {
    const uint8_t *data;
    uint32_t size;

    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *speaker;
    uint32_t speaker_count;
    const uint8_t *catalog;
    uint32_t catalog_size;
    const uint8_t *entry;
    uint32_t entry_count;
    const uint8_t *assetname;
    uint32_t assetname_count;
    const uint8_t *asset;
    uint32_t asset_count;
    const uint8_t *gallery;
    uint32_t gallery_group_count;
    uint32_t gallery_entry_count;
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *ops;
    uint32_t ops_size;
    const uint8_t *meta;
    uint32_t meta_size;
    const uint8_t *alias;
    uint32_t alias_count;

    uint16_t scene_count;
    uint16_t block_count;
    uint16_t route_count;
    uint16_t chapter_count;
} clannad_pack_t;

typedef struct {
    uint16_t id;
    uint16_t name_off;
    uint16_t source_off;
    uint16_t length;
    uint16_t entries_index;
    uint8_t entry_count;
    uint8_t route;
    uint16_t first_block;
    uint16_t block_count;
} clannad_scene_t;

typedef struct {
    uint32_t off;
    uint32_t comp_size;
    uint32_t raw_size;
    uint16_t op_count;
} clannad_block_rec_t;

typedef struct {
    uint16_t scene_id;
    uint16_t name_off;
    uint8_t route;
    uint8_t flags;
} clannad_chapter_t;

typedef struct {
    uint16_t name_off;
    uint16_t first_chapter;
    uint16_t chapter_count;
} clannad_route_t;

typedef struct {
    uint32_t jpeg_off;
    uint32_t jpeg_len;
    uint16_t w;
    uint16_t h;
    uint32_t mask_off;
    uint32_t mask_len;
    uint16_t ow;
    uint16_t oh;
    int16_t ax;
    int16_t ay;
    uint8_t kind;
} clannad_asset_t;

typedef struct {
    uint16_t label_off;
    uint16_t first_entry;
    uint16_t count;
    uint16_t cover;
} clannad_gallery_group_t;

typedef struct {
    uint16_t asset;
} clannad_gallery_entry_t;

// 包内 op（从 16 B 记录解出）
typedef struct {
    uint8_t type;
    uint8_t n;
    uint16_t x;
    uint16_t y;
    uint16_t z;
    uint16_t text_off;
    uint16_t text_len;
    uint16_t extra;
} clannad_op_t;

static inline uint16_t clannad_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t clannad_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool clannad_pack_open(clannad_pack_t *pack, const uint8_t *data, uint32_t size);

// 包完整性预检：section、表项引用、场景/块/入口/资源范围。
bool clannad_pack_validate(const clannad_pack_t *pack, char *why, size_t cap);

bool clannad_pack_scene(const clannad_pack_t *pack, uint16_t index, clannad_scene_t *out);
bool clannad_pack_find_scene(const clannad_pack_t *pack, uint16_t id, uint16_t *index_out);
bool clannad_pack_block(const clannad_pack_t *pack, uint16_t block_index,
                        clannad_block_rec_t *out);
bool clannad_pack_chapter(const clannad_pack_t *pack, uint16_t index, clannad_chapter_t *out);
bool clannad_pack_route(const clannad_pack_t *pack, uint16_t index, clannad_route_t *out);
bool clannad_pack_asset(const clannad_pack_t *pack, uint16_t index, clannad_asset_t *out);
bool clannad_pack_gallery_group(const clannad_pack_t *pack, uint16_t index,
                                clannad_gallery_group_t *out);
bool clannad_pack_gallery_entry(const clannad_pack_t *pack, uint16_t index,
                                clannad_gallery_entry_t *out);

const char *clannad_pack_text(const clannad_pack_t *pack, uint32_t off);
const char *clannad_pack_speaker(const clannad_pack_t *pack, uint16_t index);
const char *clannad_pack_scene_name(const clannad_pack_t *pack, uint16_t scene_index);
const char *clannad_pack_scene_source(const clannad_pack_t *pack, uint16_t scene_index);
const char *clannad_pack_chapter_name(const clannad_pack_t *pack, uint16_t chapter_index);
const char *clannad_pack_route_name(const clannad_pack_t *pack, uint16_t route_index);
const char *clannad_pack_gallery_label(const clannad_pack_t *pack, uint16_t group);
const char *clannad_pack_asset_name(const clannad_pack_t *pack, uint16_t asset);
const char *clannad_pack_meta(const clannad_pack_t *pack, const char *key);
int clannad_pack_meta_int(const clannad_pack_t *pack, const char *key, int fallback);

// 包内字符串不带 NUL：复制到调用方缓冲（总以 NUL 结尾，超长截断）。
bool clannad_pack_copy_text(const clannad_pack_t *pack, uint32_t off, char *out, size_t cap);

// 解压一个块到 raw（>= raw_size）。op_count/text/extra 从块头回读。
bool clannad_pack_load_block(const clannad_pack_t *pack, uint16_t block_index,
                             uint8_t *raw, uint32_t cap, clannad_block_rec_t *rec_out);
// raw 里的 op 访问（边界由块头校验）。
bool clannad_pack_block_op(const uint8_t *raw, uint32_t raw_size, uint16_t op_index,
                           clannad_op_t *out);
const uint8_t *clannad_pack_block_extra(const uint8_t *raw, uint32_t raw_size,
                                        uint16_t op_count, uint16_t *size_out);
const char *clannad_pack_block_text(const uint8_t *raw, uint32_t raw_size,
                                    uint16_t op_count, uint16_t extra_size,
                                    uint16_t *size_out);

// raw-DEFLATE (RFC 1951)。输出必须是整块、非环绕缓冲；返回是否成功。
bool clannad_inflate_raw(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

// raw-DEFLATE 固定 Huffman 压缩（仅存档；失败时回退 stored）。返回是否成功。
bool clannad_deflate_raw(const uint8_t *src, uint32_t len, uint8_t *dst,
                         uint32_t cap, uint32_t *out_len);
