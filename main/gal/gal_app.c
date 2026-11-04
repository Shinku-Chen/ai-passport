// main/gal/gal_app.c —— 《纸上的魔法使》横屏（320x240 屏，320x150 画面区）LVGL 界面与交互。
//
// 版面：320x150 画面区（背景/CG/立绘合成）+ 底部 90px 不透明对话框；
// 名牌在框上沿。列表页（菜单/存档/章节/鉴赏）复用 5 行窗口，UP/DOWN 移动、OK 确认、
// 长按 OK 返回。正文按 300px 宽、16px 字、4 行分页；20px 档本作因 Flash 预算未启用。
#include "gal_app.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "paper_pack.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gal_cjk_16);

static const char *TAG = "paper_app";

#define SCR_W 320
#define SCR_H 240
#define BOX_H 64           // 176..240：与画布实际高度(176 行)对齐，接缝正好在面板顶
#define BOX_Y 176
#define ART_H BOX_Y        // 立绘基线 = 对话框顶
#define TEXT_X 10
#define TEXT_W (SCR_W - 2 * TEXT_X)
#define LINE_H 19
#define TEXT_LINES 3          // 面板 64 - 顶部 4px = 60px，3 行 x19 = 57 放得下
#define LIST_ROW_H 30
#define LIST_TOP 30

#define SPEED_MIN_MS(s) ((s) == GAL_SPEED_SLOW ? 55 : (s) == GAL_SPEED_FAST ? 10 : 25)
#define AUTO_WAIT_MS 1200
// 快进：每个 tick 立刻推进（无 page 间延迟），文字由 typing=false 瞬显。
#define FF_STEP_MS 0
// CG 铺满：320x150 画布 cover 到 320x240 = 1.6x（256=1.0x）。
#define PAPER_CG_SCALE 410

static const char *const s_title_menu[] = {
    "开始游戏", "继续阅读", "读取存档", "CG 鉴赏", "章节选择", "设置", "关于",
};
static const int s_title_menu_n = (int)(sizeof(s_title_menu) / sizeof(s_title_menu[0]));

static const char *const s_game_menu[] = {
    "继续阅读", "保存进度", "读取存档", "历史文本", "设置", "CG 鉴赏", "章节选择", "返回标题",
};
static const int s_game_menu_n = (int)(sizeof(s_game_menu) / sizeof(s_game_menu[0]));

// 标题背景：开场场景（脚本 scene 11）里的图书馆背景（图片表 472，267x150）。
// 不用 0（事件 CG，会剧透）也不用 503（纯黑场）；换包时这个下标要同步。
#define PAPER_TITLE_IMAGE 472

// ---------------------------------------------------------------- 小工具
static void set_bg(lv_obj_t *o, uint32_t hex, lv_opa_t opa)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &gal_cjk_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

static int s_list_top = LIST_TOP;
static int s_list_row_h = LIST_ROW_H;
// 标题背景只在进入标题页时解一次（光标移动也会 rebuild，不能每键都解 JPEG）。
static bool s_title_bg_drawn;

#ifdef PAPER_UI_TEST_HOOKS
// 最近一次画进画布的标题图下标（host 回归断言用）。
uint16_t gal_app_test_title_image;
#endif

static lv_obj_t *mk_list_row(lv_obj_t *parent, int i)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, SCR_W - 24, s_list_row_h - 4);
    lv_obj_set_pos(r, 12, s_list_top + i * s_list_row_h);
    set_bg(r, 0x1A2233, LV_OPA_80);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x2E3C55), 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    return r;
}

static void row_set(lv_obj_t *row, const char *text, bool selected)
{
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (!lbl) {
        lbl = mk_label(row, text, 8, 3, SCR_W - 40);
    } else {
        lv_label_set_text(lbl, text);
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x2F4A78 : 0x1A2233), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(selected ? 0xFFE9A8 : 0xDDDDDD), 0);
}

static int utf8_next(const char *s, int i)
{
    unsigned char c = (unsigned char)s[i];
    if (!c) {
        return i;
    }
    if (c < 0x80) {
        return i + 1;
    }
    if ((c >> 5) == 6) {
        return i + 2;
    }
    if ((c >> 4) == 14) {
        return i + 3;
    }
    return i + 4;
}

// 从字节 offset 往后推进 n 个码点，返回新的字节偏移（保证 UTF-8 边界）
static int utf8_advance(const char *s, int offset, int n)
{
    int len = (int)strlen(s);
    int i = offset;
    while (n-- > 0 && i < len) {
        i = utf8_next(s, i);
    }
    if (i > len) {
        i = len;
    }
    // 回退到合法起始字节（防御：源串若被截断在半途）
    while (i > 0 && i < len && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

static int glyph_w(const char *s, int i)
{
    unsigned char c = (unsigned char)s[i];
    return c < 0x80 ? 8 : 16;
}

// ---------------------------------------------------------------- 页面
static void clear_root(gal_app_t *app)
{
    if (app->root) {
        lv_obj_del(app->root);
    }
    app->root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app->root, SCR_W, SCR_H);
    lv_obj_set_pos(app->root, 0, 0);
    set_bg(app->root, 0x000000, LV_OPA_COVER);
    lv_obj_set_scrollbar_mode(app->root, LV_SCROLLBAR_MODE_OFF);
    app->img_art = NULL;
    app->box = NULL;
    app->lbl_name = NULL;
    app->lbl_text = NULL;
    app->lbl_hint = NULL;
    app->lbl_notice = NULL;
    app->header = NULL;
    for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
        app->rows[i] = NULL;
        app->row_labels[i] = NULL;
    }
}

