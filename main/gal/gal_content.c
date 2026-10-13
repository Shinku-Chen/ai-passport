// main/gal/gal_content.c —— 骨架固件的占位内容(对白、事件流、菜单与列表数据)。
#include "gal_content.h"

// ---------------------------------------------------------------- 对白
// 覆盖:打字机、说话人与旁白、超长文本自动分页,以及按剧情标记出现的条件台词。
const gal_line_t gal_content_lines[] = {
    { "", "这是一台占位的框架固件。", GAL_UNLOCK_NONE },
    { "", "屏幕上的文字来自 main/gal/gal_content.c。", GAL_UNLOCK_NONE },
    { "角色 A", "接入真实作品时,替换内容层即可,界面与交互保持不变。", GAL_UNLOCK_NONE },
    { "角色 B", "启动页、标题页、阅读器与设置面板都已经就位。", GAL_UNLOCK_NONE },
    { "", "夜色像潮水一样漫过站台,远处的灯一盏一盏亮起来,列车还没有进站,"
          "风里带着铁轨和一点点海水的味道。故事从这里开始,也会在这里结束。",
      GAL_UNLOCK_NONE },
    { "角色 A", "排版按框架规范:对话框贴底、每页固定行数、字号两档。", GAL_UNLOCK_NONE },
    { "角色 B", "长按确定打开设置,试试对话框透明度。", GAL_UNLOCK_NONE },
    { "", "占位内容走完了第一章。", GAL_UNLOCK_NONE },
    // 下面两句是同一位置的两个分支:按前面选了什么,只会出现其中一句。
    { "角色 A", "追上去的那一步,让后面的路跟着变了。", GAL_FLAG_CHOICE_A },
    { "角色 B", "留在原地也不算坏,故事照样往前走。", GAL_FLAG_CHOICE_B },
    { "", "第二章:轨道伸向看不见的地方,风把云推过站台顶棚。", GAL_UNLOCK_NONE },
    { "角色 A", "分支在这里汇合,后面的路是同一段。", GAL_UNLOCK_NONE },
    { "角色 B", "那就继续走吧。", GAL_UNLOCK_NONE },
};

const int gal_content_line_count =
    (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));

// ---------------------------------------------------------------- 选项
static const gal_option_t choice0_options[] = {
    { "追上去", GAL_FLAG_CHOICE_A, GAL_UNLOCK_NONE },
    { "留在原地", GAL_FLAG_CHOICE_B, GAL_UNLOCK_NONE },
    { "询问站务员", GAL_FLAG_CHOICE_C, GAL_UNLOCK_NONE },
};

// 第二组:第一个选项需要“走过一次结局”才会出现(剧情锁演示)。
static const gal_option_t choice1_options[] = {
    { "走向隐藏的站台", GAL_UNLOCK_NONE, GAL_FLAG_CLEARED },
    { "停在月台", GAL_UNLOCK_NONE, GAL_UNLOCK_NONE },
};

const gal_choice_t gal_content_choices[] = {
    { "她停下脚步,回头看我。", 3, choice0_options },
    { "夜里还有一班车,站牌上却没有写。", 2, choice1_options },
};

const int gal_content_choice_count =
    (int)(sizeof(gal_content_choices) / sizeof(gal_content_choices[0]));

