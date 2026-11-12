// main/clannad_vm.c —— RW5 虚拟机 + 存档/通关记录编解码实现。
#include "clannad_vm.h"

#include <stdio.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- utilities
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t crc32_of(const uint8_t *data, size_t len)
{
    return crc32_update(0, data, len);
}

static uint16_t u8_len(const char *s)
{
    return s ? (uint16_t)strlen(s) : 0;
}

// UTF-8：返回下一个码位起点；非法字节按 1 字节推进。
static uint16_t utf8_next(const char *s, uint16_t len, uint16_t pos)
{
    if (pos >= len) return len;
    const uint8_t *p = (const uint8_t *)s;
    uint8_t lead = p[pos];
    uint16_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) step = 2;
    else if ((lead & 0xF0u) == 0xE0u) step = 3;
    else if ((lead & 0xF8u) == 0xF0u) step = 4;
    if (pos + step > len) return len;
    for (uint16_t i = 1; i < step; ++i) {
        if ((p[pos + i] & 0xC0u) != 0x80u) return pos + 1;
    }
    return pos + step;
}

static int utf8_count(const char *s, uint16_t len)
{
    int n = 0;
    for (uint16_t pos = 0; pos < len; n++) pos = utf8_next(s, len, pos);
    return n;
}

static bool utf8_valid(const char *s, uint16_t len)
{
    uint16_t pos = 0;
    while (pos < len) {
        const uint8_t lead = (uint8_t)s[pos];
        uint16_t step;
        uint32_t cp;
        if (lead < 0x80u) {
            if (lead == 0) return false;
            step = 1;
            cp = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            step = 2;
            cp = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            step = 3;
            cp = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            step = 4;
            cp = lead & 0x07u;
        } else {
            return false;
        }
        if (pos + step > len) return false;
        for (uint16_t i = 1; i < step; ++i) {
            const uint8_t b = (uint8_t)s[pos + i];
            if ((b & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (step == 2 && cp < 0x80u) return false;
        if (step == 3 && cp < 0x800u) return false;
        if (step == 4 && cp < 0x10000u) return false;
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        pos += step;
    }
    return true;
}

// 把 [pos,pos+units) 码位区间换算成字节偏移。
static uint16_t utf8_offset_for_units(const char *s, uint16_t len, int units)
{
    uint16_t pos = 0;
    for (int i = 0; i < units && pos < len; ++i) pos = utf8_next(s, len, pos);
    return pos;
}

static int utf8_index_of(const char *hay, uint16_t hay_len, const char *needle,
                         uint16_t needle_len)
{
    if (!needle_len) return 0;
    int index = 0;
    for (uint16_t pos = 0; pos + needle_len <= hay_len;
         pos = utf8_next(hay, hay_len, pos), ++index) {
        if (memcmp(hay + pos, needle, needle_len) == 0) return index;
    }
    return -1;
}

static bool intern_string(clannad_vm_t *vm, const char *src, uint16_t len,
                          uint16_t *off_out, uint16_t *len_out)
{
    if (!vm || (len && !src)) return false;
    if (len == 0) {
        *off_out = 0;
        *len_out = 0;
        return true;
    }
    uint16_t pos = 0;
    while (pos + 2 <= vm->string_used) {
        const uint16_t n = clannad_rd16((const uint8_t *)vm->strings + pos);
        const uint16_t body = (uint16_t)(pos + 2);
        if (body + n > vm->string_used) break;
        if (n == len && memcmp(vm->strings + body, src, len) == 0) {
            *off_out = body;
            *len_out = n;
            return true;
        }
        pos = (uint16_t)(body + n);
    }
    const uint32_t need = (uint32_t)vm->string_used + 2u + len;
    if (need > CLAN_VM_STRING_POOL) {
        snprintf(vm->error, sizeof(vm->error), "string pool full (%u)", (unsigned)need);
        return false;
    }
    vm->strings[vm->string_used] = (char)(len & 0xFFu);
    vm->strings[vm->string_used + 1] = (char)(len >> 8);
    memcpy(vm->strings + vm->string_used + 2, src, len);
    *off_out = (uint16_t)(vm->string_used + 2);
    *len_out = len;
    vm->string_used = (uint16_t)need;
    return true;
}

static uint32_t var_key(uint8_t kind, uint32_t index)
{
    return (((uint32_t)kind & 0x1Fu) << 20) | (index & CLAN_NO_INDEX);
}

static uint32_t var_hash(uint32_t key)
{
    key *= 2654435761u;
    return (key >> 23) & (CLAN_VM_VAR_SLOTS - 1u);
}

static clannad_var_t *var_find(clannad_vm_t *vm, uint32_t key, bool create)
{
    const uint32_t stored = key + 1u;
    uint32_t h = var_hash(key);
    for (uint32_t probe = 0; probe < CLAN_VM_VAR_SLOTS; ++probe) {
        clannad_var_t *slot = &vm->vars[(h + probe) & (CLAN_VM_VAR_SLOTS - 1u)];
        if (slot->key == stored) return slot;
        if (slot->key == 0) {
            if (!create) return NULL;
            if (vm->var_count + 1u >= CLAN_VM_VAR_SLOTS * 3u / 4u) {
                snprintf(vm->error, sizeof(vm->error), "variable table full");
                return NULL;
            }
            slot->key = stored;
            slot->type = 0;
            slot->value = 0;
            slot->str_off = 0;
            slot->str_len = 0;
            vm->var_count++;
            return slot;
        }
    }
    snprintf(vm->error, sizeof(vm->error), "variable table probe failed");
    return NULL;
}

static const clannad_var_t *var_get(const clannad_vm_t *vm, uint32_t key)
{
    const uint32_t stored = key + 1u;
    uint32_t h = var_hash(key);
    for (uint32_t probe = 0; probe < CLAN_VM_VAR_SLOTS; ++probe) {
        const clannad_var_t *slot = &vm->vars[(h + probe) & (CLAN_VM_VAR_SLOTS - 1u)];
        if (slot->key == stored) return slot;
        if (slot->key == 0) return NULL;
    }
    return NULL;
}

static bool is_key_kind(uint8_t kind)
{
    return kind == 6;   // cur_call.k 的空值在源引擎里是 ""
}

// ---------------------------------------------------------------- scene ops
static bool ensure_block(clannad_vm_t *vm)
{
    uint16_t scene_index;
    if (!clannad_pack_find_scene(&vm->pack, vm->chapter, &scene_index)) {
        snprintf(vm->error, sizeof(vm->error), "scene %u missing", vm->chapter);
        return false;
    }
    clannad_scene_t scene;
    if (!clannad_pack_scene(&vm->pack, scene_index, &scene)) return false;
    if (vm->cursor >= scene.length) return true;
    const uint16_t block_index =
        (uint16_t)(scene.first_block + vm->cursor / CLAN_BLOCK_SIZE);
    if (vm->block_loaded && vm->loaded_block == block_index) return true;
    clannad_block_rec_t rec;
    if (!clannad_pack_load_block(&vm->pack, block_index, vm->raw, vm->raw_cap, &rec)) {
        snprintf(vm->error, sizeof(vm->error), "block %u/%u decode failed",
                 vm->chapter, (unsigned)(vm->cursor / CLAN_BLOCK_SIZE));
        vm->block_loaded = false;
        return false;
    }
    vm->loaded_block = block_index;
    vm->block_loaded = true;
    vm->block_ops = clannad_rd16(vm->raw);
    vm->block_text_size = clannad_rd16(vm->raw + 2);
    vm->block_extra_size = clannad_rd16(vm->raw + 4);
    return true;
}

static bool current_op(clannad_vm_t *vm, clannad_op_t *op)
{
    if (!ensure_block(vm)) return false;
    const uint16_t local = (uint16_t)(vm->cursor % CLAN_BLOCK_SIZE);
    if (local >= vm->block_ops) {
        snprintf(vm->error, sizeof(vm->error), "op index out of block");
        return false;
    }
    return clannad_pack_block_op(vm->raw, vm->raw_cap, local, op);
}

static const uint8_t *block_extra(const clannad_vm_t *vm)
{
    return vm->raw + 8u + (uint32_t)vm->block_ops * CLAN_OP_BYTES;
}

static const char *block_text(const clannad_vm_t *vm)
{
    return (const char *)(vm->raw + 8u + (uint32_t)vm->block_ops * CLAN_OP_BYTES +
                          vm->block_extra_size);
}

// ---------------------------------------------------------------- values
typedef struct {
    bool is_str;
    int32_t i;
    const char *s;
    uint16_t len;
} val_t;

static bool val_truthy(const val_t *v)
{
    return v->is_str ? v->len > 0 : v->i != 0;
}

static int32_t val_int(const val_t *v)
{
    if (!v->is_str) return v->i;
    int32_t value = 0;
    int sign = 1;
    uint16_t pos = 0;
    while (pos < v->len && (v->s[pos] == ' ' || v->s[pos] == '\t' ||
                            v->s[pos] == '\n' || v->s[pos] == '\r')) {
        ++pos;
    }
    if (pos < v->len && (v->s[pos] == '-' || v->s[pos] == '+')) {
        if (v->s[pos] == '-') sign = -1;
        ++pos;
    }
    bool any = false;
    while (pos < v->len && v->s[pos] >= '0' && v->s[pos] <= '9') {
        value = value * 10 + (v->s[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? value * sign : 0;
}

static void val_int_out(val_t *v, int32_t i)
{
    v->is_str = false;
    v->i = i;
    v->s = NULL;
    v->len = 0;
}

static bool val_string(clannad_vm_t *vm, val_t *v, const char *s, uint16_t len)
{
    uint16_t off, n;
    if (!intern_string(vm, s, len, &off, &n)) return false;
    v->is_str = true;
    v->s = vm->strings + off;
    v->len = n;
    v->i = 0;
    return true;
}

static const char *fallback_decimal(int32_t value, char *buf, size_t cap)
{
    snprintf(buf, cap, "%ld", (long)value);
    return buf;
}

// ---------------------------------------------------------------- expressions
static bool eval_expr(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                      uint16_t off, val_t *out, int depth);
static bool eval_var_value(clannad_vm_t *vm, uint32_t key, val_t *out);

static bool eval_var_key(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                         uint16_t off, uint32_t *key_out, bool *is_var)
{
    *is_var = false;
    if (off >= extra_size) return false;
    const uint8_t code = extra[off];
    if (code == 0x10) {  // VAR_STATIC
        if ((uint32_t)off + 4 > extra_size) return false;
        const uint8_t kind = extra[off + 1];
        const uint16_t index = clannad_rd16(extra + off + 2);
        *key_out = var_key(kind, index);
        *is_var = true;
        return true;
    }
    if (code == 0x11) {  // VAR_NOINDEX
        if ((uint32_t)off + 2 > extra_size) return false;
        *key_out = var_key(extra[off + 1], CLAN_NO_INDEX);
        *is_var = true;
        return true;
    }
    if (code == 0x12) {  // VAR_DYNAMIC
        if ((uint32_t)off + 2 > extra_size) return false;
        val_t index;
        if (!eval_expr(vm, extra, extra_size, (uint16_t)(off + 2), &index, 0)) return false;
        int32_t value = val_int(&index);
        if (value < 0) value = 0;
        if (value > (int32_t)CLAN_NO_INDEX - 1) value = (int32_t)CLAN_NO_INDEX - 1;
        *key_out = var_key(extra[off + 1], (uint32_t)value);
        *is_var = true;
        return true;
    }
    return true;
}

// 真正使用的表达式求值：同时返回解析结束位置。
static bool eval_expr_end(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                          uint16_t off, val_t *out, uint16_t *end, int depth);

static bool eval_expr(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                      uint16_t off, val_t *out, int depth)
{
    uint16_t end = 0;
    return eval_expr_end(vm, extra, extra_size, off, out, &end, depth);
}

// 变量节点：一次求值同时得到值与 key（动态下标只求值一次，避免计数器副作用）。
static bool eval_var_node(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                          uint16_t off, val_t *out, uint32_t *key, uint16_t *end,
                          int depth)
{
    const uint8_t code = extra[off];
    if (code == 0x10) {
        if ((uint32_t)off + 4 > extra_size) return false;
        *key = var_key(extra[off + 1], clannad_rd16(extra + off + 2));
        *end = (uint16_t)(off + 4);
        return eval_var_value(vm, *key, out);
    }
    if (code == 0x11) {
        if ((uint32_t)off + 2 > extra_size) return false;
        *key = var_key(extra[off + 1], CLAN_NO_INDEX);
        *end = (uint16_t)(off + 2);
        return eval_var_value(vm, *key, out);
    }
    if (code == 0x12) {
        if ((uint32_t)off + 2 > extra_size) return false;
        val_t index;
        uint16_t sub_end = (uint16_t)(off + 2);
        if (!eval_expr_end(vm, extra, extra_size, (uint16_t)(off + 2), &index,
                           &sub_end, depth + 1)) {
            return false;
        }
        int32_t value = val_int(&index);
        if (value < 0) value = 0;
        if (value > (int32_t)CLAN_NO_INDEX - 1) value = (int32_t)CLAN_NO_INDEX - 1;
        *key = var_key(extra[off + 1], (uint32_t)value);
        *end = sub_end;
        return eval_var_value(vm, *key, out);
    }
    return false;
}

static bool eval_call_real(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                           uint16_t off, val_t *out, uint16_t *end, int depth)
{
    if ((uint32_t)off + 3 > extra_size) return false;
    const uint8_t nid = extra[off + 1];
    const uint8_t argc = extra[off + 2];
    if (argc > 8) {
        snprintf(vm->error, sizeof(vm->error), "native argc %u", argc);
        return false;
    }
    val_t args[8];
    uint16_t pos = (uint16_t)(off + 3);
    for (uint8_t i = 0; i < argc; ++i) {
        uint16_t sub_end = pos;
        if (!eval_expr_end(vm, extra, extra_size, pos, &args[i], &sub_end, depth + 1)) {
            return false;
        }
        pos = sub_end;
    }
    val_t receiver;
    bool has_receiver = false;
    uint32_t receiver_key = 0;
    bool receiver_is_var = false;
    if (pos >= extra_size) return false;
    has_receiver = extra[pos++] != 0;
    if (has_receiver) {
        const uint8_t rcode = extra[pos];
        if (rcode == 0x10 || rcode == 0x11 || rcode == 0x12) {
            uint16_t sub_end = pos;
            if (!eval_var_node(vm, extra, extra_size, pos, &receiver, &receiver_key,
                               &sub_end, depth + 1)) {
                return false;
            }
            receiver_is_var = true;
            pos = sub_end;
        } else {
            uint16_t sub_end = pos;
            if (!eval_expr_end(vm, extra, extra_size, pos, &receiver, &sub_end,
                               depth + 1)) {
                return false;
            }
            pos = sub_end;
        }
    }
    *end = pos;

    const uint8_t kind = receiver_is_var ? (uint8_t)(receiver_key >> 20) : 0xFFu;
    const uint32_t index = receiver_is_var ? (receiver_key & CLAN_NO_INDEX) : CLAN_NO_INDEX;
    const int counter_slot = (kind == 7 && index < CLAN_VM_COUNTERS) ? (int)index : -1;

    switch (nid) {
    case 0: {  // counter.start / start_real / start_frame_real
        if (counter_slot >= 0) {
            memset(&vm->counters[counter_slot], 0, sizeof(vm->counters[counter_slot]));
            vm->counters[counter_slot].value = argc > 1 ? val_int(&args[1]) : 0;
            vm->counters[counter_slot].ticking = argc < 2;
        }
        val_int_out(out, 0);
        return true;
    }
    case 1: {  // counter.get
        if (counter_slot < 0) {
            val_int_out(out, 0);
            return true;
        }
        if (vm->counters[counter_slot].ticking) {
            vm->counters[counter_slot].value += 1000;
        }
        val_int_out(out, vm->counters[counter_slot].value);
        return true;
    }
    case 2:  // global.random
        val_int_out(out, 0);
        return true;
    case 3: {  // global.get_scene_name
        uint16_t scene_index;
        const char *source = "";
        if (clannad_pack_find_scene(&vm->pack, vm->chapter, &scene_index)) {
            source = clannad_pack_scene_source(&vm->pack, scene_index);
        }
        return val_string(vm, out, source, u8_len(source));
    }
    case 4:  // global.get_line_no
        val_int_out(out, 0);
        return true;
    case 5: {  // $filename_translate
        const val_t *src = argc ? &args[0] : NULL;
        char tmp[192];
        uint16_t len = 0;
        if (src) {
            if (src->is_str) {
                len = src->len < sizeof(tmp) ? src->len : (uint16_t)sizeof(tmp);
                for (uint16_t i = 0; i < len; ++i) {
                    char c = src->s[i];
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    tmp[i] = c;
                }
            } else {
                const char *dec = fallback_decimal(src->i, tmp, sizeof(tmp));
                len = u8_len(dec);
            }
        }
        return val_string(vm, out, tmp, len);
    }
    case 6: {  // global.namae
        const char *name = "朋也";
        if (argc && args[0].is_str) {
            if (args[0].len == 6) {
                static const struct { const char *key; const char *value; } MAP[] = {
                    {"％Ａ", "渚"}, {"％Ｂ", "秋生"}, {"％Ｃ", "朋也"},
                    {"％Ｄ", "朋也"}, {"％Ｅ", "朋也"}, {"％Ｆ", "朋也"},
                };
                for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); ++i) {
                    if (memcmp(args[0].s, MAP[i].key, 6) == 0) {
                        name = MAP[i].value;
                        break;
                    }
                }
            }
        }
        return val_string(vm, out, name, u8_len(name));
    }
    case 7:  // input/script checks: the watch presents settled frames
        val_int_out(out, 1);
        return true;
    case 8:  // timewait_key
        val_int_out(out, 1);
        return true;
    case 9:  // $disp_dialog
        val_int_out(out, 1);
        return true;
    case 10:  // auto mode flag
    case 11:  // anime type
    case 12:  // anime time
    case 13:  // no wipe
    case 14:  // koe dont stop
    case 15:  // filter color
    case 16:  // search chara center
        val_int_out(out, 0);
        return true;
    default:
        break;
    }
    // 字符串方法
    {
        char numbuf[16];
        const char *text = "";
        uint16_t text_len = 0;
        if (has_receiver) {
            if (receiver.is_str) {
                text = receiver.s;
                text_len = receiver.len;
            } else {
                text = fallback_decimal(receiver.i, numbuf, sizeof(numbuf));
                text_len = u8_len(text);
            }
        }
        if (nid == 17) {  // search
            const char *needle = "";
            uint16_t needle_len = 0;
            if (argc) {
                if (args[0].is_str) {
                    needle = args[0].s;
                    needle_len = args[0].len;
                } else {
                    needle = fallback_decimal(args[0].i, numbuf, sizeof(numbuf));
                    needle_len = u8_len(needle);
                }
            }
            val_int_out(out, utf8_index_of(text, text_len, needle, needle_len));
            return true;
        }
        if (nid == 18) {  // len / cnt
            val_int_out(out, utf8_count(text, text_len));
            return true;
        }
        if (nid == 19 || nid == 20) {  // mid / left
            const int32_t a0 = argc > 0 ? val_int(&args[0]) : 0;
            const int32_t a1 = argc > 1 ? val_int(&args[1]) : 0;
            const int units = utf8_count(text, text_len);
            int start;
            int count;
            if (nid == 20) {
                start = 0;
                count = a0;
            } else {
                start = a0;
                count = a1;
            }
            if (start < 0) start = units + start;
            if (start < 0) start = 0;
            if (start > units) start = units;
            if (count < 0) count = 0;
            if (start + count > units) count = units - start;
            const uint16_t b0 = utf8_offset_for_units(text, text_len, start);
            const uint16_t b1 = utf8_offset_for_units(text, text_len, start + count);
            return val_string(vm, out, text + b0, (uint16_t)(b1 - b0));
        }
        if (nid == 21) {  // tonum
            val_t tmp = {.is_str = true, .i = 0, .s = text, .len = text_len};
            val_int_out(out, val_int(&tmp));
            return true;
        }
    }
    snprintf(vm->error, sizeof(vm->error), "native %u not implemented", nid);
    return false;
}

static bool eval_var_value(clannad_vm_t *vm, uint32_t key, val_t *out)
{
    const clannad_var_t *var = var_get(vm, key);
    if (!var) {
        const uint8_t kind = (uint8_t)(key >> 20);
        if (is_key_kind(kind)) {
            return val_string(vm, out, "", 0);
        }
        val_int_out(out, 0);
        return true;
    }
    if (var->type == 1) {
        out->is_str = true;
        out->s = vm->strings + var->str_off;
        out->len = var->str_len;
        out->i = 0;
        return true;
    }
    val_int_out(out, var->value);
    return true;
}

// 跳过一棵已序列化的表达式（短路求值时仍需越过未执行的分支）。
static bool skip_expr(const uint8_t *extra, uint16_t extra_size, uint16_t off,
                      uint16_t *end, int depth)
{
    if (depth > 32 || off >= extra_size) return false;
    const uint8_t code = extra[off];
    uint16_t pos = (uint16_t)(off + 1);
    switch (code) {
    case 0x00: case 0x01:
        break;
    case 0x02:
        pos++;
        break;
    case 0x03:
        pos += 4;
        break;
    case 0x04:
        pos += 4;
        break;
    case 0x10:
        pos += 3;
        break;
    case 0x11:
        pos += 1;
        break;
    case 0x12:
        pos++;
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        break;
    case 0x20:
        pos++;
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        break;
    case 0x21:
        pos++;
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        break;
    case 0x22: {
        if ((uint32_t)pos + 2 > extra_size) return false;
        const uint8_t argc = extra[pos + 1];
        pos = (uint16_t)(pos + 2);
        for (uint8_t i = 0; i < argc; ++i) {
            if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        }
        if (pos >= extra_size) return false;
        const bool has_receiver = extra[pos++] != 0;
        if (has_receiver &&
            !skip_expr(extra, extra_size, pos, &pos, depth + 1)) {
            return false;
        }
        break;
    }
    case 0x23: case 0x24:
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        if (!skip_expr(extra, extra_size, pos, &pos, depth + 1)) return false;
        break;
    default:
        return false;
    }
    if (pos > extra_size) return false;
    *end = pos;
    return true;
}

static bool eval_expr_end(clannad_vm_t *vm, const uint8_t *extra, uint16_t extra_size,
                          uint16_t off, val_t *out, uint16_t *end, int depth)
{
    if (depth > 32 || off >= extra_size) {
        snprintf(vm->error, sizeof(vm->error), "expression depth/range");
        return false;
    }
    const uint8_t code = extra[off];
    uint16_t pos = (uint16_t)(off + 1);
    if (code == 0x00) {
        val_int_out(out, 0);
        *end = pos;
        return true;
    }
    if (code == 0x01) {
        val_int_out(out, 1);
        *end = pos;
        return true;
    }
    if (code == 0x02) {
        if (pos + 1 > extra_size) return false;
        val_int_out(out, (int8_t)extra[pos]);
        *end = (uint16_t)(pos + 1);
        return true;
    }
    if (code == 0x03) {
        if (pos + 4 > extra_size) return false;
        val_int_out(out, (int32_t)clannad_rd32(extra + pos));
        *end = (uint16_t)(pos + 4);
        return true;
    }
    if (code == 0x04) {  // block text string
        if (pos + 4 > extra_size) return false;
        const uint16_t toff = clannad_rd16(extra + pos);
        const uint16_t tlen = clannad_rd16(extra + pos + 2);
        if ((uint32_t)toff + tlen > vm->block_text_size) return false;
        if (!utf8_valid(block_text(vm) + toff, tlen)) return false;
        if (!val_string(vm, out, block_text(vm) + toff, tlen)) return false;
        *end = (uint16_t)(pos + 4);
        return true;
    }
    if (code == 0x10) {
        if (pos + 3 > extra_size) return false;
        const uint8_t kind = extra[pos];
        const uint16_t index = clannad_rd16(extra + pos + 1);
        if (!eval_var_value(vm, var_key(kind, index), out)) return false;
        *end = (uint16_t)(pos + 3);
        return true;
    }
    if (code == 0x11) {
        if (pos + 1 > extra_size) return false;
        const uint8_t kind = extra[pos];
        if (!eval_var_value(vm, var_key(kind, CLAN_NO_INDEX), out)) return false;
        *end = (uint16_t)(pos + 1);
        return true;
    }
    if (code == 0x12) {
        if (pos + 1 > extra_size) return false;
        const uint8_t kind = extra[pos];
        val_t index;
        uint16_t sub_end = (uint16_t)(pos + 1);
        if (!eval_expr_end(vm, extra, extra_size, (uint16_t)(pos + 1), &index,
                           &sub_end, depth + 1)) {
            return false;
        }
        int32_t value = val_int(&index);
        if (value < 0) value = 0;
        if (value > (int32_t)CLAN_NO_INDEX - 1) value = (int32_t)CLAN_NO_INDEX - 1;
        if (!eval_var_value(vm, var_key(kind, (uint32_t)value), out)) return false;
        *end = sub_end;
        return true;
    }
    if (code == 0x20) {  // BINOP
        if (pos + 1 > extra_size) return false;
        const uint8_t bop = extra[pos];
        val_t a, b;
        uint16_t a_end = (uint16_t)(pos + 1);
        if (!eval_expr_end(vm, extra, extra_size, (uint16_t)(pos + 1), &a, &a_end,
                           depth + 1)) {
            return false;
        }
        uint16_t b_end = a_end;
        if (!eval_expr_end(vm, extra, extra_size, a_end, &b, &b_end, depth + 1)) {
            return false;
        }
        *end = b_end;
        if (bop == 1) {  // '-'
            val_int_out(out, val_int(&a) - val_int(&b));
            return true;
        }
        if (bop == 0) {  // '+'
            if (a.is_str || b.is_str) {
                char tmp[512];
                char num[16];
                const char *as = a.is_str ? a.s : fallback_decimal(a.i, num, sizeof(num));
                const uint16_t alen = a.is_str ? a.len : u8_len(as);
                char num2[16];
                const char *bs = b.is_str ? b.s : fallback_decimal(b.i, num2, sizeof(num2));
                const uint16_t blen = b.is_str ? b.len : u8_len(bs);
                if ((uint32_t)alen + blen > sizeof(tmp)) {
                    snprintf(vm->error, sizeof(vm->error), "concat too long");
                    return false;
                }
                memcpy(tmp, as, alen);
                memcpy(tmp + alen, bs, blen);
                return val_string(vm, out, tmp, (uint16_t)(alen + blen));
            }
            val_int_out(out, a.i + b.i);
            return true;
        }
        if (bop == 5 || bop == 6) {  // == / !=
            bool eq;
            if (a.is_str && b.is_str) {
                eq = a.len == b.len && memcmp(a.s, b.s, a.len) == 0;
            } else if (!a.is_str && !b.is_str) {
                eq = a.i == b.i;
            } else {
                eq = false;
            }
            val_int_out(out, bop == 5 ? (eq ? 1 : 0) : (eq ? 0 : 1));
            return true;
        }
        int32_t ai = val_int(&a), bi = val_int(&b);
        switch (bop) {
        case 2: val_int_out(out, ai * bi); return true;
        case 3: val_int_out(out, bi ? ai / bi : 0); return true;
        case 4: val_int_out(out, bi ? ai % bi : 0); return true;
        case 7: val_int_out(out, ai < bi); return true;
        case 8: val_int_out(out, ai <= bi); return true;
        case 9: val_int_out(out, ai > bi); return true;
        case 10: val_int_out(out, ai >= bi); return true;
        case 11: val_int_out(out, ai & bi); return true;
        case 12: val_int_out(out, ai | bi); return true;
        case 13: val_int_out(out, ai ^ bi); return true;
        case 14: val_int_out(out, bi >= 0 && bi < 32 ? (int32_t)((uint32_t)ai << bi) : 0); return true;
        case 15: val_int_out(out, ai >> (bi & 31)); return true;
        default:
            snprintf(vm->error, sizeof(vm->error), "binop %u", bop);
            return false;
        }
    }
    if (code == 0x21) {  // UNOP
        if (pos + 1 > extra_size) return false;
        const uint8_t uop = extra[pos];
        val_t a;
        uint16_t a_end = (uint16_t)(pos + 1);
        if (!eval_expr_end(vm, extra, extra_size, (uint16_t)(pos + 1), &a, &a_end,
                           depth + 1)) {
            return false;
        }
        *end = a_end;
        switch (uop) {
        case 0: val_int_out(out, -val_int(&a)); return true;
        case 1: val_int_out(out, val_int(&a)); return true;
        case 2: val_int_out(out, val_truthy(&a) ? 0 : 1); return true;
        case 3: val_int_out(out, ~val_int(&a)); return true;
        default:
            snprintf(vm->error, sizeof(vm->error), "unop %u", uop);
            return false;
        }
    }
    if (code == 0x22) {  // CALL
        return eval_call_real(vm, extra, extra_size, off, out, end, depth);
    }
    if (code == 0x23 || code == 0x24) {  // && / ||
        val_t a, b;
        uint16_t a_end = pos;
        if (!eval_expr_end(vm, extra, extra_size, pos, &a, &a_end, depth + 1)) return false;
        if (code == 0x23) {
            if (!val_truthy(&a)) {
                if (!skip_expr(extra, extra_size, a_end, end, depth + 1)) return false;
                val_int_out(out, 0);
                return true;
            }
            uint16_t b_end = a_end;
            if (!eval_expr_end(vm, extra, extra_size, a_end, &b, &b_end, depth + 1)) {
                return false;
            }
            *end = b_end;
            val_int_out(out, val_truthy(&b) ? 1 : 0);
            return true;
        }
        if (val_truthy(&a)) {
            if (!skip_expr(extra, extra_size, a_end, end, depth + 1)) return false;
            val_int_out(out, 1);
            return true;
        }
        uint16_t b_end = a_end;
        if (!eval_expr_end(vm, extra, extra_size, a_end, out, &b_end, depth + 1)) {
            return false;
        }
        *end = b_end;
        return true;
    }
    snprintf(vm->error, sizeof(vm->error), "expr opcode 0x%02x", code);
    return false;
}

// ---------------------------------------------------------------- assignment
static bool assign_var(clannad_vm_t *vm, uint32_t key, const val_t *value)
{
    clannad_var_t *var = var_find(vm, key, true);
    if (!var) return false;
    if (value->is_str) {
        uint16_t off, len;
        if (!intern_string(vm, value->s, value->len, &off, &len)) return false;
        var->type = 1;
        var->str_off = off;
        var->str_len = len;
        var->value = 0;
    } else {
        var->type = 0;
        var->value = value->i;
        var->str_off = 0;
        var->str_len = 0;
    }
    return true;
}

// ---------------------------------------------------------------- VM core
bool clannad_vm_init(clannad_vm_t *vm, const uint8_t *pack_data, uint32_t pack_size,
                     uint8_t *raw, uint32_t raw_cap, char *why, size_t cap)
{
    if (!vm || !pack_data || !raw) return false;
    memset(vm, 0, sizeof(*vm));
    if (!clannad_pack_open(&vm->pack, pack_data, pack_size)) {
        if (why && cap) snprintf(why, cap, "pack header invalid");
        return false;
    }
    char detail[96];
    if (!clannad_pack_validate(&vm->pack, detail, sizeof(detail))) {
        if (why && cap) snprintf(why, cap, "pack: %s", detail);
        return false;
    }
    const uint32_t max_raw =
        (uint32_t)clannad_pack_meta_int(&vm->pack, "max_raw_block", 32768);
    if (raw_cap < 8192u || raw_cap < max_raw) {
        if (why && cap) snprintf(why, cap, "scratch buffer too small");
        return false;
    }
    vm->raw = raw;
    vm->raw_cap = raw_cap;
    vm->mode = CLAN_MODE_STORY;
    vm->chapter = (uint16_t)clannad_pack_meta_int(&vm->pack, "flow_scene", 414);
    vm->root = vm->chapter;
    vm->background = (uint16_t)clannad_pack_meta_int(&vm->pack, "kuro_asset", 0);
    return true;
}

static void reset_runtime(clannad_vm_t *vm)
{
    vm->var_count = 0;
    memset(vm->vars, 0, sizeof(vm->vars));
    vm->string_used = 0;
    memset(vm->strings, 0, sizeof(vm->strings));
    memset(vm->counters, 0, sizeof(vm->counters));
    vm->stack_depth = 0;
    vm->block_loaded = false;
    vm->bodies[0] = vm->bodies[1] = vm->bodies[2] = 0;
    vm->body_count = 0;
    vm->ended = false;
    vm->error[0] = '\0';
}

bool clannad_vm_new(clannad_vm_t *vm, uint16_t scene_id, uint8_t mode)
{
    if (!vm) return false;
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, scene_id, &index)) return false;
    reset_runtime(vm);
    vm->chapter = scene_id;
    vm->root = scene_id;
    vm->cursor = 0;
    vm->mode = mode;
    vm->background = (uint16_t)clannad_pack_meta_int(&vm->pack, "kuro_asset", 0);
    return true;
}

static clannad_frame_t frame_error(clannad_vm_t *vm, const char *msg)
{
    clannad_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = CLAN_FRAME_ERROR;
    frame.text = msg ? msg : vm->error;
    return frame;
}

static bool jump_scene(clannad_vm_t *vm, uint16_t scene_id, uint8_t entry, bool call)
{
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, scene_id, &index)) {
        snprintf(vm->error, sizeof(vm->error), "jump scene %u missing", scene_id);
        return false;
    }
    clannad_scene_t scene;
    if (!clannad_pack_scene(&vm->pack, index, &scene)) return false;
    if (call) {
        if (vm->stack_depth >= CLAN_VM_STACK_MAX) {
            snprintf(vm->error, sizeof(vm->error), "call stack overflow");
            return false;
        }
        vm->stack[vm->stack_depth].chapter = vm->chapter;
        vm->stack[vm->stack_depth].cursor = (uint16_t)(vm->cursor + 1u);
        vm->stack_depth++;
    }
    if (entry >= scene.entry_count) {
        snprintf(vm->error, sizeof(vm->error), "entry %u out of range", entry);
        return false;
    }
    const uint8_t *ep = vm->pack.entry + (size_t)(scene.entries_index + entry) * 2u;
    vm->chapter = scene_id;
    vm->cursor = clannad_rd16(ep);
    vm->ended = false;
    if (!call && vm->stack_depth == 0) vm->root = scene_id;
    return true;
}

