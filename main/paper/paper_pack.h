// main/paper/paper_pack.h —— 《纸上的魔法使》资源包读取层。
//
// 两个包都由 tools/paper_pack.py 生成，二进制直读，不做 JSON 解析：
//   paper_img.bin  PAPRIMG1  图片（JPEG 场景图 / BLOCK 立绘 / PATCH 表情差分）
//   paper_scn.bin  PAPRSCN1  剧本（场景表 + 8 字节定长 op + 压缩正文块 + 说话人/CG/章节/结局表）
//
// 两个包通过 main/CMakeLists.txt 的 target_add_binary_data 编进固件，运行时是
// flash 直接映射的只读指针，不占 RAM、不需要文件系统。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- 图片包
enum {
    PAPER_IMG_JPEG = 0,    // 场景图（背景 / CG / 标题），按 320x150 内接
    PAPER_IMG_BLOCK = 1,   // 立绘：RGB565 调色板 + 每索引 alpha + 8 行一块 zlib
    PAPER_IMG_PATCH = 2,   // 立绘差分：基准图 + 1bpp 掩码 + RGB565 deflate
};

typedef struct {
    uint32_t name_off;
    uint16_t kind;
    uint16_t w;
    uint16_t h;
    uint16_t extra_len;
    uint32_t data_off;     // 相对包起点
    uint32_t data_len;
    uint32_t reserved;
} paper_img_entry_t;

typedef struct {
    uint16_t base;         // PATCH：基准图下标
    uint16_t x, y, w, h;   // PATCH：变更矩形
} paper_img_patch_t;

// ---------------------------------------------------------------- 剧本包
enum {
    PAPER_OP_TEXT = 0,      // a=speaker, b=len, c=text_off
    PAPER_OP_SPRITE = 1,    // a=slot, b=image(0xFFFF=清空该槽), c=0
    PAPER_OP_BACKGROUND = 2,// a=clearBody, b=image, c=0
    PAPER_OP_SET = 3,       // b=var, c=value
    PAPER_OP_IFFALSE = 4,   // a=cond kind(0 false / 1 tf.scene_mode / 2 sf.pv==1), c=target op
    PAPER_OP_JUMPSCENE = 5, // b=scene, c=target op
    PAPER_OP_CLEARSPRITES = 6,
    PAPER_OP_END = 7,
    PAPER_OP_CHOICE = 8,    // a=item count, b=choice index, c=0
};

typedef struct {
    uint8_t kind;
    uint8_t a;
    uint16_t b;
    uint32_t c;
} paper_op_t;

typedef struct {
    uint16_t id;
    uint16_t route;
    uint32_t first_op;
    uint32_t op_count;
    uint32_t name_off;     // 相对 scene_names 段
    uint32_t reserved;
} paper_scene_t;

typedef struct {
    uint32_t text_off;     // 逻辑正文偏移
    uint16_t len;          // UTF-8 字节数（不含结尾 NUL）
    uint16_t image;        // 选项按钮图（0xFFFF=无）
    uint32_t to;           // 目标 op
} paper_choice_item_t;

typedef struct {
    uint32_t logical_off;
    uint32_t comp_off;     // 相对 textdata 段
    uint16_t comp_len;
    uint16_t raw_len;
    uint32_t reserved;
} paper_text_block_t;

typedef struct {
    const uint8_t *img;            // paper_img.bin
    uint32_t img_size;
    const uint8_t *scn;            // paper_scn.bin
    uint32_t scn_size;

    const paper_img_entry_t *entries;
    uint32_t entry_count;
    const char *img_names;
    const uint16_t *img_group;     // entry_count 项，0xFFFF = 无组

    const paper_scene_t *scenes;
    uint32_t scene_count;
    const char *scene_names;

    const paper_op_t *ops;
    uint32_t op_count;

    const paper_text_block_t *text_blocks;
    uint32_t text_block_count;
    const uint8_t *text_data;
    uint32_t text_data_size;

    const uint32_t *speaker_offs;  // 指向 speaker 段里的偏移表
    const char *speaker_blob;
    uint32_t speaker_count;

    const uint32_t *cg_offs;
    const char *cg_blob;
    const uint32_t *cg_member_offs;
    const uint8_t *cg_members;     // 每项：u16 count + count×u16 image
    uint32_t cg_count;
    uint32_t cg_members_size;   // CG 成员段可用字节（越界检查用）

    const uint32_t *chapter_offs;
    const char *chapter_blob;
    const uint16_t *chapter_meta;  // 每章 4×u16: id, route, id, pad
    uint32_t chapter_count;

    const uint32_t *choice_offs;
    const uint8_t *choice_blob;
    uint32_t choice_count;

    const uint32_t *ending_offs;
    const char *ending_blob;
    const uint16_t *ending_scene;
    uint32_t ending_count;
    uint32_t narrative_size;       // 正文块解压缓冲的实际上限
    uint16_t flow_scene;           // 源 catalog.flowScene：新游戏入口
    uint16_t initial_scene;        // 源 catalog.initial
} paper_pack_t;

// 全局单例：启动时 paper_pack_init(&pack) 一次；失败返回 false 并打印原因。
bool paper_pack_init(paper_pack_t *pack);

// 场景查找（按 domain id）。
const paper_scene_t *paper_scene_find(const paper_pack_t *pack, uint16_t scene_id);
int paper_scene_index(const paper_pack_t *pack, uint16_t scene_id);
const char *paper_scene_name(const paper_pack_t *pack, const paper_scene_t *scene);

// 对白正文：把包含 logical_off 的块解压进内部缓存并返回指向 '\0' 结尾字符串的指针。
// 返回的指针在下一次调用前有效。失败返回 NULL。
const char *paper_text_get(paper_pack_t *pack, uint32_t logical_off, uint16_t len);

const char *paper_speaker_name(const paper_pack_t *pack, int index);

// 图片
const paper_img_entry_t *paper_image(const paper_pack_t *pack, uint16_t index);
const uint8_t *paper_image_data(const paper_pack_t *pack, const paper_img_entry_t *e);
// PATCH 的元数据存在 blob 头部（entry->data_off 起 10 字节），payload 紧随其后。
paper_img_patch_t paper_image_patch(const paper_pack_t *pack, const paper_img_entry_t *e);
const uint8_t *paper_image_payload(const paper_pack_t *pack, const paper_img_entry_t *e);

// 图片所属 CG 组（gallery.txt 顺序）；-1 = 不属于任何 CG 组。
int paper_image_cg_group(const paper_pack_t *pack, uint16_t index);

// CG 鉴赏
const char *paper_cg_name(const paper_pack_t *pack, uint32_t group);
uint16_t paper_cg_first_image(const paper_pack_t *pack, uint32_t group);
int paper_cg_member_count(const paper_pack_t *pack, uint32_t group);
uint16_t paper_cg_member(const paper_pack_t *pack, uint32_t group, int i);

// 章节 / 结局
const char *paper_chapter_name(const paper_pack_t *pack, uint32_t index);
uint16_t paper_chapter_scene(const paper_pack_t *pack, uint32_t index);
uint16_t paper_chapter_route(const paper_pack_t *pack, uint32_t index);
const char *paper_ending_name(const paper_pack_t *pack, uint32_t index);
uint16_t paper_ending_scene(const paper_pack_t *pack, uint32_t index);

// 选项
int paper_choice_count(const paper_pack_t *pack, uint32_t choice_index);
const paper_choice_item_t *paper_choice_item(const paper_pack_t *pack, uint32_t choice_index, int i);
