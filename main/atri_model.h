// main/atri_model.h —— 阅读器的纯逻辑:章节/场景/对白推进、分页、选择历史、存档序列化。
//
// 这一层不碰 ESP-IDF 与 LVGL,只依赖 atri_pack 的只读视图,方便在宿主机上直接跑测试
// (tests/test_atri_model.c 用真实资源包走完整条剧情线)。
#pragma once

#include "atri_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一屏最多几页、历史选择最多记几项(源脚本每章最多 3 次选择)。
#define ATRI_MAX_PAGES 8
#define ATRI_CHOICE_HISTORY 8
// 单句文本缓冲区:源脚本最长一句 144 个汉字(约 432 字节),留够余量。
#define ATRI_TEXT_BUFFER 640

// 排版参数:一行多少"半角单位"(CJK 算 2)、一屏几行。字号变化时由应用层换算。
typedef struct {
    int units_per_line;
    int lines_per_page;
} atri_layout_t;

typedef struct {
    uint16_t chapter;    // 章节下标(不是源脚本编号)
    uint16_t scene;      // 章节内场景下标
    uint16_t dialogue;   // 场景内对白下标
    uint16_t page;       // 当前页
    uint16_t page_count; // 当前对白页数
    uint8_t at_choice;   // 停在选项上,等玩家选择
    uint8_t ended;       // 已到结局
    uint16_t end_name;   // 结局名(name 表下标;ATRI_NONE = 未知)
    uint8_t choice_len;
    uint8_t choice_pick[ATRI_CHOICE_HISTORY];
} atri_player_t;

typedef enum {
    ATRI_STEP_TEXT = 0,   // 同一场景内翻页 / 换句
    ATRI_STEP_SCENE,      // 换场景(要换背景与叠加)
    ATRI_STEP_CHOICE,     // 进入选项
    ATRI_STEP_ENDING,     // 抵达结局
    ATRI_STEP_CHAPTER,    // 换章(下一章与本章可能是同一段剧情线,由调用方决定是否显示过场)
    ATRI_STEP_STUCK,      // 没有可推进的内容(数据异常)
} atri_step_t;

typedef struct {
    uint16_t chapter;
    uint16_t scene;
    uint16_t dialogue;
    uint8_t choice_len;
    uint8_t choice_pick[ATRI_CHOICE_HISTORY];
} atri_save_t;

void atri_player_reset(atri_player_t *player);

// 从指定章节的第一幕开始阅读。章节下标非法时返回 false。
bool atri_player_start(atri_player_t *player, const atri_pack_t *pack, uint16_t chapter,
                       const atri_layout_t *layout);

// 推进:翻页 -> 下一句 -> 下一幕 -> 选择分流/下一章 -> 结局。返回发生了什么。
atri_step_t atri_player_advance(atri_player_t *player, const atri_pack_t *pack,
                                const atri_layout_t *layout);

// 选择选项(仅在 at_choice 时有效),按源脚本语义跳到 当前场景 + choice_jump。
bool atri_player_choose(atri_player_t *player, const atri_pack_t *pack, uint8_t index,
                        const atri_layout_t *layout);

// 跳过当前场景(仅在"本场景还有下一幕且没有选项/结局/跳转"时允许)。
bool atri_player_skip_scene(atri_player_t *player, const atri_pack_t *pack,
                            const atri_layout_t *layout);

// 跳过本章剩余内容:一直推进到换章 / 遇见选项 / 抵达结局为止。
// 返回停下时的推进结果(ATRI_STEP_CHAPTER 表示已经进入下一章)。
// 选项与结局一定会停 —— 这就是"跳过章节"的安全边界。
#define ATRI_SKIP_CHAPTER_MAX_STEPS 20000
atri_step_t atri_player_skip_chapter(atri_player_t *player, const atri_pack_t *pack,
                                     const atri_layout_t *layout);

// 从存档恢复到指定章节/场景/对白。失败(存档指向已失效的位置)返回 false。
bool atri_player_load(atri_player_t *player, const atri_pack_t *pack, const atri_save_t *save,
                      const atri_layout_t *layout);
// 当前对白整段文本;返回写入字节数(不含 NUL)。
size_t atri_player_text(const atri_player_t *player, const atri_pack_t *pack, char *out,
                        size_t capacity);

// 当前对白当前页的文本(自动按 layout 分页);返回写入字节数。
size_t atri_player_page_text(const atri_player_t *player, const atri_pack_t *pack,
                             const atri_layout_t *layout, char *out, size_t capacity);

// 当前说话人名字;返回写入字节数(旁白返回 0)。
size_t atri_player_speaker(const atri_player_t *player, const atri_pack_t *pack, char *out,
                           size_t capacity);

// 当前场景的视图信息(背景/叠加/选项文案)。玩家状态非法时返回 false。
bool atri_player_scene_view(const atri_player_t *player, const atri_pack_t *pack,
                            atri_scene_t *out);

// 当前章节的标志位(ATRI_CH_*);非法章节返回 0。
uint8_t atri_player_chapter_flags(const atri_player_t *player, const atri_pack_t *pack);

// 把一整段 UTF-8 切成若干屏。offsets 需要 max_offsets 项,函数写入 page_count+1 个
// 字节偏移(第 page_count 项 = 文本总长度)。返回页数。
// 宽度模型:CJK/全角 = 2 单位,ASCII/半角片假名 = 1 单位。行尾遇到禁则标点会把它
// 拽回上一行,避免标点孤立在行首。
int atri_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                    uint32_t *offsets, int max_offsets);

// 存档序列化(供 NVS 保存;编解码可往返)。
size_t atri_save_encode(const atri_save_t *save, uint8_t *out, size_t capacity);
bool atri_save_decode(atri_save_t *save, const uint8_t *data, size_t len);
