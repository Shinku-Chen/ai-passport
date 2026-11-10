#include "voice_list_view.h"

voice_list_update_t voice_list_plan(int count, int rows, int top,
                                    int previous, int selected)
{
    voice_list_update_t update = {0};
    if (count <= 0 || rows <= 0) return update;

    if (rows > count) rows = count;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    int next_top = top;
    if (selected < next_top) next_top = selected;
    if (selected >= next_top + rows) next_top = selected - rows + 1;
    if (next_top < 0) next_top = 0;
    if (next_top > count - rows) next_top = count - rows;

    update.top = next_top;
    update.rows = rows;
    update.previous = previous;
    update.selected = selected;
    update.rebind = next_top != top;
    return update;
}

bool voice_list_row_dirty(const voice_list_update_t *update, int row)
{
    if (row < 0 || row >= update->rows) return false;
    if (update->rebind) return true;
    int index = update->top + row;
    return update->previous != update->selected &&
           (index == update->previous || index == update->selected);
}
