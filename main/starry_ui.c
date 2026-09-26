// main/starry_ui.c —— 页面绘制实现。
//
// 每页画什么在这里,剧情推进与按键语义在 starry_app.c。列表页整体重画的代价很小
// (纯色底 + 几行字),所以只做"画面/文本框去重",不做逐行脏矩形。
#include "starry_ui.h"

#include "starry_gfx.h"
#include "starry_model.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "starry_ui";

// 缺字点名:字形包少一个码位就会画出空白,真机上很难判断,这里点名告警。
static void check_glyphs(const starry_font_t *font, const char *text, const char *what)
{
    if (!font || !text) return;
    static int reported;
    const size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        size_t next = 0;
        const uint32_t cp = starry_utf8_decode(text, len, pos, &next);
        pos = next > pos ? next : pos + 1;
        if (cp == '\n' || cp == ' ' || cp == 0) continue;
        starry_glyph_t gl;
        if (!starry_font_glyph(font, cp, &gl) && reported++ < 8) {
            ESP_LOGW(TAG, "%s 缺字形 U+%04X(字形包未覆盖)", what, (unsigned)cp);
        }
    }
}

static const starry_font_t *text_font(const starry_app_t *app)
{
    return app->settings.font_large ? app->render.font_large : app->render.font_small;
}

static const char *speed_label(const starry_app_t *app)
{
    switch (app->settings.text_speed) {
    case 0: return "慢";
    case 2: return "快";
    default: return "中";
    }
}

// 立绘可见性规则在模型层(starry_player_visible_sprite),这里只做转发。
static uint16_t speaking_sprite(const starry_app_t *app)
{
    return starry_player_visible_sprite(&app->player, &app->pack);
}

static void notice_over_art(starry_app_t *app)
{
    starry_render_notice(&app->render, app->notice, app->render.font_small, true);
}

// ---------------------------------------------------------------------------
// 阅读页与它的浮层
// ---------------------------------------------------------------------------
void starry_ui_draw_text(starry_app_t *app)
{
    const starry_font_t *font = text_font(app);
    char name[STARRY_NAME_MAX] = { 0 };
    starry_player_speaker(&app->player, &app->pack, name, sizeof(name));
    if (app->box_dirty || strcmp(name, app->drawn_name) != 0) {
        starry_render_name(&app->render, name, app->drawn_name, font);
        snprintf(app->drawn_name, sizeof(app->drawn_name), "%s", name);
        app->box_dirty = false;
    }
    if (app->text_visible > app->text_total) app->text_visible = app->text_total;
    starry_render_body(&app->render, app->text, app->text_visible, font,
                       starry_layout_for(app).units_per_line);
}

static void draw_reading(starry_app_t *app, bool force)
{
    if (force) starry_render_box_clear(&app->render);   // 从菜单/列表页回来:先整块重刷

    starry_render_auto(&app->render, app->auto_mode != 0);   // 自动阅读指示
    // 先把当前章号登记给渲染层:整屏重画时它会被直接画进条带,不会先闪一下旧章号。
    const uint16_t chapter_id = starry_pack_chapter_id(&app->pack, app->player.chapter);
    char label[24];
    snprintf(label, sizeof(label), "第%u章", (unsigned)chapter_id);
    starry_render_chapter_set(&app->render, label);

    const bool scene_changed = starry_render_scene(&app->render, app->player.bg,
                                                   speaking_sprite(app));
    if (scene_changed || force) app->box_dirty = true;

    // 左上角章节标签:底图在场景合成时存好,只有换章才重画那一条。
    if (scene_changed || force || chapter_id != app->drawn_chapter) {
        starry_render_chapter(&app->render, label, app->render.font_small);
        app->drawn_chapter = chapter_id;
    }

    starry_ui_draw_text(app);
    if (app->battery_percent != app->drawn_battery) {
        starry_render_battery(&app->render, app->battery_percent);
        app->drawn_battery = app->battery_percent;
    }
}

static void draw_title(starry_app_t *app)
{
    starry_render_clear_overlays(&app->render);   // 标题页不该留着上一章的角色名/章节标签
    starry_render_scene(&app->render, STARRY_BG_TITLE, STARRY_NONE);
    static const char *const labels[STARRY_TITLE_ROWS] = { "开始阅读", "继续阅读", "读取存档",
                                                           "设置" };
    starry_row_t rows[STARRY_TITLE_ROWS];
    for (int i = 0; i < STARRY_TITLE_ROWS; ++i) {
        rows[i] = (starry_row_t){ .label = labels[i], .value = NULL,
                                  .dim = (i == 1 && !app->have_auto) ? 1 : 0 };
    }
    starry_render_box_list(&app->render, rows, STARRY_TITLE_ROWS, app->title_sel,
                           app->render.font_small);
    if (app->battery_percent != app->drawn_battery) {
        starry_render_battery(&app->render, app->battery_percent);
        app->drawn_battery = app->battery_percent;
    }
    if (app->notice_ms) notice_over_art(app);
}

