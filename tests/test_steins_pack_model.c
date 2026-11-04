// tests/test_steins_pack_model.c —— 真实资源包 + 纯逻辑层的宿主机回归。
//
// 覆盖:
//   * 包结构与每个定长段的边界(截断包、坏 magic、错版本、段长度不符都要拒绝)
//   * 全部 74 个节点:chunk 引用链累计帧数 == count;每个 chunk 的首帧/帧数不越界
//   * 全部帧:view/layer/asset 下标有效;正文与说话人可解成 UTF-8;分页不丢字
//   * 全部画面:层矩形与资源裁剪框自洽(目标矩形能落在画布上)
//   * 全部选择项:落点是合法节点;动作码在枚举内
//   * 存档编解码与 CRC 篡改;解锁位与合并不回退;世界线格式化;章节卡选择
//
// 资源包路径可用 --pack 传入,默认 assets/steins/steins_pack.bin。
#include "steins_model.h"
#include "steins_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);      \
            fprintf(stderr, "\n");             \
            ++g_failures;                      \
        }                                      \
    } while (0)

static uint8_t *load_file(const char *path, uint32_t *size_out)
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

static int test_pack_rejects(const uint8_t *data, uint32_t size)
{
    steins_pack_t pack;
    // 截断
    CHECK(!steins_pack_open(&pack, data, size - 4u), "truncated pack accepted");
    // 坏 magic
    uint8_t *copy = (uint8_t *)malloc(size);
    memcpy(copy, data, size);
    copy[0] = 'X';
    CHECK(!steins_pack_open(&pack, copy, size), "bad magic accepted");
    // 错版本
    memcpy(copy, data, size);
    copy[8] = 9;
    CHECK(!steins_pack_open(&pack, copy, size), "bad version accepted");
    // 段长度不符(把 NODE 段的 size 改小)
    memcpy(copy, data, size);
    const uint32_t sections = steins_rd32(copy + 16);
    for (uint32_t i = 0; i < sections; ++i) {
        uint8_t *rec = copy + 32u + i * 16u;
        if (steins_rd32(rec) == STEINS_SEC_NODE) {
            const uint32_t len = steins_rd32(rec + 12);
            uint8_t *bytes = rec + 12;
            bytes[0] = (uint8_t)(len - 1u);
            break;
        }
    }
    CHECK(!steins_pack_open(&pack, copy, size), "section size mismatch accepted");
    free(copy);
    return 0;
}

// ------------------------------------------------------------------ 包结构拒绝
// 这些用例都走真实的 steins_pack_open(),不是"只比 manifest"。
static void test_pack_structure_negatives(const uint8_t *data, uint32_t size)
{
    uint8_t *copy = (uint8_t *)malloc(size);
    CHECK(copy != NULL, "cannot allocate pack copy");
    if (!copy) return;
    uint32_t sections = 0;
    steins_pack_t probe;

    // 0) 正常包必须被接受。
    memcpy(copy, data, size);
    CHECK(steins_pack_open(&probe, copy, size), "clean pack rejected");

    sections = steins_rd32(copy + 16);

    // 1) 重复段类型:把第 2 条记录的类型改成第 1 条的类型。
    memcpy(copy, data, size);
    const uint32_t type0 = steins_rd32(copy + 32);
    copy[32 + 16] = (uint8_t)(type0 & 0xFFu);
    copy[32 + 16 + 1] = (uint8_t)((type0 >> 8) & 0xFFu);
    copy[32 + 16 + 2] = (uint8_t)((type0 >> 16) & 0xFFu);
    copy[32 + 16 + 3] = (uint8_t)((type0 >> 24) & 0xFFu);
    CHECK(!steins_pack_open(&probe, copy, size), "duplicate section type accepted");

    // 2) 段偏移指进段表内部。
    memcpy(copy, data, size);
    {
        const uint32_t bad = 32u;
        memcpy(copy + 32 + 4, &bad, 4);
    }
    CHECK(!steins_pack_open(&probe, copy, size), "section offset inside directory accepted");

    // 3) 两个非空顶层段相互重叠:让第 2 段的 offset 等于第 1 段的 offset。
    memcpy(copy, data, size);
    {
        const uint32_t off0 = steins_rd32(copy + 32 + 4);
        memcpy(copy + 32 + 16 + 4, &off0, 4);
    }
    CHECK(!steins_pack_open(&probe, copy, size), "overlapping sections accepted");

    // 4) CHUNKREF count 乘法回绕:count = 0x80000000,size 不变。
    memcpy(copy, data, size);
    for (uint32_t i = 0; i < sections; ++i) {
        uint8_t *rec = copy + 32u + i * 16u;
        if (steins_rd32(rec) == STEINS_SEC_CHUNKREF) {
            const uint32_t huge = 0x80000000u;
            memcpy(rec + 8, &huge, 4);
            break;
        }
    }
    CHECK(!steins_pack_open(&probe, copy, size), "chunkref count overflow accepted");

    // 5) 段长度比文件还长(减法式越界检查)。
    memcpy(copy, data, size);
    {
        const uint32_t huge = 0xFFFFFFF0u;
        memcpy(copy + 32 + 12, &huge, 4);
    }
    CHECK(!steins_pack_open(&probe, copy, size), "section length overflow accepted");
    free(copy);
    printf("  pack structure negatives checked (dup type / directory offset / overlap /"
           " refcount overflow / length overflow)\n");
}

