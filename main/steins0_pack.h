// main/steins0_pack.h —— SG0PK01 资源包的只读解析层(不依赖 ESP-IDF)。
//
// 包由 tools/steins0_pack.py 生成,整包嵌入应用 Flash,固件按偏移直接读取:
// 运行时不解析 JSON、不访问文件系统。本层只做“字节 -> 结构”的映射与按块
// zlib 解压(每块正文独立流,解压缓冲很小),因此可在宿主机上直接回归。
//
// 字节布局见 tools/steins0_pack.py 顶部注释;两边改动必须同步,并由
// tools/steins0_audit.py 用独立解析器逐 op 对照。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SG0_PACK_MAGIC "SG0PK001"
#define SG0_PACK_VERSION 1u
#define SG0_NONE 0xFFFFu

// 段落类型(与打包器一致)。
enum {
    SG0_SEC_TEXT = 0,
    SG0_SEC_STRT,
    SG0_SEC_NODE,
    SG0_SEC_CHUNK,
    SG0_SEC_FRAME,      // 保留(帧在 chunk 流内)
    SG0_SEC_VIEW,
    SG0_SEC_LAYER,
    SG0_SEC_CHOICE,
    SG0_SEC_COND,
    SG0_SEC_ENDING,
    SG0_SEC_ENDLIST,
    SG0_SEC_BREAK,
    SG0_SEC_CHGRP,
    SG0_SEC_EVENT,
    SG0_SEC_START,
    SG0_SEC_ASSETID,
    SG0_SEC_PART,
    SG0_SEC_BLOB,
    SG0_SEC_GG,
    SG0_SEC_GE,
    SG0_SEC_META,
    SG0_SEC_SPK,
    SG0_SEC_DATE,
    SG0_SEC_LAYERREF,
    SG0_SEC_TITLE,
    SG0_SEC_RMSG,
    SG0_SEC_RREPLY,
    SG0_SEC_RCHOICE,
    SG0_SEC_RSEQ,
    SG0_SEC_RCONTACT,
    SG0_SEC_RBASE,
    SG0_SEC_CARD,
    SG0_SEC_COUNT
};

#define SG0_KIND_PHOTO 0u
#define SG0_KIND_SPRITE 1u

#define SG0_EVENT_CLOSE_CHOICE 0u
#define SG0_EVENT_MESSAGE 1u
#define SG0_EVENT_RINE_RESET 2u
#define SG0_EVENT_RINE_REPLY 3u
#define SG0_EVENT_EXPIRE_RINE 4u

// expireRine.flags: source `readMessages` exists (even when the array is empty).
// Missing `readMessages` must stay distinguishable from an empty array because
// the source treats a missing property as falsy and an empty array as truthy.
#define SG0_EVENT_FLAG_READ_MESSAGES_PRESENT 2u

#define SG0_FLAG_PR_COMPLETE 0u   // 唯一的源条件:结局 0 通关(盟誓的文艺复兴链)

typedef struct {
    uint32_t offset;
    uint32_t count;
    uint32_t size;
} sg0_section_t;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    sg0_section_t sec[SG0_SEC_COUNT];
    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *meta;
    uint32_t meta_size;
} sg0_pack_t;

typedef struct {
    uint16_t chunk_first;
    uint16_t chunk_count;
    uint16_t choice;
    uint16_t condition;
    int8_t ending;
    uint8_t flags;
    uint16_t break_first;
    uint8_t break_count;
    uint16_t chapter_first;
    uint8_t chapter_count;
    uint16_t event_first;
    uint16_t event_count;
    uint16_t start_first;
    uint8_t start_count;
} sg0_node_t;

typedef struct {
    uint32_t zoff;
    uint32_t zlen;
    uint16_t raw_len;
    uint8_t frames;
} sg0_chunk_t;

typedef struct {
    uint16_t text_off;
    uint16_t text_len;
    uint16_t view;
    uint8_t speaker;
    uint8_t group;
    uint16_t date;
    uint8_t flags;
} sg0_frame_t;

typedef struct {
    uint16_t layer_first;
    uint8_t layer_count;
} sg0_view_t;

typedef struct {
    uint16_t asset;
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t opacity;   // 0..1000
} sg0_layer_t;

typedef struct {
    uint8_t kind;        // 0 call, 1 power
    uint8_t critical;
    uint16_t heading;
    uint16_t category;
    uint8_t count;
    uint8_t to[2];
    uint8_t action[2];   // 0 accept, 1 defer
    uint16_t text[2];
} sg0_choice_t;

typedef struct {
    uint8_t flag;
    uint8_t yes;
    uint8_t no;
} sg0_cond_t;

typedef struct {
    uint16_t name;
    uint8_t ending_id;
} sg0_ending_t;

typedef struct {
    uint16_t at;
    uint8_t type;
    uint8_t flags;
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint16_t d;
    uint16_t data;   // message catalog / reply / fallback index
} sg0_event_t;

typedef struct {
    uint16_t id;
    uint16_t base;
    uint16_t date;      // DATE 表下标
    uint8_t contact;
    uint8_t sender;
    uint8_t flags;      // bit0 outgoing, bit1 stamp
    uint8_t kind;
    uint16_t text;      // 字符串下标;0xFFFF 无正文
    uint16_t stamp;     // 贴纸资源下标;0xFFFF 非贴纸
} sg0_rine_msg_t;

