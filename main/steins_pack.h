// main/steins_pack.h —— 《命运石之门》资源包的只读访问层。
//
// 包由 tools/steins_pack.py 生成,整包嵌进应用 Flash。运行时不解析 JSON、不做
// 文件系统访问:这一层只把"字节 -> 结构"按偏移映射出来。所有表项都是定长、小端
// 且**未对齐**(表头 32 字节 + 16 字节段表,段内定长记录),所以读取一律走 memcpy,
// 不要直接把 uint32_t* 指到表上。本层不依赖 ESP-IDF,宿主机测试直接编译。
//
// 字节布局(与 tools/steins_pack.py 一致,改动必须两边同时改):
//   header  { char magic[8] "STEINPK3"; u32 version; u32 total; u32 sections;
//             u32 r0, r1, r2 }                                              32 B
//   section { u32 type; u32 offset; u32 count; u32 size }                    16 B
//     STRT    { u32 off; u16 len }(UTF-16 码元数)                      6 B/条
//     TXT     UTF-16LE 字串池
//     NODE    { u16 id_str; u32 count; u16 chunk_first; u8 chunk_count;
//               u8 break_first; u8 break_count; u16 choice; u16 ending }  15 B
//     CHUNKREF u16 数组:节点引用的 chunk 下标
//     CHUNK   { u32 first_frame; u16 count; u16 pad }                    8 B
//     FRAME   { u16 text; u16 speaker; u16 view; u16 aux; u16 title }    10 B
//     VIEW    { u8 count; u8 pad; u16 layer_first }                      4 B
//     LAYER   { u16 asset; i16 x; i16 y; u16 w; u16 h; u16 opacity }     12 B
//     ASSET   { u32 off; u32 len; u32 mask_off; u32 mask_len;
//               u16 w; u16 h; u16 mw; u16 mh;
//               u16 cx; u16 cy; u16 cw; u16 ch; u16 ow; u16 oh;
//               u16 flags }                                             40 B
//     BLOB    JPEG 与 1bpp 遮罩,4 字节对齐
//     CHOICE  { u16 heading; u16 category; u16 first_item; u8 count;
//               u8 kind; u8 pad[4] }                                    12 B
//     CITEM   { u16 text; u16 label; u16 to; u8 action; u8 pad; u8 r }   9 B
//     CHAPTER { u16 name; u16 node; u32 cursor }                          8 B
//     GAL     { u16 label; u16 first; u16 count; u16 pad }                8 B
//     GENTRY  { u16 asset; u16 pad }                                      4 B
//     WLINE   { u16 node; u16 first; u8 count; u8 pad; i32 rate }        10 B
//     WCHANGE { u32 cursor; i32 rate }                                    8 B
//     BREAK   u32 数组:下一章跳点
//     UI      { u16 key_str; u16 value_str }                              4 B
//     META    key=value 文本
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STEINS_PACK_MAGIC "STEINPK3"
// 邮件 id 空间(源 mailIndex 646 条):checkpoint 位图与 seed/call 引用都用它做上界。
#define STEINS_MAIL_IDS 646
// v3 的 20 个段一个字节都不变;手机段是**追加**的,所以先保持 3 —— 打包器把新段
// 写进同一个包时再一起升到 4(build/mobile-integration/pack-api.md 里同步说明)。
#define STEINS_PACK_VERSION 5u
// v5:TXT 段是 2 KiB 独立 raw-DEFLATE 块,TXTRAW 记 raw 长度,TXTBLK 每块 8 B
#define STEINS_NONE 0xFFFFu

// 画布:320x180 覆盖源 432x243 艺术画布(等比 0.7407407)。正文页对白带从
// 画布 y=150 起且不透明,所以只出现在正文里的图在包里只保留 150/180。
#define STEINS_CANVAS_W 320
#define STEINS_CANVAS_H 180
#define STEINS_SCENE_VISIBLE_H 150

// 选择项动作
enum {
    STEINS_ACT_READ = 0,
    STEINS_ACT_IGNORE,
    STEINS_ACT_SEND,
    STEINS_ACT_NOSEND,
    STEINS_ACT_CALL,
    STEINS_ACT_NOCALL,
    STEINS_ACT_REPLY,
    STEINS_ACT_DMAIL,
};

