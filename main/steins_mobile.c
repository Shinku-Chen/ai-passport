// main/steins_mobile.c —— 源 `mobile.js` / `phone.js` / `engine.js(locate,choose)` 的手机语义移植。
//
// 逐函数对照（左侧为本文件函数，右侧为源函数）：
//   mob_receive            receiveMobile
//   mob_erase              eraseMail
//   mob_sync               syncMobile（含 migrateMail 的等价处理）
//   mob_mark_read          markMailRead
//   mob_send_optional      sendOptional / canReply
//   mob_defer_begin        beginDeferred
//   mob_deferred_boundary  deferredBoundary / deferredLimit
//   mob_answer_deferred    answerDeferred
//   mob_defer_key_phone    deferKeyPhone
//   mob_keyphone_boundary  keyPhoneBoundary / keyPhoneLimit
//   mob_answer_key_phone   answerKeyPhone
//   mob_current_call       currentCall
//   mob_take_call          takeCall
//   mob_return_call        returnCall
//   mob_plot_presentation  plotPresentation
//   mob_finish_pres        finishPresentation
//   mob_next_pres_boundary nextPresentationBoundary
//   steins_mobile_locate   locate() 的手机部分
//   steins_mobile_choose   choose() 的手机部分
//
// 容量与失败约定：先校验再写；任何容量不足/内容缺失都返回 false **且不改动 checkpoint**。
#include "steins_mobile.h"

#include <string.h>

// ---------------------------------------------------------------- 小工具
static bool id_ok(uint16_t id) { return id < STEINS_MAIL_IDS; }

static bool id_in(const uint16_t *ids, uint16_t count, uint16_t id)
{
    for (uint16_t i = 0; i < count; ++i) {
        if (ids[i] == id) return true;
    }
    return false;
}

static bool pres_kind_valid(uint8_t kind) { return kind <= STEINS_MOB_PRES_VIEW; }

static bool call_kind_valid(uint8_t kind) { return kind <= STEINS_MOB_CALL_DMAIL; }

// ---------------------------------------------------------------- mail 元数据
static bool mail_meta(const steins_mobile_t *m, uint16_t id, steins_mobile_mail_t *out)
{
    if (!m || !m->content) return false;
    if (!id_ok(id)) return false;
    memset(out, 0, sizeof(*out));
    if (!m->content->mail_meta || !m->content->mail_meta(m->ctx, id, out)) return false;
    out->id = id;
    return true;
}

static bool mail_has_type(const steins_mobile_t *m, uint16_t id, uint8_t type)
{
    steins_mobile_mail_t meta;
    return mail_meta(m, id, &meta) && meta.type == type;
}

// 源 mailIndex[id][3].some(x => mailIndex[x][2] === type)
static bool mail_succ_has_type(const steins_mobile_t *m, uint16_t id, uint8_t type)
{
    uint16_t succ[16];
    if (!m->content->mail_succ) return false;
    const uint8_t n = m->content->mail_succ(m->ctx, id, succ, (uint8_t)(sizeof(succ) / sizeof(succ[0])));
    for (uint8_t i = 0; i < n; ++i) {
        steins_mobile_mail_t meta;
        if (mail_meta(m, succ[i], &meta) && meta.type == type) return true;
    }
    return false;
}

// 源 row[3].find(x => mailIndex[x][2] === 2)（找不到返回 0xFFFF）。
static uint16_t mail_succ_first_type2(const steins_mobile_t *m, uint16_t id)
{
    uint16_t succ[16];
    if (!m->content->mail_succ) return 0xFFFFu;
    const uint8_t n = m->content->mail_succ(m->ctx, id, succ, (uint8_t)(sizeof(succ) / sizeof(succ[0])));
    for (uint8_t i = 0; i < n; ++i) {
        steins_mobile_mail_t meta;
        if (mail_meta(m, succ[i], &meta) && meta.type == STEINS_MOB_MAIL_TYPE_INCOMING) {
            return succ[i];
        }
    }
    return 0xFFFFu;
}

// ---------------------------------------------------------------- open（源 m.open 字典）
// 编码：open_val = channel + 1（源里 channel 可以是 -1，0 是合法通道）。
static int16_t open_lookup(const steins_checkpoint_t *chk, uint16_t id)
{
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        if (chk->open_id[i] == id) return (int16_t)((int16_t)chk->open_val[i] - 1);
    }
    return -1;
}

static bool open_set(steins_checkpoint_t *chk, uint16_t id, int32_t channel)
{
    if (channel < -1 || channel > 254) return false;
    const uint8_t encoded = (uint8_t)(channel + 1);
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        if (chk->open_id[i] == id) { chk->open_val[i] = encoded; return true; }
    }
    if (chk->open_count >= STEINS_CHK_MAX_OPEN) return false;
    chk->open_id[chk->open_count] = id;
    chk->open_val[chk->open_count] = encoded;
    chk->open_count++;
    return true;
}

static bool open_remove(steins_checkpoint_t *chk, uint16_t id)
{
    uint8_t out = 0;
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        if (chk->open_id[i] == id) continue;
        chk->open_id[out] = chk->open_id[i];
        chk->open_val[out] = chk->open_val[i];
        out++;
    }
    chk->open_count = out;
    return true;
}

// ---------------------------------------------------------------- mailOwners（稀疏）
// 源：`(state.mailOwners||{})[id]||0` —— 缺失恒等于 0，所以只存非零 owner。
static uint8_t owner_get(const steins_checkpoint_t *chk, uint16_t id)
{
    for (uint8_t i = 0; i < chk->owner_count; ++i) {
        if (chk->owner_id[i] == id) return chk->owner_val[i];
    }
    return 0;
}

static bool owner_set(steins_checkpoint_t *chk, uint16_t id, uint8_t owner)
{
    if (!id_ok(id)) return false;
    for (uint8_t i = 0; i < chk->owner_count; ++i) {
        if (chk->owner_id[i] == id) {
            if (owner == 0) {   // 归零 = 删除（语义等价）
                chk->owner_id[i] = chk->owner_id[chk->owner_count - 1];
                chk->owner_val[i] = chk->owner_val[chk->owner_count - 1];
                chk->owner_count--;
            } else {
                chk->owner_val[i] = owner;
            }
            return true;
        }
    }
    if (owner == 0) return true;
    if (chk->owner_count >= STEINS_CHK_MAX_OWNERS) return false;
    chk->owner_id[chk->owner_count] = id;
    chk->owner_val[chk->owner_count] = owner;
    chk->owner_count++;
    return true;
}

