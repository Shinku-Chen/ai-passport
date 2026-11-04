// main/gal_save.c —— 存档编码/解码。纯 C,宿主机可测故障注入。
#include "gal_save.h"
#include "gal_text.h"

#include <stdio.h>
#include <string.h>

#define SAVE_MAGIC 0x3156534Du   // "MSV1" little endian
#define SAVE_VERSION 2u
#define SAVE_VERSION_LEGACY 1u
#define SAVE_HEADER 12u

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool ok;
} reader_t;

typedef struct {
    uint8_t *p;
    uint8_t *end;
    bool ok;
} writer_t;

static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void w_u8(writer_t *w, uint8_t v)
{
    if (w->ok && w->p < w->end) *w->p++ = v; else w->ok = false;
}
static void w_u16(writer_t *w, uint16_t v)
{
    w_u8(w, (uint8_t)v); w_u8(w, (uint8_t)(v >> 8));
}
static void w_u32(writer_t *w, uint32_t v)
{
    w_u16(w, (uint16_t)v); w_u16(w, (uint16_t)(v >> 16));
}
static void w_i16(writer_t *w, int16_t v) { w_u16(w, (uint16_t)v); }
static void w_f32(writer_t *w, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    w_u32(w, bits);
}

static uint8_t r_u8(reader_t *r)
{
    if (r->ok && r->p < r->end) return *r->p++;
    r->ok = false;
    return 0;
}
static uint16_t r_u16(reader_t *r)
{
    const uint16_t a = r_u8(r);
    const uint16_t b = r_u8(r);
    return (uint16_t)(a | (b << 8));
}
static uint32_t r_u32(reader_t *r)
{
    const uint32_t a = r_u16(r);
    const uint32_t b = r_u16(r);
    return a | (b << 16);
}
static int16_t r_i16(reader_t *r) { return (int16_t)r_u16(r); }
static float r_f32(reader_t *r)
{
    const uint32_t bits = r_u32(r);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static bool valid_image(const gal_pack_t *pack, uint16_t id)
{
    return id == GAL_NONE || id < pack->count[GAL_SEC_IMAGES];
}

static bool valid_str(const gal_pack_t *pack, uint16_t id)
{
    return id == GAL_NONE || id < pack->count[GAL_SEC_STR];
}

bool gal_save_encode(const gal_vm_t *vm, const gal_save_ui_t *ui,
                     uint8_t *out, size_t cap, size_t *out_len)
{
    if (!vm || !ui || !out || !out_len || cap < SAVE_HEADER) return false;
    writer_t w = { out + SAVE_HEADER, out + cap, true };
    w_u8(&w, SAVE_VERSION);
    uint8_t flags = 0;
    if (vm->ended) flags |= 1u;
    if (vm->trial) flags |= 2u;
    if (vm->trial_fail) flags |= 4u;
    if (vm->initial_book) flags |= 8u;
    if (vm->debug_direct) flags |= 16u;
    if (vm->progression_story) flags |= 32u;
    if (vm->legacy_visuals) flags |= 64u;
    if (vm->choice_block) flags |= 128u;
    w_u8(&w, flags);
    w_u8(&w, ui->scene_mode);
    w_u8(&w, ui->page);
    w_u8(&w, ui->choice_row);
    w_u16(&w, vm->chapter);
    w_u16(&w, vm->root);
    w_u16(&w, vm->cursor);
    w_u16(&w, vm->focus);
    w_u16(&w, vm->block_end);
    w_u16(&w, vm->block_continuation);
    w_u16(&w, vm->clue_image);
    w_u16(&w, vm->clue_background);
    w_u16(&w, vm->background);
    w_u16(&w, vm->regular_background);
    w_u16(&w, vm->choices_made);
    w_u16(&w, vm->end_kind);
    w_u8(&w, vm->active_valid ? 1 : 0);

    // 变量:只存与默认值不同的项
    uint16_t var_count = 0;
    const size_t var_count_pos = (size_t)(w.p - out);
    w_u16(&w, 0);
    const uint16_t direct_id = gal_expr_find(vm->pack, "isDebugDirectJump");
    for (uint16_t i = 0; i < vm->var_count; ++i) {
        const gal_var_t *def = gal_pack_var(vm->pack, i);
        if (!def) continue;
        const gal_val_t *v = &vm->vars[i];
        bool same = false;
        if (def->type == GAL_VAL_NUM) {
            float d;
            memcpy(&d, &def->default_bits, sizeof(d));
            same = v->type == GAL_VAL_NUM && v->num == d;
        } else if (def->type == GAL_VAL_BOOL) {
            const bool d = def->default_bits != 0;
            same = v->type == GAL_VAL_BOOL && ((v->num != 0) == d);
        } else {
            same = v->type == GAL_VAL_STR && v->str == GAL_NONE;
        }
        // The source variable is mutable and its decode baseline depends on
        // the old direct flag; always persist its real value (true and false).
        if (same && i != direct_id) continue;
        w_u16(&w, i);
        w_u8(&w, v->type);
        w_u8(&w, 0);
        w_u16(&w, v->type == GAL_VAL_STR ? v->str : GAL_NONE);
        w_f32(&w, v->num);
        var_count++;
    }
    if (w.ok) {
        out[var_count_pos] = (uint8_t)var_count;
        out[var_count_pos + 1] = (uint8_t)(var_count >> 8);
    }

    // 手册:插入顺序 + 版本
    w_u16(&w, vm->book_count);
    for (uint16_t i = 0; i < vm->book_count; ++i) {
        const uint16_t key = vm->book_order[i];
        w_u16(&w, key);
        w_u8(&w, key < GAL_MAX_BOOK ? vm->book[key] : 0);
    }

    // 立绘
    uint8_t sprite_count = 0;
    for (uint8_t i = 0; i < vm->sprite_count; ++i) if (vm->sprites[i].used) sprite_count++;
    w_u8(&w, sprite_count);
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        const gal_sprite_t *s = &vm->sprites[i];
        if (!s->used) continue;
        w_u16(&w, s->actor); w_u16(&w, s->sub); w_u16(&w, s->image);
        w_i16(&w, s->x); w_i16(&w, s->y); w_i16(&w, s->world_y); w_i16(&w, s->scale100);
        w_u16(&w, s->order);
        w_u8(&w, s->visible); w_u8(&w, s->offscreen);
    }

    // 当前画面 bodies(源 checkpoint.bodies)。choice/evidence 页读档不再
    // 依赖 read_frame 重算;完整保存 image/x/y/scale/order/visibility。
    w_u8(&w, vm->body_count);
    for (uint8_t i = 0; i < vm->body_count; ++i) {
        const gal_sprite_t *b = &vm->bodies[i];
        w_u16(&w, b->actor); w_u16(&w, b->sub); w_u16(&w, b->image);
        w_i16(&w, b->x); w_i16(&w, b->y); w_i16(&w, b->world_y); w_i16(&w, b->scale100);
        w_u16(&w, b->order);
        w_u8(&w, b->visible); w_u8(&w, b->offscreen);
    }

    // 背景层
    uint8_t layer_count = 0;
    for (uint8_t i = 0; i < vm->layer_count; ++i) if (vm->layers[i].used) layer_count++;
    w_u8(&w, layer_count);
    for (uint8_t i = 0; i < vm->layer_count; ++i) {
        const gal_layer_t *l = &vm->layers[i];
        if (!l->used) continue;
        w_u16(&w, l->actor); w_u16(&w, l->sub); w_u16(&w, l->image); w_u16(&w, l->order);
        w_u8(&w, l->visible);
    }

    // 调用栈
    w_u8(&w, vm->stack_count);
    for (uint8_t i = 0; i < vm->stack_count; ++i) {
        w_u16(&w, vm->stack[i][0]); w_u16(&w, vm->stack[i][1]);
    }

    // offer 分支
    w_u8(&w, vm->pending_count);
    for (uint8_t i = 0; i < vm->pending_count; ++i) {
        const gal_pending_t *p = &vm->pending[i];
        w_u16(&w, p->text_str); w_u16(&w, p->to); w_u16(&w, p->end); w_u16(&w, p->menu);
    }

    // 选项缓存
    w_u16(&w, vm->active_chapter);
    w_u16(&w, vm->active_cursor);
    w_u8(&w, vm->active_count);
    for (uint8_t i = 0; i < vm->active_count; ++i) {
        const gal_row_t *r = &vm->active_rows[i];
        w_u32(&w, r->text_off); w_u16(&w, r->text_len); w_u16(&w, r->text_str);
        w_u16(&w, r->to); w_u16(&w, r->end); w_u16(&w, r->value); w_u16(&w, r->source_index);
        w_u8(&w, r->kind); w_u8(&w, r->branch); w_u8(&w, r->visited); w_u8(&w, r->text_is_str);
    }

    // 全局解锁位(读档时只合并,不回退)
    for (size_t i = 0; i < sizeof(vm->unlock_gallery); ++i) w_u8(&w, vm->unlock_gallery[i]);
    for (size_t i = 0; i < sizeof(vm->unlock_chapter); ++i) w_u8(&w, vm->unlock_chapter[i]);

    if (!w.ok) return false;
    const size_t payload = (size_t)(w.p - (out + SAVE_HEADER));
    if (SAVE_HEADER + payload > cap) return false;
    memcpy(out, "MSV1", 4);
    out[4] = 0;
    out[5] = 0;
    out[6] = (uint8_t)payload;
    out[7] = (uint8_t)(payload >> 8);
    const uint32_t crc = crc32_le(0, out + SAVE_HEADER, payload);
    out[8] = (uint8_t)crc;
    out[9] = (uint8_t)(crc >> 8);
    out[10] = (uint8_t)(crc >> 16);
    out[11] = (uint8_t)(crc >> 24);
    *out_len = SAVE_HEADER + payload;
    return true;
}

