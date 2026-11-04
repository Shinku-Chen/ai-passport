// Host regression for the real stella_ui.c visibility flags (fake LVGL objects).
#include "stella_ui.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); failures++; } } while (0)

static stella_ui_t s_ui;
static lv_obj_t s_root;
static uint16_t s_canvas[320 * 240];

// Only the image object creation is faked; all UI page logic below is production code.
bool stella_image_init(stella_image_t *img, lv_obj_t *parent, uint16_t *pixels)
{
    (void)parent; (void)pixels;
    if (!img) return false;
    memset(img, 0, sizeof(*img));
    return true;
}

static bool visible(const lv_obj_t *obj) { return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }

static int visible_pages(void)
{
    return (int)visible(s_ui.page_title) + (int)visible(s_ui.page_game) +
           (int)visible(s_ui.page_list) + (int)visible(s_ui.page_cg) +
           (int)visible(s_ui.page_history) + (int)visible(s_ui.page_ending) +
           (int)visible(s_ui.page_confirm);
}


// 菜单高亮：固定行宽 + 圆角 6 + 无边框；只有当前选中行 bg_opa==70，其余 0；
// 行间距 ≥2px（避免相邻高亮条边界相接产生竖线）。
static void check_menu_highlight(void)
{
    const char *rows[5] = {"a", "b", "c", "d", "e"};
    stella_ui_set_list(&s_ui, "菜单", rows, NULL, 5, 0);
    CHECK(s_ui.list_rows[0]->bg_opa == 70, "selected bg_opa=%d", s_ui.list_rows[0]->bg_opa);
    for (int i = 1; i < 5; i++) {
        CHECK(s_ui.list_rows[i]->bg_opa == 0, "row %d must be unhighlighted (%d)", i,
              s_ui.list_rows[i]->bg_opa);
    }
    const int w = s_ui.list_rows[0]->w;
    for (int i = 1; i < 5; i++) {
        CHECK(s_ui.list_rows[i]->w == w, "row width must be fixed (%d vs %d)", w,
              s_ui.list_rows[i]->w);
    }
    CHECK(s_ui.list_rows[0]->radius == 6, "highlight radius=%d", s_ui.list_rows[0]->radius);
    CHECK(s_ui.list_rows[0]->border_w == 0, "highlight border=%d", s_ui.list_rows[0]->border_w);
    for (int i = 1; i < 5; i++) {
        const int gap = s_ui.list_rows[i]->y - (s_ui.list_rows[i - 1]->y + s_ui.list_rows[i - 1]->h);
        CHECK(gap >= 2, "row gap %d must be >= 2", gap);
    }
    // 切选中：只有新行 70、旧行归零
    stella_ui_list_select(&s_ui, 2);
    CHECK(s_ui.list_rows[0]->bg_opa == 0, "old row cleared (%d)", s_ui.list_rows[0]->bg_opa);
    CHECK(s_ui.list_rows[2]->bg_opa == 70, "new row highlighted (%d)", s_ui.list_rows[2]->bg_opa);
}

