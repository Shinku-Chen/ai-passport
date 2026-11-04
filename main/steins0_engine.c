// main/steins0_engine.c —— 纯逻辑剧情解释器。
#include "steins0_engine.h"

#include <string.h>

void sg0_engine_init(sg0_state_t *state, uint8_t endings, uint8_t flags)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->node = 0;
    state->cursor = 0;
    state->mode = 0;
    state->endings = endings & 0x3Fu;
    if (state->endings & (1u << 5)) state->endings |= (uint8_t)(1u << 3);
    state->flags = flags;
    state->event_node = 0xFFu;
}

void sg0_engine_enter_chapter(sg0_state_t *state, const sg0_start_t *start)
{
    if (!state || !start) return;
    state->node = start->node;
    state->cursor = start->index;
    state->mode = 1;
    state->choice_active = 0;
    state->ended = 0;
    state->ending_id = 0;
    state->event_node = 0xFFu;
    state->event_cursor = 0;
}

static int base_bit(const sg0_pack_t *pack, uint16_t base)
{
    for (uint8_t i = 0; i < pack->sec[SG0_SEC_RBASE].count; ++i) {
        uint16_t value = 0;
        if (sg0_pack_rine_base(pack, i, &value) && value == base) return i;
    }
    return -1;
}

static bool rine_has_ref(const sg0_state_t *state, uint16_t ref)
{
    for (uint16_t i = 0; i < state->rine.count; ++i) {
        if (state->rine.refs[i] == ref) return true;
    }
    return false;
}

static void rine_add(const sg0_pack_t *pack, sg0_state_t *state, uint16_t ref)
{
    if (state->rine.count >= SG0_RINE_MAX || rine_has_ref(state, ref)) return;
    sg0_rine_msg_t msg;
    if (!sg0_pack_rine_msg(pack, ref, &msg)) return;
    const int bit = base_bit(pack, msg.base);
    if (bit >= 0 && (state->rine.replied & (1u << bit))) return;
    state->rine.refs[state->rine.count] = ref;
    state->rine.read[state->rine.count] = 0;
    ++state->rine.count;
}

static bool consume_events(const sg0_pack_t *pack, sg0_state_t *state, const sg0_node_t *node)
{
    if (state->event_node != state->node) {
        state->event_node = state->node;
        state->event_cursor = 0;
    }
    while (state->event_cursor < node->event_count) {
        sg0_event_t ev;
        const uint16_t index = (uint16_t)(node->event_first + state->event_cursor);
        if (!sg0_pack_event(pack, index, &ev)) return false;
        if (ev.at > state->cursor) break;
        ++state->event_cursor;
        if (ev.type == SG0_EVENT_CLOSE_CHOICE) {
            state->rine.pending_valid = 0;
        } else if (ev.type == SG0_EVENT_MESSAGE) {
            rine_add(pack, state, ev.data);
        } else if (ev.type == SG0_EVENT_RINE_RESET) {
            state->rine.windows = 0;
            if ((ev.flags & 1u) == 0) {
                state->rine.count = 0;
                state->rine.replied = 0;
            }
        } else if (ev.type == SG0_EVENT_RINE_REPLY) {
            const int bit = base_bit(pack, ev.c);
            if (bit >= 0) state->rine.windows |= (uint16_t)(1u << bit);
        } else if (ev.type == SG0_EVENT_EXPIRE_RINE) {
            const int bit = base_bit(pack, ev.c);
            if (bit >= 0) state->rine.windows &= (uint16_t)~(1u << bit);
            if (bit >= 0 && (state->rine.replied & (1u << bit)) == 0 && ev.b > 0) {
                bool read = false;
                for (uint16_t i = 0; i < state->rine.count; ++i) {
                    sg0_rine_msg_t msg;
                    if (!sg0_pack_rine_msg(pack, state->rine.refs[i], &msg)) continue;
                    if (msg.base == ev.c && !(msg.flags & 1u) && state->rine.read[i]) {
                        read = true;
                        break;
                    }
                }
                if (read) {
                    for (uint16_t i = 0; i < ev.b; ++i) {
                        rine_add(pack, state, (uint16_t)(ev.data + i));
                    }
                }
            }
        }
    }
    return true;
}

static uint8_t current_group(const sg0_pack_t *pack, const sg0_node_t *node, uint16_t cursor)
{
    uint8_t group = 0;
    for (uint8_t i = 0; i < node->chapter_count; ++i) {
        sg0_chgrp_t ch;
        if (!sg0_pack_chgrp(pack, (uint16_t)(node->chapter_first + i), &ch)) break;
        if (ch.cursor > cursor) break;
        group = ch.group;
    }
    return group;
}

