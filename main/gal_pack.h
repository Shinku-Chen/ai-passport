// main/gal_pack.h —— 《魔法少女的魔女审判》资源包只读解析层。
//
// 包由 tools/manosaba_pack.py 从当前红米手表 5 源快照生成,整包以 EMBED_FILES
// 放进固件,运行时直接读 Flash:不解析 JSON、不解压。本层只做“字节 -> 结构”的
// 纯映射和边界校验,不依赖 ESP-IDF/LVGL,可在宿主机上测试。
//
// 布局(little endian,全部记录 packed):
//   header   magic "MNPK0001", version u32, total u32, section_count u32, reserved
//   table    {type u32, offset u32, count u32, size u32} * section_count
//   sections 见下方 enum 与各结构体。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAL_PACK_MAGIC "MNPK0001"
#define GAL_PACK_VERSION 1u
#define GAL_PACK_SECTIONS 29
#define GAL_NONE 0xFFFFu
#define GAL_MAX_VARS 200

// UI capacity hard limits at pack level. Real source maxima are far below
// these; an oversized pack is rejected at open instead of silently truncated.
#define GAL_PACK_MAX_LINK_ROWS 31u       // last of 32 UI rows is fixed "return"
#define GAL_PACK_MAX_CHOICE_ROWS 32u
#define GAL_PACK_MAX_MAP_ROWS 32u
#define GAL_PACK_MAX_ROUTE_ENTRIES 64u   // current max route is 52 chapters
// 段类型(必须与 tools/manosaba_pack.py 一致)
enum {
    GAL_SEC_META = 0,
    GAL_SEC_STR = 1,
    GAL_SEC_SCENES = 2,
    GAL_SEC_ROUTES = 3,
    GAL_SEC_CHAPTERS = 4,
    GAL_SEC_OPS = 5,
    GAL_SEC_TEXTS = 6,
    GAL_SEC_LINKS = 7,
    GAL_SEC_ITEMS = 8,
    GAL_SEC_POSES = 9,
    GAL_SEC_CHAR_DETAILS = 10,
    GAL_SEC_MOVES = 11,
    GAL_SEC_MOVE_ENTRIES = 12,
    GAL_SEC_VIS = 13,
    GAL_SEC_AREAS = 14,
    GAL_SEC_EVIDENCE_CATS = 15,
    GAL_SEC_MAP_AREAS = 16,
    GAL_SEC_EXPRS = 17,
    GAL_SEC_EXPR_DATA = 18,
    GAL_SEC_IMAGES = 19,
    GAL_SEC_IMGDATA = 20,
    GAL_SEC_BOOK = 21,
    GAL_SEC_GALLERY = 22,
    GAL_SEC_GALLERY_ITEMS = 23,
    GAL_SEC_VARS = 24,
    GAL_SEC_LINK_OPS = 25,
    GAL_SEC_MOVE_OPS = 26,
    GAL_SEC_BOOKKEYS = 27,
    GAL_SEC_STROFF = 28,
};

// 指令(必须与 tools/manosaba_pack.py 一致)
enum {
    GAL_OP_TEXT = 0,
    GAL_OP_CHAR = 1,
    GAL_OP_BG = 2,
    GAL_OP_FOCUS = 3,
    GAL_OP_CALL = 4,
    GAL_OP_JUMP = 5,
    GAL_OP_HIDE_ALL = 6,
    GAL_OP_OFFER = 7,
    GAL_OP_VISIBILITY = 8,
    GAL_OP_RETURN = 9,
    GAL_OP_DAMAGE = 10,
    GAL_OP_CHOICE = 11,
    GAL_OP_CANCEL_CALL = 12,
    GAL_OP_IF = 13,
    GAL_OP_SET = 14,
    GAL_OP_AUTOSAVE = 15,
    GAL_OP_RESET_BOOK = 16,
    GAL_OP_CLUE_HIDE = 17,
    GAL_OP_CLUE_IMAGE = 18,
    GAL_OP_EVIDENCE = 19,
    GAL_OP_FLAG = 20,
    GAL_OP_END = 21,
    GAL_OP_RECORD_CHOICE = 22,
    GAL_OP_MAP = 23,
    GAL_OP_STOP = 24,
    GAL_OP_BOOK_CLEAR = 25,
    GAL_OP_EXECUTION = 26,
    GAL_OP_PAUSE = 27,
    GAL_OP_DYNAMIC_JUMP = 28,
    GAL_OP_BOOK_UPDATE = 29,
};

