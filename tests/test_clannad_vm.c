// tests/test_clannad_vm.c —— 生产 VM/存档/通关记录/文本分页的宿主回归。
//
// 语料来自 tools/clannad_audit.py 的独立解释器（tests/fixtures/clannad_routes.h），
// 不是打包器导入的模块，也不是旧构建报告。
#include "clannad_text.h"
#include "clannad_vm.h"
#include "fixtures/clannad_paths.h"
#include "fixtures/clannad_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define EXPECT(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static uint8_t raw[32768];
static uint8_t pack_data[8u * 1024u * 1024u];
static clannad_vm_t vm;

typedef struct {
    uint16_t chapter, root, cursor, background;
    uint16_t bodies[3];
    uint8_t body_count, stack_depth, mode;
    uint16_t var_count;
    uint32_t var_hash;
    uint32_t counter_hash;
} fingerprint_t;

static fingerprint_t fingerprint(const clannad_vm_t *v)
{
    fingerprint_t f;
    memset(&f, 0, sizeof(f));
    f.chapter = v->chapter;
    f.root = v->root;
    f.cursor = v->cursor;
    f.background = v->background;
    memcpy(f.bodies, v->bodies, sizeof(f.bodies));
    f.body_count = v->body_count;
    f.stack_depth = v->stack_depth;
    f.mode = v->mode;
    f.var_count = v->var_count;
    // 顺序无关：每一项单独做 FNV 后累加，避免哈希表插入顺序影响比较。
    uint32_t h = 0;
    for (int i = 0; i < CLAN_VM_VAR_SLOTS; ++i) {
        if (!v->vars[i].key) continue;
        uint32_t item = 2166136261u;
        item = (item ^ v->vars[i].key) * 16777619u;
        item = (item ^ (uint32_t)v->vars[i].value) * 16777619u;
        item = (item ^ v->vars[i].type) * 16777619u;
        item = (item ^ v->vars[i].str_len) * 16777619u;
        if (v->vars[i].type == 1) {
            for (uint16_t k = 0; k < v->vars[i].str_len; ++k) {
                item = (item ^ (uint8_t)v->strings[v->vars[i].str_off + k]) * 16777619u;
            }
        }
        h += item;
    }
    f.var_hash = h;
    h = 2166136261u;
    for (int i = 0; i < CLAN_VM_COUNTERS; ++i) {
        h = (h ^ (uint32_t)v->counters[i].value) * 16777619u;
        h = (h ^ v->counters[i].ticking) * 16777619u;
    }
    f.counter_hash = h;
    return f;
}

static uint64_t fnv = 1469598103934665603ull;
static void fnv_bytes(const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; ++i) {
        fnv ^= p[i];
        fnv *= 1099511628211ull;
    }
}

static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void test_w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void test_w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t test_skip_leb(const uint8_t *p, size_t len, size_t pos)
{
    for (int i = 0; i < 5 && pos < len; ++i) {
        if (!(p[pos++] & 0x80u)) return pos;
    }
    return 0;
}

// Locate the preview/name tail of a CLSV blob. The test uses this only to
// corrupt/rewrite fields while keeping the production parser authoritative.
static bool save_tail_fields(const uint8_t *save, size_t len, size_t *plen_off,
                             size_t *name_off, uint16_t *plen, uint16_t *nlen)
{
    if (!save || len < 16 || memcmp(save, "CLSV", 4) != 0 || save[4] != 1) {
        return false;
    }
    size_t p = 8;
    p += 12;                                  // chapter/root/cursor/page/choice/background
    p += 6;                                   // bodies
    const uint8_t stack_depth = save[p + 1];
    p += 2;                                   // body_count / stack_depth
    p += 4;                                   // saved_at
    const uint16_t var_count = clannad_rd16(save + p);
    p += 2;
    for (uint16_t i = 0; i < var_count; ++i) {
        p = test_skip_leb(save, len, p);
        if (!p || p >= len) return false;
        const uint8_t type = save[p++];
        if (type == 1) {
            if (p + 2 > len) return false;
            const uint16_t slen = clannad_rd16(save + p);
            p += 2;
            if (p + slen > len) return false;
            p += slen;
        } else {
            if (p + 4 > len) return false;
            p += 4;
        }
    }
    if (p >= len) return false;
    const uint8_t counter_count = save[p++];
    if (p + (size_t)counter_count * 6u > len) return false;
    p += (size_t)counter_count * 6u;
    if (p + (size_t)stack_depth * 4u > len) return false;
    p += (size_t)stack_depth * 4u;
    *plen_off = p;
    if (p + 2 > len) return false;
    *plen = clannad_rd16(save + p);
    p += 2;
    if (p + *plen > len) return false;
    p += *plen;
    *name_off = p;
    if (p + 2 > len) return false;
    *nlen = clannad_rd16(save + p);
    p += 2;
    if (p + *nlen + 4 != len) return false;
    return true;
}