static void draw_menu(starry_app_t *app)
{
    starry_render_scene(&app->render, app->player.bg, speaking_sprite(app));
    static const char *const labels[STARRY_MENU_ROWS] = { "保存进度", "读取存档", "跳过章节",
                                                          "返回标题", "返回" };
    starry_row_t rows[STARRY_MENU_ROWS];
    for (int i = 0; i < STARRY_MENU_ROWS; ++i) {
        rows[i] = (starry_row_t){ .label = labels[i], .value = NULL, .dim = 0 };
    }
    // 菜单盖在正文上:清掉正文浮层,免得新解码的画面把文字烘进菜单底下。
    starry_render_body(&app->render, "", 0, app->render.font_small,
                       STARRY_SMALL_UNITS_PER_LINE);
    starry_render_box_list(&app->render, rows, STARRY_MENU_ROWS, app->menu_sel,
                           app->render.font_small);
    if (app->notice_ms) notice_over_art(app);
}

static void draw_choices(starry_app_t *app)
{
    starry_render_scene(&app->render, app->player.bg, speaking_sprite(app));
    char text[STARRY_TEXT_BUFFER] = { 0 };
    const starry_layout_t layout = starry_layout_for(app);
    starry_player_page_text(&app->player, &app->pack, &layout, text, sizeof(text));

    starry_chapter_t ch;
    starry_pack_chapter(&app->pack, app->player.chapter, &ch);
    starry_scene_t sc;
    starry_pack_scene(&app->pack, (uint16_t)(ch.first_scene + app->player.scene), &sc);
    char first[128] = { 0 };
    char second[128] = { 0 };
    starry_pack_name(&app->pack, sc.choice_name[0], first, sizeof(first));
    starry_pack_name(&app->pack, sc.choice_name[1], second, sizeof(second));
    check_glyphs(app->render.font_small, first, "选项");
    check_glyphs(app->render.font_small, second, "选项");
    const char *items[2] = { first, second };
    // 选项页同理:正文浮层清掉,画面重画时不会把上一句正文烘进去。
    starry_render_body(&app->render, "", 0, app->render.font_small,
                       STARRY_SMALL_UNITS_PER_LINE);
    starry_render_choices(&app->render, text, items, (int)sc.choice_count, app->choice_sel,
                          app->render.font_small);
    if (app->battery_percent != app->drawn_battery) {
        starry_render_battery(&app->render, app->battery_percent);
        app->drawn_battery = app->battery_percent;
    }
}

// ---------------------------------------------------------------------------
// 整页界面
// ---------------------------------------------------------------------------
static void draw_settings(starry_app_t *app)
{
    starry_row_t rows[STARRY_SETTING_ROWS];
    rows[0] = (starry_row_t){ .label = "文字速度", .value = speed_label(app), .dim = 0 };
    rows[1] = (starry_row_t){ .label = "文字大小", .value = app->settings.font_large ? "大" : "小",
                              .dim = 0 };
    rows[2] = (starry_row_t){ .label = "操作说明", .value = NULL, .dim = 0 };
    rows[3] = (starry_row_t){ .label = "关于", .value = NULL, .dim = 0 };
    rows[4] = (starry_row_t){ .label = "返回", .value = NULL, .dim = 0 };
    starry_render_full_list(&app->render, "设置", rows, STARRY_SETTING_ROWS, app->settings_sel,
                            app->render.font_small, STARRY_COL_PANEL);
    starry_render_battery_solid(&app->render, app->battery_percent, STARRY_COL_PANEL);
}

