// main/steins_mobile_pack.c —— pack v4 → 事件机内容层的翻译实现。
//
// 翻译规则（全部有源依据，见 build/mobile-integration/engine-api.md §4）：
//   * 事件行：MROW.cursor 是 **int32**（seed 行 = -1）；事件机用 uint32 表示，-1 → 0xFFFFFFFF。
//   * 事件参数：MEVT.arg 是 uint16；`receive` 的 channel=-1 在包里是 0xFFFF，**只在这一位**
//     转回 -1（其它参数保持无符号语义）。
//   * action：包的 ACTION_CODE = read/ignore/send/noSend/call/noCall/reply/dmail（0..7），
//     与事件机的 STEINS_MOB_ACT_* **不是同一套值**，必须显式映射（不能强转）。
//   * 通话：CALLSITE 表按"下标"使用；CALLITEM.clip 是 u8 片段下标 → 换成
//     稳定虚拟节点号 0x8000+clip；0xFF = 无 clip → 0xFFFF。
//   * 通话描述符（CALLDESC）与 CALLSITE 顺序不同，但事件机只消费 CALLSITE，故不使用 CALLDESC
//     的 site 映射（描述符仅用于 UI 的 items 视图）。
#include "steins_mobile_pack.h"

#include <string.h>

// ---------------------------------------------------------------- action 映射
// ---------------------------------------------------------------- 两套 action 编码（不可混用）
// 手机 typed 域（WINOPT / CALLITEM / CALLDITEM）：值的语义就是 STEINS_MOB_ACT_*，直接校验范围。
bool steins_mobile_pack_typed_action(uint8_t code, uint8_t *out_action)
{
    if (code > (uint8_t)STEINS_MOB_ACT_EXPIRE_DEFERRED) return false;
    if (out_action) *out_action = code;
    return true;
}

// 主图 legacy 域（CHOICE / CITEM）：read0 / ignore1 / send2 / noSend3 / call4 / noCall5 /
// reply6 / dmail7（tools/steins_pack.py 的 ACTIONS 顺序）→ 事件机枚举。
bool steins_mobile_pack_legacy_action(uint8_t code, uint8_t *out_action)
{
    uint8_t mapped;
    switch (code) {
    case 0: mapped = STEINS_MOB_ACT_NONE; break;          // read
    case 1: mapped = STEINS_MOB_ACT_NONE; break;          // ignore
    case 2: mapped = STEINS_MOB_ACT_SEND; break;
    case 3: mapped = STEINS_MOB_ACT_NO_SEND; break;
    case 4: mapped = STEINS_MOB_ACT_CALL; break;
    case 5: mapped = STEINS_MOB_ACT_NO_CALL; break;
    case 6: mapped = STEINS_MOB_ACT_REPLY; break;
    case 7: mapped = STEINS_MOB_ACT_SEND; break;          // dmail（目标由 choice.source 决定）
    default: return false;
    }
    if (out_action) *out_action = mapped;
    return true;
}

// CALLDESC 的 site 字符串 → CALLSITE 下标（两表顺序不同，按 site 身份匹配）。
static uint16_t callsite_index_of_str(const steins_pack_t *pack, uint16_t site_str)
{
    char want[40];
    want[0] = 0;
    if (steins_pack_str_utf8(pack, site_str, want, sizeof(want)) <= 0) return 0xFFFFu;
    for (uint16_t i = 0; i < steins_pack_call_site_count(pack); ++i) {
        steins_callsite_t cs;
        if (!steins_pack_call_site(pack, i, &cs)) continue;
        char text[40];
        text[0] = 0;
        if (steins_pack_str_utf8(pack, cs.source_str, text, sizeof(text)) <= 0) continue;
        if (strcmp(text, want) == 0) return i;
    }
    return 0xFFFFu;
}

static uint8_t call_kind_from_str(const char *s)
{
    return (s && strcmp(s, "dmail") == 0) ? STEINS_MOB_CALL_DMAIL : STEINS_MOB_CALL_PHONE;
}

