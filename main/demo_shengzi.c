// main/demo_shengzi.c — 生字卡片识记 demo。
// 三个模式: BROWSE(浏览字卡) / SELFTEST(自测认识/不认识) / SPELL(看拼音猜字)。
// 用 UP/DOWN 长按切模式; OK 短按在自测/拼读里"揭晓"。
// "已认识"标记持久化到 NVS(sz_data)。
#include "demo.h"
#include "sz_data.h"
#include "sz_font.h"
#include "bsp_display.h"   // bsp_lvgl_lock/unlock
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_random.h"   // esp_random
#include <string.h>

static const char *TAG = "demo_shengzi";

// 模式
typedef enum { MODE_BROWSE = 0, MODE_SELFTEST, MODE_SPELL, MODE_COUNT } sz_mode_t;
static const char *MODE_NAME[MODE_COUNT] = { "BROWSE", "TEST", "SPELL" };

static lv_obj_t *s_scr, *s_hanzi, *s_pinyin, *s_info, *s_mascot;
static sz_mode_t s_mode;
static int s_idx;            // 当前字索引
static bool s_revealed;      // 自测/拼读是否已揭晓

// 屏幕布局(240x320):
// 标题栏约 y0-46; 拼音 y58; 大字卡片区 y78-218; 底部信息 y255-290
static void build_ui(void)
{
    s_scr = ui_pixel_screen_create("SHENGZI");

    // 拼音行(大字上方)
    s_pinyin = ui_pixel_label(s_scr, "", &sz_small, UI_INK);
    lv_obj_set_style_text_align(s_pinyin, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_pinyin, 0, 52);
    lv_obj_set_width(s_pinyin, 240);

    // 大字卡片区(拼音下方, 近全屏宽让大汉字占满)
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 40, 72, 160, 160, UI_PAPER);
    s_hanzi = lv_label_create(panel);
    lv_obj_set_style_text_font(s_hanzi, &sz_big, 0);
    lv_obj_set_style_text_color(s_hanzi, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_hanzi, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_hanzi);

    // 底部信息: 模式 + 已认识 N/总
    s_info = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -16);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);
}

static void refresh(void)
{
    // 大字
    lv_label_set_text(s_hanzi, sz_cards[s_idx].hanzi);

    // 拼音: 揭晓才显示(自测/拼读未揭晓时显示占位)
    switch (s_mode) {
    case MODE_BROWSE:
        lv_label_set_text(s_pinyin, sz_cards[s_idx].pinyin);
        break;
    case MODE_SELFTEST:
    case MODE_SPELL:
        lv_label_set_text(s_pinyin, s_revealed ? sz_cards[s_idx].pinyin : "?");
        break;
    default:
        break;
    }

    // 底部信息
    lv_label_set_text_fmt(s_info, "%s  %u/%u  KNOWN:%u",
                          MODE_NAME[s_mode], (unsigned)(s_idx + 1),
                          (unsigned)sz_card_count, (unsigned)sz_known_count());
}

static void next_mode(int dir)
{
    s_mode = (sz_mode_t)((s_mode + dir + MODE_COUNT) % MODE_COUNT);
    s_revealed = false;
}

// UP: 顺序上一个
static void goto_prev(void)
{
    s_idx = (s_idx + sz_card_count - 1) % sz_card_count;
    s_revealed = false;
}

// DOWN: 随机跳到另一张卡(避免连续翻到同一张)
static void goto_random(void)
{
    int n = sz_card_count;
    if (n > 1) {
        int r = esp_random() % n;
        if (r == s_idx) r = (r + 1) % n;   // 避免停在原卡
        s_idx = r;
    }
    s_revealed = false;
}

void demo_shengzi_enter(void)
{
    s_mode = MODE_BROWSE;
    s_idx = 0;
    s_revealed = false;

    // 初始化字卡数据 + NVS 认识标记(幂等)
    sz_data_init();

    build_ui();
    refresh();
    lv_screen_load(s_scr);

    ESP_LOGI(TAG, "生字卡进入: %u 字, 模式 BROWSE", (unsigned)sz_card_count);
}

void demo_shengzi_exit(void)
{
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL;
                 s_hanzi = s_pinyin = s_info = s_mascot = NULL; }
}

void demo_shengzi_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 长按 UP/DOWN = 切模式
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        next_mode(btn == BSP_BTN_UP ? -1 : 1);
        ui_pixel_mascot_jump(s_mascot);
        refresh();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    switch (s_mode) {
    case MODE_BROWSE:
        if (btn == BSP_BTN_UP)   { goto_prev(); ui_pixel_mascot_jump(s_mascot); }
        else if (btn == BSP_BTN_DOWN) { goto_random(); ui_pixel_mascot_jump(s_mascot); }
        refresh();
        break;

    case MODE_SELFTEST:
        if (!s_revealed) {
            // 未揭晓: UP/DOWN 翻字, OK 揭晓
            if (btn == BSP_BTN_UP)   { goto_prev(); }
            else if (btn == BSP_BTN_DOWN) { goto_random(); }
            else if (btn == BSP_BTN_OK) { s_revealed = true; ui_pixel_mascot_jump(s_mascot); }
            refresh();
        } else {
            // 已揭晓: UP=认识, DOWN=不认识, 都随机推进下一字
            if (btn == BSP_BTN_UP)   { sz_set_known(s_idx, true);  goto_random(); }
            else if (btn == BSP_BTN_DOWN) { sz_set_known(s_idx, false); goto_random(); }
            refresh();
        }
        break;

    case MODE_SPELL:
        if (!s_revealed) {
            // 看拼音猜字: OK 揭晓
            if (btn == BSP_BTN_OK) { s_revealed = true; ui_pixel_mascot_jump(s_mascot); }
            else if (btn == BSP_BTN_DOWN) { goto_random(); }
            else if (btn == BSP_BTN_UP)   { goto_prev(); }
            refresh();
        } else {
            if (btn == BSP_BTN_OK) { s_revealed = false; goto_random(); }
            refresh();
        }
        break;

    default:
        break;
    }
}
