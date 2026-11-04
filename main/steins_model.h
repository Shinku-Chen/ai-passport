// main/steins_model.h —— 与硬件无关的纯逻辑:折行/分页、存档、解锁、世界线。
//
// 这一层不包含 ESP-IDF / LVGL 头文件,宿主机测试直接编译。所有"排版算得出来、
// 渲染时也一定放得下"的前提都靠 steins_adv_table(由 tools/steins_font.py 从
// 同一份字体栅格化导出):这里的推进宽度和 LVGL 取到的 adv_w 同源。
#pragma once

#include "steins_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一页正文的缓冲。源剧本单帧最长 451 个 UTF-16 码元,UTF-8 最多约 1.4KB。
#define STEINS_TEXT_BYTES 4096
#define STEINS_PAGE_CHARS 200
// 折行工作区(码点)。手机/邮件正文实测最长 996 个码点(id 1,2988 UTF-8 B),
// 所以这里按 1024 留余量;超过它的文本 steins_page_build 返回 0(绝不静默截断)。
#define STEINS_TEXT_CHARS 1024
#define STEINS_LINE_MAX 96

// 排版参数(与 steins_ui.c 的几何一致)
#define STEINS_LINE_H 19
#define STEINS_TEXT_LINES 4
#define STEINS_TEXT_W 304
#define STEINS_TEXT_SIZE 16

// 存档模式:沿用源 state.progression 的语义
#define STEINS_MODE_STORY 0u
#define STEINS_MODE_CHAPTER 1u

// 结局上限(源 graph 里 9 个)
#define STEINS_MAX_ENDINGS 16
#define STEINS_MAX_CHAPTERS 16
#define STEINS_MAX_GALLERY 128

#define STEINS_SAVE_BYTES 16
#define STEINS_SAVE_VERSION 1u
#define STEINS_UNLOCK_BYTES 26
#define STEINS_UNLOCK_VERSION 1u

typedef struct {
    uint16_t node;
    uint32_t cursor;
    uint8_t mode;
    uint8_t flags;
} steins_pos_t;

typedef struct {
    char text[STEINS_TEXT_BYTES];          // 本页渲染用的 UTF-8(含插入的 '\n')
    uint16_t offsets[STEINS_PAGE_CHARS + 1]; // 第 n 个可见字符的字节起点
    int16_t chars;                         // 本页可见字符数
    int16_t bytes;
    int16_t page;
    int16_t pages;
} steins_text_page_t;

typedef struct {
    uint8_t version;      // STEINS_UNLOCK_VERSION
    uint8_t endings[2];   // 16 位:结局
    uint8_t chapters[2];  // 16 位:章节
    uint8_t gallery[16];  // 128 位:CG
    uint8_t flags;        // bit0 = 真结局
} steins_unlock_t;

// 一个字形的推进宽度(1/16 px,16px 字号下)。见 assets/fonts/steins_text_metrics.c。
uint16_t steins_adv_of(uint32_t cp);

size_t steins_utf8_next(const char *s, size_t len, size_t pos);
uint32_t steins_utf8_decode(const char *s, size_t len, size_t *pos);
// 全是空白(含 '\n')时返回 true —— 源里 skipBlank 的语义,这种帧直接跳过。
bool steins_text_blank(const char *utf8);
// 取前 n 个字符(不按字节截断),复制到 out;返回是否成功。
bool steins_text_prefix(const char *utf8, int chars, char *out, size_t cap);

// 把 utf8 折行并切页。lines_per_page 行一页;page_index 越界会夹到最后一页。
// 返回页数(>=1)。返回 0 表示失败(参数非法,或文本超出 STEINS_TEXT_CHARS 个工作区
// 码点——此时绝不返回"被截断的前几页",调用方必须显式报错/降级)。
int steins_page_build(const char *utf8, int size_px, int width_px, int lines_per_page,
                      int page_index, steins_text_page_t *out);

// 存档编解码:magic + version + mode + node + cursor + flags + CRC16。
size_t steins_save_encode(const steins_pos_t *pos, uint8_t *out, size_t cap);
bool steins_save_decode(steins_pos_t *pos, const uint8_t *data, size_t len);