// 选择项种类:mail / dmail / phone / reply
enum {
    STEINS_KIND_MAIL = 0,
    STEINS_KIND_DMAIL,
    STEINS_KIND_PHONE,
    STEINS_KIND_REPLY,
};

enum {
    STEINS_SEC_STRT = 1,
    STEINS_SEC_TXT,
    STEINS_SEC_NODE,
    STEINS_SEC_CHUNKREF,
    STEINS_SEC_CHUNK,
    STEINS_SEC_FRAME,
    STEINS_SEC_VIEW,
    STEINS_SEC_LAYER,
    STEINS_SEC_ASSET,
    STEINS_SEC_BLOB,
    STEINS_SEC_CHOICE,
    STEINS_SEC_CITEM,
    STEINS_SEC_CHAPTER,
    STEINS_SEC_GAL,
    STEINS_SEC_GENTRY,
    STEINS_SEC_WLINE,
    STEINS_SEC_WCHANGE,
    STEINS_SEC_BREAK,
    STEINS_SEC_UI,
    STEINS_SEC_META,
    // ---- v4:手机/邮件内容(见 build/mobile-integration/pack-api.md)
    STEINS_SEC_MAIL,
    STEINS_SEC_MAIL_SUCC,
    STEINS_SEC_MAIL_STAMP,
    STEINS_SEC_MROW,
    STEINS_SEC_MEVT,
    STEINS_SEC_MNODE,
    STEINS_SEC_CALLSITE,
    STEINS_SEC_CALLITEM,
    STEINS_SEC_CLIP,
    STEINS_SEC_CFRAME,
    STEINS_SEC_WINDOW,
    STEINS_SEC_WINOPT,
    STEINS_SEC_KEYPHONE,
    STEINS_SEC_MOBILE,
    STEINS_SEC_MMAP,
    STEINS_SEC_SEED,
    STEINS_SEC_SEED_ID,
    STEINS_SEC_CALLDESC,
    STEINS_SEC_CALLDITEM,
    STEINS_SEC_TXTBLK,
    STEINS_SEC_TXTRAW,
    STEINS_SEC_PRES,
    STEINS_SEC_CHOICE_META,
    STEINS_SEC_CITEM_META,
    STEINS_SEC_COUNT
};

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t sec_off[STEINS_SEC_COUNT];
    uint32_t sec_count[STEINS_SEC_COUNT];
    uint8_t sec_version;
    uint32_t txt_raw_len;   // v5:逻辑 raw 长度;v3/v4 = TXT 段长度
    uint32_t sec_size[STEINS_SEC_COUNT];
} steins_pack_t;

typedef struct {
    uint16_t id_str;
    uint32_t count;
    uint16_t chunk_first;
    uint8_t chunk_count;
    uint8_t break_first;
    uint8_t break_count;
    uint16_t choice;
    uint16_t ending;
} steins_node_t;

typedef struct {
    uint32_t first_frame;
    uint16_t count;
} steins_chunk_t;

typedef struct {
    uint16_t text;
    uint16_t speaker;
    uint16_t view;
    uint16_t aux;      // 日期字串(源 row[3])
    uint16_t title;    // 章节标题字串(源 row[4])
} steins_frame_t;

typedef struct {
    uint8_t count;
    uint16_t layer_first;
} steins_view_t;

typedef struct {
    uint16_t asset;
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t opacity;   // /1000
} steins_layer_t;

typedef struct {
    uint32_t off, len;
    uint32_t mask_off, mask_len;
    uint16_t w, h;          // JPEG 尺寸
    uint16_t mw, mh;        // 半分辨率遮罩尺寸
    uint16_t cx, cy, cw, ch; // 在原始图上的裁剪框
    uint16_t ow, oh;        // 原始尺寸
    uint16_t flags;
} steins_asset_t;

typedef struct {
    uint16_t heading;
    uint16_t category;
    uint16_t first_item;
    uint8_t count;
    uint8_t kind;
} steins_choice_t;

typedef struct {
    uint16_t text;
    uint16_t label;
    uint16_t to;
    uint8_t action;
} steins_citem_t;

typedef struct {
    uint16_t name;
    uint16_t node;
    uint32_t cursor;
} steins_chapter_t;

typedef struct {
    uint16_t label;
    uint16_t first;
    uint16_t count;
} steins_gal_t;

typedef struct {
    uint16_t node;
    uint16_t first;
    uint8_t count;
    int32_t rate;
} steins_wline_t;

