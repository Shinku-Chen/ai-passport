// tests/test_steins_mobile_events.c —— 生产手机事件机（main/steins_mobile.c）的源 oracle 回归。
//
// 夹具：tests/fixtures/steins_mobile/steins_mobile_fixture.h（由 gen_fixture.py 从只读源快照生成，
// 含 mail-index 646 条、n0/n3/n32 的 212/150/* 条事件行、窗口/关键电话/通话/图内 choice）。
// 期望值来自 build/review-model/mobile-contract-20261102-mobile/logs/oracle.json（S1..S12）。
//
// 用法：
//   test_steins_mobile_events            # 正常回归，全绿返回 0
//   test_steins_mobile_events --self-check   # 比较器自测：把每项期望故意改一个字段，
//                                            # 要求每个场景都能报出 >0 处不一致（否则失败）
#include "steins_mobile.h"
#include "steins_mobile_pack.h"

#include "fixtures/steins_mobile/steins_mobile_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_fails;
static int g_mismatch;   // 当前场景的比较器累计不一致数

#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { g_fails++; \
    printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static bool g_mutate;   // --self-check：期望值故意偏移

// 内容提供者：默认夹具；--pack <file> 时用生产适配层（main/steins_mobile_pack.c）。
static bool g_use_pack;
static steins_pack_t g_pack;
static uint8_t *g_pack_blob;
static steins_mobile_pack_ctx_t g_pack_ctx;
static steins_mobile_content_t g_pack_content;

// 源节点号(n<num>) ↔ 包节点下标（pack 模式）。夹具模式恒等。
static uint16_t NODE(uint16_t source_node)
{
    if (!g_use_pack) return source_node;
    uint16_t index = 0xFFFFu;
    if (!steins_mobile_pack_index_of_source(&g_pack, source_node, &index)) {
        printf("  FAIL source node n%u not found in pack NODE table%c", source_node, 10);
        return 0xFFFFu;
    }
    return index;
}

// ---------------------------------------------------------------- 内容后端（夹具）
static const fx_row_t *rows_for(uint16_t node, uint16_t *count)
{
    if (node == 0) { if (count) *count = FX_ROWS_n0_COUNT; return FX_ROWS_n0; }
    if (node == 3) { if (count) *count = FX_ROWS_n3_COUNT; return FX_ROWS_n3; }
    if (node == 32) { if (count) *count = FX_ROWS_n32_COUNT; return FX_ROWS_n32; }
    if (node == 246) { if (count) *count = FX_ROWS_n246_COUNT; return FX_ROWS_n246; }
    if (count) *count = 0;
    return NULL;
}

static const fx_event_t *event_table(uint16_t node)
{
    if (node == 0) return FX_EV_n0;
    if (node == 3) return FX_EV_n3;
    if (node == 32) return FX_EV_n32;
    if (node == 246) return FX_EV_n246;
    return NULL;
}

static bool fx_mail_meta(void *ctx, uint16_t id, steins_mobile_mail_t *out)
{
    (void)ctx;
    if (id >= FX_MAIL_COUNT) return false;
    out->id = id;
    out->type = FX_MAIL[id].type;
    return true;
}

static uint8_t fx_mail_succ(void *ctx, uint16_t id, uint16_t *out, uint8_t cap)
{
    (void)ctx;
    if (id >= FX_MAIL_COUNT) return 0;
    const uint16_t first = FX_MAIL_SUCC_OFF[id];
    const uint16_t last = FX_MAIL_SUCC_OFF[id + 1];
    uint8_t n = 0;
    for (uint16_t i = first; i < last && n < cap; ++i) out[n++] = FX_MAIL_SUCC[i];
    return n;
}

static uint16_t fx_row_count(void *ctx, uint16_t node)
{
    (void)ctx;
    uint16_t count = 0;
    (void)rows_for(node, &count);
    return count;
}

static bool fx_row(void *ctx, uint16_t node, uint16_t index, steins_mobile_row_t *out)
{
    (void)ctx;
    uint16_t count = 0;
    const fx_row_t *rows = rows_for(node, &count);
    if (!rows || index >= count) return false;
    out->cursor = rows[index].cursor;
    out->index = index;
    out->count = rows[index].count;
    return true;
}

static bool fx_event(void *ctx, uint16_t node, const steins_mobile_row_t *row, uint8_t index,
                     steins_mobile_event_t *out)
{
    (void)ctx;
    const fx_event_t *evs = event_table(node);
    const fx_row_t *rows = NULL;
    uint16_t count = 0;
    rows = rows_for(node, &count);
    if (!evs || !rows || row->index >= count) return false;
    const fx_row_t *r = &rows[row->index];
    if (index >= r->count) return false;
    const fx_event_t *ev = &evs[r->first + index];
    out->kind = ev->kind;
    out->argc = ev->argc;
    for (int i = 0; i < (int)STEINS_MOB_EVENT_ARGS; ++i) out->arg[i] = ev->arg[i];
    return true;
}

// 源 seed 行的 arg[0] 是 FX_SEED_DESC 下标（{first,count}），ID 流在 FX_SEED_IDS。
static uint16_t fx_seed_ids(void *ctx, uint16_t desc, uint16_t offset, uint16_t *out, uint16_t cap)
{
    (void)ctx;
    if (desc >= FX_SEED_DESC_COUNT) return 0;
    const uint32_t first = FX_SEED_DESC[desc].first;
    const uint16_t n = FX_SEED_DESC[desc].count;
    if (offset >= n) return 0;
    uint16_t used = 0;
    for (uint16_t i = offset; i < n && used < cap; ++i) out[used++] = FX_SEED_IDS[first + i];
    return used;
}

// 源 call 事件的第 5 个参数：有序 items（夹具断言过它与 callsite 目录一致）。
static uint8_t fx_event_items(void *ctx, uint16_t desc, steins_mobile_item_ref_t *out, uint8_t cap)
{
    (void)ctx;
    const uint16_t site = desc;             // 夹具：items 描述符 == CALLSITE 下标
    if (site >= FX_CALLSITE_COUNT) return 0;
    uint8_t used = 0;
    for (uint8_t i = 0; i < FX_CALLSITES[site].item_count && used < cap; ++i) {
        const fx_callitem_t *it = &FX_CALLITEMS[FX_CALLSITES[site].first_item + i];
        out[used].action = it->action;
        out[used].id = it->id;
        used++;
    }
    return used;
}

static bool fx_window(void *ctx, uint16_t node, steins_mobile_window_t *out)
{
    (void)ctx;
    for (uint16_t i = 0; i < FX_WINDOW_COUNT; ++i) {
        if (FX_WINDOWS[i].node != node) continue;
        out->node = FX_WINDOWS[i].node;
        out->id = FX_WINDOWS[i].id;
        out->limit = FX_WINDOWS[i].limit;
        out->skip = FX_WINDOWS[i].skip;
        out->read = FX_WINDOWS[i].read;
        out->option_count = FX_WINDOWS[i].opt_count;
        return true;
    }
    return false;
}

static bool fx_window_option(void *ctx, uint16_t node, uint8_t index,
                             steins_mobile_window_option_t *out)
{
    (void)ctx;
    for (uint16_t i = 0; i < FX_WINDOW_COUNT; ++i) {
        if (FX_WINDOWS[i].node != node) continue;
        if (index >= FX_WINDOWS[i].opt_count) return false;
        const fx_winopt_t *opt = &FX_WINOPTS[FX_WINDOWS[i].first_opt + index];
        out->action = opt->action;
        out->id = opt->id;
        out->to = opt->to;
        return true;
    }
    return false;
}

static bool fx_keyphone(void *ctx, uint16_t node, steins_mobile_keyphone_t *out)
{
    (void)ctx;
    for (uint16_t i = 0; i < FX_KEYPHONE_COUNT; ++i) {
        if (FX_KEYPHONES[i].node != node) continue;
        out->node = FX_KEYPHONES[i].node;
        out->skip = FX_KEYPHONES[i].skip;
        out->go = FX_KEYPHONES[i].go;
        out->limit = FX_KEYPHONES[i].limit;
        out->after = FX_KEYPHONES[i].after;
        out->sent = FX_KEYPHONES[i].sent;
        return true;
    }
    return false;
}

static bool fx_call_site(void *ctx, uint16_t site, steins_mobile_callsite_t *out)
{
    (void)ctx;
    if (site >= FX_CALLSITE_COUNT) return false;
    out->site = FX_CALLSITES[site].site;
    out->limit = FX_CALLSITES[site].limit;
    out->owner = FX_CALLSITES[site].owner;
    out->item_count = FX_CALLSITES[site].item_count;
    return true;
}

static bool fx_call_item(void *ctx, uint16_t site, uint8_t index, steins_mobile_callitem_t *out)
{
    (void)ctx;
    if (site >= FX_CALLSITE_COUNT) return false;
    if (index >= FX_CALLSITES[site].item_count) return false;
    const fx_callitem_t *it = &FX_CALLITEMS[FX_CALLSITES[site].first_item + index];
    out->action = it->action;
    out->clip = it->clip;
    out->join = it->join;
    out->id = it->id;
    return true;
}