// ---------------------------------------------------------------- 事件流
// 对白 → 选项 → 对白(含条件分支) → 章节过场(第二章) → 对白 → 选项 → 结局。
const gal_event_t gal_content_events[] = {
    { GAL_EV_LINE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 1, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 2, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 3, GAL_UNLOCK_NONE },
    { GAL_EV_CHOICE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 4, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 5, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 6, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 7, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 8, GAL_UNLOCK_NONE },     // 条件台词(选了「追上去」)
    { GAL_EV_LINE, 9, GAL_UNLOCK_NONE },     // 条件台词(选了「留在原地」)
    { GAL_EV_TRANSITION, 0, GAL_FLAG_CHAPTER_2 },
    { GAL_EV_LINE, 10, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 11, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 12, GAL_UNLOCK_NONE },
    { GAL_EV_CHOICE, 1, GAL_UNLOCK_NONE },
    { GAL_EV_ENDING, 0, GAL_FLAG_CLEARED },
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
const gal_ending_t gal_content_endings[] = {
    { "TRUE END", "星光抵达的地方", "故事暂告一段落。\n感谢你陪他们走到这里。" },
};

const int gal_content_ending_count =
    (int)(sizeof(gal_content_endings) / sizeof(gal_content_endings[0]));

// ---------------------------------------------------------------- 鉴赏室
// unlock_event = 到达该事件下标时解锁(运行时由 gal_unlock 记录)。
const gal_cg_t gal_content_cgs[] = {
    { "CG 01", "夜行列车 · 初遇", 0, false },
    { "CG 02", "月台尽头", 3, false },
    { "CG 03", "雨中的站牌", 6, false },
    { "CG 04", "窗边的对话", 9, false },
    { "CG 05", "雨停之后", 11, false },
    { "CG 06", "轨道与星空", 13, false },
    { "CG 07", "未解锁", 16, false },
    { "CG 08", "未解锁", 16, false },
};

const int gal_content_cg_count = (int)(sizeof(gal_content_cgs) / sizeof(gal_content_cgs[0]));

// ---------------------------------------------------------------- 章节 / 路线
const gal_chapter_t gal_content_chapters[] = {
    { "序章 · 夜行列车", "从站台上的相遇开始,寻找时刻表中缺失的那一站。", "完成度　1 / 3",
      GAL_UNLOCK_NONE, GAL_UNLOCK_NONE, false },
    { "林线 · 雨停之前", "雨停之前,站台长椅上还有一段没有被说完的话。", "完成度　1 / 3",
      11, GAL_UNLOCK_NONE, false },
    { "隐藏线 · 星轨", "走完一次结局后解锁。", "完成度　0 / 3",
      GAL_UNLOCK_NONE, GAL_FLAG_CLEARED, false },
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

// ---------------------------------------------------------------- CG 回想
// 每条需要对应 CG 已解锁(=看过那一段),从这里的事件重新开始演。
const gal_recall_t gal_content_recalls[] = {
    { "月台初遇", "序章", 0, 0 },
    { "雨中的站牌", "第二章", 2, 6 },
    { "窗边的对话", "第二章", 3, 9 },
    { "轨道与星空", "第二章", 5, 13 },
};

const int gal_content_recall_count =
    (int)(sizeof(gal_content_recalls) / sizeof(gal_content_recalls[0]));

// ---------------------------------------------------------------- 音乐鉴赏
// 骨架不携带音频资源:按音高表合成一段短旋律循环播放(notes 为音高表下标)。
static const uint8_t track0_notes[] = { 0, 4, 7, 12, 7, 4, 0, GAL_NOTE_REST };
static const uint8_t track1_notes[] = { 5, 7, 9, 7, 5, 2, 0, GAL_NOTE_REST };
static const uint8_t track2_notes[] = { 2, 4, 5, 7, 9, 7, 5, 4, GAL_NOTE_REST, GAL_NOTE_REST };
static const uint8_t track3_notes[] = { 7, 9, 11, 12, 11, 9, 7, GAL_NOTE_REST };
static const uint8_t track4_notes[] = { 0, 2, 4, 2, 0, GAL_NOTE_REST, 4, 5 };
static const uint8_t track5_notes[] = { 12, 11, 9, 7, 5, 4, 2, 0 };

const gal_track_t gal_content_tracks[] = {
    { "夜行列车", "03:12", 0, track0_notes, (uint8_t)sizeof(track0_notes) },
    { "站台的风", "02:48", 1, track1_notes, (uint8_t)sizeof(track1_notes) },
    { "雨停之前", "04:05", 2, track2_notes, (uint8_t)sizeof(track2_notes) },
    { "星轨", "03:36", 3, track3_notes, (uint8_t)sizeof(track3_notes) },
    { "时刻表上没有的站", "02:57", 5, track4_notes, (uint8_t)sizeof(track4_notes) },
    { "星光抵达的地方", "05:20", 6, track5_notes, (uint8_t)sizeof(track5_notes) },
};

const int gal_content_track_count =
    (int)(sizeof(gal_content_tracks) / sizeof(gal_content_tracks[0]));

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
    "CG 鉴赏", "CG 回想", "剧情鉴赏", "音乐鉴赏",
};

const int gal_content_gallery_menu_count =
    (int)(sizeof(gal_content_gallery_menu) / sizeof(gal_content_gallery_menu[0]));
