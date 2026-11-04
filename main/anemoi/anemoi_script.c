// main/anemoi/anemoi_script.c —— 剧情解释器实现（纯 C，可在宿主机测试）。
//
// 结构：表达式字节码求值 -> native / helper -> 指令循环。所有调用都以
// "源快照 engine-vm.js 的行为" 为准；host 测试用真实包走完整路线与结局。
#include "anemoi_script.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { E_NUM = 1, E_STR = 2, E_VAR = 3, E_ARRAY = 4, E_UN = 5, E_BIN = 6, E_CALL = 7,
       E_METHOD = 8, E_VARDYN = 9 };

enum { M_LEFT = 0, M_RIGHT = 1, M_MID = 2, M_COUNT = 3, M_TONUM = 4, M_SEARCH = 5,
       M_GET_SIZE = 6, M_GET = 7, M_GET_FILE_NAME = 8, M_CHECK = 9 };

static uint8_t rd8(const uint8_t *p) { return p[0]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void refresh_bodies(anem_script_t *s);

static void set_error(anem_script_t *s, const char *msg)
{
    s->failed = true;
    snprintf(s->notice, sizeof(s->notice), "%s", msg ? msg : "剧情解释器错误");
}

static anem_value_t v_num(int32_t n)
{
    anem_value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = ANEM_VAL_NUM;
    v.num = n;
    return v;
}

static anem_value_t v_str(uint16_t text)
{
    anem_value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = ANEM_VAL_STR;
    v.text = text;
    return v;
}

const char *anem_script_text(const anem_script_t *s, uint16_t id)
{
    if (s == NULL) {
        return "";
    }
    if (id & ANEM_SCRATCH_FLAG) {
        const uint8_t slot = (uint8_t)(id & 0x7F);
        return slot < ANEM_SCRATCH_SLOTS ? s->scratch[slot] : "";
    }
    const char *text = anem_pack_text(s->pack, id);
    return text ? text : "";
}

static const char *text_of(const anem_script_t *s, uint16_t id)
{
    return anem_script_text(s, id);
}

static const char *str_of(const anem_script_t *s, anem_value_t v)
{
    return v.kind == ANEM_VAL_STR ? text_of(s, v.text) : "";
}

static int32_t num_of(anem_value_t v) { return v.kind == ANEM_VAL_NUM ? v.num : 0; }

static bool truthy(anem_value_t v)
{
    if (v.kind == ANEM_VAL_NUM) {
        return v.num != 0;
    }
    if (v.kind == ANEM_VAL_STR) {
        return v.text != 0;
    }
    return v.len != 0;
}

// ---------------------------------------------------------------- 变量

bool anem_var_id_by_name(const anem_pack_t *pack, const char *name, uint16_t *out)
{
    if (name == NULL) {
        return false;
    }
    for (uint16_t i = 0; i < ANEM_VAR_MAX; i++) {
        uint16_t text = 0;
        if (!anem_pack_var(pack, i, &text)) {
            break;
        }
        const char *cand = anem_pack_text(pack, text);
        if (cand != NULL && strcmp(cand, name) == 0) {
            if (out) {
                *out = i;
            }
            return true;
        }
    }
    return false;
}

const char *anem_var_name(const anem_pack_t *pack, uint16_t id)
{
    uint16_t text = 0;
    if (!anem_pack_var(pack, id, &text)) {
        return NULL;
    }
    return anem_pack_text(pack, text);
}

anem_value_t anem_script_var_get(const anem_script_t *s, uint16_t id)
{
    if (id >= s->st.var_count) {
        return v_num(0);
    }
    return s->st.vars[id];
}

bool anem_script_var_set_num(anem_script_t *s, uint16_t id, int32_t num)
{
    if (id >= s->st.var_count) {
        return false;
    }
    s->st.vars[id] = v_num(num);
    return true;
}

bool anem_script_var_set_text(anem_script_t *s, uint16_t id, uint16_t text)
{
    if (id >= s->st.var_count) {
        return false;
    }
    s->st.vars[id] = v_str(text);
    return true;
}

// 动态变量键：把 "base:N" 落到已注册的变量名上（z:201..450 / a:0..200 等）。
// 找不到返回 0xFFFF —— 调用方按“未定义 = 0”处理，不会静默写到别的变量。
static uint16_t var_lookup_dynamic(anem_script_t *s, const char *base, int32_t index)
{
    char name[32];
    snprintf(name, sizeof(name), "%s:%d", base, (int)index);
    uint16_t id = 0xFFFF;
    if (anem_var_id_by_name(s->pack, name, &id)) {
        return id;
    }
    return 0xFFFF;
}

static void var_assign(anem_script_t *s, uint16_t id, anem_value_t value)
{
    if (id < s->st.var_count) {
        s->st.vars[id] = value;
    }
}

static uint16_t var_id(anem_script_t *s, const char *name)
{
    uint16_t id = 0;
    return anem_var_id_by_name(s->pack, name, &id) ? id : 0xFFFF;
}

static int32_t var_num(anem_script_t *s, const char *name)
{
    const uint16_t id = var_id(s, name);
    if (id == 0xFFFF) {
        return 0;
    }
    return num_of(s->st.vars[id]);
}

static void var_set_num(anem_script_t *s, const char *name, int32_t value)
{
    const uint16_t id = var_id(s, name);
    if (id != 0xFFFF) {
        s->st.vars[id] = v_num(value);
    }
}

// ---------------------------------------------------------------- 表达式

static uint32_t expr_len(const uint8_t *p);

static uint32_t expr_len(const uint8_t *p)
{
    switch (rd8(p)) {
    case E_NUM:
        return 5;
    case E_STR:
    case E_VAR:
        return 3;
    case 9:  // E_VARDYN
        return 3 + expr_len(p + 3);
    case E_ARRAY: {
        const uint16_t n = rd16(p + 1);
        uint32_t len = 3;
        for (uint16_t i = 0; i < n; i++) {
            len += expr_len(p + len);
        }
        return len;
    }
    case E_UN:
        return 2 + expr_len(p + 2);
    case E_BIN: {
        const uint32_t a = expr_len(p + 2);
        return 2 + a + expr_len(p + 2 + a);
    }
    case E_METHOD: {
        const uint8_t argc = p[2];
        uint32_t len = 3 + expr_len(p + 3);
        for (uint8_t i = 0; i < argc; i++) {
            len += expr_len(p + len);
        }
        return len;
    }
    case E_CALL: {
        const uint8_t has_recv = p[3];
        const uint8_t argc = p[4];
        uint32_t len = 5;
        if (has_recv) {
            len += expr_len(p + len);
        }
        for (uint8_t i = 0; i < argc; i++) {
            len += expr_len(p + len);
        }
        return len;
    }
    default:
        return 1;
    }
}

static int value_cmp(const anem_script_t *s, anem_value_t a, anem_value_t b)
{
    if (a.kind == ANEM_VAL_STR || b.kind == ANEM_VAL_STR) {
        if (a.kind != ANEM_VAL_STR || b.kind != ANEM_VAL_STR) {
            return a.kind == ANEM_VAL_STR ? 1 : -1;
        }
        return strcmp(str_of(s, a), str_of(s, b));
    }
    const int32_t x = num_of(a), y = num_of(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static uint16_t intern_text(anem_script_t *s, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    for (uint32_t i = 0; i < anem_pack_text_count(s->pack); i++) {
        const char *cand = anem_pack_text(s->pack, (uint16_t)i);
        if (cand != NULL && strcmp(cand, text) == 0) {
            return (uint16_t)i;
        }
    }
    // 包里没有：存进运行期 scratch。绝不能返回 0（那会变成池里第 0 条字符串，
    // 例如把 "07" 变成某个人物名），这是 $$set_date 月/日解析失败的直接原因。
    const uint8_t slot = s->scratch_next % ANEM_SCRATCH_SLOTS;
    s->scratch_next = (uint8_t)(s->scratch_next + 1);
    snprintf(s->scratch[slot], ANEM_SCRATCH_LEN, "%s", text);
    return (uint16_t)(ANEM_SCRATCH_FLAG | slot);
}

static bool eval_expr(anem_script_t *s, uint32_t off, anem_value_t *out, int depth);

static bool eval_call(anem_script_t *s, const uint8_t *p, uint32_t off, anem_value_t *out,
                      int depth)
{
    const uint8_t *base = anem_pack_expr_base(s->pack);
    (void)off;
    const uint16_t name_id = rd16(p + 1);
    const uint8_t has_recv = p[3];
    const uint8_t argc = p[4];
    anem_value_t args[9];
    anem_value_t receiver;
    memset(&receiver, 0, sizeof(receiver));
    const uint8_t *q = p + 5;
    if (has_recv) {
        const uint32_t sub = (uint32_t)(q - base);
        if (!eval_expr(s, sub, &receiver, depth + 1)) {
            return false;
        }
        q += expr_len(q);
    }
    for (uint8_t i = 0; i < argc && i < 9; i++) {
        const uint32_t sub = (uint32_t)(q - base);
        if (!eval_expr(s, sub, &args[i], depth + 1)) {
            return false;
        }
        q += expr_len(q);
    }
    const char *name = anem_pack_text(s->pack, name_id);
    if (name == NULL) {
        set_error(s, "剧情调用名缺失");
        return false;
    }
    return anem_script_native(s, name, args, argc, has_recv ? &receiver : NULL, out);
}

static bool eval_expr(anem_script_t *s, uint32_t off, anem_value_t *out, int depth)
{
    if (depth > 12) {
        set_error(s, "表达式嵌套过深");
        return false;
    }
    const uint8_t *base = anem_pack_expr_base(s->pack);
    if (base == NULL || off >= anem_pack_expr_size(s->pack)) {
        set_error(s, "表达式越界");
        return false;
    }
    const uint8_t *p = base + off;
    switch (rd8(p)) {
    case E_NUM:
        *out = v_num((int32_t)rd32(p + 1));
        return true;
    case E_STR:
        *out = v_str(rd16(p + 1));
        return true;
    case E_VAR: {
        const uint16_t id = rd16(p + 1);
        *out = id < s->st.var_count ? s->st.vars[id] : v_num(0);
        return true;
    }
    case E_VARDYN: {  // 9: base 文本 id + 索引表达式
        const uint16_t base_text = rd16(p + 1);
        anem_value_t index;
        if (!eval_expr(s, off + 3, &index, depth + 1)) {
            return false;
        }
        const char *base = text_of(s, base_text);
        const uint16_t id = var_lookup_dynamic(s, base, num_of(index));
        *out = id != 0xFFFF ? s->st.vars[id] : v_num(0);
        return true;
    }
    case E_ARRAY: {
        const uint16_t n = rd16(p + 1);
        const uint16_t start = s->arena_used;
        if (s->arena_used + n > ANEM_ARRAY_POOL) {
            set_error(s, "表达式数组超出容量");
            return false;
        }
        const uint8_t *q = p + 3;
        for (uint16_t i = 0; i < n; i++) {
            const uint32_t sub = (uint32_t)(q - base);
            anem_value_t item;
            if (!eval_expr(s, sub, &item, depth + 1)) {
                s->arena_used = start;
                return false;
            }
            s->arena[s->arena_used++] = item;
            q += expr_len(q);
        }
        memset(out, 0, sizeof(*out));
        out->kind = ANEM_VAL_ARR;
        out->text = start;
        out->len = (uint8_t)n;
        return true;
    }
    case E_UN: {
        anem_value_t a;
        if (!eval_expr(s, off + 2, &a, depth + 1)) {
            return false;
        }
        switch (p[1]) {
        case 1:
            *out = v_num(-num_of(a));
            break;
        case 2:
            *out = v_num(num_of(a));
            break;
        case 3:
            *out = v_num(truthy(a) ? 0 : 1);
            break;
        default:
            *out = v_num(~num_of(a));
            break;
        }
        return true;
    }
    case E_BIN: {
        anem_value_t a, b;
        if (!eval_expr(s, off + 2, &a, depth + 1)) {
            return false;
        }
        if (!eval_expr(s, off + 2 + expr_len(p + 2), &b, depth + 1)) {
            return false;
        }
        switch (p[1]) {
        case 1:
            if (a.kind == ANEM_VAL_STR || b.kind == ANEM_VAL_STR) {
                char buf[192];
                snprintf(buf, sizeof(buf), "%s%s", str_of(s, a), str_of(s, b));
                *out = v_str(intern_text(s, buf));
                return true;
            }
            *out = v_num(num_of(a) + num_of(b));
            break;
        case 2:
            *out = v_num(num_of(a) - num_of(b));
            break;
        case 3:
            *out = v_num(num_of(a) * num_of(b));
            break;
        case 4: {
            const int32_t d = num_of(b);
            *out = v_num(d ? num_of(a) / d : 0);
            break;
        }
        case 5: {
            const int32_t d = num_of(b);
            *out = v_num(d ? num_of(a) % d : 0);
            break;
        }
        case 6:
            *out = v_num(value_cmp(s, a, b) == 0);
            break;
        case 7:
            *out = v_num(value_cmp(s, a, b) != 0);
            break;
        case 8:
            *out = v_num(value_cmp(s, a, b) < 0);
            break;
        case 9:
            *out = v_num(value_cmp(s, a, b) <= 0);
            break;
        case 10:
            *out = v_num(value_cmp(s, a, b) > 0);
            break;
        case 11:
            *out = v_num(value_cmp(s, a, b) >= 0);
            break;
        case 12:
            *out = v_num(num_of(a) & num_of(b));
            break;
        case 13:
            *out = v_num(num_of(a) | num_of(b));
            break;
        case 14:
            *out = v_num(num_of(a) ^ num_of(b));
            break;
        case 15:
            *out = v_num(num_of(a) << num_of(b));
            break;
        case 16:
            *out = v_num(num_of(a) >> num_of(b));
            break;
        case 17:
            *out = v_num(truthy(a) ? (truthy(b) ? 1 : 0) : 0);
            break;
        case 18:
            *out = v_num((truthy(a) || truthy(b)) ? 1 : 0);
            break;
        default:
            set_error(s, "未知运算符");
            return false;
        }
        return true;
    }
    case E_METHOD: {
        const uint8_t method = p[1];
        const uint8_t argc = p[2];
        anem_value_t recv;
        if (!eval_expr(s, off + 3, &recv, depth + 1)) {
            return false;
        }
        anem_value_t args[4];
        const uint8_t *q = p + 3 + expr_len(p + 3);
        for (uint8_t i = 0; i < argc && i < 4; i++) {
            const uint32_t sub = (uint32_t)(q - base);
            if (!eval_expr(s, sub, &args[i], depth + 1)) {
                return false;
            }
            q += expr_len(q);
        }
        char buf[192];
        switch (method) {
        case M_COUNT:
            *out = v_num(recv.kind == ANEM_VAL_STR ? (int32_t)strlen(str_of(s, recv))
                                                   : (recv.kind == ANEM_VAL_ARR ? recv.len : 0));
            return true;
        case M_LEFT: {
            const char *text = str_of(s, recv);
            int32_t n = argc ? num_of(args[0]) : 0;
            size_t len = strlen(text);
            if (n < 0) {
                len = 0;
            } else if ((size_t)n < len) {
                len = (size_t)n;
            }
            if (len >= sizeof(buf)) {
                len = sizeof(buf) - 1;
            }
            memcpy(buf, text, len);
            buf[len] = 0;
            *out = v_str(intern_text(s, buf));
            return true;
        }
        case M_RIGHT: {
            const char *text = str_of(s, recv);
            const int32_t n = argc ? num_of(args[0]) : 0;
            const size_t total = strlen(text);
            size_t take = n <= 0 ? 0 : (size_t)n;
            if (take > total) {
                take = total;
            }
            if (take >= sizeof(buf)) {
                take = sizeof(buf) - 1;
            }
            memcpy(buf, text + total - take, take);
            buf[take] = 0;
            *out = v_str(intern_text(s, buf));
            return true;
        }
        case M_MID: {
            const char *text = str_of(s, recv);
            const int32_t start = argc > 0 ? num_of(args[0]) : 0;
            const int32_t count = argc > 1 ? num_of(args[1]) : 0;
            const size_t total = strlen(text);
            size_t from = start >= 0 ? (size_t)start : 0;
            if (from > total) {
                from = total;
            }
            size_t take = count > 0 ? (size_t)count : 0;
            if (take > total - from) {
                take = total - from;
            }
            if (take >= sizeof(buf)) {
                take = sizeof(buf) - 1;
            }
            memcpy(buf, text + from, take);
            buf[take] = 0;
            *out = v_str(intern_text(s, buf));
            return true;
        }
        case M_TONUM:
            *out = v_num((int32_t)strtol(str_of(s, recv), NULL, 10));
            return true;
        case M_SEARCH: {
            const char *needle = argc ? str_of(s, args[0]) : "";
            const char *text = str_of(s, recv);
            const char *pos = strstr(text, needle);
            *out = v_num(pos ? (int32_t)(pos - text) : -1);
            return true;
        }
        case M_GET_SIZE:
            if (recv.kind == ANEM_VAL_ARR) {
                *out = v_num(recv.len);
            } else if (recv.kind == ANEM_VAL_STR) {
                *out = v_num((int32_t)strlen(str_of(s, recv)));
            } else {
                *out = v_num(0);
            }
            return true;
        case M_GET: {
            const int32_t idx = argc ? num_of(args[0]) : 0;
            if (recv.kind == ANEM_VAL_ARR) {
                *out = (idx >= 0 && idx < recv.len) ? s->arena[recv.text + idx] : v_num(0);
            } else if (recv.kind == ANEM_VAL_STR) {
                const char *text = str_of(s, recv);
                const int32_t n = (int32_t)strlen(text);
                buf[0] = (idx >= 0 && idx < n) ? text[idx] : 0;
                buf[1] = 0;
                *out = v_str(intern_text(s, buf));
            } else {
                *out = v_num(0);
            }
            return true;
        }
        case M_GET_FILE_NAME:
            *out = (recv.kind == ANEM_VAL_STR) ? recv : v_str(s->st.regular_bg);
            return true;
        case M_CHECK:
            *out = v_num(0);
            return true;
        default:
            set_error(s, "未知字符串/数组方法");
            return false;
        }
    }
    case E_CALL:
        return eval_call(s, p, off, out, depth);
    default:
        set_error(s, "未知表达式字节码");
        return false;
    }
}

// ---------------------------------------------------------------- helpers

static uint16_t current_scene_id(const anem_script_t *s) { return s->st.chapter; }

static bool find_helper(anem_script_t *s, const char *name, uint16_t scene,
                        anem_helper_t *out)
{
    bool have_global = false;
    anem_helper_t global_row;
    for (uint32_t i = 0;; i++) {
        anem_helper_t row;
        if (!anem_pack_helper(s->pack, i, &row)) {
            break;
        }
        const char *cand = anem_pack_text(s->pack, row.name);
        if (cand == NULL || strcmp(cand, name) != 0) {
            continue;
        }
        if (row.scope == scene) {
            *out = row;
            return true;
        }
        if (row.scope == ANEM_NONE16 && !have_global) {
            global_row = row;
            have_global = true;
        }
    }
    if (have_global) {
        *out = global_row;
    }
    return have_global;
}

static bool run_helper(anem_script_t *s, const anem_helper_t *helper, anem_value_t *args,
                       uint8_t argc, anem_value_t *out);

// ---------------------------------------------------------------- natives

static int32_t time_suffix_value(anem_script_t *s)
{
    return var_num(s, "$time_control");
}

static const char *time_suffix(anem_script_t *s)
{
    const int32_t value = time_suffix_value(s);
    switch (value) {
    case 0: return "_01";
    case 1: return "_02";
    case 2: return "_03";
    case 3: return "_04";
    case 4: return "_80";
    case 5: return "_04";
    case 6: return "_05";
    default: return "";
    }
}

static bool image_exists(anem_script_t *s, const char *name)
{
    return anem_pack_exists_image(s->pack, name);
}

bool anem_script_native(anem_script_t *s, const char *name, anem_value_t *args, uint8_t argc,
                        const anem_value_t *receiver, anem_value_t *out)
{
    (void)receiver;
    *out = v_num(0);
    if (name == NULL) {
        set_error(s, "剧情指令名为空");
        return false;
    }
    // 脚本函数优先（源 native() 在少数特判之后先查 helpers）。
    anem_helper_t helper;
    if (find_helper(s, name, current_scene_id(s), &helper)) {
        return run_helper(s, &helper, args, argc, out);
    }
    if (strcmp(name, "$$set_time_control") == 0) {
        var_set_num(s, "$time_control", argc ? num_of(args[0]) : 0);
        return true;
    }
    if (strcmp(name, "$$init_time_control") == 0) {
        var_set_num(s, "$time_control", 0);
        return true;
    }
    if (strcmp(name, "$$get_time_control_suffix") == 0) {
        *out = v_str(intern_text(s, time_suffix(s)));
        return true;
    }
    if (strcmp(name, "$$get_bg_time_control_name") == 0) {
        char buf[128];
        const char *base = argc ? str_of(s, args[0]) : "";
        snprintf(buf, sizeof(buf), "%s%s", base, time_suffix(s));
        if (image_exists(s, buf)) {
            *out = v_str(intern_text(s, buf));
        } else {
            *out = v_str(intern_text(s, base));
        }
        return true;
    }
    if (strcmp(name, "$$chop_bg_blur_name") == 0) {
        const char *base = argc ? str_of(s, args[0]) : "";
        const char *prefix = "b_";
        const size_t plen = strlen(prefix);
        *out = v_str(intern_text(s, strncmp(base, prefix, plen) == 0 ? base + plen : base));
        return true;
    }
    if (strcmp(name, "$$set_date") == 0) {
        // 源 helpers 的 $$set_date 在这里之前不会命中（helper 优先），保留直接语义兜底。
        const char *date = argc ? str_of(s, args[0]) : "";
        if (strlen(date) >= 4) {
            var_set_num(s, "a:1", (date[0] - '0') * 10 + (date[1] - '0'));
            var_set_num(s, "a:2", (date[2] - '0') * 10 + (date[3] - '0'));
        }
        return true;
    }
    if (strcmp(name, "$$exists_image") == 0 || strcmp(name, "$$exists_g00") == 0) {
        *out = v_num(argc && image_exists(s, str_of(s, args[0])) ? 1 : 0);
        return true;
    }
    if (strcmp(name, "$$bold") == 0) {
        *out = argc ? args[0] : v_num(0);
        return true;
    }
    if (strcmp(name, "$$set_bs_dress_type") == 0) {
        if (argc >= 2) {
            char actor[8];
            snprintf(actor, sizeof(actor), "%s", str_of(s, args[0]));
            for (char *p = actor; *p; p++) {
                *p = (char)tolower((unsigned char)*p);
            }
            for (uint16_t i = 0; i < ANEM_ACTOR_MAX; i++) {
                anem_actor_t row;
                if (!anem_pack_actor(s->pack, i, &row)) {
                    break;
                }
                const char *row_name = anem_pack_text(s->pack, row.name);
                if (row_name != NULL && strcmp(row_name, actor) == 0) {
                    s->st.dress[i] = (uint8_t)num_of(args[1]);
                    break;
                }
            }
            // 立绘按新 dress 重新解析（源 refreshPortraits）
            refresh_bodies(s);
        }
        return true;
    }
    if (strcmp(name, "$$set_bs_overlay_face_type") == 0) {
        /* 源仅记录 overlay 变量，不改变可见立绘；这里同样只记变量。 */
        return true;
    }
    if (strcmp(name, "$$get_bs_obj_index") == 0) {
        *out = v_num(21);
        return true;
    }
    if (strcmp(name, "$$init_bbs_data") == 0) {
        s->board_count = 0;
        return true;
    }
    if (strcmp(name, "$$set_bbs_request_data") == 0) {
        // 源实现只在 preview 与 scene 两个名字都能在 catalog 里找到时才入板
        // （engine-vm.js 的 byName 过滤）。082/085/086/081 等场景在手表版
        // catalog 里不存在，源会静默丢弃 —— 这里必须同样丢弃，否则板上会多出
        // 永远进不去的委托。
        bool resolvable = false;
        if (argc >= 9) {
            const char *preview = str_of(s, args[5]);
            const char *scene = str_of(s, args[7]);
            resolvable = anem_pack_scene_by_source(s->pack, preview) != ANEM_NONE16 &&
                         anem_pack_scene_by_source(s->pack, scene) != ANEM_NONE16;
        }
        if (s->board_count < ANEM_BOARD_MAX && argc >= 9 && resolvable) {
            anem_board_item_t *item = &s->board[s->board_count++];
            memset(item, 0, sizeof(*item));
            item->button = (uint16_t)intern_text(s, str_of(s, args[0]));
            item->key = (uint16_t)intern_text(s, str_of(s, args[1]));
            item->position = (int16_t)num_of(args[2]);
            item->character = (int16_t)num_of(args[3]);
            item->time = num_of(args[4]);
            item->preview = (uint16_t)intern_text(s, str_of(s, args[5]));
            item->entry_task = (uint16_t)num_of(args[6]);
            item->scene = (uint16_t)intern_text(s, str_of(s, args[7]));
            item->entry_letter = (uint16_t)num_of(args[8]);
        }
        return true;
    }
    if (strcmp(name, "$$check_character_bbs_disp") == 0) {
        *out = v_num(argc ? num_of(args[0]) : 0);
        return true;
    }
    if (strcmp(name, "$$get_record_flag") == 0) {
        const int32_t index = argc ? num_of(args[0]) : 0;
        char key[24];
        snprintf(key, sizeof(key), "z:%d", (int)(201 + index));
        *out = v_num(index < 0 || index >= 250 ? 0 : var_num(s, key));
        return true;
    }
    if (strcmp(name, "$$record_flag_on") == 0 || strcmp(name, "$$record_flag_off") == 0) {
        const int32_t index = argc ? num_of(args[0]) : 0;
        if (index >= 0 && index < 250) {
            char key[24];
            snprintf(key, sizeof(key), "z:%d", (int)(201 + index));
            var_set_num(s, key, strcmp(name, "$$record_flag_on") == 0 ? 1 : 0);
        }
        return true;
    }
    if (strcmp(name, "$$check_route") == 0) {
        *out = v_num(s->route_result);
        return true;
    }
    if (strcmp(name, "$$route_clear") == 0) {
        s->route_result = 0;
        return true;
    }
    if (strcmp(name, "global.get_scene_name") == 0) {
        anem_chap_t chap;
        if (anem_pack_chapter_for(s->pack, s->st.chapter, &chap)) {
            *out = v_str(chap.source);
        }
        return true;
    }
    if (strcmp(name, "global.get_line_no") == 0 || strcmp(name, "$$get_toast_size") == 0 ||
        strcmp(name, "$$get_wipe_type_ready") == 0 || strcmp(name, "$$get_wipe_wait_ready") == 0 ||
        strcmp(name, "$$get_disp_cg_msg_close") == 0 ||
        strcmp(name, "$$get_disp_cg_wipe_no") == 0 ||
        strcmp(name, "global.system.check_debug_flag") == 0 ||
        strcmp(name, "$$check_debug_mode_enable") == 0 || strcmp(name, "global.bgm.check") == 0 ||
        strcmp(name, "global.syscom.check_joypad_mode") == 0 ||
        strcmp(name, "global.get_mwnd") == 0 || strcmp(name, "$$input_update") == 0 ||
        strcmp(name, "$$get_record_total") == 0 || strcmp(name, "$$get_hhp_total_play_count") == 0 ||
        strcmp(name, "$$get_hhp_play_result") == 0 || strcmp(name, "$$get_hhp_play_count") == 0 ||
        strcmp(name, "$$has_hhp_skill") == 0 || strcmp(name, "$$get_urace_grand_entry_flag") == 0 ||
        strcmp(name, "$$init_urace_user_data") == 0 || strcmp(name, "$$init_urace_am_data") == 0 ||
        strcmp(name, "$$init_urace_pm_data") == 0 || strcmp(name, "$$init_urace_day_data") == 0 ||
        strcmp(name, "$$set_urace_medal_flag") == 0 ||
        strcmp(name, "$$set_sp_flag") == 0 || strcmp(name, "$$change_title_menu_level") == 0 ||
        strcmp(name, "$$reverse_flag") == 0 || strcmp(name, "$$record_flag_off") == 0) {
        *out = v_num(0);
        return true;
    }
    if (strcmp(name, "$$get_hhp_play_level") == 0) {
        *out = v_num(1);
        return true;
    }
    if (strcmp(name, "global.farcall") == 0) {
        *out = v_num(s->last_call_result);
        return true;
    }
    if (strcmp(name, "global.get_file_name") == 0) {
        *out = args[0].kind == ANEM_VAL_STR ? args[0] : v_str(s->st.regular_bg);
        return true;
    }
    if (strcmp(name, "global.math.rand") == 0 || strcmp(name, "global.random") == 0) {
        const int32_t lo = argc > 0 ? num_of(args[0]) : 0;
        const int32_t hi = argc > 1 ? num_of(args[1]) : 32767;
        s->rng = (int32_t)(((uint32_t)s->rng * 1664525u + 1013904223u) & 0xFFFFFFFFu);
        const int32_t span = (hi - lo + 1) > 1 ? (hi - lo + 1) : 1;
        *out = v_num(lo + (s->rng % span));
        return true;
    }
    if (strcmp(name, "global.math.abs") == 0) {
        *out = v_num(argc ? abs(num_of(args[0])) : 0);
        return true;
    }
    if (strcmp(name, "global.math.tostr") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)(argc ? num_of(args[0]) : 0));
        *out = v_str(intern_text(s, buf));
        return true;
    }
    if (strcmp(name, "global.math.tostr_zero") == 0) {
        char buf[32];
        const int32_t width = argc > 1 ? num_of(args[1]) : 1;
        snprintf(buf, sizeof(buf), "%0*d", (int)(width < 1 ? 1 : (width > 12 ? 12 : width)),
                 (int)(argc ? num_of(args[0]) : 0));
        *out = v_str(intern_text(s, buf));
        return true;
    }
    if (strcmp(name, "global.math.linear") == 0) {
        *out = v_num(argc >= 5 ? num_of(args[0]) + (num_of(args[1]) - num_of(args[0])) *
                                                 num_of(args[4]) / (num_of(args[3]) ? num_of(args[3]) : 1)
                               : (argc ? num_of(args[0]) : 0));
        return true;
    }
    if (strcmp(name, "$$get_time_control") == 0) {
        *out = v_num(time_suffix_value(s));
        return true;
    }
    char message[80];
    snprintf(message, sizeof(message), "尚未支持的剧情指令：%.40s", name);
    set_error(s, message);
    return false;
}

// ---------------------------------------------------------------- 状态 / 姿势

static void pose_key_impl(anem_script_t *s, const char *raw, char *out, size_t cap);

void anem_script__pose_key(anem_script_t *s, const char *raw, char *out, size_t cap)
{
    pose_key_impl(s, raw, out, cap);
}

static int actor_slot_of_name(anem_script_t *s, const char *name, char *actor_out)
{
    const char *scan = name;
    if (strncmp(scan, "bs1_", 4) == 0 || strncmp(scan, "bs2_", 4) == 0 ||
        strncmp(scan, "bs3_", 4) == 0) {
        scan += 4;
    }
    char actor[3] = { (char)tolower((unsigned char)scan[0]),
                      (char)tolower((unsigned char)scan[1]), 0 };
    if (actor_out) {
        memcpy(actor_out, actor, 3);
    }
    for (uint16_t i = 0; i < ANEM_ACTOR_MAX; i++) {
        anem_actor_t row;
        if (!anem_pack_actor(s->pack, i, &row)) {
            break;
        }
        const char *row_name = anem_pack_text(s->pack, row.name);
        if (row_name != NULL && strcmp(row_name, actor) == 0) {
            return i;
        }
    }
    return -1;
}

static void pose_key_impl(anem_script_t *s, const char *raw, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = 0;
    if (raw == NULL || raw[0] == 0) {
        return;
    }
    char name[64];
    size_t len = 0;
    for (; raw[len] != 0 && len + 1 < sizeof(name); len++) {
        name[len] = (char)tolower((unsigned char)raw[len]);
    }
    name[len] = 0;
    char *act = strstr(name, "_act_");
    if (act != NULL) {
        *act = 0;
    }
    char actor[3] = { 0 };
    const int slot = actor_slot_of_name(s, name, actor);
    const int fallback = (int)anem_pack_actor_default_dress(s->pack, actor);
    if (strcmp(name, actor) == 0) {
        if (slot >= 0 && s->st.pose[slot] != 0) {
            snprintf(name, sizeof(name), "%s", text_of(s, s->st.pose[slot]));
            char *act2 = strstr(name, "_act_");
            if (act2 != NULL) {
                *act2 = 0;
            }
        } else {
            snprintf(name, sizeof(name), "%s11_01", actor);
        }
    }
    const size_t nlen = strlen(name);
    if (nlen == 9 && (strncmp(name, "bs1_", 4) == 0 || strncmp(name, "bs2_", 4) == 0 ||
                      strncmp(name, "bs3_", 4) == 0) && isdigit((unsigned char)name[6]) &&
        isdigit((unsigned char)name[7]) && !isdigit((unsigned char)name[5])) {
        snprintf(name + nlen, sizeof(name) - nlen, "_01");
    }
    if (slot >= 0 && strcmp(name, actor) != 0) {
        const uint16_t text = intern_text(s, name);
        if (text != 0) {
            s->st.pose[slot] = text;
        }
    }
    anem_pose_view_t view;
    int dress = fallback;
    if (slot >= 0 && s->st.dress[slot] != 0) {
        dress = s->st.dress[slot];
    }
    if (!anem_pack_pose(s->pack, name, dress, fallback, &view)) {
        return;
    }
    snprintf(out, cap, "%s", name);
}

static void refresh_bodies(anem_script_t *s)
{
    for (uint8_t i = 0; i < s->st.body_count; i++) {
        const char *pose = text_of(s, s->st.bodies[i].name);
        char resolved[64];
        pose_key_impl(s, pose, resolved, sizeof(resolved));
        if (resolved[0] != 0) {
            s->st.bodies[i].name = intern_text(s, resolved);
        }
    }
}

// ---------------------------------------------------------------- helper 执行

static void save_locals(anem_script_t *s)
{
    if (s->local_depth >= ANEM_HELPER_DEPTH) {
        return;
    }
    for (uint8_t i = 0; i < s->local_count; i++) {
        s->local_saved[s->local_depth][i] = s->st.vars[s->local_ids[i]];
    }
    s->local_depth++;
}

static void restore_locals(anem_script_t *s)
{
    if (s->local_depth == 0) {
        return;
    }
    s->local_depth--;
    for (uint8_t i = 0; i < s->local_count; i++) {
        s->st.vars[s->local_ids[i]] = s->local_saved[s->local_depth][i];
    }
}

static bool run_helper(anem_script_t *s, const anem_helper_t *helper, anem_value_t *args,
                       uint8_t argc, anem_value_t *out)
{
    if (s->helper_depth >= ANEM_HELPER_DEPTH) {
        set_error(s, "剧情函数嵌套过深");
        return false;
    }
    s->helper_depth++;
    save_locals(s);
    const uint16_t arena_base = s->arena_used;  // 嵌套求值只往上长，退出时归还
    // 绑定参数：<chapter>:<name> 的形参来自 HELPER.params（0x1F 分隔）
    const char *params = text_of(s, helper->params);
    {
        uint8_t index = 0;
        const char *p = params;
        while (*p != 0 && index < argc) {
            const char *end = strchr(p, 0x1F);
            char name[40];
            const size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len < sizeof(name)) {
                memcpy(name, p, len);
                name[len] = 0;
                char local[64];
                snprintf(local, sizeof(local), "cur_call.%.40s", name);
                const uint16_t id = var_id(s, local);
                if (id != 0xFFFF) {
                    s->st.vars[id] = args[index];
                }
            }
            index++;
            if (end == NULL) {
                break;
            }
            p = end + 1;
        }
    }
    bool result_ok = true;
    bool has_result = false;
    anem_value_t result = v_num(0);
    uint32_t pc = 0;
    for (int guard = 0; guard < 4096; guard++) {
        if (pc >= helper->micro_count) {
            break;  // 源实现里 pc 走到函数体末尾等价于 return 0
        }
        anem_micro_t op;
        if (!anem_pack_micro(s->pack, (uint16_t)(helper->micro_off + pc), &op)) {
            set_error(s, "剧情函数越界");
            result_ok = false;
            break;
        }
        if (op.kind == 1) {  // set
            const uint16_t var = op.a;
            if (var >= s->st.var_count) {
                set_error(s, "剧情函数变量越界");
                result_ok = false;
                break;
            }
            anem_set_t set;
            if (!anem_pack_set(s->pack, (uint16_t)op.b, &set)) {
                set_error(s, "剧情函数赋值表越界");
                result_ok = false;
                break;
            }
            anem_value_t value;
            if (!eval_expr(s, set.expr, &value, 0)) {
                result_ok = false;
                break;
            }
            var_assign(s, var, value);
        } else if (op.kind == 2) {  // native
            anem_value_t value;
            uint32_t off = 0;
            if (!anem_pack_call(s->pack, (uint16_t)op.b, &off) ||
                !eval_expr(s, off, &value, 0)) {
                result_ok = false;
                break;
            }
        } else if (op.kind == 3) {  // goto
            pc = op.b;
            continue;
        } else if (op.kind == 4 || op.kind == 5) {  // ifFalse / ifTrue
            uint32_t off = 0;
            anem_value_t cond;
            if (!anem_pack_cond(s->pack, op.a, &off) || !eval_expr(s, off, &cond, 0)) {
                result_ok = false;
                break;
            }
            const bool want = op.kind == 4 ? !truthy(cond) : truthy(cond);
            if (want) {
                pc = op.b;
                continue;
            }
        } else if (op.kind == 7) {  // set（动态键）
            anem_value_t index;
            if (!eval_expr(s, op.b, &index, 0)) {
                result_ok = false;
                break;
            }
            const uint16_t id = var_lookup_dynamic(s, text_of(s, op.a), num_of(index));
            anem_value_t value;
            if (!eval_expr(s, op.c, &value, 0)) {
                result_ok = false;
                break;
            }
            if (id != 0xFFFF) {
                s->st.vars[id] = value;
            }
        } else if (op.kind == 6) {  // return
            // 契约：带值 return 的“有值”标记在 flags bit0（打包器同样在 c 置 1，
            // 兼容旧包）。绝不能只看 flags —— 旧包里标记在 c 上，只看 flags 会把
            // 所有带返回值的脚本函数当成 return 0（见 tests/test_anemoi_helper_return）。
            bool has_value;
#ifdef ANEM_LEGACY_HELPER_RETURN
            has_value = (op.flags & 1u) != 0;  // 旧实现：故意忽略 c，用于回归负例
#else
            has_value = ((op.flags & 1u) != 0) || ((op.c & 1u) != 0);
#endif
            if (has_value) {
                anem_value_t value;
                if (!eval_expr(s, op.b, &value, 0)) {
                    result_ok = false;
                    break;
                }
                result = value;
            }
            has_result = true;
            break;
        } else {
            set_error(s, "剧情函数指令未知");
            result_ok = false;
            break;
        }
        pc++;
    }
    s->arena_used = arena_base;
    restore_locals(s);
    s->helper_depth--;
    if (!result_ok) {
        return false;
    }
    if (has_result) {
        *out = result;
    }
    return true;
}

