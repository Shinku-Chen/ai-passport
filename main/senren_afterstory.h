// main/senren_afterstory.h —— 源 AFTER_STORIES 表与通关位(clear)的纯逻辑。
//
// 源工程 9644883 的 src/common/constants.js 定义六个后日谈起点:
// scn 是 1-based 剧本文件号,from 是该 chunk 内 0-based 节点。pack 的 chunk
// 下标是 0-based,所以直接使用 scn - 1;from 与生产 player 的 node 一致。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SENREN_AFTERSTORY_COUNT 6

typedef struct {
    const char *name;      // 标题页显示的角色名
    const char *ending;    // 主线达到的正式 END 名
    uint16_t source_scn;   // 源 constants.js 的 scn(1-based)
    uint16_t chunk;        // pack 的 0-based chunk = source_scn - 1
    uint32_t node;         // 源 constants.js 的 from(0-based)
    uint8_t selects;       // 该后日谈段内的 SELECT 数(每处 2 选项)
} senren_afterstory_t;

int senren_afterstory_count(void);
const senren_afterstory_t *senren_afterstory_get(int index);

// 把主线 END 名映射到后日谈下标;不是六条主线 END 时返回 -1。
int senren_afterstory_index_for_ending(const char *ending);

// 通关位是 6 bit 的位图,与旧 settings blob 分开存 NVS。
bool senren_afterstory_unlocked(uint8_t clears, int index);
uint8_t senren_afterstory_set_clear(uint8_t clears, int index);

