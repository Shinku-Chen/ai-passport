// main/summer_model.c —— 剧本虚拟机、表达式、存档、周目进度。
#include "summer_model.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- varints
static bool rd_uvar(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    uint32_t shift = 0;
    for (int i = 0; i < 5; ++i) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *pp = p;
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool rd_svar(const uint8_t **pp, const uint8_t *end, int32_t *out)
{
    uint32_t u = 0;
    if (!rd_uvar(pp, end, &u)) return false;
    *out = (int32_t)((u >> 1) ^ (uint32_t)(-(int32_t)(u & 1u)));
    return true;
}

typedef struct {
    uint8_t *p;
    uint8_t *end;
    bool ok;
} writer_t;

static void wr_bytes(writer_t *w, const void *data, size_t len)
{
    if (!w->ok || len > (size_t)(w->end - w->p)) {
        w->ok = false;
        return;
    }
    memcpy(w->p, data, len);
    w->p += len;
}

static void wr_u8(writer_t *w, uint8_t v)
{
    wr_bytes(w, &v, 1);
}

static void wr_u16(writer_t *w, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };
    wr_bytes(w, b, 2);
}

static void wr_u32(writer_t *w, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
                           (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu) };
    wr_bytes(w, b, 4);
}

static void wr_uvar(writer_t *w, uint32_t v)
{
    uint8_t b[5];
    int n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v) b[n] |= 0x80u;
        n++;
    } while (v && n < 5);
    wr_bytes(w, b, (size_t)n);
}