static const fx_node_t *fx_node(uint16_t node)
{
    for (uint16_t i = 0; i < FX_NODE_COUNT; ++i) {
        if (FX_NODES[i].node == node) return &FX_NODES[i];
    }
    return NULL;
}

static bool fx_node_count(void *ctx, uint16_t node, uint32_t *count)
{
    (void)ctx;
    const fx_node_t *n = fx_node(node);
    if (!n) return false;
    *count = n->count;
    return true;
}

static bool fx_node_choice_id(void *ctx, uint16_t node, uint16_t *choice_id)
{
    (void)ctx;
    const fx_node_t *n = fx_node(node);
    if (!n || n->choice_id == 0xFFFFu) return false;
    *choice_id = n->choice_id;
    return true;
}

static uint8_t fx_choice_items(void *ctx, uint16_t node, steins_mobile_choice_item_t *out,
                               uint8_t cap)
{
    (void)ctx;
    const fx_node_t *n = fx_node(node);
    if (!n) return 0;
    uint8_t k = 0;
    for (uint8_t i = 0; i < n->item_count && k < cap; ++i) {
        const fx_choice_item_t *it = &FX_CHOICE_ITEMS[n->first_item + i];
        out[k].to = it->to;
        out[k].id = it->id;
        out[k].source = it->source;
        out[k].action = it->action;
        k++;
    }
    return k;
}

// presentation 的稳定 key 编号：夹具按源 key 顺序枚举（下标即 key）。
static uint16_t fx_presentation_key(void *ctx, uint16_t node, const steins_mobile_row_t *row,
                                    uint8_t event_index)
{
    (void)ctx;
    const uint32_t cursor = row->cursor;
    for (uint16_t i = 0; i < FX_PRES_COUNT; ++i) {
        if (FX_PRES[i].node != node) continue;
        if (FX_PRES[i].cursor != cursor) continue;
        if (FX_PRES[i].event_index != event_index) continue;
        return FX_PRES[i].key;
    }
    return 0xFFFFu;
}

static uint16_t fx_presentation_key_count(void *ctx)
{
    (void)ctx;
    return (uint16_t)FX_PRES_COUNT;
}

static bool fx_clip_frames(void *ctx, uint16_t node, uint32_t *frames)
{
    (void)ctx;
    for (uint16_t i = 0; i < FX_CLIP_COUNT; ++i) {
        if (FX_CLIPS[i].node != node) continue;
        *frames = FX_CLIPS[i].frames;
        return true;
    }
    return false;
}

static bool fx_is_clip_node(void *ctx, uint16_t node)
{
    uint32_t frames = 0;
    return fx_clip_frames(ctx, node, &frames);
}

static const steins_mobile_content_t FIXTURE_CONTENT = {
    .mail_meta = fx_mail_meta,
    .mail_succ = fx_mail_succ,
    .row_count = fx_row_count,
    .row = fx_row,
    .event = fx_event,
    .seed_ids = fx_seed_ids,
    .event_items = fx_event_items,
    .window = fx_window,
    .window_option = fx_window_option,
    .keyphone = fx_keyphone,
    .call_site = fx_call_site,
    .call_item = fx_call_item,
    .node_count = fx_node_count,
    .node_choice_id = fx_node_choice_id,
    .choice_items = fx_choice_items,
    .clip_frames = fx_clip_frames,
    .is_clip_node = fx_is_clip_node,
    .presentation_key = fx_presentation_key,
    .presentation_key_count = fx_presentation_key_count,
};

// ---------------------------------------------------------------- provider 包装
// 场景里不再直连 fx_*（那样包模式会读错内容），统一走当前 provider。
static const steins_mobile_content_t *prov(void)
{
    return g_use_pack ? &g_pack_content : &FIXTURE_CONTENT;
}

static void *prov_ctx(void)
{
    return g_use_pack ? (void *)&g_pack_ctx : NULL;
}

static uint16_t prov_row_count(uint16_t node)
{
    return prov()->row_count ? prov()->row_count(prov_ctx(), node) : 0;
}

static bool prov_row(uint16_t node, uint16_t index, steins_mobile_row_t *out)
{
    return prov()->row && prov()->row(prov_ctx(), node, index, out);
}

static bool prov_event(uint16_t node, const steins_mobile_row_t *row, uint8_t index,
                       steins_mobile_event_t *out)
{
    return prov()->event && prov()->event(prov_ctx(), node, row, index, out);
}

static uint16_t prov_seed_ids(uint16_t desc, uint16_t offset, uint16_t *out, uint16_t cap)
{
    return prov()->seed_ids ? prov()->seed_ids(prov_ctx(), desc, offset, out, cap) : 0;
}

static uint8_t prov_event_items(uint16_t desc, steins_mobile_item_ref_t *out, uint8_t cap)
{
    return prov()->event_items ? prov()->event_items(prov_ctx(), desc, out, cap) : 0;
}

static uint32_t prov_clip_frames(uint16_t node)
{
    uint32_t frames = 0;
    if (!prov()->clip_frames || !prov()->clip_frames(prov_ctx(), node, &frames)) return 0;
    return frames;
}

static uint16_t prov_site_index(const char *key)
{
    if (!g_use_pack) {
        for (uint16_t i = 0; i < FX_CALLSITE_COUNT; ++i) {
            if (strcmp(FX_CALLSITE_KEYS[i], key) == 0) return i;
        }
        return 0xFFFFu;
    }
    /* pack：CALLSITE.source_str 与源字符串比对（CALLDESC 顺序与 CALLSITE 不同，
       所以按 site 身份匹配，不能按下标假设）。 */
    for (uint16_t i = 0; i < steins_pack_call_site_count(&g_pack); ++i) {
        steins_callsite_t cs;
        if (!steins_pack_call_site(&g_pack, i, &cs)) continue;
        char text[40];
        text[0] = 0;
        (void)steins_pack_str_utf8(&g_pack, cs.source_str, text, sizeof(text));
        if (strcmp(text, key) == 0) return i;
    }
    return 0xFFFFu;
}

// 通话站点 "21:583" 在 FX_CALLSITES 里的下标（夹具按 key 字典序）。
static uint16_t site_index_of(const char *key)
{
    for (uint16_t i = 0; i < FX_CALLSITE_COUNT; ++i) {
        if (strcmp(FX_CALLSITE_KEYS[i], key) == 0) return i;
    }
    return 0xFFFFu;
}

// ---------------------------------------------------------------- 比较器
static void expect_u32(const char *label, uint32_t got, uint32_t want)
{
    const uint32_t expect = g_mutate ? want + 1u : want;
    if (got != expect) {
        g_mismatch++;
        printf("  mismatch %-38s got=%u want=%u\n", label, got, expect);
    }
}

static void expect_ids(const char *label, const uint16_t *got, uint16_t n, const uint16_t *want,
                       uint16_t wn)
{
    uint16_t want_copy[STEINS_CHK_MAX_INBOX];
    uint16_t wn_used = wn;
    if (wn_used > STEINS_CHK_MAX_INBOX) wn_used = STEINS_CHK_MAX_INBOX;
    memcpy(want_copy, want, (size_t)wn_used * sizeof(uint16_t));
    if (g_mutate && wn_used > 0) want_copy[0] = (uint16_t)(want_copy[0] ^ 0x1u);
    if (g_mutate && wn_used == 0) wn_used = 1;   // 空期望：改成一个不存在的元素
    if (n != wn_used) {
        g_mismatch++;
        printf("  mismatch %-38s count got=%u want=%u\n", label, n, wn_used);
        return;
    }
    for (uint16_t i = 0; i < n; ++i) {
        if (got[i] != want_copy[i]) {
            g_mismatch++;
            printf("  mismatch %-38s [%u] got=%u want=%u\n", label, i, got[i], want_copy[i]);
            return;
        }
    }
}

static void expect_channels(const steins_checkpoint_t *chk, const uint16_t want[5])
{
    uint16_t w[5];
    memcpy(w, want, sizeof(w));
    if (g_mutate) w[1] = (uint16_t)(w[1] ^ 0x1u);
    for (int i = 0; i < 5; ++i) {
        if (chk->channels[i] != w[i]) {
            g_mismatch++;
            printf("  mismatch channels[%d] got=%u want=%u\n", i, chk->channels[i], w[i]);
            return;
        }
    }
}

