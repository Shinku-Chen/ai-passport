// tests/gal_app_stubs/gal_content_choice.c —— 分支内容的测试替身。
//
// 生产状态机自身支持选项（框架能力）。本作是线性剧情，没有分支，所以用一个
// 最小分支内容单独验证：快进必须停在选项页、长按只做长按动作、选项写入标记。
#include "gal_content.h"


const gal_line_t gal_content_lines[] = {
    { "", "第一句。", GAL_SCENE_NONE, GAL_UNLOCK_NONE, NULL, NULL },
    { "A", "第二句。", GAL_SCENE_NONE, GAL_UNLOCK_NONE, NULL, NULL },
    { "", "要追上去吗？", GAL_SCENE_NONE, GAL_UNLOCK_NONE, NULL, NULL },
    { "A", "追上去了。", GAL_SCENE_NONE, 1, NULL, NULL },
    { "B", "留在原地。", GAL_SCENE_NONE, GAL_UNLOCK_NONE, NULL, NULL },
    { "", "过场前最后一句。", GAL_SCENE_NONE, GAL_UNLOCK_NONE, NULL, NULL },
};
const int gal_content_line_count = (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));

static const gal_option_t choice0_options[] = {
    { "追上去", 1, GAL_UNLOCK_NONE },
    { "留在原地", GAL_UNLOCK_NONE, GAL_UNLOCK_NONE },
};
const gal_choice_t gal_content_choices[] = {
    { "要追上去吗？", 2, choice0_options },
};
const int gal_content_choice_count =
    (int)(sizeof(gal_content_choices) / sizeof(gal_content_choices[0]));

const gal_event_t gal_content_events[] = {
    { GAL_EV_LINE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 1, GAL_UNLOCK_NONE },
    { GAL_EV_CHOICE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 4, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 3, GAL_FLAG_CLEARED },
    { GAL_EV_TRANSITION, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 5, GAL_UNLOCK_NONE },
    { GAL_EV_ENDING, 0, GAL_FLAG_CLEARED },
};
const int gal_content_event_count =
    (int)(sizeof(gal_content_events) / sizeof(gal_content_events[0]));

const gal_transition_t gal_content_transitions[] = {
    { "第一章", "CHAPTER 01", "过场", "测试过场" },
};
const int gal_content_transition_count = 1;

const gal_ending_t gal_content_endings[] = {
    { "END", "结束", "测试结局。" },
};
const int gal_content_ending_count = 1;

const gal_cg_t gal_content_cgs[] = {
    { "CG 01", "测试 CG 1", GAL_EVENT_NONE, 0, true, NULL },
    { "CG 02", "测试 CG 2", GAL_EVENT_NONE, 1, true, NULL },
};
const int gal_content_cg_count = 2;

const gal_chapter_t gal_content_chapters[] = {
    { "第一章", "测试章节", "全 6 段", 0, GAL_UNLOCK_NONE, true },
};
const int gal_content_chapter_count = 1;

const gal_about_row_t gal_content_about_rows[] = {
    { "Version", "v0.0.0" },
};
const int gal_content_about_row_count = 1;

const gal_recall_t gal_content_recalls[] = {
    { "第一章回看", "第一章", 0, 0 },
};
const int gal_content_recall_count = 1;

const gal_track_t gal_content_tracks[] = {
    { "测试曲", "00:01", 0, NULL, 0, 0 },
};
const int gal_content_track_count = 1;

const char *const gal_content_title_menu[] = { "开始", "继续", "读取", "设置", "鉴赏", "关于" };
const int gal_content_title_menu_count = 6;
const char *const gal_content_quick_menu[] = { "返回", "保存", "读取", "历史", "设置", "标题" };
const int gal_content_quick_menu_count = 6;
const char *const gal_content_gallery_menu[] = { "CG", "回想", "剧情", "音乐" };
const int gal_content_gallery_menu_count = 4;

const void *gal_content_title_image = NULL;