// ---------------------------------------------------------------- 跳转

static const char *source_name_of(const anem_script_t *s, uint16_t scene)
{
    anem_chap_t chap;
    if (!anem_pack_chapter_for(s->pack, scene, &chap)) {
        return "";
    }
    const char *name = anem_pack_text(s->pack, chap.source);
    return name ? name : "";
}

static bool scene_range(const anem_script_t *s, uint16_t scene, uint32_t *first,
                        uint32_t *count)
{
    return anem_pack_scene_range(s->pack, scene, first, count);
}

static void jump_direct(anem_script_t *s, uint16_t scene, uint16_t entry)
{
    uint32_t first = 0, count = 0;
    if (!scene_range(s, scene, &first, &count)) {
        anem_chap_t chap;
        if (anem_pack_chapter_for(s->pack, scene, &chap)) {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s",
                     text_of(s, chap.name));
        } else {
            s->boundary_chapter[0] = 0;
        }
        s->st.chapter = scene;
        s->st.ended = true;
        set_error(s, "本卷外的章节");
        return;
    }
    uint32_t start = anem_pack_entry_op(s->pack, scene, entry);
    if (start == ANEM_NONE32 || start < first || start >= first + count) {
        // entry 落在分卷裁切之外：这是本卷边界，不是“回到场景开头”。
        anem_chap_t chap;
        if (anem_pack_chapter_for(s->pack, scene, &chap)) {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s",
                     text_of(s, chap.name));
        } else {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s", source_name_of(s, scene));
        }
        set_error(s, "本卷外的章节入口");
        return;
    }
    s->st.chapter = scene;
    s->st.cursor = start;
    s->st.ended = false;
    s->st.root_chapter = scene;
}

