// main/dracu_pack.h —— 《DRACU-RIOT!》图片包的只读解析层。
//
// 包由 tools/dracu_pack.py 生成,整块放在 Flash 里由固件直接读:没有解压、没有运行时
// JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,可以在宿主机上跑单测
// (tests/test_dracu_pack_layout.py 用真实包核对布局与 id 映射)。
//
// 字节序与字段布局必须与 tools/dracu_pack.py 一致:
//   SEC_NAME   UTF-8 名字连排(NUL 分隔),条目按 (pool, name) 升序,可二分查找
//   SEC_ASSET  { name_off u32, name_len u16, kind u8, pool u8, base u16,
//                w u16, h u16, dx u16, dy u16, dw u16, dh u16,
//                data_off u32, data_len u32, flags u32, reserved u16 } = 36 字节
//                data_off 相对 SEC_BLOB
//   SEC_BLOB   载荷区
//   SEC_SPRITE 立绘合成表:u32 count + count × 8 字节
//                { body u16, face u16, facex i16, facey i16 }
//   SEC_META   key=value 文本
//
// 名字空间(pool)就是剧本里的 id 空间:同一个 pool 内条目按名字升序,下标即 id。
//   POOL_BG   背景:0 号固定是标题图(剧本不引用它),其余是剧本里的背景 id
//   POOL_CG   事件 CG(含差分补丁条目)
//   POOL_SD   SD 小人
//   POOL_BODY 立绘身体
//   POOL_FACE 立绘表情
//
// 载荷:
//   BG / CG          JPEG 240x320(解出来直接就是画布内容)
//   CG_DIFF          { mask_len u32, block_count u32, block_len[block_count] u32,
//                      deflate(mask), 像素块... }:mask 是 dx/dy/dw/dh 矩形内的 1bpp,
//                      像素是置位像素的 RGB565 小端序列,按扫描序切块压缩(每块 ≤1024 像素)
//   SD / BODY / FACE 分块调色板图(不是 PNG):
//                      u16 宽, u16 高, u16 调色板数, u16 每块行数, u16 块数, u16 保留,
//                      u32 每块解压字节(= 每块行数 x (宽 + 1)),
//                      u16 调色板[RGB565 小端], u8 alpha[每色一项],
//                      u32 每块压缩长度[块数], 逐块 zlib 流。
//                      每块解压成若干行:每行 = 1 字节 PNG 滤波类型 + 宽度个索引;
//                      上一行跨块沿用。固件用一块 24 x (240+1) = 5.8 KB 的静态缓冲解块,
//                      不需要 tinfl 的 32 KB 环形字典(本板空闲堆很紧)。
//
// 立绘的画法(与源工程 detail.ux 的合成规则一致):
//   身体画在 ((240 - body.w) / 2, DRACU_SPRITE_HEAD_Y - sprite.facey),裁到屏幕内的行;
//   表情画在身体左上角 + (sprite.facex + face.dx, sprite.facey + face.dy)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRACU_PACK_MAGIC "DRACUPK1"
#define DRACU_PACK_VERSION 1u

// 段类型(与 tools/dracu_pack.py 的 SEC_* 一致)
enum {
    DRACU_SEC_NAME = 0,
    DRACU_SEC_ASSET = 1,
    DRACU_SEC_BLOB = 2,
    DRACU_SEC_SPRITE = 3,
    DRACU_SEC_META = 4,
};

// 条目类型
enum {
    DRACU_KIND_BG = 0,       // 背景 JPEG
    DRACU_KIND_CG = 1,       // 事件 CG JPEG
    DRACU_KIND_CG_DIFF = 2,  // 事件 CG 掩码补丁
    DRACU_KIND_SD = 3,       // SD 小人(分块调色板)
    DRACU_KIND_BODY = 4,     // 立绘身体(分块调色板)
    DRACU_KIND_FACE = 5,     // 立绘表情(分块调色板)
    DRACU_KIND_MISSING = 6,  // 占位(源素材缺失),不画
};

