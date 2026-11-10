#pragma once

#include <stdbool.h>

// A visible-window update. Rebind text only when the window moves; otherwise
// just the old and new focus rows change. This keeps navigation off the heap.
typedef struct {
    int top;
    int rows;
    int previous;
    int selected;
    bool rebind;
} voice_list_update_t;

voice_list_update_t voice_list_plan(int count, int rows, int top,
                                    int previous, int selected);
bool voice_list_row_dirty(const voice_list_update_t *update, int row);