int main(void)
{
    static const lv_font_t font = {0};
    CHECK(stella_ui_create(&s_ui, &s_root, s_canvas, &font, &font), "ui create");
    CHECK(visible(s_ui.page_title), "title initially visible");
    CHECK(visible_pages() == 1, "initial visible pages=%d", visible_pages());
    CHECK(!visible(s_ui.notice_label), "notice initially hidden");
    check_menu_highlight();
    // 标题菜单必须落在下半屏（不能遮住 320x240 标题图的 2/3），4 行窗口不越 240。
    CHECK(s_ui.title_rows[0]->y >= 150, "title row0 y=%d (expected >=150)",
          s_ui.title_rows[0]->y);
    // 阅读页四硬指标：名字框外、页码在面板内且不压正文、末行不出面板。
    // 对话框 156..240（98->84）；名字底条只包文字、底边不压面板；正文贴面板顶 +4。
    CHECK(s_ui.name_box->y + s_ui.name_box->h <= 156,
          "speaker must sit above the panel (y=%d h=%d)", s_ui.name_box->y,
          s_ui.name_box->h);
    // 姓名条必须能量出宽度（CLIP+SIZE_CONTENT 会退化成 0 → 实机“名字消失”）。
    CHECK(s_ui.name_box->long_mode == LV_LABEL_LONG_MODE_DOTS,
          "name label must use single-line DOTS mode (%d)", (int)s_ui.name_box->long_mode);
    CHECK(s_ui.name_box->w == LV_SIZE_CONTENT || s_ui.name_box->w > 0,
          "name strip must hug text (w=%d)", s_ui.name_box->w);
    CHECK(s_ui.text_label->y == 160, "body must start at panel_top+4 (y=%d)",
          s_ui.text_label->y);
    CHECK(s_ui.page_label->y + s_ui.page_label->h <= s_ui.text_label->y,
          "page label overlaps body (page bottom %d > text top %d)",
          s_ui.page_label->y + s_ui.page_label->h, s_ui.text_label->y);
    CHECK(s_ui.text_label->y + s_ui.text_label->h <= 238,
          "last text line bottom %d", s_ui.text_label->y + s_ui.text_label->h);
    CHECK(s_ui.title_rows[3]->y + s_ui.title_rows[3]->h <= 238,
          "title last window row bottom=%d (expected <=238)",
          s_ui.title_rows[3]->y + s_ui.title_rows[3]->h);
    CHECK(s_ui.title_rows[1]->x == s_ui.title_rows[0]->x &&
              s_ui.title_rows[1]->y > s_ui.title_rows[0]->y,
          "title rows must be one column (row1 %d,%d row0 %d,%d)",
          s_ui.title_rows[1]->x, s_ui.title_rows[1]->y,
          s_ui.title_rows[0]->x, s_ui.title_rows[0]->y);

    const stella_page_t pages[] = {
        STELLA_PAGE_GAME, STELLA_PAGE_CG, STELLA_PAGE_HISTORY,
        STELLA_PAGE_ENDING, STELLA_PAGE_CONFIRM,
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        stella_ui_show(&s_ui, pages[i]);
        CHECK(visible_pages() == 1, "page %d visible pages=%d", (int)pages[i], visible_pages());
    }
    const stella_page_t lists[] = {
        STELLA_PAGE_MENU, STELLA_PAGE_SLOTS, STELLA_PAGE_SETTINGS,
        STELLA_PAGE_CHAPTERS, STELLA_PAGE_GALLERY,
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        stella_ui_show(&s_ui, lists[i]);
        CHECK(visible(s_ui.page_list) && visible_pages() == 1,
              "list page %d visible=%d pages=%d", (int)lists[i],
              (int)visible(s_ui.page_list), visible_pages());
    }

    stella_ui_show(&s_ui, STELLA_PAGE_TITLE);
    const char *rows[8] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    // 单列 4 行窗口：选中项始终在 [first, first+3]，末行盒底 <= 240。
    stella_ui_set_title_rows(&s_ui, rows, 8, 0);
    for (int i = 0; i < 4; ++i) CHECK(visible(s_ui.title_rows[i]), "title window row %d", i);
    for (int sel = 0; sel < 8; ++sel) {
        stella_ui_set_title_rows(&s_ui, rows, 8, sel);
        const int first = s_ui.title_first;
        CHECK(sel >= first && sel < first + 4, "window: sel=%d first=%d", sel, first);
        CHECK(s_ui.title_rows[3]->y + s_ui.title_rows[3]->h <= 238,
              "window bottom %d", s_ui.title_rows[3]->y + s_ui.title_rows[3]->h);
        CHECK(strcmp(s_ui.title_rows[sel - first]->text, rows[sel]) == 0,
              "window maps sel=%d to row text", sel);
    }
    stella_ui_set_title_rows(&s_ui, rows, 3, 1);
    for (int i = 0; i < 3; ++i) CHECK(visible(s_ui.title_rows[i]), "title short row %d", i);
    CHECK(!visible(s_ui.title_rows[3]), "title 4th slot hidden when count=3");

    stella_ui_show(&s_ui, STELLA_PAGE_MENU);
    const char *values[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    stella_ui_set_list(&s_ui, "menu", rows, values, 8, 0);
    for (int i = 0; i < 8; ++i) {
        CHECK(visible(s_ui.list_rows[i]) && visible(s_ui.list_values[i]), "list row %d", i);
    }
    stella_ui_set_list(&s_ui, "menu", rows, values, 2, 1);
    for (int i = 0; i < 2; ++i) {
        CHECK(visible(s_ui.list_rows[i]) && visible(s_ui.list_values[i]), "list short row %d", i);
    }
    for (int i = 2; i < 8; ++i) {
        CHECK(!visible(s_ui.list_rows[i]) && !visible(s_ui.list_values[i]), "list hidden row %d", i);
    }

    stella_ui_notice(&s_ui, "保存失败");
    CHECK(visible(s_ui.notice_label), "notice shown");
    CHECK(strcmp(s_ui.notice_text->text, "保存失败") == 0, "notice text");
    stella_ui_notice_hide(&s_ui);
    CHECK(!visible(s_ui.notice_label), "notice hidden");

    stella_ui_show(&s_ui, STELLA_PAGE_GAME);
    stella_ui_show_choices(&s_ui, "prompt", "option");
    CHECK(visible(s_ui.choice_box), "choices shown");
    stella_ui_hide_choices(&s_ui);
    CHECK(!visible(s_ui.choice_box), "choices hidden");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: real UI visibility (pages/rows/notice/choices)\n");
    return 0;
}