static void expect_bits(const char *label, const steins_checkpoint_t *chk,
                        bool (*fn)(const steins_checkpoint_t *, uint16_t), const uint16_t *want,
                        uint16_t wn)
{
    uint16_t wn_used = wn;
    const uint16_t *w = want;
    uint16_t mutated[STEINS_CHK_MAX_INBOX];
    if (g_mutate) {
        memcpy(mutated, want, (size_t)wn * sizeof(uint16_t));
        if (wn > 0) mutated[0] = (uint16_t)(mutated[0] ^ 0x1u);
        w = mutated;
        if (wn == 0) wn_used = 1;
    }
    uint16_t n = 0;
    for (uint16_t id = 0; id < 646; ++id) if (fn(chk, id)) n++;
    if (n != wn_used) {
        g_mismatch++;
        printf("  mismatch %-38s count got=%u want=%u\n", label, n, wn_used);
        return;
    }
    for (uint16_t i = 0; i < wn_used; ++i) {
        if (!fn(chk, w[i])) {
            g_mismatch++;
            printf("  mismatch %-38s missing id=%u\n", label, w[i]);
            return;
        }
    }
}

static bool bit_read(const steins_checkpoint_t *chk, uint16_t id) { return steins_checkpoint_read(chk, id); }
static bool bit_unread(const steins_checkpoint_t *chk, uint16_t id) { return steins_checkpoint_unread(chk, id); }

static void expect_open(const steins_checkpoint_t *chk, uint16_t id, int16_t want)
{
    const int16_t expect = g_mutate ? (int16_t)(want + 1) : want;
    const int16_t got = steins_mobile_open_channel(chk, id);
    if (got != expect) {
        g_mismatch++;
        printf("  mismatch open[%u] got=%d want=%d\n", id, got, expect);
    }
}

// ---------------------------------------------------------------- 场景工具
static steins_mobile_t g_engine;

static void scenario_begin(const char *name)
{
    g_mismatch = 0;
    printf("[scenario] %s\n", name);
}

static void scenario_end(const char *name, bool expect_mismatch)
{
    g_checks++;
    const bool ok = expect_mismatch ? (g_mismatch > 0) : (g_mismatch == 0);
    if (!ok) {
        g_fails++;
        printf("  FAIL %s: %s (mismatch=%d)\n", name,
               expect_mismatch ? "comparator did not detect the mutation" : "state mismatch",
               g_mismatch);
    } else if (!expect_mismatch) {
        printf("  ok %s\n", name);
    }
}

// 进入 n0@351 之类的"已经走到某游标"的状态
static void setup_at(steins_checkpoint_t *chk, uint16_t node, uint32_t cursor, uint32_t last_at)
{
    steins_checkpoint_init(chk);
    chk->node = node;
    chk->cursor = cursor;
    chk->last_node = node;
    chk->last_at = last_at;
    chk->channels[0] = chk->channels[1] = chk->channels[2] = chk->channels[3] = chk->channels[4] = 999;
}

// ---------------------------------------------------------------- S1
static void run_s1(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S1 seed/receive n0@351");
    setup_at(&chk, NODE(0), 351, 350);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S1 sync");
    const uint16_t want_inbox[] = {0};
    expect_ids("inbox", chk.inbox, chk.inbox_count, want_inbox, 1);
    expect_u32("sent count", chk.sent_count, 0);
    expect_bits("unread", &chk, bit_unread, want_inbox, 1);
    expect_u32("lastAt", chk.last_at, 351);
    scenario_end("S1", g_mutate);
}

// ---------------------------------------------------------------- S2/S3
static void run_s2(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S2 deferred boundary n3@34 (window n0/297)");
    setup_at(&chk, NODE(3), 34, 33);
    chk.deferred_present = 1;
    chk.deferred_root = NODE(0);
    chk.deferred_id = 297;
    chk.deferred_read = 0;
    chk.open_id[0] = 297;
    chk.open_val[0] = 1;              // channel 0
    chk.open_count = 1;
    steins_mobile_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(steins_mobile_deferred_boundary(&g_engine, &chk, &frame), "S2 boundary at limit 34");
    expect_u32("S2 boundary type", frame.type, STEINS_MOB_FRAME_DEFERRED);
    expect_u32("S2 boundary items", frame.item_count, 4);   // 298/299/300 + expire
    {
        steins_mobile_boundary_item_t opts[8];
        const uint8_t n = steins_mobile_deferred_options(&g_engine, &chk, opts, 8);
        expect_u32("S2 option count", n, 4);
        expect_u32("S2 option0 id", opts[0].id, 298);
        expect_u32("S2 option0 action", opts[0].action, STEINS_MOB_ACT_REPLY);
        expect_u32("S2 option0 to", opts[0].to, NODE(3));
        expect_u32("S2 option3 action", opts[3].action, STEINS_MOB_ACT_EXPIRE_DEFERRED);
    }
    scenario_end("S2", g_mutate);

    scenario_begin("S3 reply 298 from deferred boundary");
    steins_mobile_pos_t land;
    CHECK(steins_mobile_can_reply(&g_engine, &chk, 297), "S3 canReply(297)");
    CHECK(steins_mobile_answer_deferred(&g_engine, &chk, STEINS_MOB_ACT_REPLY, 298, &land),
          "S3 answer reply 298");
    {
        const uint16_t want[] = {298};
        expect_ids("S3 sent", chk.sent, chk.sent_count, want, 1);
        CHECK(steins_checkpoint_replied(&chk, 297), "S3 replied contains 297");
        expect_u32("S3 deferred cleared", chk.deferred_present, 0);
        expect_open(&chk, 297, -1);                            // 源：delete open[parent]
        expect_u32("S3 landing node", land.node, NODE(3));           // 298 → n3
        expect_u32("S3 landing cursor", land.cursor, 34);      // **不是 0**
        expect_u32("S3 lastAt", chk.last_at, 33);              // cursor-1
    }
    scenario_end("S3", g_mutate);
}

// ---------------------------------------------------------------- S4
static void run_s4(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S4 erase n0@531");
    setup_at(&chk, NODE(0), 531, 530);
    const uint16_t inbox[] = {8, 212, 233, 300, 636};
    for (uint16_t i = 0; i < 5; ++i) chk.inbox[chk.inbox_count++] = inbox[i];
    chk.sent[chk.sent_count++] = 212;
    (void)steins_checkpoint_set_unread(&chk, 300, true);
    {
        // open{300:1}：直接用事件机接口无法设置，这里按上传状态手工写入。
        chk.open_id[0] = 300;
        chk.open_val[0] = (uint8_t)(1 + 1);        // 编码 = channel+1
        chk.open_count = 1;
    }
    CHECK(steins_mobile_sync(&g_engine, &chk), "S4 sync");
    const uint16_t want_inbox[] = {1};
    expect_ids("inbox", chk.inbox, chk.inbox_count, want_inbox, 1);
    expect_u32("sent count", chk.sent_count, 0);
    expect_bits("unread", &chk, bit_unread, NULL, 0);   // readBit 1 true 之后不再是未读
    expect_bits("read", &chk, bit_read, want_inbox, 1);
    expect_u32("open count", chk.open_count, 0);
    scenario_end("S4", g_mutate);
}

// ---------------------------------------------------------------- S5
static void run_s5(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S5 expire+queue n0@557");
    setup_at(&chk, NODE(0), 557, 556);
    chk.open_id[0] = 297;
    chk.open_val[0] = 1;   // channel 0
    chk.open_count = 1;
    CHECK(steins_mobile_sync(&g_engine, &chk), "S5 sync");
    const uint16_t want[5] = {999, 388, 473, 999, 576};
    expect_channels(&chk, want);
    expect_u32("open count", chk.open_count, 0);
    scenario_end("S5", g_mutate);
}

// ---------------------------------------------------------------- S6
static void run_s6(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S6 dispatch->receive n0@938");
    setup_at(&chk, NODE(0), 938, 937);
    chk.channels[1] = 388;
    CHECK(steins_mobile_sync(&g_engine, &chk), "S6 sync");
    const uint16_t want_inbox[] = {388};
    expect_ids("inbox", chk.inbox, chk.inbox_count, want_inbox, 1);
    expect_bits("unread", &chk, bit_unread, want_inbox, 1);
    expect_open(&chk, 388, 1);
    const uint16_t want_ch[5] = {999, 388, 999, 999, 999};
    expect_channels(&chk, want_ch);
    scenario_end("S6", g_mutate);
}

// ---------------------------------------------------------------- S7
static void run_s7(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S7 rule 1627 (read 404)");
    setup_at(&chk, NODE(0), 4799, 4798);
    (void)steins_checkpoint_set_read(&chk, 404, true);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S7a sync");
    {
        const uint16_t want_inbox[] = {405};
        expect_ids("inbox(read 404)", chk.inbox, chk.inbox_count, want_inbox, 1);
        expect_open(&chk, 405, 1);
        const uint16_t want_ch[5] = {999, 405, 999, 999, 999};
        expect_channels(&chk, want_ch);
    }
    scenario_end("S7a", g_mutate);

    scenario_begin("S7 rule 1627 (404 unread)");
    setup_at(&chk, NODE(0), 4799, 4798);
    chk.channels[1] = 399;
    CHECK(steins_mobile_sync(&g_engine, &chk), "S7b sync");
    {
        const uint16_t want_inbox[] = {399};
        expect_ids("inbox(404 unread)", chk.inbox, chk.inbox_count, want_inbox, 1);
        const uint16_t want_ch[5] = {999, 399, 999, 999, 999};
        expect_channels(&chk, want_ch);
    }
    scenario_end("S7b", g_mutate);
}