// 名字空间
enum {
    DRACU_POOL_BG = 0,
    DRACU_POOL_CG = 1,
    DRACU_POOL_SD = 2,
    DRACU_POOL_BODY = 3,
    DRACU_POOL_FACE = 4,
    DRACU_POOL_COUNT = 5,
};

// 条目 flags
#define DRACU_ASSET_ALIAS (1u << 0)

// 0xFFFF 在 entry 下标 / CG id / 立绘 id 里表示"没有"
#define DRACU_NO_ENTRY 0xFFFFu
// 页记录里"没有"的两种宽度(见 dracu_scn.h 的记录布局)
#define DRACU_NONE16 0xFFFFu
#define DRACU_NONE8 0xFFu
// 标题画面的背景条目名字。它和普通背景一起按名字排在背景池里(所以 id 不是固定的 0),
// 固件在启动时用 dracu_pack_find 查一次名字,再用 dracu_pack_pool_id 换成池内 id。
#define DRACU_BG_TITLE_NAME "标题画面"

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *names;
    uint32_t names_size;
    const uint8_t *entries;
    uint32_t entries_size;
    uint32_t entry_count;
    const uint8_t *payload;
    uint32_t payload_size;
    const uint8_t *sprites;      // SEC_SPRITE 数据区(u32 count + 每行 8 字节)
    uint32_t sprite_count;
    const uint8_t *meta;
    uint32_t meta_size;
    // 每个 pool 在条目表里的起点与条数(open 时扫一遍算好;脚本里的 id 就是池内下标)
    uint16_t pool_first[DRACU_POOL_COUNT];
    uint16_t pool_count[DRACU_POOL_COUNT];
} dracu_pack_t;

typedef struct {
    const char *name;         // 指向包内 UTF-8,不保证 NUL 结尾 -> 用 name_len
    uint16_t name_len;
    uint8_t kind;
    uint8_t pool;
    uint16_t index;           // 全局条目下标
    uint16_t base;            // 补丁的基准条目下标;无则 0xFFFF
    uint16_t w;
    uint16_t h;
    uint16_t dx;              // CG 补丁矩形 / FACE 包围盒偏移 / SD 放置位置
    uint16_t dy;
    uint16_t dw;
    uint16_t dh;
    const uint8_t *data;
    uint32_t data_len;
    bool alias;
} dracu_asset_t;

typedef struct {
    uint16_t body;   // 身体条目下标;DRACU_NO_ENTRY = 没有
    uint16_t face;
    int16_t facex;   // 表情层相对身体画布左上角的偏移(设备像素)
    int16_t facey;
} dracu_sprite_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool dracu_pack_open(dracu_pack_t *pack, const uint8_t *data, uint32_t size);

// 按 (名字空间, 池内 id) 取条目 —— 剧本里的引用就是这么来的。
bool dracu_pack_id(const dracu_pack_t *pack, uint8_t pool, uint16_t id, dracu_asset_t *out);

// 按条目下标取条目(解析补丁基准用)。越界返回 false。
bool dracu_pack_at(const dracu_pack_t *pack, uint16_t index, dracu_asset_t *out);

// 按 (名字空间, 名字) 取条目。表内按 (pool, name) 升序,二分查找。
bool dracu_pack_find(const dracu_pack_t *pack, uint8_t pool, const char *name, dracu_asset_t *out);

// 把全局条目下标换算成池内 id(池内下标):条目不在该池里返回 false。
bool dracu_pack_pool_id(const dracu_pack_t *pack, uint16_t index, uint8_t pool, uint16_t *id);

// 立绘合成表:越界返回 false。
bool dracu_pack_sprite(const dracu_pack_t *pack, uint16_t id, dracu_sprite_t *out);

// 把名字复制进 out 并以 NUL 结尾;返回写入长度(不含 NUL)。
size_t dracu_pack_name(const dracu_asset_t *asset, char *out, size_t capacity);

// 补丁载荷头部(掩码长度 + 块数 + 块表)至少 8 字节。
#define DRACU_PATCH_HEADER 8u
// 补丁像素块解压缓冲上限(与打包器一致:每块 ≤1024 个 RGB565)
#define DRACU_PATCH_BLOCK_PIXELS 1024u
