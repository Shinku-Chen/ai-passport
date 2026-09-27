// main/dracu_model.h —— 《DRACU-RIOT!》阅读状态机与排版(纯逻辑,不依赖 ESP-IDF / LVGL)。
//
// 源工程把整部作品压成一条 52,787 页的线性页表,所有"偏离线性"的地方都由
// branchConfig 描述(前进特判 / 回退堵死 / 条件路由 / 结局页)。这一层就把那四个
// 机制翻译成"推进、回退、选择、跳过章节"四个动作,并在换页时按字号分页。
//
// 这一层只依赖 dracu_scn 的只读视图,所以可以在宿主机上跑测试
// (tests/test_dracu_model.c 用真实剧本包走完整条主线与五条路线)。
#pragma once

#include "dracu_scn.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一页最多切几屏(最长一句 144 字,16px 字一屏 75 字,实测最多 2 屏;留足余量)
#define DRACU_MAX_PAGES 8
// 存档格式版本
#define DRACU_SAVE_VERSION 1u

typedef enum {
    DRACU_STEP_TEXT = 0,   // 同一页还有下一屏
    DRACU_STEP_PAGE,       // 已经换到新的一页
    DRACU_STEP_CHOICE,     // 当前页是选项页,等玩家选择
    DRACU_STEP_ENDING,     // 当前页是结局页
    DRACU_STEP_STUCK,      // 数据异常(页面越界 / 解压失败),停住并给出 error
} dracu_step_t;

// 排版参数:一行多少"半角单位"(CJK 算 2)、一屏几行。字号变化时由应用层换算。
typedef struct {
    int units_per_line;
    int lines_per_page;
} dracu_layout_t;

typedef struct {
    uint32_t page;                  // 当前页号(1 起)
    dracu_page_t record;            // 当前页记录(背景 / 事件图 / 立绘 / 说话人)
    char text[DRACU_TEXT_BUFFER];   // 当前页正文(UTF-8)
    uint32_t page_offsets[DRACU_MAX_PAGES];
    uint16_t page_index;            // 当前是第几屏(0 起)
    uint16_t page_count;            // 本页共几屏(0 = 本页没有正文)
    uint8_t choice[DRACU_COND_MAX]; // 条件页的选择历史(下标 = dracu_scn_cond_index)
    uint32_t chapter;               // 当前章节下标;0xFFFFFFFF = 还没进任何章节
    bool at_choice;                 // 停在选项页上,等玩家选择
    dracu_choice_t choice_view;     // 当前选项页的选项
    bool ended;                     // 当前页是结局页
    uint16_t end_name;              // 结局名(字符串表下标;ATRI_NONE 语义 = DRACU_SCN_NONE16)
    const char *error;              // 最近一次失败原因(静态字符串)
} dracu_player_t;

// 存档:位置 + 章节 + 选择历史(选项影响后续路由,必须一起存)
typedef struct {
    uint32_t page;
    uint32_t chapter;
    uint8_t choice[DRACU_COND_MAX];
} dracu_save_t;

#define DRACU_CHAPTER_NONE 0xFFFFFFFFu

void dracu_player_reset(dracu_player_t *player);

// 从指定页开始阅读(清空选择历史)。页号非法时返回 false 并停止。
bool dracu_player_start(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                        const dracu_layout_t *layout);

// 直接从 (页号, 选择历史) 恢复(读档用)。
bool dracu_player_resume(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                         const uint8_t *choice, const dracu_layout_t *layout);

// 推进:先翻本页的下一屏,再按「结局 -> 条件路由 -> 前进特判 -> 下一页」换页。
dracu_step_t dracu_player_advance(dracu_player_t *player, dracu_scn_t *scn,
                                  const dracu_layout_t *layout);

// 回退:先回本页的上一屏,再按「回退堵死 -> 上一页」退页。
dracu_step_t dracu_player_back(dracu_player_t *player, dracu_scn_t *scn,
                               const dracu_layout_t *layout);

// 选择第 index 个选项(0 起)。只在 at_choice 时有效;命中会记录选择历史并跳到目标页。
bool dracu_player_choose(dracu_player_t *player, dracu_scn_t *scn, uint8_t index,
                         const dracu_layout_t *layout);

// 跳到指定页(章节列表 / 调试跳转用),保留选择历史。
bool dracu_player_goto(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                       const dracu_layout_t *layout);

// 跳过当前章节:一直推进到下一个章节的起始页(或结局 / 选项)。
dracu_step_t dracu_player_skip_chapter(dracu_player_t *player, dracu_scn_t *scn,
                                       const dracu_layout_t *layout);

// 当前屏 / 整页正文 / 说话人(UTF-8,NUL 结尾);返回写入字节数。
size_t dracu_player_page_text(const dracu_player_t *player, char *out, size_t capacity);
size_t dracu_player_text(const dracu_player_t *player, char *out, size_t capacity);
size_t dracu_player_speaker(const dracu_player_t *player, dracu_scn_t *scn, char *out,
                            size_t capacity);

// 最近一次失败的原因(静态字符串,单线程使用)。
const char *dracu_get_error(void);

// 页号所属的章节下标(章表里最后一个起点 <= 页号的条目);没有返回 DRACU_CHAPTER_NONE。
uint32_t dracu_chapter_of_page(const dracu_scn_t *scn, uint32_t page);

// 从存档快照。
void dracu_save_from_player(const dracu_player_t *player, dracu_save_t *out);

// 存档序列化(供 NVS 保存;编解码可往返)。
size_t dracu_save_encode(const dracu_save_t *save, uint8_t *out, size_t capacity);
bool dracu_save_decode(dracu_save_t *save, const uint8_t *data, size_t len);

// ---- 纯排版逻辑(宿主机可测)------------------------------------------------
int dracu_char_units(uint32_t codepoint);
int dracu_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines);
int dracu_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                     int max_offsets);
size_t dracu_utf8_next_boundary(const char *utf8, size_t len, size_t pos);
uint32_t dracu_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out);
