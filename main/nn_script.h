// main/nn_script.h —— 剧本运行时（纯逻辑：不依赖 ESP-IDF / LVGL，宿主可测）。
//
// 脚本来自资源包（tools/nn_pack.py 编译）。运行时只做三件事：
//   1. 顺序执行非阻塞操作（背景 / CG / 立绘 / 章节标题 / 特效 / 跳转），
//      维护“当前画面状态” nn_view_t；
//   2. 在 SAY（对白）与 CHOICE（选项）处停下，交还给界面层；
//   3. 选项决定下一条路径——包括“直接进入结局”，因此选项不会线性落进错误结局。
//
// 存档只需要 scene + op + view 三个字段：恢复后从该 op 继续，不必重放剧本。
#pragma once

#include "nn_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 画面状态：某一时刻屏幕上应该显示什么。
typedef struct {
    uint16_t bg;       // 背景图像 id；NN_NONE16 = 纯黑
    uint16_t cg;       // 事件 CG 图像 id；NN_NONE16 = 无（CG 覆盖背景）
    uint16_t sprite;   // 立绘图像 id；NN_NONE16 = 无
    const char *chapter;   // 最近一次章节标题（可为 NULL）
    uint16_t chapter_len;
} nn_view_t;

typedef enum {
    NN_STEP_SAY = 0,    // 停在一句对白
    NN_STEP_CHOICE,     // 停在选项
    NN_STEP_END,        // 进入结局（ending_id 有效）
    NN_STEP_BROKEN,     // 脚本异常（跳转越界 / 无限循环），界面应报错而不是静默续播
} nn_step_t;

typedef struct {
    const nn_pack_t *pack;
    uint16_t scene;
    uint16_t op;             // 下一条要执行的操作（场景内下标）
    nn_view_t view;
    // 最近一次 SAY 的输出
    const char *text;
    uint16_t text_len;
    uint16_t speaker;
    // 最近一次 CHOICE 的输出
    uint16_t choice_first;
    uint8_t choice_count;
    // 结局
    uint8_t ending_id;
    uint16_t steps;          // 本次 run 执行了多少条操作（诊断/测试用）
} nn_rt_t;

void nn_rt_start(nn_rt_t *rt, const nn_pack_t *pack);

// 从当前位置一路执行，直到遇到对白 / 选项 / 结局。
nn_step_t nn_rt_run(nn_rt_t *rt);

// 选择第 option 个选项；之后继续用 nn_rt_run()。
bool nn_rt_choose(nn_rt_t *rt, uint8_t option);

// 取选项文案（0..choice_count-1）。
const char *nn_rt_choice_text(const nn_rt_t *rt, uint8_t option, uint16_t *len);

// 取结局名与结语。
const char *nn_rt_ending_name(const nn_pack_t *pack, uint8_t ending_id, uint16_t *len);
const char *nn_rt_ending_text(const nn_pack_t *pack, uint8_t ending_id, uint16_t *len);

// 直接把位置设到 (scene, op)，view 由调用方提供（读档用）。返回 false 表示越界。
bool nn_rt_seek(nn_rt_t *rt, uint16_t scene, uint16_t op, const nn_view_t *view);

#ifdef __cplusplus
}
#endif
