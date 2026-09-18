// main/c4_ui.c —— 四子棋界面实现:设置屏 + 棋盘屏,全部自绘(不复用 demo 的像素风壳)。
//
// 视觉设计(横屏 320x240):
//   深蓝黑底 + 圆角屏幕遮罩由 BSP 统一处理,这里只画内容;
//   顶部 24px 状态条:左侧状态短句、右上角电量;
//   下面整块留给 7x6 棋盘:孔大、棋子小,让棋盘本身显得更大更透气。
#include "c4_ui.h"

#include "bsp_battery.h"
#include "c4_layout.h"
#include "lvgl.h"

#include <string.h>

#define C4_UI_BG           0x0E1420
#define C4_UI_FRAME        0x081A33
#define C4_UI_BOARD        0x1D4E9E
#define C4_UI_BOARD_EDGE   0x3F82DE
#define C4_UI_HOLE         0x07101E
#define C4_UI_P1           0xFFC53D
#define C4_UI_P1_DARK      0x9A6A00
#define C4_UI_P2           0xFF5A4E
#define C4_UI_P2_DARK      0x96271F
#define C4_UI_TEXT         0xEAF1FF
#define C4_UI_MUTED        0x8FA3BF
#define C4_UI_BAT_OK       0x7FD98F
#define C4_UI_BAT_LOW      0xFF6B6B

// 设置屏配色
#define C4_UI_ROW_BG       0x16213A
#define C4_UI_ROW_SEL_BG   0x27518F
#define C4_UI_ROW_OFF_BG   0x11162A
#define C4_UI_ROW_OFF_TXT  0x4A5670
#define C4_UI_START_BG     0x2A2410
#define C4_UI_ROW_TEXT     0xEAF1FF

#define C4_UI_DROP_MS      200

// 设置屏布局
#define C4_UI_MENU_ROW_X   24
#define C4_UI_MENU_ROW_W   272
#define C4_UI_MENU_ROW_H   26
#define C4_UI_MENU_ROW_Y0  42
#define C4_UI_MENU_ROW_DY  34

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_board_scr;
static lv_obj_t *s_board;
static lv_obj_t *s_status;
static lv_obj_t *s_bat_menu;
static lv_obj_t *s_bat_board;
static lv_obj_t *s_menu_hint;
static lv_obj_t *s_menu_rows[C4_MENU_ROW_COUNT];
static lv_obj_t *s_menu_labels[C4_MENU_ROW_COUNT];
static lv_obj_t *s_menu_values[C4_MENU_ROW_COUNT];
static lv_timer_t *s_bat_timer;

static c4_layout_t s_layout;
static const c4_game_t *s_game;   // 控制器持有,界面只读
static int s_cursor;
static bool s_cursor_on;
static c4_preview_t s_preview;

static bool s_drop_active;
static int s_drop_col, s_drop_row, s_drop_offset;
static lv_anim_t s_drop_anim;

static uint32_t disc_color(uint8_t player)
{
    return (player == C4_P1) ? C4_UI_P1 : C4_UI_P2;
}

static uint32_t disc_edge_color(uint8_t player)
{
    return (player == C4_P1) ? C4_UI_P1_DARK : C4_UI_P2_DARK;
}

static void style_plain(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    style_plain(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C4_UI_BG), 0);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void set_text_color(lv_obj_t *label, uint32_t color)
{
    if (label) lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// ——— 绘图小工具 ———————————————————————————————————————————————

static void fill_rounded(lv_layer_t *layer, const c4_rect_t *rect, uint32_t color,
                         int32_t radius, lv_opa_t opa)
{
    lv_area_t area = { rect->x, rect->y, rect->x + rect->w - 1, rect->y + rect->h - 1 };
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = opa;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, &area);
}

static void fill_circle(lv_layer_t *layer, int cx, int cy, int diameter, uint32_t color,
                        lv_opa_t opa)
{
    if (diameter <= 0) return;
    const c4_rect_t rect = { cx - diameter / 2, cy - diameter / 2, diameter, diameter };
    fill_rounded(layer, &rect, color, LV_RADIUS_CIRCLE, opa);
}

static void stroke_circle(lv_layer_t *layer, int cx, int cy, int diameter, int width,
                          uint32_t color)
{
    if (diameter <= 0 || width <= 0) return;

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = cx;
    dsc.center.y = cy;
    dsc.radius = diameter / 2;
    dsc.start_angle = 0;
    dsc.end_angle = 359;   // lv_draw_arc 对 start == end 直接返回,画整圆要用 359
    dsc.width = width;
    dsc.color = lv_color_hex(color);
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = 1;
    lv_draw_arc(layer, &dsc);
}