static bool jump_with_frame(anem_script_t *s, uint16_t scene, uint16_t entry, bool call)
{
    uint32_t first = 0, count = 0;
    if (!scene_range(s, scene, &first, &count)) {
        anem_chap_t chap;
        if (anem_pack_chapter_for(s->pack, scene, &chap)) {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s",
                     text_of(s, chap.name));
        } else {
            s->boundary_chapter[0] = 0;
        }
        set_error(s, "本卷外的章节");
        return false;
    }
    if (call && s->call_count < ANEM_STACK_FRAMES) {
        anem_call_frame_t *frame = &s->calls[s->call_count++];
        memset(frame, 0, sizeof(*frame));
        frame->scene = s->st.chapter;
        frame->cursor = s->st.cursor + 1;
        frame->board_stage = 0;
    }
    uint32_t start = anem_pack_entry_op(s->pack, scene, entry);
    if (start == ANEM_NONE32 || start < first || start >= first + count) {
        // entry 落在分卷裁切之外：这是本卷边界，不是“回到场景开头”。
        anem_chap_t chap;
        if (anem_pack_chapter_for(s->pack, scene, &chap)) {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s",
                     text_of(s, chap.name));
        } else {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s", source_name_of(s, scene));
        }
        set_error(s, "本卷外的章节入口");
        return false;
    }
    s->st.chapter = scene;
    s->st.cursor = start;
    s->st.ended = false;
    anem_chap_t chap;
    if (anem_pack_chapter_for(s->pack, scene, &chap)) {
        s->st.root_chapter = scene;
    }
    return true;
}

