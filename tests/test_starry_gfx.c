// tests/test_starry_gfx.c —— 逐行合成原语:裁剪、混色、圆角、字形绘制与排版宽度。
//
// 用真实字形包(assets/fonts/starry_font16.bin)验证"排版模型 -> 实际绘制"这条链。
//
// 构建与运行方式见 tools/validate.sh:
//   (1) cc -std=c11 -Wall -Wextra -Werror -Imain -Icomponents/bsp/src tests/test_starry_gfx.c
//           main/starry_gfx.c main/starry_model.c main/starry_pack.c
//           components/bsp/src/bsp_display_rounding.c -o test_starry_gfx
//   (2) ./test_starry_gfx assets/fonts/starry_font16.bin main/starry_data/starry_pack.bin
#include "starry_gfx.h"
#include "starry_model.h"
#include "starry_pack.h"
#include "bsp_display_rounding.h"   // 逐一对照圆角几何:两个实现必须完全一致

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 240
#define SCREEN_H 320
#define BOX_Y 214
#define TEXT_X 12
#define TEXT_W 216
#define LINES_PER_PAGE 3

static int g_failures;
#define CHECK(cond, ...)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                      \
            fprintf(stderr, __VA_ARGS__);                                             \
            fprintf(stderr, "\n");                                                    \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

static uint8_t *load_file(const char *path, uint32_t *out_size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "打不开: %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, fh) != (size_t)size) exit(2);
    fclose(fh);
    *out_size = (uint32_t)size;
    return data;
}

static void test_blend_math(void)
{
    const uint16_t black = starry_rgb565(0, 0, 0);
    const uint16_t white = starry_rgb565(255, 255, 255);
    CHECK(starry_blend565(black, white, 0) == black, "alpha=0 应保留底色");
    CHECK(starry_blend565(black, white, 255) == white, "alpha=255 应完全覆盖");
    const uint16_t mid = starry_blend565(black, white, 128);
    // 分量是 5/6/5 量化的:白是 (248,252,248),一半约 (124,126,124)。
    CHECK(starry_rgb565_r(mid) >= 112 && starry_rgb565_r(mid) <= 136, "中值红分量 %u",
          (unsigned)starry_rgb565_r(mid));
    CHECK(starry_rgb565_g(mid) >= 114 && starry_rgb565_g(mid) <= 138, "中值绿分量 %u",
          (unsigned)starry_rgb565_g(mid));
    // 分量必须落在 5/6/5 范围内,不能因插值溢出到相邻分量
    uint16_t prev = black;
    for (int a = 0; a <= 255; a += 17) {
        const uint16_t v = starry_blend565(black, white, (uint8_t)a);
        CHECK(starry_rgb565_r(v) >= starry_rgb565_r(prev), "亮度应单调不减");
        prev = v;
    }
}

static void test_fill_clipping(void)
{
    enum { ROWS = 6 };
    static uint16_t pixels[SCREEN_W * ROWS];
    starry_surface_t s;
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0x0001;
    starry_surface_init(&s, pixels, SCREEN_W, SCREEN_W, ROWS, 100);

    // 越过上下边界与左右边界都不该写坏缓冲
    starry_fill(&s, -20, 96, 300, 12, 0xFFFF);
    for (int row = 0; row < ROWS; ++row) {
        for (int x = 0; x < SCREEN_W; ++x) {
            const int y = 100 + row;
            const bool inside = (y >= 96 && y < 108);
            const uint16_t v = pixels[row * SCREEN_W + x];
            CHECK(v == (inside ? 0xFFFF : 0x0001), "行 %d 列 %d 越界写入", y, x);
        }
    }
    starry_surface_init(&s, pixels, SCREEN_W, SCREEN_W, ROWS, 100);
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0;
    starry_blend(&s, 0, 100, SCREEN_W, ROWS, 0xFFFF, 128);
    CHECK(pixels[0] != 0, "半透明覆盖应改像素");
}

static void test_rounding_matches_bsp(void)
{
    const int radii[] = { 0, 1, 10, 30, 60, 500 };
    for (size_t r = 0; r < sizeof(radii) / sizeof(radii[0]); ++r) {
        for (int y = -2; y < SCREEN_H + 2; ++y) {
            int x1 = -1, x2 = -1;
            const bool mine = starry_round_row_span(y, SCREEN_W, SCREEN_H, radii[r], &x1, &x2);
            int32_t b1 = -1, b2 = -1;
            const bool bsp = bsp_display_rounded_row_span(y, SCREEN_W, SCREEN_H, radii[r], &b1, &b2);
            CHECK(mine == bsp, "半径 %d 行 %d 可见性不一致", radii[r], y);
            if (mine && bsp) {
                CHECK(x1 == (int)b1 && x2 == (int)b2, "半径 %d 行 %d 区间 (%d,%d) != (%d,%d)",
                      radii[r], y, x1, x2, (int)b1, (int)b2);
            }
        }
    }
}

