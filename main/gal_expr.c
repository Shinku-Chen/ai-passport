// main/gal_expr.c —— 预编译表达式求值器(纯 C,无 ESP-IDF/LVGL 依赖)。
#include "gal_expr.h"

#include <math.h>
#include <string.h>

enum {
    X_PUSH_NUM = 1,
    X_PUSH_STR = 2,
    X_PUSH_VAR = 3,
    X_PUSH_TRUE = 4,
    X_PUSH_FALSE = 5,
    X_NEG = 6,
    X_NOT = 7,
    X_ADD = 8,
    X_SUB = 9,
    X_MUL = 10,
    X_DIV = 11,
    X_MOD = 12,
    X_EQ = 13,
    X_NE = 14,
    X_LT = 15,
    X_LE = 16,
    X_GT = 17,
    X_GE = 18,
    X_AND = 19,
    X_OR = 20,
    X_MAX = 21,
    X_MIN = 22,
    X_SIN = 23,
    X_COS = 24,
    X_ABS = 25,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static float rd_f32(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t gal_expr_find(const gal_pack_t *pack, const char *name)
{
    if (!pack || !name) return GAL_NONE;
    for (uint16_t i = 0; i < pack->count[GAL_SEC_VARS]; ++i) {
        const gal_var_t *v = gal_pack_var(pack, i);
        if (v && strcmp(gal_pack_str(pack, (uint16_t)v->name), name) == 0) return i;
    }
    return GAL_NONE;
}

void gal_expr_init(gal_expr_ctx_t *ctx, const gal_pack_t *pack, gal_val_t *vars,
                   uint16_t var_count, bool story)
{
    if (!ctx) return;
    ctx->pack = pack;
    ctx->vars = vars;
    ctx->var_count = var_count;
    if (!pack || !vars) return;
    const uint16_t n = pack->count[GAL_SEC_VARS] < var_count ? pack->count[GAL_SEC_VARS] : var_count;
    for (uint16_t i = 0; i < n; ++i) {
        const gal_var_t *v = gal_pack_var(pack, i);
        gal_val_t val = { 0 };
        val.type = v ? v->type : GAL_VAL_NUM;
        if (v) memcpy(&val.num, &v->default_bits, sizeof(val.num));
        if (val.type == GAL_VAL_STR) val.str = GAL_NONE;
        if (val.type == GAL_VAL_BOOL) val.num = val.num != 0.0f ? 1.0f : 0.0f;
        vars[i] = val;
    }
    for (uint16_t i = n; i < var_count; ++i) {
        vars[i] = (gal_val_t){ .type = GAL_VAL_NUM, .num = 0.0f, .str = GAL_NONE };
    }
    const uint16_t direct = gal_expr_find(pack, "isDebugDirectJump");
    if (direct != GAL_NONE && direct < var_count) {
        vars[direct].type = GAL_VAL_BOOL;
        vars[direct].num = story ? 0.0f : 1.0f;
    }
}

static bool truthy(const gal_expr_ctx_t *ctx, const gal_val_t *v)
{
    if (!v) return false;
    if (v->type == GAL_VAL_BOOL) return v->num != 0.0f;
    if (v->type == GAL_VAL_STR) {
        if (v->str == GAL_NONE) return false;
        const char *s = gal_pack_str(ctx->pack, v->str);
        return s && s[0] != 0;
    }
    return v->num != 0.0f;
}

static bool strict_eq(const gal_val_t *a, const gal_val_t *b)
{
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (a->type == GAL_VAL_NUM) return a->num == b->num;
    if (a->type == GAL_VAL_BOOL) return (a->num != 0.0f) == (b->num != 0.0f);
    return a->str == b->str;
}

bool gal_expr_eval(const gal_expr_ctx_t *ctx, uint16_t expr_id, gal_val_t *out)
{
    if (!ctx || !ctx->pack || !ctx->vars || !out) return false;
    const gal_expr_t *e = gal_pack_expr(ctx->pack, expr_id);
    if (!e) return false;
    const uint8_t *code = (const uint8_t *)ctx->pack->sec[GAL_SEC_EXPR_DATA] + e->off;
    const uint32_t len = e->len;
    uint32_t pc = 0;
    gal_val_t st[GAL_EXPR_STACK];
    int sp = 0;

    while (pc < len) {
        const uint8_t op = code[pc++];
        switch (op) {
        case X_PUSH_NUM:
            if (pc + 4 > len || sp >= GAL_EXPR_STACK) return false;
            st[sp].type = GAL_VAL_NUM;
            st[sp].num = rd_f32(code + pc);
            st[sp].str = GAL_NONE;
            sp++;
            pc += 4;
            break;
        case X_PUSH_STR:
            if (pc + 2 > len || sp >= GAL_EXPR_STACK) return false;
            st[sp].type = GAL_VAL_STR;
            st[sp].num = 0;
            st[sp].str = rd16(code + pc);
            if (st[sp].str >= ctx->pack->count[GAL_SEC_STR]) return false;
            sp++;
            pc += 2;
            break;
        case X_PUSH_VAR: {
            if (pc + 2 > len || sp >= GAL_EXPR_STACK) return false;
            const uint16_t id = rd16(code + pc);
            if (id >= ctx->var_count) return false;
            st[sp] = ctx->vars[id];
            sp++;
            pc += 2;
            break;
        }
        case X_PUSH_TRUE:
        case X_PUSH_FALSE:
            if (sp >= GAL_EXPR_STACK) return false;
            st[sp].type = GAL_VAL_BOOL;
            st[sp].num = (op == X_PUSH_TRUE) ? 1.0f : 0.0f;
            st[sp].str = GAL_NONE;
            sp++;
            break;
        case X_NEG:
        case X_NOT:
            if (sp < 1) return false;
            if (op == X_NEG) {
                st[sp - 1].num = -gal_val_num(&st[sp - 1]);
                st[sp - 1].type = GAL_VAL_NUM;
            } else {
                const bool b = !truthy(ctx, &st[sp - 1]);
                st[sp - 1].type = GAL_VAL_BOOL;
                st[sp - 1].num = b ? 1.0f : 0.0f;
            }
            break;
        case X_ADD:
        case X_SUB:
        case X_MUL:
        case X_DIV:
        case X_MOD:
        case X_LT:
        case X_LE:
        case X_GT:
        case X_GE:
        case X_AND:
        case X_OR:
        case X_EQ:
        case X_NE: {
            if (sp < 2) return false;
            const gal_val_t b = st[--sp];
            gal_val_t *a = &st[sp - 1];
            if (op == X_EQ || op == X_NE) {
                const bool eq = strict_eq(a, &b);
                a->type = GAL_VAL_BOOL;
                a->num = ((op == X_EQ) == eq) ? 1.0f : 0.0f;
            } else if (op == X_AND || op == X_OR) {
                const bool av = truthy(ctx, a), bv = truthy(ctx, &b);
                a->type = GAL_VAL_BOOL;
                a->num = (op == X_AND ? (av && bv) : (av || bv)) ? 1.0f : 0.0f;
            } else if (op == X_LT || op == X_LE || op == X_GT || op == X_GE) {
                const float av = gal_val_num(a), bv = gal_val_num(&b);
                bool r = false;
                if (op == X_LT) r = av < bv;
                else if (op == X_LE) r = av <= bv;
                else if (op == X_GT) r = av > bv;
                else r = av >= bv;
                a->type = GAL_VAL_BOOL;
                a->num = r ? 1.0f : 0.0f;
            } else {
                const float av = gal_val_num(a), bv = gal_val_num(&b);
                float r = 0;
                if (op == X_ADD) r = av + bv;
                else if (op == X_SUB) r = av - bv;
                else if (op == X_MUL) r = av * bv;
                else if (op == X_DIV) r = av / bv;
                else r = fmodf(av, bv);
                a->type = GAL_VAL_NUM;
                a->num = r;
            }
            break;
        }
        case X_MAX:
        case X_MIN: {
            if (pc >= len) return false;
            const int extra = code[pc++];
            const int argc = extra + 2;
            if (sp < argc) return false;
            float v = gal_val_num(&st[sp - argc]);
            for (int i = 1; i < argc; ++i) {
                const float n = gal_val_num(&st[sp - argc + i]);
                v = (op == X_MAX) ? (n > v ? n : v) : (n < v ? n : v);
            }
            sp -= argc - 1;
            st[sp - 1].type = GAL_VAL_NUM;
            st[sp - 1].num = v;
            break;
        }
        case X_SIN:
        case X_COS:
        case X_ABS:
            if (sp < 1) return false;
            st[sp - 1].num = op == X_SIN ? sinf(st[sp - 1].num)
                             : op == X_COS ? cosf(st[sp - 1].num)
                             : fabsf(st[sp - 1].num);
            st[sp - 1].type = GAL_VAL_NUM;
            break;
        default:
            return false;
        }
    }
    if (sp != 1) return false;
    *out = st[0];
    return true;
}
