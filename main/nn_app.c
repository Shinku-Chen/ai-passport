// main/nn_app.c —— 阅读器状态机实现。
#include "nn_app.h"

#include "nn_canvas.h"
#include "nn_save_codec.h"

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "gal_text.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "nn_app";

// 状态机里的按键/事件编号必须与 BSP 完全一致；不一致时编译期就失败。
_Static_assert((int)NN_BTN_UP == (int)BSP_BTN_UP, "button ids must mirror bsp_btn_t");
_Static_assert((int)NN_BTN_DOWN == (int)BSP_BTN_DOWN, "button ids must mirror bsp_btn_t");
_Static_assert((int)NN_BTN_OK == (int)BSP_BTN_OK, "button ids must mirror bsp_btn_t");
_Static_assert((int)NN_EV_PRESS == (int)BSP_BTN_PRESS, "event ids must mirror bsp_btn_ev_t");
_Static_assert((int)NN_EV_CLICK == (int)BSP_BTN_CLICK, "event ids must mirror bsp_btn_ev_t");
_Static_assert((int)NN_EV_LONG == (int)BSP_BTN_LONG, "event ids must mirror bsp_btn_ev_t");
_Static_assert((int)NN_EV_RELEASE == (int)BSP_BTN_RELEASE, "event ids must mirror bsp_btn_ev_t");

// 版面：横屏 320x240，对话框 90px。
//   16px 档：18 汉字/行 x 4 行 = 36 半角单位/行
//   20px 档：14 汉字/行 x 3 行 = 28 半角单位/行
//
// 为什么不是 19/15：正文区宽 304px，16px 汉字正好 16px 宽，19 字刚好占满；
// 分页器允许行首禁则标点“挂在行尾”，那一行会变成 20 字 = 320px，LVGL 会再折一行，
// 第 5 行就被裁掉。留一个字的位置给悬挂标点，既不会多翻页（最长一句 138 单位 < 144），
// 也不会出现被裁掉的正文。
// 竖屏 240x320：正文宽 224px。16px 汉字 16px 宽 → 14 字 = 224；留 1 字给悬挂标点。
#define NN_UNITS_16 28
#define NN_LINES_16 4
#define NN_UNITS_20 22
#define NN_LINES_20 3

#define TYPE_SPEED_MS 40      // 每字 40ms（“中”速），0 表示瞬间
#define AUTO_DELAY_MS 900
#define FF_STEP_MS 0        // 0 = 每个 tick 立即推进，不再有 70ms 间隔

// 列表项
enum { TITLE_NEW = 0, TITLE_CONTINUE, TITLE_LOAD, TITLE_SETTINGS, TITLE_GALLERY, TITLE_ABOUT };
enum { MENU_RESUME = 0, MENU_SAVE, MENU_LOAD, MENU_HISTORY, MENU_SETTINGS, MENU_TITLE };
enum { SET_SPEED = 0, SET_FONT, SET_AUTO, SET_OPA, SET_CLEAR, SET_ABOUT, SET_COUNT };

// 前向声明：画面/鉴赏相关的小工具在下面定义，先声明避免隐式声明（-Werror）。
static void snapshot(nn_app_t *app, nn_save_t *out);
static int cg_index_of_image(const nn_app_t *app, uint16_t image_id);
static uint16_t title_image_id(const nn_app_t *app);
static int cg_count(const nn_app_t *app);
static uint16_t cg_image_id(const nn_app_t *app, int index);
static uint16_t cg_thumb_id(const nn_app_t *app, int index);

static int units_per_line(const nn_app_t *app)
{
    return app->settings.font_large ? NN_UNITS_20 : NN_UNITS_16;
}

static int lines_per_page(const nn_app_t *app)
{
    return app->settings.font_large ? NN_LINES_20 : NN_LINES_16;
}

static const char *scene_name(const nn_app_t *app, uint16_t scene, uint16_t *len)
{
    return nn_pack_scene_name(app->pack, scene, len);
}

static void update_header(nn_app_t *app)
{
    uint16_t len = 0;
    const char *name = scene_name(app, app->rt.scene, &len);
    if (app->rt.view.chapter) {
        snprintf(app->header_buf, sizeof(app->header_buf), "%.*s",
                 (int)app->rt.view.chapter_len, app->rt.view.chapter);
    } else if (name) {
        // "chunk003" -> "第 3 幕"
        int n = 0;
        if (sscanf(name, "chunk%d", &n) == 1) {
            snprintf(app->header_buf, sizeof(app->header_buf), "第 %d 幕 · %d/%d",
                     n, (int)app->rt.scene + 1, (int)app->pack->scene_count);
        } else {
            snprintf(app->header_buf, sizeof(app->header_buf), "%.*s", (int)len, name);
        }
    }
    nn_ui_set_header(&app->ui, app->header_buf);
}

// ---------------------------------------------------------------- 分带绘制
// 依次把 0..239 画满：每条带用同一块 320xh 缓冲，画完立刻 invalidate+flush。
//   title/CG/背景：kind 4/3/0；sprite：kind 1（叠加在背景上）
static void paint_bands_cg(nn_app_t *app, uint16_t image_id, uint16_t sprite_id,
                           bool cg_full)
{
    const int bands = nn_canvas_band_count();
    for (int b = 0; b < bands; ++b) {
        nn_canvas_black_band(b);
        if (image_id != NN_NONE16) {
            (void)nn_canvas_draw_image_band(app->pack, image_id, b, cg_full);
        }
        if (sprite_id != NN_NONE16) {
            (void)nn_canvas_draw_image_band(app->pack, sprite_id, b, false);
        }
        nn_ui_refresh_band(&app->ui, b);
    }
    // 三带都填完再统一刷一次（独立缓冲，不需要逐带等待）。
    lv_refr_now(lv_display_get_default());
}

static void paint_bands(nn_app_t *app, uint16_t image_id, uint16_t sprite_id)
{
    paint_bands_cg(app, image_id, sprite_id, false);   // 游戏内：CG cover + 顶部对齐
}

// ---------------------------------------------------------------- 画面
// 上一次真正画进分带的 (cg,bg,sprite)。三带现在是独立缓冲、内容会保留，
// 所以画面没变就不必重画 —— 快进时每句都重解码背景是"每句停一下"的主因。
static uint16_t s_painted_cg, s_painted_bg, s_painted_sprite;
static bool s_painted_valid;

void nn_app_invalidate_view(void)
{
    s_painted_valid = false;
}