// ------------------------------------------------------- UTF-16 -> UTF-8 安全
static void test_str_utf8_safety(steins_pack_t *pack)
{
    const uint32_t count = steins_pack_count(pack, STEINS_SEC_STRT);
    uint16_t worst = STEINS_NONE;
    int worst_len = -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t units = 0;
        const uint16_t *src = steins_pack_str(pack, (uint16_t)i, &units);
        if (!src) continue;
        // UTF-8 上限:3 字节/码元(代理对只会上界更松)
        const int bytes = (int)units * 3 + 4;
        if (bytes > worst_len) {
            worst_len = bytes;
            worst = (uint16_t)i;
        }
    }
    CHECK(worst != STEINS_NONE, "no strings found");

    // 最长串:容量按**UTF-16 码元数的严格上界** 3*units+1(最坏 BMP 全 3 字节 + NUL)。
    // units*3+4 只是选取用的保守键;真正分配用 3*units+1,这才是该串的合法容量上界。
    uint16_t worst_units = 0;
    const bool worst_ok = steins_pack_str(pack, worst, &worst_units) != NULL && worst_units > 0u;
    CHECK(worst_ok, "cannot resolve worst string units");
    char *big = worst_ok ? (char *)malloc((size_t)worst_units * 3u + 1u) : NULL;
    CHECK(big != NULL, "cannot allocate utf8 buffer");
    int n = -1;
    if (big) {
        const size_t cap = (size_t)worst_units * 3u + 1u;
        memset(big, 0x5A, cap);
        n = steins_pack_str_utf8(pack, worst, big, cap);
        CHECK(n >= 0, "longest string failed to transcode (%d)", n);
        /* n 先证实有效再下标:否则 n == -1 时 big[n] 越界(ZigUBSan 会 trap)。 */
        CHECK(n >= 0 && (size_t)n < cap && big[n] == 0,
              "longest string not NUL terminated (n=%d cap=%zu)", n, cap);
        /* 2048 负控:条件用 n >= sizeof(tight),UTF-8 恰等于 cap 时也放不下 NUL。 */
        if (n >= 0 && (size_t)n >= 2048u) {
            char tight[2048];
            memset(tight, 0x5A, sizeof(tight));
            const int refused = steins_pack_str_utf8(pack, worst, tight, sizeof(tight));
            CHECK(refused == -1, "undersized buffer unexpectedly accepted (%d)", refused);
        }
        free(big);
    }

    // 缓冲太小:返回 -1,但缓冲里是 NUL 结尾的合法前缀,不会残留脏字节。
    char small[16];
    memset(small, 0x5A, sizeof(small));
    const int m = steins_pack_str_utf8(pack, worst, small, sizeof(small));
    CHECK(m == -1, "small buffer returned %d instead of -1", m);
    const size_t small_len = strnlen(small, sizeof(small));
    CHECK(small_len < sizeof(small), "small buffer has no NUL inside its capacity");
    CHECK(small_len > 0, "small buffer kept no prefix at all");
    for (size_t i = small_len + 1u; i < sizeof(small); ++i) {
        CHECK(small[i] == 0x5A, "byte after the NUL was modified (index %zu)", i);
    }
    // 失败前缀必须是完整 UTF-8 码点:按 utf8_next 走一遍不能有残缺字节。
    {
        size_t pos = 0;
        while (pos < small_len) {
            const size_t next = steins_utf8_next(small, small_len, pos);
            CHECK(next > pos, "dangling byte in prefix at %zu", pos);
            pos = next;
        }
    }

    // cap == 0:不写任何字节。
    char zero[4] = { 'A', 'B', 'C', 'D' };
    CHECK(steins_pack_str_utf8(pack, worst, zero, 0) == -1, "cap 0 accepted");
    CHECK(zero[0] == 'A' && zero[3] == 'D', "cap 0 wrote into the buffer");

    // 非法下标:返回 -1 且缓冲被清成空串。
    char dirty[8];
    memset(dirty, 0x5A, sizeof(dirty));
    CHECK(steins_pack_str_utf8(pack, 0xFFFFu, dirty, sizeof(dirty)) == -1, "bad index accepted");
    CHECK(dirty[0] == 0, "bad index left a dirty buffer");

    // 脏缓冲再写正常串:不能有尾部旧内容。
    memset(dirty, 0x5A, sizeof(dirty));
    char *src = (char *)malloc(4096);
    CHECK(src != NULL, "alloc");
    if (src) {
        const int full = steins_pack_str_utf8(pack, worst, src, 4096);
        CHECK(full > 0, "full decode failed");
        const int again = steins_pack_str_utf8(pack, worst, dirty, 8);
        CHECK(again == -1, "dirty small buffer not rejected");
        for (size_t i = strnlen(dirty, sizeof(dirty)) + 1u; i < sizeof(dirty); ++i) {
            CHECK(dirty[i] == 0x5A, "dirty tail byte %zu overwritten", i);
        }
        free(src);
    }
    puts("  str_utf8 safety checked");
}

