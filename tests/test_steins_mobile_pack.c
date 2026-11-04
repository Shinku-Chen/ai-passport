// tests/test_steins_mobile_pack.c —— v4 手机内容包的真实 C 读回回归。
//
// 用一个**真实产出**的候选包(由 tools/steins_mobile_pack.py 生成)驱动生产
// 访问器,逐项核对:646 索引 / 609 非空正文 / 163 个 type3 后继 / 2001 事件行 /
// 4072 事件(按 kind 计数) / 371 通话帧 / 21 通话站点 / 窗口与 keyphone /
// 有序 seed 描述符 / call 描述符(站点字符串 + kind + 有序 items + waiting) /
// presentation 显式 enum(draft/send 必须不同)。
//
// 用法: test_steins_mobile_pack <v4 候选包> [v3 官方包]
//   只给 v4 候选:只跑新数据检查。
//   再给 v3 官方包:额外确认旧包仍能被同一 reader 打开(版本兼容)。
#include "steins_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);           \
            fprintf(stderr, "\n");                  \
            ++g_failures;                           \
        }                                           \
    } while (0)

static uint8_t *load(const char *path, uint32_t *size_out)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fh);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(fh);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, fh) != (size_t)size) {
        free(data);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    *size_out = (uint32_t)size;
    return data;
}

static void check_mails(const steins_pack_t *pack)
{
    const uint16_t count = steins_pack_mail_count(pack);
    CHECK(count == 646, "mail count %u != 646", (unsigned)count);
    unsigned nonempty = 0;
    unsigned type3 = 0;
    unsigned succ_total = 0;
    char body[8192];
    for (uint16_t id = 0; id < count; ++id) {
        steins_mail_t mail;
        CHECK(steins_pack_mail(pack, id, &mail), "mail %u unreadable", (unsigned)id);
        if (mail.body != 0xFFFF) {
            const int n = steins_pack_str_utf8(pack, mail.body, body, sizeof(body));
            CHECK(n >= 0, "mail %u body does not transcode (%d)", (unsigned)id, n);
            if (n > 0) ++nonempty;
        }
        uint16_t succ[STEINS_MAIL_SUCC_MAX];
        const uint8_t all = steins_pack_mail_succ(pack, &mail, succ, STEINS_MAIL_SUCC_MAX, false);
        CHECK(all == mail.succ_count, "mail %u succ %u != %u", (unsigned)id,
              (unsigned)all, (unsigned)mail.succ_count);
        succ_total += all;
        for (uint8_t i = 0; i < all; ++i) {
            steins_mail_t target;
            CHECK(steins_pack_mail(pack, succ[i], &target), "mail %u succ %u unreadable",
                  (unsigned)id, (unsigned)succ[i]);
        }
        const uint8_t t3 = steins_pack_mail_succ(pack, &mail, succ, STEINS_MAIL_SUCC_MAX, true);
        type3 += t3;
        uint16_t lo = 0, hi = 0;
        CHECK(steins_pack_mail_stamp(pack, id, &lo, &hi), "mail %u stamp missing", (unsigned)id);
    }
    CHECK(nonempty == 609, "non-empty bodies %u != 609", nonempty);
    CHECK(type3 == 163, "type3 successors %u != 163", type3);
    printf("  mails: %u, non-empty %u, successors %u, type3 %u\n", (unsigned)count,
           nonempty, succ_total, type3);
}

