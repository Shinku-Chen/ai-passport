// main/dracu_scn.h —— 《DRACU-RIOT!》剧本包的只读解析层。
//
// 剧本包由 tools/dracu_scn_pack.py 生成:一部作品压成一条 52,787 页的线性页表,
// 每页 16 字节;正文按字符表编码成 2 字节下标,和页表一样按小块 raw deflate 存,
// 所以读一页只要解一块(页表块固定 256 页 = 4 KB,正文块 ≤3 KB)。固件只需要一块
// 4 KB 的解压缓冲 —— 本板没有 PSRAM,空闲堆最大连续块只有几 KB。
//
// 字节序与字段布局必须与 tools/dracu_scn_pack.py 一致:
//   SEC_CHAR   u32 count + count x u16 码位(按字频降序;下标 = 正文里的字符编码,
//              同时是字库里的字形顺序)
//   SEC_STR    u32 count + count x (u16 字符数 + 字符码...)   # 说话人 / 结局名 / 章节标题
//   SEC_BLOCK  u16 block_count, u16 page_block_count, u32 page_count, u32 pages_per_block,
//              然后 block_count x { off u32, comp u32, raw u32 }(off 相对 SEC_BLOB)
//   SEC_BLOB   逐块 raw deflate(zlib wbits=-15)
//   SEC_BRANCH 分支表(见 dracu_scn.c 的 open 实现)
//   SEC_CHOICE u32 count + count x { page u32, options u8, pad u8, pad u16,
//              5 x { text_off u32, text_len u16, pad u16, target u32 } }
//   SEC_CHAPTERS u32 count + count x { page u32, name u16, route u8, pad u8 }
//   SEC_META   key=value 文本
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "dracu_pack.h"

#include <stdint.h>

#define DRACU_SCN_MAGIC "DRACUSC1"
#define DRACU_SCN_VERSION 1u

enum {
    DRACU_SCN_SEC_CHAR = 0,
    DRACU_SCN_SEC_STR = 1,
    DRACU_SCN_SEC_BLOCK = 2,
    DRACU_SCN_SEC_BLOB = 3,
    DRACU_SCN_SEC_BRANCH = 4,
    DRACU_SCN_SEC_CHOICE = 5,
    DRACU_SCN_SEC_CHAPTERS = 6,
    DRACU_SCN_SEC_META = 7,
};

// 页记录 flags
#define DRACU_PAGE_BLUR (1u << 0)
#define DRACU_PAGE_FLASH (1u << 1)
#define DRACU_PAGE_VIBRATE (1u << 2)
#define DRACU_PAGE_CHOOSE (1u << 3)

// 0xFFFF / 0xFF 在记录里表示"没有"
#define DRACU_SCN_NONE16 DRACU_NONE16
#define DRACU_SCN_NONE8 DRACU_NONE8
// 正文流里的空正文标记
#define DRACU_SCN_NO_TEXT 0xFFFFFFFFu
// 剧本最多 5 个选项
#define DRACU_CHOICE_MAX 5
// 条件路由里出现过的选项页个数上限(源数据 29 个)
#define DRACU_COND_MAX 64
// 单句正文缓冲:源脚本最长一句 144 个汉字(约 432 字节)
#define DRACU_TEXT_BUFFER 640
// 名字缓冲(说话人 / 结局名 / 章节名)
#define DRACU_NAME_MAX 64
// 解块缓冲:一页页表 4 KB、正文块 3 KB,取 4 KB
#define DRACU_BLOCK_BUFFER 4096

typedef struct {
    uint32_t text_off;
    uint16_t text_len;   // 字符数
    uint16_t cg;         // 事件 CG id;DRACU_SCN_NONE16 = 无
    uint16_t sprite;     // 立绘 id;DRACU_SCN_NONE16 = 无
    uint8_t bg;          // 背景 id;DRACU_SCN_NONE8 = 无(黑屏)
    uint8_t sd;          // SD id;DRACU_SCN_NONE8 = 无
    uint8_t name;        // 说话人 id;DRACU_SCN_NONE8 = 旁白
    uint8_t flags;
    uint8_t cs;          // 立绘缩放百分比(源数据)
    uint8_t fs;          // 字号百分比
} dracu_page_t;

typedef struct {
    uint32_t page;
    uint8_t count;
    uint32_t target[DRACU_CHOICE_MAX];
    uint32_t text_off[DRACU_CHOICE_MAX];
    uint16_t text_len[DRACU_CHOICE_MAX];
} dracu_choice_t;

