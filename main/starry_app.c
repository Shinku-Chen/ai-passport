// main/starry_app.c —— 应用状态机:页面切换、按键语义、打字机、自动存档。
#include "starry_app.h"

#include "starry_ui.h"


#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "starry_app";

// 打字机速度(毫秒/字):慢 / 中 / 快。
static const uint32_t SPEED_MS[3] = { 60, 38, 18 };
#define STARRY_BATTERY_PERIOD_MS 30000u
// 自动阅读(长按下):每句停留时长,等这一句完全显示后才开始计时。
#define STARRY_AUTO_LINE_MS 900u
#define STARRY_NOTICE_MS 1600u

starry_layout_t starry_layout_for(const starry_app_t *app)
{
    starry_layout_t layout;
    layout.units_per_line = app->settings.font_large ? STARRY_LARGE_UNITS_PER_LINE
                                                     : STARRY_SMALL_UNITS_PER_LINE;
    layout.lines_per_page = app->settings.font_large ? STARRY_LARGE_LINES_PER_PAGE
                                                     : STARRY_SMALL_LINES_PER_PAGE;
    return layout;
}

static uint32_t speed_ms(const starry_app_t *app)
{
    return SPEED_MS[app->settings.text_speed > 2 ? 1 : app->settings.text_speed];
}

static void set_notice(starry_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = STARRY_NOTICE_MS;
}

// 重新取当前页的正文,并把打字机复位。
static void refresh_text(starry_app_t *app)
{
    const starry_layout_t layout = starry_layout_for(app);
    starry_player_page_text(&app->player, &app->pack, &layout, app->text, sizeof(app->text));
    app->text_total = strlen(app->text);
    app->text_visible = 0;
    app->text_complete = app->text_total == 0;
    app->type_accum_ms = 0;
}

static void goto_page(starry_app_t *app, starry_page_t page)
{
    app->page = page;
    app->drawn_battery = -1;   // 强制下一页重画电量(否则首次进入可能一直空着)
    starry_ui_draw(app, true);
}

static void show_chapter_card(starry_app_t *app)
{
    app->chapter_index = app->player.chapter;
    app->chapter_id = starry_pack_chapter_id(&app->pack, app->player.chapter);
    goto_page(app, STARRY_PAGE_CHAPTER);
}

static void start_chapter(starry_app_t *app, uint16_t chapter)
{
    const starry_layout_t layout = starry_layout_for(app);
    if (!starry_player_start(&app->player, &app->pack, chapter, &layout)) {
        ESP_LOGE(TAG, "章节 %u 打不开", (unsigned)chapter);
        set_notice(app, "剧情数据异常");
        return;
    }
    refresh_text(app);
    app->box_dirty = true;
    goto_page(app, app->player.at_choice ? STARRY_PAGE_CHOICES : STARRY_PAGE_READING);
}

static void load_auto(starry_app_t *app)
{
    starry_save_t save;
    if (!starry_auto_load(&save)) {
        set_notice(app, "没有可续读的进度");
        starry_ui_draw(app, false);
        return;
    }
    const starry_layout_t layout = starry_layout_for(app);
    if (!starry_player_load(&app->player, &app->pack, &save, &layout)) {
        set_notice(app, "存档已失效");
        starry_ui_draw(app, false);
        return;
    }
    refresh_text(app);
    app->box_dirty = true;
    app->drawn_name[0] = '\0';
    goto_page(app, app->player.at_choice ? STARRY_PAGE_CHOICES : STARRY_PAGE_READING);
}

