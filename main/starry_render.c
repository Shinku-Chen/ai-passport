// main/starry_render.c —— 逐条带合成实现。
#include "starry_render.h"

#include "starry_gfx.h"
#include "starry_model.h"

#include "starry_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "rom/tjpgd.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "starry_render";

// ---------------------------------------------------------------------------
// JPEG:直接用 C3 ROM 里的 TJpgDec(JD_FORMAT=0,输出 RGB888),逐块转 RGB565。
// 不引入 esp_jpeg 组件:ROM 里就有解码器,省掉的 Flash 正好给字形包。
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} jpeg_input_t;

typedef struct {
    uint16_t *pixels;        // 目标缓冲
    int stride;              // 每行像素数
    int width;               // 图像宽
    int height;              // 图像高
    int strip_rows;          // 每次交给 emit 的行数(= height 表示整图模式)
    int base_y;              // pixels 当前对应图像的第几行
    void (*emit)(void *ctx, int y0, int rows);
    void *ctx;
} jpeg_sink_t;

// TJpgDec 只接受【一个】device 指针,输入回调与输出回调都从 jd->device 取它,
// 所以两者必须挂在同一个结构上:否则输出回调会把输入状态当成 sink 读。
typedef struct {
    jpeg_sink_t sink;
    jpeg_input_t in;
} jpeg_ctx_t;

static UINT jpeg_in_cb(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    jpeg_input_t *in = ctx ? &ctx->in : NULL;
    if (!in || in->pos >= in->len) return 0;
    UINT n = nbyte;
    if ((uint32_t)n > in->len - in->pos) n = (UINT)(in->len - in->pos);
    if (buff) memcpy(buff, in->data + in->pos, n);
    in->pos += n;
    return n;
}

static UINT jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    jpeg_sink_t *sink = ctx ? &ctx->sink : NULL;
    if (!sink || !bitmap || !sink->pixels) return 0;
    const uint8_t *src = (const uint8_t *)bitmap;
    for (int y = (int)rect->top; y <= (int)rect->bottom; ++y) {
        while (y - sink->base_y >= sink->strip_rows) {
            if (sink->emit) sink->emit(sink->ctx, sink->base_y, sink->strip_rows);
            sink->base_y += sink->strip_rows;
        }
        if (y < sink->base_y) {
            src += ((int)rect->right - (int)rect->left + 1) * 3;
            continue;
        }
        uint16_t *dst = sink->pixels + (size_t)(y - sink->base_y) * sink->stride;
        const int x0 = (int)rect->left;
        for (int x = x0; x <= (int)rect->right; ++x) {
            const uint8_t r = src[(x - x0) * 3 + 0];
            const uint8_t g = src[(x - x0) * 3 + 1];
            const uint8_t b = src[(x - x0) * 3 + 2];
            if (x >= 0 && x < sink->width) dst[x] = (uint16_t)(((r & 0xF8u) << 8) |
                                                              ((g & 0xFCu) << 3) | (b >> 3));
        }
        src += ((int)rect->right - (int)rect->left + 1) * 3;
    }
    return 1;
}

static bool decode_jpeg(const uint8_t *data, uint32_t len, jpeg_sink_t *sink,
                        const starry_render_buffers_t *buf, uint32_t *elapsed_ms)
{
    jpeg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sink = *sink;
    ctx.in.data = data;
    ctx.in.len = len;
    ctx.in.pos = 0;
    JDEC jd;
    const int64_t start = esp_timer_get_time();
    JRESULT res = jd_prepare(&jd, jpeg_in_cb, buf->jpeg_work, buf->jpeg_work_size, &ctx);
    if (res != JDR_OK) {
        STARRY_LOGE(TAG, "JPEG 头解析失败: %d(工作区 %u 字节;JDR_MEM1=3 需要调大)",
                 (int)res, (unsigned)buf->jpeg_work_size);
        return false;
    }
    if (sink->width != (int)jd.width || sink->height != (int)jd.height) {
        STARRY_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %dx%d", (unsigned)jd.width, (unsigned)jd.height,
                 sink->width, sink->height);
        return false;
    }
    res = jd_decomp(&jd, jpeg_out_cb, 0);
    if (elapsed_ms) *elapsed_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (res != JDR_OK) {
        STARRY_LOGE(TAG, "JPEG 解码失败: %d", (int)res);
        return false;
    }
    // 回写更新过的 sink(逐条带状态在 ctx.sink 上推进),再做最后一段的收尾。
    *sink = ctx.sink;
    if (sink->emit && sink->base_y < sink->height) {
        sink->emit(sink->ctx, sink->base_y, sink->height - sink->base_y);
        sink->base_y = sink->height;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 推屏:LVGL 那条路径靠 esp_lvgl_port 交换字节,直推必须自己转大端 RGB565。
// ---------------------------------------------------------------------------
static void push_strip(starry_render_t *r, int y0, int rows)
{
    if (!r->panel || rows <= 0 || y0 >= STARRY_SCREEN_H) return;
    if (y0 < 0) {
        rows += y0;
        y0 = 0;
        if (rows <= 0) return;
    }
    if (y0 + rows > STARRY_SCREEN_H) rows = STARRY_SCREEN_H - y0;
    uint16_t *p = r->buf.strip;
    const size_t count = (size_t)rows * STARRY_SCREEN_W;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t v = p[i];
        p[i] = (uint16_t)((v >> 8) | (v << 8));
    }
    const esp_err_t err = esp_lcd_panel_draw_bitmap(r->panel, 0, y0, STARRY_SCREEN_W, y0 + rows, p);
    if (err != ESP_OK) {
        STARRY_LOGE(TAG, "推屏失败: %s", esp_err_to_name(err));
        return;
    }
    // 等这一笔 SPI 传完再推下一笔,行为与 esp_lvgl_port 的 flush 一致。
    if (r->trans_done) (void)xSemaphoreTake(r->trans_done, pdMS_TO_TICKS(200));
}

// 一块"待推送的区域":先从文本框底板拷底,画完再推。
typedef struct {
    starry_render_t *r;
    starry_surface_t surface;
    int y0;
    int rows;
} frame_t;