static bool returned(clannad_vm_t *vm, bool *ended)
{
    if (vm->stack_depth) {
        vm->stack_depth--;
        vm->chapter = vm->stack[vm->stack_depth].chapter;
        vm->cursor = vm->stack[vm->stack_depth].cursor;
        *ended = false;
        return true;
    }
    vm->ended = true;
    *ended = true;
    return true;
}

static bool prepare_choice(clannad_vm_t *vm, const clannad_op_t *op, clannad_frame_t *frame)
{
    const uint8_t *extra = block_extra(vm);
    const uint16_t extra_size = vm->block_extra_size;
    if (op->n > CLAN_VM_CHOICES) {
        snprintf(vm->error, sizeof(vm->error), "choice count %u", op->n);
        return false;
    }
    frame->choice_count = op->n;
    if (op->type == CLAN_OP_CHOICE) {
        if ((uint32_t)op->x + 1u + (uint32_t)op->n * 12u > extra_size) return false;
        const uint8_t count = extra[op->x];
        if (count != op->n) return false;
        for (uint8_t i = 0; i < op->n; ++i) {
            const uint8_t *item = extra + op->x + 1u + (uint32_t)i * 12u;
            frame->choice_value[i] = (int32_t)clannad_rd32(item);
            const uint16_t toff = clannad_rd16(item + 8);
            const uint16_t tlen = clannad_rd16(item + 10);
            if ((uint32_t)toff + tlen > vm->block_text_size) {
                snprintf(vm->error, sizeof(vm->error), "choice text range");
                return false;
            }
            if (tlen >= CLAN_VM_CHOICE_TEXT) {
                snprintf(vm->error, sizeof(vm->error), "choice text too long");
                return false;
            }
            memcpy(vm->choice_buf[i], block_text(vm) + toff, tlen);
            vm->choice_buf[i][tlen] = '\0';
            frame->choice_text[i] = vm->choice_buf[i];
        }
        return true;
    }
    // dynamic choice: each label is an expression; value = index
    if ((uint32_t)op->x + 1u > extra_size) return false;
    const uint8_t count = extra[op->x];
    if (count != op->n) return false;
    uint16_t pos = (uint16_t)(op->x + 1u);
    for (uint8_t i = 0; i < op->n; ++i) {
        if ((uint32_t)pos + 2u > extra_size) return false;
        const uint16_t expr = clannad_rd16(extra + pos);
        val_t label;
        if (!eval_expr(vm, extra, extra_size, expr, &label, 0)) return false;
        const char *text = label.is_str ? label.s : "";
        const uint16_t len = label.is_str ? label.len : 0;
        if (len >= CLAN_VM_CHOICE_TEXT) {
            snprintf(vm->error, sizeof(vm->error), "dynamic choice too long");
            return false;
        }
        memcpy(vm->choice_buf[i], text, len);
        vm->choice_buf[i][len] = '\0';
        frame->choice_text[i] = vm->choice_buf[i];
        frame->choice_value[i] = i;
        pos = (uint16_t)(pos + 2u);
    }
    return true;
}

