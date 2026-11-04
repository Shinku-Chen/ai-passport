// main/steins_ui.c —— 320x240 横屏界面的 LVGL 对象装配。
//
// 这一层只负责"把字符串放进正确的行对象",不做状态判断:所有按键语义、页面
// 来源栈、存档/解锁逻辑都在 steins_app.c。这样宿主机测试可以直接驱动生产 UI
// (用 LVGL 薄桩记录可见位与行文本),确认页面真的显示出来了、行文本真的是它。
#include "steins_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

// 源快应用的配色(Steins;Gate 深色 + 琥珀强调)
#define C_BG 0x141512u
#define C_TEXT 0xF0EEE4u
#define C_DIM 0xA4A48Eu
#define C_AMBER 0xEFAA42u
#define C_SELECT 0x303127u
#define C_ACCENT 0xE49A2Cu
#define C_ERROR 0xEAB7B7u

static struct _lv_obj_t *panel(struct _lv_obj_t *parent, int x, int y, int w, int h,
                               uint32_t color, int opa)
{
    struct _lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)opa, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static struct _lv_obj_t *label(struct _lv_obj_t *parent, int x, int y, int w, int h,
                               const struct _lv_font_t *font, uint32_t color,
                               const char *text)
{
    struct _lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, (const lv_font_t *)font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    // 默认单行 CLIP（列表行/正文都按行排版）；需要折行的控件自己再设回 WRAP。
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(obj, text ? text : "");
    return obj;
}

static struct _lv_obj_t *page(struct _lv_obj_t *root)
{
    struct _lv_obj_t *obj = lv_obj_create(root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, STEINS_UI_W, STEINS_UI_H);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static void set_row(struct _lv_obj_t *row, const char *text)
{
    lv_label_set_text(row, text ? text : "");
}

static void row_style(struct _lv_obj_t *row, bool selected)
{
    lv_obj_set_style_text_color(row, lv_color_hex(selected ? C_AMBER : C_TEXT), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_SELECT), 0);
    lv_obj_set_style_bg_opa(row, (lv_opa_t)(selected ? 210 : 0), 0);
}