typedef struct {
    uint8_t node;
    uint8_t group;
    uint16_t script;
    uint16_t index;
    uint16_t date;      // DATE 表下标
    uint16_t name;      // 字符串下标
} sg0_start_t;

typedef struct {
    uint16_t part_first;
    uint8_t part_count;
    uint8_t kind;
    uint16_t name;      // 字符串下标
} sg0_assetid_t;

typedef struct {
    uint32_t off;
    uint32_t len;
    uint32_t mask_off;
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
    int16_t ax;
    int16_t ay;
    uint16_t ow;
    uint16_t oh;
    uint8_t flags;      // bit0: 有 1bpp 遮罩
} sg0_part_t;

typedef struct {
    uint16_t label;
    uint16_t first;
    uint8_t count;
} sg0_gallery_group_t;

typedef struct {
    uint16_t cursor;
    uint8_t group;
} sg0_chgrp_t;

typedef struct {
    uint16_t first;
    uint8_t count;
} sg0_start_range_t;

bool sg0_pack_open(sg0_pack_t *pack, const uint8_t *data, uint32_t size);
bool sg0_pack_string(const sg0_pack_t *pack, uint16_t index, const char **out, uint16_t *len);
bool sg0_pack_speaker(const sg0_pack_t *pack, uint8_t index, const char **out, uint16_t *len);
uint32_t sg0_pack_date(const sg0_pack_t *pack, uint16_t index);
bool sg0_pack_node(const sg0_pack_t *pack, uint8_t index, sg0_node_t *out);
bool sg0_pack_chunk_at(const sg0_pack_t *pack, uint16_t chunk_index, sg0_chunk_t *out);
bool sg0_pack_chunk_load(const sg0_pack_t *pack, const sg0_chunk_t *chunk, uint8_t *out,
                         uint32_t cap, uint32_t *raw_len);
// 通用 zlib 解压(遮罩/正文共用同一实现,调用方提供输出缓冲)。
bool sg0_pack_inflate(const uint8_t *src, uint32_t len, uint8_t *out, uint32_t cap,
                      uint32_t *out_len);
bool sg0_pack_frame(const uint8_t *chunk_raw, uint32_t raw_len, uint8_t index, sg0_frame_t *out);
const char *sg0_pack_frame_text(const uint8_t *chunk_raw, uint32_t raw_len, uint8_t frames,
                                const sg0_frame_t *frame, uint16_t *len);
bool sg0_pack_view(const sg0_pack_t *pack, uint16_t index, sg0_view_t *out);
bool sg0_pack_layer(const sg0_pack_t *pack, uint16_t ref_index, sg0_layer_t *out);
bool sg0_pack_choice(const sg0_pack_t *pack, uint16_t index, sg0_choice_t *out);
bool sg0_pack_cond(const sg0_pack_t *pack, uint16_t index, sg0_cond_t *out);
bool sg0_pack_ending(const sg0_pack_t *pack, uint16_t index, sg0_ending_t *out);
bool sg0_pack_endlist(const sg0_pack_t *pack, uint8_t index, uint16_t *string_index);
bool sg0_pack_break(const sg0_pack_t *pack, uint16_t index, uint16_t *cursor);
bool sg0_pack_chgrp(const sg0_pack_t *pack, uint16_t index, sg0_chgrp_t *out);
bool sg0_pack_event(const sg0_pack_t *pack, uint16_t index, sg0_event_t *out);
bool sg0_pack_start(const sg0_pack_t *pack, uint16_t index, sg0_start_t *out);
bool sg0_pack_asset(const sg0_pack_t *pack, uint16_t index, sg0_assetid_t *out);
bool sg0_pack_part(const sg0_pack_t *pack, uint16_t index, sg0_part_t *out);
bool sg0_pack_title(const sg0_pack_t *pack, uint8_t group, const char **out, uint16_t *len);
bool sg0_pack_rine_msg(const sg0_pack_t *pack, uint16_t index, sg0_rine_msg_t *out);
bool sg0_pack_rine_reply(const sg0_pack_t *pack, uint16_t index, uint16_t *base,
                         uint8_t *contact, uint8_t *choices, uint16_t *first_choice,
                         uint16_t *key);
bool sg0_pack_rine_choice(const sg0_pack_t *pack, uint16_t index, uint16_t *selected,
                          uint16_t *seq_first, uint8_t *seq_count, uint8_t *stamp);
bool sg0_pack_rine_seq(const sg0_pack_t *pack, uint16_t index, uint16_t *ref);
bool sg0_pack_rine_contact(const sg0_pack_t *pack, uint8_t index, uint8_t *id,
                           const char **name, uint16_t *name_len,
                           const char **number, uint16_t *number_len);
bool sg0_pack_rine_base(const sg0_pack_t *pack, uint8_t index, uint16_t *base);
bool sg0_pack_card(const sg0_pack_t *pack, uint8_t group, uint16_t *asset);
bool sg0_pack_gallery_group(const sg0_pack_t *pack, uint8_t index, sg0_gallery_group_t *out);
bool sg0_pack_gallery_entry(const sg0_pack_t *pack, uint16_t index, uint16_t *asset);
uint16_t sg0_pack_meta_u16(const sg0_pack_t *pack, const char *key, uint16_t fallback);

// 包级统计(启动日志与门禁使用)。
uint32_t sg0_pack_chunk_count(const sg0_pack_t *pack);
uint32_t sg0_pack_asset_count(const sg0_pack_t *pack);
uint32_t sg0_pack_gallery_count(const sg0_pack_t *pack);
