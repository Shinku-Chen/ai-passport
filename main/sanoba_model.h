// main/sanoba_model.h —— 《魔女的夜宴》剧本包(SANOSCN1)的只读解析 + 阅读状态机。
//
// 剧本包由 tools/sanoba_scn_pack.py 生成,整块放在 Flash 里。位置是"场景 + 块 + 节点":
// 179 个块各自是一段独立 raw deflate 的记录流(解压后最大 19.5 KB),换块时只解一块,
// 所以阅读顺序推进不需要把整部剧本读进内存。
//
// 字节序与字段布局必须与 tools/sanoba_scn_pack.py 一致:
//   SEC_CHAR     u32 count + count × u16 码位(按词频降序;码值即字符表下标,
//                同一张表同时是固件字库的字形号)
//   SEC_NAME     字典块连排,顺序 speaker / sprite_key / sprite_position /
//                sprite_expression / sprite_outfit / background / event / flag / label;
//                每块 u16 count,随后每条 u16 长度 + 长度个 u16 字符码(无段前计数)
//   SEC_SCENARIO u16 count,每项 { first_chunk u16, chunk_count u16, title text }
//   SEC_CHUNK    u16 count,每块 { data_off u32, data_len u32, raw_len u32, node_count u32 }
//   SEC_BLOB     逐块 raw deflate(zlib wbits=-15)
//   SEC_LABEL    u32 count,每条 { name_id u32, scenario u16, chunk u16, node u32 }
//   SEC_ROUTE    u16 触发场景, u16 兜底场景, u16 目标数, 每项 { flag_id u8, scenario u16 }
//   SEC_META     key=value 文本
//
// 记录流(解压后):
//   u16 node_count,然后逐条 u8 kind:
//     0 LABEL      u32 label_id
//     1 CHAPTER    text
//     2 BG         u8 背景下标
//     3 DIALOGUE   u8 说话人下标,text,u8 立绘数,每立绘 4×u8(本作没有立绘图,跳过)
//     4 SELECT     u8 选项数,每项 { text,u8 目标类型,u32 label_id,u8 赋值数,
//                  每赋值 { u8 flag_id,u8 op,u16 value } }
//     5 EV         u16 =(kind<<14)|下标;kind 0=EV 1=SD 2=源里未支持;0xFFFF=清空
//     6 NEXT       u8 目标类型,u32 label_id,u8 条件数,每条件 { u8 flag_id,u8 op,u16 value }
//     7 SPRITE_OFF 无参
//   text = u16 字符数 + 该数量的 u16 码
//   目标类型:0=标签(可能落在别的场景) 1=源里哪都没定义的标签(保持当前进度) 2=结局
//   op:0=赋值 1=累加
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SANOBA_SCN_MAGIC "SANOSCN1"
#define SANOBA_SCN_VERSION 1u

// 单个字符串缓冲:说话人 / 资源名 / 选项文案都够用
#define SANOBA_NAME_MAX 64
// 单句正文缓冲(源文本最长 60 字,UTF-8 最多 3 字节/字)
#define SANOBA_TEXT_BUFFER 512
// 选项数量上限(源脚本每处最多 3 个)
#define SANOBA_CHOICE_MAX 4
// 单个选项最多几条赋值(实测最多 3 条)
#define SANOBA_ASSIGN_MAX 6
// 单个 NEXT 最多几个条件(源脚本没有条件,格式保留)
#define SANOBA_COND_MAX 8
// 旗标槽位(本作 8 个:sel/meg/nen/tsu/tou/wak/eye/glass_flag)
#define SANOBA_FLAG_MAX 32
// 单块解压缓冲上限(实测最大 19.5 KB,留余量)
#define SANOBA_CHUNK_RAW_MAX 32768
// 章节标题缓冲(源数据形如 "chapter 4-8")
#define SANOBA_TITLE_MAX 64

// 节点类型,与打包器一一对应
enum {
    SANOBA_NODE_LABEL = 0,
    SANOBA_NODE_CHAPTER = 1,
    SANOBA_NODE_BG = 2,
    SANOBA_NODE_DIALOGUE = 3,
    SANOBA_NODE_SELECT = 4,
    SANOBA_NODE_EV = 5,
    SANOBA_NODE_NEXT = 6,
    SANOBA_NODE_SPRITE_OFF = 7,
};