bool gal_save_decode(const uint8_t *data, size_t len, const gal_pack_t *pack,
                     gal_vm_t *vm_out, gal_save_ui_t *ui_out, const char **err)
{
    if (err) *err = NULL;
    if (!data || !pack || !vm_out || !ui_out) { { if (err) *err = "存档参数无效"; return false; } }
    if (len < SAVE_HEADER || memcmp(data, "MSV1", 4) != 0) { { if (err) *err = "存档标识不符"; return false; } }
    const uint16_t payload_len = (uint16_t)(data[6] | (data[7] << 8));
    if (payload_len == 0 || SAVE_HEADER + payload_len != len) { { if (err) *err = "存档长度不符"; return false; } }
    const uint32_t crc = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                         ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    if (crc32_le(0, data + SAVE_HEADER, payload_len) != crc) { { if (err) *err = "存档 CRC 不符"; return false; } }

    reader_t r = { data + SAVE_HEADER, data + len, true };
    const uint8_t version = r_u8(&r);
    if (version != SAVE_VERSION && version != SAVE_VERSION_LEGACY) {
        { if (err) *err = "存档版本不兼容"; return false; }
    }
    const uint8_t flags = r_u8(&r);
    ui_out->scene_mode = r_u8(&r);
    ui_out->page = r_u8(&r);
    ui_out->choice_row = r_u8(&r);
    ui_out->_pad = 0;
    if (ui_out->scene_mode > 2) { { if (err) *err = "存档页面状态非法"; return false; } }

    gal_vm_init(vm_out, pack);
    vm_out->chapter = r_u16(&r);
    vm_out->root = r_u16(&r);
    vm_out->cursor = r_u16(&r);
    vm_out->focus = r_u16(&r);
    vm_out->block_end = r_u16(&r);
    vm_out->block_continuation = r_u16(&r);
    vm_out->clue_image = r_u16(&r);
    vm_out->clue_background = r_u16(&r);
    vm_out->background = r_u16(&r);
    vm_out->regular_background = r_u16(&r);
    vm_out->choices_made = r_u16(&r);
    vm_out->end_kind = r_u16(&r);
    vm_out->active_valid = r_u8(&r) ? 1 : 0;
    if (!r.ok) { { if (err) *err = "存档截断"; return false; } }

    const uint16_t var_count = r_u16(&r);
    if (var_count > vm_out->var_count) { { if (err) *err = "存档变量数非法"; return false; } }
    gal_expr_init(&vm_out->expr, pack, vm_out->vars, vm_out->var_count,
                  version >= SAVE_VERSION ? true : !(flags & 16u));
    for (uint16_t i = 0; i < var_count; ++i) {
        const uint16_t id = r_u16(&r);
        const uint8_t type = r_u8(&r);
        (void)r_u8(&r);
        const uint16_t str = r_u16(&r);
        const float num = r_f32(&r);
        if (!r.ok || id >= vm_out->var_count || type > GAL_VAL_BOOL) {
            { if (err) *err = "存档变量非法"; return false; }
        }
        if (type == GAL_VAL_STR && !valid_str(pack, str)) { { if (err) *err = "存档字符串非法"; return false; } }
        vm_out->vars[id].type = type;
        vm_out->vars[id].num = num;
        vm_out->vars[id].str = type == GAL_VAL_STR ? str : GAL_NONE;
    }

    const uint16_t book_count = r_u16(&r);
    if (book_count > pack->count[GAL_SEC_BOOKKEYS] || book_count > GAL_MAX_BOOK) {
        { if (err) *err = "存档手册条目过多"; return false; }
    }
    memset(vm_out->book, 0, sizeof(vm_out->book));
    vm_out->book_count = 0;
    for (uint16_t i = 0; i < book_count; ++i) {
        const uint16_t key = r_u16(&r);
        const uint8_t version = r_u8(&r);
        if (!r.ok || key >= pack->count[GAL_SEC_BOOKKEYS] || version == 0) {
            { if (err) *err = "存档手册条目非法"; return false; }
        }
        for (uint16_t k = 0; k < vm_out->book_count; ++k) {
            if (vm_out->book_order[k] == key) { { if (err) *err = "存档手册重复"; return false; } }
        }
        vm_out->book[key] = version;
        vm_out->book_order[vm_out->book_count++] = (uint8_t)key;
    }

    const uint8_t sprite_count = r_u8(&r);
    if (sprite_count > GAL_MAX_SPRITES) { { if (err) *err = "存档立绘过多"; return false; } }
    vm_out->sprite_count = sprite_count;
    for (uint8_t i = 0; i < sprite_count; ++i) {
        gal_sprite_t *s = &vm_out->sprites[i];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->actor = r_u16(&r); s->sub = r_u16(&r); s->image = r_u16(&r);
        s->x = r_i16(&r); s->y = r_i16(&r); s->world_y = r_i16(&r); s->scale100 = r_i16(&r);
        s->order = r_u16(&r);
        s->visible = r_u8(&r); s->offscreen = r_u8(&r);
        if (!r.ok || !valid_str(pack, s->actor) || !valid_str(pack, s->sub) ||
            !valid_image(pack, s->image)) {
            { if (err) *err = "存档立绘非法"; return false; }
        }
    }

    if (version >= SAVE_VERSION) {
        const uint8_t body_count = r_u8(&r);
        if (body_count > GAL_MAX_BODY) { { if (err) *err = "存档立绘合成过多"; return false; } }
        vm_out->body_count = body_count;
        for (uint8_t i = 0; i < body_count; ++i) {
            gal_sprite_t *b = &vm_out->bodies[i];
            memset(b, 0, sizeof(*b));
            b->used = 1;
            b->actor = r_u16(&r); b->sub = r_u16(&r); b->image = r_u16(&r);
            b->x = r_i16(&r); b->y = r_i16(&r); b->world_y = r_i16(&r); b->scale100 = r_i16(&r);
            b->order = r_u16(&r);
            b->visible = r_u8(&r); b->offscreen = r_u8(&r);
            if (!r.ok || !valid_str(pack, b->actor) || !valid_str(pack, b->sub) ||
                !valid_image(pack, b->image)) {
                { if (err) *err = "存档立绘合成非法"; return false; }
            }
        }
    } else {
        vm_out->body_count = 0;
    }

    const uint8_t layer_count = r_u8(&r);
    if (layer_count > GAL_MAX_LAYERS) { { if (err) *err = "存档背景过多"; return false; } }
    vm_out->layer_count = layer_count;
    for (uint8_t i = 0; i < layer_count; ++i) {
        gal_layer_t *l = &vm_out->layers[i];
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->actor = r_u16(&r); l->sub = r_u16(&r); l->image = r_u16(&r); l->order = r_u16(&r);
        l->visible = r_u8(&r);
        if (!r.ok || !valid_str(pack, l->actor) || !valid_str(pack, l->sub) ||
            !valid_image(pack, l->image)) {
            { if (err) *err = "存档背景非法"; return false; }
        }
    }

    // 恢复 stage.serial:否则读档后新指令的 order 从 1 重新开始,会压到旧层/立绘下面。
    uint16_t max_order = 0;
    for (uint8_t i = 0; i < vm_out->sprite_count; ++i) {
        if (vm_out->sprites[i].used && vm_out->sprites[i].order > max_order) {
            max_order = vm_out->sprites[i].order;
        }
    }
    for (uint8_t i = 0; i < vm_out->layer_count; ++i) {
        if (vm_out->layers[i].used && vm_out->layers[i].order > max_order) {
            max_order = vm_out->layers[i].order;
        }
    }
    vm_out->serial = max_order;

    const uint8_t stack_count = r_u8(&r);
    if (stack_count > GAL_MAX_STACK) { { if (err) *err = "存档返回栈过深"; return false; } }
    vm_out->stack_count = stack_count;
    for (uint8_t i = 0; i < stack_count; ++i) {
        const uint16_t chapter = r_u16(&r), cursor = r_u16(&r);
        const gal_scene_t *sc = gal_pack_scene(pack, chapter);
        if (!r.ok || !sc || cursor >= sc->op_count) { { if (err) *err = "存档返回位置非法"; return false; } }
        vm_out->stack[i][0] = chapter;
        vm_out->stack[i][1] = cursor;
    }

    const uint8_t pending_count = r_u8(&r);
    if (pending_count > GAL_MAX_PENDING) { { if (err) *err = "存档分支过多"; return false; } }
    vm_out->pending_count = pending_count;
    for (uint8_t i = 0; i < pending_count; ++i) {
        gal_pending_t *p = &vm_out->pending[i];
        p->used = 1;
        p->text_str = r_u16(&r); p->to = r_u16(&r); p->end = r_u16(&r); p->menu = r_u16(&r);
        if (!r.ok || !valid_str(pack, p->text_str)) { { if (err) *err = "存档分支非法"; return false; } }
        const gal_scene_t *psc = gal_pack_scene(pack, vm_out->chapter);
        if (!psc || p->to >= psc->op_count || p->end > psc->op_count) {
            { if (err) *err = "存档分支位置非法"; return false; }
        }
    }

    vm_out->active_chapter = r_u16(&r);
    vm_out->active_cursor = r_u16(&r);
    vm_out->active_count = r_u8(&r);
    if (vm_out->active_count > GAL_MAX_ROWS) { { if (err) *err = "存档选项缓存过大"; return false; } }
    for (uint8_t i = 0; i < vm_out->active_count; ++i) {
        gal_row_t *row = &vm_out->active_rows[i];
        row->text_off = r_u32(&r); row->text_len = r_u16(&r); row->text_str = r_u16(&r);
        row->to = r_u16(&r); row->end = r_u16(&r); row->value = r_u16(&r); row->source_index = r_u16(&r);
        row->kind = r_u8(&r); row->branch = r_u8(&r); row->visited = r_u8(&r); row->text_is_str = r_u8(&r);
        if (!r.ok || row->kind > GAL_ROW_CONTINUE || !valid_str(pack, row->text_str)) {
            { if (err) *err = "存档选项缓存非法"; return false; }
        }
    }

    for (size_t i = 0; i < sizeof(vm_out->unlock_gallery); ++i) vm_out->unlock_gallery[i] = r_u8(&r);
    for (size_t i = 0; i < sizeof(vm_out->unlock_chapter); ++i) vm_out->unlock_chapter[i] = r_u8(&r);
    if (!r.ok || r.p != r.end) { { if (err) *err = "存档尾部非法"; return false; } }

    const gal_scene_t *sc = gal_pack_scene(pack, vm_out->chapter);
    if (!sc || vm_out->cursor >= sc->op_count) { { if (err) *err = "存档位置非法"; return false; } }
    if (vm_out->root >= pack->count[GAL_SEC_SCENES]) { { if (err) *err = "存档章节非法"; return false; } }
    if (!valid_str(pack, vm_out->focus) || !valid_str(pack, vm_out->end_kind)) {
        { if (err) *err = "存档焦点/结局非法"; return false; }
    }
    if (!valid_image(pack, vm_out->clue_image) || !valid_image(pack, vm_out->clue_background) ||
        !valid_image(pack, vm_out->background) || !valid_image(pack, vm_out->regular_background)) {
        { if (err) *err = "存档图片引用非法"; return false; }
    }
    vm_out->ended = (flags & 1u) ? 1 : 0;
    vm_out->trial = (flags & 2u) ? 1 : 0;
    vm_out->trial_fail = (flags & 4u) ? 1 : 0;
    vm_out->initial_book = (flags & 8u) ? 1 : 0;
    vm_out->debug_direct = (flags & 16u) ? 1 : 0;
    vm_out->progression_story = (flags & 32u) ? 1 : 0;
    vm_out->legacy_visuals = (flags & 64u) ? 1 : 0;
    vm_out->choice_block = (flags & 128u) ? 1 : 0;
    vm_out->expr.pack = pack;
    vm_out->expr.vars = vm_out->vars;
    vm_out->expr.var_count = vm_out->var_count;
    vm_out->error = NULL;
    return true;
}