clannad_frame_t clannad_vm_run(clannad_vm_t *vm)
{
    clannad_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    if (!vm) return frame_error(NULL, "vm null");
    for (uint32_t steps = 0; steps < 65536u; ++steps) {
        uint16_t scene_index;
        if (!clannad_pack_find_scene(&vm->pack, vm->chapter, &scene_index)) {
            snprintf(vm->error, sizeof(vm->error), "scene %u missing", vm->chapter);
            return frame_error(vm, NULL);
        }
        clannad_scene_t scene;
        if (!clannad_pack_scene(&vm->pack, scene_index, &scene)) {
            return frame_error(vm, NULL);
        }
        if (vm->cursor >= scene.length) {
            bool ended = false;
            if (!returned(vm, &ended)) return frame_error(vm, NULL);
            if (ended) {
                frame.type = CLAN_FRAME_END;
                frame.text = "";
                return frame;
            }
            continue;
        }
        clannad_op_t op;
        if (!current_op(vm, &op)) return frame_error(vm, NULL);
        switch (op.type) {
        case CLAN_OP_TEXT: {
            if ((uint32_t)op.text_off + op.text_len > vm->block_text_size) {
                snprintf(vm->error, sizeof(vm->error), "text range");
                return frame_error(vm, NULL);
            }
            const char *text = block_text(vm) + op.text_off;
            if (!utf8_valid(text, op.text_len)) {
                snprintf(vm->error, sizeof(vm->error), "text not UTF-8");
                return frame_error(vm, NULL);
            }
            frame.type = CLAN_FRAME_TEXT;
            frame.speaker = clannad_pack_speaker(&vm->pack, op.x);
            frame.text = text;
            return frame;
        }
        case CLAN_OP_CHOICE:
        case CLAN_OP_DYNAMIC_CHOICE: {
            frame.type = CLAN_FRAME_CHOICE;
            if (!prepare_choice(vm, &op, &frame)) return frame_error(vm, NULL);
            return frame;
        }
        case CLAN_OP_SET: {
            const uint8_t *extra = block_extra(vm);
            uint32_t key;
            bool is_var = false;
            if (!eval_var_key(vm, extra, vm->block_extra_size, op.x, &key, &is_var) ||
                !is_var) {
                snprintf(vm->error, sizeof(vm->error), "set target");
                return frame_error(vm, NULL);
            }
            val_t value;
            if (!eval_expr(vm, extra, vm->block_extra_size, op.y, &value, 0)) {
                return frame_error(vm, NULL);
            }
            if (!assign_var(vm, key, &value)) return frame_error(vm, NULL);
            break;
        }
        case CLAN_OP_NATIVE: {
            val_t ignored;
            if (!eval_expr(vm, block_extra(vm), vm->block_extra_size, op.x, &ignored, 0)) {
                return frame_error(vm, NULL);
            }
            break;
        }
        case CLAN_OP_GOTO:
            // 源脚本用 goto 0 回到本场景开头；(0-1) 在 u16 里回绕，随后 cursor++
            // 正好落到 0，与 engine.js 的 `cursor = to - 1; cursor++` 一致。
            if (op.x > scene.length) {
                snprintf(vm->error, sizeof(vm->error), "goto %u", op.x);
                return frame_error(vm, NULL);
            }
            vm->cursor = (uint16_t)(op.x - 1u);
            break;
        case CLAN_OP_IF_FALSE:
        case CLAN_OP_IF_TRUE: {
            val_t cond;
            if (!eval_expr(vm, block_extra(vm), vm->block_extra_size, op.y, &cond, 0)) {
                return frame_error(vm, NULL);
            }
            const bool yes = val_truthy(&cond);
            if ((op.type == CLAN_OP_IF_TRUE) == yes) {
                if (op.x > scene.length) {
                    snprintf(vm->error, sizeof(vm->error), "branch %u", op.x);
                    return frame_error(vm, NULL);
                }
                vm->cursor = (uint16_t)(op.x - 1u);
            }
            break;
        }
        case CLAN_OP_CALL:
            if (vm->stack_depth >= CLAN_VM_STACK_MAX) {
                snprintf(vm->error, sizeof(vm->error), "call stack overflow");
                return frame_error(vm, NULL);
            }
            vm->stack[vm->stack_depth].chapter = vm->chapter;
            vm->stack[vm->stack_depth].cursor = (uint16_t)(vm->cursor + 1u);
            vm->stack_depth++;
            if (op.x > scene.length) {
                snprintf(vm->error, sizeof(vm->error), "call %u", op.x);
                return frame_error(vm, NULL);
            }
            vm->cursor = (uint16_t)(op.x - 1u);
            break;
        case CLAN_OP_JUMP_SCENE:
            if (!jump_scene(vm, op.x, op.n, false)) return frame_error(vm, NULL);
            continue;   // 源引擎跳转后立即返回，不推进游标
        case CLAN_OP_CALL_SCENE:
            if (!jump_scene(vm, op.x, op.n, true)) return frame_error(vm, NULL);
            continue;
        case CLAN_OP_RETURN: {
            bool ended = false;
            if (!returned(vm, &ended)) return frame_error(vm, NULL);
            if (ended) {
                frame.type = CLAN_FRAME_END;
                frame.text = "";
                return frame;
            }
            continue;   // 返回点就是下一条要执行的 op
        }
        case CLAN_OP_END:
            vm->ended = true;
            frame.type = CLAN_FRAME_END;
            frame.text = "";
            return frame;
        case CLAN_OP_BACKGROUND:
            if (op.x >= vm->pack.asset_count) {
                snprintf(vm->error, sizeof(vm->error), "background asset %u", op.x);
                return frame_error(vm, NULL);
            }
            vm->background = op.x;
            if (op.n) {
                vm->body_count = 0;
                vm->bodies[0] = vm->bodies[1] = vm->bodies[2] = 0;
            }
            break;
        case CLAN_OP_BODIES: {
            if (op.n > 3) {
                snprintf(vm->error, sizeof(vm->error), "bodies %u", op.n);
                return frame_error(vm, NULL);
            }
            const uint16_t list[3] = {op.x, op.y, op.z};
            for (uint8_t i = 0; i < 3; ++i) vm->bodies[i] = 0;
            for (uint8_t i = 0; i < op.n; ++i) {
                if (list[i] >= vm->pack.asset_count) {
                    snprintf(vm->error, sizeof(vm->error), "body asset %u", list[i]);
                    return frame_error(vm, NULL);
                }
                vm->bodies[i] = list[i];
            }
            vm->body_count = op.n;
            break;
        }
        case CLAN_OP_BODY_ADD: {
            if (op.x >= vm->pack.asset_count) {
                snprintf(vm->error, sizeof(vm->error), "bodyAdd asset %u", op.x);
                return frame_error(vm, NULL);
            }
            const char *name = clannad_pack_asset_name(&vm->pack, op.x);
            char family[5];
            memcpy(family, name, 4);
            family[4] = '\0';
            int at = -1;
            for (uint8_t i = 0; i < vm->body_count; ++i) {
                const char *other = clannad_pack_asset_name(&vm->pack, vm->bodies[i]);
                if (strncmp(other, family, 4) == 0) {
                    at = i;
                    break;
                }
            }
            if (at >= 0) {
                vm->bodies[at] = op.x;
            } else {
                if (vm->body_count >= 3) {
                    vm->bodies[0] = vm->bodies[1];
                    vm->bodies[1] = vm->bodies[2];
                    vm->body_count = 2;
                }
                vm->bodies[vm->body_count++] = op.x;
            }
            break;
        }
        default:
            snprintf(vm->error, sizeof(vm->error), "op %u", op.type);
            return frame_error(vm, NULL);
        }
        vm->cursor++;
    }
    snprintf(vm->error, sizeof(vm->error), "script stalled");
    return frame_error(vm, NULL);
}