// ---------------------------------------------------------------- mail
static bool pk_mail_meta(void *u, uint16_t id, steins_mobile_mail_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_mail_t m;
    if (!steins_pack_mail(c->pack, id, &m)) return false;
    out->id = id;
    out->type = m.type;
    return true;
}

static uint8_t pk_mail_succ(void *u, uint16_t id, uint16_t *out, uint8_t cap)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_mail_t m;
    if (!steins_pack_mail(c->pack, id, &m)) return 0;
    return steins_pack_mail_succ(c->pack, &m, out, cap, false);
}

// ---------------------------------------------------------------- events
static uint16_t pk_row_count(void *u, uint16_t node)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t first = 0, count = 0;
    if (!steins_pack_mrow_range(c->pack, node, &first, &count)) return 0;
    return count;
}

static bool pk_row(void *u, uint16_t node, uint16_t index, steins_mobile_row_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t first = 0, count = 0;
    if (!steins_pack_mrow_range(c->pack, node, &first, &count)) return false;
    if (index >= count) return false;
    steins_mrow_t r;
    if (!steins_pack_mrow(c->pack, (uint16_t)(first + index), &r)) return false;
    out->cursor = (r.cursor < 0) ? 0xFFFFFFFFu : (uint32_t)r.cursor;   // -1 → 0xFFFFFFFF
    out->index = index;
    out->count = r.event_count;
    return true;
}

static bool pk_event(void *u, uint16_t node, const steins_mobile_row_t *row, uint8_t index,
                     steins_mobile_event_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t first = 0, count = 0;
    if (!steins_pack_mrow_range(c->pack, node, &first, &count)) return false;
    steins_mrow_t r;
    if (!steins_pack_mrow(c->pack, (uint16_t)(first + row->index), &r)) return false;
    steins_mevent_t e;
    if (!steins_pack_mevent(c->pack, &r, index, &e)) return false;
    out->kind = e.kind;
    out->argc = e.argc;
    memset(out->arg, 0, sizeof(out->arg));
    for (uint8_t i = 0; i < e.argc && i < STEINS_MEVENT_MAX_ARGS; ++i) {
        out->arg[i] = (int32_t)e.arg[i];
    }
    // 只有 receive 的 channel 位允许 -1（包内 0xFFFF）。
    if (e.kind == STEINS_MOB_EV_RECEIVE && e.argc >= 2 && e.arg[1] == 0xFFFFu) {
        out->arg[1] = -1;
    }
    // 源 call 是 argc=5（site/owner/kind/items/waiting）。包把前 4 项归并进 CALLDESC，
    // MEVT 只留 desc 下标（argc=1）；这里按源顺序展开，事件机因此看到与源同形的参数。
    if (e.kind == STEINS_MOB_EV_CALL) {
        const uint16_t desc = (e.argc >= 1) ? e.arg[0] : 0xFFFFu;
        uint16_t site_str = 0, kind_str = 0, first_item = 0;
        uint8_t item_count = 0, owner = 0;
        bool waiting = false;
        if (!steins_pack_call_desc(c->pack, desc, &site_str, &kind_str, &first_item, &item_count,
                                   &owner, &waiting)) {
            return false;
        }
        char kind[16];
        kind[0] = 0;
        (void)steins_pack_str_utf8(c->pack, kind_str, kind, sizeof(kind));
        out->kind = STEINS_MOB_EV_CALL;
        out->argc = 5;
        out->arg[0] = (int32_t)callsite_index_of_str(c->pack, site_str);
        out->arg[1] = owner;
        out->arg[2] = call_kind_from_str(kind);
        // items 描述符统一用 CALLSITE 下标：`event_items(desc)` 因此对夹具/pack 同构。
        out->arg[3] = out->arg[0];
        out->arg[4] = waiting ? 1 : 0;
        if (out->arg[0] == 0xFFFF) return false;
    }
    return true;
}