bool sg0_engine_prev_group(const sg0_pack_t *pack, const sg0_state_t *state, uint8_t *group)
{
    sg0_node_t node;
    if (!pack || !state || !group || !sg0_pack_node(pack, state->node, &node)) return false;
    *group = current_group(pack, &node, state->cursor);
    return true;
}

sg0_place_t sg0_engine_locate(const sg0_pack_t *pack, sg0_state_t *state, sg0_choice_t *choice)
{
    if (!pack || !state) return SG0_PLACE_ERROR;
    if (state->ended) return SG0_PLACE_END;
    if (state->choice_active) {
        if (!choice) return SG0_PLACE_ERROR;
        sg0_node_t node;
        if (!sg0_pack_node(pack, state->node, &node) || node.choice == SG0_NONE) {
            return SG0_PLACE_ERROR;
        }
        return sg0_pack_choice(pack, node.choice, choice) ? SG0_PLACE_CHOICE : SG0_PLACE_ERROR;
    }
    for (int guard = 0; guard < 64; ++guard) {
        sg0_node_t node;
        if (!sg0_pack_node(pack, state->node, &node)) return SG0_PLACE_ERROR;
        if (!consume_events(pack, state, &node)) return SG0_PLACE_ERROR;
        if (state->cursor < node.chunk_count * 0u) return SG0_PLACE_ERROR;
        // cursor 是节点内累计帧号;逐块相加判断是否还有正文。
        uint32_t total = 0;
        for (uint8_t i = 0; i < node.chunk_count; ++i) {
            sg0_chunk_t chunk;
            if (!sg0_pack_chunk_at(pack, (uint16_t)(node.chunk_first + i), &chunk)) {
                return SG0_PLACE_ERROR;
            }
            total += chunk.frames;
        }
        if (state->cursor < total) return SG0_PLACE_TEXT;
        if (node.condition != SG0_NONE) {
            sg0_cond_t cond;
            if (!sg0_pack_cond(pack, node.condition, &cond)) return SG0_PLACE_ERROR;
            const bool yes = (state->endings & 0x01u) != 0;
            state->node = yes ? cond.yes : cond.no;
            state->cursor = 0;
            state->event_node = 0xFFu;
            continue;
        }
        if (node.choice != SG0_NONE) {
            sg0_choice_t value;
            if (!sg0_pack_choice(pack, node.choice, &value)) return SG0_PLACE_ERROR;
            if (!value.critical) {
                state->rine.pending_valid = 1;
                state->rine.pending_node = state->node;
                state->node = value.to[1];
                state->cursor = 0;
                state->event_node = 0xFFu;
                continue;
            }
            state->choice_active = 1;
            if (!choice) return SG0_PLACE_CHOICE;
            *choice = value;
            return SG0_PLACE_CHOICE;
        }
        if (node.ending >= 0) {
            sg0_ending_t ending;
            if (!sg0_pack_ending(pack, (uint16_t)node.ending, &ending)) {
                return SG0_PLACE_ERROR;
            }
            state->ended = 1;
            state->ending_id = ending.ending_id < 6 ? ending.ending_id : 0;
            state->endings |= (uint8_t)(1u << state->ending_id);
            if (state->ending_id == 5) state->endings |= (uint8_t)(1u << 3);
            return SG0_PLACE_END;
        }
        return SG0_PLACE_ERROR;
    }
    return SG0_PLACE_ERROR;
}

bool sg0_engine_choose(const sg0_pack_t *pack, sg0_state_t *state, uint8_t index)
{
    if (!pack || !state || !state->choice_active || index > 1) return false;
    sg0_node_t node;
    sg0_choice_t choice;
    if (!sg0_pack_node(pack, state->node, &node) || node.choice == SG0_NONE) return false;
    if (!sg0_pack_choice(pack, node.choice, &choice) || index >= choice.count) return false;
    state->choice_active = 0;
    state->rine.pending_valid = index == 1 ? 1 : 0;
    state->rine.pending_node = state->node;
    state->node = choice.to[index];
    state->cursor = 0;
    state->event_node = 0xFFu;
    state->event_cursor = 0;
    return true;
}

bool sg0_engine_next(const sg0_pack_t *pack, sg0_state_t *state)
{
    if (!pack || !state) return false;
    if (sg0_engine_locate(pack, state, NULL) != SG0_PLACE_TEXT) return false;
    ++state->cursor;
    return true;
}