// ——— 棋盘自绘 ———————————————————————————————————————————————

static void board_draw(lv_event_t *event)
{
    if (!s_game) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    // 孔与棋子同径:棋子落下正好填满孔。
    const int disc_d = c4_layout_disc_diameter(&s_layout);

    // 外框:深色描边 + 亮色内边,给棋盘一点厚度
    c4_rect_t frame = s_layout.board;
    frame.x -= 2; frame.y -= 2; frame.w += 4; frame.h += 4;
    fill_rounded(layer, &frame, C4_UI_FRAME, 10, LV_OPA_COVER);

    c4_rect_t inner = s_layout.board;
    inner.x -= 1; inner.y -= 1; inner.w += 2; inner.h += 2;
    fill_rounded(layer, &inner, C4_UI_BOARD_EDGE, 9, LV_OPA_COVER);

    fill_rounded(layer, &s_layout.board, C4_UI_BOARD, 8, LV_OPA_COVER);

    // 孔 + 棋子
    const uint8_t turn = s_game->turn;
    for (int col = 0; col < C4_COLS; col++) {
        for (int row = 0; row < C4_ROWS; row++) {
            const int cx = c4_layout_col_center_x(&s_layout, col);
            const int cy = c4_layout_row_center_y(&s_layout, row);

            fill_circle(layer, cx, cy, disc_d, C4_UI_HOLE, LV_OPA_COVER);

            const uint8_t cell = c4_cell_get(s_game, col, row);
            const bool dropping = s_drop_active && s_drop_col == col && s_drop_row == row;
            if (cell == C4_EMPTY && !dropping) continue;

            if (dropping) {
                // s_drop_offset 从「负的落程」渐变到 0:圆盘从该列最上一行滑到落点。
                const int drop_y = cy + s_drop_offset;
                fill_circle(layer, cx, drop_y, disc_d, disc_edge_color(cell), LV_OPA_COVER);
                fill_circle(layer, cx, drop_y, disc_d - 3, disc_color(cell), LV_OPA_COVER);
                continue;
            }

            fill_circle(layer, cx, cy, disc_d, disc_edge_color(cell), LV_OPA_COVER);
            fill_circle(layer, cx, cy, disc_d - 3, disc_color(cell), LV_OPA_COVER);

            if (c4_is_winning_cell(s_game, col, row)) {
                stroke_circle(layer, cx, cy, disc_d, 2, C4_UI_TEXT);
            }
        }
    }

    // 光标:整列半透明提亮。压在孔和棋子上,否则只在孔间隙里看得到。
    if (s_cursor_on && s_game->status == C4_ONGOING) {
        const c4_rect_t band = { s_layout.board.x + s_cursor * s_layout.cell,
                                 s_layout.board.y, s_layout.cell, s_layout.board.h };
        fill_rounded(layer, &band, C4_UI_TEXT, 7, LV_OPA_20);
    }

    // 预览盘:按设置画在真实落点,或只画在该列最上一行。
    if (s_cursor_on && s_game->status == C4_ONGOING) {
        int row = -1;
        if (s_preview == C4_PREVIEW_TOP) {
            if (c4_cell_get(s_game, s_cursor, C4_ROWS - 1) == C4_EMPTY) row = C4_ROWS - 1;
        } else {
            row = c4_landing_row(s_game, s_cursor);
        }

        if (row >= 0) {
            const int cx = c4_layout_col_center_x(&s_layout, s_cursor);
            const int cy = c4_layout_row_center_y(&s_layout, row);
            fill_circle(layer, cx, cy, disc_d, disc_color(turn), LV_OPA_30);
            stroke_circle(layer, cx, cy, disc_d, 2, disc_color(turn));
        }
    }
}

static void drop_anim_exec(void *var, int32_t value)
{
    (void)var;
    s_drop_offset = (int)value;
    if (s_board) lv_obj_invalidate(s_board);
}

static void drop_anim_completed(lv_anim_t *anim)
{
    (void)anim;
    s_drop_active = false;
    if (s_board) lv_obj_invalidate(s_board);
}

static void drop_cancel(void)
{
    if (!s_drop_active) return;
    lv_anim_delete(&s_drop_offset, drop_anim_exec);
    s_drop_active = false;
}

// ——— 电池 ———————————————————————————————————————————————