bool clannad_vm_next(clannad_vm_t *vm)
{
    if (!vm) return false;
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, vm->chapter, &index)) return false;
    clannad_scene_t scene;
    if (!clannad_pack_scene(&vm->pack, index, &scene)) return false;
    if (vm->cursor >= scene.length) return false;
    vm->cursor++;
    return true;
}

bool clannad_vm_choose(clannad_vm_t *vm, uint8_t choice_index)
{
    if (!vm) return false;
    clannad_op_t op;
    if (!current_op(vm, &op)) return false;
    if (op.type != CLAN_OP_CHOICE && op.type != CLAN_OP_DYNAMIC_CHOICE) return false;
    if (choice_index >= op.n) return false;
    const uint8_t *extra = block_extra(vm);
    uint32_t key;
    bool is_var = false;
    if (!eval_var_key(vm, extra, vm->block_extra_size, op.y, &key, &is_var) || !is_var) {
        snprintf(vm->error, sizeof(vm->error), "choice target");
        return false;
    }
    val_t value;
    if (op.type == CLAN_OP_CHOICE) {
        if ((uint32_t)op.x + 1u + (uint32_t)choice_index * 12u + 4u > vm->block_extra_size) {
            return false;
        }
        const int32_t item_value =
            (int32_t)clannad_rd32(extra + op.x + 1u + (uint32_t)choice_index * 12u);
        val_int_out(&value, item_value);
    } else {
        val_int_out(&value, choice_index);
    }
    if (!assign_var(vm, key, &value)) return false;
    vm->cursor++;
    return true;
}