static void repaint_view(nn_app_t *app)
{
    // CG 优先于背景；都没有就是黑场。分带：0..79 / 80..159 / 160..239。
    const uint16_t bg = app->rt.view.cg != NN_NONE16
                            ? app->rt.view.cg
                            : app->rt.view.bg;
    if (app->rt.view.cg != NN_NONE16) {
        const int cgi = cg_index_of_image(app, app->rt.view.cg);
        if (cgi >= 0) nn_unlock_cg((uint8_t)cgi);
    }
    // 画面没变就不重画：每帧 6 带 x 一次 JPEG 解码，快进时会明显一顿一顿。
    // （之前怀疑的"右侧竖线"已确认是 root 滚动条，与本缓存无关。）
    if (s_painted_valid && s_painted_bg == app->rt.view.bg &&
        s_painted_cg == app->rt.view.cg && s_painted_sprite == app->rt.view.sprite) {
        return;
    }
    // 快进节流：重画一帧要解 6 带 JPEG（实机 ~150ms），而快进每 50ms 就换一句，
    // 不节流就会一直卡在解码里（"快进一卡一卡"）。快进期间最多 200ms 刷一次画面，
    // 文字照常每 tick 前进；松手后下一次 tick 会立刻补画最终画面。
    if (app->ff_hold && app->page == NN_PAGE_GAME) {
        static uint32_t s_last_paint;
        const uint32_t now = lv_tick_get();
        if (s_last_paint && (now - s_last_paint) < 200u) {
            return;
        }
        s_last_paint = now ? now : 1u;
    }
    paint_bands(app, bg, app->rt.view.sprite);
    s_painted_bg = app->rt.view.bg;
    s_painted_cg = app->rt.view.cg;
    s_painted_sprite = app->rt.view.sprite;
    s_painted_valid = true;
}

// CG 与缩略图在包里各自成表、顺序一致（tools/nn_pack.py 先 CG 后缩略图），
// 所以“图像 id -> 鉴赏位”可以用 kind 精确换算，不靠尺寸或命名猜测。
static int cg_index_of_image(const nn_app_t *app, uint16_t image_id)
{
    int idx = 0;
    for (uint32_t i = 0; i < app->pack->image_count; ++i) {
        if (app->pack->images[i].kind != NN_IMG_CG) continue;
        if (i == image_id) return idx;
        idx++;
    }
    return -1;
}

static void history_push(nn_app_t *app, const char *text, uint16_t speaker)
{
    app->hist_text[app->hist_head] = text;
    app->hist_speaker[app->hist_head] = speaker;
    app->hist_head = (app->hist_head + 1) % NN_HISTORY_MAX;
    if (app->hist_count < NN_HISTORY_MAX) app->hist_count++;
}

static void paginate_line(nn_app_t *app)
{
    app->page_count = gal_text_pages(app->line_text, units_per_line(app),
                                     lines_per_page(app), app->page_off,
                                     GAL_TEXT_MAX_PAGES + 1);
    if (app->page_count > GAL_TEXT_MAX_PAGES) app->page_count = GAL_TEXT_MAX_PAGES;
    app->page_index = 0;
}

static uint16_t page_len(const nn_app_t *app)
{
    const uint32_t start = app->page_off[app->page_index];
    uint32_t end = app->page_off[app->page_index + 1];
    if (end > app->line_len) end = app->line_len;
    if (end < start) end = start;
    return (uint16_t)(end - start);
}

static const char *page_text(const nn_app_t *app)
{
    return app->line_text + app->page_off[app->page_index];
}

// 把当前页复制成 NUL 结尾的临时串：包里的字符串是整句（句末才有 NUL），
// 多页长句如果直接把 page_text() 交给 LVGL，第二页会把后面所有页一起显示。
static char s_page_buf[256];
static const char *page_text_clipped(nn_app_t *app, uint16_t *out_len)
{
    uint16_t n = page_len(app);
    if (n >= sizeof(s_page_buf)) n = sizeof(s_page_buf) - 1;
    memcpy(s_page_buf, page_text(app), n);
    s_page_buf[n] = 0;
    if (out_len) *out_len = n;
    return s_page_buf;
}

// 打字机按“字符”推进：把前 chars 个字符换算成字节数（UTF-8 边界对齐）。
static int bytes_for_chars(const char *s, int chars)
{
    int count = 0;
    int bytes = 0;
    while (s[bytes] && count < chars) {
        const unsigned char c = (unsigned char)s[bytes];
        int step = 1;
        if ((c & 0xE0u) == 0xC0u) step = 2;
        else if ((c & 0xF0u) == 0xE0u) step = 3;
        else if ((c & 0xF8u) == 0xF0u) step = 4;
        for (int i = 1; i < step; ++i) {
            if (!s[bytes + i]) return bytes;
        }
        bytes += step;
        count++;
    }
    return bytes;
}

// 一页有多少字符（打字机进度按字符算，不按字节算）。
static int chars_in_page(nn_app_t *app)
{
    uint16_t len = 0;
    (void)page_text_clipped(app, &len);
    int count = 0;
    for (int i = 0; i < len; ) {
        const unsigned char c = (unsigned char)s_page_buf[i];
        int step = 1;
        if ((c & 0xE0u) == 0xC0u) step = 2;
        else if ((c & 0xF0u) == 0xE0u) step = 3;
        else if ((c & 0xF8u) == 0xF0u) step = 4;
        i += step;
        count++;
    }
    return count;
}

static void render_line(nn_app_t *app)
{
    uint16_t slen = 0;
    const char *spk = nn_pack_speaker(app->pack, app->line_speaker, &slen);
    char speaker[32];
    speaker[0] = '\0';
    if (spk && slen) {
        const size_t n = slen < sizeof(speaker) - 1 ? slen : sizeof(speaker) - 1;
        memcpy(speaker, spk, n);
        speaker[n] = '\0';
    }
    // 只显示当前页（包里的字符串是整句，句末才有 NUL）：多页长句如果直接把
    // page_text() 交给 LVGL，第二页会把后面所有页一起显示。
    const char *page = page_text_clipped(app, NULL);
    if (app->typing) {
        nn_ui_set_dialogue(&app->ui, speaker, page,
                           bytes_for_chars(page, app->shown_chars) + 1);
    } else {
        nn_ui_set_dialogue(&app->ui, speaker, page, 0);
    }
    nn_ui_set_page(&app->ui, app->page_index, app->page_count);
    nn_ui_set_hint(&app->ui, "OK 菜单");
    update_header(app);
}

static void show_game(nn_app_t *app)
{
    app->page = NN_PAGE_GAME;
    nn_ui_show(&app->ui, NN_PAGE_GAME);
    render_line(app);
}

static uint16_t s_last_auto_scene = 0xFFFF;