// 跳转目标类型
enum {
    SANOBA_TARGET_LABEL = 0,
    SANOBA_TARGET_IGNORED = 1,
    SANOBA_TARGET_ENDING = 2,
};

// 赋值 / 条件操作符
enum {
    SANOBA_OP_SET = 0,   // f.x = value
    SANOBA_OP_ADD = 1,   // f.x += value(++ 等价于 value=1)
};

// 事件图层:0=整屏事件 CG 1=SD 装饰窗 2=源里未支持的名字(不画)
enum {
    SANOBA_EV_KIND_CG = 0,
    SANOBA_EV_KIND_SD = 1,
    SANOBA_EV_KIND_UNSUPPORTED = 2,
};

// 立绘动作:本作源工程没有立绘图,保留枚举只为了让 sprite_action 字段有语义
enum {
    SANOBA_ACTION_NONE = 0,
    SANOBA_ACTION_FADE_IN = 1,
    SANOBA_ACTION_FADE_OUT = 2,
    SANOBA_ACTION_CHANGE = 3,
};

// 结局标签(源数据里哪都没定义的两个名字)
#define SANOBA_ENDING_TITLE 0u
#define SANOBA_ENDING_COLLECTION 1u

// 阅读推进的结果
typedef enum {
    SANOBA_STEP_TEXT,     // 有新的正文页可读
    SANOBA_STEP_CHOICE,   // 停在选项上,等 sanoba_player_choose
    SANOBA_STEP_CHAPTER,  // 走进新章节(显示章节卡)
    SANOBA_STEP_ENDING,   // 剧情走完
    SANOBA_STEP_STUCK,    // 数据有问题,停止推进
} sanoba_step_t;

// 段类型(与 tools/sanoba_scn_pack.py 的 SEC_* 一致)
enum {
    SANOBA_SCN_SEC_CHAR = 0,
    SANOBA_SCN_SEC_NAME = 1,
    SANOBA_SCN_SEC_SCENARIO = 2,
    SANOBA_SCN_SEC_CHUNK = 3,
    SANOBA_SCN_SEC_BLOB = 4,
    SANOBA_SCN_SEC_LABEL = 5,
    SANOBA_SCN_SEC_ROUTE = 6,
    SANOBA_SCN_SEC_META = 7,
};

// "没有下一个场景"(剧情结束)
#define SANOBA_SCENARIO_NONE 0xFFFFu

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *chars;    // SEC_CHAR 数据区
    uint32_t char_count;
    const uint8_t *names;    // SEC_NAME 数据区(字典块连排)
    uint32_t names_size;
    const uint8_t *scenarios;  // SEC_SCENARIO 数据区(段体首 2 字节是计数)
    uint32_t scenario_count;
    uint32_t scenarios_size;
    const uint8_t *chunks;   // SEC_CHUNK 数据区(u16 count 之后)
    uint32_t chunks_size;
    uint32_t chunk_count;
    const uint8_t *payload;  // SEC_BLOB 数据区
    uint32_t payload_size;
    const uint8_t *labels;   // SEC_LABEL 数据区(u32 count 之后)
    uint32_t label_count;
    uint32_t labels_size;
    const uint8_t *route;    // SEC_ROUTE 数据区(触发/兜底/目标数 + 目标)
    uint32_t route_size;
    const uint8_t *meta;
    uint32_t meta_size;
    // 字典块(sanoba_scn_open 填好;固件只用到说话人 / 事件 / 背景 / 旗标)
    const uint8_t *speakers;
    uint32_t speaker_count;
    const uint8_t *sprite_keys;
    uint32_t sprite_count;
    const uint8_t *bg_names;
    uint32_t bg_count;
    const uint8_t *event_names;
    uint32_t event_count;
    const uint8_t *flag_names;
    uint32_t flag_count;
    const uint8_t *label_names;
    uint32_t label_name_count;
} sanoba_scn_t;

