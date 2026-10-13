// main/gal/gal_content.h —— 框架骨架的占位内容。
//
// 骨架固件刻意不携带任何具体作品:这里只有一组演示用的占位对白与文案,
// 用来驱动原型里的全部逻辑屏。接入真实作品时,把本文件替换为内容层
// (或资源包读取层)即可,界面与交互保持不变。
//
// 剧情锁:内容层只声明“条件”,运行时状态由 gal_unlock 维护:
//   台词  按 require_flag 决定是否出现(条件分支)
//   选项  按 require_flag 决定是否可选,选中后写 flag_set
//   CG    到达 unlock_event 时解锁
//   章节  到达 unlock_event 且满足 require_flag 时解锁
#pragma once

#include "gal_unlock.h"

#include <stdbool.h>
#include <stdint.h>

// 骨架约定的剧情标记(作品可自行增删;最多 32 个)。
enum {
    GAL_FLAG_CHOICE_A = 0,   // 选择了「追上去」
    GAL_FLAG_CHOICE_B,       // 选择了「留在原地」
    GAL_FLAG_CHOICE_C,       // 选择了「询问站务员」
    GAL_FLAG_CHAPTER_2,      // 到达第二章
    GAL_FLAG_CLEARED,        // 走过一次结局
};

// ---------------------------------------------------------------- 对白
typedef struct {
    const char *speaker;   // NULL 或空串 = 旁白
    const char *text;      // UTF-8 正文(一段 = 一屏,内部按行/页自动切分)
    uint8_t require_flag;  // 需要该标记才出现;GAL_UNLOCK_NONE = 总是出现
} gal_line_t;

// ---------------------------------------------------------------- 选项
typedef struct {
    const char *label;
    uint8_t flag_set;      // 选中后写入的标记;GAL_UNLOCK_NONE = 不写
    uint8_t require_flag;  // 需要该标记才可选;GAL_UNLOCK_NONE = 无条件
} gal_option_t;

typedef struct {
    const char *prompt;              // 选项前的最后一句台词(留在底部对话框)
    int count;
    const gal_option_t *options;
} gal_choice_t;

// ---------------------------------------------------------------- 剧情事件
// 骨架的剧情就是一条占位事件序列:顺序读下来即是一个完整流程。
typedef enum {
    GAL_EV_LINE = 0,     // index = gal_lines 下标
    GAL_EV_CHOICE,       // index = gal_choices 下标
    GAL_EV_TRANSITION,   // index = gal_transitions 下标(章节过场)
    GAL_EV_ENDING,       // index = gal_endings 下标
} gal_event_kind_t;

typedef struct {
    unsigned char kind;
    unsigned char index;
    unsigned char flag_set;   // 到达该事件时写入的标记;GAL_UNLOCK_NONE = 不写
} gal_event_t;

typedef struct {
    const char *chapter;    // 进度归属的章节名(存档页显示)
    const char *eyebrow;    // 过场小标(CHAPTER 02)
    const char *title;      // 过场标题
    const char *subtitle;   // 过场副标题
} gal_transition_t;

typedef struct {
    const char *eyebrow;             // TRUE END
    const char *title;
    const char *body;
} gal_ending_t;

// ---------------------------------------------------------------- 鉴赏室与章节
typedef struct {
    const char *label;      // CG 01
    const char *caption;    // 夜行列车 · 初遇
    uint8_t unlock_event;   // 到达该事件解锁;GAL_UNLOCK_NONE = 初始解锁
    bool unlocked;          // 运行时填充:界面用(由 gal_unlock 状态换算)
} gal_cg_t;

typedef struct {
    const char *name;
    const char *summary;
    const char *progress;
    uint8_t unlock_event;   // 到达该事件解锁;GAL_UNLOCK_NONE = 不依赖事件
    uint8_t require_flag;   // 额外需要的标记;GAL_UNLOCK_NONE = 不需要
    bool unlocked;          // 运行时填充:界面用
} gal_chapter_t;

typedef struct {
    const char *label;
    const char *value;
} gal_about_row_t;