static void on_say(nn_app_t *app)
{
    app->line_text = app->rt.text;
    app->line_len = app->rt.text_len;
    app->line_speaker = app->rt.speaker;
    app->line_scene = app->rt.scene;
    app->line_op = (uint16_t)(app->rt.op > 0 ? app->rt.op - 1 : 0);
    paginate_line(app);
    app->typing = true;
    app->shown_chars = 0;
    app->type_accum_ms = 0;
    app->auto_wait = false;
    app->auto_accum_ms = 0;
    history_push(app, app->line_text, app->line_speaker);
    repaint_view(app);
    // 换场景时自动存一次档：断电/休眠后“继续阅读”能从这一场接上。
    if (app->line_scene != s_last_auto_scene) {
        s_last_auto_scene = app->line_scene;
        nn_save_t save;
        snapshot(app, &save);
        (void)nn_save_write(NN_SLOT_AUTO, &save);
    }
    show_game(app);
    if (app->settings.text_speed >= 3) {
        app->typing = false;
        app->shown_chars = 0;
    }
    render_line(app);
}

// 只重画选项行（不动 choice_sel）——上下移动后必须走这个，否则选择变了屏幕不动。
static void choice_repaint(nn_app_t *app)
{
    const char *rows[NN_UI_MAX_ROWS] = {0};
    static char row_buf[NN_UI_MAX_ROWS][64];
    const int n = app->choice_count < NN_UI_MAX_ROWS ? app->choice_count : NN_UI_MAX_ROWS;
    for (int i = 0; i < n; ++i) {
        uint16_t len = 0;
        const char *t = nn_rt_choice_text(&app->rt, (uint8_t)i, &len);
        if (!t) t = "";
        snprintf(row_buf[i], sizeof(row_buf[i]), "%.*s", (int)len, t);
        rows[i] = row_buf[i];
    }
    nn_ui_overlay_compact(&app->ui, true);
    nn_ui_overlay_opa(&app->ui, 92);
    nn_ui_set_overlay(&app->ui, "选择", rows, NULL, n, app->choice_sel);
    nn_ui_set_hint(&app->ui, "上/下 选择 · OK 确认");
}

static void show_choice(nn_app_t *app)
{
    app->page = NN_PAGE_CHOICE;
    app->choice_pending = true;
    app->choice_count = app->rt.choice_count;
    app->choice_sel = 0;
    nn_ui_show(&app->ui, NN_PAGE_CHOICE);
    choice_repaint(app);
}

static void show_ending(nn_app_t *app)
{
    app->page = NN_PAGE_ENDING;
    uint16_t nlen = 0, tlen = 0;
    const char *name = nn_rt_ending_name(app->pack, app->ending_id, &nlen);
    const char *text = nn_rt_ending_text(app->pack, app->ending_id, &tlen);
    static char name_buf[64], text_buf[192];
    snprintf(name_buf, sizeof(name_buf), "%.*s", (int)nlen, name ? name : "");
    snprintf(text_buf, sizeof(text_buf), "%.*s", (int)tlen, text ? text : "");
    nn_unlock_ending(app->ending_id);
    paint_bands(app, NN_NONE16, NN_NONE16);
    s_painted_bg = s_painted_cg = s_painted_sprite = NN_NONE16;
    s_painted_valid = true;
    nn_ui_refresh_art(&app->ui);
    nn_ui_show(&app->ui, NN_PAGE_ENDING);
    nn_ui_set_ending(&app->ui, name_buf, text_buf);
    nn_ui_set_hint(&app->ui, "");
    update_header(app);
}

// 执行到下一个阻塞点并渲染。
static void advance(nn_app_t *app)
{
    const nn_step_t step = nn_rt_run(&app->rt);
    switch (step) {
    case NN_STEP_SAY:
        on_say(app);
        break;
    case NN_STEP_CHOICE:
        // 选项前先把画面状态刷出来
        repaint_view(app);
        show_choice(app);
        break;
    case NN_STEP_END:
        app->ending_id = app->rt.ending_id;
        show_ending(app);
        break;
    case NN_STEP_BROKEN:
    default:
        ESP_LOGE(TAG, "脚本异常:scene %u op %u", (unsigned)app->rt.scene, (unsigned)app->rt.op);
        app->error_shown = true;
        nn_ui_set_dialogue(&app->ui, "", "剧本数据异常，请回到标题页重新开始。", 0);
        nn_ui_set_hint(&app->ui, "OK 菜单");
        break;
    }
}

// ---------------------------------------------------------------- 读档/存档
static void snapshot(nn_app_t *app, nn_save_t *out)
{
    memset(out, 0, sizeof(*out));
    out->scene = app->line_scene;
    out->op = app->line_op;
    out->bg = app->rt.view.bg;
    out->cg = app->rt.view.cg;
    out->sprite = app->rt.view.sprite;
    out->stamp = app->uptime_s;
    out->cg_mask = nn_unlock_cg_mask();
    out->ending_mask = nn_unlock_ending_mask();
    uint16_t len = 0;
    const char *name = scene_name(app, app->line_scene, &len);
    if (name) {
        const size_t n = len < sizeof(out->chapter) - 1 ? len : sizeof(out->chapter) - 1;
        memcpy(out->chapter, name, n);
    }
    const char *src = app->line_text ? app->line_text : "";
    int cut = 27;
    const int total = (int)strlen(src);
    if (cut > total) cut = total;
    while (cut > 0 && (((unsigned char)src[cut]) & 0xC0u) == 0x80u) cut--;
    memcpy(out->preview, src, (size_t)cut);
}

static void apply_restore(nn_app_t *app, const nn_save_t *save)
{
    if (!nn_save_record_fits_pack(save, app->pack)) {
        ESP_LOGW(TAG, "存档与当前剧本包不自洽,忽略本次读档");
        nn_ui_overlay_foot(&app->ui, "该存档与当前版本不兼容");
        return;
    }
    nn_view_t view;
    memset(&view, 0, sizeof(view));
    view.bg = save->bg;
    view.cg = save->cg;
    view.sprite = save->sprite;
    if (!nn_rt_seek(&app->rt, save->scene, save->op, &view)) {
        ESP_LOGW(TAG, "存档位置越界,忽略本次读档");
        return;
    }
    // 解锁状态以当前全局记录为准（存档里的只是快照）
    app->choice_pending = false;
    app->ret_depth = 0;        // 读档回正文 = 回到根页，清掉存档页/菜单留下的来源
    advance(app);
}

static void auto_save(nn_app_t *app)
{
    nn_save_t save;
    snapshot(app, &save);
    if (!nn_save_write(NN_SLOT_AUTO, &save)) {
        ESP_LOGW(TAG, "自动存档写入失败");
    }
}