// ---------------------------------------------------------------- S8
static void run_s8(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S8 call + clip round trip n0@2131");
    setup_at(&chk, NODE(0), 2131, 2130);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S8 sync");
    CHECK(chk.call_present == 1, "S8 call present");
    const uint16_t site = prov_site_index("21:583");
    CHECK(site != 0xFFFFu, "S8 site index");
    expect_u32("call site", chk.call_site, site);
    expect_u32("call at", chk.call_at, 2131);
    {
        steins_mobile_row_t row;
        CHECK(prov_row(NODE(0), 0, &row), "S8 row0");
        // 找出 n0@2131 的行，读 call 事件的 item_count
        for (uint16_t i = 0; i < prov_row_count(NODE(0)); ++i) {
            if (!prov_row(NODE(0), i, &row)) continue;
            if (row.cursor != 2131) continue;
            for (uint8_t k = 0; k < row.count; ++k) {
                steins_mobile_event_t ev;
                if (!prov_event(NODE(0), &row, k, &ev)) continue;
                if (ev.kind != STEINS_MOB_EV_CALL) continue;
                expect_u32("call event argc", ev.argc, 5);
                expect_u32("call event items desc", ev.arg[3], site_index_of("21:583"));
                steins_mobile_item_ref_t refs[8];
                const uint8_t n = prov_event_items((uint16_t)ev.arg[3], refs, 8);
                expect_u32("call event items", n, 4);
                expect_u32("call item0 action", refs[0].action, STEINS_MOB_ACT_COMPOSE);
            }
            break;
        }
    }
    uint16_t cur_site = 0;
    uint8_t items = 0;
    CHECK(steins_mobile_call_current(&g_engine, &chk, &cur_site, &items), "S8 currentCall");
    expect_u32("call items", items, 4);
    {
        steins_mobile_pos_t land;
        CHECK(steins_mobile_take_call(&g_engine, &chk, 1, &land), "S8 takeCall(1)");
        expect_u32("after take node", land.node, 0x8000u);          // c0
        expect_u32("after take cursor", land.cursor, 0);
        CHECK(chk.call_return_present == 1, "S8 callReturn present");
        expect_u32("callReturn node", chk.call_return_node, NODE(0));
        expect_u32("callReturn cursor", chk.call_return_cursor, 2163);
        CHECK(chk.cursor == 0 && chk.node == 0x8000u, "S8 in clip node at 0");
        uint32_t frames = 0;
        CHECK((frames = prov_clip_frames(0x8000u)) == 20, "S8 clip has 20 frames");
        chk.cursor = frames;                                        // 播完 clip
        steins_mobile_pos_t back;
        CHECK(steins_mobile_return_call(&g_engine, &chk, &back), "S8 returnCall");
        expect_u32("return node", back.node, NODE(0));
        expect_u32("return cursor", back.cursor, 2163);
        CHECK(chk.call_return_present == 0, "S8 callReturn cleared");
    }
    scenario_end("S8", g_mutate);
}

// ---------------------------------------------------------------- S9
static void run_s9(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S9 presentation draft n32@695");
    setup_at(&chk, NODE(32), 695, 694);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S9a sync");
    expect_u32("pending presentations", chk.pres_count, 1);
    {
        uint16_t id = 0; uint8_t kind = 0xFF, owner = 0xFF;
        CHECK(steins_mobile_plot_presentation(&chk, &id, &kind, &owner), "S9a plot");
        expect_u32("plot id", id, 263);
        expect_u32("plot kind", kind, STEINS_MOB_PRES_DRAFT);
        expect_u32("plot owner", owner, 1);
        CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "S9a finish");
        expect_u32("sent count after draft", chk.sent_count, 0);
        expect_u32("pending after finish", chk.pres_count, 0);
    }
    scenario_end("S9a", g_mutate);

    scenario_begin("S9 presentation send n32@751");
    setup_at(&chk, NODE(32), 751, 750);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S9b sync");
    expect_u32("pending presentations", chk.pres_count, 1);
    CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "S9b finish");
    {
        const uint16_t want[] = {266};
        expect_ids("sent", chk.sent, chk.sent_count, want, 1);
    }
    scenario_end("S9b", g_mutate);
}

// ---------------------------------------------------------------- S10
static void run_s10(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S10 restore keeps lastAt (no call replay)");
    setup_at(&chk, NODE(0), 900, 900);
    CHECK(steins_mobile_sync(&g_engine, &chk), "S10 sync with lastAt");
    expect_u32("call present (kept lastAt)", chk.call_present, 0);
    scenario_end("S10-kept", g_mutate);

    scenario_begin("S10 restore without lastAt (call replays)");
    setup_at(&chk, NODE(0), 900, 0xFFFFFFFFu);   // lastAt 丢失
    CHECK(steins_mobile_sync(&g_engine, &chk), "S10 sync without lastAt");
    expect_u32("call present (lost lastAt)", chk.call_present, 1);
    expect_u32("replayed call at", chk.call_at, 862);
    scenario_end("S10-lost", g_mutate);
}

// ---------------------------------------------------------------- S12
static void run_s12(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("S12 D-Mail send n3@8907");
    setup_at(&chk, NODE(3), 8907, 8906);
    steins_mobile_pos_t land;
    CHECK(steins_mobile_choose(&g_engine, &chk, NODE(3), 0, 0, &land), "S12 choose item0");
    {
        const uint16_t want[] = {233};
        expect_ids("sent", chk.sent, chk.sent_count, want, 1);
    }
    expect_u32("landing node", land.node, NODE(9));
    expect_u32("landing cursor", land.cursor, 0);
    scenario_end("S12", g_mutate);
}

// ---------------------------------------------------------------- K1 关键电话
static void run_k1(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("K1 keyPhone n3 (noSend -> boundary -> send)");
    setup_at(&chk, NODE(3), 8907, 8906);
    steins_mobile_pos_t land;
    CHECK(steins_mobile_choose(&g_engine, &chk, NODE(3), 1, 0, &land), "K1 choose noSend");
    CHECK(chk.keyphone_present == 1, "K1 keyPhone window present");
    expect_u32("skip node", chk.node, NODE(10));
    expect_u32("skip cursor", chk.cursor, 0);
    expect_u32("keyphone root", chk.keyphone_root, NODE(3));
    // 到 skip 节点的 limit 之前不弹边界
    chk.cursor = 21;
    steins_mobile_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(!steins_mobile_keyphone_boundary(&g_engine, &chk, &frame), "K1 no boundary before limit");
    chk.cursor = 22;
    CHECK(steins_mobile_keyphone_boundary(&g_engine, &chk, &frame), "K1 boundary at limit");
    expect_u32("boundary items", frame.item_count, 2);
    {
        steins_mobile_boundary_item_t opts[8];
        const uint8_t n = steins_mobile_keyphone_options(&g_engine, &chk, opts, 8);
        expect_u32("keyphone options", n, 2);
        expect_u32("option0 action", opts[0].action, STEINS_MOB_ACT_SEND);
    }
    CHECK(steins_mobile_answer_key_phone(&g_engine, &chk, 0, &land), "K1 answer send");
    expect_u32("answer node", land.node, NODE(9));
    expect_u32("answer cursor", land.cursor, 1);
    {
        const uint16_t want[] = {233};
        expect_ids("sent", chk.sent, chk.sent_count, want, 1);
    }
    scenario_end("K1", g_mutate);
}

// ---------------------------------------------------------------- 容量守卫
static void run_capacity(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("C1 inbox 满不静默截断");
    setup_at(&chk, NODE(0), 351, 350);
    chk.inbox_count = STEINS_CHK_MAX_INBOX;
    for (uint16_t i = 0; i < STEINS_CHK_MAX_INBOX; ++i) chk.inbox[i] = (uint16_t)((i % 645) + 1);
    const uint16_t before = chk.inbox_count;
    CHECK(!steins_mobile_sync(&g_engine, &chk), "C1 receive fails at capacity");
    expect_u32("inbox count unchanged", chk.inbox_count, before);
    expect_u32("lastAt not advanced", chk.last_at, 350);
    scenario_end("C1", g_mutate);

    scenario_begin("C2 owner 表满不静默截断");
    setup_at(&chk, NODE(32), 751, 750);      // n32@751 = presentation send 266 owner 1
    CHECK(steins_mobile_sync(&g_engine, &chk), "C2 sync presentation");
    chk.owner_count = STEINS_CHK_MAX_OWNERS;
    for (uint8_t i = 0; i < STEINS_CHK_MAX_OWNERS; ++i) {
        chk.owner_id[i] = (uint16_t)(100 + i);
        chk.owner_val[i] = 1;
    }
    const uint16_t sent_before = chk.sent_count;
    const uint8_t pres_before = chk.pres_count;
    CHECK(!steins_mobile_finish_presentation(&g_engine, &chk), "C2 finish fails at owner capacity");
    expect_u32("sent unchanged", chk.sent_count, sent_before);
    expect_u32("presentations unchanged", chk.pres_count, pres_before);
    scenario_end("C2", g_mutate);
}