// ---------------------------------------------------------------- CG 回想与音乐鉴赏
// 回想:需要对应 CG 已解锁(等于“看过那一段”),从 event_index 重新开始演。
typedef struct {
    const char *title;        // 场景名
    const char *chapter;      // 所在章节(列表右侧显示)
    uint8_t cg_index;         // 需要该 CG 已解锁
    uint8_t event_index;      // 从该事件开始重看
} gal_recall_t;

// 音乐:骨架不带音频资源,用方波按 notes 合成一段短旋律循环播放。
// notes 是音高表下标(见 gal_music.c),GAL_NOTE_REST = 休止。
#define GAL_NOTE_REST 0xFF

typedef struct {
    const char *title;
    const char *duration;     // 显示用时长(占位文本)
    uint8_t cg_index;         // 需要该 CG 已解锁(听过那一段)
    const uint8_t *notes;
    uint8_t note_count;
} gal_track_t;

// 作品信息(由作品配置替换)。
#define GAL_CONTENT_TITLE    "未定作品"
#define GAL_CONTENT_SUBTITLE "一段尚未写完的旅程"
#define GAL_CONTENT_LOGO_SUB "GALGAME FRAMEWORK"
#define GAL_CONTENT_VERSION  "v0.1.0"

extern const gal_line_t gal_content_lines[];
extern const int gal_content_line_count;

extern const gal_event_t gal_content_events[];
extern const int gal_content_event_count;

extern const gal_choice_t gal_content_choices[];
extern const int gal_content_choice_count;

extern const gal_transition_t gal_content_transitions[];
extern const int gal_content_transition_count;

extern const gal_ending_t gal_content_endings[];
extern const int gal_content_ending_count;

extern const gal_cg_t gal_content_cgs[];
extern const int gal_content_cg_count;

extern const gal_chapter_t gal_content_chapters[];
extern const int gal_content_chapter_count;

extern const gal_about_row_t gal_content_about_rows[];
extern const int gal_content_about_row_count;

extern const gal_recall_t gal_content_recalls[];
extern const int gal_content_recall_count;

extern const gal_track_t gal_content_tracks[];
extern const int gal_content_track_count;

// 标题页菜单与游戏内快捷菜单(名称固定,可用状态由存档决定)。
extern const char *const gal_content_title_menu[];
extern const int gal_content_title_menu_count;
extern const char *const gal_content_quick_menu[];
extern const int gal_content_quick_menu_count;

// 鉴赏室分类入口。
extern const char *const gal_content_gallery_menu[];
extern const int gal_content_gallery_menu_count;

enum {
    GAL_GALLERY_CG = 0,
    GAL_GALLERY_RECALL,
    GAL_GALLERY_STORY,
    GAL_GALLERY_MUSIC,
};

// 标题页菜单项下标(供状态机使用)。
enum {
    GAL_TITLE_MENU_NEW = 0,
    GAL_TITLE_MENU_CONTINUE,
    GAL_TITLE_MENU_LOAD,
    GAL_TITLE_MENU_SETTINGS,
    GAL_TITLE_MENU_GALLERY,
    GAL_TITLE_MENU_ABOUT,
};

// 游戏内快捷菜单项下标。
enum {
    GAL_MENU_RESUME = 0,
    GAL_MENU_SAVE,
    GAL_MENU_LOAD,
    GAL_MENU_HISTORY,
    GAL_MENU_SETTINGS,
    GAL_MENU_TITLE,
};

// 关于页正文(在页头标题栏下方滚动)。
#define GAL_CONTENT_ABOUT \
    "本固件是 GalGame 框架的骨架,不含任何具体作品内容:\n" \
    "启动页、标题页、对话页、选项、菜单、存档、设置、\n" \
    "历史、鉴赏室、章节、过场、结局与关于页都已经就位。\n" \
    "\n" \
    "剧情锁:台词与选项按剧情标记出现或可选,CG 与章节\n" \
    "随剧情推进解锁,解锁记录与标记跨存档保存。\n" \
    "\n" \
    "排版按框架规范:对话框贴底、每页固定行数、字号两档\n" \
    "(16px / 20px),对话框透明度可在设置里调节。\n" \
    "\n" \
    "接入真实作品时,替换 main/gal/gal_content.c 即可,\n" \
    "界面与交互保持不变."
