// main/starry_font.h —— 自研字形包(4bpp 位图)的只读解析层。
//
// 由 tools/starry_font.py 生成,和资源包一起放在 Flash 里直接读:没有解压、没有
// 运行时分配,每个字形按"行"取位图。本应用不用 LVGL 画字(见 starry_gfx.c 的
// 逐行合成器),所以格式是按逐行取图设计的:
//
//   header 40B : magic "SSRFONT1" | version | total | px | line_height | ascent
//                | glyph_count | bitmap_size | reserved
//   cmap       : glyph_count × { cp u32, index u16, pad u16 },按码位升序
//   glyph dsc  : glyph_count × { bitmap_off u32, w u8, h u8, ofs_x i8, ofs_y i8,
//                                advance u8, pad u16 } = 12 字节
//   bitmap     : 每字形 h 行 × ceil(w/2) 字节,行内 4bpp 高半字节在左
//
// 纯逻辑,不依赖 ESP-IDF,可在宿主机上直接测试(tests/test_starry_gfx.c 里用
// 真实字形包画字并校验像素)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STARRY_FONT_MAGIC "SSRFONT1"
#define STARRY_FONT_VERSION 1u

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    uint16_t px;            // 字号(单元格宽度 = 单位 × px/2)
    uint16_t line_height;   // 行高(px 加上行距)
    int16_t ascent;         // 基线位置(从行框顶边算起)
    uint32_t glyph_count;
    const uint8_t *cmap;    // 码位表
    const uint8_t *dsc;     // 字形描述表
    const uint8_t *bitmap;
    uint32_t bitmap_size;
} starry_font_t;

typedef struct {
    const uint8_t *rows;   // 4bpp,stride = (w+1)/2
    uint8_t w;
    uint8_t h;
    int8_t ofs_x;          // 相对单元格左上角
    int8_t ofs_y;          // 相对行框顶边
    uint8_t advance;       // 源字体的自然宽度(px),绘制按它推进
} starry_glyph_t;

// 校验魔数/版本/长度并建立各表视图。失败返回 false。
bool starry_font_open(starry_font_t *font, const uint8_t *data, uint32_t size);

// 取字形;没有该码位或没有墨迹时返回 false(调用方按空格处理)。
bool starry_font_glyph(const starry_font_t *font, uint32_t codepoint, starry_glyph_t *out_gl);

// 取字形位图第 row 行的高半字节起始地址(row < gl.h)。
static inline const uint8_t *starry_glyph_row(const starry_glyph_t *gl, int row)
{
    return gl->rows + (size_t)row * (((size_t)gl->w + 1u) / 2u);
}

static inline int starry_glyph_stride(const starry_glyph_t *gl)
{
    return (int)(((size_t)gl->w + 1u) / 2u);
}