bool clannad_vm_var_int(const clannad_vm_t *vm, uint8_t kind, uint32_t index, int32_t *out)
{
    if (!vm) return false;
    const clannad_var_t *var = var_get(vm, var_key(kind, index));
    if (!var) {
        if (out) *out = 0;
        return true;
    }
    if (var->type == 1) {
        if (out) *out = 0;
        return true;
    }
    if (out) *out = var->value;
    return true;
}

bool clannad_vm_var_set_int(clannad_vm_t *vm, uint8_t kind, uint32_t index, int32_t value)
{
    if (!vm) return false;
    val_t v;
    val_int_out(&v, value);
    return assign_var(vm, var_key(kind, index), &v);
}

const char *clannad_vm_chapter_name(const clannad_vm_t *vm)
{
    if (!vm) return "";
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, vm->root, &index)) return "";
    return clannad_pack_scene_name(&vm->pack, index);
}

const char *clannad_vm_scene_source(const clannad_vm_t *vm)
{
    if (!vm) return "";
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, vm->chapter, &index)) return "";
    return clannad_pack_scene_source(&vm->pack, index);
}

const uint8_t *clannad_vm_raw(const clannad_vm_t *vm)
{
    return vm && vm->block_loaded ? vm->raw : NULL;
}

// ---------------------------------------------------------------- save codec
typedef struct {
    uint8_t *out;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

static void w_bytes(writer_t *w, const void *data, size_t len)
{
    if (!w->ok || w->len + len > w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->out + w->len, data, len);
    w->len += len;
}

static void w_u8(writer_t *w, uint8_t v) { w_bytes(w, &v, 1); }
static void w_u16(writer_t *w, uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    w_bytes(w, b, 2);
}
static void w_u32(writer_t *w, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                          (uint8_t)(v >> 24)};
    w_bytes(w, b, 4);
}
static void w_i32(writer_t *w, int32_t v) { w_u32(w, (uint32_t)v); }
static void w_leb(writer_t *w, uint32_t v)
{
    while (v >= 0x80u) {
        w_u8(w, (uint8_t)(v | 0x80u));
        v >>= 7;
    }
    w_u8(w, (uint8_t)v);
}

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool ok;
} reader_t;