bool sg0_engine_next_chapter(const sg0_pack_t *pack, sg0_state_t *state)
{
    if (!pack || !state) return false;
    if (sg0_engine_locate(pack, state, NULL) != SG0_PLACE_TEXT) return false;
    sg0_node_t node;
    if (!sg0_pack_node(pack, state->node, &node)) return false;
    const uint8_t group = current_group(pack, &node, state->cursor);
    uint32_t total = 0;
    for (uint8_t i = 0; i < node.chunk_count; ++i) {
        sg0_chunk_t chunk;
        if (!sg0_pack_chunk_at(pack, (uint16_t)(node.chunk_first + i), &chunk)) return false;
        total += chunk.frames;
    }
    uint16_t target = (uint16_t)total;
    // 先看源 chapters 表(章节开头),再看 breaks(普通分段)。
    for (uint8_t i = 0; i < node.chapter_count; ++i) {
        sg0_chgrp_t ch;
        if (!sg0_pack_chgrp(pack, (uint16_t)(node.chapter_first + i), &ch)) return false;
        if (ch.cursor > state->cursor && ch.group != group) {
            target = ch.cursor;
            break;
        }
    }
    if (target == (uint16_t)total) {
        for (uint8_t i = 0; i < node.break_count; ++i) {
            uint16_t at = 0;
            if (!sg0_pack_break(pack, (uint16_t)(node.break_first + i), &at)) return false;
            if (at > state->cursor) {
                target = at;
                break;
            }
        }
    }
    if (target > state->cursor) state->cursor = target;
    return true;
}

uint8_t sg0_engine_node_count(const sg0_pack_t *pack)
{
    return pack ? (uint8_t)(pack->sec[SG0_SEC_NODE].count > 255 ? 255
                                                              : pack->sec[SG0_SEC_NODE].count)
                : 0;
}

bool sg0_engine_answer_pending(const sg0_pack_t *pack, sg0_state_t *state, bool accept)
{
    if (!pack || !state || !state->rine.pending_valid) return false;
    sg0_node_t node;
    sg0_choice_t choice;
    if (!sg0_pack_node(pack, state->rine.pending_node, &node) || node.choice == SG0_NONE ||
        !sg0_pack_choice(pack, node.choice, &choice)) {
        return false;
    }
    if (!accept) return true;
    state->rine.pending_valid = 0;
    state->choice_active = 0;
    state->node = choice.to[0];
    state->cursor = 0;
    state->event_node = 0xFFu;
    state->event_cursor = 0;
    return true;
}

bool sg0_engine_event_ordinal(const sg0_pack_t *pack, uint8_t node_index,
                              uint16_t story_cursor, uint16_t *ordinal)
{
    if (!pack || !ordinal) return false;
    sg0_node_t node;
    if (!sg0_pack_node(pack, node_index, &node)) return false;
    uint16_t count = 0;
    for (uint8_t i = 0; i < node.event_count; ++i) {
        sg0_event_t ev;
        if (!sg0_pack_event(pack, (uint16_t)(node.event_first + i), &ev)) return false;
        if (ev.at > story_cursor) break;
        ++count;
    }
    *ordinal = count;
    return true;
}

uint16_t sg0_engine_rine_date(const sg0_pack_t *pack, const sg0_state_t *state,
                              uint16_t ref)
{
    if (!pack) return 0;
    sg0_rine_msg_t msg;
    if (!sg0_pack_rine_msg(pack, ref, &msg)) return 0;
    if (sg0_pack_date(pack, msg.date) != 0) return msg.date;
    // Source sendReply uses the last inbound mail with the same base as the
    // mother message. Prefer the live delivery order so reply/sequence rows
    // land in the same UI thread as the received mail.
    if (state) {
        for (uint16_t i = state->rine.count; i > 0; --i) {
            sg0_rine_msg_t mother;
            if (!sg0_pack_rine_msg(pack, state->rine.refs[i - 1], &mother)) continue;
            if (mother.flags & 1u) continue;
            if (mother.base != msg.base) continue;
            if (sg0_pack_date(pack, mother.date) != 0) return mother.date;
        }
    }
    // Deterministic fallback for a migrated/synthetic state that has no live
    // mother ref: the latest inbound catalog row for this base.
    uint16_t fallback = 0;
    for (uint16_t i = 0; i < pack->sec[SG0_SEC_RMSG].count; ++i) {
        sg0_rine_msg_t mother;
        if (!sg0_pack_rine_msg(pack, i, &mother)) continue;
        if (mother.flags & 1u) continue;
        if (mother.base != msg.base) continue;
        if (sg0_pack_date(pack, mother.date) != 0) fallback = mother.date;
    }
    return fallback;
}