static uint16_t pk_seed_ids(void *u, uint16_t desc, uint16_t offset, uint16_t *out, uint16_t cap)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t first = 0, count = 0;
    if (!steins_pack_seed_desc(c->pack, desc, &first, &count)) return 0;
    (void)first;
    if (offset >= count) return 0;
    uint16_t used = 0;
    for (uint16_t i = offset; i < count && used < cap; ++i) {
        uint16_t id = 0;
        if (!steins_pack_seed_id(c->pack, desc, i, &id)) break;
        out[used++] = id;
    }
    return used;
}

// 参数是 CALLSITE 下标（事件 arg[3]）。CALLSITE 的 item 只有包内 ACTION_CODE；
// 源字符串动作在 CALLDESC/CALLDITEM 里，两者取其一：这里用 CALLSITE（与事件机一致）。
static uint8_t pk_event_items(void *u, uint16_t site, steins_mobile_item_ref_t *out, uint8_t cap)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_callsite_t cs;
    if (!steins_pack_call_site(c->pack, site, &cs)) return 0;
    uint8_t used = 0;
    for (uint8_t i = 0; i < cs.item_count && used < cap; ++i) {
        steins_callitem_t it;
        if (!steins_pack_call_item(c->pack, (uint16_t)(cs.first_item + i), &it)) break;
        if (!steins_mobile_pack_typed_action(it.action, &out[used].action)) break;
        out[used].id = it.id;
        used++;
    }
    return used;
}

// ---------------------------------------------------------------- 窗口 / 关键电话
// WINDOW / KEYPHONE 段是**按记录紧凑存放**（21 / 24 条，不是按节点下标铺满），
// 所以按 node 查找要线性扫描记录里的 node 字段（getter 的第二个参数是段内下标）。
static bool find_window(const steins_pack_t *pack, uint16_t node, uint16_t *index_out)
{
    const uint32_t n = steins_pack_count(pack, STEINS_SEC_WINDOW);
    for (uint32_t i = 0; i < n; ++i) {
        steins_window_t w;
        if (!steins_pack_mobile_window(pack, (uint16_t)i, &w)) continue;
        if (w.node == node) {
            if (index_out) *index_out = (uint16_t)i;
            return true;
        }
    }
    return false;
}

static bool find_keyphone(const steins_pack_t *pack, uint16_t node, uint16_t *index_out)
{
    const uint32_t n = steins_pack_count(pack, STEINS_SEC_KEYPHONE);
    for (uint32_t i = 0; i < n; ++i) {
        steins_keyphone_t kp;
        if (!steins_pack_keyphone(pack, (uint16_t)i, &kp)) continue;
        if (kp.node == node) {
            if (index_out) *index_out = (uint16_t)i;
            return true;
        }
    }
    return false;
}

static bool pk_window(void *u, uint16_t node, steins_mobile_window_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t index = 0;
    if (!find_window(c->pack, node, &index)) return false;
    steins_window_t w;
    if (!steins_pack_mobile_window(c->pack, index, &w)) return false;
    out->node = w.node;
    out->id = w.id;
    out->limit = w.limit;
    out->skip = w.skip;
    out->read = w.read;
    out->option_count = w.opt_count;
    return true;
}

static bool pk_window_option(void *u, uint16_t node, uint8_t index,
                             steins_mobile_window_option_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t win_index = 0;
    if (!find_window(c->pack, node, &win_index)) return false;
    steins_window_t w;
    if (!steins_pack_mobile_window(c->pack, win_index, &w)) return false;
    if (index >= w.opt_count) return false;
    steins_winopt_t opt;
    if (!steins_pack_window_opt(c->pack, (uint16_t)(w.first_opt + index), &opt)) return false;
    // WINOPT 是 **typed** 域：值就是事件机枚举，不能走 legacy 映射。
    if (!steins_mobile_pack_typed_action(opt.action, &out->action)) return false;
    out->id = opt.id;
    out->to = opt.to;
    return true;
}