static void check_events(const steins_pack_t *pack)
{
    const uint16_t rows = steins_pack_mrow_count(pack);
    CHECK(rows == 2001, "event rows %u != 2001", (unsigned)rows);
    // 每个节点只拿自己那段行区间:同一 file 被多个节点共用,不能整段给节点。
    unsigned per_node_rows = 0;
    unsigned nodes_with_events = 0;
    unsigned neg_cursor = 0;
    for (uint16_t n = 0; n < (uint16_t)steins_pack_count(pack, STEINS_SEC_NODE); ++n) {
        uint16_t first = 0, count = 0;
        if (!steins_pack_mrow_range(pack, n, &first, &count)) continue;
        ++nodes_with_events;
        per_node_rows += count;
        steins_mrow_t first_row;
        CHECK(steins_pack_mrow(pack, first, &first_row), "node %u first row unreadable",
              (unsigned)n);
        if (first_row.cursor == -1) ++neg_cursor;
        CHECK(first_row.cursor <= 1000000, "node %u cursor %d looks unsigned-swapped",
              (unsigned)n, (int)first_row.cursor);
    }
    CHECK(per_node_rows == rows, "per-node rows %u != total %u", per_node_rows,
          (unsigned)rows);
    CHECK(nodes_with_events >= 20, "only %u nodes have event ranges", nodes_with_events);
    CHECK(neg_cursor >= 20, "only %u nodes start with a -1 (seed) row", neg_cursor);
    printf("  node ranges: %u nodes, %u rows, %u start at cursor -1\n", nodes_with_events,
           per_node_rows, neg_cursor);
    unsigned events = 0;
    unsigned kinds[STEINS_EV_KIND_COUNT];
    memset(kinds, 0, sizeof(kinds));
    unsigned seed_multi = 0;
    unsigned call_events = 0;
    unsigned pres_draft = 0;
    unsigned pres_send = 0;
    for (uint16_t r = 0; r < rows; ++r) {
        steins_mrow_t row;
        CHECK(steins_pack_mrow(pack, r, &row), "row %u unreadable", (unsigned)r);
        for (uint8_t k = 0; k < row.event_count; ++k) {
            steins_mevent_t ev;
            CHECK(steins_pack_mevent(pack, &row, k, &ev), "row %u event %u unreadable",
                  (unsigned)r, (unsigned)k);
            if (ev.kind < STEINS_EV_KIND_COUNT) ++kinds[ev.kind];
            ++events;
            if (ev.kind == STEINS_EV_SEED && ev.argc == 1) {
                // arg0 指向 SEED 描述符:first/count 必须落在 SEED_ID 内
                const uint32_t off = pack->sec_off[STEINS_SEC_SEED] + (uint32_t)ev.arg[0] * 4u;
                if (pack->sec_off[STEINS_SEC_SEED] && off + 4u <= pack->size) {
                    const uint16_t first = steins_rd16(pack->data + off);
                    const uint16_t cnt = steins_rd16(pack->data + off + 2);
                    CHECK(first + cnt <= pack->sec_count[STEINS_SEC_SEED_ID],
                          "seed desc %u id range out of bounds", (unsigned)ev.arg[0]);
                    if (cnt > 1) ++seed_multi;
                }
            }
            if (ev.kind == STEINS_EV_CALL) {
                ++call_events;
                CHECK(ev.argc == 1, "call event argc %u != 1 (descriptor ref)", ev.argc);
                const uint32_t off = pack->sec_off[STEINS_SEC_CALLDESC] +
                                     (uint32_t)ev.arg[0] * 8u;
                CHECK(off + 8u <= pack->size, "call desc %u out of bounds", (unsigned)ev.arg[0]);
                if (off + 8u <= pack->size) {
                    const uint16_t site_i = steins_rd16(pack->data + off);
                    const uint16_t kind_i = steins_rd16(pack->data + off + 2);
                    const uint16_t first_item = steins_rd16(pack->data + off + 4);
                    const uint8_t item_count = pack->data[off + 6];
                    char site[64];
                    char kind[32];
                    CHECK(steins_pack_str_utf8(pack, site_i, site, sizeof(site)) > 0,
                          "call desc %u site string missing", (unsigned)ev.arg[0]);
                    CHECK(steins_pack_str_utf8(pack, kind_i, kind, sizeof(kind)) > 0,
                          "call desc %u kind string missing", (unsigned)ev.arg[0]);
                    CHECK(item_count > 0, "call desc %u has no items", (unsigned)ev.arg[0]);
                    CHECK(first_item + item_count <= pack->sec_count[STEINS_SEC_CALLDITEM],
                          "call desc %u items out of bounds", (unsigned)ev.arg[0]);
                    CHECK(site[0] != 0, "call desc %u site string is empty", (unsigned)ev.arg[0]);
                }
            }
            if (ev.kind == STEINS_EV_PRESENTATION) {
                CHECK(ev.argc == 3, "presentation argc %u != 3", ev.argc);
                if (ev.arg[1] == STEINS_PK_DRAFT) ++pres_draft;
                if (ev.arg[1] == STEINS_PK_SEND) ++pres_send;
            }
        }
    }
    CHECK(events == 4072, "event count %u != 4072", events);
    CHECK(kinds[STEINS_EV_SEED] == 74, "seed events %u != 74",
          kinds[STEINS_EV_SEED]);
    CHECK(kinds[STEINS_EV_RECEIVE] == 1036, "receive %u != 1036", kinds[STEINS_EV_RECEIVE]);
    CHECK(kinds[STEINS_EV_ERASE] == 1089, "erase %u != 1089", kinds[STEINS_EV_ERASE]);
    CHECK(kinds[STEINS_EV_EXPIRE] == 1185, "expire %u != 1185", kinds[STEINS_EV_EXPIRE]);
    CHECK(kinds[STEINS_EV_READBIT] == 147, "readBit %u != 147", kinds[STEINS_EV_READBIT]);
    CHECK(kinds[STEINS_EV_REPLYBIT] == 14, "replyBit %u != 14", kinds[STEINS_EV_REPLYBIT]);
    CHECK(kinds[STEINS_EV_QUEUE] == 151, "queue %u != 151", kinds[STEINS_EV_QUEUE]);
    CHECK(kinds[STEINS_EV_DISPATCH] == 217, "dispatch %u != 217", kinds[STEINS_EV_DISPATCH]);
    CHECK(kinds[STEINS_EV_RULE] == 8, "rule %u != 8", kinds[STEINS_EV_RULE]);
    CHECK(kinds[STEINS_EV_CALL] == 95, "call %u != 95", kinds[STEINS_EV_CALL]);
    CHECK(kinds[STEINS_EV_PRESENTATION] == 56, "presentation %u != 56",
          kinds[STEINS_EV_PRESENTATION]);
    CHECK(seed_multi >= 20, "only %u seed descriptors carry more than one id", seed_multi);
    CHECK(call_events == 95, "call events %u", call_events);
    CHECK(pres_draft == 14 && pres_send == 18, "presentation draft/send %u/%u != 14/18",
          pres_draft, pres_send);
    printf("  events: %u rows, %u events, seed with >1 id: %u, presentations draft/send %u/%u\n",
           (unsigned)rows, events, seed_multi, pres_draft, pres_send);
}