static bool r_need(reader_t *r, size_t n)
{
    if (!r->ok || r->pos + n > r->len) {
        r->ok = false;
        return false;
    }
    return true;
}
static uint8_t r_u8(reader_t *r)
{
    if (!r_need(r, 1)) return 0;
    return r->p[r->pos++];
}
static uint16_t r_u16(reader_t *r)
{
    if (!r_need(r, 2)) return 0;
    const uint16_t v = clannad_rd16(r->p + r->pos);
    r->pos += 2;
    return v;
}
static uint32_t r_u32(reader_t *r)
{
    if (!r_need(r, 4)) return 0;
    const uint32_t v = clannad_rd32(r->p + r->pos);
    r->pos += 4;
    return v;
}
static uint32_t r_leb(reader_t *r)
{
    uint32_t value = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {
        const uint8_t b = r_u8(r);
        if (!r->ok) return 0;
        value |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) return value;
        shift += 7;
    }
    r->ok = false;
    return 0;
}

static bool peek_op(clannad_vm_t *vm, uint16_t chapter, uint16_t cursor, clannad_op_t *op)
{
    uint16_t index;
    if (!clannad_pack_find_scene(&vm->pack, chapter, &index)) return false;
    clannad_scene_t scene;
    if (!clannad_pack_scene(&vm->pack, index, &scene)) return false;
    if (cursor >= scene.length) return false;
    const uint16_t block_index =
        (uint16_t)(scene.first_block + cursor / CLAN_BLOCK_SIZE);
    clannad_block_rec_t rec;
    if (!clannad_pack_load_block(&vm->pack, block_index, vm->raw, vm->raw_cap, &rec)) {
        return false;
    }
    const uint16_t ops = clannad_rd16(vm->raw);
    const uint16_t local = (uint16_t)(cursor % CLAN_BLOCK_SIZE);
    if (local >= ops) return false;
    return clannad_pack_block_op(vm->raw, vm->raw_cap, local, op);
}

static const char *vm_current_text(const clannad_vm_t *vm, uint16_t *len)
{
    clannad_vm_t *self = (clannad_vm_t *)vm;
    clannad_op_t op;
    if (current_op(self, &op) && op.type == CLAN_OP_TEXT &&
        (uint32_t)op.text_off + op.text_len <= self->block_text_size) {
        *len = op.text_len;
        return block_text(self) + op.text_off;
    }
    *len = 0;
    return "";
}