// ---------------------------------------------------------------- receive / erase
static bool mob_receive(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t id, int32_t channel,
                        bool open, uint8_t owner)
{
    steins_mobile_mail_t meta;
    if (!mail_meta(m, id, &meta)) return false;   // 源：if(!mailIndex[id]) return;
    // 先校验 owner 容量（owner 只在非零且新出现时占槽），失败则整体不改动。
    if (owner != 0 && owner_get(chk, id) == 0 && chk->owner_count >= STEINS_CHK_MAX_OWNERS) {
        return false;
    }
    if (!id_in(chk->inbox, chk->inbox_count, id)) {
        if (chk->inbox_count >= STEINS_CHK_MAX_INBOX) return false;
        chk->inbox[chk->inbox_count++] = id;
        if ((meta.type == STEINS_MOB_MAIL_TYPE_TEXT || meta.type == STEINS_MOB_MAIL_TYPE_INCOMING) &&
            !steins_checkpoint_read(chk, id)) {
            (void)steins_checkpoint_set_unread(chk, id, true);
        }
    }
    if (!owner_set(chk, id, owner)) return false;
    if (open && !steins_checkpoint_replied(chk, id)) {
        if (channel < -1 || channel > 254) return false;
        if (!open_set(chk, id, channel)) return false;
    }
    return true;
}

static void mob_erase(steins_checkpoint_t *chk, uint16_t first, uint16_t last)
{
    if (last < first) return;
    // inbox / sent：保序过滤
    uint16_t out = 0;
    for (uint16_t i = 0; i < chk->inbox_count; ++i) {
        const uint16_t id = chk->inbox[i];
        if (id < first || id > last) chk->inbox[out++] = id;
    }
    chk->inbox_count = out;
    out = 0;
    for (uint16_t i = 0; i < chk->sent_count; ++i) {
        const uint16_t id = chk->sent[i];
        if (id < first || id > last) chk->sent[out++] = id;
    }
    chk->sent_count = out;
    // unread 位图
    const uint16_t hi = last < STEINS_MAIL_IDS - 1 ? last : (uint16_t)(STEINS_MAIL_IDS - 1);
    for (uint32_t id = first; id <= hi; ++id) {
        (void)steins_checkpoint_set_unread(chk, (uint16_t)id, false);
    }
    // open 字典
    uint8_t o = 0;
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        const uint16_t id = chk->open_id[i];
        if (id < first || id > last) {
            chk->open_id[o] = chk->open_id[i];
            chk->open_val[o] = chk->open_val[i];
            o++;
        }
    }
    chk->open_count = o;
}

// ---------------------------------------------------------------- read / reply 位
static void mob_read_bit(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t id, bool value)
{
    (void)m;
    (void)steins_checkpoint_set_read(chk, id, false);
    (void)steins_checkpoint_set_unread(chk, id, false);
    if (value) {
        (void)steins_checkpoint_set_read(chk, id, true);
    } else if (id_in(chk->inbox, chk->inbox_count, id) &&
               (mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_TEXT) ||
                mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_INCOMING))) {
        (void)steins_checkpoint_set_unread(chk, id, true);
    }
}

static void mob_reply_bit(steins_checkpoint_t *chk, uint16_t id, bool value)
{
    (void)steins_checkpoint_set_replied(chk, id, false);
    if (value) (void)steins_checkpoint_set_replied(chk, id, true);
}

// ---------------------------------------------------------------- 事件应用
typedef struct {
    bool used[5];
} dispatched_t;

// 返回 false = 该行处理失败（容量/内容缺失），checkpoint 可能已部分改动；
// sync 会在失败时整体拒绝并把游标留在原处（见 steins_mobile_sync 的注释）。
static bool mob_apply_event(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t node,
                            const steins_mobile_row_t *row, uint8_t ev_index,
                            const steins_mobile_event_t *ev, dispatched_t *dispatched)
{
    switch (ev->kind) {
    case STEINS_MOB_EV_SEED:
        // 源 syncMobile 不处理 seed（只有 migrateMail 用）；C 侧 mailLifeVersion 恒为 1。
        return true;
    case STEINS_MOB_EV_RECEIVE: {
        if (ev->argc < 4) return false;
        const uint16_t id = (uint16_t)ev->arg[0];
        const int32_t channel = ev->arg[1];
        const bool open = ev->arg[2] != 0;
        const uint8_t owner = (uint8_t)ev->arg[3];
        if (channel > 0) {
            if (channel < 5 && dispatched->used[channel]) return true;   // 同行的 dispatch 已收
        }
        return mob_receive(m, chk, id, channel, open, owner);
    }
    case STEINS_MOB_EV_ERASE:
        if (ev->argc < 2) return false;
        mob_erase(chk, (uint16_t)ev->arg[0], (uint16_t)ev->arg[1]);
        return true;
    case STEINS_MOB_EV_READ_BIT:
        if (ev->argc < 2) return false;
        mob_read_bit(m, chk, (uint16_t)ev->arg[0], ev->arg[1] != 0);
        return true;
    case STEINS_MOB_EV_REPLY_BIT:
        if (ev->argc < 2) return false;
        mob_reply_bit(chk, (uint16_t)ev->arg[0], ev->arg[1] != 0);
        return true;
    case STEINS_MOB_EV_EXPIRE:
        chk->open_count = 0;     // 源：m.open = {}
        return true;
    case STEINS_MOB_EV_QUEUE:
        if (ev->argc < 2) return false;
        if (ev->arg[0] < 0 || ev->arg[0] >= 5) return false;
        chk->channels[ev->arg[0]] = (uint16_t)ev->arg[1];
        return true;
    case STEINS_MOB_EV_RULE:
        // 源 mobile.js 里只有三条硬编码规则。
        if (ev->argc < 1) return false;
        switch (ev->arg[0]) {
        case 1627:
            if (steins_checkpoint_read(chk, 404)) chk->channels[1] = 405;
            break;
        case 1630:
            if (steins_checkpoint_read(chk, 451) || steins_checkpoint_read(chk, 452)) {
                chk->channels[1] = 453;
            }
            break;
        case 1635:
            if (steins_checkpoint_read(chk, 464)) chk->channels[1] = 467;
            break;
        default:
            break;   // 源对未知 rule 无动作
        }
        return true;
    case STEINS_MOB_EV_DISPATCH: {
        if (ev->argc < 1) return false;
        const int32_t channel = ev->arg[0];
        if (channel < 0 || channel >= 5) return false;
        dispatched->used[channel] = true;
        const uint16_t id = chk->channels[channel];
        if (!id_ok(id)) return true;                      // 通道空（999 或越界）
        if (mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_INCOMING)) {
            return mob_receive(m, chk, id, channel, true, 0);
        }
        return true;
    }
    case STEINS_MOB_EV_CALL:
        // 源 argc=5：site / owner / kind / 有序 items / waiting
        if (ev->argc < 5) return false;
        if (ev->arg[0] < 0 || ev->arg[0] > 0xFFFF) return false;
        if (ev->arg[1] < 0 || ev->arg[1] > 255) return false;
        if (!call_kind_valid((uint8_t)ev->arg[2])) return false;
        chk->call_present = 1;
        chk->call_site = (uint16_t)ev->arg[0];
        chk->call_owner = (uint8_t)ev->arg[1];
        chk->call_kind = (uint8_t)ev->arg[2];
        chk->call_waiting = ev->arg[4] != 0 ? 1u : 0u;
        chk->call_node = node;
        chk->call_at = row->cursor;
        return true;
    case STEINS_MOB_EV_PRESENTATION: {
        if (ev->argc < 3) return false;
        if (!id_ok((uint16_t)ev->arg[0])) return false;
        if (!pres_kind_valid((uint8_t)ev->arg[1])) return false;
        if (ev->arg[2] < 0 || ev->arg[2] > 255) return false;
        if (chk->pres_count >= STEINS_CHK_MAX_PRES) return false;
        // 源 key = chapter:row:id:kind:owner；pending 里存的是 **uint8 key 编号**（0xFF = 无 key）。
        // 校验顺序（越界一律 false 且不改动 checkpoint）：
        //   1) 内容层给出的 raw key：0xFFFF = 该事件无 key（允许，降级）
        //   2) 有 key：必须 < PRES 真实条数（不是常量）且 < STEINS_CHK_MAX_PRES
        uint8_t key = 0xFFu;
        if (m->content->presentation_key) {
            const uint16_t raw = m->content->presentation_key(m->ctx, chk->node, row, ev_index);
            if (raw != 0xFFFFu) {
                // key 的有效范围 = min(源 key 空间 56, 真实 PRES 条数)。**不是**队列容量 64。
                uint16_t key_limit = STEINS_CHK_MAX_PRES_KEYS;
                if (m->content->presentation_key_count) {
                    const uint16_t dir_count = m->content->presentation_key_count(m->ctx);
                    if (dir_count == 0) return false;      // 声称有 key 但没有目录 → 内容不一致
                    if (dir_count < key_limit) key_limit = dir_count;
                }
                if (raw >= key_limit) return false;        // 不存在/越界的 key：拒绝，不改状态
                if (chk->pres_read_bits[raw >> 3] & (uint8_t)(1u << (raw & 7u))) return true;
                for (uint8_t i = 0; i < chk->pres_count; ++i) {
                    if (chk->pres_key[i] == (uint8_t)raw) return true;
                }
                key = (uint8_t)raw;
            }
        }
        if (key == 0xFFu) {
            // 无 key 目录/无 key：源用 key 去重，这里只能退化为 (id,kind)（文档已标注降级）。
            for (uint8_t i = 0; i < chk->pres_count; ++i) {
                if (chk->pres_id[i] == (uint16_t)ev->arg[0] &&
                    chk->pres_kind[i] == (uint8_t)ev->arg[1]) {
                    return true;
                }
            }
        }
        chk->pres_id[chk->pres_count] = (uint16_t)ev->arg[0];
        chk->pres_kind[chk->pres_count] = (uint8_t)ev->arg[1];
        chk->pres_owner[chk->pres_count] = (uint8_t)ev->arg[2];
        chk->pres_key[chk->pres_count] = key;
        chk->pres_count++;
        return true;
    }
    default:
        return false;
    }
}