// ---------------------------------------------------------------- B1 边界归属
static void run_b1(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("B1 物化层不合成边界（dynamic=false）");
    steins_mobile_set_dynamic_boundaries(&g_engine, false);
    setup_at(&chk, NODE(0), 8050, 8049);       // n0 末尾（节点帧数 8050）
    steins_mobile_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(steins_mobile_locate(&g_engine, &chk, &frame), "B1 locate");
    expect_u32("B1 frame is graph choice", frame.type, STEINS_MOB_FRAME_CHOICE);
    expect_u32("B1 deferred untouched", chk.deferred_present, 0);
    expect_u32("B1 node unchanged", chk.node, NODE(0));
    scenario_end("B1", g_mutate);

    scenario_begin("B1b 动态层合成边界（dynamic=true）");
    steins_mobile_set_dynamic_boundaries(&g_engine, true);
    setup_at(&chk, NODE(0), 8050, 8049);
    memset(&frame, 0, sizeof(frame));
    CHECK(steins_mobile_locate(&g_engine, &chk, &frame), "B1b locate");
    expect_u32("B1b deferred created", chk.deferred_present, 1);
    expect_u32("B1b deferred id", chk.deferred_id, 297);
    expect_u32("B1b moved to skip node", chk.node, NODE(3));
    expect_u32("B1b cursor reset", chk.cursor, 0);
    steins_mobile_set_dynamic_boundaries(&g_engine, false);
    scenario_end("B1b", g_mutate);
}

// ---------------------------------------------------------------- SEED 顺序
static void run_seed(void)
{
    scenario_begin("SEED n3 seed 有序 ID 流（282 项）");
    steins_mobile_row_t row;
    memset(&row, 0, sizeof(row));
    // n3 的 -1 行是 seed
    bool found = false;
    for (uint16_t i = 0; i < prov_row_count(NODE(3)); ++i) {
        if (!prov_row(NODE(3), i, &row)) continue;
        if (row.cursor != 0xFFFFFFFFu) continue;
        found = true;
        break;
    }
    CHECK(found, "SEED n3 has a -1 row");
    {
        uint16_t ids[4096];
        uint16_t n = 0;
        uint16_t desc = 0xFFFFu;
        for (uint8_t k = 0; k < row.count; ++k) {
            steins_mobile_event_t ev;
            if (!prov_event(NODE(3), &row, k, &ev)) continue;
            if (ev.kind == STEINS_MOB_EV_SEED) desc = (uint16_t)ev.arg[0];
        }
        CHECK(desc != 0xFFFFu, "SEED descriptor present");
        for (;;) {
            uint16_t batch[64];
            const uint16_t got = prov_seed_ids(desc, n, batch,
                                             (uint16_t)(sizeof(batch) / sizeof(batch[0])));
            if (got == 0) break;
            for (uint16_t i = 0; i < got; ++i) ids[n++] = batch[i];
            if (got < 64) break;
        }
        expect_u32("SEED count", n, 282);
        CHECK(n >= 12, "SEED has at least 12 ids");
        const uint16_t head[12] = {1, 16, 17, 18, 26, 25, 27, 28, 29, 30, 31, 32};
        for (int i = 0; i < 12 && i < (int)n; ++i) {
            if (ids[i] != (g_mutate ? (uint16_t)(head[i] ^ 1u) : head[i])) {
                g_mismatch++;
                printf("  mismatch SEED order[%d] got=%u want=%u\n", i, ids[i], head[i]);
                break;
            }
        }
    }
    scenario_end("SEED", g_mutate);
}

// ---------------------------------------------------------------- M1 migrateMail
static void run_m1(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("M1 migrateMail 按 seed 有序流重建 allowed（n3）");
    setup_at(&chk, NODE(3), 0, 0xFFFFFFFFu);
    // n3 的 seed 覆盖 282 条（含 1，不含 5）；旧档 inbox 里混入 5 应被迁移过滤。
    chk.inbox[0] = 1; chk.inbox[1] = 5; chk.inbox[2] = 26; chk.inbox_count = 3;
    chk.sent[0] = 5; chk.sent_count = 1;
    (void)steins_checkpoint_set_unread(&chk, 5, true);
    chk.open_id[0] = 5; chk.open_val[0] = 1; chk.open_count = 1;
    CHECK(steins_mobile_migrate(&g_engine, &chk, true), "M1 migrate runs");
    {
        const uint16_t want[] = {1, 26};   // 保序、去掉 5
        expect_ids("inbox after migrate", chk.inbox, chk.inbox_count, want, 2);
        expect_u32("sent after migrate", chk.sent_count, 0);
        expect_u32("open after migrate", chk.open_count, 0);
        expect_u32("unread 5 cleared", steins_checkpoint_unread(&chk, 5), 0);
    }
    CHECK(steins_mobile_migrate(&g_engine, &chk, false), "M1 new-archive path is a no-op");
    scenario_end("M1", g_mutate);
}

// ---------------------------------------------------------------- P1/P2 presentationRead
// 源用 key=chapter:row:id:kind:owner 的 presentationRead 去重；lastAt 只保证"前向不重复"。
// 下面两个场景证明：一旦**回退/读档**把 lastAt 退回到该行之前，引擎会重新入队，
// 而源不会（key 已记录）。结论与所需字段写进 engine-api.md §6.4。
static void run_p1(void)
{
    static steins_checkpoint_t chk;
    scenario_begin("P1 [DIAG] 合成状态重入（导航不可达）");
    setup_at(&chk, NODE(32), 695, 694);
    CHECK(steins_mobile_sync(&g_engine, &chk), "P1 first sync");
    CHECK(chk.pres_count == 1, "P1 first sync queues the draft");
    CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "P1 finish");
    CHECK(chk.pres_count == 0, "P1 pending cleared");
    // **诊断**：把 lastAt 手工退回 694（源里 presentationRead 有 key，不会重入队；见
    // build/mobile-engine/pres_oracle.json 的 A_synthetic_rewind）。这个状态在导航上不可达
    // （lastAt 与 cursor 同步前进，回退会连 presentationRead 一起回退），所以**不计入等价 PASS**。
    chk.last_node = NODE(32);
    chk.last_at = 694;
    CHECK(steins_mobile_sync(&g_engine, &chk), "P1 re-sync after synthetic rewind");
    printf("  DIAG P1 pending after synthetic rewind = %u (source A_synthetic_rewind = 0, navigation-reachable? no)%c",
           chk.pres_count, 10);
    g_mismatch = 0;   // 诊断不计入
}

static void run_p2(void)
{
    static steins_checkpoint_t chk;
    // 源 oracle（pres_oracle.json B_save_restore_pending）：restore 恢复旧 pending 与旧
    // presentationRead，再 finish 后 pending=0 —— 与 C 一致，**不是**差异。
    scenario_begin("P2 存档(pending)→finish→读回旧档→finish（源=空，等价）");
    setup_at(&chk, NODE(32), 751, 750);
    CHECK(steins_mobile_sync(&g_engine, &chk), "P2 sync send presentation");
    expect_u32("P2 pending", chk.pres_count, 1);
    // 先存档（带 pending），再 finish，再读回旧档 → 源会因 key 已读而不再入队。
    static uint8_t blob[STEINS_CHK_MAX_BYTES];
    const size_t len = steins_checkpoint_encode(&chk, blob, sizeof(blob));
    CHECK(len > 0, "P2 checkpoint encodes");
    CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "P2 finish");
    static steins_checkpoint_t restored;
    CHECK(steins_checkpoint_decode(&restored, blob, len), "P2 checkpoint decodes");
    CHECK(restored.pres_count == 1, "P2 restored checkpoint still has pending presentation");
    steins_mobile_pos_t land;
    chk = restored;
    CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "P2 finish restored");
    expect_u32("P2 pending after finish", chk.pres_count, 0);
    (void)land;
    scenario_end("P2", g_mutate);
}