// ---------------------------------------------------------------- 列表页
static void show_title(nn_app_t *app)
{
    app->page = NN_PAGE_TITLE;
    nn_ui_show(&app->ui, NN_PAGE_TITLE);
    const uint16_t title_id = title_image_id(app);
    paint_bands(app, title_id, NN_NONE16);
    s_painted_valid = false;   // 分带已被标题图占用，回游戏必须重画

    static const char *rows[] = { "开始游戏", "继续阅读", "读取存档", "设置", "鉴赏室", "关于" };
    static char cont_value[24];
    cont_value[0] = '\0';
    nn_save_t auto_save_rec;
    if (nn_save_read(NN_SLOT_AUTO, &auto_save_rec)) {
        snprintf(cont_value, sizeof(cont_value), "%s", auto_save_rec.chapter);
    }
    const char *values[] = { "", cont_value, "", "", "", "" };
    nn_ui_overlay_compact(&app->ui, false);
    nn_ui_overlay_opa(&app->ui, 62);      // 标题页让标题画透出来
    nn_ui_overlay_title_layout(&app->ui, true);   // 6 项菜单让到画面区下方
    nn_ui_set_overlay(&app->ui, "男娘恋爱物语", rows, values, 6, app->title_sel);
    nn_ui_overlay_foot(&app->ui, "上/下 选择 · OK 确认");
    nn_ui_set_hint(&app->ui, "");
}

static void show_menu(nn_app_t *app)
{
    // 只有真正“进入”菜单才把光标归零；同一页重绘（上下移动）必须保留选中项。
    if (app->page != NN_PAGE_MENU) app->menu_sel = 0;
    app->page = NN_PAGE_MENU;
    nn_ui_show(&app->ui, NN_PAGE_MENU);
    static const char *rows[] = { "继续阅读", "保存进度", "读取存档", "历史记录", "设置", "回到标题" };
    nn_ui_overlay_compact(&app->ui, false);
    nn_ui_overlay_opa(&app->ui, 90);
    nn_ui_set_overlay(&app->ui, "菜单", rows, NULL, 6, app->menu_sel);
    nn_ui_overlay_foot(&app->ui, "上/下 选择 · OK 确认 · 长按OK 返回");
}

// 返回栈：进入子页时压当前页，长按 OK 弹栈。栈满时丢最旧的一层，
// 保留最近的来源（避免返回跳到很久以前的页面，也不会静默丢层）。
static void push_return(nn_app_t *app, nn_page_t page)
{
    if (app->ret_depth >= NN_RETURN_STACK_MAX) {
        memmove(&app->ret_stack[0], &app->ret_stack[1],
                (NN_RETURN_STACK_MAX - 1) * sizeof(app->ret_stack[0]));
        app->ret_depth = (uint8_t)(NN_RETURN_STACK_MAX - 1);
    }
    app->ret_stack[app->ret_depth++] = page;
}

// 进入子页时统一记录来源页：返回时弹栈，而不是靠 hist_count 猜。
static void enter_page(nn_app_t *app, nn_page_t page)
{
    push_return(app, app->page);
    app->page = page;
    nn_ui_show(&app->ui, page);
}

// notice：非 NULL 时覆盖页脚（保存成功/失败这类一次性提示必须能看见；
// 之前是先写提示再 show_slots() 覆盖掉，等于没提示）。
static void show_slots(nn_app_t *app, bool for_save, const char *notice)
{
    app->slots_saving = for_save;
    app->page = NN_PAGE_SLOTS;
    nn_ui_show(&app->ui, NN_PAGE_SLOTS);
    static const char *rows[NN_SLOT_COUNT];
    static char row_buf[NN_SLOT_COUNT][40];
    static const char *values[NN_SLOT_COUNT];
    static char val_buf[NN_SLOT_COUNT][48];
    rows[0] = "自动存档";
    for (int i = 0; i < NN_SLOT_MANUAL; ++i) {
        snprintf(row_buf[i + 1], sizeof(row_buf[i + 1]), "存档 %d", i + 1);
        rows[i + 1] = row_buf[i + 1];
    }
    for (int i = 0; i < NN_SLOT_COUNT; ++i) {
        nn_save_t save;
        if (nn_save_read((uint8_t)i, &save)) {
            snprintf(val_buf[i], sizeof(val_buf[i]), "%.8s %.24s", save.chapter, save.preview);
        } else {
            snprintf(val_buf[i], sizeof(val_buf[i]), "-");
        }
        values[i] = val_buf[i];
    }
    nn_ui_overlay_compact(&app->ui, false);
    nn_ui_overlay_opa(&app->ui, 90);
    nn_ui_set_overlay(&app->ui, for_save ? "保存进度" : "读取存档", rows, values,
                      NN_SLOT_COUNT, app->slot_sel);
    if (notice) {
        nn_ui_overlay_foot(&app->ui, notice);
    } else if (!nn_store_available()) {
        nn_ui_overlay_foot(&app->ui, "存档不可用：NVS 未就绪（仍可继续阅读）");
    } else {
        nn_ui_overlay_foot(&app->ui, for_save ? "上/下 选槽 · OK 保存 · 长按OK 返回"
                                              : "上/下 选槽 · OK 读取 · 长按OK 返回");
    }
}

static void show_settings(nn_app_t *app)
{
    app->page = NN_PAGE_SETTINGS;
    nn_ui_show(&app->ui, NN_PAGE_SETTINGS);
    static const char *rows[SET_COUNT] = { "文字速度", "字号", "自动阅读", "文本框", "清除观看记录", "关于" };
    static char v_speed[8], v_font[8], v_auto[8], v_opa[8];
    static const char *values[SET_COUNT];
    static const char *const SPEED[4] = { "慢", "中", "快", "瞬间" };
    snprintf(v_speed, sizeof(v_speed), "%s", SPEED[app->settings.text_speed & 3]);
    snprintf(v_font, sizeof(v_font), "%s", app->settings.font_large ? "20px" : "16px");
    snprintf(v_auto, sizeof(v_auto), "%s", app->settings.auto_play ? "开" : "关");
    snprintf(v_opa, sizeof(v_opa), "%u%%", (unsigned)app->settings.panel_opa);
    values[0] = v_speed;
    values[1] = v_font;
    values[2] = v_auto;
    values[3] = v_opa;
    values[4] = "";
    values[5] = "";
    nn_ui_overlay_compact(&app->ui, false);
    nn_ui_overlay_opa(&app->ui, 90);
    nn_ui_set_overlay(&app->ui, "设置", rows, values, SET_COUNT, app->settings_sel);
    nn_ui_overlay_foot(&app->ui, "上/下 选择 · OK 修改 · 长按OK 返回");
}

