// main/senren_model.h —— 《千恋＊万花》剧本包的只读解析 + 阅读状态机。
//
// 剧本包由 tools/senren_scn_pack.py 生成,整块放在 Flash 里。位置是"块 + 节点":
// 112 个块各自是一段独立 raw deflate 的记录流,换块时只解一块(约 21 KB 解压后),
// 所以阅读顺序推进不需要把整部剧本读进内存。
//
// 字节序与字段布局必须与 tools/senren_scn_pack.py 一致:
//   SEC_CHAR  u32 count + count x u16 码位(按词频降序;码值即字符表下标)
//   SEC_NAME  字典块连排:说话人 / 立绘键 / 事件图 / 背景 / 结局(最后一块可缺)。
//             每块 u16 count,随后每条 u16 长度 + 长度个 u16 字符码
//   SEC_FLAG  u32 count + count x u32 页号(f.c<页号> -> 下标即 flag_id)
//   SEC_CHUNK u16 count,每块 { data_off u32, data_len u32, raw_len u32, node_count u32 }
//   SEC_BLOB  逐块 raw deflate(zlib wbits=-15)
//   SEC_META  key=value 文本
//
// 记录流(解压后):
//   u16 node_count,然后逐条 u8 kind:
//     0 LABEL      u32 页号
//     1 CHAPTER    text
//     2 BG         u8 背景下标
//     3 DIALOGUE   u8 说话人下标,text,u8 action,u16 立绘键下标(0xFFFF = 无)
//     4 SELECT     u8 选项数,每项 { text,u8 目标类型,u16 跨块号,u32 页号,
//                  u8 flag_id,u8 值 }
//     5 EV         u16 事件图下标(0xFFFF = 清 CG)
//     6 NEXT       u8 目标类型,u16 跨块号,u32 页号,u8 条件数,
//                  每条件 { u8 flag_id,u8 值 }
//     7 SPRITE_OFF 无参
//   text = u16 字符数 + 该数量的 u16 码
//   目标类型:0=本块(页号),1=跨块(块号 + 页号),2=结局(页号 = 结局下标)
//
// 立绘动作 action:0=none 1=fadein 2=fadeout 3=change(未知值归到 3)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENREN_SCN_MAGIC "SENRSCN1"
#define SENREN_SCN_VERSION 1u

// 单个字符串缓冲:说话人 / 资源名 / 选项文案都够用
#define SENREN_NAME_MAX 64
// 单句正文缓冲(源文本最长 60 字,UTF-8 最多 3 字节/字)
#define SENREN_TEXT_BUFFER 512
// 选项数量上限(源脚本每处最多 3 个)
#define SENREN_CHOICE_MAX 4
// 条件里出现的 f.c<页号> 个数(源脚本 44 个)
#define SENREN_FLAG_MAX 64
// 单句最多切几页(最长 60 字 -> 每行 13 字 5 行,实测最多 2 页)
#define SENREN_PAGE_MAX 8
// 单个小块解压缓冲上限:打包器把每块的原始字节压到 <= 3002 字节(见 BLOCK_MAX_BYTES)
#define SENREN_BLOCK_RAW_MAX 4096

// 节点类型,与转换脚本一一对应
enum {
    SENREN_NODE_LABEL = 0,
    SENREN_NODE_CHAPTER = 1,
    SENREN_NODE_BG = 2,
    SENREN_NODE_DIALOGUE = 3,
    SENREN_NODE_SELECT = 4,
    SENREN_NODE_EV = 5,
    SENREN_NODE_NEXT = 6,
    SENREN_NODE_SPRITE_OFF = 7,
};

// 跳转目标类型
enum {
    SENREN_TARGET_SAME = 0,
    SENREN_TARGET_CROSS = 1,
    SENREN_TARGET_ENDING = 2,
};

// 立绘动作
enum {
    SENREN_ACTION_NONE = 0,
    SENREN_ACTION_FADE_IN = 1,
    SENREN_ACTION_FADE_OUT = 2,
    SENREN_ACTION_CHANGE = 3,
};

// 阅读推进的结果
typedef enum {
    SENREN_STEP_TEXT,     // 有新的正文页可读
    SENREN_STEP_CHOICE,   // 停在选项上,等 senren_player_choose
    SENREN_STEP_CHAPTER,  // 走进新章节(显示章节卡)
    SENREN_STEP_ENDING,   // 剧情走完
    SENREN_STEP_STUCK,    // 数据有问题(找不到目标),停止推进
} senren_step_t;