bool steins_mobile_sync(steins_mobile_t *m, steins_checkpoint_t *chk)
{
    if (!m || !chk) return false;
    if (!m->events_bound || m->events_node != chk->node) return true;   // 源：mobileNode!==s.chapter
    int64_t from = -1;   // 源：m.lastAt 未设置时是 -1
    if (chk->last_node == chk->node && chk->last_at != 0xFFFFFFFFu) from = (int64_t)chk->last_at;
    const uint16_t rows = m->content->row_count ? m->content->row_count(m->ctx, chk->node) : 0;
    for (uint16_t i = 0; i < rows; ++i) {
        steins_mobile_row_t row;
        if (!m->content->row || !m->content->row(m->ctx, chk->node, i, &row)) return false;
        const int64_t row_cursor = (row.cursor == 0xFFFFFFFFu) ? -1 : (int64_t)row.cursor;
        if (row_cursor <= from || row_cursor > (int64_t)chk->cursor) continue;
        dispatched_t dispatched;
        memset(&dispatched, 0, sizeof(dispatched));
        for (uint8_t k = 0; k < row.count; ++k) {
            steins_mobile_event_t ev;
            if (!m->content->event || !m->content->event(m->ctx, chk->node, &row, k, &ev)) {
                return false;
            }
            if (!mob_apply_event(m, chk, chk->node, &row, k, &ev, &dispatched)) return false;
        }
    }
    chk->last_node = chk->node;
    chk->last_at = chk->cursor;
    return true;
}

// ---------------------------------------------------------------- mark read / reply
bool steins_mobile_can_reply(steins_mobile_t *m, const steins_checkpoint_t *chk, uint16_t id)
{
    if (!m || !chk || !id_ok(id)) return false;
    const bool window = (chk->deferred_present && chk->deferred_id == id) ||
                        (open_lookup(chk, id) != -1);
    if (!window) return false;
    if (!mail_succ_has_type(m, id, STEINS_MOB_MAIL_TYPE_REPLY)) return false;
    return !steins_checkpoint_replied(chk, id);
}

bool steins_mobile_mark_read(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t id,
                            steins_mobile_pos_t *land)
{
    if (!m || !chk || !id_ok(id)) return false;
    (void)steins_checkpoint_set_unread(chk, id, false);
    (void)steins_checkpoint_set_read(chk, id, true);
    if (!mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_TEXT) &&
        !mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_INCOMING) &&
        !mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_SENT) &&
        !mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_REPLY)) {
        return true;   // 源：if(!row) return;
    }
    uint16_t target_node = 0xFFFFu;
    bool moved = false;
    if (chk->deferred_present && chk->deferred_id == id) {
        chk->deferred_read = 1;
        steins_mobile_window_t w;
        memset(&w, 0, sizeof(w));
        if (!m->content->window || !m->content->window(m->ctx, chk->deferred_root, &w)) return false;
        target_node = w.read;
        moved = true;
        if (w.option_count == 0) {
            chk->deferred_present = 0;
            (void)open_remove(chk, id);
        }
    }
    const int16_t channel = open_lookup(chk, id);
    if (channel != -1 && !mail_succ_has_type(m, id, STEINS_MOB_MAIL_TYPE_REPLY)) {
        const uint16_t follow = mail_succ_first_type2(m, id);
        if (follow != 0xFFFFu && channel > 0 && channel < 5) chk->channels[channel] = follow;
    }
    if (moved && target_node != 0xFFFFu) { chk->node = target_node; }
    if (land) {
        land->node = chk->node;
        land->cursor = chk->cursor;
    }
    return true;
}