static void show_history(nn_app_t *app)
{
    app->page = NN_PAGE_HISTORY;
    nn_ui_show(&app->ui, NN_PAGE_HISTORY);
    // 从最近一句往前显示 6 行
    static char buf[6][96];
    static const char *lines[6];
    int n = 0;
    const int total = app->hist_count;
    int start = total - 6 - app->history_top;
    if (start < 0) start = 0;
    for (int i = 0; i < 6; ++i) {
        const int idx = start + i;
        if (idx >= total) break;
        const int ring = (app->hist_head - total + idx + NN_HISTORY_MAX * 2) % NN_HISTORY_MAX;
        const char *t = app->hist_text[ring];
        uint16_t slen = 0;
        const char *spk = nn_pack_speaker(app->pack, app->hist_speaker[ring], &slen);
        if (spk && slen) {
            snprintf(buf[n], sizeof(buf[n]), "%.*s：%.70s", (int)slen, spk, t ? t : "");
        } else {
            snprintf(buf[n], sizeof(buf[n]), "%.76s", t ? t : "");
        }
        lines[n] = buf[n];
        n++;
    }
    nn_ui_overlay_compact(&app->ui, false);
    nn_ui_overlay_opa(&app->ui, 92);
    nn_ui_set_overlay(&app->ui, "历史记录", lines, NULL, n, -1);
    nn_ui_overlay_foot(&app->ui, "上/下 翻看 · 长按OK 返回");
}


static int cg_count(const nn_app_t *app)
{
    int n = 0;
    for (uint32_t i = 0; i < app->pack->image_count; ++i) {
        const nn_image_t *img = &app->pack->images[i];
        if (img->kind == NN_IMG_THUMB) n++;
    }
    return n;
}

static uint16_t cg_image_id(const nn_app_t *app, int index)
{
    int n = 0;
    for (uint32_t i = 0; i < app->pack->image_count; ++i) {
        if (app->pack->images[i].kind != NN_IMG_CG) continue;
        if (n == index) return (uint16_t)i;
        n++;
    }
    return NN_NONE16;
}

// 标题图（kind 4），没有就用黑场。
static uint16_t title_image_id(const nn_app_t *app)
{
    for (uint32_t i = 0; i < app->pack->image_count; ++i) {
        if (app->pack->images[i].kind == NN_IMG_TITLE) return (uint16_t)i;
    }
    return NN_NONE16;
}

static uint16_t cg_thumb_id(const nn_app_t *app, int index)
{
    int n = 0;
    for (uint32_t i = 0; i < app->pack->image_count; ++i) {
        if (app->pack->images[i].kind != NN_IMG_THUMB) continue;
        if (n == index) return (uint16_t)i;
        n++;
    }
    return NN_NONE16;
}

static void show_gallery(nn_app_t *app)
{
    app->page = NN_PAGE_GALLERY;
    nn_ui_show(&app->ui, NN_PAGE_GALLERY);
    const int total = cg_count(app);
    const int pages = (total + 5) / 6;
    if (app->gallery_page >= pages) app->gallery_page = pages ? pages - 1 : 0;
    const uint16_t mask = nn_unlock_cg_mask();
    const void *thumbs[6] = {0};
    int first = app->gallery_page * 6;
    for (int i = 0; i < 6; ++i) {
        const int idx = first + i;
        if (idx >= total) break;
        const uint16_t id = cg_thumb_id(app, idx);
        const nn_image_t *img = nn_pack_image(app->pack, id);
        if (!img) continue;
        // 未解锁的 CG 只显示占位（不暴露画面）
        thumbs[i] = (mask & (1u << idx)) ? (const void *)img : NULL;
        (void)cg_image_id(app, idx);
    }
    char cap[96];
    snprintf(cap, sizeof(cap), "第 %d/%d 页 · 共 %d 张 · 已解锁 %d", app->gallery_page + 1,
             pages ? pages : 1, total, __builtin_popcount((unsigned)mask));
    nn_ui_set_gallery(&app->ui, thumbs, 6, app->gallery_sel, cap);
}

// 鉴赏室光标：每页 6 张，跨页时整页翻。
static void gallery_move(nn_app_t *app, int delta)
{
    const int total = cg_count(app);
    if (total <= 0) return;
    const int pages = (total + 5) / 6;
    if (delta > 0 && app->gallery_sel >= 5) {
        if (app->gallery_page + 1 < pages) {
            app->gallery_page++;
            app->gallery_sel = 0;
            show_gallery(app);
        }
        return;
    }
    if (delta < 0 && app->gallery_sel <= 0 && app->gallery_page > 0) {
        app->gallery_page--;
        show_gallery(app);
        app->gallery_sel = 5;
        show_gallery(app);
        return;
    }
    int sel = app->gallery_sel + delta;
    if (sel < 0) sel = 0;
    if (sel > 5) sel = 5;
    app->gallery_sel = sel;
    show_gallery(app);
}

// 打开选中的 CG（未解锁只给提示，不暴露画面）。
static void gallery_ok(nn_app_t *app)
{
    const int idx = app->gallery_page * 6 + app->gallery_sel;
    const int total = cg_count(app);
    if (idx >= total) return;
    const uint16_t mask = nn_unlock_cg_mask();
    if ((mask & (1u << idx)) == 0) {
        nn_ui_gallery_caption(&app->ui, "尚未解锁：读到这段剧情后才会收录");
        return;
    }
    const uint16_t id = cg_image_id(app, idx);
    if (id == NN_NONE16) return;
    paint_bands_cg(app, id, NN_NONE16, true);   // CGVIEW：contain，整幅可见
    s_painted_valid = false;   // CG 浏览占用分带
    app->page = NN_PAGE_CGVIEW;
    nn_ui_show(&app->ui, NN_PAGE_CGVIEW);
    nn_ui_set_hint(&app->ui, "OK 返回");
}

// ---------------------------------------------------------------- 初始化
bool nn_app_init(nn_app_t *app, const nn_pack_t *pack)
{
    memset(app, 0, sizeof(*app));
    app->pack = pack;
    nn_settings_load(&app->settings);
    if (!nn_ui_create(&app->ui)) return false;
    nn_ui_set_font_large(&app->ui, app->settings.font_large != 0);
    nn_ui_set_panel_opa(&app->ui, app->settings.panel_opa);
    app->auto_mode = app->settings.auto_play != 0;
    app->battery = -1;

    nn_rt_start(&app->rt, pack);
    // 标题页：先画标题图（kind 4）
    const uint16_t title_id = title_image_id(app);
    paint_bands(app, title_id, NN_NONE16);
    s_painted_valid = false;   // 分带已被标题图占用，回游戏必须重画

    app->title_sel = 0;
    // 有自动存档时默认停在“继续阅读”
    if (nn_save_exists(NN_SLOT_AUTO)) app->title_sel = TITLE_CONTINUE;
    show_title(app);
    ESP_LOGI(TAG, "就绪: %u 场景 / %u 句对白 / %u 结局", (unsigned)pack->scene_count,
             (unsigned)pack->op_count, (unsigned)pack->ending_count);
    return true;
}