static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    const int soc = bsp_battery_soc();

    uint32_t color = C4_UI_BAT_OK;
    char text[8];
    if (soc < 0) {
        strcpy(text, "--");
        color = C4_UI_MUTED;
    } else {
        lv_snprintf(text, sizeof(text), "%d%%", soc);
        if (soc < 20) color = C4_UI_BAT_LOW;
    }

    lv_obj_t *labels[2] = { s_bat_menu, s_bat_board };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (!labels[i]) continue;
        lv_label_set_text(labels[i], text);
        set_text_color(labels[i], color);
    }
}

void c4_ui_battery_start(void)
{
    if (!s_bat_timer) s_bat_timer = lv_timer_create(battery_tick, 2000, NULL);
    battery_tick(NULL);
}

// ——— 设置屏 ———————————————————————————————————————————————

static void build_menu_screen(void)
{
    s_menu_scr = make_screen();

    lv_obj_t *title = make_label(s_menu_scr, &lv_font_montserrat_20, C4_UI_P1);
    lv_label_set_text(title, "CONNECT 4");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_bat_menu = make_label(s_menu_scr, &lv_font_montserrat_14, C4_UI_BAT_OK);
    lv_label_set_text(s_bat_menu, "--");
    lv_obj_align(s_bat_menu, LV_ALIGN_TOP_RIGHT, -16, 8);

    for (int row = 0; row < C4_MENU_ROW_COUNT; row++) {
        lv_obj_t *panel = lv_obj_create(s_menu_scr);
        style_plain(panel);
        lv_obj_set_style_radius(panel, 7, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(C4_UI_ROW_BG), 0);
        lv_obj_set_size(panel, C4_UI_MENU_ROW_W, C4_UI_MENU_ROW_H);
        lv_obj_set_pos(panel, C4_UI_MENU_ROW_X,
                       C4_UI_MENU_ROW_Y0 + row * C4_UI_MENU_ROW_DY);
        s_menu_rows[row] = panel;

        s_menu_labels[row] = make_label(panel, &lv_font_montserrat_14, C4_UI_MUTED);
        lv_label_set_text(s_menu_labels[row], c4_settings_row_label(row));
        lv_obj_align(s_menu_labels[row], LV_ALIGN_LEFT_MID, 12, 0);

        s_menu_values[row] = make_label(panel, &lv_font_montserrat_14, C4_UI_ROW_TEXT);
        lv_obj_align(s_menu_values[row], LV_ALIGN_RIGHT_MID, -12, 0);
    }

    s_menu_hint = make_label(s_menu_scr, &lv_font_montserrat_14, C4_UI_MUTED);
    lv_label_set_text(s_menu_hint, "UP/DOWN SELECT   OK CHANGE");
    lv_obj_align(s_menu_hint, LV_ALIGN_TOP_MID, 0, 196);
}

void c4_ui_render_menu(const c4_settings_t *settings)
{
    if (!settings) return;

    for (int row = 0; row < C4_MENU_ROW_COUNT; row++) {
        const bool enabled = c4_settings_row_enabled(settings, row);
        const bool selected = (settings->row == row);
        const bool is_start = (row == C4_MENU_ROW_START);

        uint32_t bg = C4_UI_ROW_BG;
        if (!enabled) bg = C4_UI_ROW_OFF_BG;
        else if (is_start) bg = selected ? C4_UI_P1 : C4_UI_START_BG;
        else if (selected) bg = C4_UI_ROW_SEL_BG;

        lv_obj_set_style_bg_color(s_menu_rows[row], lv_color_hex(bg), 0);

        uint32_t label_color = C4_UI_MUTED;
        uint32_t value_color = C4_UI_ROW_TEXT;
        if (!enabled) {
            label_color = C4_UI_ROW_OFF_TXT;
            value_color = C4_UI_ROW_OFF_TXT;
        } else if (is_start) {
            label_color = selected ? C4_UI_BG : C4_UI_P1;
            value_color = selected ? C4_UI_BG : C4_UI_P1;
        } else if (selected) {
            label_color = C4_UI_TEXT;
            value_color = C4_UI_P1;
        }

        lv_obj_set_style_text_color(s_menu_labels[row], lv_color_hex(label_color), 0);
        lv_obj_set_style_text_color(s_menu_values[row], lv_color_hex(value_color), 0);
        lv_label_set_text(s_menu_values[row], c4_settings_row_value(settings, row));
    }
}

// ——— 棋盘屏 ———————————————————————————————————————————————