// 推进一句;返回是否停在需要换页的状态。
static void advance(starry_app_t *app)
{
    const starry_layout_t layout = starry_layout_for(app);
    const starry_step_t step = starry_player_advance(&app->player, &app->pack, &layout);
    switch (step) {
    case STARRY_STEP_TEXT:
    case STARRY_STEP_SCENE:
        refresh_text(app);
        if (step == STARRY_STEP_SCENE) {
            // 每次换场景写一次自动存档:深睡唤醒后能接上。
            starry_save_t save;
            starry_save_from_player(&app->player, &save);
            app->have_auto = starry_auto_store(&save);
            app->box_dirty = true;
        }
        goto_page(app, STARRY_PAGE_READING);
        break;
    case STARRY_STEP_CHOICE:
        app->choice_sel = 0;
        goto_page(app, STARRY_PAGE_CHOICES);
        break;
    case STARRY_STEP_CHAPTER:
        refresh_text(app);
        show_chapter_card(app);
        break;
    case STARRY_STEP_ENDING:
        goto_page(app, STARRY_PAGE_ENDING);
        break;
    default:
        set_notice(app, "剧情数据异常");
        starry_ui_draw(app, false);
        break;
    }
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
static void key_tips(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) {
        app->settings.seen_tips = 1;
        starry_settings_store(&app->settings);
        goto_page(app, STARRY_PAGE_TITLE);
    }
}

static void key_title(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) {
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_DOUBLE) {
            app->title_sel = (app->title_sel + delta + STARRY_TITLE_ROWS) % STARRY_TITLE_ROWS;
            starry_ui_redraw_page(app);
        }
        return;
    }
    if (key->btn != BSP_BTN_OK) return;
    if (key->ev == BSP_BTN_LONG) {
        // 长按确定:回到第一次阅读(有自动存档时)。
        load_auto(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    switch (app->title_sel) {
    case 0: start_chapter(app, 0); break;
    case 1: load_auto(app); break;
    case 2: app->slots_saving = false; app->slots_sel = 0; goto_page(app, STARRY_PAGE_SLOTS); break;
    default:
        app->return_page = STARRY_PAGE_TITLE;
        app->settings_sel = 0;
        goto_page(app, STARRY_PAGE_SETTINGS);
        break;
    }
}

// 正文页按键:
//   上/下 短按 = 下一句(打字中则立即显示全文)
//   上/下 长按 = 快进(按住期间一直推进,松手立刻停)
//   确定 短按 = 打开菜单(长按同样是菜单,方便按)
static void key_reading(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;   // 松手就停,不需要再点一下
        return;
    }
    if (key->btn == BSP_BTN_OK) {
        if (key->ev != BSP_BTN_CLICK && key->ev != BSP_BTN_LONG) return;
        app->fast_forward = false;
        app->auto_mode = 0;
        app->menu_sel = 0;
        goto_page(app, STARRY_PAGE_MENU);
        return;
    }
    if (key->ev == BSP_BTN_LONG) {
        if (key->btn == BSP_BTN_DOWN) {
            // 长按下:切换自动阅读(每 900ms 一句),再长按下或用其它键取消。
            app->auto_mode = app->auto_mode ? 0 : 1;
            app->auto_accum_ms = 0;
            app->fast_forward = false;
            starry_ui_redraw_page(app);   // 立刻更新"自动"指示
            return;
        }
        // 长按上:开始快进,松手(BSP_BTN_RELEASE)才停。
        app->fast_forward = true;
        app->auto_mode = 0;
        app->ff_accum_ms = 0;
        if (!app->text_complete) {
            app->text_visible = app->text_total;
            app->text_complete = true;
        }
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    app->fast_forward = false;
    app->auto_mode = 0;
    if (!app->text_complete) {
        app->text_visible = app->text_total;
        app->text_complete = true;
        starry_ui_draw(app, false);
        return;
    }
    advance(app);
}

static void key_choices(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_OK) {
        if (key->ev == BSP_BTN_LONG) {
            app->menu_sel = 0;
            goto_page(app, STARRY_PAGE_MENU);
            return;
        }
        if (key->ev != BSP_BTN_CLICK) return;
        const starry_layout_t layout = starry_layout_for(app);
        if (!starry_player_choose(&app->player, &app->pack, (uint8_t)app->choice_sel, &layout)) {
            set_notice(app, "选项数据异常");
            starry_ui_draw(app, false);
            return;
        }
        refresh_text(app);
        app->box_dirty = true;
        goto_page(app, STARRY_PAGE_READING);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) {
        app->choice_sel = app->choice_sel ? 0 : 1;
        starry_ui_redraw_page(app);
    }
}