size_t clannad_vm_save_encode(const clannad_vm_t *vm, uint16_t page, uint16_t choice_sel,
                              uint32_t saved_at, const char *chapter_name,
                              uint8_t *out, size_t cap)
{
    if (!vm || !out) return 0;
    writer_t w = {.out = out, .cap = cap, .len = 0, .ok = true};
    w_bytes(&w, "CLSV", 4);
    w_u8(&w, 1);
    w_u8(&w, vm->mode);
    uint8_t flags = 0;
    {
        clannad_op_t at;
        clannad_save_info_t dummy;
        (void)dummy;
        if (current_op((clannad_vm_t *)vm, &at) &&
            (at.type == CLAN_OP_CHOICE || at.type == CLAN_OP_DYNAMIC_CHOICE)) {
            flags |= 1;
        }
    }
    w_u8(&w, flags);
    w_u8(&w, 0);
    w_u16(&w, vm->chapter);
    w_u16(&w, vm->root);
    w_u16(&w, vm->cursor);
    w_u16(&w, page);
    w_u16(&w, choice_sel);
    w_u16(&w, vm->background);
    for (int i = 0; i < 3; ++i) w_u16(&w, vm->bodies[i]);
    w_u8(&w, vm->body_count);
    w_u8(&w, vm->stack_depth);
    w_u32(&w, saved_at);
    // sparse variables, sorted by key
    uint16_t order[CLAN_VM_VAR_SLOTS];
    uint16_t count = 0;
    for (uint16_t i = 0; i < CLAN_VM_VAR_SLOTS; ++i) {
        if (vm->vars[i].key) order[count++] = i;
    }
    for (uint16_t i = 1; i < count; ++i) {
        const uint16_t key = order[i];
        int j = i - 1;
        while (j >= 0 && vm->vars[order[j]].key > vm->vars[key].key) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }
    w_u16(&w, count);
    uint32_t prev = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const clannad_var_t *var = &vm->vars[order[i]];
        const uint32_t key = var->key - 1u;
        w_leb(&w, key - prev);
        prev = key;
        w_u8(&w, var->type);
        if (var->type == 1) {
            w_u16(&w, var->str_len);
            w_bytes(&w, vm->strings + var->str_off, var->str_len);
        } else {
            w_i32(&w, var->value);
        }
    }
    uint8_t counter_count = 0;
    for (uint8_t i = 0; i < CLAN_VM_COUNTERS; ++i) {
        if (vm->counters[i].value || vm->counters[i].ticking) counter_count++;
    }
    w_u8(&w, counter_count);
    for (uint8_t i = 0; i < CLAN_VM_COUNTERS; ++i) {
        if (!vm->counters[i].value && !vm->counters[i].ticking) continue;
        w_u8(&w, i);
        w_i32(&w, vm->counters[i].value);
        w_u8(&w, vm->counters[i].ticking ? 1 : 0);
    }
    for (uint8_t i = 0; i < vm->stack_depth; ++i) {
        w_u16(&w, vm->stack[i].chapter);
        w_u16(&w, vm->stack[i].cursor);
    }
    uint16_t text_len = 0;
    const char *text = vm_current_text(vm, &text_len);
    uint16_t preview_len = text_len > CLAN_VM_PREVIEW - 1 ? CLAN_VM_PREVIEW - 1 : text_len;
    while (preview_len && (((uint8_t)text[preview_len] & 0xC0u) == 0x80u)) preview_len--;
    w_u16(&w, preview_len);
    w_bytes(&w, text, preview_len);
    const uint16_t name_len = u8_len(chapter_name);
    w_u16(&w, name_len);
    w_bytes(&w, chapter_name, name_len);
    const uint32_t crc = crc32_of(out, w.len);
    w_u32(&w, crc);
    if (!w.ok) return 0;
    return w.len;
}

typedef struct {
    uint8_t mode;
    uint8_t flags;
    uint8_t body_count;
    uint8_t stack_depth;
    uint16_t chapter, root, cursor, page, choice_sel, background;
    uint16_t bodies[3];
    uint32_t saved_at;
    uint16_t var_count;
    uint8_t counter_count;
} save_head_t;

static bool parse_save_head(reader_t *r, save_head_t *h, clannad_save_info_t *info)
{
    (void)info;
    if (r_u8(r) != 'C' || r_u8(r) != 'L' || r_u8(r) != 'S' || r_u8(r) != 'V') {
        return false;
    }
    if (r_u8(r) != 1) return false;
    h->mode = r_u8(r);
    h->flags = r_u8(r);
    (void)r_u8(r);
    h->chapter = r_u16(r);
    h->root = r_u16(r);
    h->cursor = r_u16(r);
    h->page = r_u16(r);
    h->choice_sel = r_u16(r);
    h->background = r_u16(r);
    for (int i = 0; i < 3; ++i) h->bodies[i] = r_u16(r);
    h->body_count = r_u8(r);
    h->stack_depth = r_u8(r);
    h->saved_at = r_u32(r);
    h->var_count = r_u16(r);
    return r->ok;
}

bool clannad_vm_save_validate(clannad_vm_t *vm, const uint8_t *blob, size_t len,
                              clannad_save_info_t *info)
{
    if (!vm || !blob || len < 16) return false;
    if (crc32_of(blob, len - 4) != clannad_rd32(blob + len - 4)) return false;
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    save_head_t h;
    if (!parse_save_head(&r, &h, info)) return false;
    if (h.mode > CLAN_MODE_CHAPTER) return false;
    if (h.body_count > 3 || h.stack_depth > CLAN_VM_STACK_MAX) return false;
    if (h.var_count > CLAN_VM_VAR_SLOTS) return false;
    if (h.page > 64 || h.choice_sel >= CLAN_VM_CHOICES) return false;
    uint16_t chapter_index;
    if (!clannad_pack_find_scene(&vm->pack, h.chapter, &chapter_index)) return false;
    clannad_scene_t scene;
    if (!clannad_pack_scene(&vm->pack, chapter_index, &scene)) return false;
    if (h.cursor >= scene.length) return false;
    uint16_t root_index;
    if (!clannad_pack_find_scene(&vm->pack, h.root, &root_index)) return false;
    if (h.background >= vm->pack.asset_count) return false;
    for (uint8_t i = 0; i < h.body_count; ++i) {
        if (h.bodies[i] >= vm->pack.asset_count) return false;
    }
    // 变量、字符串、计数器
    uint32_t prev_key = 0;
    for (uint16_t i = 0; i < h.var_count; ++i) {
        const uint32_t delta = r_leb(&r);
        if (!r.ok) return false;
        const uint32_t key = prev_key + delta;
        if (key < prev_key || (i > 0 && delta == 0)) return false;
        prev_key = key;
        const uint8_t kind = (uint8_t)(key >> 20);
        const uint32_t index = key & CLAN_NO_INDEX;
        (void)index;
        if (kind > 14) return false;
        const uint8_t type = r_u8(&r);
        if (type > 1) return false;
        if (type == 1) {
            const uint16_t slen = r_u16(&r);
            if (!r_need(&r, slen)) return false;
            if (!utf8_valid((const char *)r.p + r.pos, slen)) return false;
            r.pos += slen;
        } else {
            (void)r_u32(&r);
        }
    }
    h.counter_count = r_u8(&r);
    if (h.counter_count > CLAN_VM_COUNTERS) return false;
    for (uint8_t i = 0; i < h.counter_count; ++i) {
        const uint8_t slot = r_u8(&r);
        if (slot >= CLAN_VM_COUNTERS) return false;
        (void)r_u32(&r);
        const uint8_t ticking = r_u8(&r);
        if (ticking > 1) return false;
    }
    for (uint8_t i = 0; i < h.stack_depth; ++i) {
        const uint16_t frame_chapter = r_u16(&r);
        const uint16_t frame_cursor = r_u16(&r);
        uint16_t frame_index;
        if (!clannad_pack_find_scene(&vm->pack, frame_chapter, &frame_index)) return false;
        clannad_scene_t frame_scene;
        if (!clannad_pack_scene(&vm->pack, frame_index, &frame_scene)) return false;
        if (frame_cursor > frame_scene.length) return false;
    }
    const uint16_t preview_len = r_u16(&r);
    if (!r_need(&r, preview_len) || !utf8_valid((const char *)r.p + r.pos, preview_len)) {
        return false;
    }
    if (preview_len >= CLAN_VM_PREVIEW) return false;
    r.pos += preview_len;
    const uint16_t name_len = r_u16(&r);
    if (!r_need(&r, name_len) || name_len >= CLAN_VM_NAME) return false;
    if (!utf8_valid((const char *)r.p + r.pos, name_len)) return false;
    r.pos += name_len;
    if (r.pos != r.len) return false;
    // 校验保存点确实是 text/choice；只借用 scratch，不改 VM 的位置。
    clannad_op_t op;
    if (!peek_op(vm, h.chapter, h.cursor, &op)) return false;
    if (op.type != CLAN_OP_TEXT && op.type != CLAN_OP_CHOICE &&
        op.type != CLAN_OP_DYNAMIC_CHOICE) {
        vm->block_loaded = false;
        return false;
    }
    vm->block_loaded = false;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->mode = h.mode;
        info->flags = h.flags;
        info->body_count = h.body_count;
        info->stack_depth = h.stack_depth;
        info->chapter = h.chapter;
        info->root = h.root;
        info->cursor = h.cursor;
        info->page = h.page;
        info->choice_sel = h.choice_sel;
        info->background = h.background;
        memcpy(info->bodies, h.bodies, sizeof(h.bodies));
        info->saved_at = h.saved_at;
        reader_t pr = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
        save_head_t skip;
        if (!parse_save_head(&pr, &skip, NULL)) return false;
        for (uint16_t i = 0; i < h.var_count; ++i) {
            (void)r_leb(&pr);
            const uint8_t type = r_u8(&pr);
            if (type == 1) {
                const uint16_t slen = r_u16(&pr);
                pr.pos += slen;
            } else {
                pr.pos += 4;
            }
        }
        pr.pos += 1;
        for (uint8_t i = 0; i < h.counter_count; ++i) pr.pos += 6;
        pr.pos += (size_t)h.stack_depth * 4u;
        const uint16_t plen = r_u16(&pr);
        if (pr.ok && plen < CLAN_VM_PREVIEW) {
            memcpy(info->preview, pr.p + pr.pos, plen);
            info->preview[plen] = '\0';
            pr.pos += plen;
            const uint16_t nlen = r_u16(&pr);
            if (pr.ok && nlen < CLAN_VM_NAME) {
                memcpy(info->chapter_name, pr.p + pr.pos, nlen);
                info->chapter_name[nlen] = '\0';
            }
        }
    }
    return true;
}