// 排版参数:每行字符单位数与每页行数(与界面字号设置对应)
typedef struct {
    uint16_t units_per_line;
    uint16_t lines_per_page;
} sanoba_layout_t;

typedef struct {
    // 当前场景(下标,对应 SEC_SCENARIO 的顺序)
    uint16_t scenario;
    // 章节标题(UTF-8,已解码;源数据形如 "chapter 4-8")
    char chapter_title[SANOBA_TITLE_MAX];
    // 下一个要执行的节点位置(node 从 0 起)
    uint16_t chunk;
    uint32_t node;
    // 当前背景 / 事件图 / 立绘的资源名(""=没有)
    char bg[SANOBA_NAME_MAX];
    char ev[SANOBA_NAME_MAX];
    uint8_t ev_kind;
    // 立绘:本作源工程没有 ch/ 立绘图,恒为空(保留字段以免界面层改签名)
    char sprite[SANOBA_NAME_MAX];
    uint8_t sprite_action;
    bool sprite_visible;
    // 当前句
    char speaker[SANOBA_NAME_MAX];
    char text[SANOBA_TEXT_BUFFER];
    char page[SANOBA_TEXT_BUFFER];
    uint16_t page_index;
    uint16_t page_count;
    // 选项:文案 + 目标 + 要做的赋值
    uint8_t choice_count;
    char choice[SANOBA_CHOICE_MAX][SANOBA_NAME_MAX];
    uint8_t choice_target_kind[SANOBA_CHOICE_MAX];
    uint32_t choice_label[SANOBA_CHOICE_MAX];
    uint8_t choice_assign_count[SANOBA_CHOICE_MAX];
    uint8_t choice_assign_flag[SANOBA_CHOICE_MAX][SANOBA_ASSIGN_MAX];
    uint8_t choice_assign_op[SANOBA_CHOICE_MAX][SANOBA_ASSIGN_MAX];
    uint16_t choice_assign_value[SANOBA_CHOICE_MAX][SANOBA_ASSIGN_MAX];
    // 结局下标(NEXT / 选项目标类型为结局时填)
    uint16_t ending_index;
    // 选项直接指向结局时置位,下一次 advance 立即返回 SANOBA_STEP_ENDING
    bool ending_pending;
    // 当前页在 player->text 里的起点(分页结果)
    uint32_t page_offsets[8];
    // 产生当前可见步骤的那条记录(存档从这里恢复,再走一次 advance 就会重现)
    uint32_t resume_node;
    // 旗标(下标 = flag_id)
    uint8_t flags[SANOBA_FLAG_MAX];
    // 解压缓冲与它当前装的是哪一块(0xFFFF = 空);缓冲首次用到时 malloc:
    // 32 KB 放在结构体里会把 DRAM 静态段挤爆(本板静态段还装着 153 KB 画布与 LVGL 池)。
    uint16_t loaded_chunk;
    uint32_t loaded_len;
    uint8_t *raw;
    uint32_t raw_capacity;
} sanoba_player_t;

// 存档:位置 + 旗标 + 重新开画面所需的当前资源名
typedef struct {
    uint16_t scenario;
    uint16_t chunk;
    uint32_t node;
    uint8_t flags[SANOBA_FLAG_MAX];
    char bg[SANOBA_NAME_MAX];
    char ev[SANOBA_NAME_MAX];
    uint8_t ev_kind;
} sanoba_save_t;

// ---- 剧本包 ---------------------------------------------------------------
bool sanoba_scn_open(sanoba_scn_t *scn, const uint8_t *data, uint32_t size);

// 解开一块到 raw 缓冲(容量必须 >= 该块 raw_len);返回解压后字节数,失败返回 0。
uint32_t sanoba_scn_chunk(const sanoba_scn_t *scn, uint16_t index, uint8_t *raw, uint32_t capacity);

// 场景表:第 index 个场景的首块与块数;title_out 可为 NULL。
bool sanoba_scn_scenario(const sanoba_scn_t *scn, uint16_t index, uint16_t *first_chunk,
                         uint16_t *chunk_count, char *title_out, size_t title_capacity);

// 场景表里预先算好的下一个场景(同组内下一个;0xFFFF = 本组走完即通关)。
uint16_t sanoba_scn_scenario_next(const sanoba_scn_t *scn, uint16_t index);

