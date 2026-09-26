// main/starry_model.h —— 阅读器的纯逻辑:章节/场景/对白推进、分页、存档序列化。
//
// 这一层不碰 ESP-IDF、LVGL 与显示屏,只依赖 starry_pack 的只读视图,方便在宿主机上
// 直接跑测试(tests/test_starry_model.c 用真实资源包走完整条剧情线)。
//
// 与《沙耶之歌》版的差别:本作的选项跳的是"同一章内的某个场景"(源数据是
// 当前场景 + nextScene 的相对下标),所以场景记录里存的是全局场景下标,
// 由 starry_player_choose() 换算成章内位置。
#pragma once

#include <stdint.h>

#include "starry_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一屏最多几页、历史选择最多记几项。本作最长一句 74 字,按 16px 字号最多 2 页。
#define STARRY_MAX_PAGES 8
#define STARRY_CHOICE_HISTORY 8
// 单句文本缓冲区:源脚本最长一句 222 字节,留够余量。
#define STARRY_TEXT_BUFFER 512
// 一行一断的换行结果最多记这么多行(512 字节正文最多约 40 行)。
#define STARRY_TEXT_MAX_LINES 64

// 一屏行数与行宽(半角单位,全角算 2)。数值与 main/starry_render.h 的字号/行高、
// tools/starry_font.py 的 px -> 行高一一对应:
//   16px 正文:13 个全角字 = 208px -> 26 单位,行距 19px,一屏 5 行
//   20px 正文:10 个全角字 = 200px -> 20 单位,行距 24px,一屏 4 行
// (行距/行数见 main/starry_render.h 的 STARRY_*_LINE_PITCH 与 STARRY_*_LINES_PER_PAGE,
//  文本框高 106px:5x18=90 / 3x25=75,都留得下边距。)
// 留出的 16px / 20px 余量是给"禁则"用的:行尾遇到不能起头的标点时,它会被挂在本行
// 行尾(最多超出一个全角字),渲染端的裁剪上限因此是 216 + px/2 = 224px(见 starry_render.h)。
#define STARRY_SMALL_UNITS_PER_LINE 26
#define STARRY_LARGE_UNITS_PER_LINE 20
#define STARRY_SMALL_LINES_PER_PAGE 5
#define STARRY_LARGE_LINES_PER_PAGE 4

// 排版参数:一行多少"半角单位"(全角算 2)、一屏几行。字号不同时由应用层换算。
// units_per_line × (px/2) 必须不超过文本框可用宽度(见 starr_render.h)。
typedef struct {
    int units_per_line;
    int lines_per_page;
} starry_layout_t;

typedef struct {
    uint16_t chapter;    // 章节下标(不是源脚本编号)
    uint16_t scene;      // 章节内场景下标
    uint16_t dialogue;   // 场景内对白下标
    uint16_t page;       // 当前页
    uint16_t page_count; // 当前对白页数
    uint16_t bg;         // 当前背景下标(已消化 STARRY_BG_KEEP)
    uint16_t sprite;     // 当前立绘下标(STARRY_NONE = 无)
    uint8_t at_choice;   // 停在选项上,等玩家选择
    uint8_t ended;       // 已到结局
    uint16_t end_name;   // 结局名(name 表下标)
    uint8_t choice_len;
    uint8_t choice_pick[STARRY_CHOICE_HISTORY];
} starry_player_t;

typedef enum {
    STARRY_STEP_TEXT = 0,   // 同一场景内翻页 / 换句
    STARRY_STEP_SCENE,      // 换场景(需要换背景与立绘)
    STARRY_STEP_CHOICE,     // 进入选项
    STARRY_STEP_ENDING,     // 抵达结局
    STARRY_STEP_CHAPTER,    // 换章
    STARRY_STEP_STUCK,      // 没有可推进的内容(数据异常)
} starry_step_t;

typedef struct {
    uint16_t chapter;
    uint16_t scene;
    uint16_t dialogue;
    uint16_t bg;
    uint16_t sprite;
    uint8_t choice_len;
    uint8_t choice_pick[STARRY_CHOICE_HISTORY];
} starry_save_t;

