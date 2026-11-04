// main/steins_mobile_pack.h —— 把 pack v4 访问器接到 `steins_mobile_content_t`（生产适配层）。
//
// 责任边界：本文件**只做"pack 记录 → 事件机内容记录"的翻译**，不缓存、不另建影子表、
// 不重新解析包字节；所有数据都来自 `main/steins_pack.h` 已发布的 getter。
//
// 节点空间（重要）：包把所有手机相关节点号都归一化成**包节点下标**（NODE 段下标），
// 因此本适配层与事件机统一使用"包节点下标"，不做 source→pack 再映射。
//   - MNODE/WINDOW/KEYPHONE/MROW 都在包节点下标空间；
//   - 通话片段用**稳定虚拟节点号** `STEINS_MOB_CLIP_NODE_BASE(0x8000) + clip 下标`。
//
// 已知内容缺口（见 build/mobile-integration/engine-api.md §4）：
//   CHOICE/CITEM 没有 choice.id / choice.source / item.id 字段，所以
//   `node_choice_id()`、`choice_items().id/source` 只能给 0xFFFF。事件机对 0xFFFF 的处理
//   与源"该字段缺失"一致（不写 sent / 不 incoming），但 S12 的 D-Mail 发送与图内 reply
//   需要这些字段才能逐字段等于源；已向包负责人报精确字段需求。
#pragma once

#include "steins_mobile.h"
#include "steins_pack.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const steins_pack_t *pack;
} steins_mobile_pack_ctx_t;

// 初始化 ctx 并填写 content/user。pack 必须已 steins_pack_open() 成功。
// 返回 false = 包缺少手机段（v3 包）：此时 content/user 不可用，调用方必须显式失败。
bool steins_mobile_pack_content(const steins_pack_t *pack, steins_mobile_pack_ctx_t *ctx,
                                steins_mobile_content_t *content, void **user);

// 包节点下标 → 源节点号（"n123" → 123）；失败返回 0xFFFF（不 fallback 0）。
uint16_t steins_mobile_pack_source_node(const steins_pack_t *pack, uint16_t pack_node);

// **源数字 ID → 包节点下标**（正向入口）。UI/测试都用它，不要自己解析 NODE 表或硬编码
// 下标（cand-3 的 NODE 顺序是 n32/n33/…，n0 = 73）。找不到返回 false，`*pack_index` 不变。
bool steins_mobile_pack_index_of_source(const steins_pack_t *pack, uint16_t source_id,
                                        uint16_t *pack_index);

// ---------------------------------------------------------------- 两套 action 编码（**不可混用**）
// 手机 typed 域（WINOPT / CALLITEM / CALLDITEM）：pack 直接使用 `steins_mobile.h` 的
// STEINS_MOB_ACT_*（0 NONE / 1 reply / 2 send / 3 call / 4 compose / 5 noCall / 6 noSend /
// 7 readDeferred / 8 expireDeferred）。未知值必须拒绝。
bool steins_mobile_pack_typed_action(uint8_t code, uint8_t *out_action);
// 主图 legacy 域（CHOICE / CITEM）：read0 / ignore1 / send2 / noSend3 / call4 / noCall5 /
// reply6 / dmail7 → STEINS_MOB_ACT_*。未知值必须拒绝。
bool steins_mobile_pack_legacy_action(uint8_t code, uint8_t *out_action);

// 通话片段的虚拟节点空间（与剧情节点分开、同属 uint16 节点号）：0x8000 + clip 下标。
#define STEINS_MOB_CLIP_NODE_BASE 0x8000u
uint16_t steins_mobile_pack_clip_node(uint8_t clip);
bool steins_mobile_pack_clip_index(uint16_t node, uint8_t *clip);
