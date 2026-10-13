// main/gal/gal_content.c —— 骨架固件的占位内容(对白、事件流、菜单与列表数据)。
#include "gal_content.h"

// ---------------------------------------------------------------- 对白
// 覆盖:打字机、说话人与旁白、超长文本自动分页。
const gal_line_t gal_content_lines[] = {
    { "", "这是一台占位的框架固件。" },
    { "旁白", "屏幕上的文字来自 main/gal/gal_content.c。" },
    { "角色 A", "接入真实作品时,替换内容层即可,界面与交互保持不变。" },
    { "角色 B", "启动页、标题页、阅读器与设置面板都已经就位。" },
    { "", "夜色像潮水一样漫过站台,远处的灯一盏一盏亮起来,列车还没有进站,"
          "风里带着铁轨和一点点海水的味道。故事从这里开始,也会在这里结束。" },
    { "角色 A", "排版按框架规范:对话框贴底、每页固定行数、字号两档。" },
    { "角色 B", "长按确定打开设置,试试对话框透明度。" },
    { "", "占位内容走完了第一章。" },
    { "旁白", "第二章:轨道伸向看不见的地方,风把云推过站台顶棚。" },
    { "角色 A", "分支在这里汇合,后面的路是同一段。" },
    { "角色 B", "那就继续走吧。" },
};

const int gal_content_line_count =
    (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));

// ---------------------------------------------------------------- 选项
static const char *const choice0_options[] = { "追上去", "留在原地", "询问站务员" };

const gal_choice_t gal_content_choices[] = {
    { "她停下脚步,回头看我。", 3, choice0_options },
};

const int gal_content_choice_count =
    (int)(sizeof(gal_content_choices) / sizeof(gal_content_choices[0]));

// ---------------------------------------------------------------- 事件流
// 对白 0-3 → 选项 → 对白 4-7 → 章节过场 → 对白 8-10 → 结局。
const gal_event_t gal_content_events[] = {
    { GAL_EV_LINE, 0 },
    { GAL_EV_LINE, 1 },
    { GAL_EV_LINE, 2 },
    { GAL_EV_LINE, 3 },
    { GAL_EV_CHOICE, 0 },
    { GAL_EV_LINE, 4 },
    { GAL_EV_LINE, 5 },
    { GAL_EV_LINE, 6 },
    { GAL_EV_LINE, 7 },
    { GAL_EV_TRANSITION, 0 },
    { GAL_EV_LINE, 8 },
    { GAL_EV_LINE, 9 },
    { GAL_EV_LINE, 10 },
    { GAL_EV_ENDING, 0 },
};

const int gal_content_event_count =
    (int)(sizeof(gal_content_events) / sizeof(gal_content_events[0]));

// ---------------------------------------------------------------- 章节过场
const gal_transition_t gal_content_transitions[] = {
    { "第 2 章", "CHAPTER 02", "雨停之后", "第二章 · 雨停之前" },
};

const int gal_content_transition_count =
    (int)(sizeof(gal_content_transitions) / sizeof(gal_content_transitions[0]));

// ---------------------------------------------------------------- 结局
static const char *const ending0_actions[] = { "返回标题", "重看结局", "鉴赏室" };

const gal_ending_t gal_content_endings[] = {
    { "TRUE END", "星光抵达的地方", "故事暂告一段落。\n感谢你陪他们走到这里。", 3, ending0_actions },
};

const int gal_content_ending_count =
    (int)(sizeof(gal_content_endings) / sizeof(gal_content_endings[0]));

// ---------------------------------------------------------------- 鉴赏室
const gal_cg_t gal_content_cgs[] = {
    { "CG 01", "夜行列车 · 初遇", true },
    { "CG 02", "月台尽头", true },
    { "CG 03", "雨中的站牌", true },
    { "CG 04", "窗边的对话", true },
    { "CG 05", "雨停之后", true },
    { "CG 06", "轨道与星空", true },
    { "CG 07", "未解锁", false },
    { "CG 08", "未解锁", false },
};

const int gal_content_cg_count = (int)(sizeof(gal_content_cgs) / sizeof(gal_content_cgs[0]));

// ---------------------------------------------------------------- 章节 / 路线
const gal_chapter_t gal_content_chapters[] = {
    { "序章 · 夜行列车", "从站台上的相遇开始,寻找时刻表中缺失的那一站。", "完成度　1 / 3", true },
    { "林线 · 雨停之前", "雨停之前,站台长椅上还有一段没有被说完的话。", "完成度　1 / 3", true },
    { "隐藏线 · 星轨", "满足条件后解锁。", "完成度　0 / 3", false },
};

const int gal_content_chapter_count =
    (int)(sizeof(gal_content_chapters) / sizeof(gal_content_chapters[0]));

// ---------------------------------------------------------------- 关于
const gal_about_row_t gal_content_about_rows[] = {
    { "版本", GAL_CONTENT_VERSION },
    { "作者", "作者名待填写" },
    { "美术 / 音乐", "素材信息待填写" },
    { "授权说明", "按作品配置填写" },
};

const int gal_content_about_row_count =
    (int)(sizeof(gal_content_about_rows) / sizeof(gal_content_about_rows[0]));

// ---------------------------------------------------------------- 菜单
const char *const gal_content_title_menu[] = {
    "开始新游戏", "继续游戏", "读取存档", "设置", "鉴赏室", "关于",
};

const int gal_content_title_menu_count =
    (int)(sizeof(gal_content_title_menu) / sizeof(gal_content_title_menu[0]));

const char *const gal_content_quick_menu[] = {
    "返回剧情", "保存进度", "读取进度", "历史文本", "设置", "返回标题",
};

const int gal_content_quick_menu_count =
    (int)(sizeof(gal_content_quick_menu) / sizeof(gal_content_quick_menu[0]));

// ---------------------------------------------------------------- 鉴赏室分类
const char *const gal_content_gallery_menu[] = {
    "CG 鉴赏", "剧情鉴赏",
};

const int gal_content_gallery_menu_count =
    (int)(sizeof(gal_content_gallery_menu) / sizeof(gal_content_gallery_menu[0]));