static void test_frames_and_views(steins_pack_t *pack)
{
    const uint32_t nodes = steins_pack_count(pack, STEINS_SEC_NODE);
    const uint32_t frames = steins_pack_count(pack, STEINS_SEC_FRAME);
    const uint32_t views = steins_pack_count(pack, STEINS_SEC_VIEW);
    const uint32_t assets = steins_pack_count(pack, STEINS_SEC_ASSET);
    const uint32_t chunks = steins_pack_count(pack, STEINS_SEC_CHUNK);
    CHECK(nodes == 74, "node count %u != 74", (unsigned)nodes);
    CHECK(frames > 30000, "frame count %u too small", (unsigned)frames);
    CHECK(views > 4000, "view count %u too small", (unsigned)views);
    CHECK(assets > 1000, "asset count %u too small", (unsigned)assets);
    CHECK(steins_pack_count(pack, STEINS_SEC_LAYER) > 10000, "layer count too small");

    uint64_t total_frames = 0;
    uint32_t bad_views = 0;
    uint32_t blank_frames = 0;
    uint32_t paged_frames = 0;
    uint32_t max_pages = 0;
    uint32_t char_mismatch = 0;

    for (uint32_t i = 0; i < nodes; ++i) {
        steins_node_t node;
        CHECK(steins_pack_node(pack, (uint16_t)i, &node), "node %u unreadable", (unsigned)i);
        uint64_t sum = 0;
        for (uint8_t c = 0; c < node.chunk_count; ++c) {
            uint16_t chunk_index = 0;
            steins_chunk_t chunk;
            CHECK(steins_pack_node_chunk(pack, &node, c, &chunk_index),
                  "node %u chunk %u unreadable", (unsigned)i, (unsigned)c);
            CHECK(chunk_index < chunks, "node %u chunk index %u out of range", (unsigned)i,
                  (unsigned)chunk_index);
            CHECK(steins_pack_chunk(pack, chunk_index, &chunk), "chunk %u unreadable",
                  (unsigned)chunk_index);
            CHECK((uint64_t)chunk.first_frame + chunk.count <= frames,
                  "chunk %u frames out of range", (unsigned)chunk_index);
            sum += chunk.count;
        }
        CHECK(sum == node.count, "node %u frames %llu != count %u", (unsigned)i,
              (unsigned long long)sum, (unsigned)node.count);
        total_frames += node.count;
    }
    CHECK(total_frames > 130000, "reachable frame span %llu too small",
          (unsigned long long)total_frames);

    for (uint32_t i = 0; i < frames; ++i) {
        steins_frame_t frame;
        CHECK(steins_pack_frame(pack, i, &frame), "frame %u unreadable", (unsigned)i);
        CHECK(frame.view < views, "frame %u view %u out of range", (unsigned)i,
              (unsigned)frame.view);
        char text[STEINS_TEXT_BYTES];
        char speaker[256];
        CHECK(steins_pack_str_utf8(pack, frame.text, text, sizeof(text)) >= 0,
              "frame %u text too long", (unsigned)i);
        CHECK(steins_pack_str_utf8(pack, frame.speaker, speaker, sizeof(speaker)) >= 0,
              "frame %u speaker too long", (unsigned)i);
        char aux[128];
        char title[160];
        CHECK(steins_pack_str_utf8(pack, frame.aux, aux, sizeof(aux)) >= 0,
              "frame %u date too long", (unsigned)i);
        CHECK(steins_pack_str_utf8(pack, frame.title, title, sizeof(title)) >= 0,
              "frame %u title too long", (unsigned)i);

        if (steins_text_blank(text)) {
            ++blank_frames;
            continue;
        }
        // 逐页排版:每页首尾相接,页数与字符数必须自洽。
        steins_text_page_t page;
        const int pages = steins_page_build(text, STEINS_TEXT_SIZE, STEINS_TEXT_W,
                                            STEINS_TEXT_LINES, 0, &page);
        CHECK(pages >= 1, "frame %u pagination returned %d", (unsigned)i, pages);
        if (pages <= 0) continue;
        if (pages > 1) ++paged_frames;
        if ((uint32_t)pages > max_pages) max_pages = (uint32_t)pages;

        size_t expected = strlen(text);
        size_t seen = 0;
        for (int p = 0; p < pages; ++p) {
            const int got = steins_page_build(text, STEINS_TEXT_SIZE, STEINS_TEXT_W,
                                              STEINS_TEXT_LINES, p, &page);
            CHECK(got == pages, "frame %u page %d reported %d pages", (unsigned)i, p, got);
            CHECK(page.chars > 0, "frame %u page %d has no chars", (unsigned)i, p);
            CHECK(page.offsets[page.chars] == (uint16_t)page.bytes,
                  "frame %u page %d offsets do not end at text end", (unsigned)i, p);
            seen += (size_t)page.chars;
            // 显示的字节数不能超过原文（只多出行分隔符：每页最多 lines-1 个）
            CHECK((size_t)page.bytes <= expected + (size_t)pages * (STEINS_TEXT_LINES - 1),
                  "frame %u page %d bytes %d > source %zu", (unsigned)i, p, page.bytes,
                  expected);
        }
        // 字符总数必须等于原文的字符数:少一个就是排版吞字。
        // 硬换行在排版里是结构字符(不占可见字符位),比较时排除。
        size_t src_chars = 0;
        for (size_t pos = 0; pos < expected;) {
            const size_t next = steins_utf8_next(text, expected, pos);
            if (next == pos) break;
            if (!(next == pos + 1u && text[pos] == 0x0A)) ++src_chars;
            pos = next;
        }
        if (seen != src_chars) {
            ++char_mismatch;
            if (char_mismatch < 4) {
                fprintf(stderr, "  frame %u chars %zu != source %zu\n", (unsigned)i, seen,
                        src_chars);
            }
        }
    }
    CHECK(char_mismatch == 0, "%u frames lost characters while paginating", char_mismatch);
    printf("  frames=%u blank=%u paged=%u max_pages=%u\n", (unsigned)frames,
           (unsigned)blank_frames, (unsigned)paged_frames, (unsigned)max_pages);

    for (uint32_t v = 0; v < views; ++v) {
        steins_view_t view;
        CHECK(steins_pack_view(pack, (uint16_t)v, &view), "view %u unreadable", (unsigned)v);
        if (view.count == 0) continue;   // 源里有整屏黑的空画面
        for (uint8_t l = 0; l < view.count; ++l) {
            steins_layer_t layer;
            CHECK(steins_pack_layer(pack, (uint16_t)(view.layer_first + l), &layer),
                  "view %u layer %u unreadable", (unsigned)v, (unsigned)l);
            CHECK(layer.asset < assets, "view %u layer %u asset %u out of range",
                  (unsigned)v, (unsigned)l, (unsigned)layer.asset);
            steins_asset_t asset;
            CHECK(steins_pack_asset(pack, layer.asset, &asset), "asset %u unreadable",
                  (unsigned)layer.asset);
            CHECK(layer.w > 0 && layer.h > 0, "view %u layer %u empty rect", (unsigned)v,
                  (unsigned)l);
            CHECK(asset.cx + asset.cw <= asset.ow && asset.cy + asset.ch <= asset.oh,
                  "asset %u crop box outside source", (unsigned)layer.asset);
            // 目标矩形必须与画布相交,否则这一层永远画不出来。
            const int x0 = layer.x + (layer.w * (int)asset.cx) / (int)asset.ow;
            const int x1 = layer.x + (layer.w * (int)(asset.cx + asset.cw)) / (int)asset.ow;
            const int y0 = layer.y + (layer.h * (int)asset.cy) / (int)asset.oh;
            const int y1 = layer.y + (layer.h * (int)(asset.cy + asset.ch)) / (int)asset.oh;
            if (x1 <= 0 || x0 >= STEINS_CANVAS_W || y1 <= 0 || y0 >= STEINS_CANVAS_H) {
                ++bad_views;
            }
        }
    }
    CHECK(bad_views == 0, "%u layers never intersect the canvas", bad_views);
}