// ---------------------------------------------------------------- 按键
static void start_new_game(nn_app_t *app)
{
    nn_rt_start(&app->rt, app->pack);
    s_last_auto_scene = 0xFFFF;
    app->hist_count = 0;
    app->hist_head = 0;
    app->error_shown = false;  // 新游戏必须从“可推进”状态开始，否则错误态永不复位
    app->choice_pending = false;
    app->ret_depth = 0;        // 新游戏 = 根页
    app->auto_mode = app->settings.auto_play != 0;
    advance(app);
}

static void toggle_auto(nn_app_t *app)
{
    app->auto_mode = !app->auto_mode;
    app->settings.auto_play = app->auto_mode ? 1 : 0;
    nn_settings_store(&app->settings);
    nn_ui_set_hint(&app->ui, app->auto_mode ? "自动阅读 开" : "自动阅读 关");
    app->auto_wait = false;
    app->auto_accum_ms = 0;
}

static void handle_game_key(nn_app_t *app, const nn_key_t *key)
{
    if (app->error_shown) {
        // 剧本坏了只允许“OK 短按”进菜单离开，避免玩家卡在错误页
        if (key->btn == NN_BTN_OK && key->ev == NN_EV_CLICK) show_menu(app);
        return;
    }
    if (key->btn == NN_BTN_UP) {
        // 长按 = 快进（按住生效），松手停：RELEASE 全局只用于这一个动作
        if (key->ev == NN_EV_LONG) { app->ff_hold = true; return; }
        if (key->ev == NN_EV_RELEASE) { app->ff_hold = false; return; }
        if (key->ev != NN_EV_CLICK) return;
        if (app->typing) {
            app->typing = false;
            render_line(app);
            return;
        }
        if (app->page_index + 1 < app->page_count) {
            app->page_index++;
            app->typing = false;
            render_line(app);
            return;
        }
        // 选项未选时离开过选项页：再按 UP 要回到同一个选项，绝不能直接跳过分支。
        if (app->choice_pending) { show_choice(app); return; }
        advance(app);
        return;
    }
    if (key->btn == NN_BTN_DOWN) {
        if (key->ev == NN_EV_LONG) { toggle_auto(app); return; }
        if (key->ev != NN_EV_CLICK) return;
        enter_page(app, NN_PAGE_HISTORY);
        show_history(app);
        return;
    }
    if (key->btn == NN_BTN_OK) {
        // 长按 OK = 进菜单；短按 OK = 推进下一句（与其余 11 部统一）。
        if (key->ev == NN_EV_LONG) {
            app->suppress_click = true;
            app->suppress_tick = lv_tick_get();
            show_menu(app);
            return;
        }
        if (key->ev != NN_EV_CLICK) return;
        if (app->suppress_click) {   // 吞掉长按后驱动补发的 CLICK
            app->suppress_click = false;
            return;
        }
        if (app->typing) {
            app->typing = false;
            render_line(app);
            return;
        }
        if (app->page_index + 1 < app->page_count) {
            app->page_index++;
            app->typing = false;
            render_line(app);
            return;
        }
        if (app->choice_pending) { show_choice(app); return; }
        advance(app);
    }
}

// 浮层选中项移动后必须重绘（此前只改了数据，屏幕上高亮不动 → “设置菜单上下无法导航”）。
static void overlay_repaint(nn_app_t *app)
{
    switch (app->page) {
    case NN_PAGE_TITLE: show_title(app); break;
    case NN_PAGE_MENU: show_menu(app); break;
    case NN_PAGE_SETTINGS: show_settings(app); break;
    case NN_PAGE_SLOTS: show_slots(app, app->slots_saving, NULL); break;
    case NN_PAGE_CHOICE: choice_repaint(app); break;   // 选项上下移动（原来漏了这一页）
    default: break;
    }
}

static void handle_overlay_key(nn_app_t *app, const nn_key_t *key, int count, int *sel,
                               void (*on_ok)(nn_app_t *, int), void (*on_back)(nn_app_t *))
{
    const bool up = (key->btn == NN_BTN_UP && key->ev == NN_EV_CLICK);
    const bool down = (key->btn == NN_BTN_DOWN && key->ev == NN_EV_CLICK);
    if (up) {
        if (*sel > 0) (*sel)--;
        else if (*sel < 0) *sel = 0;
        overlay_repaint(app);
        return;
    }
    if (down) {
        if (*sel < count - 1) (*sel)++;
        overlay_repaint(app);
        return;
    }
    if (key->btn == NN_BTN_OK && key->ev == NN_EV_CLICK) { on_ok(app, *sel); return; }
    if (key->btn == NN_BTN_OK && key->ev == NN_EV_LONG) { on_back(app); return; }
}

static void title_ok(nn_app_t *app, int sel)
{
    switch (sel) {
    case TITLE_NEW:
        start_new_game(app);
        break;
    case TITLE_CONTINUE: {
        nn_save_t save;
        if (nn_save_read(NN_SLOT_AUTO, &save)) {
            apply_restore(app, &save);
        } else {
            start_new_game(app);
        }
        break;
    }
    case TITLE_LOAD:
        app->slot_sel = 0;
        enter_page(app, NN_PAGE_SLOTS);
        show_slots(app, false, NULL);
        break;
    case TITLE_SETTINGS:
        app->settings_sel = 0;
        enter_page(app, NN_PAGE_SETTINGS);
        show_settings(app);
        break;
    case TITLE_GALLERY:
        app->gallery_sel = 0;
        app->gallery_page = 0;
        enter_page(app, NN_PAGE_GALLERY);
        show_gallery(app);
        break;
    case TITLE_ABOUT:
        enter_page(app, NN_PAGE_ABOUT);
        nn_ui_set_ending(&app->ui, "", "");
        nn_ui_set_about(&app->ui,
            "男娘恋爱物语 · AI Passport 横屏移植\n\n"
            "剧本、立绘与 CG 来自 hrk666666/NianNiang-MiBand-10"
            "（原作《男娘恋爱物语》）。素材按源文件 1:1 缩放到屏幕高度居中显示，"
            "未裁切、未二次拉伸；原始画面比例未知。\n\n"
            "按键：上=推进（长按快进）  下=历史（长按自动）  OK=菜单\n"
            "本移植仅供个人学习交流。");
        break;
    default:
        break;
    }
}