static uint16_t board_scene_id(anem_script_t *s, uint16_t name_text)
{
    const char *name = text_of(s, name_text);
    return anem_pack_scene_by_source(s->pack, name);
}

static void script_return(anem_script_t *s)
{
    if (s->call_count == 0) {
        s->st.ended = true;
        return;
    }
    anem_call_frame_t frame = s->calls[--s->call_count];
    s->st.chapter = frame.scene;
    s->st.cursor = frame.cursor;
    s->st.ended = false;
    if (frame.board_stage == 1) {
        anem_call_frame_t *next = NULL;
        if (s->call_count < ANEM_STACK_FRAMES) {
            next = &s->calls[s->call_count++];
            memset(next, 0, sizeof(*next));
            next->scene = s->st.chapter;
            next->cursor = s->st.cursor;
            next->board_stage = 2;
            next->board = frame.board;
        }
        if (next == NULL) {
            set_error(s, "委托栈溢出");
            return;
        }
        const uint16_t scene = board_scene_id(s, frame.board.preview);
        uint32_t first = 0, count = 0;
        if (scene == ANEM_NONE16 || !scene_range(s, scene, &first, &count)) {
            snprintf(s->boundary_chapter, sizeof(s->boundary_chapter), "%s",
                     text_of(s, frame.board.key));
            set_error(s, "委托任务章节不在本卷内");
            return;
        }
        uint32_t start = anem_pack_entry_op(s->pack, scene, frame.board.entry_task);
        if (start == ANEM_NONE32 || start < first || start >= first + count) {
            set_error(s, "委托任务入口不在本卷内");
            return;
        }
        s->st.chapter = scene;
        s->st.cursor = start;
        return;
    }
    if (frame.board_stage == 2) {
        const uint16_t a4 = var_id(s, "a:4");
        const uint16_t a3 = var_id(s, "a:3");
        const uint16_t a5 = var_id(s, "a:5");
        if (a4 != 0xFFFF) {
            var_assign(s, a4, v_num(frame.board.time));
        }
        if (a3 != 0xFFFF && num_of(s->st.vars[a3]) == 1 && a5 != 0xFFFF) {
            var_assign(s, a5, v_num(frame.board.character));
        }
        s->last_call_result = 0;
        s->board_count = 0;
    }
}