static void test_choices(steins_pack_t *pack)
{
    const uint32_t nodes = steins_pack_count(pack, STEINS_SEC_NODE);
    uint32_t choice_nodes = 0;
    uint32_t items = 0;
    uint32_t endings = 0;
    for (uint32_t i = 0; i < nodes; ++i) {
        steins_node_t node;
        steins_pack_node(pack, (uint16_t)i, &node);
        if (node.ending != STEINS_NONE) ++endings;
        if (node.choice == STEINS_NONE) continue;
        ++choice_nodes;
        steins_choice_t choice;
        CHECK(steins_pack_choice(pack, node.choice, &choice), "node %u choice unreadable",
              (unsigned)i);
        CHECK(choice.count > 0, "node %u choice has no items", (unsigned)i);
        CHECK(choice.kind <= STEINS_KIND_REPLY, "node %u choice kind %u invalid",
              (unsigned)i, (unsigned)choice.kind);
        for (uint8_t k = 0; k < choice.count; ++k) {
            steins_citem_t item;
            CHECK(steins_pack_citem(pack, (uint16_t)(choice.first_item + k), &item),
                  "node %u item %u unreadable", (unsigned)i, (unsigned)k);
            CHECK(item.to < nodes, "node %u item %u target %u out of range", (unsigned)i,
                  (unsigned)k, (unsigned)item.to);
            CHECK(item.action <= STEINS_ACT_DMAIL, "node %u item %u action %u invalid",
                  (unsigned)i, (unsigned)k, (unsigned)item.action);
            char label[128];
            CHECK(steins_pack_str_utf8(pack, item.text, label, sizeof(label)) >= 0,
                  "node %u item %u text too long", (unsigned)i, (unsigned)k);
            ++items;
        }
    }
    CHECK(choice_nodes == 65, "choice node count %u != 65 (source graph)", choice_nodes);
    CHECK(items >= 136, "choice item count %u too small", items);
    CHECK(endings >= 9, "ending node count %u != 9", endings);
    printf("  choice_nodes=%u items=%u endings=%u\n", (unsigned)choice_nodes, (unsigned)items,
           (unsigned)endings);

    const uint32_t chapters = steins_pack_count(pack, STEINS_SEC_CHAPTER);
    CHECK(chapters == 13, "chapter count %u != 13", (unsigned)chapters);
    for (uint32_t c = 0; c < chapters; ++c) {
        steins_chapter_t chapter;
        CHECK(steins_pack_chapter(pack, (uint16_t)c, &chapter), "chapter %u unreadable",
              (unsigned)c);
        CHECK(chapter.node < nodes, "chapter %u node %u out of range", (unsigned)c,
              (unsigned)chapter.node);
        steins_node_t node;
        steins_pack_node(pack, chapter.node, &node);
        CHECK(chapter.cursor <= node.count, "chapter %u cursor %u beyond node", (unsigned)c,
              (unsigned)chapter.cursor);
        char name[128];
        CHECK(steins_pack_str_utf8(pack, chapter.name, name, sizeof(name)) >= 0,
              "chapter %u name too long", (unsigned)c);
    }
}