static bool pk_keyphone(void *u, uint16_t node, steins_mobile_keyphone_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    uint16_t kp_index = 0;
    if (!find_keyphone(c->pack, node, &kp_index)) return false;
    steins_keyphone_t kp;
    if (!steins_pack_keyphone(c->pack, kp_index, &kp)) return false;
    out->node = kp.node;
    out->skip = kp.skip;
    out->go = kp.go;
    out->limit = kp.limit;
    out->after = kp.after;
    out->sent = kp.sent;
    // Older packs encoded an absent sent field as zero. Plain telephone
    // choices do not send mail; preserve zero as a valid id for D-Mail choices.
    if (out->sent == 0) {
        steins_node_t root;
        steins_choice_t choice;
        if (!steins_pack_node(c->pack, node, &root) ||
            !steins_pack_choice(c->pack, root.choice, &choice)) return false;
        bool call = false, send = false;
        for (uint8_t i = 0; i < choice.count; ++i) {
            steins_citem_t item;
            uint8_t action;
            if (!steins_pack_citem(c->pack, (uint16_t)(choice.first_item + i), &item) ||
                !steins_mobile_pack_legacy_action(item.action, &action)) return false;
            call |= action == STEINS_MOB_ACT_CALL;
            send |= action == STEINS_MOB_ACT_SEND;
        }
        if (call && !send) out->sent = 0xFFFFu;
    }
    return true;
}

// ---------------------------------------------------------------- 通话
static bool pk_call_site(void *u, uint16_t site, steins_mobile_callsite_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_callsite_t cs;
    if (!steins_pack_call_site(c->pack, site, &cs)) return false;
    out->site = site;
    out->limit = cs.limit;
    out->owner = cs.owner;
    out->item_count = cs.item_count;
    return true;
}

static bool pk_call_item(void *u, uint16_t site, uint8_t index, steins_mobile_callitem_t *out)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_callsite_t cs;
    if (!steins_pack_call_site(c->pack, site, &cs)) return false;
    if (index >= cs.item_count) return false;
    steins_callitem_t it;
    if (!steins_pack_call_item(c->pack, (uint16_t)(cs.first_item + index), &it)) return false;
    if (!steins_mobile_pack_typed_action(it.action, &out->action)) return false;
    out->id = it.id;
    out->join = it.join;
    out->clip = (it.clip == 0xFFu) ? 0xFFFFu
                                   : (uint16_t)(STEINS_MOB_CLIP_NODE_BASE + it.clip);
    return true;
}

// ---------------------------------------------------------------- 图节点
static bool pk_node_count(void *u, uint16_t node, uint32_t *count)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_node_t nd;
    if (!steins_pack_node(c->pack, node, &nd)) return false;
    uint32_t total = 0;
    for (uint8_t i = 0; i < nd.chunk_count; ++i) {
        // nd.chunk_first 是 CHUNKREF 下标：必须先经 steins_pack_node_chunk 换成 CHUNK 下标。
        uint16_t chunk_index = 0;
        if (!steins_pack_node_chunk(c->pack, &nd, i, &chunk_index)) return false;
        steins_chunk_t ch;
        if (!steins_pack_chunk(c->pack, chunk_index, &ch)) return false;
        total += ch.count;
    }
    *count = total;
    return true;
}

static bool pk_node_choice_id(void *u, uint16_t node, uint16_t *choice_id)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_node_t nd;
    if (!steins_pack_node(c->pack, node, &nd)) return false;
    if (nd.choice == 0xFFFFu) return false;
    // 源 choice.id 是**邮件 id**（不是 CHOICE 段下标）：只有 CHOICE_META 侧元数据能给。
    // 没有 side 元数据的包 → 0xFFFF（源里 f.id 缺失），**不要**拿段下标冒充。
    uint16_t meta_id = 0xFFFFu;
    if (steins_pack_choice_meta(c->pack, nd.choice, NULL, &meta_id, NULL) && meta_id != 0xFFFFu) {
        *choice_id = meta_id;
        return true;
    }
    *choice_id = 0xFFFFu;
    return true;
}