static size_t rewrite_save_tail(const uint8_t *save, size_t len,
                                const char *preview, const char *name,
                                uint8_t *out, size_t cap)
{
    size_t plen_off, name_off;
    uint16_t old_plen, old_nlen;
    if (!save_tail_fields(save, len, &plen_off, &name_off, &old_plen, &old_nlen)) {
        return 0;
    }
    (void)name_off;
    (void)old_plen;
    (void)old_nlen;
    const size_t plen = strlen(preview);
    const size_t nlen = strlen(name);
    if (plen >= CLAN_VM_PREVIEW || nlen >= CLAN_VM_NAME) return 0;
    if (plen_off + 2 + plen + 2 + nlen + 4 > cap) return 0;
    memcpy(out, save, plen_off);
    size_t p = plen_off;
    test_w16(out + p, (uint16_t)plen);
    p += 2;
    memcpy(out + p, preview, plen);
    p += plen;
    test_w16(out + p, (uint16_t)nlen);
    p += 2;
    memcpy(out + p, name, nlen);
    p += nlen;
    test_w32(out + p, test_crc32(out, p));
    return p + 4;
}

static void fix_save_crc(uint8_t *save, size_t len)
{
    if (len >= 4) test_w32(save + len - 4, test_crc32(save, len - 4));
}

static uint64_t choice_signature(const clannad_vm_t *v)
{
    uint64_t h = 1469598103934665603ull;
#define MIX(x) do { h ^= (uint64_t)(x); h *= 1099511628211ull; } while (0)
    MIX(v->chapter);
    MIX(v->cursor);
    MIX(v->stack_depth);
    for (int i = 0; i < v->stack_depth; ++i) {
        MIX(v->stack[i].chapter);
        MIX(v->stack[i].cursor);
    }
    for (int i = 0; i < CLAN_VM_VAR_SLOTS; ++i) {
        if (!v->vars[i].key) continue;
        MIX(v->vars[i].key);
        MIX(v->vars[i].type);
        if (v->vars[i].type == 1) {
            MIX(v->vars[i].str_len);
            for (uint16_t k = 0; k < v->vars[i].str_len; ++k) {
                MIX((uint8_t)v->strings[v->vars[i].str_off + k]);
            }
        } else {
            MIX((uint32_t)v->vars[i].value);
        }
    }
    for (int i = 0; i < CLAN_VM_COUNTERS; ++i) {
        MIX((uint32_t)v->counters[i].value);
        MIX(v->counters[i].ticking);
    }
#undef MIX
    return h;
}

static int run_route(uint16_t chapter, uint8_t mode, int policy, uint32_t max_steps,
                     uint32_t *texts, uint32_t *choices, uint16_t *end_scene,
                     uint16_t *end_op, char *err, size_t errcap)
{
    if (!clannad_vm_new(&vm, chapter, mode)) {
        snprintf(err, errcap, "new %u: %s", chapter, vm.error);
        return -1;
    }
    fnv = 1469598103934665603ull;
    *texts = *choices = 0;
    *end_scene = *end_op = 0;
    static uint64_t seen[8192];
    int seen_count = 0;
    for (uint32_t step = 0; step < max_steps; ++step) {
        clannad_frame_t frame = clannad_vm_run(&vm);
        if (frame.type == CLAN_FRAME_TEXT) {
            (*texts)++;
            fnv_bytes("T", 1);
            fnv_bytes(frame.speaker ? frame.speaker : "",
                      strlen(frame.speaker ? frame.speaker : ""));
            fnv_bytes("", 1);
            fnv_bytes(frame.text ? frame.text : "",
                      strlen(frame.text ? frame.text : ""));
            fnv_bytes("", 1);
            if (!clannad_vm_next(&vm)) {
                snprintf(err, errcap, "next failed: %s", vm.error);
                return -1;
            }
        } else if (frame.type == CLAN_FRAME_CHOICE) {
            const uint64_t sig = choice_signature(&vm);
            bool repeated = false;
            for (int i = 0; i < seen_count; ++i) {
                if (seen[i] == sig) {
                    repeated = true;
                    break;
                }
            }
            if (repeated) {
                return 0;   // 与独立解释器一致：同一玩家状态再次出现即停止
            }
            if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
                seen[seen_count++] = sig;
            }
            (*choices)++;
            fnv_bytes("C", 1);
            for (int i = 0; i < frame.choice_count; ++i) {
                fnv_bytes(frame.choice_text[i] ? frame.choice_text[i] : "",
                          strlen(frame.choice_text[i] ? frame.choice_text[i] : ""));
                fnv_bytes("\x01", 1);
            }
            const int pick = policy == 0 ? 0 : frame.choice_count - 1;
            if (!clannad_vm_choose(&vm, (uint8_t)pick)) {
                snprintf(err, errcap, "choose failed: %s", vm.error);
                return -1;
            }
        } else if (frame.type == CLAN_FRAME_END) {
            *end_scene = vm.chapter;
            *end_op = vm.cursor;
            return 0;
        } else {
            snprintf(err, errcap, "vm error: %s", frame.text ? frame.text : "?");
            return -1;
        }
    }
    snprintf(err, errcap, "step limit");
    return -1;
}

static int run_until_choice(int *choice_index)
{
    for (int i = 0; i < 20000; ++i) {
        clannad_frame_t frame = clannad_vm_run(&vm);
        if (frame.type == CLAN_FRAME_TEXT) {
            clannad_vm_next(&vm);
        } else if (frame.type == CLAN_FRAME_CHOICE) {
            *choice_index = i;
            return 0;
        } else if (frame.type == CLAN_FRAME_END) {
            return -1;
        } else {
            return -1;
        }
    }
    return -1;
}