// 选择点 heading 就是邮件正文:必须能完整转码,并且按 UI 的 6 行页面逐页拼回原文。
static void test_choice_headings(steins_pack_t *pack)
{
    const uint32_t nodes = steins_pack_count(pack, STEINS_SEC_NODE);
    static char full[4096];
    // 与 main/steins_app.h 的 STEINS_CHOICE_TEXT 一致(640);这里不引 app 头。
    static char appbuf[640];
    static char joined[STEINS_TEXT_BYTES];
    int long_count = 0;
    int max_bytes = 0;
    int checked = 0;

    for (uint32_t i = 0; i < nodes; ++i) {
        steins_node_t node;
        if (!steins_pack_node(pack, (uint16_t)i, &node)) continue;
        if (node.choice == STEINS_NONE) continue;
        steins_choice_t choice;
        if (!steins_pack_choice(pack, node.choice, &choice)) continue;
        const int n = steins_pack_str_utf8(pack, choice.heading, full, sizeof(full));
        CHECK(n >= 0, "node %u heading does not transcode (%d)", (unsigned)i, n);
        if (n < 0) continue;
        if (n >= 192) ++long_count;
        if (n > max_bytes) max_bytes = n;
        // app 侧的缓冲大小(640)也必须装得下。
        const int a = steins_pack_str_utf8(pack, choice.heading, appbuf, sizeof(appbuf));
        CHECK(a == n, "node %u heading truncated at app buffer (%d != %d)", (unsigned)i, a, n);

        // 逐页拼回:页面之间插入的换行分隔符不算内容。
        steins_text_page_t page;
        const int pages = steins_page_build(appbuf, STEINS_TEXT_SIZE, STEINS_TEXT_W, 6, 0,
                                            &page);
        CHECK(pages >= 1, "node %u heading pagination returned %d", (unsigned)i, pages);
        size_t used = 0;
        for (int p = 0; p < pages; ++p) {
            CHECK(steins_page_build(appbuf, STEINS_TEXT_SIZE, STEINS_TEXT_W, 6, p, &page) ==
                      pages,
                  "node %u page %d page-count drift", (unsigned)i, p);
            for (int k = 0; k < page.bytes; ++k) {
                if (page.text[k] == '\n') continue;
                CHECK(used + 1u < sizeof(joined), "node %u heading too long to rejoin",
                      (unsigned)i);
                if (used + 1u >= sizeof(joined)) break;
                joined[used++] = page.text[k];
            }
        }
        joined[used] = 0;
        // 硬换行是排版分隔,两边都去掉再比内容。
        static char flat[640];
        size_t flat_n = 0;
        for (int k = 0; appbuf[k] && flat_n + 1u < sizeof(flat); ++k) {
            if (appbuf[k] == '\n') continue;
            flat[flat_n++] = appbuf[k];
        }
        flat[flat_n] = 0;
        CHECK(strcmp(joined, flat) == 0, "node %u heading lost bytes when rejoining pages",
              (unsigned)i);
        ++checked;
    }
    CHECK(checked >= 65, "only %d choices checked", checked);
    CHECK(long_count >= 11, "only %d headings >= 192 B (source has 11)", long_count);
    CHECK(max_bytes >= 500, "longest heading %d B (source longest is 509)", max_bytes);
    printf("  choice headings: %d checked, %d >=192 B, longest %d B\n", checked, long_count,
           max_bytes);
}

