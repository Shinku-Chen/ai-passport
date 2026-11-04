// main/gal_expr.h —— 预编译表达式求值器(源 expressions.js 语义的受限实现)。
//
// 打包器把源表达式编译成 RPN 字节码;这里只解释字节码,不做文本解析。
// 数值使用 float(源游戏里用于布局/比例);字符串用包内 NUL 字符串 id 表示,
// 比较按 id(打包器已去重,同一文本必为同一 id)。
#pragma once

#include "gal_pack.h"

#include <stdbool.h>
#include <stdint.h>

enum { GAL_VAL_NUM = 0, GAL_VAL_STR = 1, GAL_VAL_BOOL = 2 };

typedef struct {
    uint8_t type;
    uint8_t _pad;
    float num;
    uint16_t str;
    uint16_t _pad2;
} gal_val_t;

typedef struct {
    const gal_pack_t *pack;
    gal_val_t *vars;
    uint16_t var_count;
} gal_expr_ctx_t;

// 变量求值栈深度(源表达式最深处 < 10,留余量)。
#define GAL_EXPR_STACK 24

// 按包的变量表初始化默认值。story=true 表示新游戏(源 newState(chapter,true)),
// false 表示章节直达(源 newState(chapter) -> isDebugDirectJump=true)。
void gal_expr_init(gal_expr_ctx_t *ctx, const gal_pack_t *pack, gal_val_t *vars,
                   uint16_t var_count, bool story);

// 查找变量下标,找不到返回 GAL_NONE。按名字线性扫描,只在初始化/调试用。
uint16_t gal_expr_find(const gal_pack_t *pack, const char *name);

// 求值。expr_id 越界或字节码非法返回 false。
bool gal_expr_eval(const gal_expr_ctx_t *ctx, uint16_t expr_id, gal_val_t *out);

// 便捷取值。
static inline float gal_val_num(const gal_val_t *v)
{
    return v ? (v->type == GAL_VAL_BOOL ? (v->num != 0.0f ? 1.0f : 0.0f) : v->num) : 0.0f;
}
static inline bool gal_val_bool(const gal_val_t *v)
{
    if (!v) return false;
    if (v->type == GAL_VAL_BOOL) return v->num != 0.0f;
    if (v->type == GAL_VAL_STR) return v->str != GAL_NONE;
    return v->num != 0.0f;
}
static inline int32_t gal_val_int(const gal_val_t *v)
{
    const float f = gal_val_num(v);
    if (f > 2147483000.0f) return 2147483000;
    if (f < -2147483000.0f) return -2147483000;
    return (int32_t)f;
}