static void frame_begin_box(starry_render_t *r, frame_t *f, int y0, int rows)
{
    int row0 = y0 - STARRY_BOX_Y;           // 底板缓冲的第 0 行对应屏幕 214 行
    int first = 0;
    if (row0 < 0) {
        first = -row0;
        row0 = 0;
        y0 += first;
        rows -= first;
    }
    if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
    if (y0 + rows > STARRY_SCREEN_H) rows = STARRY_SCREEN_H - y0;
    if (rows <= 0) rows = 0;
    if (row0 + rows > STARRY_BOX_H) rows = STARRY_BOX_H - row0;
    f->r = r;
    f->y0 = y0;
    f->rows = rows;
    starry_surface_init(&f->surface, r->buf.strip, STARRY_SCREEN_W, STARRY_SCREEN_W, rows, y0);
    if (rows <= 0) return;
    if (!r->backdrops_valid) {
        // 还没解码过背景:文本框底图是全 0,直接拷出去就是一条黑带,用面板底色兜底。
        starry_fill(&f->surface, 0, y0, STARRY_SCREEN_W, rows, STARRY_COL_PANEL);
        return;
    }
    memcpy(r->buf.strip, r->buf.box + (size_t)row0 * STARRY_SCREEN_W,
           (size_t)rows * STARRY_SCREEN_W * sizeof(uint16_t));
}

static void frame_begin_solid(starry_render_t *r, frame_t *f, int y0, int rows, uint16_t color)
{
    if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
    if (y0 + rows > STARRY_SCREEN_H) rows = STARRY_SCREEN_H - y0;
    if (rows <= 0) rows = 0;
    f->r = r;
    f->y0 = y0;
    f->rows = rows;
    starry_surface_init(&f->surface, r->buf.strip, STARRY_SCREEN_W, STARRY_SCREEN_W, rows, y0);
    starry_fill(&f->surface, 0, y0, STARRY_SCREEN_W, rows, color);
}

static void frame_push(frame_t *f)
{
    if (f->rows <= 0) return;
    push_strip(f->r, f->y0, f->rows);
}

// ---------------------------------------------------------------------------
// 区域绘制:条带缓冲只有 16 行高,任何比它高的区域都必须分块,否则会写穿缓冲。
// 回调拿到的是"已经铺好底"的 frame(use_box = 文本框底,否则纯色底)。
// ---------------------------------------------------------------------------
typedef void (*strip_draw_fn)(starry_render_t *r, frame_t *f, int y, int rows, void *ctx);

static void draw_region(starry_render_t *r, int y0, int rows, uint16_t solid, bool use_box,
                        strip_draw_fn fn, void *ctx)
{
    for (int y = y0; y < y0 + rows; y += STARRY_STRIP_ROWS) {
        int chunk = y0 + rows - y;
        if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
        frame_t f;
        if (use_box) frame_begin_box(r, &f, y, chunk);
        else frame_begin_solid(r, &f, y, chunk, solid);
        if (f.rows <= 0) break;
        fn(r, &f, y, chunk, ctx);
        frame_push(&f);
    }
}

// ---------------------------------------------------------------------------
// 布局辅助
// ---------------------------------------------------------------------------
static int units_per_line_of(const starry_font_t *font)
{
    if (font && font->px >= 20) return STARRY_LARGE_UNITS_PER_LINE;
    return STARRY_SMALL_UNITS_PER_LINE;
}

static uint16_t battery_color(int percent)
{
    if (percent < 0) return STARRY_COL_DIM;
    if (percent <= 20) return starry_rgb565(0xE0, 0x5A, 0x5A);
    return STARRY_COL_ACCENT;
}

// 电量胶囊:底图缓冲 + 边框 + 百分比文字。percent < 0 时只画 "--"。
static void draw_battery(starry_render_t *r, starry_surface_t *surface, int percent)
{
    const int bx = STARRY_BATTERY_X;
    const int by = STARRY_BATTERY_Y;
    // 只要文字:不要胶囊底、不要余量条。电量高低靠文字颜色区分(正常强调蓝、<=20% 红、
    // 未知时次要色),所以即使压在亮背景上也只剩一行字。
    char text[8];
    if (percent < 0) snprintf(text, sizeof(text), "--");
    else snprintf(text, sizeof(text), "%d%%", percent > 100 ? 100 : percent);
    const int width = starry_text_px(text, 0, r->font_small);
    starry_text_line(surface, bx + (STARRY_BATTERY_W - width) / 2, by, text, 0, r->font_small,
                     battery_color(percent), 0);
}

// ---------------------------------------------------------------------------
// 画面 + 文本框底板
// ---------------------------------------------------------------------------
void starry_render_plan(starry_render_plan_t *plan)
{
    if (!plan) return;
    plan->pixels = (uint32_t)STARRY_SPRITE_MAX_W * STARRY_SPRITE_H +
                   (uint32_t)STARRY_SCREEN_W * STARRY_BOX_H +
                   (uint32_t)STARRY_SCREEN_W * STARRY_STRIP_ROWS * 2 +   // 条带 + 扣住的首条带
                   (uint32_t)STARRY_SCREEN_W * STARRY_CHAPTER_H;
    plan->bytes = plan->pixels * 2u;
}

// SPI 颜色传输完成回调(跑在中断上下文,只给信号量/唤醒任务)。
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)io;
    (void)edata;
    starry_render_t *r = (starry_render_t *)user_ctx;
    BaseType_t higher_awoken = pdFALSE;
    if (r && r->trans_done) xSemaphoreGiveFromISR(r->trans_done, &higher_awoken);
    return higher_awoken == pdTRUE;
}

bool starry_render_init(starry_render_t *r, const starry_pack_t *pack,
                        esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                        const starry_font_t *font_small, const starry_font_t *font_large,
                        const starry_render_buffers_t *buffers)
{
    if (!r || !pack || !panel || !font_small || !font_large || !buffers) return false;
    if (!buffers->sprite || !buffers->box || !buffers->strip || !buffers->hold ||
        !buffers->jpeg_work) {
        STARRY_LOGE(TAG, "渲染缓冲未提供");
        return false;
    }
    memset(r, 0, sizeof(*r));
    r->pack = pack;
    r->panel = panel;
    r->panel_io = panel_io;
    r->font_small = font_small;
    r->font_large = font_large;
    r->buf = *buffers;
    r->shown_bg = STARRY_NONE;
    r->shown_sprite = STARRY_NONE;
    r->backdrops_valid = false;
    r->overlay_battery = STARRY_BATTERY_NONE;
    r->decoded_sprite = STARRY_NONE;

    // 注册"颜色传输完成"回调并等第一笔,之后 push_strip() 每笔都会等完成。
    r->trans_done = xSemaphoreCreateBinary();
    if (r->trans_done && panel_io) {
        const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
        const esp_err_t e = esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, r);
        if (e != ESP_OK) {
            STARRY_LOGW(TAG, "注册传输完成回调失败(%s),不做逐笔等待", esp_err_to_name(e));
            vSemaphoreDelete(r->trans_done);
            r->trans_done = NULL;
        }
    }
    return true;
}