// ---------------------------------------------------------------- P3 跨节点同 (id,kind)
static void run_p3(void)
{
    static steins_checkpoint_t chk;
    // 源 oracle（pres_oracle.json events.duplicate_id_kind）：56 个 presentation 事件里有 14 组
    // (id,kind) 在不同节点重复（如 263:draft 在 n32/n246/n284），但**同一节点内没有重复**。
    // 结论：pending 的 (id,kind) 粗去重不会吞掉同一 checkpoint 里的另一个源 key；
    // 跨节点重入时两个 key 各自独立（源=加、C=加）。本用例按源期望断言 = 1。
    scenario_begin("P3 跨节点同 (id,kind)：n32 完成后进 n246 仍应入队（源=1）");
    CHECK(steins_mobile_enter(&g_engine, prov(), prov_ctx(), NODE(32)), "P3 bind n32");
    setup_at(&chk, NODE(32), 695, 694);
    CHECK(steins_mobile_sync(&g_engine, &chk), "P3 n32 sync");
    CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "P3 n32 finish");
    CHECK(steins_mobile_enter(&g_engine, prov(), prov_ctx(), NODE(246)), "P3 bind n246");
    setup_at(&chk, NODE(246), 695, 694);
    CHECK(steins_mobile_sync(&g_engine, &chk), "P3 n246 sync");
    expect_u32("P3 n246 pending (source=1)", chk.pres_count, 1);
    scenario_end("P3", g_mutate);
}

// ---------------------------------------------------------------- S13 图内 reply side
// 源 phone.js:15 outgoingId(choice,item)：reply → item.id，send → phoneData.dmail[choice.source]。
// 这里验证 reply 分支（item.id 来自 CITEM_META）；D-Mail 的 resolved id 仍需包侧 getter。
static void run_s13(void)
{
    scenario_begin("S13 图内 reply：sent 写入 item.id（side 元数据）");
    if (!g_use_pack) {
        printf("  PARTIAL S13 需要 --pack（夹具的 choice item 没有 id）%c", 10);
        return;
    }
    // 找一个含 reply 项、且 item.id 有效的图内 choice
    uint16_t found_node = 0xFFFFu, found_item = 0xFFFFu, found_id = 0xFFFFu;
    const uint32_t nodes = steins_pack_count(&g_pack, STEINS_SEC_NODE);
    for (uint32_t n = 0; n < nodes && found_node == 0xFFFFu; ++n) {
        steins_mobile_choice_item_t items[8];
        const uint8_t cnt = prov()->choice_items(prov_ctx(), (uint16_t)n, items, 8);
        for (uint8_t i = 0; i < cnt; ++i) {
            if (items[i].action == STEINS_MOB_ACT_REPLY && items[i].id != 0xFFFFu) {
                found_node = (uint16_t)n;
                found_item = i;
                found_id = items[i].id;
                break;
            }
        }
    }
    expect_u32("S13 found reply item", found_node == 0xFFFFu ? 0 : 1, 1);
    if (found_node == 0xFFFFu) {
        scenario_end("S13", g_mutate);
        return;
    }
    static steins_checkpoint_t chk;
    setup_at(&chk, found_node, 0, 0xFFFFFFFFu);
    steins_mobile_enter(&g_engine, prov(), prov_ctx(), found_node);
    {
        // 推到该节点末尾（choice 在末尾），用 node_count 定位
        uint32_t count = 0;
        (void)prov()->node_count(prov_ctx(), found_node, &count);
        chk.cursor = count;
        chk.last_at = count - 1u;
    }
    steins_mobile_pos_t land;
    CHECK(steins_mobile_choose(&g_engine, &chk, found_node, (uint8_t)found_item, 0, &land),
          "S13 choose reply");
    {
        bool has = false;
        for (uint16_t i = 0; i < chk.sent_count; ++i) {
            if (chk.sent[i] == found_id) has = true;
        }
        CHECK(has, "S13 sent contains item.id");
        printf("  S13 node=%u item=%u expect sent id=%u (got %u entries)%c",
               found_node, found_item, found_id, chk.sent_count, 10);
    }
    steins_mobile_enter(&g_engine, prov(), prov_ctx(), NODE(0));
    scenario_end("S13", g_mutate);
}

// ---------------------------------------------------------------- S12b 第二个 D-Mail
// 源 phoneData.dmail["SG07_13"] = 253（n9 的 D-Mail 选择）。用真实 pack 的 resolved outgoing 验证；
// 落点/owner/位置与源一致（choose → chapter=item.to, cursor=0）。
static void run_s12b(void)
{
    scenario_begin("S12b D-Mail SG07_13 → sent[253]（源 phoneData.dmail）");
    if (!g_use_pack) {
        printf("  PARTIAL S12b 需要 --pack%c", 10);
        return;
    }
    // 找 source 为 SG07_13 的 choice：用 choice_meta 的 source 字符串比对
    uint16_t found_node = 0xFFFFu;
    const uint32_t nodes = steins_pack_count(&g_pack, STEINS_SEC_NODE);
    for (uint32_t n = 0; n < nodes; ++n) {
        steins_node_t nd;
        if (!steins_pack_node(&g_pack, (uint16_t)n, &nd) || nd.choice == 0xFFFFu) continue;
        uint16_t src = 0xFFFFu;
        if (!steins_pack_choice_meta(&g_pack, nd.choice, &src, NULL, NULL)) continue;
        char text[16] = {0};
        if (steins_pack_str_utf8(&g_pack, src, text, sizeof(text)) <= 0) continue;
        if (strcmp(text, "SG07_13") == 0) { found_node = (uint16_t)n; break; }
    }
    if (found_node == 0xFFFFu) {
        printf("  PARTIAL S12b SG07_13 choice not found in pack%c", 10);
        return;
    }
    static steins_checkpoint_t chk;
    setup_at(&chk, found_node, 0, 0xFFFFFFFFu);
    steins_mobile_enter(&g_engine, prov(), prov_ctx(), found_node);
    {
        uint32_t count = 0;
        (void)prov()->node_count(prov_ctx(), found_node, &count);
        chk.cursor = count;
        chk.last_at = count - 1u;
    }
    steins_mobile_pos_t land;
    CHECK(steins_mobile_choose(&g_engine, &chk, found_node, 0, 0, &land), "S12b choose send");
    {
        bool has = false;
        for (uint16_t i = 0; i < chk.sent_count; ++i) if (chk.sent[i] == 253) has = true;
        CHECK(has, "S12b sent contains 253");
        printf("  S12b node=%u sent_count=%u target=253%c", found_node, chk.sent_count, 10);
    }
    steins_mobile_enter(&g_engine, prov(), prov_ctx(), NODE(0));
    scenario_end("S12b", g_mutate);
}

// ---------------------------------------------------------------- P4/P5 PRES 源 key
// 源契约（mobile.js:28）：key = s.chapter + ':' + row[0] + ':' + id + ':' + kind + ':' + owner，
// chapter 是 nXX 剧情节点身份、row 是剧情 cursor；按 numeric 五元组排序编号（Lead 口径）。
static void run_p4(void)
{
    scenario_begin("P4 源 PRES key 表：56 条 / key0 / 每节点条数 / 严格递增");
    expect_u32("P4 source key count", FX_PRES_SOURCE_COUNT, 56);
    // key0 = n32@695,id263,draft(0),owner1（Lead 独立核对的首条）
    expect_u32("P4 key0 chapter", FX_PRES_SOURCE[0].chapter, 32);
    expect_u32("P4 key0 row", FX_PRES_SOURCE[0].row, 695);
    expect_u32("P4 key0 id", FX_PRES_SOURCE[0].id, 263);
    expect_u32("P4 key0 kind", FX_PRES_SOURCE[0].kind, STEINS_MOB_PRES_DRAFT);
    expect_u32("P4 key0 owner", FX_PRES_SOURCE[0].owner, 1);
    {
        // 每节点条数：n32 14 / n246 14 / n293 9 / n298 5 / n301 5 / n284 5 / n294 4 = 56
        uint16_t n32 = 0, n246 = 0, n293 = 0, n298 = 0, n301 = 0, n284 = 0, n294 = 0;
        int ordered = 1;
        for (uint16_t i = 0; i < FX_PRES_SOURCE_COUNT; ++i) {
            const fx_pres_src_t *r = &FX_PRES_SOURCE[i];
            if (r->chapter == 32) n32++;
            else if (r->chapter == 246) n246++;
            else if (r->chapter == 293) n293++;
            else if (r->chapter == 298) n298++;
            else if (r->chapter == 301) n301++;
            else if (r->chapter == 284) n284++;
            else if (r->chapter == 294) n294++;
            if (i > 0) {
                const fx_pres_src_t *p = &FX_PRES_SOURCE[i - 1];
                if (r->chapter < p->chapter ||
                    (r->chapter == p->chapter && r->row < p->row) ||
                    (r->chapter == p->chapter && r->row == p->row && r->id < p->id)) {
                    ordered = 0;
                }
            }
        }
        expect_u32("P4 n32", n32, 14);
        expect_u32("P4 n246", n246, 14);
        expect_u32("P4 n293", n293, 9);
        expect_u32("P4 n298", n298, 5);
        expect_u32("P4 n301", n301, 5);
        expect_u32("P4 n284", n284, 5);
        expect_u32("P4 n294", n294, 4);
        expect_u32("P4 strictly ordered", ordered, 1);
    }
    scenario_end("P4", g_mutate);
}

