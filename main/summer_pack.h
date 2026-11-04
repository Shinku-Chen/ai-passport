// main/summer_pack.h —— 《Summer Pockets REFLECTION BLUE》资源包的只读解析层。
//
// 包由 tools/summer_pack.py 生成，整包嵌入 Flash，运行时直接按字节读取：
// 没有 JSON、没有解压、没有整包载入 RAM。本层只做"字节 -> 结构"的纯映射，
// 不依赖 ESP-IDF / LVGL，可在宿主机上测试（tests/test_summer_pack.c）。
//
// 布局（little endian）：
//   header  "SUMMPK01" | version u16 | header_size u16 | section_count u16
//           | flags u16 | total_size u32                         (20 B)
//   table   { type u16, flags u16, offset u32, count u32, size u32 } × N (16 B)
//   payload sections（顺序固定，见 summer_pack.py）
//
// 正文不是 UTF-8 原字节，而是 12 位码位下标（0 = 字符串结束）：
//   TEXT    每 2 个下标压成 3 字节
//   CHARTAB u16[count]：下标 1..N -> Unicode 码位
// 这样 100 万字符的剧本只占约 1.5 MB，且随机访问不需要解压器。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUMMER_PACK_MAGIC "SUMMPK01"
#define SUMMER_PACK_VERSION 1u
#define SUMMER_PACK_SECTION_COUNT 25u
#define SUMMER_PACK_HEADER_SIZE 20u
#define SUMMER_PACK_SECTION_SIZE 16u

// section ids（必须与 tools/summer_pack.py 一致）
enum {
    SUMMER_SEC_TEXT = 0,
    SUMMER_SEC_OPS = 1,
    SUMMER_SEC_OPIDX = 2,
    SUMMER_SEC_SCENE = 3,
    SUMMER_SEC_CHAPTER = 4,
    SUMMER_SEC_ROUTE = 5,
    SUMMER_SEC_GALLERY = 6,
    SUMMER_SEC_GALLERY_ITEM = 7,
    SUMMER_SEC_IMAGE = 8,
    SUMMER_SEC_IMAP = 9,
    SUMMER_SEC_EXPR = 10,
    SUMMER_SEC_EXPRIDX = 11,
    SUMMER_SEC_VARS = 12,
    SUMMER_SEC_CHOICE = 13,
    SUMMER_SEC_CHOICE_ITEM = 14,
    SUMMER_SEC_MAP = 15,
    SUMMER_SEC_MAP_ITEM = 16,
    SUMMER_SEC_MAPLABEL = 17,
    SUMMER_SEC_FLOWLABEL = 18,
    SUMMER_SEC_PROGRESS = 19,
    SUMMER_SEC_META = 20,
    SUMMER_SEC_SCENE_INDEX = 21,
    SUMMER_SEC_SPEAKER = 22,
    SUMMER_SEC_IMGDATA = 23,
    SUMMER_SEC_CHARTAB = 24,
};

// 剧本指令（必须与 tools/summer_pack.py 的 OP_* 一致）
enum {
    SUMMER_OP_TEXT = 0,
    SUMMER_OP_IMAGE = 1,
    SUMMER_OP_SET = 2,
    SUMMER_OP_ADD = 3,
    SUMMER_OP_SUB = 4,
    SUMMER_OP_IF = 5,
    SUMMER_OP_GOTO = 6,
    SUMMER_OP_CALL = 7,
    SUMMER_OP_CALLSCENE = 8,
    SUMMER_OP_JUMPSCENE = 9,
    SUMMER_OP_RETURN = 10,
    SUMMER_OP_END = 11,
    SUMMER_OP_NOP = 12,
    SUMMER_OP_CHOICE = 13,
    SUMMER_OP_MAP = 14,
    SUMMER_OP_CLEAR = 15,
};

// 条件后缀指令（必须与 tools/summer_pack.py 的 EX_* 一致）
enum {
    SUMMER_EX_CONST = 0,
    SUMMER_EX_VAR = 1,
    SUMMER_EX_DATE = 2,
    SUMMER_EX_STRLIT = 3,
    SUMMER_EX_NATIVE = 4,
    SUMMER_EX_UNSUPPORTED = 5,
    SUMMER_EX_NOT = 6,
    SUMMER_EX_NEG = 7,
    SUMMER_EX_POS = 8,
    SUMMER_EX_BNOT = 9,
    SUMMER_EX_ADD = 10,
    SUMMER_EX_SUB = 11,
    SUMMER_EX_MUL = 12,
    SUMMER_EX_DIV = 13,
    SUMMER_EX_MOD = 14,
    SUMMER_EX_AND = 15,
    SUMMER_EX_OR = 16,
    SUMMER_EX_XOR = 17,
    SUMMER_EX_SHL = 18,
    SUMMER_EX_SHR = 19,
    SUMMER_EX_EQ = 20,
    SUMMER_EX_NE = 21,
    SUMMER_EX_LT = 22,
    SUMMER_EX_LE = 23,
    SUMMER_EX_GT = 24,
    SUMMER_EX_GE = 25,
    SUMMER_EX_LAND = 26,
    SUMMER_EX_LOR = 27,
    SUMMER_EX_STREQ = 28,
    SUMMER_EX_STRNE = 29,
    SUMMER_EX_VARKEY = 30,
};

