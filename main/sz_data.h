// main/sz_data.h — 生字卡数据只读表 + "已认识"持久化标记(NVS)。
// 字表由 tools/sz_gen/gen_table.py 生成, 存 Flash const, RAM=0。
// 认识标记存 NVS(212 字节), 开机读入内存, 改动即 commit。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 一个字卡
typedef struct {
    const char *hanzi;    // UTF-8 汉字(3字节+NUL)
    const char *pinyin;   // 带声调拼音, 如 "tiān"
    uint8_t     strokes;  // 笔画数, 0=未知
} sz_card_t;

// 只读字表(Flash const, 见 main/gen/sz_cards_table.c)
extern const sz_card_t sz_cards[];
extern const uint16_t  sz_card_count;

// 初始化: nvs_flash_init(幂等) + 打开句柄 + 读入 known 标记。
// 返回 ESP_OK 成功; 失败时 known 功能降级为"不保存"(不阻塞功能)。
esp_err_t sz_data_init(void);

// 查询/设置某字是否"已认识"(index 0..sz_card_count-1)
bool      sz_is_known(uint16_t idx);
void      sz_set_known(uint16_t idx, bool is_known);
uint16_t  sz_known_count(void);