static int run_path(const clannad_path_fixture_t *f, char *err, size_t errcap)
{
    if (!clannad_vm_new(&vm, f->start, f->story ? CLAN_MODE_STORY : CLAN_MODE_CHAPTER)) {
        snprintf(err, errcap, "path start %u", f->start);
        return -1;
    }
    if (f->progress) {
        for (int bit = 1; bit <= 31; ++bit) {
            clannad_vm_var_set_int(&vm, 1, (uint32_t)bit, 1);
        }
        clannad_vm_var_set_int(&vm, 1, 0, 13);
        clannad_vm_var_set_int(&vm, 1, 45, 1);
    }
    fnv = 1469598103934665603ull;
    uint32_t texts = 0, choices = 0;
    uint8_t used = 0;
    for (uint32_t step = 0; step < 400000; ++step) {
        clannad_frame_t frame = clannad_vm_run(&vm);
        if (frame.type == CLAN_FRAME_TEXT) {
            texts++;
            fnv_bytes("T", 1);
            fnv_bytes(frame.speaker ? frame.speaker : "",
                      strlen(frame.speaker ? frame.speaker : ""));
            fnv_bytes("", 1);
            fnv_bytes(frame.text ? frame.text : "",
                      strlen(frame.text ? frame.text : ""));
            fnv_bytes("", 1);
            clannad_vm_next(&vm);
        } else if (frame.type == CLAN_FRAME_CHOICE) {
            if (used >= f->path_len) {
                snprintf(err, errcap, "path exhausted");
                return -1;
            }
            choices++;
            fnv_bytes("C", 1);
            for (int i = 0; i < frame.choice_count; ++i) {
                fnv_bytes(frame.choice_text[i] ? frame.choice_text[i] : "",
                          strlen(frame.choice_text[i] ? frame.choice_text[i] : ""));
                fnv_bytes("\x01", 1);
            }
            if (!clannad_vm_choose(&vm, f->path[used])) {
                snprintf(err, errcap, "path choose %u", f->path[used]);
                return -1;
            }
            used++;
        } else if (frame.type == CLAN_FRAME_END) {
            return (vm.chapter == f->end_scene && vm.cursor == f->end_op &&
                    texts == f->texts && choices == f->choices && fnv == f->fnv)
                       ? 0 : -1;
        } else {
            snprintf(err, errcap, "path vm: %s", frame.text ? frame.text : "?");
            return -1;
        }
    }
    snprintf(err, errcap, "path step limit");
    return -1;
}

static void test_paths(void)
{
    int bad = 0;
    for (int i = 0; i < CLANNAD_PATH_COUNT; ++i) {
        char err[128] = {0};
        if (run_path(&CLANNAD_PATHS[i], err, sizeof(err)) != 0) {
            printf("path mismatch start=%u end=%u:%u %s\n",
                   CLANNAD_PATHS[i].start, CLANNAD_PATHS[i].end_scene,
                   CLANNAD_PATHS[i].end_op, err);
            bad++;
        }
    }
    EXPECT(bad == 0, "explicit choice paths cover the 7 extra termination sites");
}

static void test_text_pages(void)
{
    const char *sample =
        "也就是说…现在不是学生上学的时间。不过，就算是看到了这冷清的情景我也毫不"
        "着急，依然悠闲地迈着步子。………。距离校门还有２００米。停下脚步。「唉…」";
    uint16_t offsets[CLAN_TEXT_MAX_PAGES + 1];
    const int pages = clannad_text_pages(sample, CLAN_TEXT_UNITS_PER_LINE,
                                        CLAN_TEXT_LINES, offsets,
                                        CLAN_TEXT_MAX_PAGES + 1);
    EXPECT(pages >= 2, "sample spans several pages");
    EXPECT(offsets[0] == 0, "page offsets start at 0");
    EXPECT(offsets[pages] == (uint16_t)strlen(sample), "page offsets end at length");
    for (int i = 0; i < pages; ++i) {
        EXPECT(offsets[i] < offsets[i + 1] || i + 1 == pages,
               "page offsets strictly increase");
        char lines[CLAN_TEXT_LINES][160];
        clannad_text_layout(sample, offsets[i], offsets[i + 1], offsets[i + 1],
                            CLAN_TEXT_UNITS_PER_LINE, lines);
        int units = 0;
        for (int k = 0; k < CLAN_TEXT_LINES; ++k) {
            units = 0;
            for (const char *p = lines[k]; *p;) {
                const uint8_t c = (uint8_t)*p;
                units += (c < 0x80 ? 1 : 2);
                p += (c < 0x80 ? 1 : 3);
            }
            EXPECT(units <= CLAN_TEXT_UNITS_PER_LINE + 2, "line width bounded");
        }
        // 逐字显示不能切断码位
        for (uint16_t pos = offsets[i]; pos < offsets[i + 1];
             pos = clannad_utf8_next(sample, offsets[i + 1], pos)) {
            EXPECT((uint8_t)sample[pos] != 0x80, "typewriter stops at codepoint");
        }
    }
    EXPECT(clannad_utf8_count(sample, (uint16_t)strlen(sample)) > 50, "count");
}

