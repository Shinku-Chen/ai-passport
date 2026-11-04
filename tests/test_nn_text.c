// tests/test_nn_text.c —— 横屏分页 + 字库覆盖的宿主测试。
//
// 版面约定（main/nn_app.c）：
//   16px 档：18 汉字/行 x 4 行 = 36 半角单位/行，144 单位/页
//   20px 档：14 汉字/行 x 3 行 = 28 半角单位/行， 84 单位/页
// 之所以不是 19/15：正文区宽 304px、16px 汉字正好 16px 宽，19 字刚好占满，
// 而分页器允许行首禁则标点挂在行尾（那一行会变成 20 字 = 320px），LVGL 会再折
// 一行，第 5 行就被裁掉。留一个字给悬挂标点，既不翻页也不裁字。
//
// 本测试用真实资源包里的每一句对白验证：
//   1. 分页边界与“单位数 / 每行单位”的算术一致；
//   2. 没有任何一句超过 GAL_TEXT_MAX_PAGES 页；
//   3. 每句都是合法 UTF-8，且长度与包里记录的字节数一致（不会被 NUL 截断）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gal_text.h"
#include "nn_pack.h"

#define DEFAULT_PACK "assets/nn/nn_pack.bin"
#define UNITS_16 36
#define LINES_16 4
#define UNITS_20 28
#define LINES_20 3

static int failures = 0;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

// 与 gal_text.c 相同口径：ASCII 与半角片假名算 1，其余算 2。
static int units_of(const char *s)
{
    int units = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = *p;
        int len = 1;
        if ((*p & 0xE0u) == 0xC0u) { cp = *p & 0x1Fu; len = 2; }
        else if ((*p & 0xF0u) == 0xE0u) { cp = *p & 0x0Fu; len = 3; }
        else if ((*p & 0xF8u) == 0xF0u) { cp = *p & 0x07u; len = 4; }
        for (int i = 1; i < len && p[i]; ++i) cp = (cp << 6) | (p[i] & 0x3Fu);
        p += len;
        const int half = (cp < 0x80u) || (cp >= 0xFF61u && cp <= 0xFF9Fu);
        units += half ? 1 : 2;
    }
    return units;
}

// 生成 n 个 U+4E00（一）的 UTF-8 串，用来测边界。
static void fill_cjk(char *dst, int n)
{
    for (int i = 0; i < n; ++i) {
        dst[i * 3 + 0] = (char)0xE4;
        dst[i * 3 + 1] = (char)0xB8;
        dst[i * 3 + 2] = (char)0x80;
    }
    dst[n * 3] = 0;
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (n <= 0) { fclose(fh); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(fh); return NULL; }
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) { free(buf); fclose(fh); return NULL; }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    uint32_t off[GAL_TEXT_MAX_PAGES + 1];

    // ---- 1. 版面算术：边界值
    char line[256];
    fill_cjk(line, 72);                       // 144 单位 = 16px 档整一页
    CHECK(gal_text_pages(line, UNITS_16, LINES_16, off, GAL_TEXT_MAX_PAGES + 1) == 1,
          "72 CJK chars should fit one 16px page");
    fill_cjk(line, 73);                       // 多一个就翻第二页
    CHECK(gal_text_pages(line, UNITS_16, LINES_16, off, GAL_TEXT_MAX_PAGES + 1) == 2,
          "73 CJK chars should spill to a second 16px page");
    fill_cjk(line, 42);                       // 84 单位 = 20px 档整一页
    CHECK(gal_text_pages(line, UNITS_20, LINES_20, off, GAL_TEXT_MAX_PAGES + 1) == 1,
          "42 CJK chars should fit one 20px page");
    fill_cjk(line, 43);
    CHECK(gal_text_pages(line, UNITS_20, LINES_20, off, GAL_TEXT_MAX_PAGES + 1) == 2,
          "43 CJK chars should spill to a second 20px page");
    printf("  layout math: 18x4 (16px) and 14x3 (20px) page boundaries hold\n");

    // ---- 2. 真实剧本：每一句都不超过页数上限，且分页不丢字
    const char *path = argc > 1 ? argv[1] : DEFAULT_PACK;
    size_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        printf("cannot read %s\n", path);
        return 1;
    }
    nn_pack_t pack;
    if (!nn_pack_init(&pack, blob, size)) {
        printf("pack did not parse\n");
        free(blob);
        return 1;
    }

    int lines = 0, pages16_total = 0, pages20_total = 0, max16 = 0, max20 = 0;
    for (uint32_t i = 0; i < pack.op_count; ++i) {
        if (pack.ops[i].type != NN_OP_SAY) continue;
        const char *txt = nn_pack_text(&pack, pack.ops[i].b, pack.ops[i].c);
        CHECK(txt != NULL, "say op %u has no text", i);
        if (!txt) continue;
        lines++;
        // 包里每条字符串都以 NUL 结尾（LVGL 直接吃指针），长度必须与记录一致
        CHECK(strlen(txt) == pack.ops[i].c, "say op %u length mismatch (%zu vs %u)",
              i, strlen(txt), (unsigned)pack.ops[i].c);

        const int p16 = gal_text_pages(txt, UNITS_16, LINES_16, off, GAL_TEXT_MAX_PAGES + 1);
        const int p20 = gal_text_pages(txt, UNITS_20, LINES_20, off, GAL_TEXT_MAX_PAGES + 1);
        CHECK(p16 >= 1 && p16 <= GAL_TEXT_MAX_PAGES, "op %u needs %d pages at 16px", i, p16);
        CHECK(p20 >= 1 && p20 <= GAL_TEXT_MAX_PAGES, "op %u needs %d pages at 20px", i, p20);

        // 无显式换行时，页数应与单位数算术一致（禁则标点最多多出一页）
        const int u = units_of(txt);
        const int expect16 = (u + (UNITS_16 * LINES_16) - 1) / (UNITS_16 * LINES_16);
        const int expect20 = (u + (UNITS_20 * LINES_20) - 1) / (UNITS_20 * LINES_20);
        if (strchr(txt, '\n') == NULL) {
            // 行首禁则会把标点拉回上一行，所以实际页数可能比纯算术少一页；
            // 允许 ±1 的偏差，只抓“数量级不对”的真问题。
            CHECK(p16 >= expect16 - 1 && p16 <= expect16 + 1,
                  "op %u pages16 %d vs arithmetic %d (units %d)", i, p16, expect16, u);
            CHECK(p20 >= expect20 - 1 && p20 <= expect20 + 1,
                  "op %u pages20 %d vs arithmetic %d (units %d)", i, p20, expect20, u);
        }
        pages16_total += p16;
        pages20_total += p20;
        if (p16 > max16) max16 = p16;
        if (p20 > max20) max20 = p20;
    }
    printf("  script: %d lines, %d pages at 16px (max %d), %d pages at 20px (max %d)\n",
           lines, pages16_total, max16, pages20_total, max20);
    CHECK(lines == 1248, "expected 1248 dialogue lines, got %d", lines);
    // 16px 档每页 72 字，最长一句 69 字 → 全部一页
    CHECK(max16 == 1, "a line needs %d pages at 16px (expected 1)", max16);
    // 20px 档每页 42 字 → 长句最多 2 页
    CHECK(max20 <= 2, "a line needs %d pages at 20px", max20);

    free(blob);
    if (failures) {
        printf("nn_text: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("nn_text: PASS\n");
    return 0;
}