static void draw_slots(starry_app_t *app)
{
    static char labels[STARRY_SAVE_SLOTS + 1][16];
    static char values[STARRY_SAVE_SLOTS + 1][40];
    starry_row_t rows[STARRY_SAVE_SLOTS + 1];
    int count = 0;
    for (int i = 0; i < STARRY_SAVE_SLOTS; ++i) {
        starry_save_t save;
        snprintf(labels[i], sizeof(labels[i]), "存档 %d", i + 1);
        rows[i].label = labels[i];
        if (starry_slot_load((uint8_t)i, &save)) {
            snprintf(values[i], sizeof(values[i]), "第%u章 %u/%u",
                     (unsigned)starry_pack_chapter_id(&app->pack, save.chapter),
                     (unsigned)(save.scene + 1), (unsigned)save.dialogue);
            rows[i].dim = 0;
        } else {
            snprintf(values[i], sizeof(values[i]), "空");
            rows[i].dim = 1;
        }
        rows[i].value = values[i];
        count++;
    }
    snprintf(labels[STARRY_SAVE_SLOTS], sizeof(labels[0]), "返回");
    rows[count] = (starry_row_t){ .label = labels[STARRY_SAVE_SLOTS], .value = NULL, .dim = 0 };
    count++;

    starry_render_full_list(&app->render, app->slots_saving ? "保存进度" : "读取存档", rows,
                            count, app->slots_sel, app->render.font_small, STARRY_COL_PANEL);
    starry_render_battery_solid(&app->render, app->battery_percent, STARRY_COL_PANEL);
    if (app->notice_ms) {
        starry_render_notice(&app->render, app->notice, app->render.font_small, false);
    }
}

static void draw_about(starry_app_t *app)
{
    static const char *const lines[] = {
        "原作：星空列车与白的旅行",
        "手环移植：@liuyuze61 & @EC",
        "本移植：AI Passport 三键阅读器",
        "个人同人作品，请支持正版",
    };
    starry_row_t rows[sizeof(lines) / sizeof(lines[0]) + 1];
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        rows[i] = (starry_row_t){ .label = lines[i], .value = NULL, .dim = 1 };
    }
    const int count = (int)(sizeof(lines) / sizeof(lines[0]));
    rows[count] = (starry_row_t){ .label = "确定：返回", .value = NULL, .dim = 0 };
    starry_render_full_list(&app->render, "关于", rows, count + 1, -1, app->render.font_small,
                            STARRY_COL_PANEL);
    starry_render_battery_solid(&app->render, app->battery_percent, STARRY_COL_PANEL);
}

static void draw_tips(starry_app_t *app)
{
    static const char *const lines[] = {
        "上 / 下 短按：下一句",
        "长按上：快进（松开停）",
        "长按下：自动阅读（900ms）",
        "确定 短按：打开菜单",
        "设置里可调文字速度与大小",
        "制作：@liuyuze61 & @EC",
    };
    starry_row_t rows[sizeof(lines) / sizeof(lines[0]) + 1];
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        rows[i] = (starry_row_t){ .label = lines[i], .value = NULL, .dim = 1 };
    }
    const int count = (int)(sizeof(lines) / sizeof(lines[0]));
    rows[count] = (starry_row_t){ .label = "按确定开始阅读", .value = NULL, .dim = 0 };
    starry_render_full_list(&app->render, "操作说明", rows, count + 1, -1,
                            app->render.font_small, STARRY_COL_PANEL);
    starry_render_battery_solid(&app->render, app->battery_percent, STARRY_COL_PANEL);
}

static void draw_chapter(starry_app_t *app)
{
    char title[32];
    snprintf(title, sizeof(title), "第%u章", (unsigned)app->chapter_id);
    starry_render_center(&app->render, title, "按确定继续", app->render.font_large,
                         STARRY_COL_PANEL);
}

static void draw_ending(starry_app_t *app)
{
    char name[64] = { 0 };
    starry_pack_name(&app->pack, app->player.end_name, name, sizeof(name));
    starry_render_center(&app->render, name[0] ? name : "FIN", "感谢阅读 · 按确定返回标题",
                         app->render.font_large, STARRY_COL_PANEL);
}

void starry_ui_draw(starry_app_t *app, bool force)
{
    if (!app) return;
    switch (app->page) {
    case STARRY_PAGE_TIPS: draw_tips(app); break;
    case STARRY_PAGE_TITLE: draw_title(app); break;
    case STARRY_PAGE_READING: draw_reading(app, force); break;
    case STARRY_PAGE_CHOICES: draw_choices(app); break;
    case STARRY_PAGE_MENU: draw_menu(app); break;
    case STARRY_PAGE_SETTINGS: draw_settings(app); break;
    case STARRY_PAGE_SLOTS: draw_slots(app); break;
    case STARRY_PAGE_ABOUT: draw_about(app); break;
    case STARRY_PAGE_CHAPTER: draw_chapter(app); break;
    case STARRY_PAGE_ENDING: draw_ending(app); break;
    default: draw_title(app); break;
    }
}

void starry_ui_redraw_page(starry_app_t *app)
{
    starry_ui_draw(app, false);
}