static void back_to_title(nn_app_t *app)
{
    app->ff_hold = false;
    app->auto_wait = false;
    app->error_shown = false;  // 错误页提示“回到标题重开”必须真的生效
    app->choice_pending = false;
    app->ret_depth = 0;        // 标题 = 根页
    app->title_sel = 0;
    if (nn_save_exists(NN_SLOT_AUTO)) app->title_sel = TITLE_CONTINUE;
    show_title(app);
}

// 从菜单回正文：待选的选项要重新显示，而不是停在提示句上让玩家盲选。
static void resume_game(nn_app_t *app)
{
    if (app->choice_pending) show_choice(app);
    else show_game(app);
}

static void menu_ok(nn_app_t *app, int sel)
{
    switch (sel) {
    case MENU_RESUME:
        resume_game(app);
        break;
    case MENU_SAVE:
        app->slot_sel = 0;
        enter_page(app, NN_PAGE_SLOTS);
        show_slots(app, true, NULL);
        break;
    case MENU_LOAD:
        app->slot_sel = 0;
        enter_page(app, NN_PAGE_SLOTS);
        show_slots(app, false, NULL);
        break;
    case MENU_HISTORY:
        app->history_top = 0;
        enter_page(app, NN_PAGE_HISTORY);
        show_history(app);
        break;
    case MENU_SETTINGS:
        app->settings_sel = 0;
        enter_page(app, NN_PAGE_SETTINGS);
        show_settings(app);
        break;
    case MENU_TITLE:
        auto_save(app);
        back_to_title(app);
        break;
    default:
        break;
    }
}

// 按来源栈返回（标题/菜单/正文/设置）。栈空则回标题。
static void go_back(nn_app_t *app)
{
    nn_page_t target = NN_PAGE_TITLE;
    if (app->ret_depth > 0) target = app->ret_stack[--app->ret_depth];
    switch (target) {
    case NN_PAGE_MENU:
        show_menu(app);
        break;
    case NN_PAGE_GAME:
        show_game(app);
        break;
    case NN_PAGE_SETTINGS:
        show_settings(app);
        break;
    case NN_PAGE_TITLE:
    default:
        back_to_title(app);
        break;
    }
}

static void slots_ok(nn_app_t *app, int sel)
{
    const bool saving = app->slots_saving;
    if (saving) {
        nn_save_t save;
        snapshot(app, &save);
        if (nn_save_write((uint8_t)sel, &save)) {
            show_slots(app, true, "已保存");
        } else {
            show_slots(app, true, nn_store_available() ? "保存失败：写入 NVS 出错，旧档保持不变"
                                                       : "保存失败：NVS 不可用");
        }
        return;
    }
    nn_save_t save;
    if (!nn_save_read((uint8_t)sel, &save)) {
        show_slots(app, app->slots_saving, "该槽位没有存档");
        return;
    }
    apply_restore(app, &save);
}

static void settings_ok(nn_app_t *app, int sel)
{
    switch (sel) {
    case SET_SPEED:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) & 3);
        break;
    case SET_FONT:
        app->settings.font_large ^= 1u;
        nn_ui_set_font_large(&app->ui, app->settings.font_large != 0);
        // 换字号后重新分页，并立即按新排版重画当前这句
        if (app->line_text) {
            paginate_line(app);
            app->typing = false;
            app->shown_chars = 0;
        }
        break;
    case SET_AUTO:
        app->settings.auto_play ^= 1u;
        app->auto_mode = app->settings.auto_play != 0;
        break;
    case SET_OPA:
        app->settings.panel_opa = (uint8_t)(app->settings.panel_opa >= 70 ? 10
                                                                        : app->settings.panel_opa + 15);
        nn_ui_set_panel_opa(&app->ui, app->settings.panel_opa);
        break;
    case SET_CLEAR:
        nn_unlock_clear();
        nn_ui_overlay_foot(&app->ui, "观看记录已清除");
        break;
    case SET_ABOUT: {
        // 关于是独立页面：切过去就返回，不能再被下面的 show_settings() 覆盖
        enter_page(app, NN_PAGE_ABOUT);
        nn_ui_set_ending(&app->ui, "", "");
        nn_ui_set_about(&app->ui,
            "文字速度：慢/中/快/瞬间（瞬间=整页直接出现）\n"
            "字号：16px / 20px（每页 4 行 / 3 行）\n"
            "自动阅读：每句打字完成后 0.9 秒推进\n"
            "文本框：10%–55% 不透明度，底下画面透出\n\n"
            "按键：上=推进（长按快进） 下=历史（长按自动） OK=菜单\n"
            "列表：上/下 选择，OK 确认，长按 OK 返回。");
        return;     // 关于页是独立页面，不要再切回设置
    }
    default:
        break;
    }
    nn_settings_store(&app->settings);
    show_settings(app);
    if (app->line_text) render_line(app);
}

// ---------------------------------------------------------------- 各页回调
static void choice_ok(nn_app_t *a, int sel)
{
    if (!nn_rt_choose(&a->rt, (uint8_t)sel)) return;
    a->rt.choice_count = 0;
    a->choice_pending = false;
    repaint_view(a);
    advance(a);
}

static void choice_back(nn_app_t *a) { show_game(a); }

static void title_back(nn_app_t *a) { (void)a; }   // 标题页长按 OK 不做别的事

static void menu_back(nn_app_t *a) { resume_game(a); }

static void slots_back(nn_app_t *a) { go_back(a); }

static void settings_back(nn_app_t *a) { go_back(a); }

static void about_back(nn_app_t *a) { go_back(a); }

static void history_back(nn_app_t *a) { go_back(a); }

