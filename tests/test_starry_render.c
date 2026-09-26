// tests/test_starry_render.c —— 用"假面板"在宿主机上跑真实页面组合,并检验【局部刷新】是否干净。
//
// 两个用途:
//   1) 自动检查:在同一块虚拟屏上连续画多个页面/多次文字更新,每一步都与"从头重画"
//      的结果逐像素比对;任何残留(上一页没被覆盖的像素)都会被点名到具体行。这正对应
//      真机上"局部刷新"类问题。
//   2) 人工检查:导出 PPM,用图片直接看界面(边框、选中条、名字/章节标签位置)。
//
// 构建与运行(tools/validate.sh 里跑):
//   (1) cc -std=c11 -Wall -Wextra -Werror -Imain -Itests/render_stubs
//           tests/test_starry_render.c main/starry_render.c main/starry_gfx.c
//           main/starry_font.c main/starry_model.c main/starry_pack.c -o test_starry_render
//   (2) ./test_starry_render assets/fonts/starry_font16.bin main/starry_data/starry_pack.bin out_dir
//            [real_bg.raw]   # 可选:240x320 小端 RGB565 的真实背景(见 tools/starry_preview_bg.py),
//                            # 给了它就按真机画面检查边框/渐隐,不给就用合成底图。
#include "starry_gfx.h"
#include "starry_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 假面板:接收大端 RGB565 条带。ref_mode=1 时画进"参考帧"(从头重画的结果)。
// ---------------------------------------------------------------------------
uint16_t g_panel[STARRY_SCREEN_W * STARRY_SCREEN_H];
static uint16_t g_ref_panel[STARRY_SCREEN_W * STARRY_SCREEN_H];
static int g_ref_mode;
static int g_push_calls;
static int g_bad_rect;

