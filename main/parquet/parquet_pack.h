// main/parquet/parquet_pack.h —— 《PARQUET》资源包读取层。
//
// 两个包由 tools/parquet_pack.py 生成，运行时按 flash 只读指针直读，不做文件系统：
//   parquet_scn.bin  PQSCN1  场景表 + 16 字节定长 op + 32-op 压缩正文块 + 名称/章节/鉴赏表
//   parquet_img.bin  PQIMG1  JPEG 背景/CG + BLOCK 立绘身体/cut-in + PATCH 表情差分 + 标题图
//
// 设计要点（与源 src/common 一致）：
//   * 立绘 = 共用身体（37 张）+ 完整画布表情层（986 张）的解耦；表情按 alpha 包围盒裁剪成
//     PATCH（RGB565 + 8bit alpha），锚点沿用原作的 faceAnchor，同屏 1/2/3 人不改变比例。
//   * 文本按 32 op 一块独立 zlib 压缩：随机跳场景只需解一块，最大块解压后 < 8 KB。
//   * 资源 id（a0..a1415）→ 图片/立绘/ cut-in 的映射随包保存，运行时不做字符串解析。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PQ_NO_IMAGE 0xFFFFu

// ---------------------------------------------------------------- 图片包
enum {
    PQ_IMG_JPEG = 0,    // 背景 / CG / 标题（JPEG，按 fit 后的像素尺寸存放）
    PQ_IMG_BLOCK = 1,   // 立绘身体 / cut-in / Logo（调色板 + 8bit alpha + 8 行一块 zlib）
    PQ_IMG_PATCH = 2,   // 表情差分（RGB565 + 8bit alpha + 8 行一块 zlib）
};

enum {
    PQ_SPRITE_PORTRAIT = 0,   // asset_map 里的 0x8000 位
};

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint8_t flags;
    uint16_t w;
    uint16_t h;
    uint32_t data_off;    // 绝对偏移（包起点）
    uint32_t data_len;
    uint16_t reserved;
} pq_img_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t body;        // BLOCK 图（身体）
    uint16_t patch;       // PATCH 图（表情层）；PQ_NO_IMAGE = 该 id 与身体表情相同
    int16_t body_dx;      // 身体裁剪框在缩放画布内的偏移
    int16_t body_dy;
    int16_t patch_dx;
    int16_t patch_dy;
    int16_t anchor_x;     // faceAnchor.x 按同一比例缩放后的画布坐标
    uint16_t canvas_w;    // 缩放后的画布宽度（诊断用）
} pq_sprite_t;

typedef struct __attribute__((packed)) {
    uint16_t image;       // BLOCK 图
    int16_t x;            // 画面区坐标（已按 320x150 缩放）
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t reserved;
} pq_cutin_t;

typedef struct __attribute__((packed)) {
    uint16_t image;       // JPEG/BLOCK 图
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} pq_title_t;

// ---------------------------------------------------------------- 剧本包
enum {
    PQ_OP_TEXT = 0,
    PQ_OP_JUMP = 1,
    PQ_OP_CHOICE = 2,
    PQ_OP_END = 3,
};

enum {
    PQ_VIEW_NORMAL = 0,
    PQ_VIEW_ANOTHER = 1,
};

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t route;
    uint8_t clear_var;   // clearFlag 的变量下标；0xFF = 无
    uint16_t first_op;   // 该场景第一条 op 的全局下标
    uint16_t op_count;
    uint16_t tag;        // 章节标签下标
    uint16_t reserved;
} pq_scene_t;

typedef struct __attribute__((packed)) {
    uint8_t kind;        // PQ_OP_*
    uint8_t tag;         // 章节标签下标
    uint8_t view;        // PQ_VIEW_*
    uint8_t flags;
    uint16_t a;          // TEXT: visual；JUMP: 目标场景；CHOICE: 选项表下标
    uint16_t b;          // TEXT: 说话人下标（0xFFFF=旁白）；CHOICE: 选项数
    uint32_t c;          // TEXT: 正文逻辑偏移；JUMP: 目标 op
    uint32_t d;          // TEXT: 正文字节数
} pq_op_t;

