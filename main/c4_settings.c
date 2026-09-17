// main/c4_settings.c —— 标题屏菜单逻辑实现(无 ESP-IDF / LVGL 依赖)。
#include "c4_settings.h"

#include <stddef.h>

static const char *const MODE_VALUES[C4_MODE_COUNT] = {
    [C4_MODE_AI] = "HUMAN vs AI",
    [C4_MODE_TWO_PLAYER] = "TWO PLAYERS",
};

static const char *const LEVEL_VALUES[C4_LEVEL_COUNT] = {
    [C4_LEVEL_EASY] = "EASY",
    [C4_LEVEL_MEDIUM] = "MEDIUM",
    [C4_LEVEL_HARD] = "HARD",
};

static const char *const PREVIEW_VALUES[C4_PREVIEW_COUNT] = {
    [C4_PREVIEW_LANDING] = "LANDING",
    [C4_PREVIEW_TOP] = "TOP ROW",
};

static const char *const ROW_LABELS[C4_MENU_ROW_COUNT] = {
    [C4_MENU_ROW_MODE] = "MODE",
    [C4_MENU_ROW_LEVEL] = "LEVEL",
    [C4_MENU_ROW_PREVIEW] = "PREVIEW",
    [C4_MENU_ROW_START] = "START",
};

static uint8_t wrap(uint8_t value, int delta, uint8_t count)
{
    int next = (int)value + delta;
    while (next < 0) next += count;
    while (next >= count) next -= count;
    return (uint8_t)next;
}

void c4_settings_init(c4_settings_t *settings)
{
    if (!settings) return;

    settings->mode = C4_MODE_AI;
    settings->level = C4_LEVEL_MEDIUM;
    settings->preview = C4_PREVIEW_TOP;
    settings->row = C4_MENU_ROW_MODE;
}

bool c4_settings_row_enabled(const c4_settings_t *settings, int row)
{
    if (!settings) return false;
    if (row < 0 || row >= C4_MENU_ROW_COUNT) return false;
    if (row == C4_MENU_ROW_LEVEL && settings->mode == C4_MODE_TWO_PLAYER) return false;
    return true;
}

void c4_settings_move(c4_settings_t *settings, int step)
{
    if (!settings || (step != 1 && step != -1)) return;

    uint8_t row = settings->row;
    for (int i = 0; i < C4_MENU_ROW_COUNT; i++) {
        row = wrap(row, step, C4_MENU_ROW_COUNT);
        if (c4_settings_row_enabled(settings, row)) {
            settings->row = row;
            return;
        }
    }
}

void c4_settings_cycle(c4_settings_t *settings)
{
    if (!settings) return;

    switch (settings->row) {
    case C4_MENU_ROW_MODE:
        // 切到双人后难度行失效;上一行/下一行的移动会自动跳过它。
        settings->mode = wrap(settings->mode, 1, C4_MODE_COUNT);
        break;
    case C4_MENU_ROW_LEVEL:
        settings->level = wrap(settings->level, 1, C4_LEVEL_COUNT);
        break;
    case C4_MENU_ROW_PREVIEW:
        settings->preview = wrap(settings->preview, 1, C4_PREVIEW_COUNT);
        break;
    default:
        break;   // START 行由调用方处理
    }
}

bool c4_settings_is_start(const c4_settings_t *settings)
{
    return settings && settings->row == C4_MENU_ROW_START;
}

const char *c4_settings_row_label(int row)
{
    if (row < 0 || row >= C4_MENU_ROW_COUNT) return "";
    return ROW_LABELS[row];
}

const char *c4_settings_row_value(const c4_settings_t *settings, int row)
{
    if (!settings || row < 0 || row >= C4_MENU_ROW_COUNT) return "";
    if (!c4_settings_row_enabled(settings, row)) return "-";

    switch (row) {
    case C4_MENU_ROW_MODE:
        return MODE_VALUES[settings->mode < C4_MODE_COUNT ? settings->mode : 0];
    case C4_MENU_ROW_LEVEL:
        return LEVEL_VALUES[settings->level < C4_LEVEL_COUNT ? settings->level : 0];
    case C4_MENU_ROW_PREVIEW:
        return PREVIEW_VALUES[settings->preview < C4_PREVIEW_COUNT ? settings->preview : 0];
    case C4_MENU_ROW_START:
        return "PRESS OK";
    default:
        return "";
    }
}
