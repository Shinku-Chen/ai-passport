// main/saya_model.h —— 阅读器的纯逻辑:章节/场景/对白推进、分页、存档序列化。
//
// 这一层不碰 ESP-IDF 与 LVGL,只依赖 saya_pack 的只读视图,方便在宿主机上直接
// 跑测试(tests/test_saya_model.c 用真实资源包走完整条剧情线)。
#pragma once

#include "saya_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一屏最多几页、历史选择最多记几项。源脚本最长一句 150 字,按 16px 字号约 2 页。
#define SAYA_MAX_PAGES 8
#define SAYA_CHOICE_HISTORY 8
// 单句文本缓冲区:源脚本最长 364 字节,留够余量。
#define SAYA_TEXT_BUFFER 512

// 排版参数:一行多少"半角单位"(CJK 算 2)、一屏几行。字号不同时由应用层换算。
typedef struct {
    int units_per_line;
    int lines_per_page;
} saya_layout_t;

typedef struct {
    uint16_t chapter;    // 章节下标(不是源脚本编号)
    uint16_t scene;      // 章节内场景下标
    uint16_t dialogue;   // 场景内对白下标
    uint16_t page;       // 当前页
    uint16_t page_count; // 当前对白页数
    uint16_t fg;         // 当前立绘下标(SAYA_NONE = 无)
    uint8_t at_choice;   // 停在选项上,等玩家选择
    uint8_t ended;       // 已到结局
    uint16_t end_name;   // 结局名(name 表下标)
    uint8_t choice_len;
    uint8_t choice_pick[SAYA_CHOICE_HISTORY];
} saya_player_t;

typedef enum {
    SAYA_STEP_TEXT = 0,   // 同一场景内翻页 / 换句
    SAYA_STEP_SCENE,      // 换场景(需要换背景与立绘)
    SAYA_STEP_CHOICE,     // 进入选项
    SAYA_STEP_ENDING,     // 抵达结局
    SAYA_STEP_CHAPTER,    // 换章
    SAYA_STEP_STUCK,      // 没有可推进的内容(数据异常)
} saya_step_t;

typedef struct {
    uint16_t chapter;
    uint16_t scene;
    uint16_t dialogue;
    uint16_t fg;
    uint8_t choice_len;
    uint8_t choice_pick[SAYA_CHOICE_HISTORY];
} saya_save_t;

void saya_player_reset(saya_player_t *player);

// 从指定章节的第一幕开始阅读。章节下标非法时返回 false。
bool saya_player_start(saya_player_t *player, const saya_pack_t *pack, uint16_t chapter,
                       const saya_layout_t *layout);

// 推进:翻页 -> 下一句 -> 下一幕 -> 下一章 -> 结局。返回发生了什么。
saya_step_t saya_player_advance(saya_player_t *player, const saya_pack_t *pack,
                                const saya_layout_t *layout);

// 选择选项(仅在 at_choice 时有效)。
bool saya_player_choose(saya_player_t *player, const saya_pack_t *pack, uint8_t index,
                        const saya_layout_t *layout);

// 跳过当前章节:本章还有未经过的选项就跳到那个选项(不替玩家做决定),
// 否则跳到下一章开头。本章没有后续(结局章)或已停在选项上时返回 false。
bool saya_player_skip_chapter(saya_player_t *player, const saya_pack_t *pack,
                              const saya_layout_t *layout);

// 从存档恢复到指定章节/场景/对白。失败(存档指向已失效的位置)返回 false。
bool saya_player_load(saya_player_t *player, const saya_pack_t *pack, const saya_save_t *save,
                      const saya_layout_t *layout);

// 当前对白整段文本;返回写入字节数(不含 NUL)。
size_t saya_player_text(const saya_player_t *player, const saya_pack_t *pack, char *out,
                        size_t capacity);

// 当前对白当前页的文本(自动按 layout 分页);返回写入字节数。
size_t saya_player_page_text(const saya_player_t *player, const saya_pack_t *pack,
                             const saya_layout_t *layout, char *out, size_t capacity);

// 当前说话人名字;返回写入字节数。
size_t saya_player_speaker(const saya_player_t *player, const saya_pack_t *pack, char *out,
                           size_t capacity);

// 把一整段 UTF-8 切成若干屏。offsets 需要 max_offsets 项,函数写入 page_count+1 个
// 字节偏移(第 page_count 项 = 文本总长度)。返回页数。
// 宽度模型:CJK/全角 = 2 单位,ASCII/半角片假名 = 1 单位。行尾遇到禁则标点会把它
// 拽回上一行,避免标点孤立在行首。
int saya_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                    uint32_t *offsets, int max_offsets);

// 存档序列化(供 NVS 保存;编解码可往返)。
size_t saya_save_encode(const saya_save_t *save, uint8_t *out, size_t capacity);
bool saya_save_decode(saya_save_t *save, const uint8_t *data, size_t len);
