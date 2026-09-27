// main/limelight_assets.h —— 素材包(LLMPK001)的只读视图。
//
// 素材包由 tools/limelight_material_pack.py 生成,格式见该工具的文件头注释:
//   背景/CG/标题图 = JPEG;立绘 = JPEG + 1bpp RLE 遮罩;meta = 生成参数 + 名字表。
// 本层只做"按偏移取段 + 按名字查条目",不碰 JPEG 解码,也不碰 ESP-IDF,
// 因此在宿主机上可以直接跑测试(tests/test_limelight_data.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIME_ASSET_MAGIC "LLMPK001"
#define LIME_ASSET_VERSION 1u

// 条目类型(与打包器的 KIND_* 一致)。
#define LIME_ASSET_KIND_IMAGE 0
#define LIME_ASSET_KIND_SPRITE 1
#define LIME_ASSET_KIND_META 2

// 包级标志。
#define LIME_ASSET_FLAG_FILTERED (1u << 0)     // 已按剧本/鉴赏引用裁剪
#define LIME_ASSET_FLAG_BG_CROPPED (1u << 1)   // 背景未存满屏(底部被文本框压住)

typedef struct {
    uint32_t off;        // 相对 blob 起始的绝对偏移
    uint32_t len;        // 条目占用字节(立绘 = JPEG + 遮罩)
    uint16_t w, h;       // 解码后的像素尺寸
    uint8_t kind;        // LIME_ASSET_KIND_*
    uint8_t flags;       // 预留
    uint32_t jpeg_len;   // 仅立绘:JPEG 长度;0 表示非立绘
} lime_asset_t;

typedef struct {
    const uint8_t *blob;
    uint32_t size;
    uint16_t entries;
    uint16_t blob_count;
    uint16_t quality;
    uint16_t screen_w, screen_h, bg_rows;
    uint32_t flags;
    uint32_t index_end;          // 索引 + 对齐后的第一个段偏移
    uint32_t meta_off, meta_len; // meta 条目(生成参数 + 名字表)
    uint32_t names_off, names_end;
} lime_assets_t;

// 打开素材包。失败(魔数/版本/长度/索引越界)返回 false,不改动 assets。
bool lime_assets_open(lime_assets_t *assets, const uint8_t *blob, uint32_t size);

uint16_t lime_assets_count(const lime_assets_t *assets);

// 取第 index 条(0 起)。index 非法返回 false。
bool lime_assets_get(const lime_assets_t *assets, uint16_t index, lime_asset_t *out);

// 名字表第 index 行的家族前缀("bg" / "cg" / "sprite" / "misc"):鉴赏页按它挑条目。
const char *lime_assets_family(const lime_assets_t *assets, uint16_t index, uint16_t *len);

// 名字表第 index 行的基名(不含家族前缀,不带扩展名);返回指针与长度(不含 NUL)。
// 名字表里的一行形如 "bg\t学園_教室a_夏"。
const char *lime_assets_name(const lime_assets_t *assets, uint16_t index, uint16_t *len);

// 按剧本里的值查条目:name 可以带扩展名(会被截断),大小写敏感。
// 找不到返回 -1。meta 条目不参与查找。
int lime_assets_find(const lime_assets_t *assets, const char *name);

// 取 JPEG 段(整张图)与立绘遮罩段(1bpp RLE)。
const uint8_t *lime_assets_jpeg(const lime_assets_t *assets, const lime_asset_t *asset,
                                uint32_t *len);
const uint8_t *lime_assets_mask(const lime_assets_t *assets, const lime_asset_t *asset,
                                uint32_t *len);

// 1bpp RLE 遮罩解码:out 需要 lime_mask_raw_len(w,h) 字节。
// raw 布局 = 逐行 ceil(w/8) 字节,行首在最高位,1 = 不透明,行间不共用字节。
uint32_t lime_mask_raw_len(uint16_t w, uint16_t h);
bool lime_mask_decode(const uint8_t *mask, uint32_t mask_len, uint16_t w, uint16_t h,
                      uint8_t *out, uint32_t out_len);
