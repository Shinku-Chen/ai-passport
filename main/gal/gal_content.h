// main/gal/gal_content.h —— 爱上火车 · 二六线（GalGame 框架内容层）。
//
// 内容表由 tools/maitetsu_assets.py 从固定提交的 RW5 二六线素材生成，
// 本文件只声明结构与作品级常量。
//
// 本作是线性剧情：1,537 段对白、四章、一个结局，没有分支选项。框架的
// 选项 / 周目标记机制保留（其他作品在用），这里不产生 CHOICE 事件。
//
// 剧情锁：永久 CG / 章节 / 结局由 gal_unlock 维护；台词 / 选项用的周目标记
// 由 gal_app.flags 维护并随存档 / 自动进度保存。章节与回想入口是“试读”，
// 只有完整走完四章才授予永久结局资格（见 gal_app.c 的 run_mask）。
#pragma once

#include "gal_unlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 框架约定的永久资格标记：由 gal_unlock.ending 派生，绝不写入周目标记。
enum {
    GAL_FLAG_CLEARED = 0,
};

// ---------------------------------------------------------------- 对白
typedef struct {
    const char *speaker;   // NULL 或空串 = 旁白
    const char *text;      // UTF-8 正文（一段 = 一屏，内部按行/页自动切分）
    uint16_t scene;        // 媒体包场景下标；GAL_SCENE_NONE = 程序绘制的渐变
    uint8_t require_flag;  // 需要该标记才出现；GAL_UNLOCK_NONE = 总是出现
    const void *image;     // 兼容框架：直接背景图描述符；本作使用 scene
    const void *body;      // 兼容框架：直接立绘描述符；本作使用 scene
} gal_line_t;

#define GAL_SCENE_NONE 0xFFFF

// ---------------------------------------------------------------- 选项
typedef struct {
    const char *label;
    uint8_t flag_set;
    uint8_t require_flag;
} gal_option_t;

typedef struct {
    const char *prompt;
    int count;
    const gal_option_t *options;
} gal_choice_t;

// ---------------------------------------------------------------- 剧情事件
typedef enum {
    GAL_EV_LINE = 0,
    GAL_EV_CHOICE,
    GAL_EV_TRANSITION,
    GAL_EV_ENDING,
} gal_event_kind_t;

typedef struct {
    unsigned char kind;
    unsigned short index;     // 对白 / 选项 / 过场 / 结局的下标（uint16）
    unsigned char flag_set;   // 到达该事件时写入的标记；GAL_UNLOCK_NONE = 不写
} gal_event_t;

typedef struct {
    const char *chapter;
    const char *eyebrow;
    const char *title;
    const char *subtitle;
} gal_transition_t;

typedef struct {
    const char *eyebrow;
    const char *title;
    const char *body;
} gal_ending_t;

// ---------------------------------------------------------------- 鉴赏室与章节
typedef struct {
    const char *label;      // CG 01
    const char *caption;    // 篇章 + 差分序号
    unsigned short unlock_event;   // GAL_EVENT_NONE = 初始解锁
    unsigned short media;   // 媒体包图像下标（全图，320x180 内居中）
    bool unlocked;          // 运行时填充
    const void *image;      // 运行时指向媒体描述符
} gal_cg_t;

typedef struct {
    const char *name;
    const char *summary;
    const char *progress;
    unsigned short unlock_event;   // 到达该事件永久解锁该章
    uint8_t require_flag;
    bool unlocked;          // 运行时填充
} gal_chapter_t;

typedef struct {
    const char *label;
    const char *value;
} gal_about_row_t;

// ---------------------------------------------------------------- CG 回想与音乐
typedef struct {
    const char *title;
    const char *chapter;
    uint8_t cg_index;         // 需要该 CG 已解锁
    unsigned short event_index;   // 从该事件开始重看（试读，不计通关）
} gal_recall_t;

#define GAL_NOTE_REST 0xFF

typedef struct {
    const char *title;
    const char *duration;     // 显示用时长
    uint8_t cg_index;         // GAL_UNLOCK_NONE = 不需要 CG
    const uint8_t *notes;     // 框架方波音源（本作用媒体包，保留字段）
    uint8_t note_count;
    uint8_t media;            // 媒体包音轨下标（0 = 标题曲）
} gal_track_t;

#define GAL_CONTENT_TITLE    "爱上火车"
#define GAL_CONTENT_SUBTITLE "二六线 · 全四章"
#define GAL_CONTENT_LOGO_SUB "MAITETSU · NIIROKU"
#define GAL_CONTENT_VERSION  "v0.1.0"

extern const void *gal_content_title_image;

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

extern const char *const gal_content_title_menu[];
extern const int gal_content_title_menu_count;
extern const char *const gal_content_quick_menu[];
extern const int gal_content_quick_menu_count;
extern const char *const gal_content_gallery_menu[];
extern const int gal_content_gallery_menu_count;

enum {
    GAL_GALLERY_CG = 0,
    GAL_GALLERY_RECALL,
    GAL_GALLERY_STORY,
    GAL_GALLERY_MUSIC,
};

enum {
    GAL_TITLE_MENU_NEW = 0,
    GAL_TITLE_MENU_CONTINUE,
    GAL_TITLE_MENU_LOAD,
    GAL_TITLE_MENU_SETTINGS,
    GAL_TITLE_MENU_GALLERY,
    GAL_TITLE_MENU_ABOUT,
};

enum {
    GAL_MENU_RESUME = 0,
    GAL_MENU_SAVE,
    GAL_MENU_LOAD,
    GAL_MENU_HISTORY,
    GAL_MENU_SETTINGS,
    GAL_MENU_TITLE,
};

#define GAL_CONTENT_ABOUT \
    "本固件是《爱上火车》二六线的离线阅读版，不含原作其他路线。\n" \
    "\n" \
    "范围：正和四十一年·小仓、御一夜旅行、小仓风暴、尾声\n" \
    "四章，共 1,537 段、675 个场景、6 组 35 张 CG。\n" \
    "\n" \
    "素材来源：xuanyide01/maitetsu-rw5（固定提交），\n" \
    "原作 Lose，HIKARI FIELD 发行；文本、图像、标志与音乐\n" \
    "的权利归各权利人所有，本项目只做离线移植。\n" \
    "\n" \
    "操作：短按 OK / 上 / 下 均为推进正文；长按 OK 打开菜单；\n" \
    "长按上快进（松手停），长按下切换自动阅读。\n" \
    "\n" \
    "存档：3 个手动槽位 + 自动进度；章节与 CG 回想属于试读，\n" \
    "只有完整读完四章才记录永久通关。"