bool steins_ui_create(steins_ui_t *ui, struct _lv_obj_t *root, uint16_t *canvas_pixels,
                      const struct _lv_font_t *font_story,
                      const struct _lv_font_t *font_ui,
                      const struct _lv_font_t *font_ui_lg)
{
    if (!ui || !root || !canvas_pixels || !font_story || !font_ui) return false;
    memset(ui, 0, sizeof(*ui));
    ui->root = root;
    // 屏幕底色必须是深色：LVGL 默认浅灰，任何瞬时未覆盖区域（切页重绘）都会闪白。
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui->font_story = font_story;
    ui->font_ui = font_ui;
    ui->font_ui_lg = font_ui_lg ? font_ui_lg : font_ui;
    if (!steins_image_init(&ui->image, root, canvas_pixels)) return false;

    // ---------------- 标题页:原画背景(画布) + 世界线 + 菜单
    struct _lv_obj_t *p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_TITLE] = p;
    // 标题菜单下移到下半屏(150..240)：菜单用不透明底（半透只保留给阅读页对话框，
    // 半透面板每次重绘都要与下方画布做 alpha 混合，是标题页按键卡顿的主因）。
    panel(p, 0, 150, STEINS_UI_W, 90, C_BG, 255);
    // 作品名/世界线压在图上,加一条底条保证可读(否则深色画面上看不清)。
    panel(p, 6, 6, STEINS_UI_W - 12, 34, C_BG, 255);
    ui->title_name = label(p, 10, 10, 190, 22, ui->font_ui_lg, C_AMBER, "");
    ui->title_worldline = label(p, 196, 12, 114, 20, ui->font_ui, C_DIM, "");
    lv_obj_set_style_text_align(ui->title_worldline, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < STEINS_UI_TITLE_VISIBLE; ++i) {
        // 单列 4 行窗口：154/172/190/208，末行盒底 226，选中项跟随滚动。
        ui->title_rows[i] = label(p, 8, 154 + i * 18, STEINS_UI_W - 16, 18,
                                  ui->font_ui, C_TEXT, "");
        row_style(ui->title_rows[i], false);
    }

    // ---------------- 正文页
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_GAME] = p;
    ui->game_band = panel(p, 0, 150, STEINS_UI_W, 90, C_BG, 180);   // 0xB4：能看到画面
    panel(p, 0, 0, 120, 19, 0x000000u, 160);
    panel(p, STEINS_UI_W - 46, 0, 46, 19, 0x000000u, 160);
    ui->game_date = label(p, 4, 1, 116, 18, ui->font_ui, C_DIM, "");
    ui->game_battery = label(p, STEINS_UI_W - 46, 1, 42, 18, ui->font_ui, C_DIM, "");
    lv_obj_set_style_text_align(ui->game_battery, LV_TEXT_ALIGN_RIGHT, 0);
    ui->game_name = label(p, 10, 128, 220, 19, ui->font_story, C_AMBER, "");
    // 名字在框外（压在画面上）：底条只包住文字（内容宽度 + 左右 6px）。
    lv_obj_set_width(ui->game_name, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(ui->game_name, 6, 0);
    lv_obj_set_style_bg_color(ui->game_name, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->game_name, 180, 0);
    ui->game_status = label(p, 8, 152, 240, 19, ui->font_ui, C_DIM, "");
    ui->game_page = label(p, 262, 152, 50, 19, ui->font_ui, C_DIM, "");
    lv_obj_set_style_text_align(ui->game_page, LV_TEXT_ALIGN_RIGHT, 0);
    // 去掉“OK 推进”提示行后正文扩到 4 行：164 + 3×18 + 18 = 236（末行盒底 ≤238）。
    ui->game_text = label(p, 8, 164, STEINS_TEXT_W, 72, ui->font_story, C_TEXT, "");
    lv_obj_set_style_text_line_space(ui->game_text, -1, 0);
    // 错误提示占用正文区块（164..236）：正常阅读时不显示（按键提示已移除）。
    ui->game_hint = label(p, 8, 164, STEINS_TEXT_W, 72, ui->font_ui, C_DIM, "");
    lv_obj_add_flag(ui->game_hint, LV_OBJ_FLAG_HIDDEN);

    // ---------------- 选择页
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_CHOICE] = p;
    panel(p, 0, 0, STEINS_UI_W, STEINS_UI_H, C_BG, 248);
    ui->choice_tag = label(p, 8, 4, 240, 18, ui->font_ui, C_ACCENT, "");
    ui->choice_heading = label(p, 8, 24, STEINS_UI_W - 16, 46, ui->font_story, C_TEXT, "");
    for (int i = 0; i < STEINS_UI_CHOICE_ROWS; ++i) {
        ui->choice_rows[i] = label(p, 8, 72 + i * 34, STEINS_UI_W - 16, 32,
                                   ui->font_story, C_TEXT, "");
        row_style(ui->choice_rows[i], false);
    }
    ui->choice_hint = label(p, 8, 212, STEINS_UI_W - 16, 20, ui->font_ui, C_DIM, "");

    // ---------------- 通用列表页:菜单/存读档/历史/设置/章节/鉴赏/关于 共用一个对象
    p = page(root);
    if (!p) return false;
    panel(p, 0, 0, STEINS_UI_W, STEINS_UI_H, C_BG, 255);
    ui->list_title = label(p, 8, 4, 240, 22, ui->font_ui_lg, C_AMBER, "");
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        ui->list_rows[i] = label(p, 10, 31 + i * 36, 236, 18, ui->font_ui, C_TEXT, "");
        ui->list_subs[i] = label(p, 18, 47 + i * 36, 292, 16, ui->font_story, C_DIM, "");
        row_style(ui->list_rows[i], false);
    }
    ui->list_hint = label(p, 8, 220, STEINS_UI_W - 16, 18, ui->font_ui, C_DIM, "");
    // ABOUT 专用多行正文（列表行是单行 label，塞不下整段 body）。
    ui->about_body = label(p, 8, 30, STEINS_UI_W - 16, STEINS_UI_ABOUT_H, ui->font_ui,
                           C_TEXT, "");
    lv_label_set_long_mode(ui->about_body, LV_LABEL_LONG_MODE_WRAP);
    // 长文案必须能读全：正文可滚动，上下键逐行滚；滚动条隐藏（屏幕窄，条会压字）。
    lv_obj_add_flag(ui->about_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ui->about_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ui->about_body, LV_OBJ_FLAG_HIDDEN);
    const steins_page_t list_pages[] = {
        STEINS_PAGE_MENU, STEINS_PAGE_SAVES, STEINS_PAGE_HISTORY, STEINS_PAGE_SETTINGS,
        STEINS_PAGE_CHAPTERS, STEINS_PAGE_GALLERY, STEINS_PAGE_ABOUT,
        STEINS_PAGE_PHONE_HOME, STEINS_PAGE_PHONE_INBOX, STEINS_PAGE_PHONE_SENT,
        STEINS_PAGE_PHONE_CALLS, STEINS_PAGE_PHONE_CONTACTS, STEINS_PAGE_PHONE_REPLY,
    };
    for (size_t i = 0; i < sizeof(list_pages) / sizeof(list_pages[0]); ++i) {
        ui->pages[list_pages[i]] = p;
    }

    // ---------------- 邮件正文
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_MAIL] = p;
    ui->pages[STEINS_PAGE_PHONE_CLIP] = p;
    panel(p, 0, 0, STEINS_UI_W, STEINS_UI_H, C_BG, 255);
    ui->mail_title = label(p, 8, 4, 220, 20, ui->font_ui_lg, C_AMBER, "");
    ui->mail_page = label(p, STEINS_UI_W - 70, 4, 62, 20, ui->font_ui, C_DIM, "");
    lv_obj_set_style_text_align(ui->mail_page, LV_TEXT_ALIGN_RIGHT, 0);
    ui->mail_text = label(p, 8, 30, STEINS_TEXT_W, 6 * STEINS_LINE_H, ui->font_story, C_TEXT, "");
    ui->mail_hint = label(p, 8, 214, STEINS_UI_W - 16, 20, ui->font_ui, C_DIM, "");

    // ---------------- CG 鉴赏
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_CG] = p;
    // 画面由共用画布承担（页面必须透明，否则整屏黑面板会把 CG/章节卡盖成黑屏）。
    panel(p, 0, 200, STEINS_UI_W, 40, C_BG, 255);
    ui->cg_caption = label(p, 8, 206, STEINS_UI_W - 16, 18, ui->font_ui, C_TEXT, "");
    ui->cg_hint = label(p, 8, 224, STEINS_UI_W - 16, 16, ui->font_ui, C_DIM, "");

    // ---------------- 结局
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_ENDING] = p;
    panel(p, 0, 0, STEINS_UI_W, STEINS_UI_H, C_BG, 255);
    ui->ending_title = label(p, 12, 56, STEINS_UI_W - 24, 30, ui->font_ui_lg, C_AMBER, "");
    ui->ending_body = label(p, 12, 96, STEINS_UI_W - 24, 96, ui->font_ui, C_TEXT, "");
    ui->ending_hint = label(p, 12, 214, STEINS_UI_W - 24, 20, ui->font_ui, C_DIM, "");

    // ---------------- 确认弹窗
    p = page(root);
    if (!p) return false;
    ui->pages[STEINS_PAGE_CONFIRM] = p;
    panel(p, 0, 0, STEINS_UI_W, STEINS_UI_H, 0x000000u, 150);
    panel(p, 30, 78, STEINS_UI_W - 60, 94, C_SELECT, 250);
    ui->confirm_text = label(p, 40, 90, STEINS_UI_W - 80, 46, ui->font_ui, C_TEXT, "");
    ui->confirm_hint = label(p, 40, 142, STEINS_UI_W - 80, 20, ui->font_ui, C_AMBER, "");

    ui->page = STEINS_PAGE_TITLE;
    steins_ui_show(ui, STEINS_PAGE_TITLE);
    return true;
}