bool clannad_vm_save_apply(clannad_vm_t *vm, const uint8_t *blob, size_t len)
{
    if (!vm || !blob) return false;
    clannad_save_info_t info;
    if (!clannad_vm_save_validate(vm, blob, len, &info)) return false;
    reset_runtime(vm);
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    save_head_t h;
    if (!parse_save_head(&r, &h, NULL)) return false;
    vm->mode = h.mode;
    vm->chapter = h.chapter;
    vm->root = h.root;
    vm->cursor = h.cursor;
    vm->background = h.background;
    vm->body_count = h.body_count;
    memcpy(vm->bodies, h.bodies, sizeof(h.bodies));
    // 重新解析变量并写入
    reader_t vr = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    save_head_t vh;
    if (!parse_save_head(&vr, &vh, NULL)) return false;
    uint32_t prev = 0;
    for (uint16_t i = 0; i < vh.var_count; ++i) {
        const uint32_t key = prev + r_leb(&vr);
        prev = key;
        const uint8_t type = r_u8(&vr);
        val_t value;
        if (type == 1) {
            const uint16_t slen = r_u16(&vr);
            if (!r_need(&vr, slen)) return false;
            if (!val_string(vm, &value, (const char *)vr.p + vr.pos, slen)) return false;
            vr.pos += slen;
        } else {
            val_int_out(&value, (int32_t)r_u32(&vr));
        }
        clannad_var_t *var = var_find(vm, key, true);
        if (!var) return false;
        if (value.is_str) {
            uint16_t off, n;
            if (!intern_string(vm, value.s, value.len, &off, &n)) return false;
            var->type = 1;
            var->str_off = off;
            var->str_len = n;
            var->value = 0;
        } else {
            var->type = 0;
            var->value = value.i;
        }
    }
    const uint8_t counter_count = r_u8(&vr);
    for (uint8_t i = 0; i < counter_count; ++i) {
        const uint8_t slot = r_u8(&vr);
        vm->counters[slot].value = (int32_t)r_u32(&vr);
        vm->counters[slot].ticking = r_u8(&vr) ? 1 : 0;
    }
    for (uint8_t i = 0; i < vh.stack_depth; ++i) {
        vm->stack[i].chapter = r_u16(&vr);
        vm->stack[i].cursor = r_u16(&vr);
    }
    vm->stack_depth = vh.stack_depth;
    vm->block_loaded = false;
    vm->ended = false;
    return vr.ok;
}

// ---------------------------------------------------------------- progress
size_t clannad_vm_progress_encode(const clannad_vm_t *vm, uint8_t *out, size_t cap)
{
    if (!vm || !out) return 0;
    writer_t w = {.out = out, .cap = cap, .len = 0, .ok = true};
    w_bytes(&w, "CLPG", 4);
    w_u8(&w, 1);
    w_u8(&w, 0);
    uint16_t count = 0;
    for (uint16_t i = 0; i < CLAN_VM_VAR_SLOTS; ++i) {
        if (!vm->vars[i].key) continue;
        const uint32_t key = vm->vars[i].key - 1u;
        if ((uint8_t)(key >> 20) == 1) count++;
    }
    if (count > CLAN_VM_PROGRESS_ENTRIES_MAX) return 0;
    w_u16(&w, count);
    for (uint16_t i = 0; i < CLAN_VM_VAR_SLOTS; ++i) {
        if (!vm->vars[i].key) continue;
        const uint32_t key = vm->vars[i].key - 1u;
        if ((uint8_t)(key >> 20) != 1) continue;
        w_u16(&w, (uint16_t)(key & CLAN_NO_INDEX));
        w_i32(&w, vm->vars[i].type == 0 ? vm->vars[i].value : 0);
    }
    const uint32_t crc = crc32_of(out, w.len);
    w_u32(&w, crc);
    return w.ok ? w.len : 0;
}

bool clannad_vm_progress_validate(const uint8_t *blob, size_t len)
{
    if (!blob || len < 12) return false;
    if (memcmp(blob, "CLPG", 4) != 0) return false;
    if (blob[4] != 1) return false;
    if (crc32_of(blob, len - 4) != clannad_rd32(blob + len - 4)) return false;
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    if (r_u8(&r) != 'C' || r_u8(&r) != 'L' || r_u8(&r) != 'P' || r_u8(&r) != 'G') {
        return false;
    }
    if (r_u8(&r) != 1) return false;
    (void)r_u8(&r);
    const uint16_t count = r_u16(&r);
    if (count > CLAN_VM_PROGRESS_ENTRIES_MAX) return false;
    for (uint16_t i = 0; i < count; ++i) {
        (void)r_u16(&r);
        (void)r_u32(&r);
    }
    return r.ok && r.pos == r.len;
}

bool clannad_vm_progress_apply(clannad_vm_t *vm, const uint8_t *blob, size_t len)
{
    if (!vm || !clannad_vm_progress_validate(blob, len)) return false;
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    (void)r_u32(&r);
    (void)r_u8(&r);
    (void)r_u8(&r);
    const uint16_t count = r_u16(&r);
    if (count > CLAN_VM_PROGRESS_ENTRIES_MAX) return false;
    // Preflight only the keys that are actually absent. Existing g[] keys are
    // overlays and must not consume capacity; duplicate progress keys count
    // once (last value wins, matching the old apply order).
    reader_t pre = r;
    uint16_t new_keys[CLAN_VM_PROGRESS_ENTRIES_MAX];
    uint16_t new_count = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t index = r_u16(&pre);
        (void)r_u32(&pre);
        if (!pre.ok) return false;
        const uint32_t key = (1u << 20) | index;
        if (var_find(vm, key, false)) continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < new_count; ++j) {
            if (new_keys[j] == index) { duplicate = true; break; }
        }
        if (!duplicate) new_keys[new_count++] = index;
    }
    if (vm->var_count + new_count >= CLAN_VM_VAR_SLOTS * 3u / 4u) {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t index = r_u16(&r);
        const int32_t value = (int32_t)r_u32(&r);
        if (!clannad_vm_var_set_int(vm, 1, index, value)) return false;
    }
    return r.ok;
}

int32_t clannad_progress_light_count(const uint8_t *blob, size_t len)
{
    if (!blob || !clannad_vm_progress_validate(blob, len)) return 0;
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    (void)r_u32(&r);
    (void)r_u8(&r);
    (void)r_u8(&r);
    const uint16_t count = r_u16(&r);
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t index = r_u16(&r);
        const int32_t value = (int32_t)r_u32(&r);
        if (index == 0) return value;
    }
    return 0;
}

static int32_t progress_value(const uint8_t *blob, size_t len, uint16_t want)
{
    if (!blob || !clannad_vm_progress_validate(blob, len)) return 0;
    reader_t r = {.p = blob, .len = len - 4, .pos = 0, .ok = true};
    (void)r_u32(&r);
    (void)r_u8(&r);
    (void)r_u8(&r);
    const uint16_t count = r_u16(&r);
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t index = r_u16(&r);
        const int32_t value = (int32_t)r_u32(&r);
        if (index == want) return value;
    }
    return 0;
}

bool clannad_progress_after_unlocked(const uint8_t *blob, size_t len)
{
    static const uint16_t BITS[] = {1, 2, 3, 4, 5, 6, 9, 12, 13, 15, 30, 31};
    for (size_t i = 0; i < sizeof(BITS) / sizeof(BITS[0]); ++i) {
        if (progress_value(blob, len, BITS[i]) != 1) return false;
    }
    return true;
}

bool clannad_progress_true_ending(const uint8_t *blob, size_t len)
{
    return progress_value(blob, len, 45) == 1;
}