static void key_menu(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) {
        if (key->ev != BSP_BTN_CLICK) return;
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->menu_sel = (app->menu_sel + delta + STARRY_MENU_ROWS) % STARRY_MENU_ROWS;
        starry_ui_redraw_page(app);
        return;
    }
    if (key->btn != BSP_BTN_OK) return;
    if (key->ev == BSP_BTN_LONG) {
        goto_page(app, STARRY_PAGE_READING);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    switch (app->menu_sel) {
    case 0: app->slots_saving = true; app->slots_sel = 0; goto_page(app, STARRY_PAGE_SLOTS); break;
    case 1: app->slots_saving = false; app->slots_sel = 0; goto_page(app, STARRY_PAGE_SLOTS); break;
    case 2: {
        const starry_layout_t layout = starry_layout_for(app);
        if (starry_player_skip_chapter(&app->player, &app->pack, &layout)) {
            refresh_text(app);
            app->box_dirty = true;
            show_chapter_card(app);   // 与正常读到换章时一致:先给一张章节卡
        } else {
            set_notice(app, "已经是最后一章");
            starry_ui_redraw_page(app);
        }
        break;
    }
    case 3: goto_page(app, STARRY_PAGE_TITLE); break;
    default: goto_page(app, STARRY_PAGE_READING); break;   // 返回:继续读
    }
}

static void key_settings(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) {
        if (key->ev != BSP_BTN_CLICK) return;
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->settings_sel = (app->settings_sel + delta + STARRY_SETTING_ROWS) % STARRY_SETTING_ROWS;
        starry_ui_redraw_page(app);
        return;
    }
    if (key->btn != BSP_BTN_OK) return;
    if (key->ev == BSP_BTN_LONG) {
        goto_page(app, app->return_page);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    switch (app->settings_sel) {
    case 0:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 3);
        starry_settings_store(&app->settings);
        starry_ui_redraw_page(app);
        break;
    case 1:
        app->settings.font_large = app->settings.font_large ? 0 : 1;
        starry_settings_store(&app->settings);
        refresh_text(app);
        app->box_dirty = true;
        starry_ui_redraw_page(app);
        break;
    case 2:
        app->return_page = STARRY_PAGE_SETTINGS;
        goto_page(app, STARRY_PAGE_TIPS);
        break;
    case 3:
        app->return_page = STARRY_PAGE_SETTINGS;
        goto_page(app, STARRY_PAGE_ABOUT);
        break;
    default: goto_page(app, app->return_page); break;
    }
}

static void key_slots(starry_app_t *app, const starry_key_t *key)
{
    const int count = STARRY_SAVE_SLOTS + 1;
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) {
        if (key->ev != BSP_BTN_CLICK) return;
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->slots_sel = (app->slots_sel + delta + count) % count;
        starry_ui_redraw_page(app);
        return;
    }
    if (key->btn != BSP_BTN_OK) return;
    const bool on_slot = app->slots_sel < STARRY_SAVE_SLOTS;
    if (key->ev == BSP_BTN_LONG) {
        if (on_slot && app->slots_saving) {
            starry_slot_clear((uint8_t)app->slots_sel);
            set_notice(app, "已删除");
        } else {
            app->notice_ms = 0;
        }
        starry_ui_redraw_page(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (!on_slot) {
        app->notice_ms = 0;
        goto_page(app, app->return_page);
        return;
    }
    if (app->slots_saving) {
        starry_save_t save;
        starry_save_from_player(&app->player, &save);
        if (starry_slot_store((uint8_t)app->slots_sel, &save)) set_notice(app, "已保存");
        else set_notice(app, "保存失败");
        starry_ui_redraw_page(app);
        return;
    }
    starry_save_t save;
    if (!starry_slot_load((uint8_t)app->slots_sel, &save)) {
        set_notice(app, "这个存档位是空的");
        starry_ui_redraw_page(app);
        return;
    }
    const starry_layout_t layout = starry_layout_for(app);
    if (!starry_player_load(&app->player, &app->pack, &save, &layout)) {
        set_notice(app, "存档已失效");
        starry_ui_redraw_page(app);
        return;
    }
    refresh_text(app);
    app->box_dirty = true;
    app->drawn_name[0] = '\0';
    goto_page(app, app->player.at_choice ? STARRY_PAGE_CHOICES : STARRY_PAGE_READING);
}

static void key_about(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        goto_page(app, app->return_page);
    }
}