enum {
    SUMMER_IMG_SCENE = 0,
    SUMMER_IMG_PORTRAIT = 1,
    SUMMER_IMG_SOLID = 2,
};

#define SUMMER_IMAGE_NONE 0xFFFFu
#define SUMMER_TEXT_CAP 768u
#define SUMMER_MAX_NAME 64u

typedef struct {
    uint16_t id;
    uint16_t _pad;
    uint32_t first_op;       // 全局 op 下标（审计用）
    uint16_t op_count;
    uint16_t next;
    int8_t route;
    uint8_t phase;           // 0 普通 / 1 personal / 2 final
    uint8_t flags;           // bit0: 源场景名以 '_' 开头（callScene 时跳过）
    uint8_t _pad2;
    uint16_t first_block;    // OPIDX 段里的块下标
    uint16_t block_count;
    uint32_t name;           // TEXT 下标（fullName 标签）
    uint32_t map_name;       // TEXT 下标（地图行短名，fullName=false）
    uint32_t date;           // strings[106] 原始值（全角数字）；空 = 该场景不设置
    uint32_t date_label;     // catalog 的 date 回退（ASCII），用于 sceneLabel
    uint32_t route_name;
} summer_scene_t;

typedef struct {
    uint16_t scene;
    uint8_t route;
    uint8_t _pad;
    uint32_t name;
} summer_chapter_t;

typedef struct {
    uint32_t name;
    uint16_t scene_count;
    uint16_t chapter_count;
} summer_route_t;

typedef struct {
    uint32_t label;
    uint16_t first;
    uint8_t count;
    uint8_t _pad;
} summer_gallery_t;

typedef struct {
    uint32_t off;            // 相对 IMGDATA 段
    uint32_t len;
    uint32_t mask_off;       // 相对 IMGDATA 段；0 = 无遮罩
    uint32_t mask_len;
    uint16_t width;
    uint16_t height;
    uint8_t kind;
    uint8_t flags;
    uint16_t _pad;
} summer_image_t;

typedef struct {
    uint16_t first;
    uint8_t count;
    uint8_t _pad;
    uint16_t var;
    uint16_t _pad2;
} summer_choice_group_t;

typedef struct {
    uint32_t text;
    int32_t value;
    uint32_t cond;           // 表达式 id；0 = 无条件
} summer_choice_t;

typedef struct {
    uint16_t first;
    uint8_t count;
    uint8_t _pad;
    uint16_t var;
    uint16_t _pad2;
} summer_map_group_t;

typedef struct {
    int32_t value;
} summer_map_item_t;

typedef struct {
    uint32_t key;            // (scene << 16) | cursor
    uint32_t text;
} summer_maplabel_t;

typedef struct {
    uint32_t key;            // flow 场景内 cursor
    uint8_t kind;            // 0 label / 1 date
    uint8_t _pad;
    uint16_t _pad2;
    uint32_t text;
} summer_flowlabel_t;

typedef struct {
    uint32_t special_var;    // 24006
    uint32_t var_count;
    uint32_t image_count;
    uint32_t gallery_count;
    uint16_t flow_scene;
    uint16_t initial_scene;
    uint16_t map_scene;
    uint16_t scene_count;
    uint16_t chapter_count;
    uint16_t route_count;
    uint32_t choice_count;
    uint32_t choice_item_count;
    uint32_t map_count;
    uint32_t map_item_count;
    uint32_t block_count;
    uint32_t op_bytes;
    uint32_t title;
    uint32_t subtitle;
    uint32_t version;
    uint32_t tagline;
    uint16_t logo_image;
    uint16_t title_bg_image;
    uint16_t _pad3;
    uint16_t _pad4;
} summer_meta_t;

typedef struct {
    uint16_t max_ending;
    uint8_t heroine_count;
    uint8_t flow_ending_count;
    uint8_t globals_count;
    uint8_t _pad;
    uint32_t clear_var[15];       // 下标 1..14；0 = 无
    uint8_t heroine[15];
    uint16_t flow_cursor[15];
    uint8_t flow_ending[15];
    uint32_t globals[6];
    uint32_t restart_var;
    uint32_t special_var;
    uint32_t clear_count_var;
} summer_progress_meta_t;