static uint8_t pk_choice_items(void *u, uint16_t node, steins_mobile_choice_item_t *out,
                               uint8_t cap)
{
    steins_mobile_pack_ctx_t *c = u;
    steins_node_t nd;
    if (!steins_pack_node(c->pack, node, &nd)) return 0;
    if (nd.choice == 0xFFFFu) return 0;
    steins_choice_t ch;
    if (!steins_pack_choice(c->pack, nd.choice, &ch)) return 0;
    uint8_t used = 0;
    for (uint8_t i = 0; i < ch.count && used < cap; ++i) {
        steins_citem_t it;
        const uint16_t citem_index = (uint16_t)(ch.first_item + i);
        if (!steins_pack_citem(c->pack, citem_index, &it)) break;
        out[used].to = it.to;
        // item.id：源 item.id（reply 的回复邮件 id），来自 CITEM_META。
        uint16_t item_id = 0xFFFFu;
        (void)steins_pack_citem_meta(c->pack, citem_index, &item_id);
        out[used].id = item_id;
        if (!steins_mobile_pack_legacy_action(it.action, &out[used].action)) break;
        // 源 phone.js:15 outgoingId(choice,item)：
        //   action==='reply' ? item.id : action==='send' ? phoneData.dmail[choice.source] : undefined
        // → **只有 SEND 用 resolved outgoing**（包侧 helper 明确限 SEND；REPLY 时它返回 false、
        //   不写 out，避免把 CHOICE_META 尾部的 parent 当成"发送 id"）。
        out[used].source = 0xFFFFu;
        if (out[used].action == STEINS_MOB_ACT_SEND) {
            uint16_t mail_id = 0xFFFFu;
            if (steins_pack_choice_outgoing(c->pack, nd.choice, &mail_id) && mail_id != 0xFFFFu) {
                out[used].source = mail_id;
            }
            // source 仍是 0xFFFF → choose() 不写 sent（明确失败，不猜 SGxx→id）。
        }
        used++;
    }
    return used;
}

// presentation 的稳定 key 编号。
// q407 的 PRES 目录落地后（getter 签名已给：node-local row_index，不是全局 MROW 下标）：
//   定义 STEINS_PACK_HAVE_PRES_KEY 即自动接上，无需改本文件其它部分。
static uint16_t pk_presentation_key(void *u, uint16_t node, const steins_mobile_row_t *row,
                                    uint8_t event_index)
{
#if defined(STEINS_PACK_HAVE_PRES_KEY)
    steins_mobile_pack_ctx_t *c = u;
    uint16_t key = 0xFFFFu;
    /* row->index 是**节点内**行下标（适配层在 pk_row 里写入），正好是 PRES getter 要的语义。 */
    if (!steins_pack_pres_key(c->pack, node, row->index, event_index, &key)) return 0xFFFFu;
    return key;
#else
    (void)u; (void)node; (void)row; (void)event_index;
    return 0xFFFFu;   // 包没有 PRES 目录：明确"无 key"，不是伪造编号
#endif
}

static uint16_t pk_presentation_key_count(void *u)
{
#if defined(STEINS_PACK_HAVE_PRES_KEY)
    // q407 没有单独的 pres_count getter；用公开的段计数 API（STEINS_SEC_PRES），不自造函数。
    steins_mobile_pack_ctx_t *c = u;
    return (uint16_t)steins_pack_count(c->pack, STEINS_SEC_PRES);
#else
    (void)u;
    return 0;         // 0 = 无 key 目录
#endif
}