int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end,
                              int y_end, const void *color_data)
{
    (void)panel;
    if (!color_data || x_start != 0 || x_end != STARRY_SCREEN_W || y_start < 0 ||
        y_end > STARRY_SCREEN_H || y_end <= y_start) {
        g_bad_rect++;
        return -1;
    }
    g_push_calls++;
    uint16_t *dst_panel = g_ref_mode ? g_ref_panel : g_panel;
    const uint8_t *src = (const uint8_t *)color_data;
    for (int y = y_start; y < y_end; ++y) {
        uint16_t *dst = dst_panel + (size_t)y * STARRY_SCREEN_W;
        for (int x = 0; x < STARRY_SCREEN_W; ++x) {
            dst[x] = (uint16_t)((src[0] << 8) | src[1]);   // 大端 → 小端
            src += 2;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
static uint8_t *g_font_blob;
static uint8_t *g_pack_blob;
static uint32_t g_font_size;
static uint32_t g_pack_size;
static starry_font_t g_font;
static starry_pack_t g_pack;
static starry_render_t g_render;
static uint16_t g_sprite[STARRY_SPRITE_MAX_W * STARRY_SPRITE_H];
static uint16_t g_box[STARRY_SCREEN_W * STARRY_BOX_H];
static uint16_t g_strip[STARRY_SCREEN_W * STARRY_STRIP_ROWS];
static uint16_t g_hold[STARRY_SCREEN_W * STARRY_STRIP_ROWS];
static uint16_t g_name_bg[STARRY_SCREEN_W * STARRY_NAME_AREA_H];
static uint16_t g_chapter_bg[STARRY_SCREEN_W * STARRY_CHAPTER_H];
static uint8_t g_work[8192];

static int g_failures;

static uint8_t *load_file(const char *path, uint32_t *out_size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "打不开: %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    const long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, fh) != (size_t)n) exit(2);
    fclose(fh);
    *out_size = (uint32_t)n;
    return d;
}

// 画面像素的唯一事实来源:背景解码后的一屏内容 + 文本框渐入底色。
// 底色强度直接调用固件里的 starry_box_tint_alpha_at(),模型不会和实现对不上。
// g_art 是可选的真实背景(240x320 小端 RGB565);没给就用合成底。
static uint16_t *g_art;

// 立绘:与 starry_render.c 的 scene_emit 用同一套合成规则(遮罩命中处直接覆盖)。
static uint16_t *g_fg_pixels;
static starry_fg_t g_fg;
static bool g_fg_ok;

static bool fg_pixel(int x, int y, uint16_t *out)
{
    if (!g_fg_ok) return false;
    const int sx = x - (int)g_fg.x;
    const int sy = y - (int)g_fg.y;
    if (sx < 0 || sy < 0 || sx >= (int)g_fg.w || sy >= (int)g_fg.h) return false;
    const int row_bytes = ((int)g_fg.w + 7) / 8;
    if (!(g_fg.mask[(size_t)sy * row_bytes + (sx >> 3)] & (uint8_t)(0x80u >> (sx & 7)))) {
        return false;
    }
    *out = g_fg_pixels[(size_t)sy * (int)g_fg.w + sx];
    return true;
}

static uint16_t art_pixel(int x, int y)
{
    // 与 starry_render.c 的 scene_emit 同序:背景 -> 正文带 -> 立绘 -> 文字区暗帘。
    uint16_t base = g_art ? g_art[(size_t)y * STARRY_SCREEN_W + x]
                          : (uint16_t)(0x0821u + ((y >> 3) & 0x0Fu));
    const uint8_t band = starry_band_alpha_at(y);
    if (band) {
        base = starry_blend565(base, starry_rgb565(STARRY_BAND_R, STARRY_BAND_G, STARRY_BAND_B),
                               band);
    }
    uint16_t fg = 0;
    if (fg_pixel(x, y, &fg)) base = fg;   // 立绘压在带子上面
    const uint8_t scrim = starry_scrim_alpha_at(y);
    if (scrim) {
        base = starry_blend565(
            base, starry_rgb565(STARRY_SCRIM_R, STARRY_SCRIM_G, STARRY_SCRIM_B), scrim);
    }
    return base;
}

// 模拟"背景解码 + 合成"的结果:真机上由 scene_emit 填这几块缓冲,
// 底图都是 art_pixel 的对应子矩形(名字/章节/电量取画面区原样,文本框取压过底色的版本)。
static void fake_scene(uint16_t bg, uint16_t sprite)
{
    (void)bg;
    (void)sprite;
    for (int y = 0; y < STARRY_BOX_H; ++y) {
        for (int x = 0; x < STARRY_SCREEN_W; ++x) {
            g_box[y * STARRY_SCREEN_W + x] = art_pixel(x, STARRY_BOX_Y + y);
        }
    }
    for (int y = 0; y < STARRY_NAME_AREA_H; ++y) {
        for (int x = 0; x < STARRY_SCREEN_W; ++x) {
            g_name_bg[y * STARRY_SCREEN_W + x] = art_pixel(x, STARRY_NAME_Y + y);
        }
    }
    for (int y = 0; y < STARRY_CHAPTER_H; ++y) {
        for (int x = 0; x < STARRY_SCREEN_W; ++x) {
            g_chapter_bg[y * STARRY_SCREEN_W + x] = art_pixel(x, STARRY_CHAPTER_Y + y);
        }
    }
    // 真机上这一步由 starry_render_scene() 完成后置位;宿主这里模拟"场景已解码"。
    g_render.backdrops_valid = true;
}

// 参考帧:整屏按 art_pixel 铺一遍 —— 等价于"重新解码背景 + 从零重画这一页"。
static void fresh_screen(uint16_t art)
{
    (void)art;
    uint16_t *dst = g_ref_mode ? g_ref_panel : g_panel;
    for (int y = 0; y < STARRY_SCREEN_H; ++y) {
        for (int x = 0; x < STARRY_SCREEN_W; ++x) dst[y * STARRY_SCREEN_W + x] = art_pixel(x, y);
    }
}

static void write_ppm(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *fh = fopen(path, "wb");
    if (!fh) exit(2);
    fprintf(fh, "P6\n%d %d\n255\n", STARRY_SCREEN_W, STARRY_SCREEN_H);
    for (int i = 0; i < STARRY_SCREEN_W * STARRY_SCREEN_H; ++i) {
        const uint16_t c = g_panel[i];
        const uint8_t px[3] = { starry_rgb565_r(c), starry_rgb565_g(c), starry_rgb565_b(c) };
        fwrite(px, 1, 3, fh);
    }
    fclose(fh);
    printf("  写出 %s(推屏 %d 次,坏矩形 %d)\n", path, g_push_calls, g_bad_rect);
    g_push_calls = 0;
    g_bad_rect = 0;
}

// 逐像素比对"连续刷新"与"从头重画",把残留行报出来。
static void check_fresh(const char *what)
{
    int diff_rows = 0;
    int first_row = -1;
    for (int y = 0; y < STARRY_SCREEN_H; ++y) {
        if (memcmp(g_panel + (size_t)y * STARRY_SCREEN_W, g_ref_panel + (size_t)y * STARRY_SCREEN_W,
                   STARRY_SCREEN_W * sizeof(uint16_t)) != 0) {
            if (first_row < 0) first_row = y;
            diff_rows++;
        }
    }
    if (diff_rows) {
        fprintf(stderr, "FAIL 局部刷新残留: %s 有 %d 行与重画结果不一致(首行 %d)\n", what,
                diff_rows, first_row);
        g_failures++;
    } else {
        printf("  局部刷新干净: %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// 各页面:同一份代码在"参考帧"和"实测帧"上各跑一遍。
// ---------------------------------------------------------------------------
typedef void (*page_fn)(void);

static void page_tips(void)
{
    static const char *const lines[] = {
        "上 / 下 短按:下一句", "长按上:快进(松开停)", "长按下:自动阅读(900ms)",
        "确定 短按:打开菜单", "设置里可调文字速度与大小", "制作:@liuyuze61 & @EC",
    };
    starry_row_t rows[8];
    for (int i = 0; i < 6; ++i) rows[i] = (starry_row_t){ .label = lines[i], .dim = 1 };
    rows[6] = (starry_row_t){ .label = "按确定开始阅读", .dim = 0 };
    starry_render_full_list(&g_render, "操作说明", rows, 7, -1, &g_font, STARRY_COL_PANEL);
    starry_render_battery_solid(&g_render, 66, STARRY_COL_PANEL);
}

static void page_title(void)
{
    // 与 starr_ui.c 的 draw_title 一致:标题页先擦掉上一章的名字/章节标签
    starry_render_clear_overlays(&g_render);
    static const char *const menu[] = { "开始阅读", "继续阅读", "读取存档", "设置" };
    starry_row_t rows[4];
    for (int i = 0; i < 4; ++i) rows[i] = (starry_row_t){ .label = menu[i], .dim = (i == 1) };
    starry_render_box_list(&g_render, rows, 4, 0, &g_font);
    starry_render_battery(&g_render, 66);
}

static void page_reading_long(void)
{
    starry_render_auto(&g_render, true);   // 先登记自动状态,后面的带子重画才会带上它
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_name(&g_render, "诺瓦", "", &g_font);
    starry_render_body(&g_render, "客运车厢只有四节么。窗外的星空像是一条河,列车正沿着它缓缓向前。"
                                  "沿着它缓缓向前,一直开到冬天的尽头。",
                       0xFFFFFFFFu, &g_font, STARRY_SMALL_UNITS_PER_LINE);
    starry_render_battery(&g_render, 66);   // 右上角电量
}

static void page_reading_short(void)
{
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_name(&g_render, "晓", "", &g_font);
    starry_render_body(&g_render, "嗯。", 0xFFFFFFFFu, &g_font, STARRY_SMALL_UNITS_PER_LINE);
}

static void page_reading_typewriter(void)
{
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_name(&g_render, "晓", "", &g_font);
    // 打字机推进过程:逐字加长,每一步都要覆盖上一步(去掉尾巴)
    const char *text = "窗外掠过一颗流星,我下意识地看向了诺瓦。";
    const size_t len = strlen(text);
    for (size_t i = 1; i <= len; ++i) {
        starry_render_body(&g_render, text, i, &g_font, STARRY_SMALL_UNITS_PER_LINE);
    }
}

static void page_menu(void)
{
    static const char *const labels[] = { "保存进度", "读取存档", "跳过章节", "返回标题",
                                          "返回" };
    starry_row_t rows[5];
    for (int i = 0; i < 5; ++i) rows[i] = (starry_row_t){ .label = labels[i] };
    starry_render_box_list(&g_render, rows, 5, 2, &g_font);
}

static void page_settings(void)
{
    starry_row_t rows[5] = {
        { .label = "文字速度", .value = "快" },
        { .label = "文字大小", .value = "小" },
        { .label = "操作说明", .value = NULL },
        { .label = "关于", .value = NULL },
        { .label = "返回", .value = NULL },
    };
    starry_render_full_list(&g_render, "设置", rows, 5, 1, &g_font, STARRY_COL_PANEL);
    starry_render_battery_solid(&g_render, 66, STARRY_COL_PANEL);
}

static void page_slots(void)
{
    static char values[6][32];
    starry_row_t rows[6];
    for (int i = 0; i < 5; ++i) {
        snprintf(values[i], sizeof(values[i]), "第 %d 章 3/%d", i + 1, 20 + i);
        rows[i] = (starry_row_t){ .label = "存档", .value = values[i] };
    }
    rows[5] = (starry_row_t){ .label = "返回" };
    starry_render_full_list(&g_render, "保存进度", rows, 6, 0, &g_font, STARRY_COL_PANEL);
    starry_render_battery_solid(&g_render, 66, STARRY_COL_PANEL);
}

static void page_choices(void)
{
    const char *items[2] = { "跟着诺瓦走", "留在车厢里" };
    starry_render_choices(&g_render, "要在这里分别么。", items, 2, 1, &g_font);
}

static void page_chapter(void)
{
    starry_render_center(&g_render, "第 8 章", "按确定继续", &g_font, STARRY_COL_PANEL);
}

static void page_ending(void)
{
    starry_render_center(&g_render, "FIN", "感谢阅读 · 按确定返回标题", &g_font, STARRY_COL_PANEL);
}

// 跑一页:
//   参考帧 = 干净背景 + 这一页
//   实测帧 = 干净背景 + 【上一页】(留下残留)+ 这一页(不允许重新铺满整屏)
// 只有这一页自己该覆盖的区域被验证;覆盖不到的残留会被点名。
static void run_page2(const char *name, page_fn fn, page_fn prev)
{
    g_ref_mode = 1;
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    fn();
    g_ref_mode = 0;
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    if (prev) {
        prev();
        // 模拟"画面没变所以不重新解码":底图保持,但屏幕上是上一页的内容
        fake_scene(1, STARRY_NONE);
        // 真实流程里从菜单/列表页回正文会走 draw_reading(force=true):先整块重刷文本框
        starry_render_box_clear(&g_render);
    }
    fn();
    check_fresh(name);
}

static void run_page(const char *name, page_fn fn, int fresh_art)
{
    run_page2(name, fn, NULL);
    (void)fresh_art;
}

// 回归:电量那几行是整行推屏的 —— 除胶囊与圆角之外,必须逐像素等于画面底图。
// 曾经的 bug:底图只存了胶囊那一小段,其余部分被 solid 底色填成黑色 → 顶部一条黑带。
static void check_battery_band(void)
{
    g_ref_mode = 0;
    fake_scene(1, STARRY_NONE);
    starry_render_battery(&g_render, 66);
    int bad = 0;
    int first_x = -1;
    for (int y = STARRY_BATTERY_Y; y < STARRY_BATTERY_Y + STARRY_BATTERY_H; ++y) {
        int x1 = 0;
        int x2 = STARRY_SCREEN_W - 1;
        if (!starry_round_row_span(y, STARRY_SCREEN_W, STARRY_SCREEN_H, STARRY_RADIUS, &x1, &x2)) {
            continue;   // 圆角遮罩之外本来就该是黑的
        }
        for (int x = x1; x <= x2; ++x) {
            if (x >= STARRY_BATTERY_X && x < STARRY_BATTERY_X + STARRY_BATTERY_W) continue;
            // 章节标签与电量共用这几行:标签那一段本来就该有字,不能按"等于底图"要求。
            if (y >= STARRY_CHAPTER_Y && y < STARRY_CHAPTER_Y + STARRY_CHAPTER_H &&
                x >= STARRY_CHAPTER_X && x < STARRY_CHAPTER_TEXT_W) {
                continue;
            }
            if (g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y)) {
                if (first_x < 0) first_x = x;
                bad++;
            }
        }
    }
    if (bad) {
        fprintf(stderr, "FAIL 电量带非胶囊区有 %d 个像素与底图不符(首列 %d)\n", bad, first_x);
        g_failures++;
    } else {
        printf("  电量带:电量文字区与圆角以外逐像素等于画面底图\n");
    }
}

// 回归:整屏重画(解码背景)时,文字浮层必须已经画进条带里 —— 真机上曾经是
// "第一条带先把干净画面推上去、文字等解码完才补",看起来就是章节闪一下。
static void check_overlay_bake(void)
{
    g_ref_mode = 0;
    fake_scene(1, STARRY_NONE);
    // 让渲染器知道当前浮层(和阅读页一样:章节 + 名字 + 正文)。
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_name(&g_render, "诺瓦", NULL, &g_font);
    starry_render_body(&g_render, "窗外掠过一颗流星。", 0xFFFFu, &g_font,
                       STARRY_SMALL_UNITS_PER_LINE);
    starry_render_battery(&g_render, 66);

    // 模拟一次整屏重画:每个条带只做"铺干净画面 + 浮层烘焙 + 推屏",不额外补画文字。
    for (int y = 0; y < STARRY_SCREEN_H; y += STARRY_STRIP_ROWS) {
        int rows = STARRY_SCREEN_H - y;
        if (rows > STARRY_STRIP_ROWS) rows = STARRY_STRIP_ROWS;
        for (int i = 0; i < rows; ++i) {
            for (int x = 0; x < STARRY_SCREEN_W; ++x) {
                g_strip[i * STARRY_SCREEN_W + x] = art_pixel(x, y + i);
            }
        }
        starry_surface_t surface;
        starry_surface_init(&surface, g_strip, STARRY_SCREEN_W, STARRY_SCREEN_W, rows, y);
        starry_render_overlays(&g_render, &surface);
        // 相当于 esp_lcd_panel_draw_bitmap 的字节序转换。
        const uint8_t *src = (const uint8_t *)g_strip;
        for (int i = 0; i < rows; ++i) {
            uint16_t *dst = g_panel + (size_t)(y + i) * STARRY_SCREEN_W;
            for (int x = 0; x < STARRY_SCREEN_W; ++x) {
                dst[x] = (uint16_t)((src[0] << 8) | src[1]);
                src += 2;
            }
        }
    }

    // 三块浮层区域都要出现"与底图不同"的像素;否则说明文字没被烘进条带。
    int chapter_ink = 0;
    int name_ink = 0;
    int body_ink = 0;
    int battery_ink = 0;
    for (int y = STARRY_CHAPTER_Y; y < STARRY_CHAPTER_Y + STARRY_CHAPTER_H; ++y) {
        for (int x = STARRY_CHAPTER_X; x < STARRY_CHAPTER_TEXT_W; ++x) {
            chapter_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }
    for (int y = STARRY_NAME_Y; y < STARRY_NAME_Y + STARRY_NAME_AREA_H; ++y) {
        for (int x = STARRY_NAME_X; x < STARRY_SCREEN_W; ++x) {
            name_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }
    for (int y = STARRY_BOX_Y; y < STARRY_BOX_Y + STARRY_BOX_H; ++y) {
        for (int x = 0; x < STARRY_SCREEN_W; ++x) {
            body_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }
    for (int y = STARRY_BATTERY_Y; y < STARRY_BATTERY_Y + STARRY_BATTERY_H; ++y) {
        for (int x = STARRY_BATTERY_X; x < STARRY_BATTERY_X + STARRY_BATTERY_W; ++x) {
            battery_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }
    if (chapter_ink <= 0 || name_ink <= 0 || body_ink <= 0 || battery_ink <= 0) {
        fprintf(stderr, "FAIL 整屏重画后浮层没进条带: 章节 %d / 名字 %d / 正文 %d 个文字像素\n",
                chapter_ink, name_ink, body_ink);
        g_failures++;
    } else {
        printf("  整屏重画:章节 %d / 名字 %d / 正文 %d / 电量 %d 个文字像素已在条带里\n", chapter_ink,
               name_ink, body_ink, battery_ink);
    }
}

// 回归:章节与电量共用 6..25 行,两者都从"干净底图"整行恢复 —— 后画的那个很容易
// 把先画的擦掉。真机上的表现是"章节刚显示就消失"。这里两个方向都验一遍。
static void check_overlay_mutual(void)
{
    int chapter_ink = 0;
    int battery_ink = 0;

    g_ref_mode = 0;
    fake_scene(1, STARRY_NONE);
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_battery(&g_render, 55);
    for (int y = STARRY_CHAPTER_Y; y < STARRY_CHAPTER_Y + STARRY_CHAPTER_H; ++y) {
        for (int x = STARRY_CHAPTER_X; x < STARRY_CHAPTER_TEXT_W; ++x) {
            chapter_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }

    // 反方向:先电量、后章节
    g_ref_mode = 0;
    fake_scene(1, STARRY_NONE);
    starry_render_battery(&g_render, 55);
    starry_render_chapter(&g_render, "第8章", &g_font);
    for (int y = STARRY_BATTERY_Y; y < STARRY_BATTERY_Y + STARRY_BATTERY_H; ++y) {
        for (int x = STARRY_BATTERY_X; x < STARRY_BATTERY_X + STARRY_BATTERY_W; ++x) {
            battery_ink += g_panel[y * STARRY_SCREEN_W + x] != art_pixel(x, y);
        }
    }

    if (chapter_ink <= 0 || battery_ink <= 0) {
        fprintf(stderr, "FAIL 章节与电量互相擦除: 画电量后章节剩 %d 像素, 画章节后电量剩 %d 像素\n",
                chapter_ink, battery_ink);
        g_failures++;
    } else {
        printf("  章节/电量共存:画电量后章节 %d 像素,画章节后电量 %d 像素\n", chapter_ink,
               battery_ink);
    }
}

// 回归:底图还没就绪时擦浮层/重刷文本框不能推出黑带(曾经在标题页顶部露出黑条)。
static void check_backdrop_guard(void)
{
    g_ref_mode = 0;
    g_render.backdrops_valid = false;
    for (int i = 0; i < STARRY_SCREEN_W * STARRY_SCREEN_H; ++i) g_panel[i] = 0xFFFFu;
    starry_render_clear_overlays(&g_render);
    starry_render_box_clear(&g_render);
    starry_render_chapter(&g_render, "第8章", &g_font);
    starry_render_name(&g_render, "诺瓦", NULL, &g_font);
    int black = 0;
    for (int i = 0; i < STARRY_SCREEN_W * STARRY_SCREEN_H; ++i) black += g_panel[i] == 0;
    if (black) {
        fprintf(stderr, "FAIL 底图未就绪时写出了 %d 个黑色像素(会看到黑带)\n", black);
        g_failures++;
    } else {
        printf("  底图未就绪:浮层与文本框重刷被正确跳过,屏幕未被涂黑\n");
    }
    g_render.backdrops_valid = true;
}

int main(int argc, char **argv)
{
    const char *font_path = argc > 1 ? argv[1] : "assets/fonts/starry_font16.bin";
    const char *pack_path = argc > 2 ? argv[2] : "main/starry_data/starry_pack.bin";
    const char *out_dir = argc > 3 ? argv[3] : ".";
    const char *art_path = argc > 4 ? argv[4] : NULL;
    const char *fg_path = argc > 5 ? argv[5] : NULL;
    const uint16_t fg_id = argc > 6 ? (uint16_t)atoi(argv[6]) : 0;

    static uint16_t art_storage[STARRY_SCREEN_W * STARRY_SCREEN_H];
    if (art_path) {
        uint32_t art_size = 0;
        uint8_t *blob = load_file(art_path, &art_size);
        if (art_size != sizeof(art_storage)) {
            fprintf(stderr, "背景底图大小不对: %u(应为 %u)\n", (unsigned)art_size,
                    (unsigned)sizeof(art_storage));
            return 2;
        }
        memcpy(art_storage, blob, art_size);
        free(blob);
        g_art = art_storage;
        printf("真实背景: %s\n", art_path);
    }
    g_font_blob = load_file(font_path, &g_font_size);
    g_pack_blob = load_file(pack_path, &g_pack_size);
    if (!starry_font_open(&g_font, g_font_blob, g_font_size)) {
        fprintf(stderr, "字形包不合法\n");
        return 2;
    }
    if (!starry_pack_open(&g_pack, g_pack_blob, g_pack_size)) {
        fprintf(stderr, "资源包不合法\n");
        return 2;
    }
    static uint16_t fg_storage[STARRY_SPRITE_MAX_W * STARRY_SPRITE_H];
    if (fg_path) {
        uint32_t fg_size = 0;
        uint8_t *blob = load_file(fg_path, &fg_size);
        if (!starry_pack_fg(&g_pack, fg_id, &g_fg)) {
            fprintf(stderr, "资源包里没有立绘 %u\n", (unsigned)fg_id);
            return 2;
        }
        if (fg_size != (uint32_t)g_fg.w * g_fg.h * 2u) {
            fprintf(stderr, "立绘底图大小不对: %u(应为 %ux%ux2)\n", (unsigned)fg_size,
                    (unsigned)g_fg.w, (unsigned)g_fg.h);
            return 2;
        }
        if (g_fg.w > STARRY_SPRITE_MAX_W || g_fg.h > STARRY_SPRITE_H) {
            fprintf(stderr, "立绘超缓冲: %ux%u\n", (unsigned)g_fg.w, (unsigned)g_fg.h);
            return 2;
        }
        memcpy(fg_storage, blob, fg_size);
        free(blob);
        g_fg_pixels = fg_storage;
        g_fg_ok = true;
        printf("真实立绘 %u: %ux%u @ (%u,%u)\n", (unsigned)fg_id, (unsigned)g_fg.w,
               (unsigned)g_fg.h, (unsigned)g_fg.x, (unsigned)g_fg.y);
    }
    const starry_render_buffers_t buffers = {
        .sprite = g_sprite, .box = g_box, .strip = g_strip, .hold = g_hold,
        .name_bg = g_name_bg, .chapter_bg = g_chapter_bg,
        .jpeg_work = g_work, .jpeg_work_size = sizeof(g_work),
    };
    if (!starry_render_init(&g_render, &g_pack, (esp_lcd_panel_handle_t)1, NULL, &g_font, &g_font,
                            &buffers)) {
        fprintf(stderr, "渲染器初始化失败\n");
        return 2;
    }

    printf("--- 逐页局部刷新自检 ---\n");
    run_page("操作说明页", page_tips, 0);
    run_page2("标题页菜单(在正文页之后画)", page_title, page_reading_long);
    run_page("标题页菜单", page_title, 0);
    run_page("正文页(长句)", page_reading_long, 0);
    run_page2("正文页(短句,接在长句之后)", page_reading_short, page_reading_long);
    run_page2("打字机推进", page_reading_typewriter, page_reading_short);
    run_page("游戏内菜单", page_menu, 0);
    run_page2("正文页(接在菜单之后)", page_reading_long, page_menu);
    run_page("设置页", page_settings, 0);
    run_page("存档页", page_slots, 0);
    run_page("选项页", page_choices, 0);
    run_page("章节过场", page_chapter, 0);
    run_page("结局页", page_ending, 0);

    printf("--- 页面切换序列(菜单 -> 正文)---\n");
    // 真实流程:正文 -> 打开菜单 -> 回到正文,回来时必须完全恢复(不能留菜单的行条)
    g_ref_mode = 0;
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_reading_long();
    page_menu();
    starry_render_box_clear(&g_render);   // 与 draw_reading(force=true) 一致
    page_reading_long();
    g_ref_mode = 1;
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_reading_long();
    g_ref_mode = 0;
    check_fresh("菜单->正文(整块重画)");

    check_battery_band();
    check_backdrop_guard();
    check_overlay_bake();
    check_overlay_mutual();

    printf("--- 导出图片 ---\n");
    g_ref_mode = 0;
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_tips();
    write_ppm(out_dir, "page_tips");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_reading_long();
    write_ppm(out_dir, "page_reading");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_menu();
    write_ppm(out_dir, "page_menu");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_choices();
    write_ppm(out_dir, "page_choices");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_settings();
    write_ppm(out_dir, "page_settings");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_chapter();
    write_ppm(out_dir, "page_chapter");
    fresh_screen(0);
    fake_scene(1, STARRY_NONE);
    page_ending();
    write_ppm(out_dir, "page_ending");

    free(g_font_blob);
    free(g_pack_blob);
    if (g_bad_rect) {
        fprintf(stderr, "有 %d 次推屏矩形不合法\n", g_bad_rect);
        return 1;
    }
    if (g_failures) {
        fprintf(stderr, "starry_render: %d 项失败\n", g_failures);
        return 1;
    }
    printf("starry_render: PASS(局部刷新逐页干净,推屏参数合法)\n");
    return 0;
}