static void key_chapter(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        goto_page(app, app->player.at_choice ? STARRY_PAGE_CHOICES : STARRY_PAGE_READING);
    }
}

static void key_ending(starry_app_t *app, const starry_key_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        goto_page(app, STARRY_PAGE_TITLE);
    }
}

void starry_app_key(starry_app_t *app, const starry_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_PRESS) app->fast_forward = false;
    switch (app->page) {
    case STARRY_PAGE_TIPS: key_tips(app, key); break;
    case STARRY_PAGE_TITLE: key_title(app, key); break;
    case STARRY_PAGE_READING: key_reading(app, key); break;
    case STARRY_PAGE_CHOICES: key_choices(app, key); break;
    case STARRY_PAGE_MENU: key_menu(app, key); break;
    case STARRY_PAGE_SETTINGS: key_settings(app, key); break;
    case STARRY_PAGE_SLOTS: key_slots(app, key); break;
    case STARRY_PAGE_ABOUT: key_about(app, key); break;
    case STARRY_PAGE_CHAPTER: key_chapter(app, key); break;
    case STARRY_PAGE_ENDING: key_ending(app, key); break;
    default: break;
    }
}

void starry_app_tick(starry_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;
    // 自动阅读/快进是应用自己在推进画面,不该算"人没动":否则读着读着背光会暗、
    // 屏幕会熄、最后还会进深睡,把自动阅读打断。
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;

    if (app->notice_ms) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) starry_ui_redraw_page(app);
    }

    // 快进:每 FAST_FORWARD_MS 推进一句。
    if (app->page == STARRY_PAGE_READING && app->fast_forward) {
        app->ff_accum_ms += elapsed_ms;
        while (app->ff_accum_ms >= 90) {
            app->ff_accum_ms -= 90;
            if (!app->text_complete) {
                app->text_visible = app->text_total;
                app->text_complete = true;
            }
            advance(app);
            if (app->page != STARRY_PAGE_READING) {
                app->fast_forward = false;
                break;
            }
        }
    }

    // 自动阅读:等这一句完全显示后再等 STARRY_AUTO_LINE_MS,然后推进下一句。
    // 章节卡也自动翻过去(不打断连续阅读);选项和结局停下等玩家。
    if (app->auto_mode &&
        (app->page == STARRY_PAGE_READING || app->page == STARRY_PAGE_CHAPTER)) {
        const bool waiting_typing = app->page == STARRY_PAGE_READING && !app->text_complete;
        if (waiting_typing) {
            app->auto_accum_ms = 0;
        } else {
            app->auto_accum_ms += elapsed_ms;
            if (app->auto_accum_ms >= STARRY_AUTO_LINE_MS) {
                app->auto_accum_ms = 0;
                if (app->page == STARRY_PAGE_CHAPTER) {
                    goto_page(app, app->player.at_choice ? STARRY_PAGE_CHOICES
                                                         : STARRY_PAGE_READING);
                } else {
                    advance(app);
                    if (app->page == STARRY_PAGE_CHOICES || app->page == STARRY_PAGE_ENDING) {
                        app->auto_mode = 0;   // 要玩家做决定/走完了,停下
                    }
                }
            }
        }
    }

    if (app->page == STARRY_PAGE_READING && !app->text_complete) {
        const uint32_t ms = speed_ms(app);
        app->type_accum_ms += elapsed_ms;
        while (!app->text_complete && app->type_accum_ms >= ms) {
            app->type_accum_ms -= ms;
            const size_t next = starry_utf8_next_boundary(app->text, app->text_total,
                                                          app->text_visible);
            app->text_visible = next;
            if (app->text_visible >= app->text_total) app->text_complete = true;
        }
        // 打字机直接重画正文三行(代价很小),不区分脏行。
        starry_ui_draw_text(app);
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= STARRY_BATTERY_PERIOD_MS) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            if (app->page == STARRY_PAGE_READING || app->page == STARRY_PAGE_TITLE ||
                app->page == STARRY_PAGE_CHOICES) {
                starry_render_battery(&app->render, soc);
                app->drawn_battery = soc;
            }
        }
    }
}