typedef struct __attribute__((packed)) {
    uint16_t bg;         // 资源 id（aN）
    uint16_t body[3];    // 资源 id，0xFFFF = 空
    uint16_t cutin;      // 资源 id，0xFFFF = 无
    uint8_t view;        // PQ_VIEW_*
    uint8_t opacity;     // cut-in 不透明度（0..255）
} pq_visual_t;

typedef struct __attribute__((packed)) {
    uint32_t logical_off;   // 本块第一条正文的逻辑偏移
    uint32_t comp_off;      // 相对正文数据段
    uint16_t comp_len;
    uint16_t raw_len;
} pq_text_block_t;

typedef struct __attribute__((packed)) {
    uint32_t label_off;   // 相对字符串段
    uint16_t scene;
    uint8_t route;
    uint8_t reserved;
} pq_chapter_t;

typedef struct __attribute__((packed)) {
    uint32_t name_off;
    uint16_t first_chapter;
    uint16_t chapter_count;
} pq_route_t;

typedef struct __attribute__((packed)) {
    uint16_t first_item;
    uint8_t count;
    uint8_t reserved;
} pq_choice_t;

typedef struct __attribute__((packed)) {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t scene;
    uint32_t to;
    uint8_t var;          // 选中后写入的变量下标
    uint8_t has_effect;
    int32_t value;
} pq_choice_item_t;

typedef struct __attribute__((packed)) {
    uint16_t scene;
    uint16_t name_index;
} pq_ending_t;

typedef struct __attribute__((packed)) {
    uint32_t label_off;
    uint16_t first_member;
    uint16_t count;
} pq_gallery_t;

// ---------------------------------------------------------------- 包句柄
typedef struct {
    const uint8_t *img;
    uint32_t img_size;
    const uint8_t *scn;
    uint32_t scn_size;

    // 图片段
    uint16_t img_entry_count;
    const pq_img_entry_t *img_entries;
    uint16_t sprite_count;
    const pq_sprite_t *sprites;
    uint16_t cutin_count;
    const pq_cutin_t *cutins;
    const uint16_t *asset_map;        // 长度 asset_count；高位置 1 = 立绘下标
    const uint16_t *asset_cutin;      // 长度 asset_count；PQ_NO_IMAGE = 无
    uint16_t asset_count;
    uint8_t title_count;
    const pq_title_t *titles;

    // 剧本段
    uint16_t scene_count;
    uint16_t op_count;
    uint16_t text_block_count;
    uint16_t speaker_count;
    uint16_t chapter_count;
    uint16_t route_count;
    uint16_t tag_count;
    uint16_t var_count;
    uint16_t visual_count;
    uint16_t gallery_group_count;
    uint16_t choice_count;
    uint16_t ending_count;
    uint16_t choice_item_count;
    uint16_t gallery_member_count;
    uint16_t initial;
    uint16_t flow;
    uint16_t side;
    uint16_t after;

    const pq_scene_t *scenes;
    const pq_op_t *ops;
    const pq_visual_t *visuals;
    const pq_text_block_t *text_blocks;
    const uint8_t *text_data;
    uint32_t text_data_size;   // off_tags - off_text_data
    const uint32_t *tag_offs;
    const uint32_t *speaker_offs;
    const pq_chapter_t *chapters;
    const pq_route_t *routes;
    const pq_choice_t *choices;
    const pq_choice_item_t *choice_items;
    const pq_ending_t *endings;
    const pq_gallery_t *galleries;
    const uint16_t *gallery_members;
    const uint32_t *ending_name_offs;
    uint16_t ending_name_count;
    const uint32_t *var_offs;
    const char *strings;
    uint32_t strings_size;

    // 运行期正文块缓存（一块最大 raw < 8 KB）
    uint8_t text_buf[8192];
    uint16_t text_buf_block;   // 已装入的块号；0xFFFF = 空
    uint16_t text_buf_len;
} pq_pack_t;