void steins_unlock_init(steins_unlock_t *unlock);
bool steins_unlock_has_ending(const steins_unlock_t *u, uint8_t index);
bool steins_unlock_set_ending(steins_unlock_t *u, uint8_t index);
bool steins_unlock_has_chapter(const steins_unlock_t *u, uint8_t index);
bool steins_unlock_set_chapter(steins_unlock_t *u, uint8_t index);
bool steins_unlock_has_gallery(const steins_unlock_t *u, uint8_t index);
bool steins_unlock_set_gallery(steins_unlock_t *u, uint8_t index);
bool steins_unlock_true_end(const steins_unlock_t *u);
void steins_unlock_set_true_end(steins_unlock_t *u);
// 合并:把 other 里已解锁的位并进来;返回是否有新增(需要落盘)。
bool steins_unlock_merge(steins_unlock_t *u, const steins_unlock_t *other);
size_t steins_unlock_encode(const steins_unlock_t *u, uint8_t *out, size_t cap);
bool steins_unlock_decode(steins_unlock_t *u, const uint8_t *data, size_t len);

// ---------------------------------------------------------------- 手机/邮件 checkpoint
//
// 源把手机状态塞在 checkpoint 里一起存:lastNode/lastAt、inbox/sent 有序 id、
// read/unread/replied 集合、mailOwners、open 窗口、channels、call/callReturn、
// presentations、deferred、keyPhone 以及 endings。只存 node/cursor 会在换节点后重放
// 来电/投递(源 S10 反例:n0@900 会重放 862 来电),所以这些字段必须真的序列化。
//
// 编码是自描述的 varint + 定长位图:
//   'S','M', version, flags, mode, node, cursor, lastNode, lastAt,
//   inbox(count + ids…), sent(count + ids…), read[81], replied[81], unread[81],
//   owners(count + (id,owner)…), open(count + (id,channel)…), channels[5],
//   call(…), callReturn(…), presentations(…), deferred(…), keyPhone(…), endings,
//   CRC16
// 返回 0 表示"写不下/字段越界":调用方必须显式失败,绝不静默截断状态。
#define STEINS_MOBILE_BITMAP_BYTES ((STEINS_MAIL_IDS + 7) / 8)
// 引擎/源 restore 的 700 上限；事件机按该容量拒绝溢出，不截断。
// 源 restore 的 700 只是校验上限；实际 mail id 空间 646，receive/sent 都去重，
// 非法重复不作为可达状态。用 646 为新增 presentation key/readbits 留出 31 槽预算。
#define STEINS_CHK_MAX_INBOX STEINS_MAIL_IDS
#define STEINS_CHK_MAX_SENT STEINS_MAIL_IDS
#define STEINS_CHK_MAX_OWNERS 64
#define STEINS_CHK_MAX_OPEN 32
// presentation 队列按源 pending 容量 64；去重需要独立的 PRES key/readbits（lastAt
// 只保证前向，不能防回退/读档重入；见 engine-api §6.4 的 P1/P2）。
#define STEINS_CHK_MAX_PRES 64        // pending 队列容量（源 pending 需要 64）
#define STEINS_CHK_MAX_PRES_KEYS 56    // 源 PRES 目录实际 56 个合法 key (0..55)
#define STEINS_CHK_MAX_BYTES 4096
#define STEINS_CHK_VERSION 3u