bool anem_script_route_check(anem_script_t *s)
{
    const int32_t a10 = var_num(s, "a:10"), a11 = var_num(s, "a:11");
    const int32_t a12 = var_num(s, "a:12"), a13 = var_num(s, "a:13");
    const int32_t a14 = var_num(s, "a:14"), a2 = var_num(s, "a:2");
    int32_t chosen = 0;
    if (a10 >= 9) {
        if (s->route_prelude == 0) {
            s->route_prelude = 1;
            const uint16_t scene = anem_pack_scene_by_source(s->pack, "020_スピカ0000_4");
            if (scene == ANEM_NONE16 || !jump_with_frame(s, scene, 0, true)) {
                return false;
            }
            return true;
        }
        if (var_num(s, "a:134") == 0) {
            chosen = 100;
        }
    } else if (a11 >= 6 && a2 >= 24) {
        chosen = 150;
    } else if (a12 >= 8) {
        if (s->route_prelude == 0) {
            s->route_prelude = 2;
            const uint16_t scene = anem_pack_scene_by_source(s->pack, "040_淡雪0000");
            if (scene == ANEM_NONE16 || !jump_with_frame(s, scene, 0, true)) {
                return false;
            }
            return true;
        }
        chosen = 200;
    } else if (a13 >= 7) {
        chosen = 250;
    } else if (a14 >= 6) {
        chosen = 300;
    }
    s->route_prelude = 0;
    s->route_result = chosen ? 1 : 0;
    if (chosen) {
        var_set_num(s, "$jump_label_no", chosen);
    }
    return false;
}