void nn_app_key(nn_app_t *app, const nn_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;         // 任何按键都算“有操作”
    // 新手势开始（PRESS）先解除上一次长按留下的“吞 CLICK”状态，否则下一次
    // 独立短按的 CLICK 也会被吃掉（实机“菜单里按 OK 没反应/错位”）。
    if (key->ev == NN_EV_PRESS) app->suppress_click = false;
    // 长按 OK 后驱动可能在松手时补发一个 CLICK，必须吞掉；
    // 但**不能无限期吞**——如果驱动这次没补发，标志会一直挂着，
    // 之后的第一次确认（比如选项页 OK）就被吃掉 → 实机"选项不可选择"。
    if (app->suppress_click) {
        const bool fresh = (lv_tick_get() - app->suppress_tick) < 400u;
        if (!fresh) {
            app->suppress_click = false;
        } else if (key->ev == NN_EV_RELEASE) {
            return;
        } else if (key->ev == NN_EV_CLICK) {
            app->suppress_click = false;
            return;
        }
    }
    switch (app->page) {
    case NN_PAGE_GAME:
        handle_game_key(app, key);
        break;
    case NN_PAGE_CHOICE:
        handle_overlay_key(app, key, app->choice_count, &app->choice_sel,
                           choice_ok, choice_back);
        break;
    case NN_PAGE_TITLE:
        handle_overlay_key(app, key, 6, &app->title_sel, title_ok, title_back);
        break;
    case NN_PAGE_MENU:
        handle_overlay_key(app, key, 6, &app->menu_sel, menu_ok, menu_back);
        break;
    case NN_PAGE_SLOTS:
        // 统一语义：长按 OK = 返回来源页。不把“删除存档”绑在任何手势上。
        handle_overlay_key(app, key, NN_SLOT_COUNT, &app->slot_sel, slots_ok, slots_back);
        break;
    case NN_PAGE_SETTINGS:
        handle_overlay_key(app, key, SET_COUNT, &app->settings_sel, settings_ok, settings_back);
        break;
    case NN_PAGE_HISTORY:
        if (key->btn == NN_BTN_UP && key->ev == NN_EV_CLICK && app->history_top < app->hist_count - 1) {
            app->history_top++;
            show_history(app);
        } else if (key->btn == NN_BTN_DOWN && key->ev == NN_EV_CLICK && app->history_top > 0) {
            app->history_top--;
            show_history(app);
        } else if (key->btn == NN_BTN_OK &&
                   (key->ev == NN_EV_CLICK || key->ev == NN_EV_LONG)) {
            history_back(app);
        }
        break;
    case NN_PAGE_GALLERY:
        if (key->btn == NN_BTN_UP && key->ev == NN_EV_CLICK) gallery_move(app, -1);
        else if (key->btn == NN_BTN_DOWN && key->ev == NN_EV_CLICK) gallery_move(app, 1);
        else if (key->btn == NN_BTN_OK && key->ev == NN_EV_CLICK) gallery_ok(app);
        else if (key->btn == NN_BTN_OK && key->ev == NN_EV_LONG) go_back(app);
        break;
    case NN_PAGE_CGVIEW:
        if (key->btn == NN_BTN_OK && (key->ev == NN_EV_CLICK || key->ev == NN_EV_LONG)) {
            show_gallery(app);
        } else if ((key->btn == NN_BTN_UP || key->btn == NN_BTN_DOWN) && key->ev == NN_EV_CLICK) {
            const int total = cg_count(app);
            const int idx = app->gallery_page * 6 + app->gallery_sel + (key->btn == NN_BTN_UP ? -1 : 1);
            if (idx >= 0 && idx < total) {
                app->gallery_sel = idx % 6;
                app->gallery_page = idx / 6;
                gallery_ok(app);
            }
        }
        break;
    case NN_PAGE_ENDING:
        if (key->btn == NN_BTN_OK && (key->ev == NN_EV_CLICK || key->ev == NN_EV_LONG)) {
            back_to_title(app);
        }
        break;
    case NN_PAGE_ABOUT:
        if (key->btn == NN_BTN_OK && (key->ev == NN_EV_CLICK || key->ev == NN_EV_LONG)) {
            about_back(app);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- 计时
void nn_app_tick(nn_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->uptime_s += elapsed_ms / 1000;
    if (app->auto_mode) app->idle_ms = 0;   // 自动推进期间不算空闲
    else app->idle_ms += elapsed_ms;
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= 30000) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery) {
            app->battery = soc;
            nn_ui_set_battery(&app->ui, soc);
        }
    }

    if (app->page != NN_PAGE_GAME) return;
    // 选项还没选：快进/自动都不能代选或跳过分支（只能玩家显式 OK）。
    if (app->choice_pending) { app->ff_hold = false; return; }

    // 打字机
    if (app->typing) {
        app->type_accum_ms += elapsed_ms;
        int step = app->settings.text_speed == 0 ? 90
                 : app->settings.text_speed == 1 ? 40
                 : app->settings.text_speed == 2 ? 18 : 0;
        if (step == 0) {
            app->typing = false;
            render_line(app);
        } else {
            const int per_char_ms = step;
            int chars = (int)(app->type_accum_ms / (uint32_t)per_char_ms);
            if (chars > 0) {
                app->type_accum_ms -= (uint32_t)chars * (uint32_t)per_char_ms;
                const int total = chars_in_page(app);
                if (app->shown_chars + chars >= total) {
                    app->typing = false;
                    app->shown_chars = 0;
                } else {
                    app->shown_chars += chars;
                }
                render_line(app);
            }
        }
    }

    // 按住快进
    if (app->ff_hold) {
        app->ff_accum_ms += elapsed_ms;
        if (app->ff_accum_ms >= FF_STEP_MS) {
            app->ff_accum_ms = 0;
            // 快进：文字必须瞬显，并且一个 tick 内把当前句剩下的页翻完（最多 4 页），
            // 翻完立刻取下一句 —— 以前是"一 tick 一件事 + 70ms 间隔"，看着就是每句卡一下。
            app->typing = false;
            int stepped = 0;
            while (stepped < 4 && app->page_index + 1 < app->page_count) {
                app->page_index++;
                stepped++;
            }
            if (stepped > 0) {
                render_line(app);
            } else {
                // 快进：一 tick 内连续取 3 句（原来只 advance 一次，50ms/tick 时最快
                // 也只有 20 句/秒，看着就像"一卡一卡"）。遇到选项/结局 advance 会切页，停。
                for (int k = 0; k < 3; ++k) {
                    advance(app);
                    if (app->page != NN_PAGE_GAME || app->typing ||
                        app->page_index + 1 < app->page_count) {
                        break;
                    }
                }
            }
        }
        return;
    }

    // 自动阅读
    if (app->auto_mode && !app->typing) {
        if (!app->auto_wait) {
            app->auto_wait = true;
            app->auto_accum_ms = 0;
        } else {
            app->auto_accum_ms += elapsed_ms;
            if (app->auto_accum_ms >= AUTO_DELAY_MS) {
                app->auto_accum_ms = 0;
                app->auto_wait = false;
                app->idle_ms = 0;      // 自动阅读期间不熄屏/不休眠
                if (app->page_index + 1 < app->page_count) {
                    app->page_index++;
                    render_line(app);
                } else {
                    advance(app);
                }
            }
        }
    }
}

uint32_t nn_app_idle_ms(const nn_app_t *app)
{
    // 自动推进期间不得调暗/熄屏/深睡：有效空闲恒为 0。
    if (app && app->auto_mode) return 0;
    return app ? app->idle_ms : 0;
}

void nn_app_flush(nn_app_t *app)
{
    if (!app) return;
    if (app->page == NN_PAGE_GAME || app->page == NN_PAGE_CHOICE) auto_save(app);
}