typedef struct {
    uint32_t cursor;
    int32_t rate;
} steins_wchange_t;

static inline uint16_t steins_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t steins_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// 打开包:校验 magic/version/总长/段表。失败返回 false,pack 内容不可用。
bool steins_pack_open(steins_pack_t *pack, const uint8_t *data, uint32_t size);

// 内嵌包(固件用 CMake EMBED_FILES;宿主机测试用下面的 host hook)。
bool steins_pack_open_embedded(steins_pack_t *pack);

#if !defined(ESP_PLATFORM)
// 宿主机测试:注册一份内存镜像来替代 EMBED_FILES 符号。
void steins_pack_host_set(const uint8_t *data, uint32_t size);
#endif

bool steins_pack_node(const steins_pack_t *pack, uint16_t index, steins_node_t *out);
bool steins_pack_chunk(const steins_pack_t *pack, uint16_t index, steins_chunk_t *out);
// 节点第 n 个 chunk 的下标。
bool steins_pack_node_chunk(const steins_pack_t *pack, const steins_node_t *node,
                            uint16_t n, uint16_t *chunk_out);
bool steins_pack_frame(const steins_pack_t *pack, uint32_t index, steins_frame_t *out);
bool steins_pack_view(const steins_pack_t *pack, uint16_t index, steins_view_t *out);
bool steins_pack_layer(const steins_pack_t *pack, uint16_t index, steins_layer_t *out);
bool steins_pack_asset(const steins_pack_t *pack, uint16_t index, steins_asset_t *out);
bool steins_pack_choice(const steins_pack_t *pack, uint16_t index, steins_choice_t *out);
bool steins_pack_citem(const steins_pack_t *pack, uint16_t index, steins_citem_t *out);
bool steins_pack_chapter(const steins_pack_t *pack, uint16_t index, steins_chapter_t *out);
bool steins_pack_gal(const steins_pack_t *pack, uint16_t index, steins_gal_t *out);
bool steins_pack_gentry(const steins_pack_t *pack, uint16_t index, uint16_t *asset_out);
bool steins_pack_wline(const steins_pack_t *pack, uint16_t node, steins_wline_t *out);
bool steins_pack_wchange(const steins_pack_t *pack, uint16_t index, steins_wchange_t *out);
bool steins_pack_break(const steins_pack_t *pack, uint16_t index, uint32_t *cursor_out);
uint32_t steins_pack_break_count(const steins_pack_t *pack);
uint32_t steins_pack_choice_count(const steins_pack_t *pack);

// 字串:返回 UTF-16LE 码元指针与长度(不含 NUL,也不保证有 NUL)。
const uint16_t *steins_pack_str(const steins_pack_t *pack, uint16_t index, uint16_t *len_out);
// 复制成 UTF-8。cap > 0 时**无论成功还是失败** out 都是以 NUL 结尾的合法码点前缀:
// 返回写入字节数;返回 -1 表示下标非法、遇到孤立代理或缓冲区放不下(此时 out 里是
// 能完整编码出来的前缀,NUL 结尾,绝不会残留上一次内容或半个字符)。
int steins_pack_str_utf8(const steins_pack_t *pack, uint16_t index, char *out, size_t cap);

// 节点 id 字串("n0"/"n306"),失败返回 NULL。
const uint16_t *steins_pack_node_id(const steins_pack_t *pack, uint16_t node_index,
                                    uint16_t *len_out);

// META:整数键(缺失返回 fallback)。
int steins_pack_meta_int(const steins_pack_t *pack, const char *key, int fallback);
// META:取 text 行的剩余部分(用于 card_table 等逗号列表)。
bool steins_pack_meta_str(const steins_pack_t *pack, const char *key, char *out, size_t cap);

// UI 文案:按 ASCII 键查字串下标,缺失返回 STEINS_NONE。
uint16_t steins_pack_ui(const steins_pack_t *pack, const char *key);

// ---------------------------------------------------------------- v4 手机内容
// 记录结构与 tools/steins_pack.py、build/mobile-integration/pack-api.md 一一对应。
enum {
    STEINS_EV_SEED = 0,
    STEINS_EV_RECEIVE,
    STEINS_EV_ERASE,
    STEINS_EV_READBIT,
    STEINS_EV_REPLYBIT,
    STEINS_EV_EXPIRE,
    STEINS_EV_QUEUE,
    STEINS_EV_DISPATCH,
    STEINS_EV_RULE,
    STEINS_EV_CALL,
    STEINS_EV_PRESENTATION,
    STEINS_EV_KIND_COUNT
};