// 文本框底色的逐行强度:从画面区往下渐入,到 STARRY_BOX_Y 达到全强度并保持。
// 这样文本框上沿不再出现一条横贯全屏的硬边(那看着就像给文字框描了边框)。
// 正文带的蓝(源工程 text_bg 采样)与文字区暗帘色(都是 RGB888 常量 -> RGB565)。
static uint16_t band_color(void)
{
    return starry_rgb565(STARRY_BAND_R, STARRY_BAND_G, STARRY_BAND_B);
}

static uint16_t scrim_color(void)
{
    return starry_rgb565(STARRY_SCRIM_R, STARRY_SCRIM_G, STARRY_SCRIM_B);
}

static uint8_t fade_weight_at(int y)
{
    const int start = STARRY_BOX_Y - STARRY_BOX_LEAD_IN;
    if (y <= start) return 0;
    if (y >= STARRY_BOX_Y) return 255;
    const int t = ((y - start) * 255) / STARRY_BOX_LEAD_IN;            // 0..255
    return (uint8_t)((t * t * (3 * 255 - 2 * t)) / (255 * 255));       // smoothstep
}

// 正文带:带顶 STARRY_BAND_ALPHA_TOP -> 带底 STARRY_BAND_ALPHA_BOTTOM,前面有一段渐入。
uint8_t starry_band_alpha_at(int y)
{
    const unsigned lead = fade_weight_at(y);
    if (y < STARRY_BOX_Y) {
        return (uint8_t)((STARRY_BAND_ALPHA_TOP * lead) / 255u);
    }
    const int span = STARRY_SCREEN_H - 1 - STARRY_BOX_Y;
    const unsigned t = span > 0 ? (unsigned)(y - STARRY_BOX_Y) * 255u / (unsigned)span : 255u;
    return (uint8_t)(STARRY_BAND_ALPHA_TOP +
                     (STARRY_BAND_ALPHA_BOTTOM - STARRY_BAND_ALPHA_TOP) * t / 255u);
}

// 文字区浅暗帘:盖在立绘之上、文字之下,只为了让白字在任何立绘上都读得清。
uint8_t starry_scrim_alpha_at(int y)
{
    const unsigned lead = fade_weight_at(y);
    if (y < STARRY_BOX_Y) return (uint8_t)((STARRY_SCRIM_ALPHA_TOP * lead) / 255u);
    const int span = STARRY_SCREEN_H - 1 - STARRY_BOX_Y;
    const unsigned t = span > 0 ? (unsigned)(y - STARRY_BOX_Y) * 255u / (unsigned)span : 255u;
    return (uint8_t)(STARRY_SCRIM_ALPHA_TOP +
                     (STARRY_SCRIM_ALPHA_BOTTOM - STARRY_SCRIM_ALPHA_TOP) * t / 255u);
}

// 菜单/选项那层压暗也要跟着同一条曲线渐入,否则 214 行又会冒出一条硬边。
static void blend_box_dim(starry_surface_t *s, int y0, int rows, uint8_t alpha)
{
    for (int row = 0; row < rows; ++row) {
        const uint8_t a = (uint8_t)(((unsigned)alpha * fade_weight_at(y0 + row)) / 255u);
        if (a) starry_blend(s, 0, y0 + row, STARRY_SCREEN_W, 1, STARRY_COL_DARK, a);
    }
}