// ---------------------------------------------------------------- 指令循环

static void enter_portrait(anem_script_t *s, const anem_op_t *op)
{
    const uint16_t count = (uint16_t)(op->b & 0xFF);
    const uint8_t mode = op->flags;
    if (mode == ANEM_POSE_REPLACE) {
        s->st.body_count = 0;
    }
    for (uint16_t i = 0; i < count; i++) {
        uint32_t off = 0;
        if (!anem_pack_namelist(s->pack, (uint16_t)(op->a + i), &off)) {
            continue;
        }
        anem_value_t value;
        s->arena_used = 0;
        if (!eval_expr(s, off, &value, 0)) {
            return;
        }
        const char *raw = value.kind == ANEM_VAL_STR ? str_of(s, value) : "";
        char name[64];
        snprintf(name, sizeof(name), "%s", raw);
        for (char *p = name; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        if (name[0] == 0 || strcmp(name, "none") == 0 || strcmp(name, "no_bs") == 0) {
            continue;
        }
        char actor[3] = { 0 };
        const int slot = actor_slot_of_name(s, name, actor);
        int at = -1;
        for (uint8_t k = 0; k < s->st.body_count; k++) {
            char other[3] = { 0 };
            actor_slot_of_name(s, text_of(s, s->st.bodies[k].name), other);
            if (strcmp(other, actor) == 0) {
                at = k;
                break;
            }
        }
        if (mode == ANEM_POSE_DELETE) {
            if (at >= 0) {
                for (uint8_t k = (uint8_t)at; k + 1 < s->st.body_count; k++) {
                    s->st.bodies[k] = s->st.bodies[k + 1];
                }
                s->st.body_count--;
            }
            continue;
        }
        if (mode == ANEM_POSE_CHANGE && at < 0) {
            continue;
        }
        char resolved[64];
        pose_key_impl(s, name, resolved, sizeof(resolved));
        if (resolved[0] == 0) {
            continue;
        }
        anem_body_slot_t slot_value = { .name = intern_text(s, resolved), .valid = true };
        if (slot >= 0 && s->st.dress[slot] != 0) {
            slot_value.dress = s->st.dress[slot];
        }
        if (at >= 0) {
            s->st.bodies[at] = slot_value;
        } else if (s->st.body_count < ANEM_BODY_MAX) {
            s->st.bodies[s->st.body_count++] = slot_value;
        }
        if (slot >= 0) {
            s->st.changed |= (1u << slot);
        }
    }
    if (s->st.body_count > ANEM_BODY_MAX) {
        s->st.body_count = ANEM_BODY_MAX;
    }
}

static void board_label(anem_script_t *s, const anem_board_item_t *item, char *out, size_t cap,
                        uint16_t *detail)
{
    *detail = 0;
    const char *applicant = NULL;
    const char *body = NULL;
    for (uint32_t i = 0;; i++) {
        anem_req_t req;
        if (!anem_pack_request(s->pack, i, &req)) {
            break;
        }
        if (req.key == item->key) {
            applicant = text_of(s, req.applicant);
            body = text_of(s, req.body);
            *detail = req.body;
            break;
        }
    }
    const char *fallback = text_of(s, item->scene);
    const char *day = anem_script_day_label(s);
    if (applicant == NULL) {
        applicant = fallback;
    }
    if (day != NULL && day[0] != 0) {
        snprintf(out, cap, "%s · %s", applicant, day);
    } else {
        snprintf(out, cap, "%s", applicant);
    }
    (void)body;
}

static bool build_choices(anem_script_t *s, const anem_op_t *op)
{
    anem_choice_t choice;
    s->arena_used = 0;
    if (!anem_pack_choice(s->pack, op->a, &choice)) {
        set_error(s, "选项表越界");
        return false;
    }
    anem_value_t labels, enabled, target;
    if (!eval_expr(s, choice.labels, &labels, 0) || !eval_expr(s, choice.enabled, &enabled, 0) ||
        !eval_expr(s, choice.target, &target, 0)) {
        return false;
    }
    s->choice_count = 0;
    const uint8_t count = labels.kind == ANEM_VAL_ARR ? labels.len : 0;
    for (uint8_t i = 0; i < count && s->choice_count < ANEM_CHOICE_MAX; i++) {
        bool usable = true;
        if (enabled.kind == ANEM_VAL_ARR && i < enabled.len) {
            usable = truthy(s->arena[enabled.text + i]);
        } else if (enabled.kind != ANEM_VAL_ARR) {
            usable = truthy(enabled);
        }
        if (!usable) {
            continue;
        }
        anem_choice_item_t *item = &s->choice[s->choice_count++];
        memset(item, 0, sizeof(*item));
        const anem_value_t label = s->arena[labels.text + i];
        item->label = (uint16_t)(label.kind == ANEM_VAL_STR ? label.text : 0);
        item->enabled = 1;
        item->value = i;
        item->board = false;
    }
    // target 的赋值目标：源 assign(item.target, item.value)
    s->choice_target_expr = choice.target;
    return s->choice_count > 0;
}

static bool choice_assign_target(anem_script_t *s, int value)
{
    const uint8_t *base = anem_pack_expr_base(s->pack);
    if (base == NULL || s->choice_target_expr >= anem_pack_expr_size(s->pack)) {
        return true;
    }
    const uint8_t *p = base + s->choice_target_expr;
    if (rd8(p) != E_VAR) {
        return true;
    }
    const uint16_t id = rd16(p + 1);
    var_assign(s, id, v_num(value));
    return true;
}

anem_frame_t anem_script_read(anem_script_t *s)
{
    anem_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    s->failed = false;
    s->notice[0] = 0;
    for (int guard = 0; guard < 20000; guard++) {
        if (s->st.ended) {
            frame.kind = ANEM_FRAME_END;
            return frame;
        }
        uint32_t first = 0, count = 0;
        if (!scene_range(s, s->st.chapter, &first, &count)) {
            frame.kind = ANEM_FRAME_BOUNDARY;
            return frame;
        }
        if (s->st.cursor < first || s->st.cursor >= first + count) {
            // 越界：等价于源 readFrame 的 returned()
            script_return(s);
            if (s->failed) {
                frame.kind = ANEM_FRAME_BOUNDARY;
                return frame;
            }
            continue;
        }
        anem_op_t op;
        if (!anem_pack_op(s->pack, s->st.cursor, &op)) {
            set_error(s, "指令越界");
            frame.kind = ANEM_FRAME_ERROR;
            return frame;
        }
        const uint32_t next_cursor = s->st.cursor + 1;
        switch (op.kind) {
        case ANEM_OP_TEXT: {
            s->st.speaker = op.a;
            s->st.text = (uint16_t)op.b;
            frame.kind = ANEM_FRAME_LINE;
            frame.speaker = op.a;
            frame.text = (uint16_t)op.b;
            return frame;
        }
        case ANEM_OP_BG: {
            const uint16_t name = op.a;
            if (op.flags & 1) {
                s->st.cg = name;
                s->st.changed_cg = true;
            } else {
                // 源 resolveBackgroundName()：名字 + $time_control 对应的后缀
                // （_01/_02/_03/_04/_80/_04/_05）存在时用变体，否则用原名。
                const char *base = text_of(s, name);
                char candidate[96];
                snprintf(candidate, sizeof(candidate), "%s%s", base, time_suffix(s));
                uint16_t variant_text = 0, variant_image = 0;
                if (!anem_pack_exists_image(s->pack, base) &&
                    anem_pack_image_entry(s->pack, candidate, &variant_text, &variant_image)) {
                    s->st.regular_bg = variant_text;  // 源：base 不存在 -> 用时段变体
                } else {
                    s->st.regular_bg = name;
                }
            }
            s->st.cursor = next_cursor;
            break;
        }
        case ANEM_OP_PORTRAIT:
            enter_portrait(s, &op);
            if (s->failed) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_COPY_BODIES:
            if (op.flags == 0) {
                uint8_t out = 0;
                for (uint8_t i = 0; i < s->st.body_count; i++) {
                    char actor[3] = { 0 };
                    const int slot = actor_slot_of_name(s, text_of(s, s->st.bodies[i].name),
                                                       actor);
                    if (slot >= 0 && (s->st.changed & (1u << slot))) {
                        s->st.bodies[out++] = s->st.bodies[i];
                    }
                }
                s->st.body_count = out;
            }
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_COPY_CG:
            if (op.flags == 0 && !s->st.changed_cg) {
                s->st.cg = 0;
            }
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_COMMIT:
            s->st.changed = 0;
            s->st.changed_cg = false;
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_INIT_TIME:
            if (op.flags & 1) {
                memset(s->st.dress, 0, sizeof(s->st.dress));
                memset(s->st.pose, 0, sizeof(s->st.pose));
            }
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_IF_FALSE:
        case ANEM_OP_IF_TRUE: {
            uint32_t off = 0;
            anem_value_t cond;
            s->arena_used = 0;
            if (!anem_pack_cond(s->pack, op.a, &off) || !eval_expr(s, off, &cond, 0)) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            const bool want = op.kind == ANEM_OP_IF_FALSE ? !truthy(cond) : truthy(cond);
            s->st.cursor = want ? op.b : next_cursor;
            break;
        }
        case ANEM_OP_GOTO:
            s->st.cursor = op.b;
            break;
        case ANEM_OP_CALL:
            if (s->call_count < ANEM_STACK_FRAMES) {
                anem_call_frame_t *call = &s->calls[s->call_count++];
                memset(call, 0, sizeof(*call));
                call->scene = s->st.chapter;
                call->cursor = next_cursor;
                call->board_stage = 0;
            }
            s->st.cursor = op.b;
            break;
        case ANEM_OP_RETURN:
            script_return(s);
            break;
        case ANEM_OP_JUMP:
        case ANEM_OP_CALL_SCENE: {
            uint16_t entry = (uint16_t)op.b;
            if (op.flags & 1) {
                anem_value_t value;
                if (!eval_expr(s, op.c, &value, 0)) {
                    frame.kind = ANEM_FRAME_ERROR;
                    return frame;
                }
                entry = (uint16_t)num_of(value);
            }
            const bool call = op.kind == ANEM_OP_CALL_SCENE;
            if (!jump_with_frame(s, (uint16_t)op.a, entry, call)) {
                frame.kind = ANEM_FRAME_BOUNDARY;
                return frame;
            }
            break;
        }
        case ANEM_OP_CHOICE: {
            if (!build_choices(s, &op)) {
                if (s->failed) {
                    frame.kind = ANEM_FRAME_ERROR;
                    return frame;
                }
                set_error(s, "此处没有可用选项");
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            frame.kind = ANEM_FRAME_CHOICE;
            frame.count = s->choice_count;
            return frame;
        }
        case ANEM_OP_BOARD: {
            if (s->board_count == 0) {
                s->last_call_result = 1;
                s->st.cursor = next_cursor;
                break;
            }
            s->choice_count = 0;
            for (uint8_t i = 0; i < s->board_count && i < ANEM_CHOICE_MAX; i++) {
                uint16_t detail = 0;
                board_label(s, &s->board[i], s->boundary_chapter, sizeof(s->boundary_chapter),
                            &detail);
                anem_choice_item_t *item = &s->choice[s->choice_count++];
                memset(item, 0, sizeof(*item));
                item->label = intern_text(s, s->boundary_chapter);
                item->detail = detail;
                item->enabled = 1;
                item->value = i;
                item->board = true;
            }
            frame.kind = ANEM_FRAME_BOARD;
            frame.count = s->choice_count;
            return frame;
        }
        case ANEM_OP_END:
            s->st.ended = true;
            frame.kind = ANEM_FRAME_END;
            return frame;
        case ANEM_OP_OMITTED:
            s->last_call_result = 0;
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_SKIP_GAME: {
            // 源 story-game-flags.js：小游戏被省略，只写入剧情用到的结果。
            if (op.a == 1) {  // rps
                var_set_num(s, "a:409", 10);
                var_set_num(s, "a:410", 0);
                var_set_num(s, "a:411", 0);
            } else if (op.a == 2) {  // quiz
                var_set_num(s, "a:412", 10);
            }
            s->last_call_result = 0;
            s->st.cursor = next_cursor;
            break;
        }
        case ANEM_OP_ROUTE_CHECK:
            if (anem_script_route_check(s)) {
                if (s->failed) {
                    frame.kind = ANEM_FRAME_BOUNDARY;
                    return frame;
                }
                break;
            }
            s->st.cursor = next_cursor;
            break;
        case ANEM_OP_SET_DYN: {
            s->arena_used = 0;
            anem_value_t index;
            if (!eval_expr(s, op.b, &index, 0)) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            const uint16_t id = var_lookup_dynamic(s, text_of(s, op.a), num_of(index));
            anem_value_t value;
            if (!eval_expr(s, op.c, &value, 0)) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            if (id != 0xFFFF) {
                s->st.vars[id] = value;
            }
            s->st.cursor = next_cursor;
            break;
        }
        case ANEM_OP_SET: {
            anem_set_t set;
            s->arena_used = 0;
            if (!anem_pack_set(s->pack, op.a, &set)) {
                set_error(s, "赋值表越界");
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            anem_value_t value;
            if (!eval_expr(s, set.expr, &value, 0)) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            var_assign(s, set.var, value);
            s->st.cursor = next_cursor;
            break;
        }
        case ANEM_OP_NATIVE: {
            uint32_t off = 0;
            anem_value_t value;
            s->arena_used = 0;
            if (!anem_pack_call(s->pack, op.a, &off) || !eval_expr(s, off, &value, 0)) {
                frame.kind = ANEM_FRAME_ERROR;
                return frame;
            }
            s->st.cursor = next_cursor;
            break;
        }
        case ANEM_OP_ARRIVE_CHOICE: {
            s->choice_count = 1;
            memset(&s->choice[0], 0, sizeof(s->choice[0]));
            s->choice[0].label = op.a;
            s->choice[0].enabled = 1;
            s->choice[0].value = (uint8_t)op.c;
            s->choice[0].board = false;
            s->arrive_var = 0xFFFF;
            frame.kind = ANEM_FRAME_CHOICE;
            frame.count = 1;
            s->arrive_pending = true;
            return frame;
        }
        case ANEM_OP_BOUNDARY:
            frame.kind = ANEM_FRAME_BOUNDARY;
            return frame;
        default:
            set_error(s, "未知指令");
            frame.kind = ANEM_FRAME_ERROR;
            return frame;
        }
    }
    set_error(s, "此章节无法继续");
    frame.kind = ANEM_FRAME_ERROR;
    return frame;
}

const anem_choice_item_t *anem_script_choice_item(const anem_script_t *s, uint8_t index)
{
    if (s == NULL || index >= s->choice_count) {
        return NULL;
    }
    return &s->choice[index];
}

bool anem_script_choose(anem_script_t *s, uint8_t index)
{
    if (index >= s->choice_count) {
        set_error(s, "选项下标越界");
        return false;
    }
    const anem_choice_item_t item = s->choice[index];
    if (item.board) {
        const anem_board_item_t board = s->board[item.value];
        const uint16_t scene = board_scene_id(s, board.scene);
        if (scene == ANEM_NONE16 || !jump_with_frame(s, scene, board.entry_letter, true)) {
            if (anem_script_error(s) == NULL) {
                set_error(s, "委托章节不在本卷内");
            }
            return false;
        }
        if (s->call_count > 0) {
            s->calls[s->call_count - 1].board_stage = 1;
            s->calls[s->call_count - 1].board = board;
        }
        s->choice_count = 0;
        return true;
    }
    if (s->arrive_pending) {
        const uint16_t id = var_id(s, "$select_btn");
        if (id != 0xFFFF) {
            var_assign(s, id, v_num(item.value));
        }
        s->arrive_pending = false;
        s->st.cursor += 1;
        s->choice_count = 0;
        return true;
    }
    // dynamicChoice：把 value 写回 target 表达式指向的变量，再前进一句。
    if (!choice_assign_target(s, item.value)) {
        return false;
    }
    s->st.cursor += 1;
    s->choice_count = 0;
    return true;
}

void anem_script_next(anem_script_t *s)
{
    // 历史：只保留最近几条完整状态（上一句）。
    const uint8_t slot = s->history_next;
    s->history[slot].state = s->st;
    s->history[slot].used = true;
    s->history_next = (uint8_t)((slot + 1) % ANEM_HISTORY_MAX);
    if (s->history_count < ANEM_HISTORY_MAX) {
        s->history_count++;
    }
    // 旁白前的空行（text id = 0xFFFF）不进历史，否则历史列表首行是空白项。
    if (s->st.text != ANEM_NONE16) {
        s->history_texts[s->history_text_count % 24] = s->st.text;
        s->history_text_count++;
    }
    s->st.cursor += 1;
    s->st.ended = false;
}

bool anem_script_previous(anem_script_t *s)
{
    if (s->history_count == 0) {
        return false;
    }
    s->history_next = (uint8_t)((s->history_next + ANEM_HISTORY_MAX - 1) % ANEM_HISTORY_MAX);
    s->history_count--;
    if (!s->history[s->history_next].used) {
        return false;
    }
    s->st = s->history[s->history_next].state;
    s->history[s->history_next].used = false;
    return true;
}

uint8_t anem_script_history_text_count(const anem_script_t *s)
{
    return s->history_text_count > 24 ? 24 : s->history_text_count;
}

uint16_t anem_script_history_text(const anem_script_t *s, uint8_t index)
{
    const uint8_t count = anem_script_history_text_count(s);
    if (index >= count) {
        return 0;
    }
    const uint8_t first = (uint8_t)((s->history_text_count - count) % 24);
    return s->history_texts[(first + index) % 24];
}

// ---------------------------------------------------------------- 初始化 / 画面

static void reset_state(anem_script_t *s)
{
    memset(&s->st, 0, sizeof(s->st));
    uint16_t count = 0;
    while (count < ANEM_VAR_MAX && anem_pack_var(s->pack, count, NULL)) {
        count++;
    }
    s->st.var_count = (uint16_t)count;
    for (uint16_t i = 0; i < count; i++) {
        s->st.vars[i] = v_num(0);
    }
    s->st.regular_bg = intern_text(s, "bg_black");
    s->st.speaker = ANEM_NONE16;
    s->st.text = ANEM_NONE16;
    s->st.progression = 1;
    s->call_count = 0;
    s->board_count = 0;
    s->choice_count = 0;
    s->history_count = 0;
    s->history_next = 0;
    s->history_text_count = 0;
    s->last_call_result = 0;
    s->route_result = 0;
    s->route_prelude = 0;
    s->failed = false;
    s->notice[0] = 0;
    s->boundary_chapter[0] = 0;
    s->arrive_pending = false;
    // 源 newState：$time_control 未初始化，initTime 会调 $$init_time_control
    var_set_num(s, "$time_control", 0);
}

bool anem_script_init(anem_script_t *s, const anem_pack_t *pack)
{
    if (s == NULL || pack == NULL) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->pack = pack;
    s->rng = 1931;
    uint16_t count = 0;
    while (count < ANEM_VAR_MAX && anem_pack_var(pack, count, NULL)) {
        count++;
    }
    s->local_count = 0;
    for (uint16_t i = 0; i < count && s->local_count < 24; i++) {
        const char *name = anem_var_name(pack, i);
        if (name != NULL && strncmp(name, "cur_call.", 9) == 0) {
            s->local_ids[s->local_count++] = i;
        }
    }
    reset_state(s);
    return true;
}

void anem_script_new(anem_script_t *s, uint16_t scene, bool story)
{
    reset_state(s);
    s->st.progression = story ? 1 : 0;
    jump_direct(s, scene, 0);
    s->st.root_chapter = scene;
}

bool anem_script_start_chapter(anem_script_t *s, uint16_t scene, uint16_t entry)
{
    uint32_t first = 0, count = 0;
    if (!scene_range(s, scene, &first, &count)) {
        return false;
    }
    reset_state(s);
    s->st.progression = 0;
    jump_direct(s, scene, entry);
    s->st.root_chapter = scene;
    return true;
}

const char *anem_script_chapter_name(const anem_script_t *s)
{
    anem_chap_t chap;
    const uint16_t scene = s->st.root_chapter ? s->st.root_chapter : s->st.chapter;
    if (!anem_pack_chapter_for(s->pack, scene, &chap)) {
        return "";
    }
    return text_of(s, chap.name);
}

const char *anem_script_day_label(const anem_script_t *s)
{
    const int32_t month = var_num((anem_script_t *)s, "a:1");
    const int32_t day = var_num((anem_script_t *)s, "a:2");
    static char buf[16];
    if (month >= 8) {
        return "";
    }
    if (month == 7 && day >= 1 && day <= 25) {
        snprintf(buf, sizeof(buf), "Day %d", (int)day);
        return buf;
    }
    anem_chap_t chap;
    const uint16_t scene = s->st.root_chapter ? s->st.root_chapter : s->st.chapter;
    if (!anem_pack_chapter_for(s->pack, scene, &chap)) {
        return "";
    }
    return text_of(s, chap.day);
}

uint8_t anem_script_unlock_level(const anem_script_t *s)
{
    anem_script_t *mutable_s = (anem_script_t *)s;
    int32_t level = var_num(mutable_s, "g:30");
    if (level == 0) {
        bool all = true;
        for (int i = 0; i < 5; i++) {
            char key[8];
            snprintf(key, sizeof(key), "g:%d", i);
            if (var_num(mutable_s, key) != 1) {
                all = false;
                break;
            }
        }
        if (all) {
            level = 1;
        }
    }
    if (level == 2 && var_num(mutable_s, "g:5") == 1) {
        level = 3;
    }
    if (level == 4 && var_num(mutable_s, "g:6") == 1) {
        level = 5;
    }
    if (level == 1) {
        level = 2;
    }
    if (level == 3) {
        level = 4;
    }
    return (uint8_t)(level < 0 ? 0 : (level > 9 ? 9 : level));
}

const char *anem_script_error(const anem_script_t *s)
{
    return (s != NULL && s->failed) ? s->notice : NULL;
}

const char *anem_script_notice(const anem_script_t *s) { return s ? s->notice : ""; }

void anem_script_picture(const anem_script_t *s, anem_picture_t *out)
{
    memset(out, 0, sizeof(*out));
    out->back_image = ANEM_NONE16;
    out->back_name = s->st.cg != 0 ? s->st.cg : s->st.regular_bg;
    out->cg_layer = s->st.cg != 0;
    const char *name = text_of(s, out->back_name);
    if (name[0] != 0) {
        out->back_image = anem_pack_image_by_name(s->pack, name);
    }
    const int total = s->st.body_count;
    const int cell = total >= 3 ? 144 : (total == 2 ? 216 : 432);
    for (int i = 0; i < total && i < ANEM_BODY_MAX; i++) {
        const char *pose = text_of(s, s->st.bodies[i].name);
        if (pose[0] == 0) {
            continue;
        }
        char actor[3] = { 0 };
        const int slot = actor_slot_of_name((anem_script_t *)s, pose, actor);
        int dress = (int)anem_pack_actor_default_dress(s->pack, actor);
        if (slot >= 0 && s->st.dress[slot] != 0) {
            dress = s->st.dress[slot];
        }
        anem_pose_view_t view;
        if (!anem_pack_pose(s->pack, pose, dress, dress, &view)) {
            continue;
        }
        anem_image_t body_img;
        if (!anem_pack_image(s->pack, view.body, &body_img)) {
            continue;
        }
        const float scale = 320.0f / 432.0f;
        const int left = (int)lroundf(((i + 0.5f) * cell -
                                       (body_img.w / scale) * (view.lay1 / 1000.0f)) * scale);
        const int top = (int)lroundf(286.0f * scale) - body_img.h;
        anem_sprite_t *sprite = &out->sprites[out->sprite_count++];
        sprite->valid = true;
        sprite->image = view.body;
        sprite->x = (int16_t)left;
        sprite->y = (int16_t)top;
    }
}