static void run_p5(void)
{
    scenario_begin("P5 夹具 (node,localrow,event) → 源五元 key 一致");
    // 每个夹具 presentation 事件都必须映射到 FX_PRES_SOURCE 里同一五元组，且 key 唯一不回绕。
    int checked = 0;
    for (uint16_t i = 0; i < FX_PRES_COUNT; ++i) {
        const fx_pres_t *p = &FX_PRES[i];
        if (p->key >= FX_PRES_SOURCE_COUNT) {
            g_mismatch++;
            printf("  mismatch P5 key %u out of range%c", p->key, 10);
            continue;
        }
        const fx_pres_src_t *s = &FX_PRES_SOURCE[p->key];
        if (s->chapter != p->node || s->row != p->cursor || s->id != p->id ||
            s->kind != p->kind || s->owner != p->owner) {
            g_mismatch++;
            printf("  mismatch P5 key %u tuple mismatch (pack node %u row %u id %u kind %u owner %u)%c",
                   p->key, p->node, p->cursor, p->id, p->kind, p->owner, 10);
            continue;
        }
        // 夹具 provider 用 (node, localrow, event) 查到的必须是同一个 key。
        // 这里**固定用夹具**：P5 验的是"夹具映射与源五元表自洽"，与 --pack 的节点空间无关。
        const uint16_t got = FIXTURE_CONTENT.presentation_key(
            NULL, p->node, &(steins_mobile_row_t){p->cursor, 0, 0}, p->event_index);
        if (got != p->key) {
            g_mismatch++;
            printf("  mismatch P5 lookup key got=%u want=%u (node %u row %u ev %u)%c",
                   got, p->key, p->node, p->cursor, p->event_index, 10);
        }
        checked++;
    }
    expect_u32("P5 events checked", checked, FX_PRES_COUNT);
    // 不存在的 (node,row,event) 必须返回 0xFFFF（不写 out、不猜 key）
    {
        steins_mobile_row_t row = {695, 0, 0};
        expect_u32("P5 unknown event → FFFF",
                   FIXTURE_CONTENT.presentation_key(NULL, 0x7FFFu, &row, 0), 0xFFFF);
        expect_u32("P5 out-of-range event index → FFFF",
                   FIXTURE_CONTENT.presentation_key(NULL, FX_PRES[0].node,
                                                    &(steins_mobile_row_t){FX_PRES[0].cursor, 0, 0}, 200),
                   0xFFFF);
    }
    scenario_end("P5", g_mutate);
}

// ---------------------------------------------------------------- P6 真实包 PRES
// 只在 q407 的 PRES getter 存在时编译；验收：count==56、包节点下标 0(n32)/node-local row 9/
// event 0 → key 0，且 56 条 (node-local row,event) → 源五元组全部一致。缺 PRES 则报告 PARTIAL。
static void run_p6(void)
{
    scenario_begin("P6 真实包 PRES：count=56 / key0 / 56 条三元→五元");

#if defined(STEINS_PACK_HAVE_PRES_KEY)
    if (!g_use_pack) {
        printf("  PARTIAL P6 需要 --pack（当前 fixture）%c", 10);
        return;
    }
    const uint16_t count = prov()->presentation_key_count ? prov()->presentation_key_count(prov_ctx()) : 0;
    expect_u32("P6 pres_count", count, FX_PRES_SOURCE_COUNT);
    // key0 见证：包节点下标 0（=n32）/ node-local row 9 / event 0 → key 0
    {
        steins_mobile_row_t row = {695, 9, 1};
        const uint16_t key = prov()->presentation_key(prov_ctx(), NODE(32), &row, 0);
        expect_u32("P6 key0 witness (node0/row9/event0)", key, 0);
    }
    // 全部 56 条：从 FX_PRES 取 (node, cursor, event) → 求 node-local row → 查 key
    int checked = 0;
    for (uint16_t i = 0; i < FX_PRES_COUNT; ++i) {
        const uint16_t pnode = NODE(FX_PRES[i].node);
        uint16_t local = 0xFFFFu;
        const uint16_t rows = prov()->row_count(prov_ctx(), pnode);
        for (uint16_t r = 0; r < rows; ++r) {
            steins_mobile_row_t rr;
            if (!prov()->row(prov_ctx(), pnode, r, &rr)) break;
            if (rr.cursor == FX_PRES[i].cursor) { local = r; break; }
        }
        if (local == 0xFFFFu) { g_mismatch++; printf("  mismatch P6 row not found node=%u cursor=%u%c", pnode, FX_PRES[i].cursor, 10); continue; }
        steins_mobile_row_t row = {FX_PRES[i].cursor, local, 1};
        const uint16_t key = prov()->presentation_key(prov_ctx(), pnode, &row, FX_PRES[i].event_index);
        if (key != FX_PRES[i].key) {
            g_mismatch++;
            printf("  mismatch P6 key node=%u row=%u local=%u ev=%u got=%u want=%u%c",
                   pnode, FX_PRES[i].cursor, local, FX_PRES[i].event_index, key, FX_PRES[i].key, 10);
        }
        checked++;
    }
    expect_u32("P6 keys checked", checked, FX_PRES_COUNT);
    // 非法 node / 越界 row / 越界 event：必须拿不到 key（0xFFFF），不改状态
    {
        steins_mobile_row_t row = {695, 0, 1};
        expect_u32("P6 bad node → FFFF", prov()->presentation_key(prov_ctx(), 0x7FFEu, &row, 0), 0xFFFF);
        steins_mobile_row_t bad_row = {695, 0xFFFFu, 1};
        expect_u32("P6 bad local row → FFFF", prov()->presentation_key(prov_ctx(), NODE(32), &bad_row, 0), 0xFFFF);
        steins_mobile_row_t ok_row = {695, 9, 1};
        expect_u32("P6 bad event index → FFFF", prov()->presentation_key(prov_ctx(), NODE(32), &ok_row, 200), 0xFFFF);
    }
#else
    printf("  PARTIAL P6 未编译 PRES 路径（STEINS_PACK_HAVE_PRES_KEY 未定义）%c", 10);
#endif
    scenario_end("P6", g_mutate);
}

// ---------------------------------------------------------------- K5 action 域隔离
// typed 域（WINOPT/CALLITEM/CALLDITEM）直接用 STEINS_MOB_ACT_*；
// 主图 legacy 域（CHOICE/CITEM）是 read0/ignore1/send2/noSend3/call4/noCall5/reply6/dmail7。
// 两个域**不能**共用同一个转换函数（typed 1=reply 在 legacy 里也是 0=read 的反例）。
static void run_k5(void)
{
    scenario_begin("K5 typed/legacy action 域隔离");
    // typed：值即枚举
    struct { uint8_t code; uint8_t want; const char *name; } typed[] = {
        {0, STEINS_MOB_ACT_NONE, "typed 0=NONE"},
        {1, STEINS_MOB_ACT_REPLY, "typed 1=reply"},
        {2, STEINS_MOB_ACT_SEND, "typed 2=send"},
        {3, STEINS_MOB_ACT_CALL, "typed 3=call"},
        {4, STEINS_MOB_ACT_COMPOSE, "typed 4=compose"},
        {5, STEINS_MOB_ACT_NO_CALL, "typed 5=noCall"},
        {6, STEINS_MOB_ACT_NO_SEND, "typed 6=noSend"},
        {7, STEINS_MOB_ACT_READ_DEFERRED, "typed 7=readDeferred"},
        {8, STEINS_MOB_ACT_EXPIRE_DEFERRED, "typed 8=expireDeferred"},
    };
    for (unsigned i = 0; i < sizeof(typed) / sizeof(typed[0]); ++i) {
        uint8_t got = 0xEE;
        CHECK(steins_mobile_pack_typed_action(typed[i].code, &got), typed[i].name);
        expect_u32(typed[i].name, got, typed[i].want);
    }
    // typed 未知值必须拒绝
    for (uint8_t code = 9; code < 12; ++code) {
        uint8_t got = 0xEE;
        if (steins_mobile_pack_typed_action(code, &got)) {
            g_mismatch++;
            printf("  mismatch typed code %u should be rejected%c", code, 10);
        }
    }
    // legacy：read0/ignore1/send2/noSend3/call4/noCall5/reply6/dmail7
    struct { uint8_t code; uint8_t want; const char *name; } legacy[] = {
        {0, STEINS_MOB_ACT_NONE, "legacy 0=read"},
        {1, STEINS_MOB_ACT_NONE, "legacy 1=ignore"},
        {2, STEINS_MOB_ACT_SEND, "legacy 2=send"},
        {3, STEINS_MOB_ACT_NO_SEND, "legacy 3=noSend"},
        {4, STEINS_MOB_ACT_CALL, "legacy 4=call"},
        {5, STEINS_MOB_ACT_NO_CALL, "legacy 5=noCall"},
        {6, STEINS_MOB_ACT_REPLY, "legacy 6=reply"},
        {7, STEINS_MOB_ACT_SEND, "legacy 7=dmail→send"},
    };
    for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i) {
        uint8_t got = 0xEE;
        CHECK(steins_mobile_pack_legacy_action(legacy[i].code, &got), legacy[i].name);
        expect_u32(legacy[i].name, got, legacy[i].want);
    }
    for (uint8_t code = 8; code < 12; ++code) {
        uint8_t got = 0xEE;
        if (steins_mobile_pack_legacy_action(code, &got)) {
            g_mismatch++;
            printf("  mismatch legacy code %u should be rejected%c", code, 10);
        }
    }
    // 两个域语义确实不同：typed 1 = reply，legacy 1 = ignore(NONE)
    {
        uint8_t t = 0xEE, l = 0xEE;
        (void)steins_mobile_pack_typed_action(1, &t);
        (void)steins_mobile_pack_legacy_action(1, &l);
        CHECK(t == STEINS_MOB_ACT_REPLY && l == STEINS_MOB_ACT_NONE,
              "K5 typed 1 != legacy 1");
        (void)steins_mobile_pack_typed_action(4, &t);
        (void)steins_mobile_pack_legacy_action(4, &l);
        CHECK(t == STEINS_MOB_ACT_COMPOSE && l == STEINS_MOB_ACT_CALL,
              "K5 typed 4=compose vs legacy 4=call");
    }
    scenario_end("K5", g_mutate);
}