static void test_deflate(void)
{
    uint8_t data[1536];
    for (int i = 0; i < (int)sizeof(data); ++i) {
        data[i] = (uint8_t)((i * 37 + (i >> 3)) & 0xFF);
    }
    memset(data + 700, 0, 500);
    uint8_t packed[2048];
    uint32_t packed_len = 0;
    EXPECT(clannad_deflate_raw(data, sizeof(data), packed, sizeof(packed), &packed_len),
           "deflate succeeds");
    EXPECT(packed_len > 0 && packed_len < sizeof(data) + 8, "deflate bounded");
    uint8_t back[2048];
    uint32_t back_len = 0;
    EXPECT(clannad_inflate_raw(packed, packed_len, back, sizeof(back), &back_len),
           "inflate succeeds");
    EXPECT(back_len == sizeof(data) && memcmp(back, data, sizeof(data)) == 0,
           "deflate roundtrip");
    // 随机不可压缩数据走 stored 回退
    uint8_t rnd[512];
    uint32_t seed = 0x12345678u;
    for (int i = 0; i < (int)sizeof(rnd); ++i) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = (uint8_t)(seed >> 16);
    }
    EXPECT(clannad_deflate_raw(rnd, sizeof(rnd), packed, sizeof(packed), &packed_len),
           "deflate stored fallback");
    EXPECT(clannad_inflate_raw(packed, packed_len, back, sizeof(back), &back_len) &&
               back_len == sizeof(rnd) && memcmp(back, rnd, sizeof(rnd)) == 0,
           "stored roundtrip");
}

