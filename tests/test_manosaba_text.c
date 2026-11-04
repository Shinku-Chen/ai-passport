#include "gal_text.h"
#include <stdio.h>
#include <string.h>

static int utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 :
                (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) return 0;
        for (int i = 1; i < n; ++i) {
            if ((p[i] & 0xC0) != 0x80) return 0;
        }
        p += n;
    }
    return 1;
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

int main(void)
{
    char page[512];
    int pages = 0;
    const char *long_text =
        "第一行文字很长很长很长很长很长很长很长很长很长很长很长很长\n"
        "第二行也不短也不短也不短也不短也不短也不短也不短也不短也不短\n"
        "第三行同样同样同样同样同样同样同样同样同样同样同样同样同样同样\n"
        "第四行继续继续继续继续继续继续继续继续继续继续继续继续继续";
    CHECK(gal_text_page(long_text, 0, 304, 16, 3, page, sizeof(page), &pages), "page0");
    CHECK(pages >= 2, "pages=%d", pages);
    CHECK(strlen(page) > 0, "page0 empty");
    CHECK(gal_text_page(long_text, 1, 304, 16, 3, page, sizeof(page), &pages), "page1");
    CHECK(strlen(page) > 0, "page1 empty");
    CHECK(!gal_text_page(long_text, 99, 304, 16, 3, page, sizeof(page), &pages), "page99 accepted");

    // 不能把 UTF-8 多字节切开:页尾字节不能是 continuation byte
    for (int p = 0; p < pages; ++p) {
        CHECK(gal_text_page(long_text, p, 304, 16, 3, page, sizeof(page), NULL), "page %d", p);
        CHECK(utf8_ok(page), "page %d invalid UTF-8", p);
    }

    char prefix[64];
    CHECK(gal_text_prefix("魔法少女abc", 4, prefix, sizeof(prefix)) == 4, "prefix count");
    CHECK(strcmp(prefix, "魔法少女") == 0, "prefix=%s", prefix);
    CHECK(gal_text_prefix("魔法少女abc", 100, prefix, sizeof(prefix)) > 0, "prefix clamp");
    CHECK(strcmp(prefix, "魔法少女abc") == 0, "prefix clamp=%s", prefix);
    CHECK(gal_text_cp_count("魔法少女abc") == 7, "cp count");
    CHECK(gal_text_cp_width(0x6BDB, 16) == 16, "cjk width");
    CHECK(gal_text_cp_width('a', 16) == 8, "ascii width");
    CHECK(gal_text_page("", 0, 304, 16, 3, page, sizeof(page), &pages) && pages == 1, "empty page");

    // 显式换行
    CHECK(gal_text_page("甲\n乙\n丙\n丁", 0, 304, 16, 3, page, sizeof(page), &pages), "explicit p0");
    CHECK(strstr(page, "甲") && strstr(page, "乙") && strstr(page, "丙"), "explicit content");
    CHECK(!strstr(page, "丁"), "explicit p0 leaked 丁");
    CHECK(gal_text_page("甲\n乙\n丙\n丁", 1, 304, 16, 3, page, sizeof(page), &pages), "explicit p1");
    CHECK(strstr(page, "丁") != NULL, "explicit p1 missing 丁");

    printf("test_gal_text: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