// 背景解码的逐条带回调:立绘合成 -> 渐入底色 -> 底板拷贝 -> 圆角遮罩 -> 推屏。
static void scene_emit(void *ctx, int y0, int rows)
{
    starry_render_t *r = (starry_render_t *)ctx;
    if (!r || !r->buf.strip || rows <= 0 || rows > STARRY_STRIP_ROWS) {
        STARRY_LOGE(TAG, "条带回调参数异常: rows=%d(上限 %d)", rows, STARRY_STRIP_ROWS);
        return;
    }
    // 首条带扣到 hold 里:章节/电量都在这一段,先推会和最后一段的正文差 120ms。
    const bool held = (y0 == 0 && r->buf.hold != NULL);
    uint16_t *dst_buf = held ? r->buf.hold : r->buf.strip;
    starry_surface_t s;
    starry_surface_init(&s, dst_buf, STARRY_SCREEN_W, STARRY_SCREEN_W, rows, y0);

    // 正文带(ATRI 版做法):先铺一层蓝色渐变,立绘压在带子**上面**,最后再盖淡暗帘。
    // 这样人物不会被文本框洗掉,只有文字那几行被压暗。
    for (int row = 0; row < rows; ++row) {
        const uint8_t a = starry_band_alpha_at(y0 + row);
        if (a) starry_blend(&s, 0, y0 + row, STARRY_SCREEN_W, 1, band_color(), a);
    }

    // 立绘:1bpp 遮罩命中处直接覆盖(源素材 alpha 基本是二值的)。
    if (r->decoded_sprite != STARRY_NONE) {
        starry_fg_t fg;
        if (starry_pack_fg(r->pack, r->decoded_sprite, &fg)) {
            const int row_bytes = (fg.w + 7) / 8;
            for (int row = 0; row < rows; ++row) {
                const int y = y0 + row;
                if (y < fg.y || y >= fg.y + fg.h) continue;
                const int sy = y - fg.y;
                const uint16_t *src = r->buf.sprite + (size_t)sy * STARRY_SPRITE_MAX_W;
                const uint8_t *mask = fg.mask + (size_t)sy * row_bytes;
                uint16_t *dst = dst_buf + (size_t)row * STARRY_SCREEN_W;
                for (int col = 0; col < fg.w; ++col) {
                    if (!(mask[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) continue;
                    const int x = fg.x + col;
                    if (x >= 0 && x < STARRY_SCREEN_W) dst[x] = src[col];
                }
            }
        }
    }

    // 淡暗帘:盖在立绘之上、文字之下。
    for (int row = 0; row < rows; ++row) {
        const uint8_t a = starry_scrim_alpha_at(y0 + row);
        if (a) starry_blend(&s, 0, y0 + row, STARRY_SCREEN_W, 1, scrim_color(), a);
    }

    // 文本框区域:把"屏幕上真实显示的样子"存进底板缓冲,重绘文字时才能一像素不差地恢复。
    for (int row = 0; row < rows; ++row) {
        const int y = y0 + row;
        uint16_t *strip_row = dst_buf + (size_t)row * STARRY_SCREEN_W;
        if (r->buf.name_bg && y >= STARRY_NAME_Y && y < STARRY_NAME_Y + STARRY_NAME_AREA_H) {
            memcpy(r->buf.name_bg + (size_t)(y - STARRY_NAME_Y) * STARRY_SCREEN_W, strip_row,
                   STARRY_SCREEN_W * sizeof(uint16_t));
        }
        if (r->buf.chapter_bg && y >= STARRY_CHAPTER_Y && y < STARRY_CHAPTER_Y + STARRY_CHAPTER_H) {
            memcpy(r->buf.chapter_bg + (size_t)(y - STARRY_CHAPTER_Y) * STARRY_SCREEN_W, strip_row,
                   STARRY_SCREEN_W * sizeof(uint16_t));
        }
        if (y < STARRY_BOX_Y) continue;
        const int box_row = y - STARRY_BOX_Y;
        if (box_row < STARRY_BOX_H) {
            memcpy(r->buf.box + (size_t)box_row * STARRY_SCREEN_W, strip_row,
                   STARRY_SCREEN_W * sizeof(uint16_t));
        }
    }
    // 浮层(章节/名字/正文/电量)直接画进这一条带 —— 整屏重画时文字不会"先消失再出现"。
    starry_render_overlays(r, &s);
    starry_round_mask(&s, STARRY_SCREEN_H, STARRY_RADIUS, STARRY_COL_BLACK);
    if (held) {
        r->hold_rows = rows;      // 等整帧解码完再推,保证文字一起上屏
    } else {
        push_strip(r, y0, rows);
    }
}

bool starry_render_scene(starry_render_t *r, uint16_t bg, uint16_t sprite)
{
    if (!r) return false;
    const uint16_t want_bg = (bg == STARRY_BG_KEEP) ? r->shown_bg : bg;
    uint16_t want_sprite = (sprite == STARRY_FG_KEEP) ? r->shown_sprite : sprite;

    starry_bg_t info;
    if (!starry_pack_bg(r->pack, want_bg, &info)) {
        STARRY_LOGW(TAG, "背景下标无效: %u", (unsigned)want_bg);
        return false;
    }

    const bool sprite_changed = (want_sprite != r->shown_sprite);
    if (sprite_changed) {
        r->decoded_sprite = STARRY_NONE;
        if (want_sprite != STARRY_NONE) {
            starry_fg_t fg;
            if (starry_pack_fg(r->pack, want_sprite, &fg)) {
                if (fg.w > STARRY_SPRITE_MAX_W || fg.h > STARRY_SPRITE_H) {
                    STARRY_LOGW(TAG, "立绘尺寸超缓冲: %ux%u", (unsigned)fg.w, (unsigned)fg.h);
                } else {
                    jpeg_sink_t sink = {
                        .pixels = r->buf.sprite,
                        .stride = STARRY_SPRITE_MAX_W,
                        .width = fg.w,
                        .height = fg.h,
                        .strip_rows = fg.h,
                        .base_y = 0,
                        .emit = NULL,
                        .ctx = NULL,
                    };
                    uint32_t ms = 0;
                    if (!decode_jpeg(fg.jpeg, fg.jpeg_len, &sink, &r->buf, &ms)) {
                        STARRY_LOGW(TAG, "立绘解码失败: %u", (unsigned)want_sprite);
                    } else {
                        r->decoded_sprite = want_sprite;
                        r->last_decode_ms = ms;
                    }
                }
            } else {
                STARRY_LOGW(TAG, "立绘下标无效: %u", (unsigned)want_sprite);
            }
        }
        r->shown_sprite = want_sprite;
    } else {
        r->decoded_sprite = want_sprite;
    }

    if (want_bg == r->shown_bg && !sprite_changed) return false;

    jpeg_sink_t sink = {
        .pixels = r->buf.strip,
        .stride = STARRY_SCREEN_W,
        .width = STARRY_SCREEN_W,
        .height = STARRY_SCREEN_H,
        .strip_rows = STARRY_STRIP_ROWS,
        .base_y = 0,
        .emit = scene_emit,
        .ctx = r,
    };
    uint32_t ms = 0;
    if (!decode_jpeg(info.jpeg, info.jpeg_len, &sink, &r->buf, &ms)) {
        STARRY_LOGE(TAG, "背景解码失败: %u", (unsigned)want_bg);
        return false;
    }
    if (r->hold_rows > 0) {
        // 首条带等到这里才推:章节/电量与正文同时上屏,不会出现"两个角先闪一下"。
        uint16_t *saved = r->buf.strip;
        r->buf.strip = r->buf.hold;
        push_strip(r, 0, r->hold_rows);
        r->buf.strip = saved;
        r->hold_rows = 0;
    }
    r->shown_bg = want_bg;
    r->backdrops_valid = true;   // 名字/章节/文本框底图这一刻才有内容
    r->last_decode_ms = ms;
    STARRY_LOGI(TAG, "画面 背景%u/立绘%u 解码+合成 %u ms", (unsigned)want_bg, (unsigned)want_sprite,
             (unsigned)ms);

    return true;
}

// ---------------------------------------------------------------------------
// 文字层
// ---------------------------------------------------------------------------
// 画面区左下角的说话人名字:底图来自场景合成时存下的 name_bg,重绘不用重新解码背景。
// 在"名字区"(画面区底部、文本框上方那条)画一行带底板的白话文字。
// 名字、选项提问都走这里 —— 千万别用 frame_begin_box,那个会按文本框范围把这一带裁掉。
void starry_render_plate(starry_render_t *r, const char *text, uint16_t color,
                         const starry_font_t *font)
{
    if (!r || !font) return;
    // 还没有解码过背景:底图缓冲是全 0,恢复它会推出一条黑带。此时也本来就没画过浮层。
    if (!r->backdrops_valid) return;
    const bool has_text = text && text[0];
    snprintf(r->overlay_name, sizeof(r->overlay_name), "%s", has_text ? text : "");
    r->overlay_name_color = color;
    int plate_w = has_text ? starry_text_px(text, 0, font) : STARRY_SCREEN_W;
    if (plate_w > STARRY_SCREEN_W - STARRY_NAME_X) plate_w = STARRY_SCREEN_W - STARRY_NAME_X;
    for (int row = 0; row < STARRY_NAME_AREA_H; row += STARRY_STRIP_ROWS) {
        int chunk = STARRY_NAME_AREA_H - row;
        if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
        frame_t f;
        if (r->buf.name_bg) {
            f.r = r;
            f.y0 = STARRY_NAME_Y + row;
            f.rows = chunk;
            starry_surface_init(&f.surface, r->buf.strip, STARRY_SCREEN_W, STARRY_SCREEN_W, chunk,
                                f.y0);
            memcpy(r->buf.strip, r->buf.name_bg + (size_t)row * STARRY_SCREEN_W,
                   (size_t)chunk * STARRY_SCREEN_W * sizeof(uint16_t));
        } else {
            frame_begin_solid(r, &f, STARRY_NAME_Y + row, chunk, STARRY_COL_BLACK);
        }
        if (f.rows <= 0) break;
        // 恢复的是"不含文字"的干净底:同一行上还可能有别的浮层(电量、章节),统一重画一遍。
        starry_render_overlays(r, &f.surface);
        frame_push(&f);
    }
}

void starry_render_name(starry_render_t *r, const char *name, const char *const previous,
                        const starry_font_t *font)
{
    if (!r || !font) return;
    (void)previous;
    starry_render_plate(r, name, STARRY_COL_NAME, font);
}

// 画面区左上角的章节标签(换章时重绘一次)。
void starry_render_chapter_set(starry_render_t *r, const char *text)
{
    if (!r) return;
    snprintf(r->overlay_chapter, sizeof(r->overlay_chapter), "%s", text ? text : "");
}

void starry_render_chapter(starry_render_t *r, const char *text, const starry_font_t *font)
{
    if (!r || !font) return;
    (void)font;   // 章节标签固定用小字号(浮层重画时也用小字号,保持两处一致)
    starry_render_chapter_set(r, text);
    if (!r->backdrops_valid) return;   // 底图没有内容时不能恢复
    for (int row = 0; row < STARRY_CHAPTER_H; row += STARRY_STRIP_ROWS) {
        int chunk = STARRY_CHAPTER_H - row;
        if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
        frame_t f;
        if (r->buf.chapter_bg) {
            f.r = r;
            f.y0 = STARRY_CHAPTER_Y + row;
            f.rows = chunk;
            starry_surface_init(&f.surface, r->buf.strip, STARRY_SCREEN_W, STARRY_SCREEN_W, chunk,
                                f.y0);
            // 整行恢复:标签只占左边一段,但整行都要干净
            memcpy(r->buf.strip, r->buf.chapter_bg + (size_t)row * STARRY_SCREEN_W,
                   (size_t)chunk * STARRY_SCREEN_W * sizeof(uint16_t));
        } else {
            frame_begin_solid(r, &f, STARRY_CHAPTER_Y + row, chunk, STARRY_COL_PANEL);
        }
        if (f.rows <= 0) break;
        // 章节与电量共用 6..25 行:这里必须把电量等其它浮层一起补回来,
        // 否则后画的那个会把先画的擦掉(真机上表现为"章节显示完就消失")。
        starry_render_overlays(r, &f.surface);
        frame_push(&f);
    }
}

// 正文逐行画到给定表面上(调用方保证底板干净)。行拆分与分页规则就这一处。
static void draw_body_lines(starry_surface_t *s, const char *text, size_t visible_bytes,
                            const starry_font_t *font, int units_per_line)
{
    if (!s || !font || !text) return;
    const bool large = font->px >= 20;
    const int pitch = large ? STARRY_LARGE_LINE_PITCH : STARRY_SMALL_LINE_PITCH;
    const int max_lines = large ? STARRY_LARGE_LINES_PER_PAGE : STARRY_SMALL_LINES_PER_PAGE;
    // 逐行结果一次拿全:只画前 max_lines 行,但最后一行的结束偏移要靠下一行才准。
    uint32_t lines[STARRY_TEXT_MAX_LINES + 1];
    int count = starry_text_lines(text, units_per_line, lines, STARRY_TEXT_MAX_LINES);
    const size_t total = strlen(text);
    if (count > STARRY_TEXT_MAX_LINES) count = STARRY_TEXT_MAX_LINES;

    for (int i = 0; i < max_lines; ++i) {
        const int y = STARRY_TEXT_Y + i * pitch;
        if (y + pitch > STARRY_SCREEN_H) break;
        if (i >= count) continue;
        const size_t start_off = lines[i];
        size_t end = (i + 1 <= count) ? lines[i + 1] : total;
        if (end > total) end = total;
        const size_t clip = visible_bytes < end ? visible_bytes : end;
        if (clip <= start_off) continue;
        char buf[STARRY_TEXT_BUFFER];
        size_t len = clip - start_off;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, text + start_off, len);
        buf[len] = '\0';
        starry_text_line(s, STARRY_TEXT_X, y, buf, 0, font, STARRY_COL_TEXT,
                         STARRY_TEXT_W + font->px / 2);
    }
}

// 把当前该显示的文字浮层画到给定表面上(表面自带的 y0/height 会裁掉不在本段的行)。
void starry_render_overlays(starry_render_t *r, starry_surface_t *surface)
{
    if (!r || !surface) return;
    const starry_font_t *name_font = r->overlay_body_font ? r->overlay_body_font : r->font_small;
    if (r->overlay_chapter[0]) {
        starry_text_line(surface, STARRY_CHAPTER_X, STARRY_CHAPTER_Y, r->overlay_chapter, 0,
                         r->font_small, STARRY_COL_TEXT, STARRY_CHAPTER_TEXT_W);
    }
    if (r->overlay_name[0]) {
        // 角色名带半透明底板(压在画面上,不是实心块):文字仍然和正文左边距对齐,
        // 底板往左多出 STARRY_PLATE_PAD,右侧也留同样边距。
        const int plate_x = STARRY_NAME_X - STARRY_PLATE_PAD;
        int plate_w = starry_text_px(r->overlay_name, 0, name_font) + STARRY_PLATE_PAD * 2;
        if (plate_w > STARRY_SCREEN_W - plate_x) plate_w = STARRY_SCREEN_W - plate_x;
        starry_blend_rounded(surface, plate_x, STARRY_NAME_Y, plate_w, STARRY_NAME_AREA_H, 6,
                             STARRY_COL_PLATE, STARRY_PLATE_ALPHA);
        starry_text_line(surface, STARRY_NAME_X, STARRY_NAME_Y, r->overlay_name, 0, name_font,
                         r->overlay_name_color, STARRY_SCREEN_W - STARRY_NAME_X);
    }
    if (r->overlay_battery != STARRY_BATTERY_NONE) {
        draw_battery(r, surface, r->overlay_battery);
    }
    if (r->overlay_auto) {
        // 画面区右下角常驻"自动":和名字同一行,但不抢左边距。
        const char *label = "自动";
        const int w = starry_text_px(label, 0, r->font_small) + STARRY_PLATE_PAD * 2;
        const int x = STARRY_SCREEN_W - w - STARRY_PLATE_PAD;
        starry_blend_rounded(surface, x, STARRY_NAME_Y, w, STARRY_NAME_AREA_H, 6,
                             STARRY_COL_PLATE, STARRY_PLATE_ALPHA);
        starry_text_line(surface, x + STARRY_PLATE_PAD, STARRY_NAME_Y, label, 0, r->font_small,
                         STARRY_COL_ACCENT, w - STARRY_PLATE_PAD);
    }
    if (r->overlay_body[0] && r->overlay_body_font) {
        draw_body_lines(surface, r->overlay_body, r->overlay_body_visible, r->overlay_body_font,
                        r->overlay_body_units);
    }
}

void starry_render_body(starry_render_t *r, const char *text, size_t visible_bytes,
                        const starry_font_t *font, int units_per_line)
{
    if (!r || !font || !text) return;
    // 记下当前浮层:整屏重画时由 starry_render_overlays() 一并画进条带,避免文字闪烁。
    snprintf(r->overlay_body, sizeof(r->overlay_body), "%s", text);
    r->overlay_body_visible = visible_bytes;
    r->overlay_body_font = font;
    r->overlay_body_units = units_per_line;
    // 文本框按条带整块重刷:每屏最多两笔,且上一句的残留会被底图盖掉。
    for (int y = STARRY_BOX_Y; y < STARRY_SCREEN_H; y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_box(r, &f, y, rows);
        if (f.rows <= 0) break;
        // 走统一的浮层重画:万一这一带还压着别的浮层也不会被擦掉。
        starry_render_overlays(r, &f.surface);
        frame_push(&f);
    }
}

void starry_render_choices(starry_render_t *r, const char *prompt, const char *const *choices,
                           int count, int selected, const starry_font_t *font)
{
    if (!r || !font) return;
    const int row_height = 28;
    const int gap = 6;

    // 文本框整块压暗(同样渐入),选项条压在暗底上,和游戏内菜单同一套观感。
    for (int fill_y = STARRY_BOX_Y; fill_y < STARRY_SCREEN_H; fill_y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - fill_y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_box(r, &f, fill_y, rows);
        if (f.rows <= 0) break;
        blend_box_dim(&f.surface, fill_y, f.rows, STARRY_MENU_DIM_ALPHA);
        frame_push(&f);
    }

    // 提示文本放在说话人那一行,选项条占满剩余空间。
    if (prompt && prompt[0]) {
        uint32_t lines[2] = { 0, 0 };
        const int count_lines = starry_text_lines(prompt, units_per_line_of(font), lines, 2);
        const size_t total = strlen(prompt);
        if (count_lines >= 1) {
            size_t end = count_lines > 1 ? lines[1] : total;
            if (end > total) end = total;
            char buf[STARRY_TEXT_BUFFER];
            size_t len = end;
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, prompt, len);
            buf[len] = '\0';
            starry_render_plate(r, buf, STARRY_COL_DIM, font);
        }
    }

    const int y0 = STARRY_BOX_LIST_TOP;
    for (int i = 0; i < count && i < 2; ++i) {
        const int y = y0 + i * (row_height + gap);
        if (y + row_height > STARRY_SCREEN_H) break;
        const char *label = (choices && choices[i]) ? choices[i] : "";
        for (int row = 0; row < row_height; row += STARRY_STRIP_ROWS) {
            int chunk = row_height - row;
            if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
            frame_t f;
            frame_begin_box(r, &f, y + row, chunk);
            if (f.rows <= 0) break;
            // ATRI 风格:未选中 = 深蓝底 + 天蓝描边;选中 = 天蓝实底 + 深色字。
            const bool on = i == selected;
            starry_blend_rounded(&f.surface, STARRY_TEXT_X, y, STARRY_TEXT_W, row_height, 8,
                                 STARRY_COL_ACCENT, STARRY_ROW_SEL_ALPHA);
            starry_blend_rounded(&f.surface, STARRY_TEXT_X + 1, y + 1, STARRY_TEXT_W - 2,
                                 row_height - 2, 7, STARRY_COL_DARK,
                                 on ? 0 : (uint8_t)STARRY_ROW_ALPHA);
            const int width = starry_text_px(label, 0, font);
            starry_text_line(&f.surface, STARRY_TEXT_X + (STARRY_TEXT_W - width) / 2, y + 4, label,
                             0, font, on ? STARRY_COL_DARK : STARRY_COL_TEXT, STARRY_TEXT_W);
            frame_push(&f);
        }
    }
}