bool steins_mobile_send_reply(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t parent,
                              uint16_t id, steins_mobile_pos_t *land)
{
    if (!m || !chk || !id_ok(parent) || !id_ok(id)) return false;
    if (!steins_mobile_can_reply(m, chk, parent)) return false;      // 源：canReply 失败 → throw
    uint16_t succ[16];
    const uint8_t n = m->content->mail_succ
                          ? m->content->mail_succ(m->ctx, parent, succ, (uint8_t)(sizeof(succ) / sizeof(succ[0])))
                          : 0;
    bool is_succ = false;
    for (uint8_t i = 0; i < n; ++i) if (succ[i] == id) is_succ = true;
    if (!is_succ) return false;                                      // 源：不在后继里 → throw
    // 预先校验容量（避免"写了一半失败"）。
    const bool need_sent = !id_in(chk->sent, chk->sent_count, id);
    if (need_sent && chk->sent_count >= STEINS_CHK_MAX_SENT) return false;
    const bool need_replied = !steins_checkpoint_replied(chk, parent);
    (void)need_replied;
    const uint8_t owner = owner_get(chk, parent);
    if (!owner_set(chk, id, owner)) return false;

    if (!steins_mobile_mark_read(m, chk, parent, NULL)) return false;  // 源：先 markMailRead(parent)
    if (need_sent) chk->sent[chk->sent_count++] = id;
    (void)steins_checkpoint_set_replied(chk, parent, true);           // 源：m.replied.push(parent)
    const int16_t channel = open_lookup(chk, parent);
    if (channel > 0 && channel < 5) {
        const uint16_t follow = mail_succ_first_type2(m, id);
        chk->channels[channel] = (follow == 0xFFFFu) ? (uint16_t)STEINS_MOB_CHANNEL_EMPTY : follow;
    }
    (void)open_remove(chk, parent);
    if (land) {
        land->node = chk->node;
        land->cursor = chk->cursor;
    }
    return true;
}

// ---------------------------------------------------------------- 延迟回复窗口
bool steins_mobile_defer_begin(steins_mobile_t *m, steins_checkpoint_t *chk)
{
    if (!m || !chk || !m->content->window) return false;
    if (chk->deferred_present) return false;
    steins_mobile_window_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->window(m->ctx, chk->node, &w)) return false;
    uint32_t count = 0;
    if (!m->content->node_count || !m->content->node_count(m->ctx, chk->node, &count)) return false;
    if (chk->cursor < count) return false;                            // 源：未到节点末尾
    if (!steins_mobile_sync(m, chk)) return false;
    if (!mob_receive(m, chk, w.id, 0, true, owner_get(chk, w.id))) return false;
    chk->deferred_present = 1;
    chk->deferred_root = chk->node;
    chk->deferred_id = w.id;
    chk->deferred_read = steins_checkpoint_read(chk, w.id) ? 1u : 0u;
    chk->node = chk->deferred_read ? w.read : w.skip;
    chk->cursor = 0;
    chk->last_node = chk->node;
    chk->last_at = 0xFFFFFFFFu;
    return true;
}

bool steins_mobile_deferred_limit(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                  uint16_t *limit)
{
    if (!m || !chk || !chk->deferred_present || !m->content->window) return false;
    steins_mobile_window_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->window(m->ctx, chk->deferred_root, &w)) return false;
    if (limit) *limit = w.limit;
    return true;
}

bool steins_mobile_deferred_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                     steins_mobile_frame_t *out)
{
    if (!m || !chk || !out) return false;
    if (!chk->deferred_present || !m->content->window) return false;
    steins_mobile_window_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->window(m->ctx, chk->deferred_root, &w)) return false;
    if (chk->cursor < w.limit) return false;                          // 源：还没到边界
    uint8_t count = w.option_count;
    if (count == 0) count = 1;                                        // 源合成「阅读邮件」
    count = (uint8_t)(count + 1);                                     // + 「不回复，继续剧情」
    memset(out, 0, sizeof(*out));
    out->type = STEINS_MOB_FRAME_DEFERRED;
    out->node = chk->node;
    out->choice_id = chk->deferred_id;
    out->item_count = count;
    return true;
}

uint8_t steins_mobile_deferred_options(const steins_mobile_t *m, const steins_checkpoint_t *chk,
                                       steins_mobile_boundary_item_t *out, uint8_t cap)
{
    if (!m || !chk || !out || !chk->deferred_present || !m->content->window) return 0;
    steins_mobile_window_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->window(m->ctx, chk->deferred_root, &w)) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < w.option_count && n < cap; ++i) {
        steins_mobile_window_option_t opt;
        memset(&opt, 0, sizeof(opt));
        if (!m->content->window_option || !m->content->window_option(m->ctx, chk->deferred_root, i, &opt)) {
            break;
        }
        out[n].action = opt.action;
        out[n].id = opt.id;
        out[n].to = opt.to;
        n++;
    }
    if (w.option_count == 0 && n < cap) {                             // 源合成 readDeferred
        out[n].action = STEINS_MOB_ACT_READ_DEFERRED;
        out[n].id = chk->deferred_id;
        out[n].to = w.read;
        n++;
    }
    if (n < cap) {                                                    // 源追加 expireDeferred
        out[n].action = STEINS_MOB_ACT_EXPIRE_DEFERRED;
        out[n].id = chk->deferred_id;
        out[n].to = chk->deferred_read ? w.read : w.skip;
        n++;
    }
    return n;
}

bool steins_mobile_answer_deferred(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t action,
                                   uint16_t reply_id, steins_mobile_pos_t *land)
{
    if (!m || !chk || !chk->deferred_present) return false;
    steins_mobile_window_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->window || !m->content->window(m->ctx, chk->deferred_root, &w)) return false;
    const uint32_t cursor = chk->cursor;        // 源：const cursor=s.cursor —— 不归零
    uint16_t target;
    if (action == STEINS_MOB_ACT_REPLY) {
        uint16_t to = 0xFFFFu;
        for (uint8_t i = 0; i < w.option_count; ++i) {
            steins_mobile_window_option_t opt;
            memset(&opt, 0, sizeof(opt));
            if (!m->content->window_option || !m->content->window_option(m->ctx, chk->deferred_root, i, &opt)) {
                return false;
            }
            if (opt.id == reply_id) to = opt.to;
        }
        if (to == 0xFFFFu) return false;                                  // 源：无效回复
        if (!steins_mobile_send_reply(m, chk, chk->deferred_id, reply_id, NULL)) return false;
        target = to;
    } else if (action == STEINS_MOB_ACT_READ_DEFERRED) {
        if (!steins_mobile_mark_read(m, chk, chk->deferred_id, NULL)) return false;
        target = w.read;
    } else if (action == STEINS_MOB_ACT_EXPIRE_DEFERRED) {
        target = chk->deferred_read ? w.read : w.skip;
    } else {
        return false;
    }
    if (cursor >= w.limit) chk->open_count = 0;         // 源：if(cursor>=w.limit) m.open={}
    chk->node = target;
    chk->cursor = cursor;                                // **落点 = to @ 当前游标（不是 0）**
    chk->deferred_present = 0;
    (void)open_remove(chk, chk->deferred_id);
    chk->last_node = target;
    chk->last_at = cursor - 1u;                           // 源：m.lastAt=cursor-1
    if (land) {
        land->node = target;
        land->cursor = cursor;
    }
    return true;
}