typedef struct {
    uint32_t page;
    uint16_t name;   // 章节标题(字符串表下标)
    uint8_t route;   // 路线编号(0 = 共通线)
} dracu_chapter_t;

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *chars;      // SEC_CHAR 数据区
    uint32_t char_count;
    const uint8_t *strings;    // SEC_STR 数据区
    uint32_t string_count;
    const uint8_t *blocks;     // SEC_BLOCK 记录区(块表)
    uint32_t block_count;
    uint32_t page_block_count;
    uint32_t page_count;
    uint32_t pages_per_block;
    const uint8_t *payload;    // SEC_BLOB 数据区
    uint32_t payload_size;
    // 分支表视图(open 时按顺序切好)
    const uint8_t *no_next;
    uint32_t no_next_count;
    const uint8_t *no_back;
    uint32_t no_back_count;
    const uint8_t *ends;
    uint32_t end_count;
    const uint8_t *cond_pages;
    uint32_t cond_count;
    const uint8_t *hidden;
    uint32_t hidden_count;
    const uint8_t *rules;
    uint32_t rule_count;
    const uint8_t *literals;
    uint32_t literal_count;
    const uint8_t *choices;    // SEC_CHOICE 记录区
    uint32_t choice_count;
    const uint8_t *chapters;   // SEC_CHAPTERS 记录区
    uint32_t chapter_count;
    const uint8_t *meta;
    uint32_t meta_size;

    // 解块缓存(由 dracu_scn_attach 绑定缓冲;两块各自缓存,避免来回切换时重复解压)
    uint8_t *scratch;
    uint32_t scratch_size;
    int32_t cached_page_block;
    int32_t cached_text_block;
} dracu_scn_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false。
bool dracu_scn_open(dracu_scn_t *scn, const uint8_t *data, uint32_t size);
// 绑定解块缓冲(必须 >= DRACU_BLOCK_BUFFER 字节)。不绑定也能读,只是每次都要解块。
void dracu_scn_attach(dracu_scn_t *scn, uint8_t *scratch, uint32_t size);

// 读一页记录。页号从 1 起;越界或解压失败返回 false。
bool dracu_scn_page(dracu_scn_t *scn, uint32_t page, dracu_page_t *out);

// 把正文(char 下标的 2 字节序列)解码成 UTF-8;返回写入字节数(不含 NUL)。
size_t dracu_scn_text(dracu_scn_t *scn, uint32_t off, uint16_t len, char *out, size_t capacity);

// 字符串表项(说话人 / 结局名 / 章节名)解码成 UTF-8。
size_t dracu_scn_string(dracu_scn_t *scn, uint16_t id, char *out, size_t capacity);

// 取页码对应的选项表。不是选项页返回 false。
bool dracu_scn_choice(dracu_scn_t *scn, uint32_t page, dracu_choice_t *out);

// 分支查询:
//   前进特判:命中返回目标页号,否则返回 0
//   回退特判:同上
//   结局判定:命中把结局名的字符串下标写进 name_id,返回 true
//   条件路由:choice 是选择历史(下标 = 条件页下标,值为 1..N,0 = 未选),
//             命中把目标页号写进 target,返回 true
uint32_t dracu_scn_next_page(const dracu_scn_t *scn, uint32_t page);
uint32_t dracu_scn_back_page(const dracu_scn_t *scn, uint32_t page);
bool dracu_scn_end_name(const dracu_scn_t *scn, uint32_t page, uint16_t *name_id);
bool dracu_scn_hidden_page(const dracu_scn_t *scn, uint32_t page, const uint8_t *choice,
                           uint32_t *target);

// 选项页在条件表里的下标;不是条件页返回 -1。
int dracu_scn_cond_index(const dracu_scn_t *scn, uint32_t page);
// 条件页的选项数(历史上限用)。
uint8_t dracu_scn_cond_options(const dracu_scn_t *scn, uint32_t page);

// 章节表。
uint32_t dracu_scn_chapter_count(const dracu_scn_t *scn);
bool dracu_scn_chapter(const dracu_scn_t *scn, uint32_t index, dracu_chapter_t *out);

// 页号是否在范围内。
bool dracu_scn_page_valid(const dracu_scn_t *scn, uint32_t page);