typedef struct {
    int percent;
    bool backdrop;   // true = 底来自画面(阅读页),false = 纯色底(整页列表)
} battery_ctx_t;

static void battery_strip(starry_render_t *r, frame_t *f, int y, int rows, void *ctx)
{
    const battery_ctx_t *c = (const battery_ctx_t *)ctx;
    if (c->backdrop && !r->backdrops_valid) return;   // 电量胶囊的底图也来自场景
    if (c->backdrop && r->buf.chapter_bg) {
        // 电量那几行(6..25)完全落在章节底图(6..28)里,直接复用它,省一块 9.6KB 底图。
        for (int i = 0; i < rows; ++i) {
            const int by = y + i - STARRY_CHAPTER_Y;
            if (by < 0 || by >= STARRY_CHAPTER_H) continue;
            memcpy(r->buf.strip + (size_t)i * STARRY_SCREEN_W,
                   r->buf.chapter_bg + (size_t)by * STARRY_SCREEN_W,
                   STARRY_SCREEN_W * sizeof(uint16_t));
        }
    }
    if (c->backdrop) {
        // 这一条带恢复了整行干净底图:把章节等浮层补回来,否则电量一画章节就没了。
        starry_render_overlays(r, &f->surface);
    } else {
        draw_battery(r, &f->surface, c->percent);   // 纯色底的整页列表:只画电量
    }
    starry_round_mask(&f->surface, STARRY_SCREEN_H, STARRY_RADIUS, STARRY_COL_BLACK);
}