bool steins_ui_page_visible(const steins_ui_t *ui, steins_page_t page)
{
    if (!ui || page >= STEINS_PAGE_COUNT || !ui->pages[page]) return false;
    return !lv_obj_has_flag(ui->pages[page], LV_OBJ_FLAG_HIDDEN);
}

steins_image_t *steins_ui_image(steins_ui_t *ui)
{
    return ui ? &ui->image : NULL;
}

void steins_ui_show(steins_ui_t *ui, steins_page_t page)
{
    if (!ui || page >= STEINS_PAGE_COUNT) return;
    // 多个 page 共用同一个容器（菜单/存档/历史/设置/章节/鉴赏/关于/手机各页都是同一块），
    // 逐下标 hide/show 会让“后面的别名”把目标页又隐藏掉 → 整屏只剩 root 底色（看起来黑屏）。
    // 正确做法：先对所有“唯一对象”统一隐藏，再只显示目标页对应的对象。
    for (int i = 0; i < STEINS_PAGE_COUNT; ++i) {
        lv_obj_t *obj = ui->pages[i];
        if (!obj) continue;
        bool seen = false;
        for (int j = 0; j < i; ++j) {
            if (ui->pages[j] == obj) { seen = true; break; }
        }
        if (!seen) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui->pages[page]) lv_obj_remove_flag(ui->pages[page], LV_OBJ_FLAG_HIDDEN);
    ui->page = page;
    // 画布是全屏共用的一块:鉴赏时下移留出字幕位,标题/正文时贴顶。
    // 画布先于页面创建,因此永远在页面之下;不透明页面会把画布完全盖住。
    if (page == STEINS_PAGE_CG) steins_image_place(&ui->image, 20);
    else steins_image_place(&ui->image, 0);
    if (ui->image.canvas) {
        if (page == STEINS_PAGE_TITLE || page == STEINS_PAGE_CG || page == STEINS_PAGE_GAME) {
            lv_obj_remove_flag(ui->image.canvas, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->image.canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void steins_ui_title(steins_ui_t *ui, const char *name, const char *worldline,
                     const char *const *rows, int row_count, int selected)
{
    if (!ui) return;
    set_row(ui->title_name, name);
    set_row(ui->title_worldline, worldline);
    if (row_count > STEINS_UI_TITLE_ROWS) row_count = STEINS_UI_TITLE_ROWS;
    // 单列窗口：first = clamp(sel-1, 0, count-VIS)，让选中项尽量居中。
    int first = selected - 1;
    if (first > row_count - STEINS_UI_TITLE_VISIBLE) first = row_count - STEINS_UI_TITLE_VISIBLE;
    if (first < 0) first = 0;
    ui->title_first = first;
    for (int i = 0; i < STEINS_UI_TITLE_VISIBLE; ++i) {
        const int index = first + i;
        set_row(ui->title_rows[i], index < row_count ? rows[index] : "");
        // 缓存可见行文本，供上下键只挪高亮时复用（不再回头整页重渲染）。
        snprintf(ui->title_text[i], sizeof(ui->title_text[i]), "%s",
                 index < row_count ? rows[index] : "");
        // 只给“原选中/新选中”两行重设样式：其余行样式没变，避免整块重绘。
        if (index == selected || index == ui->title_sel_shown) {
            row_style(ui->title_rows[i], index == selected);
        }
    }
    ui->title_sel_shown = selected;
    ui->title_row_count = row_count;
    ui->title_all_count = row_count;
    for (int i = 0; i < STEINS_UI_TITLE_ROWS; ++i) {
        snprintf(ui->title_all[i], sizeof(ui->title_all[i]), "%s",
                 i < row_count ? rows[i] : "");
    }
}

void steins_ui_set_box_opacity(steins_ui_t *ui, int level)
{
    if (!ui || !ui->game_band) return;
    static const lv_opa_t levels[3] = { 205, 232, 255 };
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    lv_obj_set_style_bg_opa(ui->game_band, levels[level], 0);
}

void steins_ui_game(steins_ui_t *ui, const char *date, const char *battery,
                    const char *speaker, const char *text, const char *status,
                    const char *page, const char *hint, bool failed)
{
    if (!ui) return;
    set_row(ui->game_date, date);
    set_row(ui->game_battery, battery);
    set_row(ui->game_name, speaker);
    set_row(ui->game_status, status);
    set_row(ui->game_page, page);
    set_row(ui->game_hint, hint);
    // 出错：收起正文、顶部行改显示错误提示；正常：正文可见、顶部行显示状态/按键提示。
    if (failed) {
        set_row(ui->game_text, "");
        lv_obj_add_flag(ui->game_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->game_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->game_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_row(ui->game_text, text);
        lv_obj_remove_flag(ui->game_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->game_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->game_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void steins_ui_choice(steins_ui_t *ui, const char *tag, const char *heading,
                      const char *const *rows, int row_count, int selected,
                      const char *hint)
{
    if (!ui) return;
    set_row(ui->choice_tag, tag);
    set_row(ui->choice_heading, heading);
    const int first = selected >= STEINS_UI_CHOICE_ROWS ? selected - STEINS_UI_CHOICE_ROWS + 1 : 0;
    for (int i = 0; i < STEINS_UI_CHOICE_ROWS; ++i) {
        const int index = first + i;
        set_row(ui->choice_rows[i], index < row_count ? rows[index] : "");
        row_style(ui->choice_rows[i], index == selected);
    }
    set_row(ui->choice_hint, hint);
}

void steins_ui_list(steins_ui_t *ui, const char *title, const char *const *rows,
                    const char *const *subs, int row_count, int selected,
                    const char *hint)
{
    if (!ui) return;
    set_row(ui->list_title, title);
    // rows/subs 是“当前窗口”的 5 行,不是整张列表:调用方已经按 sel 算好了窗口。
    // 这里绝不能拿绝对下标去索引,否则越过 5 行读到栈上的野指针。
    const int first = selected >= STEINS_UI_LIST_ROWS ? selected - STEINS_UI_LIST_ROWS + 1 : 0;
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int index = first + i;
        const bool present = index < row_count;
        set_row(ui->list_rows[i], present ? rows[i] : "");
        set_row(ui->list_subs[i], (subs && present) ? subs[i] : "");
        if (index == selected || index == ui->list_sel_shown) {
            row_style(ui->list_rows[i], index == selected);
        }
    }
    set_row(ui->list_hint, hint);
    ui->list_sel_shown = selected;
    // 普通列表页：确保列表行可见、ABOUT 正文隐藏（两页共用同一容器）。
    lv_obj_add_flag(ui->about_body, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        lv_obj_remove_flag(ui->list_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->list_subs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void steins_ui_about(steins_ui_t *ui, const char *title, const char *body)
{
    if (!ui) return;
    set_row(ui->list_title, title);
    set_row(ui->about_body, body);
    lv_obj_remove_flag(ui->about_body, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        lv_obj_add_flag(ui->list_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->list_subs[i], LV_OBJ_FLAG_HIDDEN);
    }
    set_row(ui->list_hint, "OK 返回 · 长按返回");
}

void steins_ui_title_select(steins_ui_t *ui, int selected)
{
    if (!ui) return;
    if (selected < 0) selected = 0;
    if (ui->title_all_count > 0 && selected >= ui->title_all_count) {
        selected = ui->title_all_count - 1;
    }
    if (selected == ui->title_sel_shown) return;

    int first = selected - 1;
    if (first > ui->title_all_count - STEINS_UI_TITLE_VISIBLE) {
        first = ui->title_all_count - STEINS_UI_TITLE_VISIBLE;
    }
    if (first < 0) first = 0;
    const int old_sel = ui->title_sel_shown;
    if (first != ui->title_first) {
        // 窗口跟着挪：只重设真正换掉的那几行文本（从整份缓存取，不回头找 app）。
        for (int i = 0; i < STEINS_UI_TITLE_VISIBLE; ++i) {
            const int index = first + i;
            const char *want = ui->title_all[index];
            if (strcmp(ui->title_text[i], want) != 0) {
                set_row(ui->title_rows[i], want);
                snprintf(ui->title_text[i], sizeof(ui->title_text[i]), "%s", want);
            }
        }
        ui->title_first = first;
    }
    // 只改颜色：原选中与新选中的两行，不动位置、不 invalidate 父对象。
    for (int i = 0; i < STEINS_UI_TITLE_VISIBLE; ++i) {
        const int index = first + i;
        if (index == selected || index == old_sel) {
            row_style(ui->title_rows[i], index == selected);
        }
    }
    ui->title_sel_shown = selected;
}

int steins_ui_about_scroll(steins_ui_t *ui, int delta_lines)
{
    if (!ui || !ui->about_body || delta_lines == 0) return 0;
    const int before = (int)lv_obj_get_scroll_y(ui->about_body);
    lv_obj_scroll_by(ui->about_body, 0, delta_lines * STEINS_UI_LINE_H, LV_ANIM_OFF);
    return (int)lv_obj_get_scroll_y(ui->about_body) - before;
}

void steins_ui_list_font(steins_ui_t *ui, bool story_font)
{
    if (!ui) return;
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        lv_obj_set_style_text_font(ui->list_rows[i],
                                   (const lv_font_t *)(story_font ? ui->font_story
                                                                  : ui->font_ui), 0);
    }
}

void steins_ui_mail(steins_ui_t *ui, const char *title, const char *text,
                    const char *page_text, const char *hint)
{
    if (!ui) return;
    set_row(ui->mail_title, title);
    set_row(ui->mail_text, text);
    set_row(ui->mail_page, page_text);
    set_row(ui->mail_hint, hint);
}

void steins_ui_cg(steins_ui_t *ui, const char *caption, const char *hint)
{
    if (!ui) return;
    set_row(ui->cg_caption, caption);
    set_row(ui->cg_hint, hint);
}

void steins_ui_ending(steins_ui_t *ui, const char *title, const char *body, const char *hint)
{
    if (!ui) return;
    set_row(ui->ending_title, title);
    set_row(ui->ending_body, body);
    set_row(ui->ending_hint, hint);
}

void steins_ui_confirm(steins_ui_t *ui, const char *text, const char *hint)
{
    if (!ui) return;
    set_row(ui->confirm_text, text);
    set_row(ui->confirm_hint, hint);
}
