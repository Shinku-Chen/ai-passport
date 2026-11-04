// tests/test_summer_text.c —— 16px 三行分页、UTF-8、行首禁则。
#include "summer_text.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

int main(void)
{
    const summer_layout_t layout = { 36, 3 };
    uint32_t offsets[SUMMER_MAX_PAGES + 1];
    char page[SUMMER_PAGE_TEXT_CAP];

    // 20 CJK chars = 40 units, so two lines, one page.
    const char *short_text = "あいうえおかきくけこさしすせそたちつてと";
    CHECK(summer_text_pages(short_text, &layout, offsets, SUMMER_MAX_PAGES + 1) == 1,
          "short pages");
    CHECK(offsets[1] == strlen(short_text), "short end");
    CHECK(summer_text_page_text(short_text, &layout, 0, NULL, NULL, page, sizeof(page)),
          "short page text");
    CHECK(strchr(page, '\n') != NULL, "short page not wrapped");
    char flat[SUMMER_PAGE_TEXT_CAP];
    size_t fi = 0;
    for (size_t i = 0; page[i] && fi + 1 < sizeof(flat); ++i) {
        if (page[i] != '\n') flat[fi++] = page[i];
    }
    flat[fi] = 0;
    CHECK(strcmp(flat, short_text) == 0, "short text changed: %s", flat);

    // 120 CJK = 7 lines = 3 pages (18 chars/line, 3 lines/page).
    char long_text[400];
    long_text[0] = 0;
    for (int i = 0; i < 120; ++i) strcat(long_text, "漢");
    CHECK(summer_text_pages(long_text, &layout, offsets, SUMMER_MAX_PAGES + 1) >= 3,
          "long pages %d", summer_text_pages(long_text, &layout, offsets, SUMMER_MAX_PAGES + 1));
    uint32_t start = 0, end = 0;
    for (int p = 0; p < 3; ++p) {
        CHECK(summer_text_page_text(long_text, &layout, p, &start, &end, page, sizeof(page)),
              "long page %d", p);
        CHECK(start % 3 == 0 && end % 3 == 0, "split UTF-8 page %d: %u..%u", p, start, end);
        CHECK(strlen(page) > 0, "empty page %d", p);
    }

    // Explicit newline forces a line break but stays in one page.
    const char *two = "第一行\n第二行\n第三行";
    CHECK(summer_text_pages(two, &layout, offsets, SUMMER_MAX_PAGES + 1) == 1, "newline pages");
    CHECK(summer_text_page_text(two, &layout, 0, NULL, NULL, page, sizeof(page)), "newline text");
    CHECK(strstr(page, "第一行\n第二行\n第三行") != NULL, "newline lost: %s", page);

    // Closing punctuation must not start the next line.
    char punct[200];
    memset(punct, 'a', 36);
    punct[36] = 0;
    strcat(punct, "。");
    CHECK(summer_text_pages(punct, &layout, offsets, SUMMER_MAX_PAGES + 1) == 1, "punct pages");
    CHECK(summer_text_page_text(punct, &layout, 0, NULL, NULL, page, sizeof(page)), "punct text");
    CHECK(strstr(page, "a。") != NULL || strstr(page, "。") != NULL, "punct lost");

    // UTF-8 cursor.
    const char *mixed = "A漢";
    CHECK(summer_utf8_char_len(mixed, strlen(mixed), 0) == 1, "ascii len");
    CHECK(summer_utf8_char_len(mixed, strlen(mixed), 1) == 3, "cjk len");
    CHECK(summer_utf8_next(mixed, strlen(mixed), 1) == 4, "next");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_text: PASS\n");
    return 0;
}