// ---------------------------------------------------------------- 关键电话
bool steins_mobile_defer_key_phone(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t owner)
{
    if (!m || !chk || !m->content->keyphone) return false;
    steins_mobile_keyphone_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->keyphone(m->ctx, chk->node, &w)) return false;
    chk->keyphone_present = 1;
    chk->keyphone_root = chk->node;
    chk->keyphone_owner = owner;
    chk->node = w.skip;
    chk->cursor = 0;
    return true;
}

bool steins_mobile_keyphone_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                     steins_mobile_frame_t *out)
{
    if (!m || !chk || !out || !chk->keyphone_present || !m->content->keyphone) return false;
    steins_mobile_keyphone_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->keyphone(m->ctx, chk->keyphone_root, &w)) return false;
    if (chk->cursor < w.limit) return false;
    uint16_t choice_id = 0xFFFFu;
    if (m->content->node_choice_id) {
        (void)m->content->node_choice_id(m->ctx, chk->keyphone_root, &choice_id);
    }
    steins_mobile_choice_item_t items[8];
    const uint8_t n = m->content->choice_items
                          ? m->content->choice_items(m->ctx, chk->keyphone_root, items, (uint8_t)(sizeof(items) / sizeof(items[0])))
                          : 0;
    if (n == 0) return false;
    memset(out, 0, sizeof(*out));
    out->type = STEINS_MOB_FRAME_KEYPHONE;
    out->node = chk->keyphone_root;
    out->choice_id = choice_id;
    out->item_count = n;
    return true;
}

uint8_t steins_mobile_keyphone_options(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                       steins_mobile_boundary_item_t *out, uint8_t cap)
{
    if (!m || !chk || !out || !chk->keyphone_present || !m->content->choice_items) return 0;
    steins_mobile_choice_item_t items[8];
    const uint8_t n = m->content->choice_items(m->ctx, chk->keyphone_root, items,
                                               (uint8_t)(sizeof(items) / sizeof(items[0])));
    uint8_t k = 0;
    for (uint8_t i = 0; i < n && k < cap; ++i) {
        // 源 keyPhoneActions 只保留 call/send；边界时全部选项都展示（text 改写由 UI 负责）。
        out[k].action = items[i].action;
        out[k].id = items[i].id;
        out[k].to = items[i].to;
        k++;
    }
    return k;
}

bool steins_mobile_answer_key_phone(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t index,
                                    steins_mobile_pos_t *land)
{
    if (!m || !chk || !chk->keyphone_present) return false;
    steins_mobile_keyphone_t w;
    memset(&w, 0, sizeof(w));
    if (!m->content->keyphone || !m->content->keyphone(m->ctx, chk->keyphone_root, &w)) return false;
    steins_mobile_choice_item_t items[8];
    const uint8_t n = m->content->choice_items
                          ? m->content->choice_items(m->ctx, chk->keyphone_root, items,
                                                     (uint8_t)(sizeof(items) / sizeof(items[0])))
                          : 0;
    if (index >= n) return false;                                     // 源：无效选项 → throw
    const steins_mobile_choice_item_t item = items[index];
    const bool is_call = (item.action == STEINS_MOB_ACT_CALL);        // noCall/noSend 也走这里？
    const bool is_send = (item.action == STEINS_MOB_ACT_SEND);
    chk->keyphone_present = 0;
    if (is_call || is_send) {
        const bool need_sent = (w.sent != 0xFFFFu) && !id_in(chk->sent, chk->sent_count, w.sent);
        if (need_sent && chk->sent_count >= STEINS_CHK_MAX_SENT) return false;
        chk->node = w.go;
        chk->cursor = w.after;
        if (w.sent != 0xFFFFu) {
            if (need_sent) chk->sent[chk->sent_count++] = w.sent;
            if (!owner_set(chk, w.sent, chk->keyphone_owner)) return false;
        }
    }
    if (land) {
        land->node = chk->node;
        land->cursor = chk->cursor;
    }
    return true;
}

// ---------------------------------------------------------------- 来电
bool steins_mobile_call_current(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                uint16_t *site, uint8_t *item_count)
{
    if (!m || !chk || !chk->call_present) return false;
    if (chk->node != chk->call_node) return false;
    steins_mobile_callsite_t spec;
    memset(&spec, 0, sizeof(spec));
    if (!m->content->call_site || !m->content->call_site(m->ctx, chk->call_site, &spec)) return false;
    if (chk->cursor < chk->call_at) return false;
    if (chk->cursor >= chk->call_at + spec.limit) return false;
    if (site) *site = chk->call_site;
    if (item_count) *item_count = spec.item_count;
    return true;
}

bool steins_mobile_take_call(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t index,
                             steins_mobile_pos_t *land)
{
    uint16_t site = 0;
    uint8_t count = 0;
    if (!steins_mobile_call_current(m, chk, &site, &count)) return false;   // 源：throw
    if (index >= count) return false;
    steins_mobile_callitem_t item;
    memset(&item, 0, sizeof(item));
    if (!m->content->call_item || !m->content->call_item(m->ctx, site, index, &item)) return false;
    chk->call_present = 0;
    if (item.clip != 0xFFFFu) {
        chk->call_return_present = 1;
        chk->call_return_node = chk->node;
        chk->call_return_cursor = chk->call_at + item.join;
        chk->cursor = chk->cursor - chk->call_at;                 // 源：s.cursor -= c.at
        chk->node = item.clip;                                    // clip 节点号
    }
    if (land) {
        land->node = chk->node;
        land->cursor = chk->cursor;
    }
    return true;
}