// ---------------------------------------------------------------- clip
static bool pk_clip_frames(void *u, uint16_t node, uint32_t *frames)
{
    steins_mobile_pack_ctx_t *c = u;
    if (node < STEINS_MOB_CLIP_NODE_BASE) return false;
    steins_clip_t clip;
    if (!steins_pack_clip(c->pack, (uint8_t)(node - STEINS_MOB_CLIP_NODE_BASE), &clip)) return false;
    *frames = clip.frame_count;
    return true;
}

static bool pk_is_clip_node(void *u, uint16_t node)
{
    uint32_t frames = 0;
    return pk_clip_frames(u, node, &frames);
}

// ---------------------------------------------------------------- 装配
bool steins_mobile_pack_content(const steins_pack_t *pack, steins_mobile_pack_ctx_t *ctx,
                                steins_mobile_content_t *content, void **user)
{
    if (!pack || !ctx || !content) return false;
    // 缺手机段（v3 包）就明确失败：不给"假内容"。
    if (pack->sec_off[STEINS_SEC_MROW] == 0u || pack->sec_off[STEINS_SEC_MNODE] == 0u ||
        pack->sec_off[STEINS_SEC_MAIL] == 0u || pack->sec_off[STEINS_SEC_WINDOW] == 0u) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->pack = pack;
    memset(content, 0, sizeof(*content));
    content->mail_meta = pk_mail_meta;
    content->mail_succ = pk_mail_succ;
    content->row_count = pk_row_count;
    content->row = pk_row;
    content->event = pk_event;
    content->seed_ids = pk_seed_ids;
    content->event_items = pk_event_items;
    content->window = pk_window;
    content->window_option = pk_window_option;
    content->keyphone = pk_keyphone;
    content->call_site = pk_call_site;
    content->call_item = pk_call_item;
    content->node_count = pk_node_count;
    content->node_choice_id = pk_node_choice_id;
    content->choice_items = pk_choice_items;
    content->clip_frames = pk_clip_frames;
    content->is_clip_node = pk_is_clip_node;
    content->presentation_key = pk_presentation_key;
    content->presentation_key_count = pk_presentation_key_count;
    if (user) *user = ctx;
    return true;
}

bool steins_mobile_pack_index_of_source(const steins_pack_t *pack, uint16_t source_id,
                                        uint16_t *pack_index)
{
    if (!pack) return false;
    const uint32_t nodes = steins_pack_count(pack, STEINS_SEC_NODE);
    for (uint32_t i = 0; i < nodes; ++i) {
        if (steins_mobile_pack_source_node(pack, (uint16_t)i) != source_id) continue;
        if (pack_index) *pack_index = (uint16_t)i;
        return true;
    }
    return false;
}

uint16_t steins_mobile_pack_clip_node(uint8_t clip)
{
    return (uint16_t)(STEINS_MOB_CLIP_NODE_BASE + clip);
}

bool steins_mobile_pack_clip_index(uint16_t node, uint8_t *clip)
{
    if (node < STEINS_MOB_CLIP_NODE_BASE) return false;
    const uint16_t index = (uint16_t)(node - STEINS_MOB_CLIP_NODE_BASE);
    if (index > 0xFFu) return false;
    if (clip) *clip = (uint8_t)index;
    return true;
}

uint16_t steins_mobile_pack_source_node(const steins_pack_t *pack, uint16_t pack_node)
{
    if (!pack) return 0xFFFFu;
    steins_node_t nd;
    if (!steins_pack_node(pack, pack_node, &nd)) return 0xFFFFu;
    char id[16];
    id[0] = '\0';
    if (steins_pack_str_utf8(pack, nd.id_str, id, sizeof(id)) <= 0) return 0xFFFFu;
    if (id[0] != 'n') return 0xFFFFu;
    int value = 0;
    for (int i = 1; id[i] != '\0'; ++i) {
        if (id[i] < '0' || id[i] > '9') return 0xFFFFu;
        value = value * 10 + (id[i] - '0');
        if (value > 0xFFFF) return 0xFFFFu;
    }
    return (uint16_t)value;
}