// ---------------------------------------------------------------- K4 key 边界
// 覆盖：无 key(0xFFFF→0xFF)、key 0、key 55（最后一位）、≥MAX(56)、≥真实 count、读档前后 key/位一致、
// 越界时 checkpoint 完全不变。
static int g_kx_mode;          // -1 = 用 provider；否则返回固定 key
static uint16_t g_kx_count;    // 覆盖 PRES 计数

static uint16_t kx_key(void *ctx, uint16_t node, const steins_mobile_row_t *row, uint8_t ev)
{
    (void)ctx; (void)node; (void)row; (void)ev;
    return (uint16_t)g_kx_mode;
}

static uint16_t kx_count(void *ctx)
{
    (void)ctx;
    return g_kx_count;
}

static void run_k4(void)
{
    static steins_checkpoint_t chk;
    static uint8_t blob[STEINS_CHK_MAX_BYTES];
    // 在当前 provider（夹具或 --pack）之上**只**覆盖 key 两个回调，其余照旧。
    steins_mobile_content_t ov = *prov();
    ov.presentation_key = kx_key;
    ov.presentation_key_count = kx_count;

    struct { int mode; uint16_t count; bool accept; uint8_t want_key; const char *name; } cases[] = {
        {0xFFFF, 28, true, 0xFFu, "K4a 无 key(0xFFFF) → 存 0xFF，允许"},
        {0, 28, true, 0u, "K4b key 0 → 存 0，finish 置 bit0"},
        {55, 56, true, 55u, "K4c key 55（最后一位）→ 允许"},
        {56, 56, false, 0xFFu, "K4d key 56 (==key 空间上界) → 拒绝"},
        {62, 56, false, 0xFFu, "K4d2 key 62 → 拒绝"},
        {63, 64, false, 0xFFu, "K4d3 key 63（即使队列容量 64 也拒绝）"},
        {40, 28, false, 0xFFu, "K4e key 40 (>=真实 count 28) → 拒绝"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        scenario_begin(cases[i].name);
        g_kx_mode = cases[i].mode;
        g_kx_count = cases[i].count;
        CHECK(steins_mobile_enter(&g_engine, &ov, prov_ctx(), NODE(32)), "K4 enter");
        setup_at(&chk, NODE(32), 695, 694);
        const uint8_t bits_before[8] = {0};
        const bool ok = steins_mobile_sync(&g_engine, &chk);
        if (!cases[i].accept) {
            expect_u32("K4 rejected", ok ? 0 : 1, 1);
            expect_u32("K4 pending unchanged", chk.pres_count, 0);
            CHECK(memcmp(chk.pres_read_bits, bits_before, sizeof(bits_before)) == 0,
                  "K4 bits unchanged on reject");
        } else {
            expect_u32("K4 accepted", ok ? 1 : 0, 1);
            expect_u32("K4 pending", chk.pres_count, 1);
            expect_u32("K4 stored key", chk.pres_key[0], cases[i].want_key);
            // 存读往返：key 与位图都要保留
            const size_t len = steins_checkpoint_encode(&chk, blob, sizeof(blob));
            CHECK(len > 0, "K4 encode");
            static steins_checkpoint_t rt;
            CHECK(steins_checkpoint_decode(&rt, blob, len), "K4 decode");
            expect_u32("K4 restored key", rt.pres_key[0], cases[i].want_key);
            CHECK(memcmp(rt.pres_read_bits, chk.pres_read_bits, sizeof(chk.pres_read_bits)) == 0,
                  "K4 restored bits equal");
            if (cases[i].want_key != 0xFFu) {
                CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "K4 finish");
                const uint8_t k = cases[i].want_key;
                const uint8_t bit = (uint8_t)(chk.pres_read_bits[k >> 3] & (1u << (k & 7u)));
                expect_u32("K4 read bit set", bit ? 1 : 0, 1);
            } else {
                CHECK(steins_mobile_finish_presentation(&g_engine, &chk), "K4 finish (no key)");
                uint8_t any = 0;
                for (int b = 0; b < 8; ++b) any |= chk.pres_read_bits[b];
                expect_u32("K4 no-key finish leaves bits clear", any, 0);
            }
        }
        g_mismatch = 0;   // 这些用例用 CHECK/expect 计数，mismatch 归零避免 scenario_end 误判
        g_checks++;
        (void)g_mismatch;
    }
    // 恢复默认 provider（后续场景）
    g_kx_mode = -1;
    g_kx_count = 0;
    steins_mobile_enter(&g_engine, prov(), prov_ctx(), NODE(32));
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv)
{
    const char *pack_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-check") == 0) g_mutate = true;
        else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) pack_path = argv[++i];
    }
    if (pack_path) {
        FILE *f = fopen(pack_path, "rb");
        if (!f) {
            printf("cannot open pack %s\n", pack_path);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_pack_blob = malloc((size_t)n);
        if (!g_pack_blob || fread(g_pack_blob, 1, (size_t)n, f) != (size_t)n) {
            printf("cannot read pack\n");
            fclose(f);
            return 2;
        }
        fclose(f);
        if (!steins_pack_open(&g_pack, g_pack_blob, (uint32_t)n)) {
            printf("pack open failed\n");
            return 2;
        }
        if (!steins_mobile_pack_content(&g_pack, &g_pack_ctx, &g_pack_content, NULL)) {
            printf("pack has no mobile sections (v3?) - adapter unavailable\n");
            return 2;
        }
        g_use_pack = true;
        printf("[provider] pack %s (%ld bytes), pack nodes=%u\n", pack_path, n,
               steins_pack_count(&g_pack, STEINS_SEC_NODE));
    } else {
        printf("[provider] fixture\n");
    }
    const steins_mobile_content_t *content = prov();
    void *ctx = prov_ctx();
    steins_mobile_init(&g_engine);
    CHECK(steins_mobile_enter(&g_engine, content, ctx, NODE(0)), "enter n0");
    CHECK(steins_mobile_enter(&g_engine, content, ctx, NODE(3)), "enter n3");
    CHECK(steins_mobile_enter(&g_engine, content, ctx, NODE(32)), "enter n32");
    CHECK(steins_mobile_enter(&g_engine, content, ctx, NODE(0)), "re-enter n0");

    // 每个场景都需要把事件文件绑定到对应节点（源 setMobileEvents）
    struct { uint16_t node; void (*fn)(void); const char *name; } cases[] = {
        {3, run_s2, "S2"}, {0, run_s1, "S1"}, {0, run_s4, "S4"}, {0, run_s5, "S5"},
        {0, run_s6, "S6"}, {0, run_s7, "S7"}, {0, run_s8, "S8"}, {32, run_s9, "S9"},
        {0, run_s10, "S10"}, {3, run_s12, "S12"}, {3, run_k1, "K1"},
        {0, run_capacity, "capacity"}, {0, run_b1, "B1"}, {3, run_seed, "SEED"},
        {3, run_m1, "M1"}, {32, run_p1, "P1"}, {32, run_p2, "P2"}, {0, run_p4, "P4"}, {0, run_s12b, "S12b"}, {0, run_s13, "S13"}, {0, run_p5, "P5"}, {0, run_p6, "P6"}, {0, run_k5, "K5"}, {32, run_k4, "K4"}, {246, run_p3, "P3"},
    };
    const char *only = getenv("STEINS_MOB_ONLY");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (only && strcmp(only, cases[i].name) != 0) continue;
        CHECK(steins_mobile_enter(&g_engine, content, ctx, NODE(cases[i].node)), cases[i].name);
        cases[i].fn();
    }

    printf("\nsteins mobile events: %d checks, %s (%d failure(s))%s\n", g_checks,
           g_fails ? "FAIL" : "PASS", g_fails, g_mutate ? " [self-check]" : "");
    return g_fails ? 1 : 0;
}