// 标签表:label_id -> (场景, 块, 节点)。越界返回 false。
bool sanoba_scn_label(const sanoba_scn_t *scn, uint32_t label_id, uint16_t *scenario, uint16_t *chunk,
                      uint32_t *node);

// 下一个场景:命中 SEC_ROUTE 时按旗标选线(最高分优先,全 0 或并列走兜底),
// 否则用场景表里存好的 next_scenario;0xFFFF 表示本组走完(通关)。
uint16_t sanoba_scn_next_scenario(const sanoba_scn_t *scn, uint16_t scenario, const uint8_t *flags);

// 把字典项解码成 UTF-8。越界返回 0 并写空串。
size_t sanoba_scn_speaker(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t sanoba_scn_sprite_key(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t sanoba_scn_event(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t sanoba_scn_background(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t sanoba_scn_flag(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);
size_t sanoba_scn_scenario_title(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity);

// ---- 阅读状态机 -----------------------------------------------------------
void sanoba_player_reset(sanoba_player_t *player);

// 从 (scenario, chunk, node) 开始读。scenario 必须与存档里的一致。
bool sanoba_player_start(sanoba_player_t *player, const sanoba_scn_t *scn, uint16_t scenario,
                         uint16_t chunk, uint32_t node, const sanoba_layout_t *layout);

// 推进到下一个可见步骤(对话页 / 选项 / 章节卡 / 结局)。
sanoba_step_t sanoba_player_advance(sanoba_player_t *player, const sanoba_scn_t *scn,
                                    const sanoba_layout_t *layout);

// 选择第 index 个选项(true 表示选到了)。
bool sanoba_player_choose(sanoba_player_t *player, const sanoba_scn_t *scn, uint8_t index,
                          const sanoba_layout_t *layout);

// 跳到下一章 / 下一处场景切换(下一个 BG、EV 或 CHAPTER 节点)。
bool sanoba_player_skip_chapter(sanoba_player_t *player, const sanoba_scn_t *scn,
                                const sanoba_layout_t *layout);
bool sanoba_player_skip_scene(sanoba_player_t *player, const sanoba_scn_t *scn,
                              const sanoba_layout_t *layout);

// 从存档恢复:位置与画面状态直接取存档,随后由 advance 推进到第一个可见步骤。
bool sanoba_player_load(sanoba_player_t *player, const sanoba_scn_t *scn, const sanoba_save_t *save,
                        const sanoba_layout_t *layout);

// 释放状态机自己申请的缓冲(掉电前/卸载时调用;重复调用安全)。
void sanoba_player_release(sanoba_player_t *player);

// 当前句还有下一页时翻页并返回 true;已经是最后一页返回 false。
bool sanoba_player_next_page(sanoba_player_t *player);

// 取当前页 / 整句 / 说话人(UTF-8,NUL 结尾);返回写入长度。
size_t sanoba_player_page_text(const sanoba_player_t *player, char *out, size_t capacity);
size_t sanoba_player_text(const sanoba_player_t *player, char *out, size_t capacity);
size_t sanoba_player_speaker(const sanoba_player_t *player, char *out, size_t capacity);

// ---- 纯排版逻辑(宿主机可测)------------------------------------------------
// 字符宽度单位:全角 2、半角 1(与界面字号无关,只用于断行)。
int sanoba_char_units(uint32_t codepoint);
// 按 units_per_line 断行,offsets 收下每行起点(UTF-8 字节偏移);返回行数。
int sanoba_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines);
// 在断行基础上按 lines_per_page 分页,offsets 收下每页起点;返回页数。
int sanoba_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                      int max_offsets);
size_t sanoba_utf8_next_boundary(const char *utf8, size_t len, size_t pos);
uint32_t sanoba_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out);

// ---- 存档 -----------------------------------------------------------------
void sanoba_save_from_player(const sanoba_player_t *player, sanoba_save_t *out);
size_t sanoba_save_encode(const sanoba_save_t *save, uint8_t *out, size_t capacity);
bool sanoba_save_decode(sanoba_save_t *save, const uint8_t *data, size_t len);