// 段类型(与 tools/senren_scn_pack.py 的 SEC_* 一致)
enum {
    SENREN_SCN_SEC_CHAR = 0,
    SENREN_SCN_SEC_NAME = 1,
    SENREN_SCN_SEC_FLAG = 2,
    SENREN_SCN_SEC_CHUNK = 3,
    SENREN_SCN_SEC_BLOCK = 4,
    SENREN_SCN_SEC_BLOB = 5,
    SENREN_SCN_SEC_META = 6,
};

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *chars;    // SEC_CHAR 数据区
    uint32_t char_count;
    const uint8_t *names;    // SEC_NAME 数据区(字典块连排)
    uint32_t names_size;
    const uint8_t *flag_pages;  // SEC_FLAG 数据区(u32 页号)
    uint32_t flag_count;
    const uint8_t *chunks;   // SEC_CHUNK 数据区
    uint32_t chunks_size;
    uint32_t chunk_count;
    const uint8_t *blocks;   // SEC_BLOCK 数据区(每个 chunk 的节点被切成若干小块)
    uint32_t blocks_size;
    uint32_t block_count;
    const uint8_t *payload;  // SEC_BLOB 数据区
    uint32_t payload_size;
    const uint8_t *meta;
    uint32_t meta_size;
    // 字典块(senren_scn_open 填好;endings 允许为空)
    const uint8_t *speakers;
    uint32_t speaker_count;
    const uint8_t *sprite_keys;
    uint32_t sprite_count;
    const uint8_t *event_names;
    uint32_t event_count;
    const uint8_t *bg_names;
    uint32_t bg_count;
    const uint8_t *endings;
    uint32_t ending_count;
} senren_scn_t;

// 排版参数:每行字符单位数与每页行数(与界面字号设置对应)
typedef struct {
    uint16_t units_per_line;
    uint16_t lines_per_page;
} senren_layout_t;

typedef struct {
    // 当前章(0 起,按 CHAPTER 节点出现顺序编号)
    uint16_t chapter;
    // 章节标题(UTF-8,已解码;源数据形如 "CHAPTER1-1")
    char chapter_title[SENREN_NAME_MAX];
    // 下一个要执行的节点位置(node 从 0 起)
    uint16_t chunk;
    uint32_t node;
    // 当前背景 / 事件 CG / 立绘的资源名(""=没有)
    char bg[SENREN_NAME_MAX];
    char ev[SENREN_NAME_MAX];
    char sprite[SENREN_NAME_MAX];
    uint8_t sprite_action;
    bool sprite_visible;
    // 当前句
    char speaker[SENREN_NAME_MAX];
    char text[SENREN_TEXT_BUFFER];
    char page[SENREN_TEXT_BUFFER];
    uint16_t page_index;
    uint16_t page_count;
    // 选项:文案 + 目标 + 要置的标志位
    uint8_t choice_count;
    char choice[SENREN_CHOICE_MAX][SENREN_NAME_MAX];
    uint8_t choice_target_kind[SENREN_CHOICE_MAX];
    uint16_t choice_chunk[SENREN_CHOICE_MAX];
    uint32_t choice_page[SENREN_CHOICE_MAX];
    uint8_t choice_flag[SENREN_CHOICE_MAX];
    uint8_t choice_value[SENREN_CHOICE_MAX];
    // 结局下标(NEXT 目标类型为结局时填)
    uint16_t ending_index;
    // 当前页在 player->text 里的起点(分页结果)
    uint32_t page_offsets[SENREN_PAGE_MAX];
    // 产生当前可见步骤的那条记录(存档从这里恢复,再走一次 advance 就会重现)
    uint32_t resume_node;
    // 条件/选项设置的标志位(下标 = flag_id)
    uint8_t flags[SENREN_FLAG_MAX];
    // 当前装的是哪一个小块(0xFFFF = 空)与它的首节点下标
    uint16_t loaded_chunk;
    uint16_t loaded_block;
    uint32_t loaded_first_node;
    uint32_t loaded_len;
    uint8_t *raw;
    uint32_t raw_capacity;
} senren_player_t;

// 存档:位置 + 标志位 + 重新开画面所需的当前资源名
typedef struct {
    uint16_t chunk;
    uint32_t node;
    uint16_t chapter;
    uint8_t flags[SENREN_FLAG_MAX];
    char bg[SENREN_NAME_MAX];
    char ev[SENREN_NAME_MAX];
    char sprite[SENREN_NAME_MAX];
    // 当前章的标题:读档后"跳过章节"要靠它认出「下一章」是哪一个标记
    char chapter_title[SENREN_NAME_MAX];
    uint8_t sprite_action;
    bool sprite_visible;
} senren_save_t;