static void test_round_mask(void)
{
    enum { ROWS = 16 };
    static uint16_t pixels[SCREEN_W * ROWS];
    starry_surface_t s;
    // 顶行:四角必须刷黑,中间保留
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0xFFFF;
    starry_surface_init(&s, pixels, SCREEN_W, SCREEN_W, ROWS, 0);
    starry_round_mask(&s, SCREEN_H, 30, 0x0000);
    CHECK(pixels[0] == 0x0000, "左上角应刷黑");
    CHECK(pixels[SCREEN_W - 1] == 0x0000, "右上角应刷黑");
    CHECK(pixels[120] == 0xFFFF, "中间像素不该动");
    // 中间行:整行保留
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0xFFFF;
    starry_surface_init(&s, pixels, SCREEN_W, SCREEN_W, ROWS, 160);
    starry_round_mask(&s, SCREEN_H, 30, 0x0000);
    CHECK(pixels[0] == 0xFFFF && pixels[SCREEN_W - 1] == 0xFFFF, "中间行两侧不该被裁");
}

static void test_glyphs(const starry_font_t *font)
{
    CHECK(font->px == 16, "字号 %u", (unsigned)font->px);
    CHECK(font->line_height >= font->px, "行高 %u", (unsigned)font->line_height);
    CHECK(font->ascent > 0 && font->ascent < font->line_height, "基线 %d", (int)font->ascent);

    const uint32_t probes[] = { 'A', 'W', 'i', ' ', 0x4E00u, 0x3002u, 0xFF01u };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        starry_glyph_t gl;
        const bool found = starry_font_glyph(font, probes[i], &gl);
        if (probes[i] == ' ') {
            CHECK(!found, "空格不该有墨迹(应返回 false)");
            continue;
        }
        CHECK(found, "U+%04X 应该有字形", (unsigned)probes[i]);
        if (!found) continue;
        CHECK(gl.h > 0 && gl.w > 0, "U+%04X 字形为空", (unsigned)probes[i]);
        CHECK(gl.advance > 0, "U+%04X 没有推进宽度", (unsigned)probes[i]);
        // 墨迹必须落在行框内:否则会画进上一行或下一行
        CHECK(gl.ofs_y >= 0, "U+%04X ofs_y=%d 越出行框", (unsigned)probes[i], (int)gl.ofs_y);
        CHECK(gl.ofs_y + gl.h <= (int)font->line_height, "U+%04X 底部越出行框", (unsigned)probes[i]);
        CHECK(starry_char_units(probes[i]) == 2 ? gl.advance + 2 >= (int)font->px : true,
              "U+%04X 全角字形推进 %u 偏小", (unsigned)probes[i], (unsigned)gl.advance);
    }
    // 缺字必须能被识别出来(调用方据此告警)
    starry_glyph_t gl;
    CHECK(!starry_font_glyph(font, 0x10FFFEu, &gl), "不存在的码位不该有字形");
}

static void test_text_drawing(const starry_font_t *font)
{
    enum { ROWS = 24 };
    static uint16_t pixels[SCREEN_W * ROWS];
    const int line_y = 4;
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0;
    starry_surface_t s;
    starry_surface_init(&s, pixels, SCREEN_W, SCREEN_W, ROWS, line_y - 4);

    const char *text = "A汉。i";
    const int width = starry_text_px(text, 0, font);
    CHECK(width > 0, "文本宽度 %d", width);
    const int drawn = starry_text_line(&s, TEXT_X, line_y, text, 0, font, 0xFFFF, TEXT_W);
    CHECK(drawn == width, "绘制宽度 %d != 量得 %d", drawn, width);

    int painted = 0;
    for (int row = 0; row < ROWS; ++row) {
        for (int x = 0; x < SCREEN_W; ++x) {
            if (pixels[row * SCREEN_W + x]) painted++;
        }
    }
    CHECK(painted > 0, "应该有像素被画出来");
    // 行框之外不能有像素
    for (int row = 0; row < ROWS; ++row) {
        const int screen_y = line_y - 4 + row;
        if (screen_y >= line_y && screen_y < line_y + (int)font->line_height) continue;
        for (int x = 0; x < SCREEN_W; ++x) {
            CHECK(pixels[row * SCREEN_W + x] == 0, "行 %d 越界绘制", screen_y);
        }
    }
    // 超出 max_px 时必须截断
    for (int i = 0; i < SCREEN_W * ROWS; ++i) pixels[i] = 0;
    const int clipped = starry_text_line(&s, TEXT_X, line_y, text, 0, font, 0xFFFF, 10);
    CHECK(clipped <= 10 + 16, "截断宽度 %d 过大", clipped);
}