int main(int argc, char **argv)
{
    const char *pack_path = argc > 1 ? argv[1] : "assets/clannad/clannad_pack.bin";
    FILE *fh = fopen(pack_path, "rb");
    if (!fh) {
        fprintf(stderr, "cannot open %s\n", pack_path);
        return 2;
    }
    const size_t pack_size = fread(pack_data, 1, sizeof(pack_data), fh);
    fclose(fh);
    char why[128] = {0};
    EXPECT(pack_size > 1000000, "pack loaded");
    EXPECT(clannad_vm_init(&vm, pack_data, (uint32_t)pack_size, raw, sizeof(raw),
                           why, sizeof(why)), why);
    EXPECT(vm.pack.scene_count == 212, "212 scenes");
    EXPECT(vm.pack.block_count == 411, "411 chunks");
    EXPECT(vm.pack.chapter_count == 195, "195 chapter entries");
    EXPECT(vm.pack.route_count == 19, "19 routes");
    EXPECT(vm.pack.gallery_group_count == 68, "68 gallery groups");
    EXPECT(vm.pack.asset_count >= 920, "at least 920 assets");
    EXPECT(clannad_pack_meta_int(&vm.pack, "flow_scene", 0) == 414, "flow scene 414");

    // 坏包：魔数/长度/截断
    {
        clannad_vm_t bad;
        uint8_t copy[64];
        memcpy(copy, pack_data, sizeof(copy));
        copy[0] = 'X';
        EXPECT(!clannad_vm_init(&bad, copy, 64, raw, sizeof(raw), why, sizeof(why)),
               "bad magic rejected");
        EXPECT(!clannad_vm_init(&bad, pack_data, (uint32_t)pack_size - 8, raw,
                                sizeof(raw), why, sizeof(why)),
               "truncated pack rejected");
    }

    // 全部章节入口 × 两种策略 + 新游戏两条路径
    uint32_t route_texts = 0, route_choices = 0;
    uint32_t route_end_sites = 0;
    int route_fail = 0;
    for (int i = 0; i < CLANNAD_ROUTE_COUNT; ++i) {
        const clannad_route_fixture_t *f = &CLANNAD_ROUTES[i];
        uint32_t texts = 0, choices = 0;
        uint16_t es = 0, eo = 0;
        char err[128] = {0};
        const int rc = run_route(f->chapter, CLAN_MODE_CHAPTER, f->policy, 400000,
                                 &texts, &choices, &es, &eo, err, sizeof(err));
        if (rc != 0 || texts != f->texts || choices != f->choices ||
            es != f->end_scene || eo != f->end_op || fnv != f->fnv) {
            if (route_fail < 8) {
                printf("route mismatch %u/%u: rc=%d texts=%u/%u choices=%u/%u "
                       "end=%u:%u/%u:%u fnv=%016llx/%016llx %s\n",
                       f->chapter, f->policy, rc, texts, f->texts, choices,
                       f->choices, es, eo, f->end_scene, f->end_op,
                       (unsigned long long)fnv, (unsigned long long)f->fnv, err);
            }
            route_fail++;
        }
        route_texts += texts;
        route_choices += choices;
        route_end_sites++;
    }
    EXPECT(route_fail == 0, "all 390 chapter-entry runs match the independent VM");
    EXPECT(route_texts > 500000, "route transcripts cover >500k lines");
    EXPECT(route_choices > 1000, "route transcripts cover >1000 choices");
    EXPECT(route_end_sites == 390, "all chapter runs terminate");

    int flow_fail = 0;
    for (int i = 0; i < CLANNAD_FLOW_COUNT; ++i) {
        const clannad_route_fixture_t *f = &CLANNAD_FLOW[i];
        uint32_t texts = 0, choices = 0;
        uint16_t es = 0, eo = 0;
        char err[128] = {0};
        const int rc = run_route(f->chapter, CLAN_MODE_STORY, f->policy, 400000,
                                 &texts, &choices, &es, &eo, err, sizeof(err));
        if (rc != 0 || texts != f->texts || choices != f->choices ||
            es != f->end_scene || eo != f->end_op || fnv != f->fnv) {
            printf("flow mismatch %u/%u\n", f->chapter, f->policy);
            flow_fail++;
        }
    }
    EXPECT(flow_fail == 0, "new-game paths match");

    // 选项处存档：编码 → 校验 → 应用 → 继续到相同结局
    {
        EXPECT(clannad_vm_new(&vm, 414, CLAN_MODE_CHAPTER), "start 414");
        int ci = 0;
        EXPECT(run_until_choice(&ci) == 0, "reach first choice");
        const fingerprint_t before = fingerprint(&vm);
        uint8_t save[CLAN_VM_SAVE_RAW_MAX];
        const uint16_t page = 0;
        const uint16_t choice = 1;
        const size_t len = clannad_vm_save_encode(&vm, page, choice, 12345,
                                                  "共通篇 · 4月14日", save,
                                                  sizeof(save));
        EXPECT(len > 0, "choice save encoded");
        clannad_save_info_t info;
        EXPECT(clannad_vm_save_validate(&vm, save, len, &info), "choice save validates");
        EXPECT(info.chapter == 414 && info.choice_sel == choice, "save metadata");
        EXPECT(info.flags & 1, "save marks pending choice");
        EXPECT(clannad_vm_save_apply(&vm, save, len), "choice save applies");
        const fingerprint_t after = fingerprint(&vm);
        if (!(after.chapter == before.chapter && after.cursor == before.cursor &&
              after.var_hash == before.var_hash && after.background == before.background &&
              after.body_count == before.body_count && after.stack_depth == before.stack_depth &&
              after.counter_hash == before.counter_hash && after.mode == before.mode)) {
            printf("save-restore before ch=%u cur=%u bg=%u bc=%u sd=%u mode=%u nv=%u vh=%08x ct=%08x\n",
                   before.chapter, before.cursor, before.background, before.body_count,
                   before.stack_depth, before.mode, before.var_count, before.var_hash,
                   before.counter_hash);
            printf("save-restore after  ch=%u cur=%u bg=%u bc=%u sd=%u mode=%u nv=%u vh=%08x ct=%08x\n",
                   after.chapter, after.cursor, after.background, after.body_count,
                   after.stack_depth, after.mode, after.var_count, after.var_hash,
                   after.counter_hash);
        }
        EXPECT(after.chapter == before.chapter && after.cursor == before.cursor &&
                   after.var_hash == before.var_hash && after.background == before.background &&
                   after.body_count == before.body_count && after.stack_depth == before.stack_depth &&
                   after.counter_hash == before.counter_hash && after.mode == before.mode,
               "choice restore state");
        clannad_frame_t frame = clannad_vm_run(&vm);
        EXPECT(frame.type == CLAN_FRAME_CHOICE && frame.choice_count > 1,
               "choice restored at the same op");
        EXPECT(clannad_vm_choose(&vm, choice), "choose after restore");
        uint32_t texts = 0, choices = 0;
        uint16_t es = 0, eo = 0;
        char err[128] = {0};
        // 继续读完，与策略 last 的 414 结果一致
        int rc = 0;
        for (int step = 0; step < 400000; ++step) {
            frame = clannad_vm_run(&vm);
            if (frame.type == CLAN_FRAME_TEXT) {
                texts++;
                clannad_vm_next(&vm);
            } else if (frame.type == CLAN_FRAME_CHOICE) {
                choices++;
                clannad_vm_choose(&vm, (uint8_t)(frame.choice_count - 1));
            } else if (frame.type == CLAN_FRAME_END) {
                es = vm.chapter;
                eo = vm.cursor;
                break;
            } else {
                rc = -1;
                snprintf(err, sizeof(err), "%s", frame.text ? frame.text : "?");
                break;
            }
        }
        EXPECT(rc == 0 && es == 444 && eo == 86, "resume reaches the same ending");
        EXPECT(texts > 100 && choices > 10, "resume reads real content");

        // 坏存档必须原状态不变
        const fingerprint_t guard = fingerprint(&vm);
        uint8_t bad[CLAN_VM_SAVE_RAW_MAX];
        memcpy(bad, save, len);
        bad[0] = 'X';
        EXPECT(!clannad_vm_save_validate(&vm, bad, len, &info), "bad magic save rejected");
        memcpy(bad, save, len);
        bad[len - 1] ^= 0xFF;
        EXPECT(!clannad_vm_save_validate(&vm, bad, len, &info), "bad CRC save rejected");
        memcpy(bad, save, len);
        bad[4] = 2;   // 未知存档版本（旧档/未来档）必须拒绝
        EXPECT(!clannad_vm_save_validate(&vm, bad, len, &info),
               "unknown save version rejected");
        EXPECT(!clannad_vm_save_apply(&vm, bad, len), "bad save not applied");
        const fingerprint_t still = fingerprint(&vm);
        EXPECT(memcmp(&guard, &still, sizeof(guard)) == 0, "failed save leaves state intact");
    }

    // 普通对白存档：非空中文 preview/speaker、变量、页面和下一句都必须恢复。
    {
        EXPECT(clannad_vm_new(&vm, 414, CLAN_MODE_STORY), "text save story start");
        clannad_frame_t before_frame = {0};
        int steps = 0;
        while (steps++ < 200) {
            before_frame = clannad_vm_run(&vm);
            if (before_frame.type == CLAN_FRAME_CHOICE) {
                EXPECT(clannad_vm_choose(&vm, 0), "text save choice skip");
                continue;
            }
            if (before_frame.type != CLAN_FRAME_TEXT) break;
            if (before_frame.speaker && before_frame.speaker[0]) break;
            clannad_vm_next(&vm);
        }
        EXPECT(before_frame.type == CLAN_FRAME_TEXT &&
                   before_frame.speaker && before_frame.speaker[0],
               "found text with non-empty speaker");
        char text_before[1024];
        char speaker_before[128];
        snprintf(text_before, sizeof(text_before), "%s",
                 before_frame.text ? before_frame.text : "");
        snprintf(speaker_before, sizeof(speaker_before), "%s", before_frame.speaker);
        EXPECT(clannad_vm_var_set_int(&vm, 0, 3, 77), "text save variable");
        uint8_t text_save[CLAN_VM_SAVE_RAW_MAX];
        const size_t text_len = clannad_vm_save_encode(&vm, 2, 0, 123,
                                                       "共通篇 · 4月14日",
                                                       text_save, sizeof(text_save));
        EXPECT(text_len > 0, "text save encoded");
        clannad_save_info_t text_info;
        EXPECT(clannad_vm_save_validate(&vm, text_save, text_len, &text_info),
               "text save validates");
        EXPECT(text_info.page == 2 && text_info.preview[0] != 0 &&
                   text_info.chapter_name[0] != 0,
               "text save preview/name/page");
        EXPECT(strncmp(text_info.preview, text_before, strlen(text_info.preview)) == 0,
               "text save preview is current text prefix");
        const fingerprint_t text_before_fp = fingerprint(&vm);
        clannad_vm_next(&vm);
        clannad_frame_t next_before = clannad_vm_run(&vm);
        char next_text[1024];
        snprintf(next_text, sizeof(next_text), "%s",
                 next_before.text ? next_before.text : "");
        EXPECT(clannad_vm_save_apply(&vm, text_save, text_len), "text save applies");
        const fingerprint_t text_after_fp = fingerprint(&vm);
        EXPECT(memcmp(&text_before_fp, &text_after_fp, sizeof(text_before_fp)) == 0,
               "text save apply preserves VM state");
        clannad_frame_t after_frame = clannad_vm_run(&vm);
        EXPECT(after_frame.type == CLAN_FRAME_TEXT &&
                   strcmp(after_frame.text ? after_frame.text : "", text_before) == 0 &&
                   strcmp(after_frame.speaker ? after_frame.speaker : "",
                          speaker_before) == 0,
               "text save re-runs the same sentence and speaker");
        clannad_vm_next(&vm);
        clannad_frame_t next_after = clannad_vm_run(&vm);
        EXPECT(next_after.type == next_before.type &&
                   strcmp(next_after.text ? next_after.text : "", next_text) == 0,
               "text save continues to the same next op");

        // Real Chinese/multibyte preview already covered. Add explicit ASCII,
        // maximum 95-byte, empty, and malformed tail cases.
        uint8_t rewritten[CLAN_VM_SAVE_RAW_MAX];
        char ascii_preview[] = "ASCII preview";
        size_t rlen = rewrite_save_tail(text_save, text_len, ascii_preview, "ch",
                                        rewritten, sizeof(rewritten));
        EXPECT(rlen > 0, "ascii preview save rewrite");
        EXPECT(clannad_vm_save_validate(&vm, rewritten, rlen, &text_info),
               "ascii preview validates");
        EXPECT(strcmp(text_info.preview, ascii_preview) == 0 &&
                   strcmp(text_info.chapter_name, "ch") == 0,
               "ascii preview/name round-trip");
        EXPECT(clannad_vm_save_apply(&vm, rewritten, rlen), "ascii preview applies");
        EXPECT(clannad_vm_run(&vm).type == CLAN_FRAME_TEXT,
               "ascii preview stays on the same text op");

        char max_preview[CLAN_VM_PREVIEW];
        memset(max_preview, 'M', CLAN_VM_PREVIEW - 1);
        max_preview[CLAN_VM_PREVIEW - 1] = '\0';
        rlen = rewrite_save_tail(text_save, text_len, max_preview, "max",
                                 rewritten, sizeof(rewritten));
        EXPECT(rlen > 0, "maximum preview save rewrite");
        EXPECT(clannad_vm_save_validate(&vm, rewritten, rlen, &text_info),
               "maximum 95-byte preview validates");
        EXPECT(strlen(text_info.preview) == CLAN_VM_PREVIEW - 1,
               "maximum preview preserved");
        EXPECT(clannad_vm_save_apply(&vm, rewritten, rlen), "maximum preview applies");

        rlen = rewrite_save_tail(text_save, text_len, "", "empty", rewritten,
                                 sizeof(rewritten));
        EXPECT(rlen > 0, "empty preview save rewrite");
        EXPECT(clannad_vm_save_validate(&vm, rewritten, rlen, &text_info) &&
                   text_info.preview[0] == '\0',
               "empty preview validates");

        // Field-level corruption must be rejected without changing the VM.
        size_t plen_off, name_off;
        uint16_t plen, nlen;
        EXPECT(save_tail_fields(text_save, text_len, &plen_off, &name_off,
                                &plen, &nlen) && plen > 2 && nlen > 1,
               "locate preview/name fields");
        const fingerprint_t guard = fingerprint(&vm);
        uint8_t corrupt[CLAN_VM_SAVE_RAW_MAX];
        memcpy(corrupt, text_save, text_len);
        test_w16(corrupt + plen_off, (uint16_t)(plen - 1));
        fix_save_crc(corrupt, text_len);
        EXPECT(!clannad_vm_save_validate(&vm, corrupt, text_len, &text_info),
               "truncated preview length rejected");
        EXPECT(!clannad_vm_save_apply(&vm, corrupt, text_len),
               "truncated preview not applied");

        memcpy(corrupt, text_save, text_len);
        test_w16(corrupt + name_off, (uint16_t)(nlen - 1));
        fix_save_crc(corrupt, text_len);
        EXPECT(!clannad_vm_save_validate(&vm, corrupt, text_len, &text_info),
               "truncated name length rejected");
        EXPECT(!clannad_vm_save_apply(&vm, corrupt, text_len),
               "truncated name not applied");

        memcpy(corrupt, text_save, text_len);
        corrupt[plen_off + 2] = 0xFF;
        fix_save_crc(corrupt, text_len);
        EXPECT(!clannad_vm_save_validate(&vm, corrupt, text_len, &text_info),
               "bad UTF-8 preview rejected");
        EXPECT(!clannad_vm_save_apply(&vm, corrupt, text_len),
               "bad UTF-8 preview not applied");
        const fingerprint_t still = fingerprint(&vm);
        EXPECT(memcmp(&guard, &still, sizeof(guard)) == 0,
               "invalid saves do not change VM state");
    }

    // 6728 -> 6729: chapter start must see progress values supplied by the
    // source's withProgress(newState(item.id), progress) path, including zeros.
    {
        EXPECT(clannad_vm_new(&vm, 6728, CLAN_MODE_CHAPTER), "chapter 6728 start");
        clannad_frame_t f = clannad_vm_run(&vm);
        EXPECT(f.type == CLAN_FRAME_TEXT || f.type == CLAN_FRAME_CHOICE,
               "chapter 6728 runs");
        uint8_t prog[CLAN_VM_PROGRESS_MAX];
        EXPECT(clannad_vm_var_set_int(&vm, 1, 2, 1), "progress g2");
        EXPECT(clannad_vm_var_set_int(&vm, 1, 32, 0), "progress g32=0");
        EXPECT(clannad_vm_var_set_int(&vm, 1, 0, 5), "progress g0=5");
        uint8_t chapter_save[CLAN_VM_SAVE_RAW_MAX];
        const size_t chapter_len = clannad_vm_save_encode(
            &vm, 1, 0, 321, "6728 chapter", chapter_save, sizeof(chapter_save));
        clannad_save_info_t chapter_info;
        EXPECT(chapter_len > 0 &&
                   clannad_vm_save_validate(&vm, chapter_save, chapter_len,
                                            &chapter_info),
               "6728 chapter text save validates");
        EXPECT(chapter_info.mode == CLAN_MODE_CHAPTER &&
                   chapter_info.preview[0] != 0,
               "6728 chapter save keeps chapter mode and preview");
        EXPECT(clannad_vm_save_apply(&vm, chapter_save, chapter_len),
               "6728 chapter save applies");
        EXPECT(clannad_vm_run(&vm).type == CLAN_FRAME_TEXT,
               "6728 chapter save returns to its text");
        const size_t plen = clannad_vm_progress_encode(&vm, prog, sizeof(prog));
        EXPECT(plen > 0 && clannad_vm_progress_validate(prog, plen),
               "chapter progress encodes");
        // Source index.ux:183 selectBrowse unconditionally applies saved
        // progress.  With g2=1/g32=0/g0=5 the source first text is cursor 114;
        // the old chapter path (no progress) first text was cursor 203.
        EXPECT(clannad_vm_new(&vm, 6729, CLAN_MODE_CHAPTER), "chapter 6729 no-progress");
        int scan = 0;
        while (scan++ < 500) {
            f = clannad_vm_run(&vm);
            if (f.type == CLAN_FRAME_TEXT && vm.cursor >= 114) break;
            if (f.type == CLAN_FRAME_CHOICE) {
                EXPECT(clannad_vm_choose(&vm, 0), "6729 no-progress choice");
                continue;
            }
            if (f.type != CLAN_FRAME_TEXT) break;
            clannad_vm_next(&vm);
        }
        EXPECT(f.type == CLAN_FRAME_TEXT && vm.cursor == 203,
               "6729 without progress first text >=114 is cursor 203");
        EXPECT(clannad_vm_new(&vm, 6729, CLAN_MODE_CHAPTER), "chapter 6729 start");
        EXPECT(clannad_vm_progress_apply(&vm, prog, plen),
               "chapter progress applies before run");
        int32_t g2 = -1, g32 = -1;
        EXPECT(clannad_vm_var_int(&vm, 1, 2, &g2) && g2 == 1,
               "chapter sees nonzero global progress");
        EXPECT(clannad_vm_var_int(&vm, 1, 32, &g32) && g32 == 0,
               "chapter sees explicit zero global progress");
        scan = 0;
        while (scan++ < 500) {
            f = clannad_vm_run(&vm);
            if (f.type == CLAN_FRAME_TEXT && vm.cursor >= 114) break;
            if (f.type == CLAN_FRAME_CHOICE) {
                EXPECT(clannad_vm_choose(&vm, 0), "6729 progress choice");
                continue;
            }
            if (f.type != CLAN_FRAME_TEXT) break;
            clannad_vm_next(&vm);
        }
        EXPECT(f.type == CLAN_FRAME_TEXT && vm.cursor == 114,
               "6729 with saved progress first text >=114 is cursor 114");
        EXPECT(f.type == CLAN_FRAME_TEXT && f.text &&
                   strstr(f.text, "是吗") != NULL,
               "6729 source witness text at cursor 114");
        // The chapter may modify its local g values (source witness:
        // g32=1/g0=4 after this stretch); those are VM-local, not global.
        int32_t local_g32 = -1, local_g0 = -1;
        int local_steps = 0;
        while (local_steps++ < 2000) {
            if (clannad_vm_var_int(&vm, 1, 32, &local_g32) && local_g32 == 1 &&
                clannad_vm_var_int(&vm, 1, 0, &local_g0) && local_g0 == 4) {
                break;
            }
            f = clannad_vm_run(&vm);
            if (f.type == CLAN_FRAME_TEXT) {
                clannad_vm_next(&vm);
            } else if (f.type == CLAN_FRAME_CHOICE) {
                EXPECT(clannad_vm_choose(&vm, 0), "6729 local choice");
            } else {
                break;
            }
        }
        EXPECT(local_g32 == 1 && local_g0 == 4,
               "6729 chapter local g32=1/g0=4 after run");
        printf("6729 local witness: cursor=%u g32=%ld g0=%ld\n",
               (unsigned)vm.cursor, (long)local_g32, (long)local_g0);
    }

    // 调用栈位置：构造一个合法的多帧调用栈，保存/恢复 stack_depth 与帧内容。
    {
        EXPECT(clannad_vm_new(&vm, 414, CLAN_MODE_STORY), "stack save start 414");
        clannad_frame_t stack_frame = clannad_vm_run(&vm);
        EXPECT(stack_frame.type == CLAN_FRAME_TEXT, "stack save reaches text");
        vm.stack_depth = 2;
        vm.stack[0].chapter = vm.chapter;
        vm.stack[0].cursor = 0;
        vm.stack[1].chapter = vm.root;
        vm.stack[1].cursor = 0;
        uint8_t stack_save[CLAN_VM_SAVE_RAW_MAX];
        const size_t stack_len = clannad_vm_save_encode(
            &vm, 0, 0, 999, "call stack", stack_save, sizeof(stack_save));
        clannad_save_info_t stack_info;
        EXPECT(stack_len > 0 &&
                   clannad_vm_save_validate(&vm, stack_save, stack_len, &stack_info),
               "call-stack save validates");
        EXPECT(stack_info.stack_depth == 2 && stack_info.preview[0] != 0,
               "call-stack save preserves depth and preview");
        EXPECT(clannad_vm_save_apply(&vm, stack_save, stack_len),
               "call-stack save applies");
        clannad_frame_t restored = clannad_vm_run(&vm);
        EXPECT(restored.type == CLAN_FRAME_TEXT &&
                   vm.stack_depth == 2 &&
                   vm.stack[0].chapter == vm.chapter &&
                   vm.stack[1].chapter == vm.root,
               "call-stack save restores the same text and frames");
    }

    // 通关记录：g[] 覆盖/校验/After Story/真结局
    {
        EXPECT(clannad_vm_new(&vm, 414, CLAN_MODE_STORY), "story start");
        EXPECT(clannad_vm_var_set_int(&vm, 1, 0, 13), "set orbs");
        EXPECT(clannad_vm_var_set_int(&vm, 1, 1, 1), "set route flag");
        EXPECT(clannad_vm_var_set_int(&vm, 1, 45, 1), "set true ending");
        uint8_t prog[CLAN_VM_PROGRESS_MAX];
        const size_t len = clannad_vm_progress_encode(&vm, prog, sizeof(prog));
        EXPECT(len > 0 && clannad_vm_progress_validate(prog, len), "progress encodes");
        EXPECT(clannad_progress_light_count(prog, len) == 13, "light count");
        EXPECT(clannad_progress_true_ending(prog, len), "true ending flag");
        EXPECT(!clannad_progress_after_unlocked(prog, len), "after story needs all flags");
        for (int bit = 0; bit < 32; ++bit) clannad_vm_var_set_int(&vm, 1, 1 + bit, 1);
        const size_t len2 = clannad_vm_progress_encode(&vm, prog, sizeof(prog));
        EXPECT(clannad_progress_after_unlocked(prog, len2), "after story condition");
        EXPECT(clannad_vm_new(&vm, 414, CLAN_MODE_STORY), "second story start");
        EXPECT(clannad_vm_progress_apply(&vm, prog, len2), "progress applies");
        int32_t value = 0;
        EXPECT(clannad_vm_var_int(&vm, 1, 45, &value) && value == 1, "g45 restored");
        EXPECT(!clannad_vm_progress_validate(prog, len2 - 1), "truncated progress rejected");
        prog[0] = 'X';
        EXPECT(!clannad_vm_progress_validate(prog, len2), "bad progress rejected");
    }

    // 两种策略无法到达的 7 个终止点：显式选择路径
    test_paths();

    // 文本分页/打字机
    test_text_pages();
    test_deflate();

    printf("%s: checks %d, failures %d\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