// ---- 剧本包 ---------------------------------------------------------------
bool senren_scn_open(senren_scn_t *scn, const uint8_t *data, uint32_t size);

// 解开一个小块到 raw 缓冲(容量必须 >= SENREN_BLOCK_RAW_MAX);返回解压后字节数,失败返回 0。
uint32_t senren_scn_block(const senren_scn_t *scn, uint16_t block_index, uint8_t *raw, uint32_t capacity);

// 把字典项解码成 UTF-8。越界返回 0 并写空串。
size_t senren_scn_speaker(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t senren_scn_sprite_key(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t senren_scn_event(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t senren_scn_background(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t senren_scn_ending(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity);

// ---- 阅读状态机 -----------------------------------------------------------
void senren_player_reset(senren_player_t *player);

// 把章节卡标题 "CHAPTER<章>-<节>" 整理成画面上要显示的 "<章>-<节>"(例如 "1-2")。
// 没有节号时就只给章号;不是章节卡(CHAPTERshow/hide、空标题)时返回 false。
bool senren_chapter_label(const char *title, char *out, size_t capacity);

// 从 (chunk, node) 开始读,chapter 是展示用的章号(与存档一致)。
bool senren_player_start(senren_player_t *player, const senren_scn_t *scn, uint16_t chapter,
                         uint16_t chunk, uint32_t node, const senren_layout_t *layout);

// 推进到下一个可见步骤(对话页 / 选项 / 章节卡 / 结局)。
senren_step_t senren_player_advance(senren_player_t *player, const senren_scn_t *scn,
                                    const senren_layout_t *layout);

// 选择第 index 个选项(true 表示选到了)。
bool senren_player_choose(senren_player_t *player, const senren_scn_t *scn, uint8_t index,
                          const senren_layout_t *layout);

// 跳到下一章 / 下一处场景切换(下一个 BG、EV 或 CHAPTER 节点)。
bool senren_player_skip_chapter(senren_player_t *player, const senren_scn_t *scn,
                                const senren_layout_t *layout);
bool senren_player_skip_scene(senren_player_t *player, const senren_scn_t *scn,
                              const senren_layout_t *layout);

// 从存档恢复:位置与画面状态直接取存档,随后由 advance 推进到第一个可见步骤。
bool senren_player_load(senren_player_t *player, const senren_scn_t *scn, const senren_save_t *save,
                        const senren_layout_t *layout);

// 释放状态机自己申请的缓冲(掉电前/卸载时调用;重复调用安全)。
void senren_player_release(senren_player_t *player);

// 当前句还有下一页时翻页并返回 true;已经是最后一页返回 false。
bool senren_player_next_page(senren_player_t *player);

// 取当前页 / 整句 / 说话人(UTF-8,NUL 结尾);返回写入长度。
size_t senren_player_page_text(const senren_player_t *player, char *out, size_t capacity);
size_t senren_player_text(const senren_player_t *player, char *out, size_t capacity);
size_t senren_player_speaker(const senren_player_t *player, char *out, size_t capacity);

// 最近一次失败的原因(静态字符串,单线程使用)。调 senren_player_* 失败后打印它,
// 能直接看出是"小块解压失败 / 找不到标签 / 记录流走不动"里的哪一种。
const char *senren_get_error(void);

// ---- 纯排版逻辑(宿主机可测)------------------------------------------------
// 字符宽度单位:全角 2、半角 1(与界面字号无关,只用于断行)。
int senren_char_units(uint32_t codepoint);
// 按 units_per_line 断行,offsets 收下每行起点(UTF-8 字节偏移);返回行数。
int senren_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines);
// 在断行基础上按 lines_per_page 分页,offsets 收下每页起点;返回页数。
int senren_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                      int max_offsets);
size_t senren_utf8_next_boundary(const char *utf8, size_t len, size_t pos);
uint32_t senren_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out);

// ---- 存档 -----------------------------------------------------------------
void senren_save_from_player(const senren_player_t *player, senren_save_t *out);
size_t senren_save_encode(const senren_save_t *save, uint8_t *out, size_t capacity);
bool senren_save_decode(senren_save_t *save, const uint8_t *data, size_t len);