bool steins_mobile_return_call(steins_mobile_t *m, steins_checkpoint_t *chk, steins_mobile_pos_t *land)
{
    if (!m || !chk || !chk->call_return_present) return false;
    bool is_clip = false;
    uint32_t frames = 0;
    if (!m->content->is_clip_node || !m->content->is_clip_node(m->ctx, chk->node)) return false;
    is_clip = true;
    if (!m->content->clip_frames || !m->content->clip_frames(m->ctx, chk->node, &frames)) return false;
    if (is_clip && chk->cursor >= frames) {
        const uint16_t node = chk->call_return_node;
        const uint32_t cursor = chk->call_return_cursor;
        chk->call_return_present = 0;
        chk->node = node;
        chk->cursor = cursor;
        if (land) {
            land->node = node;
            land->cursor = cursor;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 手机屏演示
bool steins_mobile_plot_presentation(const steins_checkpoint_t *chk, uint16_t *id, uint8_t *kind,
                                     uint8_t *owner)
{
    if (!chk || chk->pres_count == 0) return false;
    if (id) *id = chk->pres_id[0];
    if (kind) *kind = chk->pres_kind[0];
    if (owner) *owner = chk->pres_owner[0];
    return true;
}

bool steins_mobile_finish_presentation(steins_mobile_t *m, steins_checkpoint_t *chk)
{
    if (!m || !chk || chk->pres_count == 0) return false;
    const uint16_t id = chk->pres_id[0];
    const uint8_t kind = chk->pres_kind[0];
    const uint8_t owner = chk->pres_owner[0];
    if (!owner_set(chk, id, owner)) return false;
    bool need_sent = false;
    bool need_receive = false;
    if (kind == STEINS_MOB_PRES_SEND) {
        need_sent = !id_in(chk->sent, chk->sent_count, id);
        if (need_sent && chk->sent_count >= STEINS_CHK_MAX_SENT) return false;
    } else if (kind != STEINS_MOB_PRES_DRAFT) {
        steins_mobile_mail_t meta;
        if (!mail_meta(m, id, &meta)) return false;
        if (meta.type != STEINS_MOB_MAIL_TYPE_SENT && meta.type != STEINS_MOB_MAIL_TYPE_REPLY) {
            need_receive = !id_in(chk->inbox, chk->inbox_count, id);
            if (need_receive && chk->inbox_count >= STEINS_CHK_MAX_INBOX) return false;
        }
    }
    // 源：presentationRead.push(key)。
    //   key == 0xFF → 旧档/无 key（本次不记位，仍正常出队并执行副作用）；
    //   key 有效 → 置位。越界（>= STEINS_CHK_MAX_PRES）说明 checkpoint 被破坏：拒绝且不改动。
    {
        const uint8_t key = chk->pres_key[0];
        if (key != STEINS_MOB_PRES_KEY_NONE) {
            if (key >= STEINS_CHK_MAX_PRES_KEYS) return false;
            chk->pres_read_bits[key >> 3] |= (uint8_t)(1u << (key & 7u));
        }
    }
    // 出队（源 shift）
    for (uint8_t i = 1; i < chk->pres_count; ++i) {
        chk->pres_id[i - 1] = chk->pres_id[i];
        chk->pres_kind[i - 1] = chk->pres_kind[i];
        chk->pres_owner[i - 1] = chk->pres_owner[i];
        chk->pres_key[i - 1] = chk->pres_key[i];
    }
    chk->pres_count--;
    if (need_sent) chk->sent[chk->sent_count++] = id;
    if (kind == STEINS_MOB_PRES_SEND) {
        if (chk->call_present && chk->call_kind == STEINS_MOB_CALL_DMAIL) chk->call_present = 0;
    } else if (kind != STEINS_MOB_PRES_DRAFT) {
        if (need_receive) {
            if (!mob_receive(m, chk, id, -1, false, owner)) return false;
        }
        if (!steins_mobile_mark_read(m, chk, id, NULL)) return false;
    }
    return true;
}

bool steins_mobile_next_presentation_boundary(steins_mobile_t *m, const steins_checkpoint_t *chk,
                                              uint32_t to, uint32_t *out)
{
    if (!m || !chk) return false;
    if (!m->events_bound || m->events_node != chk->node) {
        if (out) *out = to;                                            // 源：mobileNode!==chapter → to
        return true;
    }
    const uint16_t rows = m->content->row_count ? m->content->row_count(m->ctx, chk->node) : 0;
    for (uint16_t i = 0; i < rows; ++i) {
        steins_mobile_row_t row;
        if (!m->content->row || !m->content->row(m->ctx, chk->node, i, &row)) return false;
        if (row.cursor <= chk->cursor || row.cursor > to) continue;
        for (uint8_t k = 0; k < row.count; ++k) {
            steins_mobile_event_t ev;
            if (!m->content->event || !m->content->event(m->ctx, chk->node, &row, k, &ev)) return false;
            if (ev.kind == STEINS_MOB_EV_PRESENTATION) {
                if (out) *out = row.cursor;
                return true;
            }
        }
    }
    if (out) *out = to;
    return true;
}

// ---------------------------------------------------------------- migrateMail
// 源 migrateMail(s)：只有当 s.mailLifeVersion !== 1 且事件里有 row[0]===-1 时才重建 allowed。
//   allowed = seed 有序 id 流 ∪ receive(id) 的后继闭包 − erase 区间
// 然后把 inbox/sent/unread/open 里不在 allowed 的项过滤掉，最后置 version=1。
bool steins_mobile_migrate(steins_mobile_t *m, steins_checkpoint_t *chk, bool legacy_archive)
{
    if (!m || !chk) return false;
    if (!legacy_archive) return true;                       // 新档：mailLifeVersion 已是 1
    if (!m->events_bound || !m->content->seed_ids) return false;
    static uint8_t allowed[(STEINS_MAIL_IDS + 7) / 8];
    memset(allowed, 0, sizeof(allowed));
    const uint16_t rows = m->content->row_count ? m->content->row_count(m->ctx, m->events_node) : 0;
    for (uint16_t i = 0; i < rows; ++i) {
        steins_mobile_row_t row;
        if (!m->content->row || !m->content->row(m->ctx, m->events_node, i, &row)) return false;
        const int64_t row_cursor = (row.cursor == 0xFFFFFFFFu) ? -1 : (int64_t)row.cursor;
        if (row_cursor > (int64_t)chk->cursor) break;       // 源：if(row[0]>s.cursor) break;
        for (uint8_t k = 0; k < row.count; ++k) {
            steins_mobile_event_t ev;
            if (!m->content->event || !m->content->event(m->ctx, m->events_node, &row, k, &ev)) {
                return false;
            }
            if (ev.kind == STEINS_MOB_EV_SEED) {
                // 单行最多 282 项：按 offset 分批读，任何一批都不能被静默截断。
                uint16_t ids[64];
                uint16_t offset = 0;
                const uint16_t desc = (uint16_t)ev.arg[0];
                for (;;) {
                    const uint16_t n = m->content->seed_ids(m->ctx, desc, offset,
                                                            ids, (uint16_t)(sizeof(ids) / sizeof(ids[0])));
                    for (uint16_t x = 0; x < n; ++x) {
                        if (id_ok(ids[x])) allowed[ids[x] >> 3] |= (uint8_t)(1u << (ids[x] & 7u));
                    }
                    offset = (uint16_t)(offset + n);
                    if (n < (uint16_t)(sizeof(ids) / sizeof(ids[0]))) break;
                }
            } else if (ev.kind == STEINS_MOB_EV_RECEIVE && ev.argc >= 1) {
                // 源 allowFamily 递归后继闭包
                uint16_t stack[64];
                uint8_t sp = 0;
                if (id_ok((uint16_t)ev.arg[0])) stack[sp++] = (uint16_t)ev.arg[0];
                while (sp > 0) {
                    const uint16_t id = stack[--sp];
                    if (allowed[id >> 3] & (uint8_t)(1u << (id & 7u))) continue;
                    allowed[id >> 3] |= (uint8_t)(1u << (id & 7u));
                    uint16_t succ[64];
                    const uint8_t n = m->content->mail_succ
                                          ? m->content->mail_succ(m->ctx, id, succ,
                                                                  (uint8_t)(sizeof(succ) / sizeof(succ[0])))
                                          : 0;
                    for (uint8_t x = 0; x < n && sp < 64; ++x) stack[sp++] = succ[x];
                }
            } else if (ev.kind == STEINS_MOB_EV_ERASE && ev.argc >= 2) {
                const uint16_t first = (uint16_t)ev.arg[0];
                const uint16_t last = (uint16_t)ev.arg[1];
                const uint16_t hi = last < STEINS_MAIL_IDS - 1 ? last : (uint16_t)(STEINS_MAIL_IDS - 1);
                for (uint32_t id = first; id <= hi; ++id) {
                    allowed[id >> 3] &= (uint8_t)~(1u << (id & 7u));
                }
            }
        }
    }
    // 过滤 inbox / sent / unread / open
    uint16_t out = 0;
    for (uint16_t i = 0; i < chk->inbox_count; ++i) {
        const uint16_t id = chk->inbox[i];
        if (allowed[id >> 3] & (uint8_t)(1u << (id & 7u))) chk->inbox[out++] = id;
    }
    chk->inbox_count = out;
    out = 0;
    for (uint16_t i = 0; i < chk->sent_count; ++i) {
        const uint16_t id = chk->sent[i];
        if (allowed[id >> 3] & (uint8_t)(1u << (id & 7u))) chk->sent[out++] = id;
    }
    chk->sent_count = out;
    for (uint16_t id = 0; id < STEINS_MAIL_IDS; ++id) {
        if (!(allowed[id >> 3] & (uint8_t)(1u << (id & 7u)))) {
            (void)steins_checkpoint_set_unread(chk, id, false);
            (void)steins_checkpoint_set_replied(chk, id, false);
        }
    }
    out = 0;
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        const uint16_t id = chk->open_id[i];
        if (allowed[id >> 3] & (uint8_t)(1u << (id & 7u))) {
            chk->open_id[out] = chk->open_id[i];
            chk->open_val[out] = chk->open_val[i];
            out++;
        }
    }
    chk->open_count = out;
    return true;
}

// ---------------------------------------------------------------- locate / choose
bool steins_mobile_locate(steins_mobile_t *m, steins_checkpoint_t *chk, steins_mobile_frame_t *out)
{
    if (!m || !chk || !out) return false;
    memset(out, 0, sizeof(*out));
    // 源 locate 顺序：ensureMobile → returnCall → syncMobile → beginDeferred → 边界 → 图节点
    steins_mobile_pos_t land;
    if (steins_mobile_return_call(m, chk, &land)) {
        /* 回到通话后的游标，继续按新节点判定 */
    }
    if (!steins_mobile_sync(m, chk)) {
        out->type = STEINS_MOB_FRAME_ERROR;
        return false;
    }
    if (m->dynamic_boundaries) {
        // 源语义：进入节点头自动 beginDeferred（守卫：已有 deferred/未到节点末尾/无窗口）
        if (!chk->deferred_present) (void)steins_mobile_defer_begin(m, chk);
        // 边界优先于节点自身 choice（源：keyPhoneBoundary || deferredBoundary）
        if (chk->keyphone_present) {
            if (steins_mobile_keyphone_boundary(m, chk, out)) return true;
        }
        if (chk->deferred_present) {
            if (steins_mobile_deferred_boundary(m, chk, out)) return true;
        }
    }
    uint32_t count = 0;
    if (!m->content->node_count || !m->content->node_count(m->ctx, chk->node, &count)) {
        out->type = STEINS_MOB_FRAME_ERROR;
        return false;
    }
    if (chk->cursor >= count) {
        uint16_t choice_id = 0xFFFFu;
        if (m->content->node_choice_id && m->content->node_choice_id(m->ctx, chk->node, &choice_id)) {
            steins_mobile_choice_item_t items[8];
            const uint8_t n = m->content->choice_items
                                  ? m->content->choice_items(m->ctx, chk->node, items, (uint8_t)(sizeof(items) / sizeof(items[0])))
                                  : 0;
            out->type = STEINS_MOB_FRAME_CHOICE;
            out->node = chk->node;
            out->choice_id = choice_id;
            out->item_count = n;
            return true;
        }
        out->type = STEINS_MOB_FRAME_END;                              // 源：无 choice/ending → 结束
        out->node = chk->node;
        return true;
    }
    out->type = STEINS_MOB_FRAME_TEXT;
    out->node = chk->node;
    return true;
}

static bool mob_incoming(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t id, uint8_t owner)
{
    (void)m;
    if (!id_ok(id)) return true;
    if (!id_in(chk->inbox, chk->inbox_count, id)) {
        if (chk->inbox_count >= STEINS_CHK_MAX_INBOX) return false;
        chk->inbox[chk->inbox_count++] = id;
    }
    return owner_set(chk, id, owner);   // 源：s.mailOwners[id]=phoneContext(s).owner
}

bool steins_mobile_choose(steins_mobile_t *m, steins_checkpoint_t *chk, uint16_t node,
                          uint8_t item_index, uint8_t owner, steins_mobile_pos_t *land)
{
    if (!m || !chk || !m->content->choice_items) return false;
    steins_mobile_choice_item_t items[8];
    const uint8_t n = m->content->choice_items(m->ctx, node, items,
                                               (uint8_t)(sizeof(items) / sizeof(items[0])));
    if (item_index >= n) return false;
    const steins_mobile_choice_item_t item = items[item_index];
    // 边界拦截：源 choose 先看 keyPhone/deferred 标记。C 侧界面必须把边界选项
    // 交给 answer_deferred/answer_key_phone，这里拒绝误用。
    if (chk->keyphone_present || chk->deferred_present) return false;
    if (item.action == STEINS_MOB_ACT_NO_CALL || item.action == STEINS_MOB_ACT_NO_SEND) {
        if (steins_mobile_defer_key_phone(m, chk, owner)) {
            if (land) { land->node = chk->node; land->cursor = chk->cursor; }
            return true;
        }
        return false;
    }
    uint16_t choice_id = 0xFFFFu;
    // 源：`incoming(s, f.id)` 里 f.id 可以缺失（例如 n3 的 D-Mail 选择没有 id）→ 跳过。
    if (m->content->node_choice_id) (void)m->content->node_choice_id(m->ctx, node, &choice_id);
    if (choice_id != 0xFFFFu) {
        if (!mob_incoming(m, chk, choice_id, owner)) return false;
    }
    uint16_t sent_id = 0xFFFFu;
    if (item.action == STEINS_MOB_ACT_REPLY) {
        sent_id = item.id;
    } else if (item.action == STEINS_MOB_ACT_SEND) {
        sent_id = item.source;                                        // 源 phoneData.dmail[choice.source]
    }
    if (sent_id != 0xFFFFu && id_ok(sent_id)) {
        const bool need = !id_in(chk->sent, chk->sent_count, sent_id);
        if (need && chk->sent_count >= STEINS_CHK_MAX_SENT) return false;
        // owner 只在非零且新出现时占槽：先校验，避免 sent 写完才失败。
        if (owner != 0 && owner_get(chk, sent_id) == 0 &&
            chk->owner_count >= STEINS_CHK_MAX_OWNERS) {
            return false;
        }
        if (need) chk->sent[chk->sent_count++] = sent_id;
        if (!owner_set(chk, sent_id, owner)) return false;
    }
    chk->node = item.to;
    chk->cursor = 0;
    if (land) {
        land->node = chk->node;
        land->cursor = 0;
    }
    return true;
}

// ---------------------------------------------------------------- 查询
uint16_t steins_mobile_inbox_count(const steins_checkpoint_t *chk)
{
    return chk ? chk->inbox_count : 0;
}

bool steins_mobile_inbox_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id)
{
    if (!chk || index >= chk->inbox_count) return false;
    if (id) *id = chk->inbox[index];
    return true;
}

uint16_t steins_mobile_sent_count(const steins_checkpoint_t *chk)
{
    return chk ? chk->sent_count : 0;
}

bool steins_mobile_sent_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id)
{
    if (!chk || index >= chk->sent_count) return false;
    if (id) *id = chk->sent[index];
    return true;
}