uint32_t starry_app_idle_ms(const starry_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void starry_app_before_sleep(starry_app_t *app)
{
    if (!app) return;
    if (app->page == STARRY_PAGE_READING || app->page == STARRY_PAGE_CHOICES) {
        starry_save_t save;
        starry_save_from_player(&app->player, &save);
        app->have_auto = starry_auto_store(&save);
    }
}

bool starry_app_init(starry_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                     const uint8_t *font_small, uint32_t font_small_size,
                     const uint8_t *font_large, uint32_t font_large_size,
                     const starry_app_buffers_t *buffers)
{
    if (!app || !pack_data || !font_small || !font_large || !buffers) return false;
    memset(app, 0, sizeof(*app));
    if (!starry_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "资源包不合法(魔数/版本/长度)");
        return false;
    }
    if (!starry_font_open(&app->font_small, font_small, font_small_size) ||
        !starry_font_open(&app->font_large, font_large, font_large_size)) {
        ESP_LOGE(TAG, "字形包不合法");
        return false;
    }
    ESP_LOGI(TAG, "字体: 小 %upx 行高%u 基线%d 魔数%.8s / 大 %upx 行高%u 基线%d 魔数%.8s",
             (unsigned)app->font_small.px, (unsigned)app->font_small.line_height,
             (int)app->font_small.ascent, (const char *)app->font_small.blob,
             (unsigned)app->font_large.px, (unsigned)app->font_large.line_height,
             (int)app->font_large.ascent, (const char *)app->font_large.blob);
    ESP_LOGI(TAG, "资源包: 章节 %u / 场景 %u / 对白 %u / 背景 %u / 立绘 %u",
             (unsigned)app->pack.chapter_count, (unsigned)app->pack.scene_count,
             (unsigned)app->pack.dialogue_count, (unsigned)app->pack.bg_count,
             (unsigned)app->pack.fg_count);

    const starry_render_buffers_t render_buffers = {
        .sprite = buffers->sprite,
        .box = buffers->box,
        .strip = buffers->strip,
        .hold = buffers->hold,
        .name_bg = buffers->name_bg,
        .chapter_bg = buffers->chapter_bg,
        .jpeg_work = buffers->jpeg_work,
        .jpeg_work_size = buffers->jpeg_work_size,
    };
    if (!starry_render_init(&app->render, &app->pack, bsp_display_panel(), bsp_display_io(),
                            &app->font_small, &app->font_large, &render_buffers)) {
        return false;
    }

    starry_settings_default(&app->settings);
    starry_settings_load(&app->settings);
    starry_save_t auto_save;
    app->have_auto = starry_auto_load(&auto_save);
    app->battery_percent = bsp_battery_soc();
    app->drawn_battery = -1;   // 第一页也要画出电量
    app->notice_ms = 0;
    app->page = app->settings.seen_tips ? STARRY_PAGE_TITLE : STARRY_PAGE_TIPS;
    starry_ui_draw(app, true);
    return true;
}
