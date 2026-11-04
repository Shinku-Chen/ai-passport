// main/gal_pack.h —— GalGod 资源包的只读解析层。
//
// 包由 tools/galgod_pack.py 生成,整包放 Flash,固件直接读:没有解压、没有运行时
// JSON 解析。本层只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,可在宿主机上测试。
//
// 布局(little endian):
//   header : magic "GALPK001", version u32, total u32, section_count u32
//   table  : {type u32, offset u32, count u32, size u32} * section_count
// 字符串在 TEXT 里以 NUL 结尾(同一个字符串只存一份),C 侧可直接当 char* 用。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAL_PACK_MAGIC "GALPK001"
#define GAL_PACK_VERSION 1u
#define GAL_NONE 0xFFFFu
#define GAL_MAX_VARS 16

// 段类型(必须与 tools/galgod_pack.py 一致)
enum {
    GAL_SEC_TEXT = 0,
    GAL_SEC_NAME = 1,
    GAL_SEC_EVENT = 2,
    GAL_SEC_LINE = 3,
    GAL_SEC_CHOICE = 4,
    GAL_SEC_OPTION = 5,
    GAL_SEC_ACTION = 6,
    GAL_SEC_COND = 7,
    GAL_SEC_IMAGE = 8,
    GAL_SEC_CHAPTER = 9,
    GAL_SEC_ENDING = 10,
    GAL_SEC_GALLERY = 11,
    GAL_SEC_META = 12,
    GAL_SEC_TITLE = 13,
    GAL_SEC_IMGDATA = 14,
    GAL_SEC_COUNT = 15,
};

// 事件类型
enum {
    GAL_EV_LINE = 0,
    GAL_EV_BG = 1,
    GAL_EV_CG = 2,
    GAL_EV_SPRITE = 3,
    GAL_EV_CHOICE = 4,
    GAL_EV_GOTO = 5,
    GAL_EV_VAR = 6,
    GAL_EV_CHAPTER = 7,
    GAL_EV_ENDING = 8,
    GAL_EV_END = 9,
    GAL_EV_UNLOCK_END = 10,
};

enum { GAL_POS_LEFT = 0, GAL_POS_CENTER = 1, GAL_POS_RIGHT = 2 };
enum { GAL_OP_SET = 0, GAL_OP_ADD = 1, GAL_OP_SUB = 2 };
enum { GAL_CMP_EQ = 0, GAL_CMP_NE, GAL_CMP_GE, GAL_CMP_GT, GAL_CMP_LE, GAL_CMP_LT };
#define GAL_GOTO_CLEAR_SPRITES 1u

typedef struct {
    uint8_t kind;
    uint8_t a;
    uint16_t b;     // line / image / choice / target / chapter / ending id
    uint16_t c;     // cond id (goto) / value (var) / position (sprite)
    uint16_t d;
} gal_event_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t speaker;   // name id 或 GAL_NONE
} gal_line_t;

typedef struct {
    uint16_t first_opt;
    uint8_t opt_count;
    uint8_t _pad;
    uint16_t prompt_line;   // GAL_NONE = 无
} gal_choice_t;

typedef struct {
    uint16_t text;      // name id
    uint16_t target;    // 选中后跳到的事件下标
    uint16_t cond;      // cond id 或 GAL_NONE
    uint16_t act_first;
    uint8_t act_count;
    uint8_t _pad;
} gal_option_t;

typedef struct {
    uint8_t var;
    uint8_t op;
    int16_t value;
} gal_action_t;

typedef struct {
    uint16_t name;
    uint16_t first_event;
    uint16_t event_count;
    uint16_t _pad;
} gal_chapter_t;

typedef struct {
    uint16_t name;
    uint16_t text;      // GAL_NONE = 无正文
    uint16_t unlock_event;
    uint8_t kind;       // 0=终局画面, 1=只解锁
    uint8_t _pad;
} gal_ending_t;

typedef struct {
    uint16_t name;
    uint16_t image;
    uint16_t unlock_event;
    uint16_t _pad;
} gal_gallery_t;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint32_t top_off;   // 相对整包起点
    uint32_t top_len;
    uint32_t bot_off;
    uint32_t bot_len;
    uint32_t mask_top_off;
    uint32_t mask_top_len;
    uint32_t mask_bot_off;
    uint32_t mask_bot_len;
} gal_pack_image_t;

typedef struct {
    const uint8_t *blob;
    uint32_t size;
    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *name_tbl;
    uint32_t name_count;
    const gal_event_t *events;
    uint32_t event_count;
    const gal_line_t *lines;
    uint32_t line_count;
    const gal_choice_t *choices;
    uint32_t choice_count;
    const gal_option_t *options;
    uint32_t option_count;
    const gal_action_t *actions;
    uint32_t action_count;
    const gal_action_t *conds;
    uint32_t cond_count;
    const gal_pack_image_t *images;
    uint32_t image_count;
    const gal_chapter_t *chapters;
    uint32_t chapter_count;
    const gal_ending_t *endings;
    uint32_t ending_count;
    const gal_gallery_t *gallery;
    uint32_t gallery_count;
    uint16_t title_image;
} gal_pack_t;

// 解析并校验分区表/计数。失败返回 false 且 err 写入原因(可为 NULL)。
bool gal_pack_open(gal_pack_t *pack, const uint8_t *blob, uint32_t size, const char **err);

const char *gal_pack_text(const gal_pack_t *pack, uint32_t off);
const char *gal_pack_name(const gal_pack_t *pack, uint16_t name_id);
const char *gal_line_text(const gal_pack_t *pack, uint16_t line_id);
// 图片数据指针与长度(top/bottom/mask),越界返回 false。
bool gal_image_parts(const gal_pack_t *pack, uint16_t image_id,
                     const uint8_t **top, uint32_t *top_len,
                     const uint8_t **bot, uint32_t *bot_len,
                     const uint8_t **mask_top, uint32_t *mask_top_len,
                     const uint8_t **mask_bot, uint32_t *mask_bot_len,
                     uint16_t *w, uint16_t *h);
