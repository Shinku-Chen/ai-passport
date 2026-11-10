#ifdef NDEBUG
#undef NDEBUG
#endif
#include "voice_list_view.h"
#include <assert.h>
#include <stdio.h>

static int dirty_rows(const voice_list_update_t *update)
{
    int count = 0;
    for (int i = 0; i < update->rows; ++i)
        if (voice_list_row_dirty(update, i)) ++count;
    assert(!voice_list_row_dirty(update, -1));
    assert(!voice_list_row_dirty(update, update->rows));
    return count;
}

static void test_focus_only_updates_two_rows(void)
{
    voice_list_update_t update = voice_list_plan(160, 14, 0, 5, 6);
    assert(update.top == 0);
    assert(!update.rebind);
    assert(dirty_rows(&update) == 2);
    assert(voice_list_row_dirty(&update, 5));
    assert(voice_list_row_dirty(&update, 6));

    update = voice_list_plan(160, 14, 20, 26, 25);
    assert(update.top == 20 && !update.rebind);
    assert(dirty_rows(&update) == 2);
    assert(voice_list_row_dirty(&update, 5));
    assert(voice_list_row_dirty(&update, 6));
}

static void test_crossing_window_rebinds_visible_rows(void)
{
    voice_list_update_t update = voice_list_plan(160, 14, 0, 13, 14);
    assert(update.top == 1 && update.rebind);
    assert(dirty_rows(&update) == 14);

    update = voice_list_plan(160, 14, 1, 1, 0);
    assert(update.top == 0 && update.rebind);
    assert(dirty_rows(&update) == 14);
}

static void test_boundaries_do_not_redraw_or_wrap(void)
{
    voice_list_update_t update = voice_list_plan(160, 14, 0, 0, -1);
    assert(update.selected == 0 && update.top == 0);
    assert(!update.rebind && dirty_rows(&update) == 0);

    update = voice_list_plan(160, 14, 146, 159, 160);
    assert(update.selected == 159 && update.top == 146);
    assert(!update.rebind && dirty_rows(&update) == 0);
}

static void test_empty_short_and_restored_lists(void)
{
    voice_list_update_t update = voice_list_plan(0, 14, 0, 0, 1);
    assert(update.rows == 0 && dirty_rows(&update) == 0);
    update = voice_list_plan(8, 0, 0, 0, 1);
    assert(update.rows == 0 && dirty_rows(&update) == 0);

    update = voice_list_plan(3, 14, 0, 1, 2);
    assert(update.rows == 3 && update.top == 0 && !update.rebind);
    assert(dirty_rows(&update) == 2);

    // Returning from a subpage must keep the retained selection visible.
    update = voice_list_plan(30, 14, 0, -1, 24);
    assert(update.top == 11 && update.selected == 24 && update.rebind);
    assert(dirty_rows(&update) == 14);
}

int main(void)
{
    test_focus_only_updates_two_rows();
    test_crossing_window_rebinds_visible_rows();
    test_boundaries_do_not_redraw_or_wrap();
    test_empty_short_and_restored_lists();
    puts("voice_list_view: PASS");
    return 0;
}
