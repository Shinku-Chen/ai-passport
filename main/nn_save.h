// main/nn_save.h —— 持久化：5 个手动存档 + 1 个自动存档 + 观看记录 + 设置。
//
// 全部落在 NVS（namespace "nn"）。存档里保存“剧本位置 + 画面状态 + 结局/ CG
// 解锁位”，因此读档不需要重放剧本；版本号与魔数用来丢弃旧格式/损坏数据，
// 而不是把坏数据当成合法存档。
#pragma once

#include "nn_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_SLOT_MANUAL 5      // 手动槽 0..4
#define NN_SLOT_AUTO 5        // 自动槽（索引 5）
#define NN_SLOT_COUNT 6

typedef struct {
    uint16_t scene;
    uint16_t op;
    uint16_t bg;
    uint16_t cg;
    uint16_t sprite;
    uint32_t stamp;        // 保存时的运行时长（秒），只用于显示排序
    uint16_t cg_mask;      // 已解锁 CG 位图（本作 15 张，16 位足够）
    uint8_t ending_mask;   // 已看过的结局（4 个）
    uint8_t reserved;
    char chapter[12];      // 场景名（UTF-8，"chunk007" 这类）
    char preview[28];      // 该行前 27 字节（UTF-8 截断），列表里显示
} nn_save_t;

typedef struct {
    uint8_t text_speed;    // 0 慢 / 1 中 / 2 快 / 3 瞬间
    uint8_t font_large;    // 0 = 16px, 1 = 20px
    uint8_t auto_play;     // 0/1（读档后保持）
    uint8_t panel_opa;     // 文本框不透明度 10..55 (%)
} nn_settings_t;

// 存储状态。init 永不擦除 NVS（NVS 是整机共享分区，擦掉会连带删掉别的
// namespace），损坏时只降级：本次运行照常玩，存档/设置不保留。
typedef enum {
    NN_STORE_OK = 0,        // 可读可写
    NN_STORE_BROKEN,        // NVS 分区需要人工处理（无空闲页 / 新版本格式）
    NN_STORE_UNAVAILABLE,   // 其它初始化失败（如驱动未就绪）
} nn_store_state_t;

nn_store_state_t nn_store_init(void);
nn_store_state_t nn_store_state(void);
bool nn_store_available(void);
const char *nn_store_reason(void);   // 给界面/日志用的一句话原因

bool nn_save_write(uint8_t slot, const nn_save_t *save);
bool nn_save_read(uint8_t slot, nn_save_t *out);
bool nn_save_exists(uint8_t slot);
void nn_save_erase(uint8_t slot);

// 解锁进度（跨存档）：CG 与结局。
uint16_t nn_unlock_cg_mask(void);
void nn_unlock_cg(uint8_t cg_index);
uint8_t nn_unlock_ending_mask(void);
void nn_unlock_ending(uint8_t ending_id);
void nn_unlock_clear(void);

// 设置。
void nn_settings_load(nn_settings_t *out);
void nn_settings_store(const nn_settings_t *in);

#ifdef __cplusplus
}
#endif