bool gal_save_preview_vm(const gal_pack_t *pack, const gal_vm_t *vm,
                         const gal_save_ui_t *ui, char *out, size_t cap)
{
    if (!pack || !vm || !ui || !out || cap == 0) return false;
    if (ui->scene_mode == 1 || ui->scene_mode == 2) {
        snprintf(out, cap, "%s", ui->scene_mode == 2 ? "正在选择反驳词" : "正在选择下一步");
        return true;
    }
    const gal_scene_t *sc = gal_pack_scene(pack, vm->chapter);
    if (!sc || vm->cursor >= sc->op_count) return false;
    const gal_op_t *op = gal_pack_op(pack, sc->op_start + vm->cursor);
    if (!op) return false;
    char buf[192];
    if ((op->op == GAL_OP_TEXT || op->op == GAL_OP_PAUSE) &&
        gal_pack_text_utf8(pack, op->d, op->b, buf, sizeof(buf)) >= 0) {
        char line[80];
        size_t n = 0;
        for (size_t i = 0; buf[i] && buf[i] != '\n' && n + 1 < sizeof(line); ++i) line[n++] = buf[i];
        line[n] = 0;
        // Source saves.js previews 20 code points plus an ellipsis.
        (void)gal_text_prefix(line, 20, out, cap);
        if (gal_text_cp_count(line) > 20) {
            const size_t have = strlen(out);
            if (have + 4 <= cap) {
                out[have] = (char)0xE2;
                out[have + 1] = (char)0x80;
                out[have + 2] = (char)0xA6;
                out[have + 3] = 0;
            }
        }
        return true;
    }
    snprintf(out, cap, "%s", gal_pack_str(pack, (uint16_t)sc->name));
    return true;
}

bool gal_save_preview(const gal_pack_t *pack, const uint8_t *data, size_t len,
                      gal_vm_t *scratch, char *out, size_t cap)
{
    if (!pack || !data || !scratch || !out || cap == 0) return false;
    gal_save_ui_t ui;
    const char *err = NULL;
    if (!gal_save_decode(data, len, pack, scratch, &ui, &err)) return false;
    return gal_save_preview_vm(pack, scratch, &ui, out, cap);
}