// 源事件的最大参数数:call 是 argc=5(seed/call 的首参是 typed 描述符下标)。
#define STEINS_MEVENT_MAX_ARGS 5u
// presentation 的 kind 是字符串,包里存显式 enum(与 tools/steins_mobile_pack.py 一致)。
enum {
    STEINS_PK_DRAFT = 0,
    STEINS_PK_SEND = 1,
    STEINS_PK_VIEW = 2,
    STEINS_PK_INBOX = 3,
    STEINS_PK_READ = 4,
    STEINS_PK_OTHER = 5
};
#define STEINS_MAIL_SUCC_MAX 8u

typedef struct {
    uint16_t sender;      // 字符串池下标
    uint16_t title;
    uint16_t body;        // 0xFFFF = 空正文
    uint16_t succ_first;  // STEINS_SEC_MAIL_SUCC 起点
    uint8_t type;         // 源 mailIndex[id][2]
    uint8_t succ_count;
} steins_mail_t;

typedef struct {
    int32_t cursor;       // 源行游标(**有符号**:seed 行就是 -1,按源原序保留)
    uint32_t event_off;   // STEINS_SEC_MEVT 内字节偏移
    uint8_t file;         // 手机事件文件号
    uint8_t event_count;
} steins_mrow_t;

typedef struct {
    uint8_t kind;                            // STEINS_EV_*
    uint8_t argc;
    uint16_t arg[STEINS_MEVENT_MAX_ARGS];
} steins_mevent_t;

typedef struct {
    uint16_t site;
    uint16_t first_item;
    uint16_t source_str;
    uint8_t owner;
    uint8_t kind;
    uint8_t item_count;
    uint8_t limit;
} steins_callsite_t;

typedef struct {
    uint16_t text;
    uint16_t id;
    uint16_t parent;
    uint8_t action;
    uint8_t clip;
    uint8_t join;
} steins_callitem_t;

typedef struct {
    uint16_t first_frame;
    uint16_t frame_count;
    uint16_t return_frame;
} steins_clip_t;

typedef struct {
    uint16_t speaker;
    uint16_t text;
} steins_cframe_t;

typedef struct {
    uint16_t node;        // 剧情节点下标
    uint16_t id;          // 窗口邮件 id
    uint16_t limit;
    uint16_t skip;
    uint16_t read;
    uint16_t first_opt;
    uint8_t opt_count;
} steins_window_t;

typedef struct {
    uint16_t text;
    uint16_t id;          // 回复邮件 id
    uint16_t parent;      // 被回复的邮件 id
    uint16_t to;          // 目标剧情节点下标(源 to@limit 的真落点)
    uint8_t action;
} steins_winopt_t;

typedef struct {
    uint16_t node;
    uint16_t skip;
    uint16_t go;
    uint16_t limit;
    uint16_t after;
    uint16_t sent;
    uint16_t source_str;
} steins_keyphone_t;

uint16_t steins_pack_mail_count(const steins_pack_t *pack);
bool steins_pack_mail(const steins_pack_t *pack, uint16_t id, steins_mail_t *out);
uint8_t steins_pack_mail_succ(const steins_pack_t *pack, const steins_mail_t *mail,
                              uint16_t *out, uint8_t cap, bool type3_only);
bool steins_pack_mail_stamp(const steins_pack_t *pack, uint16_t id, uint16_t *lo, uint16_t *hi);

// MNODE 是 6 B 的**节点区间**:{u8 file, u8 pad, u16 first_row, u16 row_count}。
// 同一个事件文件会被多个节点共用(file0 共 213 行,n0 只有 212;file1 共 189,n3 只有 150),
// 所以按"节点自己的行区间"取,不能把整个 file 都给节点,也不能按无符号游标排序。
bool steins_pack_mobile_file(const steins_pack_t *pack, uint16_t node, uint8_t *file_out);
// PRES:五元 key =(source chapter 节点身份, 剧情 cursor, id, kind, owner)。
// node 是**包下标**;row_index 是该节点 MNODE 区间内的**局部**行号;event_index 是该行内
// presentation 事件的序号。三者都经过 MNODE/MROW 边界检查后才查表。
#define STEINS_PACK_HAVE_PRES_KEY 1
bool steins_pack_pres_key(const steins_pack_t *pack, uint16_t node, uint16_t row_index,
                          uint8_t event_index, uint16_t *key_index);
