// main/stella_pack.h —— 《星之终途》资源包的只读解析层。
//
// 包由 tools/stella_pack.py 生成，整包嵌在应用 Flash 里，固件直接按偏移读取：
// 运行时不解析 JSON、不解压图片、不做文件系统访问。本层只做“字节 -> 结构”的
// 纯映射，不依赖 ESP-IDF，因此可在宿主机上跑单元测试。
//
// 字节布局必须与 tools/stella_pack.py 一致（小端）：
//   header  magic "STELPK01", version u32, total_size u32, section_count u32 (20 B)
//   section { type u32, offset u32, count u32, size u32 } (16 B)
//     TEXT    UTF-8 blob，字符串按 (offset, length) 寻址
//     NAME    { off u32, len u16, pad u16 } (8 B)
//     SCENE   { id u16, route u8, flags u8, name u16, next u16,
//               first_op u32, op_count u16, pad u16,
//               data_off u32, data_len u32, raw_len u32 } (28 B)
//     OPS     每个场景一段独立 zlib 流
//     GALLERY groups { label u16, first u16, count u16, pad u16 } (8 B)
//             entries { asset u16, label u16 } (4 B)
//     ASSET   { off u32, len u32, w u16, h u16, mask_off u32, mask_len u32,
//               kind u8, flags u8, ax i16, ay i16, ow u16, oh u16, pad u16 } (32 B)
//     BLOB    JPEG/遮罩数据，4 字节对齐
//     META    key=value 文本
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STELLA_PACK_MAGIC "STELPK01"
#define STELLA_PACK_VERSION 1u
#define STELLA_NONE 0xFFFFu

// scene.route
#define STELLA_ROUTE_MAIN 0u
#define STELLA_ROUTE_AFTER 1u
#define STELLA_ROUTE_LEGACY 2u

// scene.flags
#define STELLA_SCENE_MAIN_CLEAR (1u << 0)   // 正篇通关后才可进入（后日谈）
#define STELLA_SCENE_AFTER_CLEAR (1u << 1)  // 后日谈通关后才可进入（附加故事）

// op.type
#define STELLA_OP_TEXT 0u
#define STELLA_OP_CHOICE 1u
#define STELLA_OP_END 2u

// asset.kind
#define STELLA_ASSET_BG 0u
#define STELLA_ASSET_SPRITE 1u
#define STELLA_ASSET_TITLE 2u

#define STELLA_OP_BYTES 24u
#define STELLA_SCENE_BYTES 28u
#define STELLA_ASSET_BYTES 32u

typedef struct {
    const uint8_t *data;
    uint32_t size;

    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *names;
    uint32_t name_count;
    const uint8_t *scenes;
    uint32_t scene_count;
    const uint8_t *ops;
    uint32_t ops_size;
    const uint8_t *groups;
    uint32_t group_count;
    const uint8_t *gentries;
    uint32_t gentry_count;
    const uint8_t *assets;
    uint32_t asset_count;
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *meta;
    uint32_t meta_size;
} stella_pack_t;

typedef struct {
    uint16_t id;
    uint8_t route;
    uint8_t flags;
    uint16_t name;
    uint16_t next;
    uint32_t first_op;
    uint16_t op_count;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t raw_len;
} stella_scene_t;

typedef struct {
    uint8_t type;
    uint8_t body_count;
    uint16_t bg;
    uint16_t body[3];
    uint16_t speaker;
    uint32_t text_off;
    uint16_t text_len;
    uint16_t arg;      // type == CHOICE: 目标 op 下标
} stella_op_t;

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    const uint8_t *mask;      // 1bpp 行打包；kind != SPRITE 时为 NULL
    uint32_t mask_len;
    uint16_t w;               // 有效（裁剪后）宽高
    uint16_t h;
    int16_t ax;               // 裁剪框在原始立绘中的左上角
    int16_t ay;
    uint16_t ow;              // 原始立绘尺寸（用于保持锚点与缩放）
    uint16_t oh;
    uint8_t kind;
} stella_asset_t;

typedef struct {
    uint16_t label;
    uint16_t first;
    uint16_t count;
} stella_group_t;

typedef struct {
    uint16_t asset;
    uint16_t label;
} stella_gentry_t;

static inline uint16_t stella_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t stella_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool stella_pack_open(stella_pack_t *pack, const uint8_t *data, uint32_t size);

bool stella_pack_scene(const stella_pack_t *pack, uint16_t index, stella_scene_t *out);
bool stella_pack_find_scene(const stella_pack_t *pack, uint16_t id, uint16_t *index_out);

// 直接返回场景的 op 流（Flash 内存映射，24 字节定长）。失败返回 NULL。
const uint8_t *stella_pack_ops(const stella_pack_t *pack, const stella_scene_t *scene,
                               uint16_t *count_out);

bool stella_pack_op(const uint8_t *ops, uint32_t op_count, uint16_t index, stella_op_t *out);

const char *stella_pack_text(const stella_pack_t *pack, uint32_t off, uint16_t len);
const char *stella_pack_name(const stella_pack_t *pack, uint16_t index);
// 把包内不带 NUL 的字符串复制到调用方缓冲，返回是否成功。
bool stella_pack_text_copy(const stella_pack_t *pack, uint32_t off, uint16_t len,
                           char *out, size_t cap);
bool stella_pack_name_copy(const stella_pack_t *pack, uint16_t index,
                           char *out, size_t cap);

// META 里的整数键（initial / after_initial / flow_scene）；缺失或非法返回 fallback。
int stella_pack_meta_int(const stella_pack_t *pack, const char *key, int fallback);

bool stella_pack_asset(const stella_pack_t *pack, uint16_t index, stella_asset_t *out);
bool stella_pack_group(const stella_pack_t *pack, uint16_t index, stella_group_t *out);
bool stella_pack_gentry(const stella_pack_t *pack, uint16_t index, stella_gentry_t *out);
