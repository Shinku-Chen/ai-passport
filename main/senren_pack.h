// main/senren_pack.h —— 《千恋＊万花》图片包的只读解析层。
//
// 包由 tools/senren_pack.py 生成,整块放在 Flash 里由固件直接读:没有解压、没有运行时
// JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,可以在宿主机上跑单测。
//
// 字节序与字段布局必须与 tools/senren_pack.py 一致:
//   SEC_NAME  UTF-8 名字,以 NUL 结尾连排放置(条目按 (pool, name) 升序,可二分查找)
//   SEC_ASSET { name_off u32, name_len u16, kind u8, pool u8, base u16,
//               w u16, h u16, dx u16, dy u16, dw u16, dh u16,
//               data_off u32, data_len u32, flags u32, reserved u16 } = 36 字节
//               data_off 相对 SEC_BLOB
//   SEC_BLOB  载荷区
//   SEC_META  key=value 文本
//
// 载荷都已经是屏幕原生几何,固件不需要缩放:
//   BG / CG / EFFECT  JPEG 240x320(解出来直接就是画布内容)
//   SPRITE           调色板 PNG,宽 x 320(底部对齐,水平居中;实测 93..173 px 宽)
//   SD               调色板 PNG 240x144
//   CG_DIFF          { mask_len u32, pixels_len u32, deflate(mask), deflate(pixels) }
//                    mask 是矩形内 1bpp(行优先,高位在左),pixels 是置位像素的
//                    RGB565 小端序列,按扫描序排列
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENREN_PACK_MAGIC "SENRNPK2"
#define SENREN_PACK_VERSION 2u

// 段类型(与 tools/senren_pack.py 的 SEC_* 一致)
enum {
    SENREN_SEC_NAME = 0,
    SENREN_SEC_ASSET = 1,
    SENREN_SEC_BLOB = 2,
    SENREN_SEC_META = 3,
};

// 条目类型
enum {
    SENREN_KIND_BG = 0,       // 背景 JPEG
    SENREN_KIND_SPRITE = 1,   // 立绘调色板 PNG
    SENREN_KIND_SD = 2,       // SD 装饰调色板 PNG
    SENREN_KIND_CG = 3,       // 事件 CG JPEG
    SENREN_KIND_CG_DIFF = 4,  // 事件 CG 掩码补丁
    SENREN_KIND_MISSING = 5,  // 名字占位(被 --drop-file 排掉),不画
    SENREN_KIND_EFFECT = 6,   // 特效 / 道具 / 画面,调色板 PNG
};

// 名字空间:对应剧本里的 bg / 立绘 / ev 三个引用空间
enum {
    SENREN_POOL_BG = 0,
    SENREN_POOL_CH = 1,
    SENREN_POOL_EV = 2,
};

// 条目 flags
#define SENREN_ASSET_ALIAS (1u << 0)  // 没有自己的载荷,画 base 指向的条目

// SD 装饰在屏幕上的位置(源工程把它放在画布上方偏左,可左右拖动)
#define SENREN_SD_X 0
#define SENREN_SD_Y 24

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *names;       // SEC_NAME 数据区
    uint32_t names_size;
    const uint8_t *entries;     // SEC_ASSET 数据区
    uint32_t entries_size;
    uint32_t entry_count;
    const uint8_t *payload;     // SEC_BLOB 数据区
    uint32_t payload_size;
    const uint8_t *meta;
    uint32_t meta_size;
} senren_pack_t;

typedef struct {
    const char *name;         // 指向包内 UTF-8,不保证 NUL 结尾 -> 用 name_len
    uint16_t name_len;
    uint8_t kind;
    uint8_t pool;
    uint16_t index;           // 条目下标(差分/别名指向的 base 用同一编号)
    uint16_t base;            // 别名或补丁的基准条目下标;无则 0xFFFF
    uint16_t w;
    uint16_t h;
    uint16_t dx;              // 补丁矩形
    uint16_t dy;
    uint16_t dw;
    uint16_t dh;
    const uint8_t *data;
    uint32_t data_len;
    bool alias;               // 画 base 指向的条目
} senren_asset_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool senren_pack_open(senren_pack_t *pack, const uint8_t *data, uint32_t size);

// 按名字空间取条目;名字以 NUL 结尾、按字节比较(表内已按 (pool,name) 升序)。
// 找不到返回 false(固件据此跳过绘制,而不是报错)。
bool senren_pack_find(const senren_pack_t *pack, uint8_t pool, const char *name, senren_asset_t *out);

// 按条目下标取条目(解析 alias / 差分基准用)。越界返回 false。
bool senren_pack_at(const senren_pack_t *pack, uint16_t index, senren_asset_t *out);

// 把名字复制进 out 并以 NUL 结尾;返回写入长度(不含 NUL)。
size_t senren_pack_name(const senren_asset_t *asset, char *out, size_t capacity);

// 补丁载荷的头部长度(掩码段 + 像素段各 4 字节长度)
#define SENREN_PATCH_HEADER 8u

// 拆出掩码段与像素段(仅做长度与边界检查,不解压)。
bool senren_patch_parts(const senren_asset_t *asset, const uint8_t **mask, uint32_t *mask_len,
                        const uint8_t **pixels, uint32_t *pixels_len);