static void wr_svar(writer_t *w, int32_t v)
{
    const uint32_t u = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    wr_uvar(w, u);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint32_t summer_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

// ---------------------------------------------------------------- variables
void summer_vars_init(summer_vars_t *vars)
{
    if (vars) {
        vars->count = 0;
        memset(vars->entries, 0, sizeof(vars->entries));
    }
}

static int vars_lower_bound(const summer_vars_t *vars, uint32_t id)
{
    int lo = 0;
    int hi = vars->count;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (vars->entries[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int summer_vars_clear_count(const summer_vars_t *vars)
{
    static const uint32_t flags[] = { 12003, 12004, 12005, 12006, 12575, 12007, 12008 };
    int count = 0;
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (summer_vars_get(vars, flags[i]) == 1) count++;
    }
    return count;
}

int32_t summer_vars_get(const summer_vars_t *vars, uint32_t id)
{
    if (!vars) return 0;
    if (id == 24006) return summer_vars_clear_count(vars);
    const int index = vars_lower_bound(vars, id);
    if (index < vars->count && vars->entries[index].id == id) {
        return vars->entries[index].value;
    }
    return 0;
}

bool summer_vars_set(summer_vars_t *vars, uint32_t id, int32_t value)
{
    if (!vars) return false;
    if (id == 24006) id = 24006; // stored but reads are computed
    const int index = vars_lower_bound(vars, id);
    if (index < vars->count && vars->entries[index].id == id) {
        vars->entries[index].value = value;
        return true;
    }
    if (vars->count >= SUMMER_VAR_CAP) return false;
    memmove(&vars->entries[index + 1], &vars->entries[index],
            (size_t)(vars->count - index) * sizeof(vars->entries[0]));
    vars->entries[index].id = id;
    vars->entries[index].value = value;
    vars->count++;
    return true;
}

int summer_vars_count_nonzero(const summer_vars_t *vars)
{
    int n = 0;
    if (!vars) return 0;
    for (int i = 0; i < vars->count; ++i) {
        if (vars->entries[i].value != 0) n++;
    }
    return n;
}

// ---------------------------------------------------------------- expressions
typedef struct {
    int32_t num;
    uint8_t kind;   // 0 int / 1 date / 2 literal text
    uint32_t off;
} expr_value_t;

static bool expr_strings(const summer_pack_t *pack, summer_vm_t *vm,
                         const expr_value_t *a, const expr_value_t *b, bool *equal)
{
    char buf_a[SUMMER_TEXT_CAP];
    char buf_b[SUMMER_TEXT_CAP];
    const char *sa = NULL;
    const char *sb = NULL;
    if (a->kind == 1) {
        sa = vm->date;
    } else if (a->kind == 2) {
        if (!summer_pack_text_into((summer_pack_t *)pack, a->off, buf_a, sizeof(buf_a))) return false;
        sa = buf_a;
    }
    if (b->kind == 1) {
        sb = vm->date;
    } else if (b->kind == 2) {
        if (!summer_pack_text_into((summer_pack_t *)pack, b->off, buf_b, sizeof(buf_b))) return false;
        sb = buf_b;
    }
    if (!sa || !sb) return false;
    *equal = strcmp(sa, sb) == 0;
    return true;
}

bool summer_expr_eval(const summer_pack_t *pack, uint32_t expr_id, summer_vm_t *vm,
                      int32_t *out)
{
    if (!pack || !vm || !out) return false;
    if (expr_id == 0) {
        *out = 1;
        return true;
    }
    if (expr_id >= pack->expr_count) {
        vm->error = 3;
        return false;
    }
    const uint8_t *p = pack->expr + pack->expridx[expr_id - 1];
    const uint8_t *end = pack->expr + pack->expridx[expr_id];
    expr_value_t stack[16];
    int sp = 0;
#define PUSH_INT(v) do { if (sp >= 16) { vm->error = 3; return false; } \
        stack[sp].kind = 0; stack[sp].num = (int32_t)(v); stack[sp].off = 0; sp++; } while (0)
#define REQUIRE(n) do { if (sp < (n)) { vm->error = 3; return false; } } while (0)
#define REQUIRE_INT(i) do { if (stack[(i)].kind != 0) { vm->error = 3; return false; } } while (0)
    while (p < end) {
        const uint8_t op = *p++;
        uint32_t u = 0;
        int32_t s = 0;
        switch (op) {
        case SUMMER_EX_CONST:
            if (!rd_svar(&p, end, &s)) goto bad;
            PUSH_INT(s);
            break;
        case SUMMER_EX_VAR:
            if (!rd_uvar(&p, end, &u)) goto bad;
            PUSH_INT(summer_vars_get(&vm->vars, u));
            break;
        case SUMMER_EX_VARKEY: {
            REQUIRE(1);
            REQUIRE_INT(sp - 1);
            const uint32_t key = (uint32_t)stack[sp - 1].num;
            sp--;
            PUSH_INT(summer_vars_get(&vm->vars, key));
            break;
        }
        case SUMMER_EX_DATE:
            PUSH_INT(0);
            stack[sp - 1].kind = 1;
            stack[sp - 1].off = 0;
            break;
        case SUMMER_EX_STRLIT:
            if (!rd_uvar(&p, end, &u)) goto bad;
            PUSH_INT(0);
            stack[sp - 1].kind = 2;
            stack[sp - 1].off = u;
            break;
        case SUMMER_EX_NATIVE:
            PUSH_INT(0);
            break;
        case SUMMER_EX_UNSUPPORTED:
            vm->error = 3;
            return false;
        case SUMMER_EX_NOT:
            REQUIRE(1);
            REQUIRE_INT(sp - 1);
            stack[sp - 1].num = stack[sp - 1].num ? 0 : 1;
            break;
        case SUMMER_EX_NEG:
            REQUIRE(1);
            REQUIRE_INT(sp - 1);
            stack[sp - 1].num = (int32_t)(0u - (uint32_t)stack[sp - 1].num);
            break;
        case SUMMER_EX_POS:
            REQUIRE(1);
            REQUIRE_INT(sp - 1);
            break;
        case SUMMER_EX_BNOT:
            REQUIRE(1);
            REQUIRE_INT(sp - 1);
            stack[sp - 1].num = ~stack[sp - 1].num;
            break;
        case SUMMER_EX_STREQ:
        case SUMMER_EX_STRNE: {
            REQUIRE(2);
            bool equal = false;
            if (!expr_strings(pack, vm, &stack[sp - 2], &stack[sp - 1], &equal)) goto bad;
            sp -= 2;
            PUSH_INT(equal ? (op == SUMMER_EX_STREQ ? 1 : 0) : (op == SUMMER_EX_STREQ ? 0 : 1));
            break;
        }
        default: {
            if (op < SUMMER_EX_ADD || op > SUMMER_EX_LOR) goto bad;
            REQUIRE(2);
            const expr_value_t b = stack[--sp];
            const expr_value_t a = stack[--sp];
            if (a.kind != 0 || b.kind != 0) {
                if (op == SUMMER_EX_EQ || op == SUMMER_EX_NE) {
                    bool equal = false;
                    if (!expr_strings(pack, vm, &a, &b, &equal)) goto bad;
                    PUSH_INT((op == SUMMER_EX_EQ) ? (equal ? 1 : 0) : (equal ? 0 : 1));
                    break;
                }
                vm->error = 3;
                return false;
            }
            const int32_t x = a.num;
            const int32_t y = b.num;
            int32_t r = 0;
            switch (op) {
            case SUMMER_EX_ADD: r = (int32_t)((uint32_t)x + (uint32_t)y); break;
            case SUMMER_EX_SUB: r = (int32_t)((uint32_t)x - (uint32_t)y); break;
            case SUMMER_EX_MUL: r = (int32_t)((uint32_t)x * (uint32_t)y); break;
            case SUMMER_EX_DIV: r = y == 0 ? 0 : (int32_t)((int64_t)x / (int64_t)y); break;
            case SUMMER_EX_MOD: r = y == 0 ? 0 : (int32_t)((int64_t)x % (int64_t)y); break;
            case SUMMER_EX_AND: r = x & y; break;
            case SUMMER_EX_OR: r = x | y; break;
            case SUMMER_EX_XOR: r = x ^ y; break;
            case SUMMER_EX_SHL: r = (int32_t)((uint32_t)x << (y & 31)); break;
            case SUMMER_EX_SHR: r = x >> (y & 31); break;
            case SUMMER_EX_EQ: r = x == y; break;
            case SUMMER_EX_NE: r = x != y; break;
            case SUMMER_EX_LT: r = x < y; break;
            case SUMMER_EX_LE: r = x <= y; break;
            case SUMMER_EX_GT: r = x > y; break;
            case SUMMER_EX_GE: r = x >= y; break;
            case SUMMER_EX_LAND: r = (x != 0) && (y != 0); break;
            case SUMMER_EX_LOR: r = (x != 0) || (y != 0); break;
            default: goto bad;
            }
            PUSH_INT(r);
            break;
        }
        }
    }
    REQUIRE(1);
    REQUIRE_INT(sp - 1);
    *out = stack[sp - 1].num;
    return true;
bad:
    vm->error = 3;
    return false;
#undef PUSH_INT
#undef REQUIRE
#undef REQUIRE_INT
}

// ---------------------------------------------------------------- VM
static void vm_set_scene(summer_vm_t *vm, const summer_pack_t *pack,
                         const summer_scene_t *scene)
{
    vm->scene_id = scene->id;
    vm->scene_index = (uint16_t)(scene - pack->scenes);
    vm->op_index = 0;
    vm->op_offset = 0;
    if (scene->op_count > 0) {
        uint32_t off = 0;
        if (summer_op_seek(pack, scene, 0, &off)) vm->op_offset = off;
    }
    // strings[106] is only updated when the source name carries MMDD; an empty
    // scene date keeps the previous value (engine.js sceneVariables never clears).
    if (scene->date != 0) {
        char date[24];
        if (summer_pack_text_into((summer_pack_t *)pack, scene->date, date, sizeof(date)) &&
            date[0]) {
            snprintf(vm->date, sizeof(vm->date), "%s", date);
        }
    }
}

static const summer_scene_t *vm_scene(const summer_vm_t *vm, const summer_pack_t *pack)
{
    if (!pack || vm->scene_index >= pack->scene_count) return NULL;
    return &pack->scenes[vm->scene_index];
}

void summer_vm_init(summer_vm_t *vm)
{
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
    summer_vars_init(&vm->vars);
    vm->mode = SUMMER_MODE_STORY;
}

bool summer_vm_start_story(summer_vm_t *vm, const summer_pack_t *pack)
{
    if (!vm || !pack) return false;
    summer_vm_init(vm);
    const summer_scene_t *scene = summer_scene_by_id(pack, pack->meta.flow_scene);
    if (!scene) return false;
    vm->mode = SUMMER_MODE_STORY;
    vm->root_scene = scene->id;
    vm_set_scene(vm, pack, scene);
    return true;
}

bool summer_vm_start_flow(summer_vm_t *vm, const summer_pack_t *pack, int action)
{
    if (!summer_vm_start_story(vm, pack)) return false;
    if (action == SUMMER_TITLE_ALKA) summer_vars_set(&vm->vars, 6004, 1);
    else if (action == SUMMER_TITLE_POCKET) summer_vars_set(&vm->vars, 6004, 2);
    return true;
}

bool summer_vm_start_chapter(summer_vm_t *vm, const summer_pack_t *pack, uint16_t scene_id)
{
    if (!vm || !pack) return false;
    summer_vm_init(vm);
    const summer_scene_t *scene = summer_scene_by_id(pack, scene_id);
    if (!scene) return false;
    vm->mode = SUMMER_MODE_CHAPTER;
    vm->root_scene = scene->id;
    vm_set_scene(vm, pack, scene);
    return true;
}

static bool vm_seek(summer_vm_t *vm, const summer_pack_t *pack, uint32_t index)
{
    const summer_scene_t *scene = vm_scene(vm, pack);
    if (!scene || index > scene->op_count) return false;
    vm->op_index = index;
    if (index == scene->op_count) {
        vm->op_offset = pack->ops_size;
        return true;
    }
    uint32_t off = 0;
    if (!summer_op_seek(pack, scene, index, &off)) return false;
    vm->op_offset = off;
    return true;
}

static bool vm_jump(summer_vm_t *vm, const summer_pack_t *pack, uint32_t target)
{
    return vm_seek(vm, pack, target);
}

static bool vm_next_op(summer_vm_t *vm, const summer_pack_t *pack)
{
    const summer_scene_t *scene = vm_scene(vm, pack);
    if (!scene) return false;
    if (vm->op_index >= scene->op_count) {
        vm->op_index = scene->op_count;
        vm->op_offset = pack->ops_size;
        return true;
    }
    summer_op_t op;
    const uint32_t next = summer_op_decode(pack, vm->op_offset, &op);
    if (next == 0 || next > pack->ops_size) {
        vm->error = 1;
        return false;
    }
    vm->op_index++;
    vm->op_offset = next;
    return true;
}

// 执行 finishChapter；返回 false 表示已结束。
static bool vm_finish(summer_vm_t *vm, const summer_pack_t *pack)
{
    if (vm->stack_depth > 0) {
        vm->stack_depth--;
        const uint16_t scene_id = vm->stack_scene[vm->stack_depth];
        const uint32_t op = vm->stack_op[vm->stack_depth];
        const summer_scene_t *scene = summer_scene_by_id(pack, scene_id);
        if (!scene) {
            vm->error = 2;
            return false;
        }
        vm_set_scene(vm, pack, scene);
        if (!vm_seek(vm, pack, op)) {
            vm->error = 1;
            return false;
        }
        return true;
    }
    if (vm->mode == SUMMER_MODE_STORY) {
        vm->ended = true;
        return false;
    }
    const summer_scene_t *root = summer_scene_by_id(pack, vm->root_scene);
    if (root && root->next != 0) {
        const summer_scene_t *next = summer_scene_by_id(pack, root->next);
        if (next) {
            vm->root_scene = next->id;
            vm_set_scene(vm, pack, next);
            return true;
        }
    }
    vm->ended = true;
    return false;
}

static bool vm_push(summer_vm_t *vm, uint32_t return_op)
{
    if (vm->stack_depth >= SUMMER_STACK_CAP) {
        vm->error = 2;
        return false;
    }
    vm->stack_scene[vm->stack_depth] = vm->scene_id;
    vm->stack_op[vm->stack_depth] = return_op;
    vm->stack_depth++;
    return true;
}

summer_step_t summer_vm_step(summer_vm_t *vm, const summer_pack_t *pack, int guard)
{
    summer_step_t out;
    memset(&out, 0, sizeof(out));
    if (!vm || !pack) {
        out.kind = SUMMER_STEP_ERROR;
        return out;
    }
    if (vm->error) {
        out.kind = SUMMER_STEP_ERROR;
        out.error = vm->error;
        return out;
    }
    if (guard <= 0) guard = 256;
    while (guard-- > 0) {
        if (vm->preview_mode && vm->scene_id != vm->preview_scene) {
            out.kind = SUMMER_STEP_SCENE;
            return out;
        }
        if (vm->ended) {
            out.kind = SUMMER_STEP_END;
            return out;
        }
        const summer_scene_t *scene = vm_scene(vm, pack);
        if (!scene) {
            vm->error = 1;
            out.kind = SUMMER_STEP_ERROR;
            out.error = vm->error;
            return out;
        }
        if (vm->op_index >= scene->op_count) {
            if (!vm_finish(vm, pack)) continue;
            continue;
        }
        summer_op_t op;
        const uint32_t next = summer_op_decode(pack, vm->op_offset, &op);
        if (next == 0 || next > pack->ops_size) {
            vm->error = 1;
            out.kind = SUMMER_STEP_ERROR;
            out.error = vm->error;
            return out;
        }
        switch (op.kind) {
        case SUMMER_OP_TEXT:
            out.kind = SUMMER_STEP_TEXT;
            out.speaker = op.a;
            out.text = op.b;
            return out;
        case SUMMER_OP_CHOICE:
            out.kind = SUMMER_STEP_CHOICE;
            out.group = op.a;
            return out;
        case SUMMER_OP_MAP:
            out.kind = SUMMER_STEP_MAP;
            out.group = op.a;
            return out;
        case SUMMER_OP_SET:
        case SUMMER_OP_ADD:
        case SUMMER_OP_SUB: {
            int32_t value = 0;
            if (op.tag == 0) {
                value = op.value;
            } else if (!summer_expr_eval(pack, op.b, vm, &value)) {
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            if (op.kind != SUMMER_OP_SET) {
                const int32_t old = summer_vars_get(&vm->vars, op.a);
                value = (op.kind == SUMMER_OP_ADD) ? (int32_t)((uint32_t)old + (uint32_t)value)
                                                  : (int32_t)((uint32_t)old - (uint32_t)value);
            }
            if (!summer_vars_set(&vm->vars, op.a, value)) {
                vm->error = 4;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            vm->op_index++;
            vm->op_offset = next;
            break;
        }
        case SUMMER_OP_IF: {
            int32_t value = 0;
            if (!summer_expr_eval(pack, op.a, vm, &value)) {
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            const bool pass = value != 0;
            if (pass == (op.truth != 0)) {
                if (!vm_jump(vm, pack, op.b)) {
                    vm->error = 1;
                    out.kind = SUMMER_STEP_ERROR;
                    out.error = vm->error;
                    return out;
                }
            } else {
                vm->op_index++;
                vm->op_offset = next;
            }
            break;
        }
        case SUMMER_OP_GOTO:
            if (!vm_jump(vm, pack, op.a)) {
                vm->error = 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            break;
        case SUMMER_OP_CALL:
            if (!vm_push(vm, vm->op_index + 1) || !vm_jump(vm, pack, op.a)) {
                vm->error = vm->error ? vm->error : 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            break;
        case SUMMER_OP_CALLSCENE: {
            const summer_scene_t *target = summer_scene_by_id(pack, (uint16_t)op.a);
            if (!target) {
                vm->error = 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            if (target->flags & 1u) {
                vm->op_index++;
                vm->op_offset = next;
                break;
            }
            if (!vm_push(vm, vm->op_index + 1)) {
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            if (vm->mode == SUMMER_MODE_STORY && target->route >= 0) {
                vm->root_scene = target->id;
            }
            vm_set_scene(vm, pack, target);
            if (!vm_seek(vm, pack, op.b)) {
                vm->error = 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            break;
        }
        case SUMMER_OP_JUMPSCENE: {
            if ((uint16_t)op.a == pack->meta.flow_scene && vm->mode != SUMMER_MODE_STORY) {
                if (!vm_finish(vm, pack)) continue;
                break;
            }
            const summer_scene_t *target = summer_scene_by_id(pack, (uint16_t)op.a);
            if (!target) {
                vm->error = 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            vm_set_scene(vm, pack, target);
            if (!vm_seek(vm, pack, op.b)) {
                vm->error = 1;
                out.kind = SUMMER_STEP_ERROR;
                out.error = vm->error;
                return out;
            }
            break;
        }
        case SUMMER_OP_RETURN:
        case SUMMER_OP_END:
            if (!vm_finish(vm, pack)) continue;
            break;
        case SUMMER_OP_IMAGE:
            if (op.a >= 10001 && op.a <= 16045) {
                vm->body = (uint16_t)op.a;
            } else if (op.a >= 1 && op.a <= 522) {
                vm->bg = (uint16_t)op.a;
                vm->body = 0;
            } else if (op.a >= 1001 && op.a <= 1856) {
                vm->bg = (uint16_t)op.a;
                vm->body = 0;
            }
            vm->op_index++;
            vm->op_offset = next;
            break;
        case SUMMER_OP_CLEAR:
            if (op.a == 1 || op.a == 65281 || op.a == 65535) vm->body = 0;
            vm->op_index++;
            vm->op_offset = next;
            break;
        case SUMMER_OP_NOP:
            vm->op_index++;
            vm->op_offset = next;
            break;
        default:
            vm->error = 1;
            out.kind = SUMMER_STEP_ERROR;
            out.error = vm->error;
            return out;
        }
    }
    out.kind = SUMMER_STEP_YIELD;
    return out;
}

bool summer_vm_advance(summer_vm_t *vm, const summer_pack_t *pack)
{
    if (!vm || !pack) return false;
    return vm_next_op(vm, pack);
}

bool summer_vm_choose(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group, int item_index)
{
    if (!vm || !pack) return false;
    const summer_choice_t *item = summer_choice_item(pack, group, item_index);
    if (!item) return false;
    const summer_choice_group_t *g = summer_choice_group(pack, group);
    if (!g) return false;
    if (!summer_vars_set(&vm->vars, g->var, item->value)) return false;
    return vm_next_op(vm, pack);
}

bool summer_vm_choose_map(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                          int item_index)
{
    if (!vm || !pack) return false;
    const summer_map_group_t *g = summer_map_group(pack, group);
    if (!g || item_index < 0 || item_index >= g->count) return false;
    const int32_t value = pack->map_items[g->first + (uint32_t)item_index].value;
    if (!summer_vars_set(&vm->vars, g->var, value)) return false;
    if (group < SUMMER_MAP_GROUPS && item_index < SUMMER_MAP_VALUES) {
        const uint32_t bit = group * SUMMER_MAP_VALUES + (uint32_t)item_index;
        vm->map_bits[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    }
    return vm_next_op(vm, pack);
}

int summer_vm_visible_choices(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                              int *out, int cap)
{
    const summer_choice_group_t *g = summer_choice_group(pack, group);
    if (!g || !out || cap <= 0) return 0;
    int count = 0;
    for (int i = 0; i < g->count; ++i) {
        const summer_choice_t *item = &pack->choice_items[g->first + (uint32_t)i];
        bool ok = true;
        if (item->cond != 0) {
            int32_t value = 0;
            if (!summer_expr_eval(pack, item->cond, vm, &value)) return 0;
            ok = value != 0;
        }
        if (ok && count < cap) out[count++] = i;
    }
    return count;
}

int summer_vm_preview_map(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                          summer_map_choice_t *out, int cap)
{
    const summer_map_group_t *g = summer_map_group(pack, group);
    if (!vm || !pack || !g || !out || cap <= 0) return 0;
    static summer_vm_t copy;
    int count = 0;
    for (int i = 0; i < g->count && count < cap; ++i) {
        if (group < SUMMER_MAP_GROUPS && i < SUMMER_MAP_VALUES) {
            const uint32_t bit = group * SUMMER_MAP_VALUES + (uint32_t)i;
            if (vm->map_bits[bit / 8u] & (uint8_t)(1u << (bit % 8u))) continue;
        }
        copy = *vm;
        copy.preview_mode = true;
        copy.preview_scene = copy.scene_id;
        const int32_t value = pack->map_items[g->first + (uint32_t)i].value;
        if (!summer_vars_set(&copy.vars, g->var, value)) continue;
        if (!summer_vm_advance(&copy, pack)) continue;
        uint16_t dest = 0;
        uint32_t dest_op = 0;
        for (int steps = 0; steps < 100; ++steps) {
            if (copy.scene_id != vm->scene_id) {
                const summer_scene_t *target = summer_scene_by_id(pack, copy.scene_id);
                if (target && target->route >= 0 && (target->flags & 2u) == 0) {
                    dest = copy.scene_id;
                    dest_op = copy.op_index;
                }
                break;
            }
            const summer_step_t st = summer_vm_step(&copy, pack, 64);
            if (st.kind == SUMMER_STEP_SCENE || copy.scene_id != vm->scene_id) {
                // engine.js stops at the first scene change; copy.cursor is the
                // callScene/jumpScene/return target used by mapLabels.
                const summer_scene_t *target = summer_scene_by_id(pack, copy.scene_id);
                if (target && target->route >= 0 && (target->flags & 2u) == 0) {
                    dest = copy.scene_id;
                    dest_op = copy.op_index;
                }
                break;
            }
            if (st.kind == SUMMER_STEP_TEXT) {
                if (!summer_vm_advance(&copy, pack)) break;
                continue;
            }
            break;
        }
        if (dest == 0) continue;
        const summer_scene_t *target = summer_scene_by_id(pack, dest);
        char name[SUMMER_TEXT_CAP];
        if (!target ||
            !summer_pack_text_into((summer_pack_t *)pack, target->map_name, name, sizeof(name))) {
            continue;
        }
        const char *hint = summer_map_label(pack, dest, dest_op);
        char *dst = out[count].label;
        const size_t cap_name = sizeof(out[count].label);
        snprintf(dst, cap_name, "%.*s", (int)(cap_name - 1), name);
        if (hint && hint[0]) {
            const size_t used = strlen(dst);
            if (used + 1 < cap_name) {
                dst[used] = '\n';
                dst[used + 1] = 0;
                snprintf(dst + used + 1, cap_name - used - 1, "%.*s",
                         (int)(cap_name - used - 2), hint);
            }
        }
        out[count].item_index = i;
        out[count].value = value;
        out[count].dest_scene = dest;
        out[count].dest_op = dest_op;
        count++;
    }
    return count;
}

bool summer_date_to_ascii(const char *raw, char *out, size_t cap)
{
    if (out && cap > 0) out[0] = 0;
    if (!raw || !raw[0] || !out || cap < 8) return false;
    size_t in = 0, w = 0;
    while (raw[in]) {
        if (w + 1 >= cap) {  // never accept a truncated date
            out[w] = 0;
            return false;
        }
        const unsigned char c = (unsigned char)raw[in];
        if (c == 0xEF && raw[in + 1] != 0 && raw[in + 2] != 0 &&
            (unsigned char)raw[in + 1] == 0xBC &&
            (unsigned char)raw[in + 2] >= 0x90 && (unsigned char)raw[in + 2] <= 0x99) {
            out[w++] = (char)('0' + ((unsigned char)raw[in + 2] - 0x90));
            in += 3;
        } else {
            out[w++] = raw[in++];
        }
    }
    out[w] = 0;
    static const char s_month[] = { (char)0xE6, (char)0x9C, (char)0x88, 0 };
    static const char s_day[] = { (char)0xE6, (char)0x97, (char)0xA5, 0 };
    const char *mon = strstr(out, s_month);
    const char *day = strstr(out, s_day);
    // "日" is three UTF-8 bytes; day[1] is 0x97, not the terminator.
    if (!mon || !day || day[sizeof(s_day) - 1] != 0 || mon > day) return false;
    int month = 0, d = 0;
    int month_digits = 0, day_digits = 0;
    for (const char *p = out; p < mon; ++p) {
        if (*p < '0' || *p > '9' || month_digits == 2) return false;
        month = month * 10 + (*p - '0');
        month_digits++;
    }
    for (const char *p = mon + 3; p < day; ++p) {
        if (*p < '0' || *p > '9' || day_digits == 2) return false;
        d = d * 10 + (*p - '0');
        day_digits++;
    }
    if (month_digits == 0 || day_digits == 0) return false;
    return month >= 1 && month <= 12 && d >= 1 && d <= 31;
}

const char *summer_vm_scene_label(const summer_vm_t *vm, const summer_pack_t *pack)
{
    static char label[SUMMER_TEXT_CAP];
    label[0] = 0;
    if (!vm || !pack) return label;
    const summer_scene_t *scene = vm_scene(vm, pack);
    if (!scene) return label;
    int flow_index = -1;
    for (int i = 0; i < vm->stack_depth; ++i) {
        if (vm->stack_scene[i] == pack->meta.flow_scene) {
            flow_index = i;
            break;
        }
    }
    if (vm->mode == SUMMER_MODE_STORY && flow_index >= 0) {
        const char *flow = summer_flow_text(pack, vm->stack_op[flow_index], 0);
        if (flow && flow[0]) {
            snprintf(label, sizeof(label), "%s", flow);
            return label;
        }
    }
    if ((flow_index < 0 || vm->mode != SUMMER_MODE_STORY) &&
        (scene->phase == 1 || scene->phase == 2) && scene->route_name != 0) {
        if (summer_pack_text_into((summer_pack_t *)pack, scene->route_name, label, sizeof(label)) &&
            label[0]) {
            return label;
        }
    }
    char disp[24];
    if (summer_date_to_ascii(vm->date, disp, sizeof(disp))) {
        snprintf(label, sizeof(label), "%s", disp);
        return label;
    }
    if (scene->date_label != 0 &&
        summer_pack_text_into((summer_pack_t *)pack, scene->date_label, label, sizeof(label)) &&
        label[0]) {
        return label;
    }
    if (flow_index >= 0) {
        const char *flow = summer_flow_text(pack, vm->stack_op[flow_index], 1);
        if (flow && flow[0]) {
            snprintf(label, sizeof(label), "%s", flow);
            return label;
        }
    }
    for (int i = vm->stack_depth - 1; i >= 0; --i) {
        const summer_scene_t *parent = summer_scene_by_id(pack, vm->stack_scene[i]);
        if (parent && parent->date_label != 0 &&
            summer_pack_text_into((summer_pack_t *)pack, parent->date_label, label, sizeof(label)) &&
            label[0]) {
            return label;
        }
    }
    snprintf(label, sizeof(label), "7月29日");
    return label;
}

int summer_vm_ending(const summer_vm_t *vm, const summer_pack_t *pack)
{
    if (!vm || !pack || vm->scene_id != pack->meta.flow_scene) return 0;
    return summer_ending_for_cursor(pack, vm->scene_id, vm->op_index);
}

// ---------------------------------------------------------------- progress
void summer_progress_init(summer_progress_t *progress)
{
    if (!progress) return;
    memset(progress, 0, sizeof(*progress));
}

int summer_progress_heroine_count(const summer_progress_t *progress)
{
    static const int heroines[] = { 4, 5, 6, 7, 13 };
    int n = 0;
    for (size_t i = 0; i < sizeof(heroines) / sizeof(heroines[0]); ++i) {
        if (progress->cleared & (uint16_t)(1u << (heroines[i] - 1))) n++;
    }
    return n;
}

bool summer_progress_has_alka(const summer_progress_t *progress)
{
    return (progress->cleared & (uint16_t)(1u << (8 - 1))) != 0;
}

bool summer_progress_has_pocket(const summer_progress_t *progress)
{
    return (progress->cleared & (uint16_t)(1u << (9 - 1))) != 0;
}

void summer_progress_reconcile(summer_progress_t *progress)
{
    if (!progress) return;
    const int count = summer_progress_heroine_count(progress);
    if (count >= 4 && progress->restart_stage == 0) progress->restart_stage = 1;
    if (count >= 5 && progress->restart_stage == 2) progress->restart_stage = 3;
}

unsigned summer_progress_title_actions(const summer_progress_t *progress)
{
    if (!progress) return SUMMER_TITLE_START;
    const bool pocket = summer_progress_has_pocket(progress);
    const bool alka = summer_progress_has_alka(progress);
    if (!pocket && (progress->restart_stage == 1 || progress->restart_stage == 3)) {
        return SUMMER_TITLE_RESTART;
    }
    if (pocket) return SUMMER_TITLE_START | SUMMER_TITLE_ALKA | SUMMER_TITLE_POCKET;
    if (alka) return SUMMER_TITLE_POCKET;
    if (summer_progress_heroine_count(progress) == 5) return SUMMER_TITLE_ALKA;
    return SUMMER_TITLE_START;
}

void summer_progress_apply(summer_vm_t *vm, const summer_pack_t *pack,
                           const summer_progress_t *progress)
{
    if (!vm || !pack || !progress) return;
    static const uint32_t keys[6] = { 12001, 12002, 12010, 12011, 12012, 12013 };
    for (int i = 0; i < 6; ++i) {
        const int32_t old = summer_vars_get(&vm->vars, keys[i]);
        if ((int32_t)progress->globals[i] > old) {
            summer_vars_set(&vm->vars, keys[i], (int32_t)progress->globals[i]);
        }
    }
    for (int ending = 1; ending <= 14; ++ending) {
        if ((progress->cleared & (uint16_t)(1u << (ending - 1))) == 0) continue;
        const uint32_t var = pack->progress.clear_var[ending];
        if (var) summer_vars_set(&vm->vars, var, 1);
    }
    const uint32_t old_stage = (uint32_t)summer_vars_get(&vm->vars, pack->progress.restart_var);
    if (progress->restart_stage > old_stage) {
        summer_vars_set(&vm->vars, pack->progress.restart_var, (int32_t)progress->restart_stage);
    }
    summer_vars_set(&vm->vars, 24006, summer_vars_clear_count(&vm->vars));
}

bool summer_progress_sync(summer_vm_t *vm, const summer_pack_t *pack,
                          summer_progress_t *progress)
{
    if (!vm || !pack || !progress) return false;
    const uint16_t old_cleared = progress->cleared;
    const uint8_t old_stage = progress->restart_stage;
    uint32_t old_globals[6];
    memcpy(old_globals, progress->globals, sizeof(old_globals));
    for (int ending = 1; ending <= 14; ++ending) {
        const uint32_t var = pack->progress.clear_var[ending];
        if (var && summer_vars_get(&vm->vars, var) == 1) {
            progress->cleared |= (uint16_t)(1u << (ending - 1));
        }
    }
    const int32_t stage = summer_vars_get(&vm->vars, pack->progress.restart_var);
    if (stage >= 0 && stage <= 4 && stage > (int32_t)progress->restart_stage) {
        progress->restart_stage = (uint8_t)stage;
    }
    static const uint32_t keys[6] = { 12001, 12002, 12010, 12011, 12012, 12013 };
    for (int i = 0; i < 6; ++i) {
        const int32_t value = summer_vars_get(&vm->vars, keys[i]);
        if (value > 0 && (uint32_t)value > progress->globals[i]) {
            progress->globals[i] = (uint32_t)value;
        }
    }
    summer_progress_reconcile(progress);
    const bool changed = progress->cleared != old_cleared ||
                         progress->restart_stage != old_stage ||
                         memcmp(old_globals, progress->globals, sizeof(old_globals)) != 0;
    if (changed) progress->dirty = true;
    return changed;
}

bool summer_progress_cg_seen(const summer_progress_t *progress, int group)
{
    if (!progress || group < 0 || group >= SUMMER_GALLERY_GROUPS_MAX) return false;
    return (progress->seen_cg[group / 8] & (uint8_t)(1u << (group % 8))) != 0;
}

bool summer_progress_set_cg(summer_progress_t *progress, int group)
{
    if (!progress || group < 0 || group >= SUMMER_GALLERY_GROUPS_MAX) return false;
    const uint8_t mask = (uint8_t)(1u << (group % 8));
    if (progress->seen_cg[group / 8] & mask) return false;
    progress->seen_cg[group / 8] |= mask;
    progress->dirty = true;
    return true;
}

bool summer_progress_merge(summer_progress_t *progress, uint16_t cleared,
                           const uint8_t *seen_cg, uint16_t seen_len)
{
    if (!progress) return false;
    bool changed = false;
    if ((cleared & (uint16_t)~progress->cleared) != 0) {
        progress->cleared |= cleared;
        changed = true;
    }
    if (seen_cg) {
        const size_t n = seen_len < sizeof(progress->seen_cg) ? seen_len : sizeof(progress->seen_cg);
        for (size_t i = 0; i < n; ++i) {
            if ((progress->seen_cg[i] | seen_cg[i]) != progress->seen_cg[i]) {
                progress->seen_cg[i] |= seen_cg[i];
                changed = true;
            }
        }
    }
    if (changed) {
        summer_progress_reconcile(progress);
        progress->dirty = true;
    }
    return changed;
}

bool summer_progress_merge_old(summer_progress_t *progress, uint16_t cleared,
                               const uint8_t *seen_cg, uint16_t seen_len)
{
    return summer_progress_merge(progress, cleared, seen_cg, seen_len);
}

bool summer_progress_encode(const summer_progress_t *progress, uint8_t *out, size_t cap,
                            size_t *len)
{
    if (!progress || !out || cap < 70) return false;
    writer_t w = { out, out + cap, true };
    wr_bytes(&w, "SPRG", 4);
    wr_u16(&w, 1);
    wr_u16(&w, progress->cleared);
    wr_u8(&w, progress->restart_stage);
    wr_u8(&w, 0);
    for (int i = 0; i < 6; ++i) wr_u32(&w, progress->globals[i]);
    wr_bytes(&w, progress->seen_cg, sizeof(progress->seen_cg));
    if (!w.ok) return false;
    const uint32_t crc = summer_crc32(out, (size_t)(w.p - out));
    wr_u32(&w, crc);
    if (!w.ok) return false;
    if (len) *len = (size_t)(w.p - out);
    return true;
}

bool summer_progress_decode(summer_progress_t *progress, const uint8_t *data, size_t len)
{
    if (!progress || !data || len < 70) return false;
    if (memcmp(data, "SPRG", 4) != 0 || rd16(data + 4) != 1) return false;
    if (summer_crc32(data, len - 4) != rd32(data + len - 4)) return false;
    summer_progress_t p;
    memset(&p, 0, sizeof(p));
    p.cleared = rd16(data + 6);
    p.restart_stage = data[8];
    if (p.restart_stage > 4) return false;
    for (int i = 0; i < 6; ++i) p.globals[i] = rd32(data + 10 + i * 4);
    memcpy(p.seen_cg, data + 10 + 24, sizeof(p.seen_cg));
    p.dirty = false;
    *progress = p;
    return true;
}

// ---------------------------------------------------------------- save codec
static bool save_pack_resolve(const summer_pack_t *pack, uint16_t scene_id,
                              const summer_scene_t **scene)
{
    const summer_scene_t *s = summer_scene_by_id(pack, scene_id);
    if (!s) return false;
    if (scene) *scene = s;
    return true;
}

size_t summer_save_encode(const summer_vm_t *vm, uint8_t page, const summer_progress_t *progress,
                          uint8_t *out, size_t cap)
{
    if (!vm || !out || cap < 32) return 0;
    writer_t w = { out, out + cap, true };
    wr_bytes(&w, "SVMR", 4);
    wr_u16(&w, SUMMER_SAVE_VERSION);
    wr_u8(&w, (uint8_t)((vm->mode == SUMMER_MODE_CHAPTER ? 1u : 0u) | (vm->ended ? 2u : 0u)));
    wr_u16(&w, vm->scene_id);
    wr_u16(&w, (uint16_t)(vm->op_index > 0xFFFFu ? 0xFFFFu : vm->op_index));
    wr_u16(&w, vm->root_scene);
    wr_u8(&w, page);
    wr_u8(&w, (uint8_t)(vm->choice_sel > 255 ? 0 : vm->choice_sel));
    wr_u16(&w, vm->bg);
    wr_u16(&w, vm->body);
    const size_t date_len = strnlen(vm->date, sizeof(vm->date) - 1);
    wr_u8(&w, (uint8_t)date_len);
    wr_bytes(&w, vm->date, date_len);
    wr_u8(&w, vm->stack_depth);
    for (int i = 0; i < vm->stack_depth; ++i) {
        wr_u16(&w, vm->stack_scene[i]);
        wr_u32(&w, vm->stack_op[i]);
    }
    uint16_t var_count = 0;
    for (int i = 0; i < vm->vars.count; ++i) {
        if (vm->vars.entries[i].value != 0) var_count++;
    }
    wr_u16(&w, var_count);
    for (int i = 0; i < vm->vars.count; ++i) {
        if (vm->vars.entries[i].value == 0) continue;
        wr_uvar(&w, vm->vars.entries[i].id);
        wr_svar(&w, vm->vars.entries[i].value);
    }
    uint16_t map_len = 0;
    for (size_t i = 0; i < sizeof(vm->map_bits); ++i) {
        if (vm->map_bits[i]) {
            map_len = (uint16_t)sizeof(vm->map_bits);
            break;
        }
    }
    wr_u16(&w, map_len);
    if (map_len) wr_bytes(&w, vm->map_bits, map_len);
    const uint8_t *seen = progress ? progress->seen_cg : NULL;
    wr_u8(&w, seen ? (uint8_t)sizeof(progress->seen_cg) : 0);
    if (seen) wr_bytes(&w, seen, sizeof(progress->seen_cg));
    wr_u16(&w, progress ? progress->cleared : 0);
    if (!w.ok) return 0;
    const uint32_t crc = summer_crc32(out, (size_t)(w.p - out));
    wr_u32(&w, crc);
    if (!w.ok) return 0;
    return (size_t)(w.p - out);
}

bool summer_save_decode(const summer_pack_t *pack, const uint8_t *data, size_t len,
                        summer_save_t *out, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
#define FAIL(msg) do { if (err && err_cap) snprintf(err, err_cap, "%s", msg); return false; } while (0)
    if (!pack || !data || !out || len < 32) FAIL("存档过短");
    if (memcmp(data, "SVMR", 4) != 0) FAIL("存档标识无效");
    if (rd16(data + 4) != SUMMER_SAVE_VERSION) FAIL("存档版本不兼容");
    if (summer_crc32(data, len - 4) != rd32(data + len - 4)) FAIL("存档校验失败");
    // Parse directly into the caller's scratch save.  This keeps the 6.7 KB
    // decoder frame off the input task stack; on failure *out may be partial,
    // but the live VM is never touched.  Callers use *out only on success.
    summer_save_t *s = out;
    memset(s, 0, sizeof(*s));
    const uint8_t flags = data[6];
    s->mode = (flags & 1u) ? SUMMER_MODE_CHAPTER : SUMMER_MODE_STORY;
    s->ended = (flags & 2u) != 0;
    s->scene_id = rd16(data + 7);
    s->op_index = rd16(data + 9);
    s->root_scene = rd16(data + 11);
    s->page = data[13];
    s->choice_sel = data[14];
    s->bg = rd16(data + 15);
    s->body = rd16(data + 17);
    size_t p = 19;
    const size_t date_len = data[p++];
    if (date_len >= sizeof(s->date)) FAIL("存档日期过长");
    if (p + date_len > len) FAIL("存档截断");
    memcpy(s->date, data + p, date_len);
    s->date[date_len] = 0;
    p += date_len;
    if (p >= len) FAIL("存档截断");
    s->stack_depth = data[p++];
    if (s->stack_depth > SUMMER_STACK_CAP) FAIL("调用栈过深");
    for (int i = 0; i < s->stack_depth; ++i) {
        if (p + 6 > len) FAIL("存档截断");
        s->stack_scene[i] = rd16(data + p);
        s->stack_op[i] = rd32(data + p + 2);
        p += 6;
    }
    if (p + 2 > len) FAIL("存档截断");
    s->var_count = rd16(data + p);
    p += 2;
    if (s->var_count > SUMMER_VAR_CAP) FAIL("变量过多");
    summer_vars_init(&s->vars);
    for (int i = 0; i < s->var_count; ++i) {
        uint32_t id = 0;
        int32_t value = 0;
        const uint8_t *pp = data + p;
        if (!rd_uvar(&pp, data + len - 4, &id) || !rd_svar(&pp, data + len - 4, &value)) {
            FAIL("存档变量截断");
        }
        p = (size_t)(pp - data);
        if (!summer_vars_set(&s->vars, id, value)) FAIL("变量过多");
    }
    if (p >= len) FAIL("存档截断");
    if (p + 2 > len) FAIL("存档截断");
    const size_t map_len = rd16(data + p);
    p += 2;
    if (map_len > sizeof(s->map_bits)) FAIL("地图记录过长");
    if (p + map_len > len) FAIL("存档截断");
    memcpy(s->map_bits, data + p, map_len);
    p += map_len;
    if (p >= len) FAIL("存档截断");
    const size_t seen_len = data[p++];
    if (seen_len > sizeof(s->seen_cg)) FAIL("鉴赏记录过长");
    if (p + seen_len > len) FAIL("存档截断");
    memcpy(s->seen_cg, data + p, seen_len);
    s->seen_cg_count = (uint16_t)seen_len;
    p += seen_len;
    if (p + 2 > len - 4) FAIL("存档截断");
    s->cleared = rd16(data + p);

    const summer_scene_t *scene = NULL;
    if (!save_pack_resolve(pack, s->scene_id, &scene)) FAIL("存档场景无效");
    if (s->op_index > scene->op_count) FAIL("存档位置无效");
    if (s->page > 15) FAIL("存档页号无效");
    if (s->bg != 0 && !summer_image(pack, s->bg)) FAIL("存档背景无效");
    if (s->body != 0 && !summer_image(pack, s->body)) FAIL("存档立绘无效");
    if (!save_pack_resolve(pack, s->root_scene, NULL)) FAIL("存档根章节无效");
    for (int i = 0; i < s->stack_depth; ++i) {
        const summer_scene_t *frame = NULL;
        if (!save_pack_resolve(pack, s->stack_scene[i], &frame)) FAIL("存档调用位置无效");
        if (s->stack_op[i] > frame->op_count) FAIL("存档调用位置无效");
    }
    return true;
#undef FAIL
}

bool summer_save_to_vm(const summer_save_t *save, summer_vm_t *vm,
                       const summer_pack_t *pack)
{
    if (!save || !vm || !pack) return false;
    const summer_scene_t *scene = summer_scene_by_id(pack, save->scene_id);
    if (!scene || save->op_index > scene->op_count) return false;
    summer_vm_init(vm);
    vm->mode = save->mode;
    vm->ended = save->ended;
    vm->scene_id = save->scene_id;
    vm->root_scene = save->root_scene;
    vm->op_index = save->op_index;
    vm->op_offset = 0;
    vm->choice_sel = save->choice_sel;
    vm->bg = save->bg;
    vm->body = save->body;
    memcpy(vm->date, save->date, sizeof(vm->date));
    vm->vars = save->vars;
    memcpy(vm->map_bits, save->map_bits, sizeof(vm->map_bits));
    vm->stack_depth = save->stack_depth;
    memcpy(vm->stack_scene, save->stack_scene, sizeof(vm->stack_scene));
    memcpy(vm->stack_op, save->stack_op, sizeof(vm->stack_op));
    vm->scene_index = (uint16_t)(scene - pack->scenes);
    if (save->op_index == scene->op_count) {
        vm->op_offset = pack->ops_size;
    } else {
        uint32_t off = 0;
        if (!summer_op_seek(pack, scene, save->op_index, &off)) return false;
        vm->op_offset = off;
    }
    return true;
}

bool summer_save_preview(const summer_pack_t *pack, const summer_save_t *save, char *out,
                         size_t cap)
{
    if (!pack || !save || !out || cap == 0) return false;
    out[0] = 0;
    const summer_scene_t *scene = summer_scene_by_id(pack, save->scene_id);
    if (!scene) return false;
    char buf[SUMMER_TEXT_CAP];
    if (save->op_index < scene->op_count) {
        uint32_t off = 0;
        summer_op_t op;
        if (summer_op_seek(pack, scene, save->op_index, &off) &&
            summer_op_decode(pack, off, &op)) {
            if (op.kind == SUMMER_OP_TEXT) {
                if (summer_pack_text_into((summer_pack_t *)pack, op.b, buf, sizeof(buf))) {
                    // saves.js: replace ・ with ·, collapse whitespace, take 20
                    // characters and append an ellipsis when truncated.
                    char norm[SUMMER_TEXT_CAP];
                    size_t w = 0;
                    bool space = false;
                    for (size_t i = 0; buf[i] && w + 4 < sizeof(norm);) {
                        const unsigned char c = (unsigned char)buf[i];
                        if (c == 0xE3 && (unsigned char)buf[i + 1] == 0x83 &&
                            (unsigned char)buf[i + 2] == 0xBB) {
                            norm[w++] = (char)0xC2;
                            norm[w++] = (char)0xB7;
                            i += 3;
                            space = false;
                        } else if (c == ' ' || c == 0x09 || c == 0x0A || c == 0x0D) {
                            if (!space) norm[w++] = ' ';
                            space = true;
                            i++;
                        } else {
                            norm[w++] = buf[i++];
                            space = false;
                        }
                    }
                    norm[w] = 0;
                    const size_t len_norm = strlen(norm);
                    size_t pos = 0, chars = 0;
                    while (pos < len_norm && chars < 20) {
                        const unsigned char c = (unsigned char)norm[pos];
                        pos += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                        if (pos > len_norm) pos = len_norm;
                        chars++;
                    }
                    snprintf(out, cap, "%.*s%s", (int)pos, norm, norm[pos] ? "…" : "");
                    return true;
                }
            } else if (op.kind == SUMMER_OP_CHOICE) {
                snprintf(out, cap, "正在选择下一步");
                return true;
            } else if (op.kind == SUMMER_OP_MAP) {
                snprintf(out, cap, "选择地点");
                return true;
            }
        }
    }
    snprintf(out, cap, "%s", save->ended ? "已到达结局" : "章节开头");
    return true;
}