static void test_save_and_unlock(void)
{
    steins_pos_t pos = { 42, 12345, STEINS_MODE_CHAPTER, 0 };
    uint8_t buf[STEINS_SAVE_BYTES];
    CHECK(steins_save_encode(&pos, buf, sizeof(buf)) == STEINS_SAVE_BYTES, "encode failed");
    steins_pos_t back;
    memset(&back, 0, sizeof(back));
    CHECK(steins_save_decode(&back, buf, sizeof(buf)), "decode failed");
    CHECK(back.node == pos.node && back.cursor == pos.cursor && back.mode == pos.mode,
          "roundtrip mismatch");
    // CRC 篡改
    uint8_t broken[STEINS_SAVE_BYTES];
    memcpy(broken, buf, sizeof(buf));
    broken[7] ^= 0x40u;
    CHECK(!steins_save_decode(&back, broken, sizeof(broken)), "CRC failure accepted");
    // 版本不符
    memcpy(broken, buf, sizeof(buf));
    broken[2] = 99;
    CHECK(!steins_save_decode(&back, broken, sizeof(broken)), "bad version accepted");
    // 模式非法
    memcpy(broken, buf, sizeof(buf));
    broken[3] = 7;
    CHECK(!steins_save_decode(&back, broken, sizeof(broken)), "bad mode accepted");
    // 长度不足
    CHECK(!steins_save_decode(&back, buf, 4), "short buffer accepted");

    steins_unlock_t a, b;
    steins_unlock_init(&a);
    steins_unlock_init(&b);
    CHECK(steins_unlock_set_ending(&a, 3), "set ending");
    CHECK(!steins_unlock_set_ending(&a, 3), "duplicate set must report no change");
    steins_unlock_set_chapter(&b, 5);
    steins_unlock_set_gallery(&b, 77);
    steins_unlock_set_true_end(&b);
    CHECK(steins_unlock_merge(&a, &b), "merge should report changes");
    CHECK(steins_unlock_has_ending(&a, 3), "merge lost ending");
    CHECK(steins_unlock_has_chapter(&a, 5), "merge lost chapter");
    CHECK(steins_unlock_has_gallery(&a, 77), "merge lost gallery");
    CHECK(steins_unlock_true_end(&a), "merge lost true end");
    CHECK(!steins_unlock_merge(&a, &b), "second merge must report no change");
    // 序列化 + 截断
    uint8_t ub[STEINS_UNLOCK_BYTES];
    CHECK(steins_unlock_encode(&a, ub, sizeof(ub)) == STEINS_UNLOCK_BYTES, "unlock encode");
    steins_unlock_t c;
    CHECK(steins_unlock_decode(&c, ub, sizeof(ub)), "unlock decode");
    CHECK(steins_unlock_has_gallery(&c, 77), "unlock roundtrip lost gallery");
    ub[10] ^= 0x01u;
    CHECK(!steins_unlock_decode(&c, ub, sizeof(ub)), "unlock CRC failure accepted");
}

