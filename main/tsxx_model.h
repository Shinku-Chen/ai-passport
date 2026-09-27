// main/tsxx_model.h —— 阅读器的纯逻辑:翻屏、翻页、选项、章节跳转、分页排版。
//
// 这一层不碰 ESP-IDF 与 LVGL,只依赖 tsxx_pack 的只读视图,方便在宿主机上直接跑测试
// (tests/test_tsxx_model.c 用真实资源包走完整条剧情线)。
//
// 剧本是一张线性页表:推进 = 页序号 +1;选项 = 跳到绝对页号;章节点由 [CHAPTER x-y]
// 标记推导。没有场景层,也没有结局分支表 —— 走到页表末尾就是结束。
#pragma once

#include "tsxx_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 单页正文缓冲区:源数据最长一页 255 字(约 765 字节),留够余量。
#define TSXX_TEXT_BUFFER 1024
// 一页最多分几屏(源数据一页最多 255 字,16 单位 x 5 行时最多 26 屏)。
#define TSXX_MAX_SCREENS 32
// 章节跳转点上限(源数据 45 个)。
#define TSXX_MAX_CHAPTERS 64

// 排版参数:一行多少"半角单位"(CJK 算 2),一屏几行。字号变化时由应用层换算。
typedef struct {
    int units_per_line;
    int lines_per_page;
} tsxx_layout_t;

typedef struct {
    uint32_t page;         // 当前页(全局下标)
    uint8_t screen;        // 当前页内的第几屏(0 起)
    uint8_t screens;       // 当前页文本分了几屏(0 = 本页没有正文)
    uint8_t at_choice;     // 停在选项上,等玩家选择
    uint8_t ended;         // 已读完(页表末尾)
    uint8_t choice_count;  // at_choice 时的选项数
} tsxx_player_t;

typedef enum {
    TSXX_STEP_SCREEN = 0,  // 同一页里翻到下一屏
    TSXX_STEP_PAGE,        // 进入新的一页(可能要换背景/立绘/事件图)
    TSXX_STEP_CHOICE,      // 进入选项,等玩家选
    TSXX_STEP_END,         // 抵达页表末尾
    TSXX_STEP_STUCK,       // 没有可推进的内容(停在选项或已结束)
} tsxx_step_t;

typedef struct {
    uint32_t page;
    uint32_t screen;
} tsxx_save_t;

typedef struct {
    uint16_t name_id;      // 说话人表下标(标签形如 [CHAPTER x-y])
    uint32_t page;         // 该标记第一次出现的页
} tsxx_chapter_t;

void tsxx_player_reset(tsxx_player_t *player);

// 从指定页开始阅读。页号越界会被夹到合法范围。返回是否成功。
bool tsxx_player_start(tsxx_player_t *player, const tsxx_pack_t *pack, uint32_t page,
                       const tsxx_layout_t *layout);

// 推进:翻屏 -> 翻页 -> 选项/结束。返回发生了什么。
tsxx_step_t tsxx_player_advance(tsxx_player_t *player, const tsxx_pack_t *pack,
                                const tsxx_layout_t *layout);

// 选择选项(仅在 at_choice 时有效),跳到该选项的目标页。
bool tsxx_player_choose(tsxx_player_t *player, const tsxx_pack_t *pack, uint8_t index,
                        const tsxx_layout_t *layout);

// 跳到指定页(章节跳转 / 读档),保留正文分屏信息。
bool tsxx_player_jump(tsxx_player_t *player, const tsxx_pack_t *pack, uint32_t page,
                      const tsxx_layout_t *layout);

// 跳过本章剩余内容:一直推进到下一个章节点、遇见选项、或抵达末尾为止。
// 选项与末尾一定会停 —— 这就是"跳过章节"的安全边界。
#define TSXX_SKIP_MAX_STEPS 4096u
tsxx_step_t tsxx_player_skip_chapter(tsxx_player_t *player, const tsxx_pack_t *pack,
                                     const tsxx_layout_t *layout);

// 当前页整段文本;返回写入字节数(不含 NUL)。
size_t tsxx_player_text(const tsxx_player_t *player, const tsxx_pack_t *pack, char *out,
                        size_t capacity);

// 当前屏的文本(自动按 layout 分页);返回写入字节数。
size_t tsxx_player_screen_text(const tsxx_player_t *player, const tsxx_pack_t *pack,
                               const tsxx_layout_t *layout, char *out, size_t capacity);

// 当前说话人名字;旁白返回 0。
size_t tsxx_player_speaker(const tsxx_player_t *player, const tsxx_pack_t *pack, char *out,
                           size_t capacity);

// 当前页的事件图渲染配方。没有事件图返回 false。
bool tsxx_player_cg(const tsxx_player_t *player, const tsxx_pack_t *pack, tsxx_cg_t *out);

// 按绝对页号取事件图(章节跳转/读档后需要重算时用)。
bool tsxx_pack_cg_of_page(const tsxx_pack_t *pack, uint32_t page, tsxx_cg_t *out);

// 把一整段 UTF-8 切成若干屏。offsets 需要 max_screens+1 项,函数写入 screens+1 个
// 字节偏移(最后一项 = 文本总长度)。返回屏数;文本为空返回 0。
//
// 宽度模型:CJK/全角 = 2 单位,ASCII/半角 = 1 单位。换行规则:
//   - '\n' 强制断行;连续空行保留;
//   - 行尾遇到"不能出现在行首"的标点(。、，．！？：；」』）等)时把它拽回上一行;
//   - 行首遇到"不能出现在行尾"的标点(「『（等)时把它推到下一行。
int tsxx_text_screens(const char *utf8, const tsxx_layout_t *layout, uint32_t *offsets,
                      int max_screens);

// 单个字符的显示宽度(半角单位)。
int tsxx_char_units(uint32_t codepoint);

// 扫描页表,收集所有 [CHAPTER ...] 标记的第一次出现,按页序写入 out。
// 返回收集到的章节点数(最多 max 个)。
int tsxx_chapters_scan(const tsxx_pack_t *pack, tsxx_chapter_t *out, int max);

// 存档编解码(可往返)。
size_t tsxx_save_encode(const tsxx_save_t *save, uint8_t *out, size_t capacity);
bool tsxx_save_decode(tsxx_save_t *save, const uint8_t *data, size_t len);