static void build_board_screen(void)
{
    s_board_scr = make_screen();

    // 自绘对象比棋盘外框大 2px:LVGL 只保证把重绘裁到对象自己的范围内,
    // 外框/描边这些超出棋盘的像素必须落在对象里,否则会被裁掉。
    s_board = lv_obj_create(s_board_scr);
    style_plain(s_board);
    lv_obj_set_size(s_board, s_layout.board.w + 4, s_layout.board.h + 4);
    lv_obj_set_pos(s_board, s_layout.board.x - 2, s_layout.board.y - 2);
    lv_obj_add_event_cb(s_board, board_draw, LV_EVENT_DRAW_MAIN_END, NULL);

    s_status = make_label(s_board_scr, &lv_font_montserrat_14, C4_UI_TEXT);
    lv_label_set_text(s_status, "YOUR TURN");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 14, 5);

    s_bat_board = make_label(s_board_scr, &lv_font_montserrat_14, C4_UI_BAT_OK);
    lv_label_set_text(s_bat_board, "--");
    lv_obj_align(s_bat_board, LV_ALIGN_TOP_RIGHT, -14, 5);
}

void c4_ui_build(void)
{
    // 逻辑分辨率跟随横竖屏:横屏下是 320x240,这样排版不会被写死的 240x320 带偏。
    c4_layout_compute(LV_HOR_RES, LV_VER_RES, &s_layout);

    build_menu_screen();
    build_board_screen();
    lv_screen_load(s_menu_scr);
}

void c4_ui_show_menu(void)
{
    if (s_menu_scr) lv_screen_load(s_menu_scr);
}

void c4_ui_show_board(void)
{
    if (s_board_scr) lv_screen_load(s_board_scr);
}

void c4_ui_set_menu_hint(const char *text)
{
    if (!s_menu_hint) return;
    lv_label_set_text(s_menu_hint, text ? text : "");
    set_text_color(s_menu_hint, C4_UI_TEXT);
}

void c4_ui_show_sleeping(void)
{
    if (s_menu_hint) {
        lv_label_set_text(s_menu_hint, "SLEEPING - PRESS ANY KEY TO WAKE");
        set_text_color(s_menu_hint, C4_UI_TEXT);
    }
    if (s_status) {
        lv_label_set_text(s_status, "SLEEPING");
        set_text_color(s_status, C4_UI_MUTED);
    }
}

void c4_ui_render(const c4_game_t *game, int cursor_col, c4_preview_t preview,
                  const char *status, uint32_t status_color)
{
    if (!game) return;

    // 局面重置时把残留的下落动画收掉,避免上一局的圆盘挂在空中。
    if (s_drop_active && c4_cell_get(game, s_drop_col, s_drop_row) == C4_EMPTY) {
        drop_cancel();
    }

    s_game = game;
    s_cursor = cursor_col;
    s_cursor_on = cursor_col >= 0 && cursor_col < C4_COLS;
    s_preview = (preview == C4_PREVIEW_TOP) ? C4_PREVIEW_TOP : C4_PREVIEW_LANDING;

    if (s_status) {
        lv_label_set_text(s_status, status ? status : "");
        set_text_color(s_status, status_color);
    }
    if (s_board) lv_obj_invalidate(s_board);
}

void c4_ui_drop_anim(int col, int row)
{
    if (!s_board || col < 0 || col >= C4_COLS || row < 0 || row >= C4_ROWS) return;

    lv_anim_delete(&s_drop_offset, drop_anim_exec);
    s_drop_active = false;
    s_drop_offset = 0;

    // 落在最上一行时落程为 0:起点=终点的动画不会回调,索性直接静态画出。
    const int distance = s_layout.cell * (C4_ROWS - 1 - row);
    if (distance <= 0) {
        lv_obj_invalidate(s_board);
        return;
    }

    s_drop_active = true;
    s_drop_col = col;
    s_drop_row = row;
    s_drop_offset = -distance;

    lv_anim_init(&s_drop_anim);
    lv_anim_set_var(&s_drop_anim, &s_drop_offset);
    lv_anim_set_values(&s_drop_anim, s_drop_offset, 0);
    lv_anim_set_duration(&s_drop_anim, C4_UI_DROP_MS);
    lv_anim_set_path_cb(&s_drop_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&s_drop_anim, drop_anim_exec);
    lv_anim_set_completed_cb(&s_drop_anim, drop_anim_completed);
    lv_anim_start(&s_drop_anim);

    lv_obj_invalidate(s_board);
}