// 主图 CHOICE/CITEM 的 side 元数据(source 脚本名 / 选择项 id),不动原主图宽度。
bool steins_pack_choice_meta(const steins_pack_t *pack, uint16_t choice, uint16_t *source_str,
                             uint16_t *choice_id, uint8_t *kind);
// CHOICE_META 尾部 u16 = 该选择的 **resolved SEND id**(源 phone.js:15 语义):
// 仅 kind==send/dmail 有值(phoneData.dmail[choice.source],如 SG06_23→233、SG07_13→253);
// kind==reply 的副作用是 **CITEM.item.id**(不是母 choice.id),此时本函数返回 false 且不改 out。
// 源 choice kinds:mail 21 / reply 20 / dmail 18 / phone 6;reply 选项共 26 条。
// 尾部在 reply 上是未消费的保留字段,读它没有意义。
bool steins_pack_choice_outgoing(const steins_pack_t *pack, uint16_t choice, uint16_t *mail_id);
bool steins_pack_citem_meta(const steins_pack_t *pack, uint16_t item, uint16_t *item_id);

bool steins_pack_node_source(const steins_pack_t *pack, uint16_t node, uint16_t *source_out);
bool steins_pack_mrow_range(const steins_pack_t *pack, uint16_t node,
                            uint16_t *first_row, uint16_t *row_count);
uint16_t steins_pack_mrow_count(const steins_pack_t *pack);
bool steins_pack_mrow(const steins_pack_t *pack, uint16_t index, steins_mrow_t *out);
bool steins_pack_mevent(const steins_pack_t *pack, const steins_mrow_t *row, uint8_t index,
                        steins_mevent_t *out);

// seed 描述符:arg0 指向 SEED{first_id,count},ID 流在 SEED_ID。顺序=源顺序。
bool steins_pack_seed_desc(const steins_pack_t *pack, uint16_t desc,
                           uint16_t *first_id, uint16_t *count);
bool steins_pack_seed_id(const steins_pack_t *pack, uint16_t desc, uint16_t index,
                         uint16_t *id_out);
// call 描述符(CALLDESC 8 B):site/kind 字符串 + 有序 items(CALLDITEM) + waiting/owner。
// 注意:desc 顺序与 CALLSITE 表**不同**,适配器必须用 site/kind 做映射,不能假设同序。
bool steins_pack_call_desc(const steins_pack_t *pack, uint16_t desc, uint16_t *site_str,
                           uint16_t *kind_str, uint16_t *first_item, uint8_t *item_count,
                           uint8_t *owner, bool *waiting);
bool steins_pack_call_ditem(const steins_pack_t *pack, uint16_t index, uint16_t *id_out,
                            uint16_t *action_str, uint16_t *text_str);

uint16_t steins_pack_call_site_count(const steins_pack_t *pack);
bool steins_pack_call_site(const steins_pack_t *pack, uint16_t index, steins_callsite_t *out);
bool steins_pack_call_item(const steins_pack_t *pack, uint16_t index, steins_callitem_t *out);
uint16_t steins_pack_clip_count(const steins_pack_t *pack);
bool steins_pack_clip(const steins_pack_t *pack, uint8_t clip, steins_clip_t *out);
uint16_t steins_pack_cframe_count(const steins_pack_t *pack);
bool steins_pack_cframe(const steins_pack_t *pack, uint16_t index, steins_cframe_t *out);

bool steins_pack_mobile_window(const steins_pack_t *pack, uint16_t node, steins_window_t *out);
bool steins_pack_window_opt(const steins_pack_t *pack, uint16_t index, steins_winopt_t *out);
bool steins_pack_keyphone(const steins_pack_t *pack, uint16_t node, steins_keyphone_t *out);

bool steins_pack_node_map(const steins_pack_t *pack, uint16_t source_node, uint16_t *pack_node);

uint32_t steins_pack_total(const steins_pack_t *pack);
uint32_t steins_pack_count(const steins_pack_t *pack, uint32_t section);
