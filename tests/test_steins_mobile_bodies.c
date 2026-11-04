// tests/test_steins_mobile_bodies.c —— 手机/邮件正文的分页容量回归。
//
// 为什么单独一个用例:旧实现按"65 个 choice heading 最长 509 B"设了 576/640 B 的
// 手机正文缓冲;但源里真正长的正文在 presentation/信件里(实测 id 1 = 2988 UTF-8 B /
// 996 个码点,id 263 = 1152 B,id 49 = 1302 B)。手机 reader 必须把这些正文完整分页。
//
// 本用例用的正文是从只读源快照 src/common/letter-packs 原样复制的 fixture
// (tests/fixtures/steins_mobile_bodies.tsv),不 import 打包器,也不需要资源包。
//
// 覆盖:
//   * 每个 fixture 正文都能分页,页数 > 1;
//   * 逐页拼回(去掉分页插入的换行)与原文逐字节相等 —— 一个字都不能丢;
//   * 每页字节数不超过 STEINS_TEXT_BYTES,且 offsets 收尾自洽;
//   * 超出折行工作区(STEINS_TEXT_CHARS 个码点)的文本必须返回 0,绝不静默截断。
#include "steins_model.h"

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

static const char *FIXTURE = "tests/fixtures/steins_mobile_bodies.tsv";

static size_t utf8_chars(const char *s)
{
    size_t count = 0;
    const size_t len = strlen(s);
    size_t pos = 0;
    while (pos < len) {
        const size_t next = steins_utf8_next(s, len, pos);
        if (next == pos) break;
        pos = next;
        ++count;
    }
    return count;
}

static void page_body(int id, const char *text, int expect_bytes)
{
    const size_t bytes = strlen(text);
    const size_t chars = utf8_chars(text);
    CHECK((int)bytes == expect_bytes, "id %d: fixture says %d B, file has %u B", id,
          expect_bytes, (unsigned)bytes);

    static char joined[STEINS_TEXT_BYTES * 4];
    size_t used = 0;
    steins_text_page_t page;
    const int pages = steins_page_build(text, STEINS_TEXT_SIZE, STEINS_TEXT_W, 6, 0, &page);
    CHECK(pages > 1, "id %d: %u B / %u chars produced %d page(s)", id, (unsigned)bytes,
          (unsigned)chars, pages);
    if (pages <= 0) return;

    for (int p = 0; p < pages; ++p) {
        const int again = steins_page_build(text, STEINS_TEXT_SIZE, STEINS_TEXT_W, 6, p, &page);
        CHECK(again == pages, "id %d: page %d reported %d pages", id, p, again);
        CHECK(page.chars > 0, "id %d: page %d is empty", id, p);
        CHECK(page.bytes > 0 && page.bytes < STEINS_TEXT_BYTES, "id %d: page %d bytes %d", id,
              p, page.bytes);
        CHECK(page.offsets[page.chars] == (uint16_t)page.bytes,
              "id %d: page %d offsets do not end at the page end", id, p);
        CHECK(page.text[page.bytes] == 0, "id %d: page %d not NUL terminated", id, p);
        // 逐字符边界:每个 offsets[] 必须落在 UTF-8 码点起点上
        for (int k = 0; k < page.chars; ++k) {
            const unsigned char c = (unsigned char)page.text[page.offsets[k]];
            CHECK((c & 0xC0u) != 0x80u, "id %d: page %d char %d is a continuation byte", id, p,
                  k);
        }
        for (int k = 0; k < page.bytes; ++k) {
            if (page.text[k] == '\n') continue;
            CHECK(used + 1u < sizeof(joined), "id %d: rejoined text too long", id);
            if (used + 1u >= sizeof(joined)) break;
            joined[used++] = page.text[k];
        }
    }
    joined[used] = 0;

    // 原文里的换行是内容的一部分(fixture 已把真正的换行换成空格),这里同样去掉分页换行。
    static char flat[STEINS_TEXT_BYTES * 4];
    size_t flat_n = 0;
    for (size_t k = 0; k < bytes && flat_n + 1u < sizeof(flat); ++k) {
        if (text[k] == '\n') continue;
        flat[flat_n++] = text[k];
    }
    flat[flat_n] = 0;
    CHECK(strcmp(joined, flat) == 0, "id %d: rejoined %u B != source %u B", id,
          (unsigned)strlen(joined), (unsigned)strlen(flat));
    printf("  id %3d: %4d B / %3u chars -> %d pages, rejoin OK\n", id, expect_bytes,
           (unsigned)chars, pages);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : FIXTURE;
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "cannot read fixture: %s\n", path);
        return 2;
    }
    char line[16384];
    int rows = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (line[0] == '#') continue;
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab1 = 0;
        *tab2 = 0;
        const int id = atoi(line);
        const int bytes = atoi(tab1 + 1);
        char *text = tab2 + 1;
        size_t len = strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = 0;
        page_body(id, text, bytes);
        ++rows;
    }
    fclose(fh);
    CHECK(rows >= 4, "fixture only had %d body row(s)", rows);

    // 超出工作区:STEINS_TEXT_CHARS 个码点以上必须报错(0),而不是返回被截断的页。
    {
        const size_t too_many = STEINS_TEXT_CHARS + 8;
        char *big = (char *)malloc(too_many * 3 + 8);
        CHECK(big != NULL, "alloc for oversize text failed");
        if (big) {
            size_t i = 0;
            for (; i < too_many; ++i) {
                big[i * 3 + 0] = (char)0xEE;
                big[i * 3 + 1] = (char)0x83;
                big[i * 3 + 2] = (char)0x98;
            }
            big[i * 3] = 0;
            steins_text_page_t page;
            const int pages = steins_page_build(big, STEINS_TEXT_SIZE, STEINS_TEXT_W, 6, 0,
                                                &page);
            CHECK(pages == 0, "oversize text returned %d pages instead of 0", pages);
            free(big);
        }
    }

    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK (%d bodies)\n", rows);
    return 0;
}