typedef struct {
    uint8_t version;
    uint8_t mode;              // STEINS_MODE_*
    uint16_t node;
    uint32_t cursor;
    uint16_t last_node;        // 0xFFFF = 未进入过手机
    uint32_t last_at;          // 0xFFFFFFFF = 无
    uint16_t inbox_count;
    uint16_t inbox[STEINS_CHK_MAX_INBOX];
    uint16_t sent_count;
    uint16_t sent[STEINS_CHK_MAX_SENT];
    uint8_t read[(STEINS_MAIL_IDS + 7) / 8];
    uint8_t replied[(STEINS_MAIL_IDS + 7) / 8];
    uint8_t unread[(STEINS_MAIL_IDS + 7) / 8];
    uint8_t owner_count;
    uint16_t owner_id[STEINS_CHK_MAX_OWNERS];
    uint8_t owner_val[STEINS_CHK_MAX_OWNERS];
    uint8_t open_count;
    uint16_t open_id[STEINS_CHK_MAX_OPEN];
    uint8_t open_val[STEINS_CHK_MAX_OPEN];
    uint16_t channels[5];      // 源里是 999 或邮件 id,必须 u16
    uint16_t source_site;      // 源 sourceSite(phoneContext owner)
    uint8_t mail_life_version; // 源 mailLifeVersion
    uint8_t opening_done;      // 源 openingDone
    uint8_t call_present;
    uint16_t call_site;
    uint8_t call_owner;
    uint8_t call_kind;
    uint8_t call_waiting;
    uint16_t call_node;
    uint32_t call_at;
    uint8_t call_return_present;
    uint16_t call_return_node;
    uint32_t call_return_cursor;
    uint8_t pres_count;
    uint16_t pres_id[STEINS_CHK_MAX_PRES];
    uint8_t pres_kind[STEINS_CHK_MAX_PRES];
    uint8_t pres_owner[STEINS_CHK_MAX_PRES];
    // 源 presentationRead：56 个 presentation 事件 -> 56 bit；待播队列每项还带
    // 稳定的 PRES key 下标（0xFF=旧档/未知；有效 0..55）。回退/读档重入时按
    // key 精确去重；不把不同 checkpoint 的“未来”readbits 做全局合并。
    uint8_t pres_key[STEINS_CHK_MAX_PRES];
    uint8_t pres_read_bits[8];   // 前 56 bit 有效；byte7 保留必须为 0
    uint8_t deferred_present;
    uint16_t deferred_root;
    uint16_t deferred_id;
    uint8_t deferred_read;
    uint8_t keyphone_present;
    uint16_t keyphone_root;
    uint8_t keyphone_owner;
    uint16_t endings;          // 位图(16 个结局位)
} steins_checkpoint_t;

void steins_checkpoint_init(steins_checkpoint_t *chk);
bool steins_checkpoint_set_read(steins_checkpoint_t *chk, uint16_t id, bool value);
bool steins_checkpoint_read(const steins_checkpoint_t *chk, uint16_t id);
bool steins_checkpoint_set_replied(steins_checkpoint_t *chk, uint16_t id, bool value);
bool steins_checkpoint_replied(const steins_checkpoint_t *chk, uint16_t id);
bool steins_checkpoint_set_unread(steins_checkpoint_t *chk, uint16_t id, bool value);
bool steins_checkpoint_unread(const steins_checkpoint_t *chk, uint16_t id);
bool steins_checkpoint_push_inbox(steins_checkpoint_t *chk, uint16_t id);
bool steins_checkpoint_push_sent(steins_checkpoint_t *chk, uint16_t id);
// 序列化:成功返回字节数(>0);0 = 失败(字段越界或缓冲区放不下,状态不会被截断写入)。
size_t steins_checkpoint_encode(const steins_checkpoint_t *chk, uint8_t *out, size_t cap);
bool steins_checkpoint_decode(steins_checkpoint_t *chk, const uint8_t *data, size_t len);
// 逐字段相等(测试与"读档后状态一致"用)。
bool steins_checkpoint_equal(const steins_checkpoint_t *a, const steins_checkpoint_t *b);

// v2 存档 = checkpoint 编码('S','M',2)。不另造第二套 codec。
size_t steins_checkpoint_save_encode(const steins_checkpoint_t *chk, uint8_t *out,
                                     size_t cap);
// 接受 v2 checkpoint；也接受旧 v1 'S','G',1，并迁移为 checkpoint(空手机状态)。
bool steins_checkpoint_save_decode(steins_checkpoint_t *chk, const uint8_t *data,
                                   size_t len, bool *migrated_from_v1);

// 世界线:源按 (节点, 游标) 查表;超出范围取最后一条。
int32_t steins_worldline_rate(const steins_pack_t *pack, uint16_t node, uint32_t cursor);
// "1.130426" / "-0.275349"
void steins_worldline_text(int32_t rate, char *out, size_t cap);

// 章节标题卡:源按标题开头数字选卡,第 10 章有两个变体(按标题里是否含"因果关系")。
// 返回资源下标;0xFFFF 表示该章没有标题卡。
uint16_t steins_chapter_card_asset(const steins_pack_t *pack, const char *title);

// 文本是否含 PUA 私有区字形(源用 textFont() 判断字体,这里用于调试/审计)。
bool steins_text_has_pua(const char *utf8);

uint16_t steins_crc16(const uint8_t *data, size_t len);