void starry_render_auto(starry_render_t *r, bool on)
{
    if (!r) return;
    r->overlay_auto = on;
}

void starry_render_clear_overlays(starry_render_t *r)
{
    if (!r) return;
    // 先把"该显示什么"清干净,再重画那两条带 —— 否则重画时会顺手把"自动"又画回去。
    r->overlay_chapter[0] = '\0';
    r->overlay_name[0] = '\0';
    r->overlay_body[0] = '\0';
    r->overlay_body_font = NULL;
    r->overlay_auto = false;
    starry_render_name(r, "", NULL, r->font_small);
    starry_render_chapter(r, "", r->font_small);
}


void starry_render_box_clear(starry_render_t *r)
{
    if (!r || !r->backdrops_valid) return;   // 底图未就绪时重刷同样会推出黑块
    for (int y = STARRY_BOX_Y; y < STARRY_SCREEN_H; y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_box(r, &f, y, rows);
        if (f.rows <= 0) break;
        starry_render_overlays(r, &f.surface);
        frame_push(&f);
    }
}

void starry_render_battery(starry_render_t *r, int percent)
{
    if (!r) return;
    // 登记当前电量:解码背景时由 starry_render_overlays() 一并画进条带,
    // 否则每次换画面它都会先被盖掉(右上角闪一下)。
    r->overlay_battery = percent;
    battery_ctx_t ctx = { .percent = percent, .backdrop = true };
    draw_region(r, STARRY_BATTERY_Y, STARRY_BATTERY_H, STARRY_COL_BLACK, false, battery_strip,
                &ctx);
}

