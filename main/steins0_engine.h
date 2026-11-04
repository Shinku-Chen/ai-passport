// main/steins0_engine.h —— 源剧情的纯逻辑解释器(不依赖 LVGL/ESP-IDF)。
//
// 对照转换源的 engine.js:节点(node)内的 cursor、节点结束后的 condition/choice/
// ending 连接、章节入口与“下一章”跳段。选项在节点末尾显式确认(源的非关键来电
// 允许稍后从手机接听;本移植把同一组两个分支都放成显式选择,不丢支线内容)。
#pragma once

#include "steins0_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define SG0_RINE_MAX 320

typedef struct {
    uint16_t count;
    uint16_t refs[SG0_RINE_MAX];   // RMSG 目录下标,按投递顺序
    uint8_t read[SG0_RINE_MAX];    // 已读(仅 inbox)
    uint16_t replied;              // 按 RBASE 下标的已回复位
    uint16_t windows;              // 当前可回复窗口位
    uint8_t pending_valid;         // 源 pendingCall
    uint8_t pending_node;
} sg0_rine_t;

typedef enum {
    SG0_PLACE_TEXT = 0,
    SG0_PLACE_CHOICE,
    SG0_PLACE_END,
    SG0_PLACE_ERROR,
} sg0_place_t;

typedef struct {
    uint8_t node;
    uint16_t cursor;
    uint8_t mode;          // 0 = story, 1 = chapter
    uint8_t endings;       // 6 个结局位(周目内状态;由全局解锁播种)
    uint8_t flags;         // bit0 prComplete
    uint8_t choice_active;
    uint8_t ended;
    uint8_t ending_id;
    uint8_t event_node;    // 事件游标所属节点(避免重复消费)
    uint16_t event_cursor;
    sg0_rine_t rine;
} sg0_state_t;

void sg0_engine_init(sg0_state_t *state, uint8_t endings, uint8_t flags);
void sg0_engine_enter_chapter(sg0_state_t *state, const sg0_start_t *start);
sg0_place_t sg0_engine_locate(const sg0_pack_t *pack, sg0_state_t *state,
                              sg0_choice_t *choice);
bool sg0_engine_choose(const sg0_pack_t *pack, sg0_state_t *state, uint8_t index);
bool sg0_engine_next(const sg0_pack_t *pack, sg0_state_t *state);
bool sg0_engine_next_chapter(const sg0_pack_t *pack, sg0_state_t *state);
// Number of node events consumed at a story cursor (event ordinal, not story cursor).
bool sg0_engine_event_ordinal(const sg0_pack_t *pack, uint8_t node_index,
                              uint16_t story_cursor, uint16_t *ordinal);
// Effective date-table index for a RINE ref; reply/sequence rows with missing
// date inherit the last inbound mother message for the same base.
uint16_t sg0_engine_rine_date(const sg0_pack_t *pack, const sg0_state_t *state,
                              uint16_t ref);
bool sg0_engine_answer_pending(const sg0_pack_t *pack, sg0_state_t *state, bool accept);
bool sg0_engine_send_reply(const sg0_pack_t *pack, sg0_state_t *state, uint16_t reply_index,
                           uint8_t choice_index);
void sg0_engine_mark_read(sg0_state_t *state);
uint16_t sg0_engine_unread(const sg0_pack_t *pack, const sg0_state_t *state);
bool sg0_engine_can_reply(const sg0_pack_t *pack, const sg0_state_t *state, uint16_t base);
bool sg0_engine_prev_group(const sg0_pack_t *pack, const sg0_state_t *state,
                           uint8_t *group);

// 读取当前 cursor 的帧(含块解压)。buf 由调用方提供(>= 8 KB)。
bool sg0_engine_read(const sg0_pack_t *pack, sg0_state_t *state, uint8_t *buf, uint32_t cap,
                     sg0_chunk_t *chunk, uint32_t *raw_len, sg0_frame_t *frame,
                     const char **text, uint16_t *text_len);

// 节点内 cursor 是否落在合法范围(读档预检)。
bool sg0_engine_cursor_valid(const sg0_pack_t *pack, uint8_t node, uint16_t cursor);
uint8_t sg0_engine_node_count(const sg0_pack_t *pack);
