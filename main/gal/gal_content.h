// main/gal/gal_content.h —— 框架骨架的占位内容。
//
// 骨架固件刻意不携带任何具体作品:这里只有一组演示用的占位对白与文案,
// 用来驱动原型里的全部逻辑屏。接入真实作品时,把本文件替换为内容层
// (或资源包读取层)即可,界面与交互保持不变。
#pragma once

#include <stdbool.h>

// ---------------------------------------------------------------- 对白
typedef struct {
    const char *speaker;   // NULL 或空串 = 旁白
    const char *text;      // UTF-8 正文(一段 = 一屏,内部按行/页自动切分)
} gal_line_t;

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
} gal_event_t;

typedef struct {
    const char *prompt;              // 选项前的最后一句台词(留在底部对话框)
    int count;
    const char *const *options;
} gal_choice_t;

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
    int action_count;
    const char *const *actions;
} gal_ending_t;

// ---------------------------------------------------------------- 鉴赏室与章节
typedef struct {
    const char *label;     // CG 01
    const char *caption;   // 夜行列车 · 初遇
    bool unlocked;
} gal_cg_t;

typedef struct {
    const char *name;
    const char *summary;
    const char *progress;
    bool unlocked;
} gal_chapter_t;

typedef struct {
    const char *label;
    const char *value;
} gal_about_row_t;

typedef struct {
    const char *label;
} gal_menu_item_t;

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

// 标题页菜单与游戏内快捷菜单(名称固定,可用状态由存档决定)。
extern const char *const gal_content_title_menu[];
extern const int gal_content_title_menu_count;
extern const char *const gal_content_quick_menu[];
extern const int gal_content_quick_menu_count;

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
    "排版按框架规范:对话框贴底、每页固定行数、字号两档\n" \
    "(16px / 20px),对话框透明度可在设置里调节。\n" \
    "\n" \
    "接入真实作品时,替换 main/gal/gal_content.c 即可,\n" \
    "界面与交互保持不变."
