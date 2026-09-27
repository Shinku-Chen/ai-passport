// main/limelight_model.h —— 阅读器纯逻辑:对白推进、分页、选项、存档序列化。
//
// 这一层不碰 ESP-IDF、LVGL 与显示屏,只依赖 limelight_script 的只读视图,
// 因此可以在宿主机上直接跑测试(tests/test_limelight_model.c 用真实剧本包跑完整条线)。
//
// 与《星空列车》版的差别:本作剧本是**平铺 id**(1..68229 连续,选项跳转给的是绝对
// id),没有"章节/场景"两级结构。章节只是源数据里说话人为 "[CHAPTERx-y]" 的标记,
// 用于显示进度与按章跳转。
#pragma once

#include "limelight_script.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一屏最多几页;单句文本缓冲(源脚本最长一句 539 个字符,UTF-8 最多 1617 字节,留足余量)。
// 最长一句 539 字 → 13 字/行 = 42 行 → 9 页,留一倍余量(页表只有几十字节)。
#define LIME_MAX_PAGES 16
#define LIME_TEXT_BUFFER 2048
#define LIME_TEXT_MAX_LINES 64
#define LIME_CHOICE_HISTORY 8

// 排版参数:一行多少"半角单位"(全角算 2)、一屏几行。
// 默认 16px 正文:13 个全角字 = 26 单位,行距 20px,一屏 5 行
// (与 main/atri_ui.h 的 ATRI_LINE_H / ATRI_TEXT_LINES 一致)。
typedef struct {
    int units_per_line;
    int lines_per_page;
} lime_layout_t;

typedef struct {
    uint32_t id;             // 当前对白 id(1 起)
    uint16_t page;           // 当前页(0 起)
    uint16_t page_count;
    uint32_t chapter;        // 章节下标,用于显示"第 n 章"
    uint16_t bg, sprite, cg, speaker;   // 名字下标(LIME_NAME_NONE = 这一句没有)
    uint8_t z;               // 源数据的演出标记(0/1/2),界面用它决定立绘的缩放
    uint8_t at_choice;       // 停在选项上,等玩家选
    uint8_t choice_count;
    uint8_t ended;           // 已经到了最后一条
    uint8_t choice_len;
    uint8_t choice_pick[LIME_CHOICE_HISTORY];   // 选过的选项(存档要带)
} lime_player_t;

typedef enum {
    LIME_STEP_TEXT = 0,      // 同一句内翻页 / 换句
    LIME_STEP_CHAPTER,       // 跨过了章节标记
    LIME_STEP_CHOICE,        // 进入选项
    LIME_STEP_ENDING,        // 抵达最后一句
    LIME_STEP_STUCK,         // 数据异常,没有可推进的内容
} lime_step_t;

typedef struct {
    uint32_t id;
    uint16_t page;
    uint8_t choice_len;
    uint8_t choice_pick[LIME_CHOICE_HISTORY];
} lime_save_t;

// 字符宽度单位:全角 2、半角 1。排版与绘制都按这一张表算宽度。
int lime_char_units(uint32_t codepoint);

void lime_player_reset(lime_player_t *player);

// 从指定 id 开始阅读(id 会被夹到 [1, entries])。
bool lime_player_start(lime_player_t *player, lime_script_t *script, uint32_t id,
                       const lime_layout_t *layout);

// 推进:翻页 -> 下一句 -> 下一章标记 -> 结束。返回发生了什么。
lime_step_t lime_player_advance(lime_player_t *player, lime_script_t *script,
                                const lime_layout_t *layout);

// 回退一句(源移植版的"返回上一句")。已经在第一句返回 false。
bool lime_player_back(lime_player_t *player, lime_script_t *script,
                      const lime_layout_t *layout);

// 选择选项(仅在 at_choice 时有效):跳到该选项的目标 id。
bool lime_player_choose(lime_player_t *player, lime_script_t *script, uint8_t index,
                        const lime_layout_t *layout);

// 跳到某个 id(章节跳转/读档用)。保留已选历史。
bool lime_player_jump(lime_player_t *player, lime_script_t *script, uint32_t id,
                      const lime_layout_t *layout);

// 当前对白整段文本(拷贝到 out);返回写入字节数(不含 NUL)。
size_t lime_player_text(const lime_player_t *player, lime_script_t *script, char *out,
                        size_t capacity);

// 当前页的文本(按 layout 分页);返回写入字节数。
size_t lime_player_page_text(const lime_player_t *player, lime_script_t *script,
                             const lime_layout_t *layout, char *out, size_t capacity);

// 当前说话人名字;返回写入字节数(没有说话人时写空串)。
size_t lime_player_speaker(const lime_player_t *player, lime_script_t *script, char *out,
                           size_t capacity);

// 立绘该不该画:这一句有立绘名就画(源移植版就是这样 —— 立绘压在 CG 之上)。
bool lime_player_show_sprite(const lime_player_t *player);

// 把一整段 UTF-8 切成若干屏。offsets 需要 max_offsets 项,写入 page_count+1 个字节
// 偏移(最后一项 = 文本总长度)。返回页数。行尾遇禁则标点会把它拽回上一行。
int lime_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                    uint32_t *offsets, int max_offsets);

// 同一套规则的逐行版本。offsets 需要 max_lines 项,返回真实行数。
int lime_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines);

// 打字机逐字显示:返回 pos 处字符的下一个字节边界(不切开多字节字符)。
size_t lime_utf8_next_boundary(const char *utf8, size_t len, size_t pos);
uint32_t lime_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out);

// 从章节标签里解析章号:源数据的标签形如 "[CHAPTER7-1]" 或 "CHAPTER7-1",
// 返回其中第一段数字(7);解析不出返回 0。
int lime_chapter_number(const char *label, size_t length);
// 解析 "CHAPTER10-3" 这类章节标记,取出 "章-节" 两个数(返回是否有章号)。
bool lime_chapter_pair(const char *label, size_t length, int *major, int *minor);

// 存档序列化(供 NVS;编解码可往返)。
size_t lime_save_encode(const lime_save_t *save, uint8_t *out, size_t capacity);
bool lime_save_decode(lime_save_t *save, const uint8_t *data, size_t len);
void lime_save_from_player(const lime_player_t *player, lime_save_t *out);