static void draw_scene(gal_app_t *app, uint16_t frame, const uint16_t sprites[3])
{
    app->draw_error = false;
    bool frame_ok = true;
    if (frame != 0xFFFF) {
        frame_ok = paper_gfx_draw_frame(app->pack, frame);
        if (!frame_ok) {
            paper_gfx_clear(0x4000);   // 明确的错误画面（洋红），不静默缺图
        }
    } else {
        paper_gfx_clear(0x0000);
    }
    // 槽位从左到右排列（1 人居中、2 人左右、3 人左中右），底部对齐、同屏可见
    uint16_t order[3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (sprites && sprites[i] != 0xFFFF) {
            order[n++] = sprites[i];
        }
    }
    static const int center1[1] = {160};
    static const int center2[2] = {88, 232};
    static const int center3[3] = {64, 160, 256};
    const int *centers = n == 1 ? center1 : n == 2 ? center2 : center3;
    bool sprites_ok = true;
    for (int i = 0; i < n; i++) {
        int w = 0, h = 0;
        if (!paper_gfx_image_size(app->pack, order[i], &w, &h)) {
            sprites_ok = false;
            continue;
        }
        int x = centers[i] - w / 2;
        if (x < 0) {
            x = 0;
        }
        if (x + w > SCR_W) {
            x = SCR_W - w;
        }
        if (!paper_gfx_draw_sprite(app->pack, order[i], x, ART_H - h)) {
            sprites_ok = false;
        }
    }
    // 整屏成功（背景 + 全部立绘）才解锁该 CG；任一失败：洋红占位 + 可见提示，不解锁
    if (frame_ok && sprites_ok && frame != 0xFFFF) {
        paper_game_unlock_image(&app->game, frame);
    } else if (!frame_ok || !sprites_ok) {
        app->draw_error = true;
        if (app->lbl_hint) {
            lv_label_set_text(app->lbl_hint, "素材读取失败");
        }
    }
    if (app->img_art) {
        lv_obj_invalidate(app->img_art);
    }
}

// 把 app->text 按 300px/16px/4 行分页；返回页数，page_start[] 存每页起始字节
static int paginate(const char *text, int *page_start, int max_pages, int *page_len)
{
    int len = (int)strlen(text);
    int pages = 0;
    int i = 0;
    while (i < len && pages < max_pages) {
        int start = i;
        for (int line = 0; line < TEXT_LINES && i < len; line++) {
            int w = 0;
            int line_start = i;
            while (i < len) {
                if (text[i] == '\n') {
                    i++;
                    break;
                }
                int gw = glyph_w(text, i);
                if (w + gw > TEXT_W) {
                    // 回退到上一个可断点（尽量在标点后断）
                    int soft = i;
                    for (int k = i - 1; k > line_start; k--) {
                        if ((unsigned char)text[k] == ' ' || (unsigned char)text[k] == 0xE3) {
                            soft = k;
                            break;
                        }
                    }
                    if (soft != i && soft > line_start) {
                        i = soft;
                        if ((unsigned char)text[i] == ' ') {
                            i++;
                        }
                    }
                    break;
                }
                w += gw;
                i = utf8_next(text, i);
            }
        }
        page_start[pages] = start;
        page_len[pages] = i - start;
        pages++;
    }
    return pages ? pages : 1;
}

static int s_page_start[64];
static int s_page_len[64];

// 测试钩子：当前页的字节长度（断言快进时 typed == 页长）。
int gal_app_test_page_len(const gal_app_t *app)
{
    if (!app || app->page_index >= app->page_count) return 0;
    return s_page_len[app->page_index];
}

