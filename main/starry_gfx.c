// main/starry_gfx.c —— 绘图原语实现。
#include "starry_gfx.h"

#include "starry_log.h"
#include "starry_model.h"   // starry_char_units / UTF-8 步进规则

#include <string.h>

static const char *TAG = "starry_gfx";

uint16_t starry_blend565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    const uint32_t inv = (uint32_t)(255u - alpha);
    const uint32_t dr = (dst >> 11) & 0x1Fu, dg = (dst >> 5) & 0x3Fu, db = dst & 0x1Fu;
    const uint32_t sr = (src >> 11) & 0x1Fu, sg = (src >> 5) & 0x3Fu, sb = src & 0x1Fu;
    // 各分量分别按 8 位插值再截回原宽度,避免两次量化叠加成明显色偏。
    const uint32_t r = (dr * inv + sr * alpha + 127u) / 255u;
    const uint32_t g = (dg * inv + sg * alpha + 127u) / 255u;
    const uint32_t b = (db * inv + sb * alpha + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void starry_surface_init(starry_surface_t *s, uint16_t *pixels, int stride, int width, int height,
                         int y0)
{
    if (!s) return;
    s->pixels = pixels;
    s->stride = stride;
    s->width = width;
    s->height = height;
    s->y0 = y0;
}

// 表面自检:条带缓冲是一行行写的,表面本身坏了才会把像素写到别的内存里。
// 注意不要拦"画到条带外":分块绘制本来就靠裁剪处理越界部分(比如一块 20 行的
// 电量胶囊分两次 16+4 行画,每次都会画到另一块的地盘上)。
static bool surface_ok(const starry_surface_t *s, int x, int y, int w, int h)
{
    if (!s || !s->pixels || s->width <= 0 || s->height <= 0 || s->stride < s->width) {
        STARRY_LOGW(TAG, "表面不可写: %p(宽 %d 高 %d stride %d)", (const void *)s,
                    s ? s->width : -1, s ? s->height : -1, s ? s->stride : -1);
        return false;
    }
    (void)x; (void)y; (void)w; (void)h;
    return true;
}

void starry_fill(starry_surface_t *s, int x, int y, int w, int h, uint16_t color)
{
    if (!surface_ok(s, x, y, w, h)) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w;
    if (x1 > s->width) x1 = s->width;
    if (x0 >= x1) return;
    int row0 = y - s->y0;
    int row1 = row0 + h;
    if (row0 < 0) row0 = 0;
    if (row1 > s->height) row1 = s->height;
    for (int row = row0; row < row1; ++row) {
        uint16_t *dst = s->pixels + (size_t)row * s->stride + x0;
        for (int i = x1 - x0; i > 0; --i) *dst++ = color;
    }
}

void starry_fill_rounded(starry_surface_t *s, int x, int y, int w, int h, int radius,
                         uint16_t color)
{
    if (!surface_ok(s, x, y, w, h)) return;
    int r = radius;
    const int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r <= 0) {
        starry_fill(s, x, y, w, h, color);
        return;
    }
    for (int row = 0; row < h; ++row) {
        int inset = 0;
        if (row < r) {
            const int dy = r - row;
            while ((inset + 1) * (inset + 1) + dy * dy <= r * r) inset++;
            inset = r - inset;
        } else if (row >= h - r) {
            const int dy = row - (h - 1 - r);
            while ((inset + 1) * (inset + 1) + dy * dy <= r * r) inset++;
            inset = r - inset;
        }
        if (inset < 0) inset = 0;
        starry_fill(s, x + inset, y + row, w - 2 * inset, 1, color);
    }
}

// 圆角半透明覆盖(底色用):逐行按圆角内缩量做混合,避免深色块的直角边露出"边框感"。
void starry_blend_rounded(starry_surface_t *s, int x, int y, int w, int h, int radius,
                          uint16_t color, uint8_t alpha)
{
    if (!surface_ok(s, x, y, w, h)) return;
    int r = radius;
    const int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r <= 0) {
        starry_blend(s, x, y, w, h, color, alpha);
        return;
    }
    for (int row = 0; row < h; ++row) {
        int inset = 0;
        if (row < r) {
            const int dy = r - row;
            while ((inset + 1) * (inset + 1) + dy * dy <= r * r) inset++;
            inset = r - inset;
        } else if (row >= h - r) {
            const int dy = row - (h - 1 - r);
            while ((inset + 1) * (inset + 1) + dy * dy <= r * r) inset++;
            inset = r - inset;
        }
        if (inset < 0) inset = 0;
        starry_blend(s, x + inset, y + row, w - 2 * inset, 1, color, alpha);
    }
}