static void test_worldline_and_cards(steins_pack_t *pack)
{
    char text[24];
    steins_worldline_text(1130426, text, sizeof(text));
    CHECK(strcmp(text, "1.130426") == 0, "worldline text %s", text);
    steins_worldline_text(-275349, text, sizeof(text));
    CHECK(strcmp(text, "-0.275349") == 0, "negative worldline text %s", text);
    steins_worldline_text(1048596, text, sizeof(text));
    CHECK(strcmp(text, "1.048596") == 0, "true worldline text %s", text);

    const int initial = steins_pack_meta_int(pack, "initial", -1);
    CHECK(initial >= 0 && initial < (int)steins_pack_count(pack, STEINS_SEC_NODE),
          "meta initial %d out of range", initial);
    steins_node_t initial_node;
    CHECK(steins_pack_node(pack, (uint16_t)initial, &initial_node), "initial node unreadable");
    char id[8];
    CHECK(steins_pack_str_utf8(pack, initial_node.id_str, id, sizeof(id)) > 0, "initial id");
    CHECK(strcmp(id, "n0") == 0, "source initial node is %s, expected n0", id);
    CHECK(steins_worldline_rate(pack, (uint16_t)initial, 0) == 1130426,
          "initial worldline %d", (int)steins_worldline_rate(pack, (uint16_t)initial, 0));
    steins_wline_t row;
    CHECK(steins_pack_wline(pack, (uint16_t)initial, &row), "wline for initial node");
    if (row.count > 0) {
        steins_wchange_t change;
        CHECK(steins_pack_wchange(pack, row.first, &change), "wchange unreadable");
        CHECK(steins_worldline_rate(pack, (uint16_t)initial, change.cursor) == change.rate,
              "worldline change not applied at cursor");
        if (change.cursor > 0) {
            CHECK(steins_worldline_rate(pack, (uint16_t)initial, change.cursor - 1u) == row.rate,
                  "worldline applied too early");
        }
    }

    // 章节卡:1..9 -> 487..495,10 两个变体,11 -> 498
    CHECK(steins_chapter_card_asset(pack, "01 \xC2\xB7 ab") != STEINS_NONE, "card for 01");
    CHECK(steins_chapter_card_asset(pack, "12 x") == STEINS_NONE, "no card for 12");
    const uint16_t causal = steins_chapter_card_asset(
        pack, "10 \xC2\xB7 \xE5\x9B\xA0\xE6\x9E\x9C\xE5\x85\xB3\xE7\xB3\xBB\xE7\x9A\x84"
              "\xE6\xBA\xB6\xE8\xA7\xA3");
    const uint16_t pure = steins_chapter_card_asset(
        pack, "10 \xC2\xB7 \xE7\xBA\xAF\xE6\xB4\x81\xE9\x80\x8F\xE6\x98\x8E\xE7\x9A\x84"
              "\xE6\x98\x9F\xE5\xB0\x98");
    CHECK(causal != STEINS_NONE && pure != STEINS_NONE && causal != pure,
          "chapter 10 needs two distinct cards (%u/%u)", (unsigned)causal, (unsigned)pure);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/steins/steins_pack.bin";
    uint32_t size = 0;
    uint8_t *data = load_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read pack: %s\n", path);
        return 2;
    }
    printf("pack %s (%u bytes)\n", path, (unsigned)size);

    steins_pack_t pack;
    if (!steins_pack_open(&pack, data, size)) {
        fprintf(stderr, "pack rejected by steins_pack_open\n");
        free(data);
        return 2;
    }
    test_pack_rejects(data, size);
    test_pack_structure_negatives(data, size);
    test_str_utf8_safety(&pack);
    test_frames_and_views(&pack);
    test_choices(&pack);
    test_choice_headings(&pack);
    test_save_and_unlock();
    test_worldline_and_cards(&pack);
    free(data);

    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