static void reader_refresh_text(gal_app_t *app)
{
    if (!app->lbl_text) {
        return;
    }
    char buf[TEXT_LINES * 64];
    const char *src = app->text + s_page_start[app->page_index];
    int n = s_page_len[app->page_index];
    if (app->typed < n) {
        n = app->typed;   // typed 永远停在 UTF-8 码点边界
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    memcpy(buf, src, n);
    buf[n] = 0;
    // 追加换行由 LVGL 自动折行处理（宽度已按 300px 排版，仍保留手动分页）
    lv_label_set_text(app->lbl_text, buf);
    if (app->lbl_page) {
        char page[32];
        if (app->page_count > 1) {
            snprintf(page, sizeof(page), "%d/%d", app->page_index + 1, app->page_count);
        } else {
            page[0] = 0;
        }
        lv_label_set_text(app->lbl_page, page);
    }
}

// 全屏画面：**一个** lv_image，对象 320x240，内容按 4/3 放大（画布 240x180 -> 320x240，零裁切）。
// 以前画布只有 320x150 且不缩放，150..240 全是黑 —— 面板半透明也透不出东西。
static void make_art_image(gal_app_t *app)
{
    app->img_art = lv_image_create(app->root);
    if (!app->img_art) {
        return;
    }
    lv_image_set_src(app->img_art, paper_gfx_canvas());
    lv_obj_set_pos(app->img_art, 0, 0);
    lv_obj_set_style_bg_opa(app->img_art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(app->img_art, 0, 0);
    lv_obj_set_style_pad_all(app->img_art, 0, 0);
    lv_obj_move_to_index(app->img_art, 0);
}

static void load_step(gal_app_t *app)
{
    if (app->page != GAL_PAGE_READER && app->page != GAL_PAGE_CHOICE) {
        app->auto_scene = 0xFFFF;
    }
    if (app->page != GAL_PAGE_READER) {
        app->fast_forward = false;
    }
    for (int guard = 0; guard < 64; guard++) {
        const paper_step_t *st = paper_game_step(&app->game);
        if (!st) {
            return;
        }
        if (st->kind == PAPER_STEP_TEXT) {
            // 画面（背景 + 立绘）没变时**不得** clear_root/重建对象/重解码：
            // 那是快进"每句之间明显停顿"的根源（每句都销毁 LVGL 对象 + 解一张背景）。
            const bool same_visual = app->img_art != NULL &&
                                     app->cur_bg == st->background &&
                                     memcmp(app->cur_sprites, st->sprites,
                                            sizeof(app->cur_sprites)) == 0;
            app->speaker = st->speaker;
            snprintf(app->text, sizeof(app->text), "%s", st->text ? st->text : "");
            app->page_count = paginate(app->text, s_page_start, 64, s_page_len);
            app->page_index = 0;
            app->typed = 0;
            // 快进时整页瞬显（自动阅读仍逐字）。
            app->typing = app->speed != GAL_SPEED_INSTANT && !app->fast_forward;
            if (app->fast_forward) app->typed = s_page_len[0];
            app->idle_ms = 0;
            app->auto_accum_ms = 0;
            if (app->game.scene_id != app->auto_scene) {
                app->auto_scene = app->game.scene_id;
                (void)paper_game_save(&app->game, PAPER_SLOT_AUTO);
            }
            app->page = GAL_PAGE_READER;
            app->page_depth = 0;   // 进入正文 = 回到根页，清掉菜单/章节页留下的来源
            if (same_visual) {
                if (app->lbl_name) {
                    lv_label_set_text(app->lbl_name,
                                      paper_speaker_name(app->pack, app->speaker));
                }
                reader_refresh_text(app);
                if (app->typing && app->speed != GAL_SPEED_INSTANT) {
                    app->typed = 0;
                }
                return;
            }
            app->cur_bg = st->background;
            memcpy(app->cur_sprites, st->sprites, sizeof(app->cur_sprites));
            clear_root(app);
            // 画布 + 对话框
            make_art_image(app);
            lv_obj_set_pos(app->img_art, 0, 0);
            app->box = lv_obj_create(app->root);
            lv_obj_set_size(app->box, SCR_W, BOX_H);
            lv_obj_set_pos(app->box, 0, BOX_Y);
            set_bg(app->box, 0x0B1220, 150);   // 0x96 半透明：比 0xB4 更透，画面能透上来
            app->lbl_name = mk_label(app->root, paper_speaker_name(app->pack, app->speaker), 8,
                                     BOX_Y - 21, 0);
            // 名字底条只包文字（内容宽 + 左右 6px），不再固定 200 拉满。
            lv_obj_set_width(app->lbl_name, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(app->lbl_name, 6, 0);
            lv_obj_set_style_bg_color(app->lbl_name, lv_color_hex(0x0B1220), 0);
            lv_obj_set_style_bg_opa(app->lbl_name, 150, 0);
            // 页码挪到面板外右上（BOX_Y-18..BOX_Y），正文才能贴面板顶 +4。
            app->lbl_page = mk_label(app->root, "", 262, BOX_Y - 18, 50);
            lv_obj_set_style_text_align(app->lbl_page, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_color(app->lbl_page, lv_color_hex(0x7C8FA6), 0);
            app->lbl_text = mk_label(app->root, "", TEXT_X, BOX_Y + 4, TEXT_W);
            lv_obj_set_style_text_line_space(app->lbl_text, 0, 0);
            app->lbl_hint = mk_label(app->root, "", SCR_W - 92, 2, 88);
            lv_obj_set_style_text_align(app->lbl_hint, LV_TEXT_ALIGN_RIGHT, 0);
            draw_scene(app, st->background, st->sprites);
            reader_refresh_text(app);
            if (app->typing && app->speed != GAL_SPEED_INSTANT) {
                app->typed = 0;
            }
            return;
        }
        if (st->kind == PAPER_STEP_CHOICE) {
            app->choice_index = st->choice;
            app->choice_count = st->choice_count;
            app->choice_sel = 0;
            app->page = GAL_PAGE_CHOICE;
            app->idle_ms = 0;
            clear_root(app);
            make_art_image(app);
            mk_label(app->root, "请选择", 12, 6, 200);
            for (int i = 0; i < st->choice_count && i < GAL_ROWS_VISIBLE; i++) {
                const paper_choice_item_t *it = paper_choice_item(app->pack, st->choice, i);
                const char *label = it ? paper_text_get(app->pack, it->text_off, it->len) : "?";
                app->rows[i] = mk_list_row(app->root, i);
                row_set(app->rows[i], label ? label : "?", i == app->choice_sel);
            }
            return;
        }
        if (st->kind == PAPER_STEP_END) {
            // 记录结局
            for (uint32_t i = 0; i < app->pack->ending_count; i++) {
                if (paper_ending_scene(app->pack, i) == st->end_scene) {
                    app->game.endings_seen |= (uint8_t)(1u << i);
                }
            }
            paper_game_save_meta(&app->game);
            app->page = GAL_PAGE_ENDING;
            app->page_depth = 0;   // 结局页是根页，不带任何来源栈
            app->idle_ms = 0;
            clear_root(app);
            make_art_image(app);
            char buf[256];
            const char *ename = "";
            for (uint32_t i = 0; i < app->pack->ending_count; i++) {
                if (paper_ending_scene(app->pack, i) == st->end_scene) {
                    ename = paper_ending_name(app->pack, i);
                }
            }
            snprintf(buf, sizeof(buf), "THE END\n\n%s", ename);
            mk_label(app->root, buf, 20, 80, SCR_W - 40);
            mk_label(app->root, "长按 OK 返回标题", 20, 200, SCR_W - 40);
            return;
        }
        // ERROR：保持当前页并提示
        snprintf(app->notice, sizeof(app->notice), "剧本执行错误");
        app->notice_ms = 3000;
        return;
    }
}

// ---------------------------------------------------------------- 列表页
static void build_list(gal_app_t *app, const char *title, int count, int sel)
{
    clear_root(app);
    app->header = mk_label(app->root, title, 12, 4, SCR_W - 24);
    lv_obj_set_style_text_color(app->header, lv_color_hex(0xFFD98A), 0);
    int top = sel - GAL_ROWS_VISIBLE / 2;
    if (top < 0) {
        top = 0;
    }
    if (top > count - GAL_ROWS_VISIBLE) {
        top = count - GAL_ROWS_VISIBLE;
    }
    if (top < 0) {
        top = 0;
    }
    app->top = top;
    int shown = count - top < GAL_ROWS_VISIBLE ? count - top : GAL_ROWS_VISIBLE;
    for (int i = 0; i < shown; i++) {
        app->rows[i] = mk_list_row(app->root, i);
        row_set(app->rows[i], "", false);
    }
}

static void save_slot_label(gal_app_t *app, int slot, char *buf, size_t cap)
{
    char brief[64];
    if (paper_game_slot_brief(slot, app->pack, brief, sizeof(brief))) {
        snprintf(buf, cap, "存档 %d · %s", slot + 1, brief);
    } else {
        snprintf(buf, cap, "存档 %d · 空", slot + 1);
    }
}

// CG 铺满：**一个** lv_image 对象，对象固定 320x240；缩放比用画布**实际高**算，
// 不写死常数（画布高改过就会算错 → 实机“CG 没铺满”）。
static int cg_cover_scale(void)
{
    const lv_image_dsc_t *dsc = paper_gfx_canvas();
    const int src_h = dsc ? (int)dsc->header.h : PAPER_SCR_H;
    const int src_w = dsc ? (int)dsc->header.w : PAPER_SCR_W;
    if (src_h <= 0 || src_w <= 0) return 256;
    const int sy = (SCR_H * 256) / src_h;
    const int sx = (SCR_W * 256) / src_w;
    return sx > sy ? sx : sy;      // cover：取大者
}

// 建 CG 图像：src -> scale -> inner_align，最后 invalidate；打印一次诊断日志。
static lv_obj_t *make_cg_image(gal_app_t *app)
{
    lv_obj_t *img = lv_image_create(app->root);
    lv_image_set_src(img, paper_gfx_canvas());
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_size(img, SCR_W, SCR_H);
    const int scale = cg_cover_scale();
    lv_image_set_scale(img, (uint32_t)scale);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(img, 0, 0);
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_move_to_index(img, 0);
    lv_obj_invalidate(img);
    return img;
}

static void rebuild_current_page(gal_app_t *app);

static void goto_page(gal_app_t *app, gal_page_t page)
{
    if (page != app->page) {
        const int cap = (int)(sizeof(app->page_stack) / sizeof(app->page_stack[0]));
        if (app->page_depth >= cap) {
            // 栈满时丢最旧一层：宁可少一层，也不能让 go_back() 弹到错误页面。
            memmove(&app->page_stack[0], &app->page_stack[1],
                    (size_t)(cap - 1) * sizeof(app->page_stack[0]));
            app->page_depth = cap - 1;
        }
        app->page_stack[app->page_depth++] = app->page;   // 压栈记录真实来源
        app->back_page = app->page;
    }
    app->page = page;
    if (page == GAL_PAGE_TITLE) s_title_bg_drawn = false;   // 进标题页必重画背景
    rebuild_current_page(app);
}

// 回退：弹出上一级来源；栈空则回标题，绝不改写更上一级来源
static void go_back(gal_app_t *app)
{
    if (app->page_depth > 0) {
        app->page = app->page_stack[--app->page_depth];
    } else {
        app->page = GAL_PAGE_TITLE;
    }
    app->back_page = app->page_depth > 0 ? app->page_stack[app->page_depth - 1] : GAL_PAGE_TITLE;
    rebuild_current_page(app);
}

static void rebuild_current_page(gal_app_t *app)
{
    char buf[96];
    switch (app->page) {
    case GAL_PAGE_TITLE: {
        clear_root(app);
        make_art_image(app);
        // 光标移动也会重建本页：背景只在进标题页时解一次。
        if (!s_title_bg_drawn) {
            (void)paper_gfx_draw_frame(app->pack, PAPER_TITLE_IMAGE);
            s_title_bg_drawn = true;
#ifdef PAPER_UI_TEST_HOOKS
            gal_app_test_title_image = PAPER_TITLE_IMAGE;
#endif
        }
        mk_label(app->root, "纸上的魔法使", 16, 2, SCR_W - 32);
        lv_obj_t *sub = mk_label(app->root, "紙の上の魔法使い", 16, 22, SCR_W - 32);
        lv_obj_set_style_text_color(sub, lv_color_hex(0xBFD0E6), 0);
        // 7 项菜单：窗口跟随光标滚动，行从副标题下方 46px 起
        s_list_top = 46;
        s_list_row_h = 27;
        int top = app->sel - GAL_ROWS_VISIBLE / 2;
        if (top < 0) {
            top = 0;
        }
        if (top > s_title_menu_n - GAL_ROWS_VISIBLE) {
            top = s_title_menu_n - GAL_ROWS_VISIBLE;
        }
        app->top = top;
        for (int i = 0; i < GAL_ROWS_VISIBLE && top + i < s_title_menu_n; i++) {
            app->rows[i] = mk_list_row(app->root, i);
            row_set(app->rows[i], s_title_menu[top + i], (top + i) == app->sel);
        }
        s_list_top = LIST_TOP;
        s_list_row_h = LIST_ROW_H;
        break;
    }
    case GAL_PAGE_MENU:
        build_list(app, "菜单", s_game_menu_n, app->menu_sel);
        for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
            int idx = app->top + i;
            if (idx >= s_game_menu_n || !app->rows[i]) {
                continue;
            }
            row_set(app->rows[i], s_game_menu[idx], idx == app->menu_sel);
        }
        break;
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD:
        build_list(app, app->page == GAL_PAGE_SAVE ? "保存进度" : "读取存档", PAPER_SLOTS, app->sel);
        for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
            int idx = app->top + i;
            if (idx >= PAPER_SLOTS || !app->rows[i]) {
                continue;
            }
            save_slot_label(app, idx, buf, sizeof(buf));
            row_set(app->rows[i], buf, idx == app->sel);
        }
        break;
    case GAL_PAGE_SETTINGS: {
        // 只提供真实可用的设置项：字号固定 16px（20px 不在本作 Flash 预算内，不展示无效选项）
        static const char *const names[] = {"文字速度", "自动播放", "返回"};
        build_list(app, "设置", 3, app->sel);
        for (int i = 0; i < 3; i++) {
            int idx = app->top + i;
            if (idx >= 3 || !app->rows[i]) {
                continue;
            }
            const char *val = "";
            if (idx == 0) {
                val = app->speed == GAL_SPEED_SLOW ? "慢" : app->speed == GAL_SPEED_NORMAL ? "中"
                      : app->speed == GAL_SPEED_FAST ? "快" : "瞬间";
            } else if (idx == 1) {
                val = app->auto_mode ? "开" : "关";
            }
            snprintf(buf, sizeof(buf), "%s    %s", names[idx], val);
            row_set(app->rows[i], buf, idx == app->sel);
        }
        break;
    }
    case GAL_PAGE_HISTORY: {
        int n = paper_game_history_count(&app->game);
        build_list(app, "历史文本", n, app->sel);
        for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
            int idx = app->top + i;
            const paper_history_entry_t *e = paper_game_history_at(&app->game, idx);
            if (!e || !app->rows[i]) {
                continue;
            }
            snprintf(buf, sizeof(buf), "%s：%.40s", e->speaker, e->text);
            row_set(app->rows[i], buf, idx == app->sel);
        }
        break;
    }
    case GAL_PAGE_GALLERY: {
        clear_root(app);
        app->header = mk_label(app->root, "CG 鉴赏", 12, 4, SCR_W - 24);
        lv_obj_set_style_text_color(app->header, lv_color_hex(0xFFD98A), 0);
        int top = app->sel - GAL_ROWS_VISIBLE / 2;
        if (top < 0) {
            top = 0;
        }
        if (top > (int)app->pack->cg_count - GAL_ROWS_VISIBLE) {
            top = (int)app->pack->cg_count - GAL_ROWS_VISIBLE;
        }
        if (top < 0) {
            top = 0;
        }
        app->top = top;
        for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
            int idx = top + i;
            if (idx >= (int)app->pack->cg_count) {
                break;
            }
            app->rows[i] = mk_list_row(app->root, i);
            bool un = paper_game_cg_unlocked(&app->game, idx);
            snprintf(buf, sizeof(buf), "%s  %s%s", un ? "◆" : "◇", paper_cg_name(app->pack, idx),
                     un ? "" : "（未解锁）");
            row_set(app->rows[i], buf, idx == app->sel);
            if (!un) {
                lv_obj_set_style_text_color(lv_obj_get_child(app->rows[i], 0),
                                            lv_color_hex(0x808080), 0);
            }
        }
        break;
    }
    case GAL_PAGE_CHAPTER: {
        clear_root(app);
        app->header = mk_label(app->root, "章节选择", 12, 4, SCR_W - 24);
        lv_obj_set_style_text_color(app->header, lv_color_hex(0xFFD98A), 0);
        int count = (int)app->pack->chapter_count;
        int top = app->sel - GAL_ROWS_VISIBLE / 2;
        if (top < 0) {
            top = 0;
        }
        if (top > count - GAL_ROWS_VISIBLE) {
            top = count - GAL_ROWS_VISIBLE;
        }
        if (top < 0) {
            top = 0;
        }
        app->top = top;
        for (int i = 0; i < GAL_ROWS_VISIBLE; i++) {
            int idx = top + i;
            if (idx >= count) {
                break;
            }
            app->rows[i] = mk_list_row(app->root, i);
            row_set(app->rows[i], paper_chapter_name(app->pack, idx), idx == app->sel);
        }
        break;
    }
    case GAL_PAGE_ABOUT:
        clear_root(app);
        mk_label(app->root, "关于", 12, 4, SCR_W - 24);
        mk_label(app->root,
                 "《纸上的魔法使》横屏阅读器\n"
                 "移植自 paper-rw5（原作为 KAG 脚本）\n"
                 "保留全部章节、分支、结局、CG 与立绘\n"
                 "按键：UP 推进/长按快进，DOWN 历史，OK 菜单\n"
                 "列表：UP/DOWN 选择，OK 确认，长按 OK 返回\n"
                 "本固件仅供个人学习交流",
                 12, 34, SCR_W - 24);
        break;
    case GAL_PAGE_CHOICE: {
        clear_root(app);
        make_art_image(app);
        mk_label(app->root, "请选择", 12, 6, 200);
        int count = app->choice_count;
        int top = app->choice_sel - GAL_ROWS_VISIBLE / 2;
        if (top < 0) {
            top = 0;
        }
        if (top > count - GAL_ROWS_VISIBLE) {
            top = count - GAL_ROWS_VISIBLE;
        }
        if (top < 0) {
            top = 0;
        }
        app->top = top;
        for (int i = 0; i < GAL_ROWS_VISIBLE && top + i < count; i++) {
            const paper_choice_item_t *it =
                paper_choice_item(app->pack, app->choice_index, top + i);
            const char *label = it ? paper_text_get(app->pack, it->text_off, it->len) : "?";
            app->rows[i] = mk_list_row(app->root, i);
            row_set(app->rows[i], label ? label : "?", (top + i) == app->choice_sel);
        }
        break;
    }
    case GAL_PAGE_READER: {
        clear_root(app);
        make_art_image(app);
        app->box = lv_obj_create(app->root);
        lv_obj_set_size(app->box, SCR_W, BOX_H);
        lv_obj_set_pos(app->box, 0, BOX_Y);
        set_bg(app->box, 0x0B1220, 180);   // 0xB4
        app->lbl_name = mk_label(app->root, paper_speaker_name(app->pack, app->speaker), 8,
                                 BOX_Y - 21, 0);
        // 名字底条只包文字（内容宽 + 左右 6px）。
        lv_obj_set_width(app->lbl_name, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(app->lbl_name, 6, 0);
        lv_obj_set_style_bg_color(app->lbl_name, lv_color_hex(0x0B1220), 0);
        lv_obj_set_style_bg_opa(app->lbl_name, 150, 0);
        // 页码挪到面板外右上，正文才能贴面板顶 +4。
        app->lbl_page = mk_label(app->root, "", 262, BOX_Y - 18, 50);
        lv_obj_set_style_text_align(app->lbl_page, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(app->lbl_page, lv_color_hex(0x7C8FA6), 0);
        app->lbl_text = mk_label(app->root, "", TEXT_X, BOX_Y + 4, TEXT_W);
        app->lbl_hint = mk_label(app->root, "", SCR_W - 92, 2, 88);
        lv_obj_set_style_text_align(app->lbl_hint, LV_TEXT_ALIGN_RIGHT, 0);
        draw_scene(app, app->cur_bg, app->cur_sprites);
        reader_refresh_text(app);
        break;
    }
    default:
        break;
    }

    // 正文页以外的页面没有 lbl_hint：没有这一行，notice（无自动进度/已保存/空槽/未解锁）
    // 写进去也永远不可见，用户会以为按键没反应。
    if (app->page != GAL_PAGE_READER && app->lbl_notice == NULL) {
        app->lbl_notice = mk_label(app->root, app->notice_ms ? app->notice : "", 12,
                                   SCR_H - 22, SCR_W - 24);
    }
}

// ---------------------------------------------------------------- 输入
static void open_cg(gal_app_t *app, int group, int member)
{
    int n = paper_cg_member_count(app->pack, group);
    if (n <= 0) {
        return;
    }
    if (member < 0) {
        member = 0;
    }
    if (member >= n) {
        member = n - 1;
    }
    app->cg_group = group;
    app->cg_member = member;
    app->page = GAL_PAGE_CG;
    clear_root(app);
    uint16_t img = paper_cg_member(app->pack, group, member);
    bool ok = paper_gfx_draw_frame(app->pack, img);
    if (ok) {
        paper_game_unlock_image(&app->game, img);
    }
    // CG 铺满整屏：**一个** lv_image，对象 320x240，内容按 cover 放大 1.6x
    // （画布 320x150 → 512x240，居中裁左右），零新增大块、无分带。
    app->img_art = make_cg_image(app);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s  %d/%d", paper_cg_name(app->pack, group), member + 1, n);
    app->header = mk_label(app->root, buf, 8, 2, SCR_W - 16);
    lv_obj_set_style_bg_color(app->header, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(app->header, LV_OPA_50, 0);
}

static void start_new_game(gal_app_t *app)
{
    paper_game_new(&app->game);
    app->auto_scene = 0xFFFF;
    load_step(app);
}

void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    app->idle_ms = 0;
    app->auto_accum_ms = 0;
    if (app->page != GAL_PAGE_READER) {
        app->fast_forward = false;
    }
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    const int up = key->btn == BSP_BTN_UP;
    const int down = key->btn == BSP_BTN_DOWN;
    const int ok = key->btn == BSP_BTN_OK;
    const int click = key->ev == BSP_BTN_CLICK;
    const int longp = key->ev == BSP_BTN_LONG;

    // 松手：停止快进；并清掉"长按后可能补发的 CLICK"
    if (key->ev == BSP_BTN_RELEASE) {
        if (key->btn == BSP_BTN_UP) {
            app->fast_forward = false;
            app->ff_accum_ms = 0;
        }
        app->suppress_click = false;   // 任何键松手都解除"吞 CLICK"状态
        bsp_lvgl_unlock();
        return;
    }
    // 长按之后驱动可能再发一次 CLICK：吞掉它，避免"长按返回"被当成确认
    if (click && app->suppress_click) {
        app->suppress_click = false;
        bsp_lvgl_unlock();
        return;
    }
    if (longp) {
        app->suppress_click = true;
    }
    if (!up && app->fast_forward) {
        app->fast_forward = false;   // 其它键一律取消快进
        app->ff_accum_ms = 0;
    }

    switch (app->page) {
    case GAL_PAGE_TITLE:
        if (up && click && app->sel > 0) {
            app->sel--;
            rebuild_current_page(app);
        } else if (down && click && app->sel + 1 < s_title_menu_n) {
            app->sel++;
            rebuild_current_page(app);
        } else if (ok && click) {
            switch (app->sel) {
            case 0:
                start_new_game(app);
                break;
            case 1:
                if (paper_game_load(&app->game, PAPER_SLOT_AUTO)) {
                    load_step(app);
                } else {
                    snprintf(app->notice, sizeof(app->notice), "无自动进度");
                    app->notice_ms = 2000;
                    rebuild_current_page(app);
                }
                break;
            case 2:
                app->sel = 0;
                goto_page(app, GAL_PAGE_LOAD);
                break;
            case 3:
                app->sel = 0;
                goto_page(app, GAL_PAGE_GALLERY);
                break;
            case 4:
                app->sel = 0;
                goto_page(app, GAL_PAGE_CHAPTER);
                break;
            case 5:
                app->sel = 0;
                goto_page(app, GAL_PAGE_SETTINGS);
                break;
            case 6:
                goto_page(app, GAL_PAGE_ABOUT);
                break;
            default:
                break;
            }
        }
        break;

    case GAL_PAGE_READER:
        if (ok && longp) {
            // 长 OK = 菜单（长按后的补发 CLICK 已在上面被吞掉）
            app->menu_sel = 0;
            app->menu_origin = app->page;
            app->menu_depth = app->page_depth;
            goto_page(app, GAL_PAGE_MENU);
            break;
        }
        if (ok && click) {
            // 短 OK = 下一句（打字中先全显）
            app->fast_forward = false;
            if (app->typing) {
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                reader_refresh_text(app);
            } else if (app->page_index + 1 < app->page_count) {
                app->page_index++;
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                reader_refresh_text(app);
            } else {
                load_step(app);
            }
            break;
        }
        if (up) {
            if (longp) {
                app->fast_forward = true;
                app->ff_accum_ms = 0;
                // 快进一开就把当前页打满，不再逐字。
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                reader_refresh_text(app);
                break;
            }
            if (click) {
                app->fast_forward = false;
                if (app->typing) {
                    app->typed = s_page_len[app->page_index];
                    app->typing = false;
                    reader_refresh_text(app);
                } else if (app->page_index + 1 < app->page_count) {
                    app->page_index++;
                    app->typed = s_page_len[app->page_index];
                    app->typing = false;
                    reader_refresh_text(app);
                } else {
                    load_step(app);
                }
            }
            break;
        }
        if (down && longp) {
            // 长按 DOWN：开/关自动阅读（自动期间 idle=0，不熄屏不深睡）。
            app->auto_mode = !app->auto_mode;
            app->auto_accum_ms = 0;
            app->fast_forward = false;
            if (app->auto_mode) {
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                reader_refresh_text(app);
            }
            break;
        }
        if (down && click) {
            app->sel = 0;
            goto_page(app, GAL_PAGE_HISTORY);
        }
        break;

    case GAL_PAGE_CHOICE:
        if (ok && longp) {
            // 长按 OK：开菜单（保存/读档/设置）；短按 OK 必须确认当前选项
            app->menu_sel = 0;
            app->menu_origin = app->page;
            app->menu_depth = app->page_depth;
            goto_page(app, GAL_PAGE_MENU);
            break;
        }
        if (up && click && app->choice_sel > 0) {
            app->choice_sel--;
            rebuild_current_page(app);
        } else if (down && click && app->choice_sel + 1 < app->choice_count) {
            app->choice_sel++;
            rebuild_current_page(app);
        } else if (ok && click) {
            if (paper_game_choose(&app->game, app->choice_sel)) {
                app->choice_sel = 0;
                load_step(app);
            }
        }
        break;

    case GAL_PAGE_MENU:
        if (up && click && app->menu_sel > 0) {
            app->menu_sel--;
            rebuild_current_page(app);
        } else if (down && click && app->menu_sel + 1 < s_game_menu_n) {
            app->menu_sel++;
            rebuild_current_page(app);
        } else if (ok && longp) {
            app->page = app->menu_origin;
            app->page_depth = app->menu_depth;
            rebuild_current_page(app);
        } else if (ok && click) {
            switch (app->menu_sel) {
            case 0:
                app->page = app->menu_origin;
                app->page_depth = app->menu_depth;
                rebuild_current_page(app);
                break;
            case 1:
                app->sel = 0;
                goto_page(app, GAL_PAGE_SAVE);
                break;
            case 2:
                app->sel = 0;
                goto_page(app, GAL_PAGE_LOAD);
                break;
            case 3:
                app->sel = 0;
                goto_page(app, GAL_PAGE_HISTORY);
                break;
            case 4:
                app->sel = 0;
                goto_page(app, GAL_PAGE_SETTINGS);
                break;
            case 5:
                app->sel = 0;
                goto_page(app, GAL_PAGE_GALLERY);
                break;
            case 6:
                app->sel = 0;
                goto_page(app, GAL_PAGE_CHAPTER);
                break;
            case 7:
                gal_app_flush(app);
                app->page = GAL_PAGE_TITLE;
                app->page_depth = 0;
                app->sel = 0;
                rebuild_current_page(app);
                break;
            default:
                break;
            }
        }
        break;

    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD: {
        const int is_load = app->page == GAL_PAGE_LOAD;
        int count = PAPER_SLOTS;
        if (up && click && app->sel > 0) {
            app->sel--;
            rebuild_current_page(app);
        } else if (down && click && app->sel + 1 < count) {
            app->sel++;
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        } else if (ok && click) {
            if (is_load) {
                if (paper_game_load(&app->game, app->sel)) {
                    load_step(app);
                } else {
                    snprintf(app->notice, sizeof(app->notice), "该槽位没有存档");
                    app->notice_ms = 2000;
                }
            } else {
                if (paper_game_save(&app->game, app->sel)) {
                    snprintf(app->notice, sizeof(app->notice), "已保存");
                } else {
                    snprintf(app->notice, sizeof(app->notice), "保存失败");
                }
                app->notice_ms = 2000;
                rebuild_current_page(app);
            }
        }
        break;
    }

    case GAL_PAGE_SETTINGS:
        if (up && click && app->sel > 0) {
            app->sel--;
        } else if (down && click && app->sel + 1 < 3) {
            app->sel++;
        } else if (ok && click) {
            if (app->sel == 0) {
                app->speed = (gal_speed_t)((app->speed + 1) % GAL_SPEED_COUNT);
            } else if (app->sel == 1) {
                app->auto_mode = !app->auto_mode;
            } else if (app->sel == 2) {
                go_back(app);
                break;
            }
        } else if (ok && longp) {
            go_back(app);
            break;
        }
        rebuild_current_page(app);
        break;

    case GAL_PAGE_HISTORY: {
        int n = paper_game_history_count(&app->game);
        if (up && click && app->sel > 0) {
            app->sel--;
            rebuild_current_page(app);
        } else if (down && click && app->sel + 1 < n) {
            app->sel++;
            rebuild_current_page(app);
        } else if ((ok && longp) || (ok && click) || (down && longp)) {
            go_back(app);
        }
        break;
    }

    case GAL_PAGE_GALLERY:
        if (up && click && app->sel > 0) {
            app->sel--;
            rebuild_current_page(app);
        } else if (down && click && app->sel + 1 < (int)app->pack->cg_count) {
            app->sel++;
            rebuild_current_page(app);
        } else if (ok && longp) {
            app->sel = 0;
            go_back(app);
        } else if (ok && click) {
            if (paper_game_cg_unlocked(&app->game, app->sel)) {
                open_cg(app, app->sel, 0);
            } else {
                snprintf(app->notice, sizeof(app->notice), "尚未解锁");
                app->notice_ms = 1500;
            }
        }
        break;

    case GAL_PAGE_CG:
        if (up && click) {
            open_cg(app, app->cg_group, app->cg_member - 1);
        } else if (down && click) {
            open_cg(app, app->cg_group, app->cg_member + 1);
        } else if (ok && (click || longp)) {
            app->page = GAL_PAGE_GALLERY;
            rebuild_current_page(app);
        }
        break;

    case GAL_PAGE_CHAPTER:
        if (up && click && app->sel > 0) {
            app->sel--;
            rebuild_current_page(app);
        } else if (down && click && app->sel + 1 < (int)app->pack->chapter_count) {
            app->sel++;
            rebuild_current_page(app);
        } else if (ok && longp) {
            app->sel = 0;
            go_back(app);
        } else if (ok && click) {
            uint16_t scene = paper_chapter_scene(app->pack, app->sel);
            if (paper_game_start(&app->game, scene)) {
                load_step(app);
            }
        }
        break;

    case GAL_PAGE_ENDING:
        if (ok && (click || longp)) {
            app->page = GAL_PAGE_TITLE;
            app->sel = 0;
            rebuild_current_page(app);
        }
        break;

    case GAL_PAGE_ABOUT:
        if (ok && (click || longp)) {
            app->sel = 0;
            go_back(app);
        }
        break;

    default:
        break;
    }

    // 提示条
    if (app->notice_ms) {
        if (app->lbl_hint) lv_label_set_text(app->lbl_hint, app->notice);
        else if (app->lbl_notice) lv_label_set_text(app->lbl_notice, app->notice);
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- tick
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms)
{
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    // 解锁进度落盘失败时按 2 秒退避重试（不在每个 tick 刷 NVS/日志）
    if (app->game.meta_dirty) {
        app->meta_retry_ms += elapsed_ms;
        if (app->meta_retry_ms >= 2000) {
            app->meta_retry_ms = 0;
            paper_game_flush_meta(&app->game);
        }
    } else {
        app->meta_retry_ms = 0;
    }
    if (app->notice_ms) {
        const uint32_t notice_before = app->notice_ms;
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        // 提示到期时把常驻提示行清空：列表页不重建也不会留一行陈旧的“已保存”。
        if (notice_before != 0 && app->notice_ms == 0 && app->lbl_notice && bsp_lvgl_lock(0)) {
            lv_label_set_text(app->lbl_notice, "");
            bsp_lvgl_unlock();
        }
    }
    if (app->page != GAL_PAGE_READER) {
        return;
    }
    if (!bsp_lvgl_lock(50)) {
        return;
    }
    if (app->fast_forward && app->typing) {
        app->typed = s_page_len[app->page_index];
        app->typing = false;
        reader_refresh_text(app);
    }
    if (app->typing) {
        uint32_t step = SPEED_MIN_MS(app->speed);
        int budget = (int)(elapsed_ms / (step ? step : 1));
        if (budget <= 0) {
            budget = 1;
        }
        int n = s_page_len[app->page_index];
        int t = utf8_advance(app->text + s_page_start[app->page_index], app->typed,
                             budget < 1 ? 1 : budget);
        app->typed = t;
        if (app->typed >= n) {
            app->typed = n;
            app->typing = false;
            app->auto_accum_ms = 0;
        }
        reader_refresh_text(app);
    } else if (app->fast_forward) {
        app->ff_accum_ms += elapsed_ms;
        if (app->ff_accum_ms >= FF_STEP_MS) {   // FF_STEP_MS=0：每 tick 都推进
            app->ff_accum_ms = 0;
            // 一 tick 内把当前句剩下的页全部翻完（最多 4 页），翻完立刻取下一句；
            // 以前是"一 tick 一页/一句"，看着就像每句之间卡一下。
            int stepped = 0;
            while (stepped < 4 && app->page_index + 1 < app->page_count) {
                app->page_index++;
                stepped++;
            }
            if (stepped > 0) {
                app->typed = s_page_len[app->page_index];
                reader_refresh_text(app);
            } else {
                load_step(app);
                if (app->typing) {
                    app->typed = s_page_len[app->page_index];
                    app->typing = false;
                    reader_refresh_text(app);
                }
            }
        }
    } else if (app->auto_mode) {
        app->auto_accum_ms += elapsed_ms;
        if (app->auto_accum_ms >= AUTO_WAIT_MS) {
            app->auto_accum_ms = 0;
            if (app->page_index + 1 < app->page_count) {
                app->page_index++;
                app->typed = s_page_len[app->page_index];
                reader_refresh_text(app);
            } else {
                load_step(app);
            }
        }
    }
    // 提示文字
    if (app->lbl_hint) {
        const char *hint = "";
        if (app->notice_ms) {
            hint = app->notice;
        } else if (app->auto_mode) {
            hint = "自动";
        } else if (app->fast_forward) {
            hint = "快进";
        }
        if (app->draw_error) {
            hint = "素材读取失败";
        }
        lv_label_set_text(app->lbl_hint, hint);
    }
    bsp_lvgl_unlock();
}

uint32_t gal_app_idle_ms(const gal_app_t *app)
{
    // 自动/快进期间不得调暗/熄屏/深睡。
    if (app && (app->auto_mode || app->fast_forward)) return 0;
    return app ? app->idle_ms : 0;
}

void gal_app_flush(gal_app_t *app)
{
    if (app->page == GAL_PAGE_READER || app->page == GAL_PAGE_CHOICE) {
        if (!paper_game_save(&app->game, PAPER_SLOT_AUTO)) {
            ESP_LOGW("paper_app", "自动存档写入失败（NVS 不可用？）");
        }
    }
    paper_game_flush_meta(&app->game);
}

void gal_app_show_sleeping(gal_app_t *app)
{
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    clear_root(app);
    mk_label(app->root, "休眠中…按任意键唤醒", 20, 110, SCR_W - 40);
    bsp_lvgl_unlock();
    app->sleeping = true;
}

#ifdef PAPER_UI_TEST_HOOKS
void gal_app_test_reload_line(gal_app_t *app)
{
    app->page = GAL_PAGE_READER;
    app->typing = true;
    app->typed = 0;
    reader_refresh_text(app);
}

bool gal_app_test_draw_frame(gal_app_t *app, uint16_t image_index)
{
    uint16_t sprites[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    draw_scene(app, image_index, sprites);
    return !app->draw_error;
}

bool gal_app_test_draw_sprites(gal_app_t *app, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t sprites[3] = {a, b, c};
    draw_scene(app, 0xFFFF, sprites);
    return !app->draw_error;
}

bool gal_app_test_goto_choice_nth(gal_app_t *app, int nth)
{
    (void)paper_game_new(&app->game);
    int seen = 0;
    for (int i = 0; i < 60000; i++) {
        load_step(app);
        if (app->page == GAL_PAGE_CHOICE) {
            if (seen == nth) {
                return true;
            }
            if (!paper_game_choose(&app->game, 0)) {
                return false;
            }
            seen++;
            continue;
        }
        if (app->game.ended || app->page == GAL_PAGE_ENDING) {
            return false;
        }
    }
    return false;
}

bool gal_app_test_goto_choice(gal_app_t *app)
{
    (void)paper_game_new(&app->game);
    for (int i = 0; i < 30000; i++) {
        load_step(app);
        if (app->page == GAL_PAGE_CHOICE) {
            return true;
        }
        if (app->game.ended) {
            return false;
        }
    }
    return false;
}

void gal_app_test_goto_title(gal_app_t *app)
{
    app->page = GAL_PAGE_TITLE;
    app->sel = 0;
    s_title_bg_drawn = false;
    rebuild_current_page(app);
}
#endif

// ---------------------------------------------------------------- init
bool gal_app_init(gal_app_t *app)
{
    memset(app, 0, sizeof(*app));
    app->pack = &app->game.pack;
    app->speed = GAL_SPEED_NORMAL;
    if (!paper_game_init(&app->game)) {
        ESP_LOGE(TAG, "资源包初始化失败");
        return false;
    }
    paper_game_load_meta(&app->game);

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 锁超时");
        return false;
    }
    bsp_lvgl_set_landscape(true);
    bsp_lvgl_unlock();

    if (!paper_gfx_init()) {
        return false;
    }
    if (!bsp_lvgl_lock(1000)) {
        return false;
    }
    app->page = GAL_PAGE_TITLE;
    rebuild_current_page(app);
    bsp_lvgl_unlock();

    size_t canvas = 0, scratch = 0;
    paper_gfx_stats(&canvas, &scratch);
    ESP_LOGI(TAG, "就绪: 场景 %u / 图 %u / 章节 %u / CG 组 %u | 画布 %u B, 工作区 %u B, 空闲堆 %u B",
             (unsigned)app->pack->scene_count, (unsigned)app->pack->entry_count,
             (unsigned)app->pack->chapter_count, (unsigned)app->pack->cg_count,
             (unsigned)canvas, (unsigned)scratch,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    return true;
}