uint16_t steins_mobile_unread_count(const steins_checkpoint_t *chk)
{
    uint16_t n = 0;
    if (!chk) return 0;
    for (uint16_t id = 0; id < STEINS_MAIL_IDS; ++id) {
        if (steins_checkpoint_unread(chk, id)) n++;
    }
    return n;
}

bool steins_mobile_unread_at(const steins_checkpoint_t *chk, uint16_t index, uint16_t *id)
{
    uint16_t n = 0;
    if (!chk) return false;
    for (uint16_t i = 0; i < STEINS_MAIL_IDS; ++i) {
        if (!steins_checkpoint_unread(chk, i)) continue;
        if (n == index) {
            if (id) *id = i;
            return true;
        }
        n++;
    }
    return false;
}

int16_t steins_mobile_open_channel(const steins_checkpoint_t *chk, uint16_t id)
{
    return chk ? open_lookup(chk, id) : -1;
}

uint16_t steins_mobile_channel(const steins_checkpoint_t *chk, uint8_t index)
{
    if (!chk || index >= 5) return STEINS_MOB_CHANNEL_EMPTY;
    return chk->channels[index];
}

bool steins_mobile_owner(const steins_checkpoint_t *chk, uint16_t id, uint8_t *owner)
{
    if (!chk || !id_ok(id)) return false;
    if (owner) *owner = owner_get(chk, id);
    return true;
}