// ---------------------------------------------------------------------------
// 整页界面
// ---------------------------------------------------------------------------
void starry_render_fill(starry_render_t *r, uint16_t color)
{
    if (!r) return;
    for (int y = 0; y < STARRY_SCREEN_H; y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_solid(r, &f, y, rows, color);
        starry_round_mask(&f.surface, STARRY_SCREEN_H, STARRY_RADIUS, STARRY_COL_BLACK);
        frame_push(&f);
    }
}

static void draw_row(starry_render_t *r, const starry_row_t *row, int y, int row_height,
                     int selected, const starry_font_t *font)
{
    for (int offset = 0; offset < row_height; offset += STARRY_STRIP_ROWS) {
        int chunk = row_height - offset;
        if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_box(r, &f, y + offset, chunk);
        if (f.rows <= 0) break;
        // 行不再画方框:只有选中行有一条圆角高亮条 + 左侧强调线,其余是纯文字。
        if (selected) {
            starry_blend_rounded(&f.surface, STARRY_TEXT_X, y + 2, STARRY_TEXT_W, row_height - 4,
                                 5, STARRY_COL_ACCENT, STARRY_ROW_SEL_ALPHA);
        } else {
            starry_blend_rounded(&f.surface, STARRY_TEXT_X, y + 2, STARRY_TEXT_W, row_height - 4,
                                 5, STARRY_COL_ROW, STARRY_ROW_ALPHA);
        }
        const uint16_t label_color = selected     ? STARRY_COL_DARK
                                     : (row && row->dim) ? STARRY_COL_DIM
                                                         : STARRY_COL_TEXT;
        if (row && row->label) {
            // 有右侧取值时给取值留位置;没有取值就让标签用满宽度,否则长文案会被截断。
            const int label_px = (row->value && row->value[0]) ? STARRY_TEXT_W - 110
                                                                : STARRY_TEXT_W - 20;
            starry_text_line(&f.surface, STARRY_TEXT_X + 10, y, row->label, 0, font, label_color,
                             label_px);
        }
        if (row && row->value) {
            const int width = starry_text_px(row->value, 0, font);
            starry_text_line(&f.surface, STARRY_TEXT_X + STARRY_TEXT_W - 10 - width, y, row->value,
                             0, font, selected ? STARRY_COL_DARK : STARRY_COL_DIM, 0);
        }
        frame_push(&f);
    }
}

void starry_render_box_list(starry_render_t *r, const starry_row_t *rows, int count, int selected,
                            const starry_font_t *font)
{
    if (!r || !font) return;
    // 先整块压暗(从文本框上沿到底,并沿 188..213 行渐入),再画行 ——
    // 列表内外底色一致,上沿也不会露出硬边。
    for (int fill_y = STARRY_BOX_Y; fill_y < STARRY_SCREEN_H; fill_y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - fill_y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_box(r, &f, fill_y, rows);
        if (f.rows <= 0) break;
        blend_box_dim(&f.surface, fill_y, f.rows, STARRY_MENU_DIM_ALPHA);
        frame_push(&f);
    }
    // 行从文本框内沿 + 8px 开始(214 是底色顶边、215..216 是强调线),不再跟着"名字"跑。
    int row_height = STARRY_BOX_LIST_ROW_H;
    if (count > 0) {
        const int avail = STARRY_SCREEN_H - STARRY_BOX_LIST_TOP - 4;
        if (avail / count < row_height) row_height = avail / count;
    }
    int y = STARRY_BOX_LIST_TOP;
    for (int i = 0; i < count && i < STARRY_BOX_LIST_ROWS; ++i) {
        draw_row(r, &rows[i], y, row_height, i == selected, font);
        y += row_height;
    }
}