// 字符宽度单位:全角 2、半角 1。排版、绘制、字形包三处都按这一张表算宽度。
int starry_char_units(uint32_t codepoint);

void starry_player_reset(starry_player_t *player);

// 从指定章节的第一幕开始阅读。章节下标非法时返回 false。
// 当前这一句该不该显示立绘(三条"宁可不画"的规则,UI 只负责把它交给渲染器):
//   1) 事件 CG / 纯色幕(打包器按源素材 evcg* 与 bg_black/red/white 打了 no_sprite 标志)
//      与结局画面上不叠立绘;
//   2) 只有立绘归属的角色本人在说话时才显示,旁白(没有说话人)和他人说话都收起来;
//   3) 归属不明(源数据里从没有名字引用过这张立绘)一律不显示,避免串场。
uint16_t starry_player_visible_sprite(const starry_player_t *player, const starry_pack_t *pack);

bool starry_player_start(starry_player_t *player, const starry_pack_t *pack, uint16_t chapter,
                         const starry_layout_t *layout);

// 推进:翻页 -> 下一句 -> 下一幕 -> 下一章 -> 结局。返回发生了什么。
starry_step_t starry_player_advance(starry_player_t *player, const starry_pack_t *pack,
                                    const starry_layout_t *layout);

// 选择选项(仅在 at_choice 时有效)。
bool starry_player_choose(starry_player_t *player, const starry_pack_t *pack, uint8_t index,
                          const starry_layout_t *layout);

// 跳过当前章节:直接跳到下一章的第一幕(没有下一章时返回 false,停在原地)。
// 菜单里的"跳过章节"用它 —— 不要求当前场景满足跳过条件,选项上也能跳。
bool starry_player_skip_chapter(starry_player_t *player, const starry_pack_t *pack,
                                const starry_layout_t *layout);

// 跳过当前场景(仅在"本场景还有下一幕且没有选项/结局/跳转"时允许)。
bool starry_player_skip_scene(starry_player_t *player, const starry_pack_t *pack,
                              const starry_layout_t *layout);

// 从存档恢复到指定章节/场景/对白。失败(存档指向已失效的位置)返回 false。
bool starry_player_load(starry_player_t *player, const starry_pack_t *pack,
                        const starry_save_t *save, const starry_layout_t *layout);

// 当前对白整段文本;返回写入字节数(不含 NUL)。
size_t starry_player_text(const starry_player_t *player, const starry_pack_t *pack, char *out,
                          size_t capacity);

// 当前对白当前页的文本(自动按 layout 分页);返回写入字节数。
size_t starry_player_page_text(const starry_player_t *player, const starry_pack_t *pack,
                               const starry_layout_t *layout, char *out, size_t capacity);

// 当前说话人名字;返回写入字节数。
size_t starry_player_speaker(const starry_player_t *player, const starry_pack_t *pack, char *out,
                             size_t capacity);

// 把一整段 UTF-8 切成若干屏。offsets 需要 max_offsets 项,函数写入 page_count+1 个
// 字节偏移(第 page_count 项 = 文本总长度)。返回页数。
// 宽度模型:CJK/全角 = 2 单位,ASCII/半角片假名 = 1 单位。行尾遇到禁则标点会把它
// 拽回上一行,避免标点孤立在行首。
int starry_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                      uint32_t *offsets, int max_offsets);

// 同一套换行规则的"逐行"版本(绘制正文用):offsets 需要 max_lines 项,写入每行起
// 始字节偏移(最后一项 = 文本总长)。返回值是真实行数,可能大于 max_lines。
int starry_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines);

// 打字机逐字显示:返回 pos 处字符的下一个字节边界(不会切开多字节字符)。
size_t starry_utf8_next_boundary(const char *utf8, size_t len, size_t pos);

// 解码 pos 处的码位(next_out 写入下一个字节边界);非法字节返回 U+FFFD。
uint32_t starry_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out);

// 存档序列化(供 NVS 保存;编解码可往返)。
size_t starry_save_encode(const starry_save_t *save, uint8_t *out, size_t capacity);
bool starry_save_decode(starry_save_t *save, const uint8_t *data, size_t len);

// 由玩家状态构造存档。
void starry_save_from_player(const starry_player_t *player, starry_save_t *out);