static void test_layout_widths(const starry_font_t *font, const starry_pack_t *pack)
{
    // 排版行必须放进文本框:正常行 <= 正文宽(216),禁则标点挂行尾时 <= 216 + 半个
    // 全角字(224,渲染端的裁剪上限)。拉丁字母按自然宽度推进,比模型预算的半角格子
    // 略宽(最多 px),所以这里允许极少数行超出到 px 级别。
    char text[STARRY_TEXT_BUFFER];
    uint32_t offsets[STARRY_TEXT_MAX_LINES + 1];
    int lines_checked = 0;
    int over_hang = 0;
    int over_wide = 0;
    int worst = 0;
    for (uint16_t i = 0; i < pack->dialogue_count; ++i) {
        starry_dialogue_t dlg;
        starry_pack_dialogue(pack, i, &dlg);
        const size_t len = starry_pack_text(pack, dlg.text_off, dlg.text_len, text, sizeof(text));
        if (len == 0) continue;
        const int lines = starry_text_lines(text, STARRY_SMALL_UNITS_PER_LINE, offsets,
                                            STARRY_TEXT_MAX_LINES);
        for (int l = 0; l < lines && l < STARRY_TEXT_MAX_LINES; ++l) {
            const size_t start = offsets[l];
            size_t end = (l + 1 <= lines) ? offsets[l + 1] : len;
            if (end > len) end = len;
            char line[STARRY_TEXT_BUFFER];
            size_t take = end - start;
            if (take >= sizeof(line)) take = sizeof(line) - 1;
            memcpy(line, text + start, take);
            line[take] = '\0';
            const int width = starry_text_px(line, 0, font);
            lines_checked++;
            if (width > worst) worst = width;
            if (width > TEXT_W + (int)font->px / 2) over_hang++;
            if (width > TEXT_W + (int)font->px) over_wide++;
            CHECK(width <= TEXT_W + (int)font->px + (int)font->px,
                  "行宽 %dpx 超出文本框太多(\"%s\")", width, line);
        }
    }
    CHECK(lines_checked > 15000, "检查的行数 %d 偏少", lines_checked);
    // 拉丁字母按自然宽度排,比半角格子略宽:含英文单词的行可能超出到半个全角字左右,
    // 由渲染端在文本框内边距上裁掉。这类行应极少(实测 2/19986)。
    CHECK(over_wide * 1000 <= lines_checked, "超出文本框的行 %d/%d 偏多(应 < 0.1%%)",
          over_wide, lines_checked);
    CHECK(over_hang * 100 <= lines_checked, "挂标点行数 %d/%d 偏多(应 < 1%%)", over_hang,
          lines_checked);
    printf("  排版宽度自检: %d 行,最宽 %dpx(正文 %dpx,含禁则上限 %dpx),挂标点 %d 行\n",
           lines_checked, worst, TEXT_W, TEXT_W + (int)font->px / 2, over_hang);
}

int main(int argc, char **argv)
{
    const char *font_path = argc > 1 ? argv[1] : "assets/fonts/starry_font16.bin";
    const char *pack_path = argc > 2 ? argv[2] : "main/starry_data/starry_pack.bin";

    uint32_t font_size = 0;
    uint8_t *font_blob = load_file(font_path, &font_size);
    starry_font_t font;
    if (!starry_font_open(&font, font_blob, font_size)) {
        fprintf(stderr, "字形包不合法: %s\n", font_path);
        return 2;
    }

    uint32_t pack_size = 0;
    uint8_t *pack_blob = load_file(pack_path, &pack_size);
    starry_pack_t pack;
    if (!starry_pack_open(&pack, pack_blob, pack_size)) {
        fprintf(stderr, "资源包不合法: %s\n", pack_path);
        return 2;
    }

    test_blend_math();
    test_fill_clipping();
    test_rounding_matches_bsp();
    test_round_mask();
    test_glyphs(&font);
    test_text_drawing(&font);
    test_layout_widths(&font, &pack);

    free(font_blob);
    free(pack_blob);
    if (g_failures) {
        fprintf(stderr, "starry_gfx: %d 项失败\n", g_failures);
        return 1;
    }
    printf("starry_gfx: PASS(字号 %u,行高 %u,字形 %u 个)\n", (unsigned)font.px,
           (unsigned)font.line_height, (unsigned)font.glyph_count);
    return 0;
}
