// main/eden_store.h —— eden* 的持久化:存档槽位、阅读进度、鉴赏解锁。
//
// 三份记录各自一个 NVS 命名空间,互不覆盖:
//   edensave  slots :3 个手动存档(位置 + 剧情变量 + 预览文字)
//   edenprog  pos   :自动阅读进度(继续游戏从这里接上)
//   edenulck  bits  :鉴赏解锁位(CG 组 / 章节 / 结局),跨存档生效
//
// 本层只做"记录 <-> 字节"的编解码与校验,实际的 NVS 读写走 eden_store_io_t:
// 固件用 eden_store_nvs.c 的适配器,宿主机测试用假适配器注入失败,不需要复制业务逻辑。
//
// 每条记录都带格式版本号;版本不认识、长度不符、字段越界、字符串没有 NUL 结尾
// 一律按"没有记录"处理 —— 读失败**不会**改动调用方传进来的结构体。
#pragma once

#include "eden_script.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDEN_SAVE_SLOTS 3
#define EDEN_PREVIEW_LEN 44
#define EDEN_TITLE_LEN 28
// 单条台词最多几页(与 eden_ui.h 的分页上限一致);超出视为坏记录。
#define EDEN_SLOT_PAGE_MAX 16
// CG 解锁位按"鉴赏归组"计;eden 的 reports/gallery-source.json 有 295 组。
#define EDEN_CG_GROUPS 320
#define EDEN_CG_BYTES ((EDEN_CG_GROUPS + 7) / 8)

// 存档格式版本。改动 eden_slot_t 布局时必须 +1。
#define EDEN_SAVE_VERSION 1u
#define EDEN_PROGRESS_VERSION 1u
#define EDEN_UNLOCK_VERSION 1u

typedef struct {
    uint8_t valid;                      // 0 = 空槽
    uint16_t scene;                     // 剧本位置
    uint32_t op;
    uint16_t page;                      // 这一句里的第几页
    uint16_t chapter;                   // 章节下标(SEC_CHAP),显示用
    int32_t vars[EDEN_VAR_MAX];         // 剧情变量
    char chapter_text[EDEN_TITLE_LEN];  // 槽位预览:章节名
    char preview[EDEN_PREVIEW_LEN];     // 槽位预览:这一句开头
} eden_slot_t;

typedef struct {
    uint8_t valid;
    uint16_t scene;
    uint32_t op;
    uint16_t page;
    int32_t vars[EDEN_VAR_MAX];
} eden_progress_t;

typedef struct {
    uint8_t cg[EDEN_CG_BYTES];
    uint32_t chapter;   // 已到过的章节位
    uint32_t ending;    // 看过的结局位
} eden_unlock_t;

// 存储适配器:返回 true 表示读/写成功。写入失败必须返回 false(包括提交失败)。
typedef struct {
    bool (*read)(const char *ns, const char *key, void *out, size_t size);
    bool (*write)(const char *ns, const char *key, const void *data, size_t size);
} eden_store_io_t;

// NVS 准备(初始化分区,不擦除任何既有命名空间)。失败时持久化降级为"本次运行内有效"。
bool eden_store_init(void);

// 绑定存储适配器;传 NULL 恢复默认 NVS 适配器(宿主机测试用假适配器)。
void eden_store_bind(const eden_store_io_t *io);
const eden_store_io_t *eden_store_current_io(void);

void eden_slot_clear(eden_slot_t *slot);
void eden_progress_clear(eden_progress_t *progress);
void eden_unlock_clear(eden_unlock_t *unlock);

void eden_unlock_grant_cg(eden_unlock_t *unlock, uint16_t group);
bool eden_unlock_has_cg(const eden_unlock_t *unlock, uint16_t group);
void eden_unlock_grant_chapter(eden_unlock_t *unlock, uint16_t index);
bool eden_unlock_has_chapter(const eden_unlock_t *unlock, uint16_t index);
void eden_unlock_grant_ending(eden_unlock_t *unlock, uint16_t index);
bool eden_unlock_has_ending(const eden_unlock_t *unlock, uint16_t index);

// 读取:成功返回 true 并写入 out;任何校验失败都返回 false 且**不改动 out**。
bool eden_store_load_slots(eden_slot_t slots[EDEN_SAVE_SLOTS]);
bool eden_store_load_progress(eden_progress_t *progress);
bool eden_store_load_unlock(eden_unlock_t *unlock);

// 写入:返回 false 表示没写进去(调用方不得提示成功)。
bool eden_store_save_slots(const eden_slot_t slots[EDEN_SAVE_SLOTS]);
bool eden_store_save_progress(const eden_progress_t *progress);
bool eden_store_save_unlock(const eden_unlock_t *unlock);

// 单条记录的字节长度(供测试与适配器断言布局)。
size_t eden_store_slots_blob_size(void);
size_t eden_store_progress_blob_size(void);
size_t eden_store_unlock_blob_size(void);

// 把记录编码进 buf(测试与适配器共用,避免测试复制一份布局)。
bool eden_store_encode_slots(const eden_slot_t slots[EDEN_SAVE_SLOTS], uint8_t *buf, size_t size);
bool eden_store_decode_slots(const uint8_t *buf, size_t size, eden_slot_t slots[EDEN_SAVE_SLOTS]);
bool eden_store_encode_progress(const eden_progress_t *progress, uint8_t *buf, size_t size);
bool eden_store_decode_progress(const uint8_t *buf, size_t size, eden_progress_t *progress);
bool eden_store_encode_unlock(const eden_unlock_t *unlock, uint8_t *buf, size_t size);
bool eden_store_decode_unlock(const uint8_t *buf, size_t size, eden_unlock_t *unlock);