typedef struct {
    const uint8_t *blob;
    uint32_t size;

    const uint8_t *text;          // 12 位码位流
    uint32_t text_codes;
    const uint8_t *chartab;
    uint32_t chartab_count;
    const uint8_t *ops;
    uint32_t ops_size;
    const uint32_t *opidx;
    uint32_t opidx_count;
    const summer_scene_t *scenes;
    uint32_t scene_count;
    const summer_chapter_t *chapters;
    uint32_t chapter_count;
    const summer_route_t *routes;
    uint32_t route_count;
    const summer_gallery_t *gallery;
    uint32_t gallery_count;
    const uint16_t *gallery_items;
    uint32_t gallery_item_count;
    const summer_image_t *images;
    uint32_t image_count;
    const uint16_t *imap;         // 源图 id -> image 下标（0 = 黑屏）
    uint32_t imap_count;
    const uint8_t *expr;
    uint32_t expr_size;
    const uint32_t *expridx;
    uint32_t expr_count;
    const uint32_t *vars;
    uint32_t var_count;
    const summer_choice_group_t *choices;
    uint32_t choice_count;
    const summer_choice_t *choice_items;
    uint32_t choice_item_count;
    const summer_map_group_t *maps;
    uint32_t map_count;
    const summer_map_item_t *map_items;
    uint32_t map_item_count;
    const summer_maplabel_t *map_labels;
    uint32_t map_label_count;
    const summer_flowlabel_t *flow_labels;
    uint32_t flow_label_count;
    summer_progress_meta_t progress;
    summer_meta_t meta;
    const uint16_t *scene_index;
    uint32_t scene_index_count;
    const uint32_t *speakers;
    uint32_t speaker_count;
    const uint8_t *imgdata;
    uint32_t imgdata_size;

    // 正文解码缓冲：summer_pack_text() 返回指向这里的 NUL 结尾 UTF-8。
    char text_buf[SUMMER_TEXT_CAP];
} summer_pack_t;

// 解码后的一条指令。字段按 kind 解释：
//   TEXT      a=speaker 下标, b=正文 TEXT 下标
//   IMAGE     a=源图 id, b=layer
//   SET/ADD/SUB a=变量 id, b=表达式 id（tag=1）或 value（tag=0）
//   IF        a=表达式 id, b=target, value=truth
//   GOTO/CALL a=target
//   *SCENE    a=场景 id, b=target
//   CHOICE/MAP a=组下标
//   CLEAR     a=layer
typedef struct {
    uint8_t kind;
    uint8_t tag;
    uint8_t truth;
    uint8_t _pad;
    uint32_t a;
    uint32_t b;
    int32_t value;
} summer_op_t;

// 解析并校验分区表/计数。失败返回 false 并把原因写入 err（可为 NULL）。
bool summer_pack_open(summer_pack_t *pack, const uint8_t *blob, uint32_t size,
                      const char **err);

const summer_scene_t *summer_scene_by_id(const summer_pack_t *pack, uint16_t id);
const summer_scene_t *summer_scene_at(const summer_pack_t *pack, uint16_t index);
const summer_image_t *summer_image(const summer_pack_t *pack, uint16_t source_id);
const summer_image_t *summer_image_at(const summer_pack_t *pack, uint32_t index);

// 正文 TEXT 下标 -> UTF-8。返回 pack->text_buf；失败返回 ""。
const char *summer_pack_text(summer_pack_t *pack, uint32_t off);
// 正文 TEXT 下标 -> 调用方缓冲（便于同时保留两个字符串，如选项列表）。
bool summer_pack_text_into(summer_pack_t *pack, uint32_t off, char *dst, size_t cap);
// 说话人下标 -> TEXT 下标；越界返回 0。
uint32_t summer_speaker_text(const summer_pack_t *pack, uint32_t speaker);

// 场景内 op 下标 -> OPS 段字节偏移。index >= op_count 时返回场景末尾。
bool summer_op_seek(const summer_pack_t *pack, const summer_scene_t *scene,
                    uint32_t index, uint32_t *offset);
// 解码 offset 处的一条指令；返回下一条的偏移，0 = 数据错误。
uint32_t summer_op_decode(const summer_pack_t *pack, uint32_t offset, summer_op_t *op);
// 审计/测试用：单条指令的字节长度；0 = 错误。
uint32_t summer_op_size(const summer_pack_t *pack, uint32_t offset);

// 选项/地图组访问。
const summer_choice_group_t *summer_choice_group(const summer_pack_t *pack, uint32_t group);
const summer_choice_t *summer_choice_item(const summer_pack_t *pack, uint32_t group, int index);
const summer_map_group_t *summer_map_group(const summer_pack_t *pack, uint32_t group);
// map 标签：destination 场景 + cursor；找不到返回 NULL。
const char *summer_map_label(const summer_pack_t *pack, uint16_t scene, uint32_t cursor);
// flow 标签/日期：flow 场景内 cursor。kind 0=label 1=date。
const char *summer_flow_text(const summer_pack_t *pack, uint32_t cursor, int kind);
// 漫画收藏组。
const summer_gallery_t *summer_gallery_group(const summer_pack_t *pack, uint32_t group);
uint16_t summer_gallery_image(const summer_pack_t *pack, uint32_t group, int variant);
int summer_gallery_group_for_image(const summer_pack_t *pack, uint16_t source_id);

// 进度元数据辅助。
int summer_ending_for_cursor(const summer_pack_t *pack, uint16_t scene, uint32_t cursor);
uint32_t summer_clear_var(const summer_pack_t *pack, int ending);
bool summer_ending_is_heroine(const summer_pack_t *pack, int ending);