uint16_t steins_mobile_last_node(const steins_checkpoint_t *chk)
{
    return chk ? chk->last_node : 0xFFFFu;
}

uint32_t steins_mobile_last_at(const steins_checkpoint_t *chk)
{
    return chk ? chk->last_at : 0xFFFFFFFFu;
}

uint16_t steins_mobile_pending_count(steins_mobile_t *m, const steins_checkpoint_t *chk, int owner)
{
    if (!m || !chk) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < chk->inbox_count; ++i) {
        const uint16_t id = chk->inbox[i];
        if (!steins_checkpoint_unread(chk, id)) continue;
        if (!mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_TEXT) &&
            !mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_INCOMING)) {
            continue;
        }
        if (owner >= 0 && (int)owner_get(chk, id) != owner) continue;
        n++;
    }
    return n;
}

uint16_t steins_mobile_mark_ordinary_read(steins_mobile_t *m, steins_checkpoint_t *chk, uint8_t owner)
{
    if (!m || !chk) return 0;
    uint16_t count = 0;
    // 源按 inbox 顺序遍历并跳过 mobileGraph.retainedMail（关键邮件）；C 侧由调用方通过
    // pending 语义区分，这里只处理"该 owner 的普通未读"。
    uint16_t snapshot[STEINS_CHK_MAX_INBOX];
    const uint16_t n = chk->inbox_count;
    if (n > STEINS_CHK_MAX_INBOX) return 0;
    memcpy(snapshot, chk->inbox, (size_t)n * sizeof(uint16_t));
    for (uint16_t i = 0; i < n; ++i) {
        const uint16_t id = snapshot[i];
        if (!steins_checkpoint_unread(chk, id)) continue;
        if (!mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_TEXT) &&
            !mail_has_type(m, id, STEINS_MOB_MAIL_TYPE_INCOMING)) {
            continue;
        }
        if (owner_get(chk, id) != owner) continue;
        count++;
        if (!steins_mobile_mark_read(m, chk, id, NULL)) return count;
    }
    return count;
}

// ---------------------------------------------------------------- 生命周期
void steins_mobile_seek(steins_checkpoint_t *chk, uint16_t node, uint32_t cursor)
{
    if (!chk) return;
    chk->node = node;
    chk->cursor = cursor;
    // 章节模式从游标 C 进入时，源用 mobile.presentationEntry={node,at:C} 跳过 row[0] < C 的
    // presentation 行；置 lastAt=C-1 后同步区间是 (C-1, C] —— **包含第 C 行**、排除更早的行，
    // 与源过滤等价（不是"空区间"）。
    chk->last_node = node;
    chk->last_at = (cursor == 0) ? 0xFFFFFFFFu : (cursor - 1u);
}

void steins_mobile_set_dynamic_boundaries(steins_mobile_t *m, bool enabled)
{
    if (m) m->dynamic_boundaries = enabled;
}

void steins_mobile_init(steins_mobile_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->events_node = 0xFFFFu;
}

bool steins_mobile_enter(steins_mobile_t *m, const steins_mobile_content_t *content, void *ctx,
                         uint16_t node)
{
    if (!m || !content) return false;
    m->content = content;
    m->ctx = ctx;
    m->events_node = node;
    m->events_bound = true;
    return true;
}