enum { GAL_SCENE_SYSTEM = 1u, GAL_SCENE_TRIAL = 2u, GAL_SCENE_TRIAL_INIT = 4u };
enum { GAL_IMG_FULL = 0, GAL_IMG_ART = 1, GAL_IMG_PORTRAIT = 2 };
enum { GAL_CAT_CLUE = 0, GAL_CAT_PROFILE = 1, GAL_CAT_RULE = 2, GAL_CAT_NOTE = 3, GAL_CAT_MAP = 4 };
enum { GAL_MOVE_X = 0, GAL_MOVE_Y = 1, GAL_MOVE_WORLD_Y = 2, GAL_MOVE_SCALE = 3 };

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t name;
    uint16_t route_plus1;
    uint16_t flags;
    uint32_t op_start;
    uint32_t op_count;
} gal_scene_t;

typedef struct __attribute__((packed)) {
    uint32_t name;
    uint16_t first;
    uint16_t count;
} gal_route_t;

typedef struct __attribute__((packed)) {
    uint32_t scene;
    uint32_t name;
    uint32_t route_plus1;
    uint16_t flags;
} gal_chapter_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t flags;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint32_t d;
} gal_op_t;

typedef struct __attribute__((packed)) {
    uint16_t scene;
    uint16_t cursor;
    uint32_t text_off;
    uint16_t text_len;
    uint16_t _pad;
} gal_link_t;

typedef struct __attribute__((packed)) {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t to;
    uint16_t end;
    uint16_t when;
    uint16_t scene;
    uint8_t branch;
    uint8_t lock;
} gal_item_t;

typedef struct __attribute__((packed)) {
    uint32_t actor;
    uint32_t appearance;
    uint32_t sub;
    uint32_t _reserved;
    uint16_t image;
    uint16_t default_image;
} gal_pose_t;

typedef struct __attribute__((packed)) {
    uint16_t pose;
    uint16_t scene_x;
    uint16_t scene_y;
    uint16_t world_y;
    uint16_t scale;
    uint8_t visible;
    uint8_t _pad;
} gal_char_t;

typedef struct __attribute__((packed)) {
    uint16_t first;
    uint16_t count;
    uint8_t clear;
    uint8_t _pad;
} gal_move_t;

typedef struct __attribute__((packed)) {
    uint32_t actor;
    uint16_t expr;
    uint8_t kind;
    uint8_t _pad;
} gal_move_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t name;
} gal_area_t;

typedef struct __attribute__((packed)) {
    uint32_t off;
    uint16_t len;
} gal_expr_t;

typedef struct __attribute__((packed)) {
    uint32_t name;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t aux_off;
    uint32_t aux_len;
    uint32_t mask_off;
    uint32_t mask_len;
    uint32_t gallery;        // 该图在鉴赏条目中的下标;GAL_NONE = 非 CG
    uint16_t w;
    uint16_t h;
    uint16_t _pad;
    uint8_t kind;
    uint8_t _pad2;
} gal_imgrec_t;

typedef struct __attribute__((packed)) {
    uint32_t category;
    uint32_t id;
    uint32_t name_off;
    uint16_t name_len;
    uint16_t first;
    uint16_t count;
    uint16_t _pad;
} gal_bookkey_t;

typedef struct __attribute__((packed)) {
    uint32_t category;
    uint32_t id;
    uint32_t name_off;
    uint16_t name_len;
    uint16_t _pad;
    uint32_t text_off;
    uint16_t text_len;
    uint16_t version;
    uint16_t image;
    uint16_t _pad2;
} gal_book_t;

typedef struct __attribute__((packed)) {
    uint32_t label_off;
    uint16_t label_len;
    uint16_t first;
    uint16_t count;
} gal_gallery_group_t;

typedef struct __attribute__((packed)) {
    uint16_t image;
    uint16_t group;
} gal_gallery_item_t;

typedef struct __attribute__((packed)) {
    uint32_t name;
    uint8_t type;
    uint8_t _pad;
    uint16_t _pad2;
    uint32_t default_bits;   // f32
} gal_var_t;