// 解析并校验两个包。失败返回 false 并写入 err（可为 NULL）。
bool pq_pack_init(pq_pack_t *pack, const uint8_t *img, size_t img_size,
                  const uint8_t *scn, size_t scn_size, char *err, size_t err_cap);

// ---------------------------------------------------------------- 场景 / op
const pq_scene_t *pq_scene_by_id(const pq_pack_t *pack, uint16_t id);
int pq_scene_index(const pq_pack_t *pack, uint16_t id);
const pq_op_t *pq_op(const pq_pack_t *pack, const pq_scene_t *scene, uint32_t index);
const pq_visual_t *pq_visual(const pq_pack_t *pack, const pq_op_t *op);

// 取一条正文（NUL 结尾，UTF-8）。指针在下一次调用前有效；失败返回 NULL。
const char *pq_text_get(pq_pack_t *pack, uint32_t logical_off, uint16_t len);
// 当前块内偏移是否存在（用于校验）
bool pq_text_block_contains(pq_pack_t *pack, uint32_t logical_off);

const char *pq_string(const pq_pack_t *pack, uint32_t off);
const char *pq_speaker(const pq_pack_t *pack, uint16_t index);   // 0xFFFF -> ""
const char *pq_tag_label(const pq_pack_t *pack, uint16_t index);
const char *pq_var_name(const pq_pack_t *pack, uint16_t index);
int pq_var_find(const pq_pack_t *pack, const char *name);   // -1 = 无

// 章节 / 路线 / 结局 / 鉴赏
const char *pq_chapter_label(const pq_pack_t *pack, uint16_t index);
uint16_t pq_chapter_scene(const pq_pack_t *pack, uint16_t index);
uint8_t pq_chapter_route(const pq_pack_t *pack, uint16_t index);
const char *pq_route_name(const pq_pack_t *pack, uint16_t index);
uint16_t pq_route_first_chapter(const pq_pack_t *pack, uint16_t index);
uint16_t pq_route_chapter_count(const pq_pack_t *pack, uint16_t index);
const char *pq_ending_name(const pq_pack_t *pack, uint16_t scene);
int pq_ending_index(const pq_pack_t *pack, uint16_t scene);      // 用于解锁位
const char *pq_gallery_label(const pq_pack_t *pack, uint16_t group);
uint16_t pq_gallery_first(const pq_pack_t *pack, uint16_t group);
uint16_t pq_gallery_count(const pq_pack_t *pack, uint16_t group);
uint16_t pq_gallery_member(const pq_pack_t *pack, uint16_t group, uint16_t i);
// 图片所属 CG 组（按 gallery 顺序）；-1 = 不属于任何组。
int pq_gallery_of_asset(const pq_pack_t *pack, uint16_t asset_id);
// 该 op 的背景是否属于某个 CG 组（鉴赏解锁用）。
int pq_cg_group_of_scene_image(const pq_pack_t *pack, uint16_t asset_id);

// 选项
const pq_choice_t *pq_choice(const pq_pack_t *pack, uint16_t index);
const pq_choice_item_t *pq_choice_item(const pq_pack_t *pack, const pq_choice_t *choice,
                                       uint16_t i);

// 图片
const pq_img_entry_t *pq_img_entry(const pq_pack_t *pack, uint16_t image_index);
const uint8_t *pq_img_data(const pq_pack_t *pack, const pq_img_entry_t *entry);
// 资源 id -> 图片下标（非立绘）；PQ_NO_IMAGE = 无
uint16_t pq_asset_image(const pq_pack_t *pack, uint16_t asset_id);
// 资源 id -> 立绘
const pq_sprite_t *pq_asset_sprite(const pq_pack_t *pack, uint16_t asset_id);
const pq_cutin_t *pq_asset_cutin(const pq_pack_t *pack, uint16_t asset_id);
const pq_title_t *pq_title(const pq_pack_t *pack, uint8_t index);   // 0..2 标题背景，3=Logo
