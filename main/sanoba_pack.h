// main/sanoba_pack.h —— 《千恋＊万花》图片包的只读解析层。
//
// 包由 tools/sanoba_pack.py 生成,整块放在 Flash 里由固件直接读:没有解压、没有运行时
// JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,可以在宿主机上跑单测。
//
// 字节序与字段布局必须与 tools/sanoba_pack.py 一致:
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
//   SPRITE           分块调色板图,宽 x 320(底部对齐,水平居中)
//   SD               分块调色板图 240x144
//   CG_DIFF          { mask_len u32, block_count u32, block_len[block_count] u32,
//                       deflate(mask), 像素块... }
//                    mask 是矩形内 1bpp(行优先,高位在左);像素是置位像素的
//                    RGB565 小端序列,按扫描序切块压缩(每块 <= 1024 像素)
//
// 分块调色板图(不是 PNG):
//   u16 宽, u16 高, u16 调色板数, u16 每块行数, u16 块数, u16 保留,
//   u32 每块解压字节(= 每块行数 x (宽 + 1)),
//   u16 调色板[每项 RGB565 小端], u8 alpha[每项], u32 每块压缩长度[块数], 逐块 zlib 流。
//   每块解压出来是若干行,每行 = 1 字节 PNG 滤波类型 + 宽度个索引,上一行跨块沿用。
//   这样每块只要一个 2 KB 级行缓冲,不需要 tinfl 的 32 KB 环形字典(本板空闲堆很紧)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SANOBA_PACK_MAGIC "SANOBPK1"
#define SANOBA_PACK_VERSION 1u

// 段类型(与 tools/sanoba_pack.py 的 SEC_* 一致)
enum {
    SANOBA_SEC_NAME = 0,
    SANOBA_SEC_ASSET = 1,
    SANOBA_SEC_BLOB = 2,
    SANOBA_SEC_META = 3,
};

// 条目类型
enum {
    SANOBA_KIND_BG = 0,       // 背景 JPEG
    SANOBA_KIND_SPRITE = 1,   // 立绘调色板 PNG
    SANOBA_KIND_SD = 2,       // SD 装饰(本项目是 JPEG 横条,见 tools/sanoba_pack.py)
    SANOBA_KIND_CG = 3,       // 事件 CG JPEG
    SANOBA_KIND_CG_DIFF = 4,  // 事件 CG 掩码补丁
    SANOBA_KIND_MISSING = 5,  // 名字占位(被 --drop-file 排掉),不画
    SANOBA_KIND_EFFECT = 6,   // 特效 / 道具 / 画面,调色板 PNG
};

// 名字空间:对应剧本里的 bg / 立绘 / ev 三个引用空间
enum {
    SANOBA_POOL_BG = 0,
    SANOBA_POOL_CH = 1,
    SANOBA_POOL_EV = 2,
};

// 条目 flags
#define SANOBA_ASSET_ALIAS (1u << 0)  // 没有自己的载荷,画 base 指向的条目

// 标题画面的背景条目名:由 tools/sanoba_pack.py 从 --title-art 打进背景名字空间。
// 剧本不会引用这个名字,界面在标题页按下标查它。
#define SANOBA_TITLE_ART_NAME "标题画面"

// SD 装饰在屏幕上的位置(源工程把它放在画布上方偏左,可左右拖动)
#define SANOBA_SD_X 0
#define SANOBA_SD_Y 24

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
} sanoba_pack_t;

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
} sanoba_asset_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool sanoba_pack_open(sanoba_pack_t *pack, const uint8_t *data, uint32_t size);

// 按名字空间取条目;名字以 NUL 结尾、按字节比较(表内已按 (pool,name) 升序)。
// 找不到返回 false(固件据此跳过绘制,而不是报错)。
bool sanoba_pack_find(const sanoba_pack_t *pack, uint8_t pool, const char *name, sanoba_asset_t *out);

// 按条目下标取条目(解析 alias / 差分基准用)。越界返回 false。
bool sanoba_pack_at(const sanoba_pack_t *pack, uint16_t index, sanoba_asset_t *out);

// 把名字复制进 out 并以 NUL 结尾;返回写入长度(不含 NUL)。
size_t sanoba_pack_name(const sanoba_asset_t *asset, char *out, size_t capacity);

// 补丁载荷的头部长度(掩码段 + 像素段各 4 字节长度)
#define SANOBA_PATCH_HEADER 8u

// 拆出掩码段与像素段(仅做长度与边界检查,不解压)。
bool sanoba_patch_parts(const sanoba_asset_t *asset, const uint8_t **mask, uint32_t *mask_len,
                        const uint8_t **pixels, uint32_t *pixels_len);