bool sg0_engine_can_reply(const sg0_pack_t *pack, const sg0_state_t *state, uint16_t base)
{
    const int bit = base_bit(pack, base);
    if (bit < 0) return false;
    return (state->rine.windows & (1u << bit)) != 0 &&
           (state->rine.replied & (1u << bit)) == 0;
}

bool sg0_engine_send_reply(const sg0_pack_t *pack, sg0_state_t *state, uint16_t reply_index,
                           uint8_t choice_index)
{
    if (!pack || !state) return false;
    uint16_t base = 0;
    uint8_t contact = 0;
    uint8_t choices = 0;
    uint16_t first_choice = 0;
    uint16_t key = 0;
    if (!sg0_pack_rine_reply(pack, reply_index, &base, &contact, &choices, &first_choice, &key)) {
        return false;
    }
    (void)contact;
    (void)key;
    if (choice_index >= choices) return false;
    const int bit = base_bit(pack, base);
    if (bit < 0 || (state->rine.windows & (1u << bit)) == 0 ||
        (state->rine.replied & (1u << bit))) {
        return false;
    }
    uint16_t selected = 0;
    uint16_t seq_first = 0;
    uint8_t seq_count = 0;
    uint8_t stamp = 0;
    if (!sg0_pack_rine_choice(pack, (uint16_t)(first_choice + choice_index), &selected,
                              &seq_first, &seq_count, &stamp)) {
        return false;
    }
    (void)stamp;
    rine_add(pack, state, selected);
    for (uint8_t i = 0; i < seq_count; ++i) {
        uint16_t ref = 0;
        if (sg0_pack_rine_seq(pack, (uint16_t)(seq_first + i), &ref)) rine_add(pack, state, ref);
    }
    state->rine.replied |= (uint16_t)(1u << bit);
    state->rine.windows &= (uint16_t)~(1u << bit);
    return true;
}

void sg0_engine_mark_read(sg0_state_t *state)
{
    if (!state) return;
    for (uint16_t i = 0; i < state->rine.count; ++i) state->rine.read[i] = 1;
}

uint16_t sg0_engine_unread(const sg0_pack_t *pack, const sg0_state_t *state)
{
    if (!pack || !state) return 0;
    uint16_t count = 0;
    for (uint16_t i = 0; i < state->rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(pack, state->rine.refs[i], &msg)) continue;
        if ((msg.flags & 1u) == 0 && !state->rine.read[i]) ++count;
    }
    return count;
}

bool sg0_engine_cursor_valid(const sg0_pack_t *pack, uint8_t node_index, uint16_t cursor)
{
    sg0_node_t node;
    if (!pack || !sg0_pack_node(pack, node_index, &node)) return false;
    uint32_t total = 0;
    for (uint8_t i = 0; i < node.chunk_count; ++i) {
        sg0_chunk_t chunk;
        if (!sg0_pack_chunk_at(pack, (uint16_t)(node.chunk_first + i), &chunk)) return false;
        total += chunk.frames;
    }
    return cursor <= total;
}

bool sg0_engine_read(const sg0_pack_t *pack, sg0_state_t *state, uint8_t *buf, uint32_t cap,
                     sg0_chunk_t *chunk_out, uint32_t *raw_len, sg0_frame_t *frame,
                     const char **text, uint16_t *text_len)
{
    if (!pack || !state || !buf || !chunk_out || !raw_len || !frame || !text || !text_len) {
        return false;
    }
    if (sg0_engine_locate(pack, state, NULL) != SG0_PLACE_TEXT) return false;
    sg0_node_t node;
    if (!sg0_pack_node(pack, state->node, &node)) return false;
    uint32_t base = 0;
    sg0_chunk_t chunk;
    uint8_t local = 0xFFu;
    for (uint8_t i = 0; i < node.chunk_count; ++i) {
        if (!sg0_pack_chunk_at(pack, (uint16_t)(node.chunk_first + i), &chunk)) return false;
        if (state->cursor < base + chunk.frames) {
            local = (uint8_t)(state->cursor - base);
            break;
        }
        base += chunk.frames;
    }
    if (local == 0xFFu) return false;
    if (!sg0_pack_chunk_load(pack, &chunk, buf, cap, raw_len)) return false;
    if (!sg0_pack_frame(buf, *raw_len, local, frame)) return false;
    *text = sg0_pack_frame_text(buf, *raw_len, chunk.frames, frame, text_len);
    *chunk_out = chunk;
    return *text != NULL;
}