static void check_call_dir(const steins_pack_t *pack)
{
    const uint16_t sites = steins_pack_call_site_count(pack);
    CHECK(sites == 21, "call sites %u != 21", (unsigned)sites);
    unsigned items = 0;
    for (uint16_t i = 0; i < sites; ++i) {
        steins_callsite_t site;
        CHECK(steins_pack_call_site(pack, i, &site), "call site %u unreadable", (unsigned)i);
        for (uint8_t k = 0; k < site.item_count; ++k) {
            steins_callitem_t item;
            CHECK(steins_pack_call_item(pack, (uint16_t)(site.first_item + k), &item),
                  "call site %u item %u unreadable", (unsigned)i, (unsigned)k);
            ++items;
        }
    }
    const uint16_t frames = steins_pack_cframe_count(pack);
    CHECK(frames == 371, "call frames %u != 371", (unsigned)frames);
    unsigned nonempty = 0;
    char text[2048];
    for (uint16_t i = 0; i < frames; ++i) {
        steins_cframe_t frame;
        CHECK(steins_pack_cframe(pack, i, &frame), "frame %u unreadable", (unsigned)i);
        const int n = steins_pack_str_utf8(pack, frame.text, text, sizeof(text));
        CHECK(n >= 0, "frame %u text does not transcode", (unsigned)i);
        if (n > 0) ++nonempty;
    }
    printf("  call: %u sites, %u items, %u frames (%u non-empty)\n", (unsigned)sites, items,
           (unsigned)frames, nonempty);
}

static void check_dirs(const steins_pack_t *pack)
{
    unsigned windows = 0;
    unsigned opts = 0;
    for (uint16_t node = 0; node < steins_pack_count(pack, STEINS_SEC_NODE); ++node) {
        steins_window_t win;
        if (!steins_pack_mobile_window(pack, node, &win)) continue;
        ++windows;
        CHECK(win.id < 646, "window node %u mail id %u out of range", (unsigned)node,
              (unsigned)win.id);
        for (uint8_t k = 0; k < win.opt_count; ++k) {
            steins_winopt_t opt;
            CHECK(steins_pack_window_opt(pack, (uint16_t)(win.first_opt + k), &opt),
                  "window node %u opt %u unreadable", (unsigned)node, (unsigned)k);
            CHECK(opt.to < steins_pack_count(pack, STEINS_SEC_NODE),
                  "window node %u opt %u target %u out of range", (unsigned)node,
                  (unsigned)k, (unsigned)opt.to);
            ++opts;
        }
        steins_keyphone_t kp;
        if (steins_pack_keyphone(pack, node, &kp)) {
            CHECK(kp.go < steins_pack_count(pack, STEINS_SEC_NODE), "keyphone go out of range");
            CHECK(kp.skip < steins_pack_count(pack, STEINS_SEC_NODE),
                  "keyphone skip out of range");
        }
    }
    CHECK(windows == 21, "windows %u != 21", windows);
    // n0 的窗口(邮件 297)有 2 个选项;key phone 窗口是另一组节点
    steins_window_t n0;
    if (steins_pack_mobile_window(pack, 73, &n0)) {
        printf("  window(node 73) mail=%u limit=%u opts=%u\n", (unsigned)n0.id,
               (unsigned)n0.limit, (unsigned)n0.opt_count);
    }
    printf("  windows %u, window options %u\n", windows, opts);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <v4 candidate pack> [v3 official pack]\n", argv[0]);
        return 2;
    }
    uint32_t size = 0;
    uint8_t *data = load(argv[1], &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    steins_pack_t pack;
    if (!steins_pack_open(&pack, data, size)) {
        fprintf(stderr, "candidate pack rejected by steins_pack_open\n");
        free(data);
        return 2;
    }
    printf("v4 candidate: %u bytes, %u mail, %u rows\n", (unsigned)size,
           (unsigned)steins_pack_mail_count(&pack), (unsigned)steins_pack_mrow_count(&pack));
    check_mails(&pack);
    check_events(&pack);
    check_call_dir(&pack);
    check_dirs(&pack);
    free(data);

    if (argc > 2) {
        uint32_t size3 = 0;
        uint8_t *data3 = load(argv[2], &size3);
        if (data3) {
            steins_pack_t p3;
            const bool ok = steins_pack_open(&p3, data3, size3);
            CHECK(ok, "v3 official pack no longer opens");
            if (ok) {
                CHECK(steins_pack_mail_count(&p3) == 0,
                      "v3 pack unexpectedly exposes mobile data");
                printf("  v3 pack still readable (%u nodes), mobile getters return 0/false\n",
                       (unsigned)steins_pack_count(&p3, STEINS_SEC_NODE));
            }
            free(data3);
        }
    }

    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