void starry_blend(starry_surface_t *s, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    if (!surface_ok(s, x, y, w, h)) return;
    if (alpha == 0) return;
    if (alpha == 255) {
        starry_fill(s, x, y, w, h, color);
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w;
    if (x1 > s->width) x1 = s->width;
    if (x0 >= x1) return;
    int row0 = y - s->y0;
    int row1 = row0 + h;
    if (row0 < 0) row0 = 0;
    if (row1 > s->height) row1 = s->height;
    for (int row = row0; row < row1; ++row) {
        uint16_t *dst = s->pixels + (size_t)row * s->stride + x0;
        for (int i = x1 - x0; i > 0; --i, ++dst) *dst = starry_blend565(*dst, color, alpha);
    }
}

static int32_t clamp_radius(int32_t width, int32_t height, int32_t radius)
{
    if (radius <= 0 || width <= 0 || height <= 0) return 0;
    const int32_t max_radius = (width < height ? width : height) / 2;
    return radius > max_radius ? max_radius : radius;
}

bool starry_round_row_span(int y, int width, int height, int radius, int *x1, int *x2)
{
    // 与 components/bsp/src/bsp_display_rounding.c 的 bsp_display_rounded_row_span()
    // 保持逐行一致(参数校验、空区间返回、边界裁剪),tests/test_starry_gfx.c 会对照。
    if (!x1 || !x2 || width <= 0 || height <= 0 || y < 0 || y >= height) return false;
    const int32_t r = clamp_radius(width, height, radius);
    if (r <= 0 || (y >= r && y < height - r)) {
        *x1 = 0;
        *x2 = width - 1;
        return true;
    }
    const int32_t edge_y = y < r ? r - y : y - (height - 1 - r);
    int32_t inset = 0;
    while ((inset + 1) * (inset + 1) + edge_y * edge_y <= r * r) {
        inset++;
    }
    *x1 = (int)(r - inset);
    *x2 = (int)(width - r + inset - 1);
    if (*x1 < 0) *x1 = 0;
    if (*x2 >= width) *x2 = width - 1;
    return *x1 <= *x2;
}

void starry_round_mask(starry_surface_t *s, int screen_height, int radius, uint16_t black)
{
    if (!s || !s->pixels) return;
    for (int row = 0; row < s->height; ++row) {
        int x1 = 0;
        int x2 = s->width - 1;
        const int screen_y = s->y0 + row;
        if (!starry_round_row_span(screen_y, s->width, screen_height, radius, &x1, &x2)) {
            starry_fill(s, 0, screen_y, s->width, 1, black);
            continue;
        }
        if (x1 > 0) starry_fill(s, 0, screen_y, x1, 1, black);
        if (x2 < s->width - 1) starry_fill(s, x2 + 1, screen_y, s->width - x2 - 1, 1, black);
    }
}

// ---------------------------------------------------------------- 文本
static uint32_t utf8_decode_one(const char *utf8, size_t len, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)utf8;
    const size_t i = *pos;
    if (i >= len) return 0;
    const uint8_t lead = p[i];
    uint32_t cp = lead;
    size_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) {
        cp = lead & 0x1Fu;
        step = 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
        cp = lead & 0x0Fu;
        step = 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
        cp = lead & 0x07u;
        step = 4;
    } else if (lead >= 0x80u) {
        *pos = i + 1;
        return 0xFFFDu;
    }
    if (i + step > len) {
        *pos = len;
        return 0xFFFDu;
    }
    for (size_t k = 1; k < step; ++k) {
        if ((p[i + k] & 0xC0u) != 0x80u) {
            *pos = i + 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    *pos = i + step;
    return cp;
}

int starry_text_px(const char *utf8, size_t len, const starry_font_t *font)
{
    if (!utf8 || !font) return 0;
    if (len == 0) len = strlen(utf8);
    const int half = font->px / 2;
    int px = 0;
    size_t pos = 0;
    while (pos < len) {
        const uint32_t cp = utf8_decode_one(utf8, len, &pos);
        if (cp == 0 || cp == '\n') break;
        // 与 starry_text_line() 一致:有字形就用它的自然推进,缺字形退回单位宽度。
        starry_glyph_t gl;
        px += starry_font_glyph(font, cp, &gl) ? gl.advance : half * starry_char_units(cp);
    }
    return px;
}

// 把 4bpp 覆盖度混进目标像素。
static void blit_glyph(starry_surface_t *s, int x, int y, const starry_glyph_t *gl,
                       uint16_t color)
{
    for (int row = 0; row < gl->h; ++row) {
        const int screen_y = y + gl->ofs_y + row;
        const int buf_row = screen_y - s->y0;
        if (buf_row < 0 || buf_row >= s->height) continue;
        const uint8_t *src = starry_glyph_row(gl, row);
        uint16_t *dst = s->pixels + (size_t)buf_row * s->stride;
        for (int col = 0; col < gl->w; ++col) {
            const int px = x + gl->ofs_x + col;
            if (px < 0 || px >= s->width) continue;
            const uint8_t nib = (col & 1) ? (src[col / 2] & 0x0Fu) : (src[col / 2] >> 4);
            if (!nib) continue;
            const uint8_t alpha = (uint8_t)(nib * 17u);   // 0..15 -> 0..255
            dst[px] = starry_blend565(dst[px], color, alpha);
        }
    }
}

int starry_text_line(starry_surface_t *s, int x, int y, const char *utf8, size_t len,
                     const starry_font_t *font, uint16_t color, int max_px)
{
    if (!utf8 || !font) return 0;
    if (len == 0) len = strlen(utf8);
    const int half = font->px / 2;
    int pen = x;
    const int limit = (max_px > 0 && x + max_px < s->width) ? x + max_px : s->width;
    size_t pos = 0;
    while (pos < len) {
        const uint32_t cp = utf8_decode_one(utf8, len, &pos);
        if (cp == 0 || cp == '\n') break;
        // 推进用字形自带的自然宽度:缺字形时退回"半角单位 × px/2"。
        starry_glyph_t gl;
        const bool have = starry_font_glyph(font, cp, &gl);
        const int advance = have ? gl.advance : half * starry_char_units(cp);
        if (pen + advance > limit) break;
        if (have) blit_glyph(s, pen, y, &gl, color);
        pen += advance;
    }
    return pen - x;
}