typedef struct __attribute__((packed)) {
    uint32_t op_index;
    uint16_t first;
    uint8_t count;
    uint8_t _pad;
} gal_link_op_t;

typedef struct __attribute__((packed)) {
    uint32_t op_index;
    uint16_t move;
} gal_move_op_t;

typedef struct {
    const uint8_t *base;
    uint32_t size;
    uint32_t count[GAL_PACK_SECTIONS];
    uint32_t len[GAL_PACK_SECTIONS];
    const uint8_t *sec[GAL_PACK_SECTIONS];
    uint32_t meta[26];
} gal_pack_t;

// 打开并校验整包。失败时 err 指向静态可读原因(不接受 NULL)。
bool gal_pack_open(gal_pack_t *pack, const uint8_t *data, uint32_t size, const char **err);

static inline uint32_t gal_pack_meta(const gal_pack_t *p, unsigned i)
{
    return i < 26 ? p->meta[i] : 0;
}
static inline uint32_t gal_pack_count(const gal_pack_t *p, int sec)
{
    return (p && sec >= 0 && sec < GAL_PACK_SECTIONS) ? p->count[sec] : 0;
}
static inline const void *gal_pack_sec(const gal_pack_t *p, int sec)
{
    return (p && sec >= 0 && sec < GAL_PACK_SECTIONS) ? (const void *)p->sec[sec] : NULL;
}

const char *gal_pack_str(const gal_pack_t *p, uint16_t id);
const gal_scene_t *gal_pack_scene(const gal_pack_t *p, uint16_t id);
const gal_op_t *gal_pack_op(const gal_pack_t *p, uint32_t index);
const gal_link_t *gal_pack_link(const gal_pack_t *p, uint16_t id);
const gal_item_t *gal_pack_item(const gal_pack_t *p, uint16_t id);
const gal_pose_t *gal_pack_pose(const gal_pack_t *p, uint16_t id);
const gal_char_t *gal_pack_char(const gal_pack_t *p, uint16_t id);
const gal_move_t *gal_pack_move(const gal_pack_t *p, uint16_t id);
const gal_move_entry_t *gal_pack_move_entry(const gal_pack_t *p, uint16_t id);
const gal_area_t *gal_pack_area(const gal_pack_t *p, uint16_t id);
const gal_imgrec_t *gal_pack_image(const gal_pack_t *p, uint16_t id);
const gal_bookkey_t *gal_pack_bookkey(const gal_pack_t *p, uint16_t id);
const gal_book_t *gal_pack_book(const gal_pack_t *p, uint16_t id);
const gal_gallery_group_t *gal_pack_gallery_group(const gal_pack_t *p, uint16_t id);
const gal_gallery_item_t *gal_pack_gallery_item(const gal_pack_t *p, uint16_t id);
const gal_var_t *gal_pack_var(const gal_pack_t *p, uint16_t id);
const gal_link_op_t *gal_pack_link_op(const gal_pack_t *p, uint16_t id);
const gal_move_op_t *gal_pack_move_op(const gal_pack_t *p, uint16_t id);
const gal_expr_t *gal_pack_expr(const gal_pack_t *p, uint16_t id);
const uint8_t *gal_pack_expr_data(const gal_pack_t *p, uint16_t id, uint16_t *len);

// 把 UTF-16 文本转成 NUL 结尾 UTF-8。返回写入字节数(不含 NUL),失败返回 -1。
int gal_pack_text_utf8(const gal_pack_t *p, uint32_t off, uint16_t len, char *out, size_t cap);

// 书名/书名文本的便捷封装。
int gal_pack_book_name_utf8(const gal_pack_t *p, const gal_book_t *b, char *out, size_t cap);
int gal_pack_book_text_utf8(const gal_pack_t *p, const gal_book_t *b, char *out, size_t cap);

// 按 category/id/version 精确查找书条目(源 bookCatalog[key:version] 语义)。
const gal_book_t *gal_pack_book_find(const gal_pack_t *p, uint8_t category, uint16_t id_str,
                                     uint16_t version);

// 视口内查找与 op 对应的 moves 索引,找不到返回 GAL_NONE。
uint16_t gal_pack_move_for_op(const gal_pack_t *p, uint32_t op_index);
const gal_link_op_t *gal_pack_link_for_op(const gal_pack_t *p, uint32_t op_index);