void starry_render_full_list(starry_render_t *r, const char *title, const starry_row_t *rows,
                             int count, int selected, const starry_font_t *font, uint16_t bg)
{
    if (!r || !font) return;
    starry_render_fill(r, bg);

    // 标题行
    if (title && title[0]) {
        for (int y = 24; y < 24 + 26; y += STARRY_STRIP_ROWS) {
            int rows = 24 + 26 - y;
            if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
            frame_t f;
            frame_begin_solid(r, &f, y, rows, bg);
            starry_text_line(&f.surface, STARRY_TEXT_X, 24, title, 0, font, STARRY_COL_ACCENT,
                             STARRY_TEXT_W);
            frame_push(&f);
        }
    }

    const int row_height = 30;
    int y = 62;
    for (int i = 0; i < count && i < STARRY_LIST_MAX_ROWS; ++i) {
        for (int row = 0; row < row_height; row += STARRY_STRIP_ROWS) {
            int chunk = row_height - row;
            if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
            frame_t f;
            frame_begin_solid(r, &f, y + row, chunk, bg);
            // ATRI 样式:每行都是圆角底(未选中深蓝、选中天蓝实底 + 深色字)。
            const bool on = i == selected;
            if (on) {
                starry_blend_rounded(&f.surface, STARRY_TEXT_X, y + 2, STARRY_TEXT_W,
                                     row_height - 4, 5, STARRY_COL_ACCENT, STARRY_ROW_SEL_ALPHA);
            } else {
                starry_blend_rounded(&f.surface, STARRY_TEXT_X, y + 2, STARRY_TEXT_W,
                                     row_height - 4, 5, STARRY_COL_ROW, STARRY_ROW_ALPHA);
            }
            const uint16_t label_color = on         ? STARRY_COL_DARK
                                         : rows[i].dim ? STARRY_COL_DIM
                                                       : STARRY_COL_TEXT;
            if (rows[i].label) {
                const int label_px = (rows[i].value && rows[i].value[0]) ? STARRY_TEXT_W - 110
                                                                        : STARRY_TEXT_W - 20;
                starry_text_line(&f.surface, STARRY_TEXT_X + 10, y, rows[i].label, 0, font,
                                 label_color, label_px);
            }
            if (rows[i].value) {
                const int width = starry_text_px(rows[i].value, 0, font);
                starry_text_line(&f.surface, STARRY_TEXT_X + STARRY_TEXT_W - 10 - width, y,
                                 rows[i].value, 0, font, on ? STARRY_COL_DARK : STARRY_COL_DIM,
                                 0);
            }
            frame_push(&f);
        }
        y += row_height + 4;
    }
}

void starry_render_center(starry_render_t *r, const char *title, const char *subtitle,
                          const starry_font_t *font, uint16_t bg)
{
    if (!r || !font) return;
    starry_render_fill(r, bg);
    const int title_y = STARRY_SCREEN_H / 2 - 30;
    for (int y = title_y; y < title_y + 28; y += STARRY_STRIP_ROWS) {
        int rows = title_y + 28 - y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        frame_t f;
        frame_begin_solid(r, &f, y, rows, bg);
        const int width = starry_text_px(title, 0, font);
        starry_text_line(&f.surface, (STARRY_SCREEN_W - width) / 2, title_y, title, 0, font,
                         STARRY_COL_TEXT, 0);
        frame_push(&f);
    }
    if (subtitle && subtitle[0]) {
        const int sub_y = title_y + 40;
        for (int y = sub_y; y < sub_y + 24; y += STARRY_STRIP_ROWS) {
            int rows = sub_y + 24 - y;
            if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
            frame_t f;
            frame_begin_solid(r, &f, y, rows, bg);
            const int width = starry_text_px(subtitle, 0, font);
            starry_text_line(&f.surface, (STARRY_SCREEN_W - width) / 2, sub_y, subtitle, 0, font,
                             STARRY_COL_DIM, 0);
            frame_push(&f);
        }
    }
}

// ---------------------------------------------------------------------------
// 提示条与整页电量
// ---------------------------------------------------------------------------
static void draw_notice_bar(starry_render_t *r, frame_t *f, const char *text,
                            const starry_font_t *font)
{
    (void)r;
    const int y = STARRY_SCREEN_H - 30;
    const int height = 24;
    const int width = starry_text_px(text, 0, font) + 24;
    const int x = (STARRY_SCREEN_W - width) / 2;
    starry_blend_rounded(&f->surface, x, y, width, height, 8, STARRY_COL_BLACK, 205);
    starry_fill_rounded(&f->surface, x + 5, y + 5, 3, height - 10, 1, STARRY_COL_ACCENT);
    starry_text_line(&f->surface, x + 14, y + 2, text, 0, font, STARRY_COL_TEXT, 0);
}

void starry_render_notice(starry_render_t *r, const char *text, const starry_font_t *font,
                          bool over_box)
{
    if (!r || !font || !text || !text[0]) return;
    const int y0 = STARRY_SCREEN_H - 30;
    const int rows = 24;
    for (int row = 0; row < rows; row += STARRY_STRIP_ROWS) {
        int chunk = rows - row;
        if (chunk > STARRY_STRIP_ROWS) chunk = STARRY_STRIP_ROWS;
        frame_t f;
        if (over_box) frame_begin_box(r, &f, y0 + row, chunk);
        else frame_begin_solid(r, &f, y0 + row, chunk, STARRY_COL_PANEL);
        if (f.rows <= 0) break;
        draw_notice_bar(r, &f, text, font);
        frame_push(&f);
    }
}

void starry_render_battery_solid(starry_render_t *r, int percent, uint16_t bg)
{
    if (!r) return;
    // 整页列表用的是纯色底(不是解码出来的画面),不参与条带烘焙。
    r->overlay_battery = STARRY_BATTERY_NONE;
    battery_ctx_t ctx = { .percent = percent, .backdrop = false };
    draw_region(r, STARRY_BATTERY_Y, STARRY_BATTERY_H, bg, false, battery_strip, &ctx);
}
