// main/gal_vm.c —— 剧情虚拟机实现。纯 C,可用宿主机编译跑全路线。
#include "gal_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ERR(const char *msg)
{
    return msg;
}

static bool streq(const gal_pack_t *p, uint16_t a, const char *b)
{
    return strcmp(gal_pack_str(p, a), b) == 0;
}

static const gal_scene_t *cur_scene(const gal_vm_t *vm)
{
    return gal_pack_scene(vm->pack, vm->chapter);
}

static void unlock_chapter_scene(gal_vm_t *vm, uint16_t scene)
{
    for (uint16_t i = 0; i < vm->pack->count[GAL_SEC_CHAPTERS]; ++i) {
        const gal_chapter_t *c = (const gal_chapter_t *)vm->pack->sec[GAL_SEC_CHAPTERS] + i;
        if (c->scene == scene) vm->unlock_chapter[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

static gal_sprite_t *find_sprite(gal_vm_t *vm, uint16_t actor, uint16_t sub)
{
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        gal_sprite_t *s = &vm->sprites[i];
        if (s->used && s->actor == actor && s->sub == sub) return s;
    }
    return NULL;
}

static bool key_is(const gal_vm_t *vm, uint16_t actor, uint16_t sub, const char *key)
{
    const char *a = gal_pack_str(vm->pack, actor);
    if (!key || !a) return false;
    const size_t alen = strlen(a);
    if (sub == GAL_NONE || gal_pack_str(vm->pack, sub)[0] == 0) {
        return strcmp(a, key) == 0;
    }
    if (strncmp(key, a, alen) != 0 || key[alen] != '-') return false;
    return strcmp(key + alen + 1, gal_pack_str(vm->pack, sub)) == 0;
}

static gal_sprite_t *find_sprite_key(gal_vm_t *vm, const char *key)
{
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        gal_sprite_t *s = &vm->sprites[i];
        if (s->used && key_is(vm, s->actor, s->sub, key)) return s;
    }
    return NULL;
}

static gal_layer_t *find_layer(gal_vm_t *vm, uint16_t actor, uint16_t sub)
{
    for (uint8_t i = 0; i < vm->layer_count; ++i) {
        gal_layer_t *l = &vm->layers[i];
        if (l->used && l->actor == actor && l->sub == sub) return l;
    }
    return NULL;
}

static gal_layer_t *add_layer(gal_vm_t *vm, uint16_t actor, uint16_t sub)
{
    if (vm->layer_count >= GAL_MAX_LAYERS) return NULL;
    gal_layer_t *l = &vm->layers[vm->layer_count++];
    memset(l, 0, sizeof(*l));
    l->used = 1;
    l->actor = actor;
    l->sub = sub;
    l->image = GAL_NONE;
    return l;
}

static bool eval_num(gal_vm_t *vm, uint16_t expr, float *out)
{
    if (expr == GAL_NONE) return false;
    gal_val_t v;
    if (!gal_expr_eval(&vm->expr, expr, &v)) return false;
    *out = gal_val_num(&v);
    return true;
}

static void update_offscreen(gal_sprite_t *s)
{
    s->offscreen = (s->world_y < -40 || s->x < -35 || s->x > 135 ||
                    s->y < -45 || s->y > 145 || s->scale100 == 0) ? 1 : 0;
}

static void update_pose(gal_vm_t *vm, gal_sprite_t *s, uint16_t scene_x, uint16_t scene_y,
                        uint16_t world_y, uint16_t scale)
{
    float v;
    if (eval_num(vm, scene_x, &v)) s->x = (int16_t)v;
    if (eval_num(vm, scene_y, &v)) s->y = (int16_t)v;
    if (eval_num(vm, world_y, &v)) s->world_y = (int16_t)v;
    if (eval_num(vm, scale, &v)) s->scale100 = (int16_t)(v * 100.0f);
    update_offscreen(s);
}

static void refresh_background(gal_vm_t *vm)
{
    gal_layer_t *top = NULL, *regular = NULL;
    for (uint8_t i = 0; i < vm->layer_count; ++i) {
        gal_layer_t *l = &vm->layers[i];
        if (!l->used || !l->visible || l->image == GAL_NONE) continue;
        if (!top || l->order > top->order) top = l;
        if (streq(vm->pack, l->actor, "MainBackground") && (!regular || l->order > regular->order)) {
            regular = l;
        }
    }
    vm->regular_background = regular ? regular->image : GAL_NONE;
    vm->background = (vm->clue_image != GAL_NONE) ? vm->clue_image
                     : (top ? top->image : GAL_NONE);
}

static void modify_background(gal_vm_t *vm, const gal_op_t *o)
{
    const uint16_t sub = o->c;
    gal_layer_t *l = find_layer(vm, o->a, sub);
    if (!l) l = add_layer(vm, o->a, sub);
    if (!l) return;
    if (o->b != GAL_NONE) l->image = o->b;
    l->visible = (o->flags & 1) ? 1 : 0;
    l->order = ++vm->serial;
    refresh_background(vm);
}

static void modify_character(gal_vm_t *vm, const gal_op_t *o)
{
    const gal_char_t *c = gal_pack_char(vm->pack, o->a);
    if (!c) return;
    const gal_pose_t *pose = gal_pack_pose(vm->pack, c->pose);
    if (!pose) return;
    gal_sprite_t *s = find_sprite(vm, pose->actor, pose->sub);
    const bool is_new = (s == NULL);
    if (!s) {
        if (vm->sprite_count >= GAL_MAX_SPRITES) {
            vm->error = ERR("立绘状态超过容量");
            return;
        }
        s = &vm->sprites[vm->sprite_count++];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->actor = pose->actor;
        s->sub = pose->sub;
        s->x = 50;
        s->y = 50;
        s->scale100 = 100;
        s->image = GAL_NONE;
    }
    if (pose->image != GAL_NONE) {
        s->image = pose->image;
    } else if (is_new) {
        s->image = pose->default_image;
    }
    s->visible = (o->flags & 1) ? 1 : 0;
    s->order = ++vm->serial;
    update_pose(vm, s, c->scene_x, c->scene_y, c->world_y, c->scale);
}

static void set_scene_visibility(gal_vm_t *vm, uint16_t first, uint16_t count, bool visible)
{
    for (uint16_t n = 0; n < count; ++n) {
        const uint16_t *idp = (const uint16_t *)vm->pack->sec[GAL_SEC_VIS] + first + n;
        const char *key = gal_pack_str(vm->pack, *idp);
        const bool all = key[0] == '*' && key[1] == 0;
        for (uint8_t i = 0; i < vm->sprite_count; ++i) {
            gal_sprite_t *s = &vm->sprites[i];
            if (!s->used) continue;
            bool match = all || key_is(vm, s->actor, s->sub, key);
            if (!match && vm->legacy_visuals && strchr(key, '-')) {
                const size_t alen = strlen(gal_pack_str(vm->pack, s->actor));
                const char *sub = gal_pack_str(vm->pack, s->sub);
                if (sub[0] == 0 && strncmp(key, gal_pack_str(vm->pack, s->actor), alen) == 0 &&
                    key[alen] == '-') {
                    match = true;
                }
            }
            if (match) s->visible = visible ? 1 : 0;
        }
        for (uint8_t i = 0; i < vm->layer_count; ++i) {
            gal_layer_t *l = &vm->layers[i];
            if (!l->used) continue;
            bool match = all || key_is(vm, l->actor, l->sub, key);
            if (match) l->visible = visible ? 1 : 0;
        }
    }
    refresh_background(vm);
}

static void hide_all(gal_vm_t *vm, bool all)
{
    for (uint8_t i = 0; i < vm->sprite_count; ++i) vm->sprites[i].visible = 0;
    if (all) {
        for (uint8_t i = 0; i < vm->layer_count; ++i) vm->layers[i].visible = 0;
        refresh_background(vm);
    }
}

static void apply_moves(gal_vm_t *vm, uint32_t op_index)
{
    const uint16_t mi = gal_pack_move_for_op(vm->pack, op_index);
    if (mi == GAL_NONE) return;
    const gal_move_t *m = gal_pack_move(vm->pack, mi);
    if (!m) return;
    if (m->clear) {
        vm->pending_count = 0;
        vm->active_valid = 0;
    }
    for (uint16_t n = 0; n < m->count; ++n) {
        const gal_move_entry_t *e = gal_pack_move_entry(vm->pack, m->first + n);
        if (!e) continue;
        gal_sprite_t *s = find_sprite_key(vm, gal_pack_str(vm->pack, (uint16_t)e->actor));
        if (!s) continue;
        uint16_t sx = GAL_NONE, sy = GAL_NONE, wy = GAL_NONE, sc = GAL_NONE;
        if (e->kind == GAL_MOVE_X) sx = e->expr;
        else if (e->kind == GAL_MOVE_Y) sy = e->expr;
        else if (e->kind == GAL_MOVE_WORLD_Y) wy = e->expr;
        else sc = e->expr;
        update_pose(vm, s, sx, sy, wy, sc);
    }
}

// JavaScript Array.prototype.sort is stable. The source first sorts by
// order, slices the last three, then sorts those by x. Both comparisons must
// preserve insertion order for equal keys; qsort is not sufficient here.
static void stable_sort_entries(gal_sprite_t *entries, uint8_t n, bool by_x)
{
    for (uint8_t i = 1; i < n; ++i) {
        const gal_sprite_t key = entries[i];
        int j = i;
        while (j > 0) {
            const int a = by_x ? entries[j - 1].x : entries[j - 1].order;
            const int b = by_x ? key.x : key.order;
            if (a <= b) break;
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

// 源 scene.js sceneBodies():决定当前画面显示哪几张立绘。
static void scene_bodies(gal_vm_t *vm, const gal_op_t *text_op)
{
    vm->body_count = 0;
    if (vm->clue_image != GAL_NONE) return;
    gal_sprite_t *entries = vm->scene_entries;
    uint8_t count = 0;
    for (uint8_t i = 0; i < vm->sprite_count; ++i) {
        gal_sprite_t *s = &vm->sprites[i];
        if (!s->used || !s->visible || s->offscreen) continue;
        entries[count++] = *s;
    }
    if (count == 0) return;
    if (vm->trial || (text_op && (text_op->flags & 1))) {
        uint16_t focus = (text_op && text_op->c != GAL_NONE &&
                          gal_pack_str(vm->pack, text_op->c)[0]) ? text_op->c : vm->focus;
        if (focus != GAL_NONE && gal_pack_str(vm->pack, focus)[0] != 0) {
            char creature[64];
            snprintf(creature, sizeof(creature), "Creature%s", gal_pack_str(vm->pack, focus));
            bool has_creature = false;
            for (uint8_t i = 0; i < count; ++i) {
                if (strcmp(gal_pack_str(vm->pack, entries[i].actor), creature) == 0) {
                    has_creature = true;
                    break;
                }
            }
            const char *prefer = has_creature ? creature : gal_pack_str(vm->pack, focus);
            uint8_t n = 0;
            for (uint8_t i = 0; i < count; ++i) {
                if (strcmp(gal_pack_str(vm->pack, entries[i].actor), prefer) == 0) {
                    entries[n++] = entries[i];
                }
            }
            count = n;
        } else if (count > 1) {
            entries[0] = entries[count - 1];
            count = 1;
        }
    } else {
        const gal_layer_t *top = NULL;
        for (uint8_t i = 0; i < vm->layer_count; ++i) {
            const gal_layer_t *l = &vm->layers[i];
            if (!l->used || !l->visible || l->image == GAL_NONE) continue;
            if (!top || l->order > top->order) top = l;
        }
        if (top && streq(vm->pack, top->actor, "Stills")) {
            uint8_t n = 0;
            for (uint8_t i = 0; i < count; ++i) {
                const char *sub = gal_pack_str(vm->pack, entries[i].sub);
                if (sub[0] != 0 && entries[i].order > top->order) entries[n++] = entries[i];
            }
            count = n;
        }
    }
    // 每个 actor 只保留 order 最大的一项。原地压缩,避免再分配一个
    // GAL_MAX_SPRITES 数组(该函数在 text 帧调用,栈帧必须低于 1 KiB)。
    uint8_t unique_count = 0;
    for (uint8_t i = 0; i < count; ++i) {
        bool found = false;
        for (uint8_t j = 0; j < unique_count; ++j) {
            if (entries[j].actor == entries[i].actor) {
                found = true;
                if (entries[i].order > entries[j].order) entries[j] = entries[i];
                break;
            }
        }
        if (!found) entries[unique_count++] = entries[i];
    }
    stable_sort_entries(entries, unique_count, false);
    if (unique_count > GAL_MAX_BODY) {
        memmove(entries, entries + unique_count - GAL_MAX_BODY, GAL_MAX_BODY * sizeof(entries[0]));
        unique_count = GAL_MAX_BODY;
    }
    stable_sort_entries(entries, unique_count, true);
    for (uint8_t i = 0; i < unique_count; ++i) {
        if (entries[i].image == GAL_NONE) continue;
        vm->bodies[vm->body_count++] = entries[i];
    }
}

static bool jump_to(gal_vm_t *vm, uint16_t scene, uint16_t cursor, bool call)
{
    const gal_scene_t *s = gal_pack_scene(vm->pack, scene);
    if (!s || cursor >= s->op_count) return false;
    if (call) {
        if (vm->stack_count >= GAL_MAX_STACK) return false;
        vm->stack[vm->stack_count][0] = vm->chapter;
        vm->stack[vm->stack_count][1] = vm->cursor + 1;
        vm->stack_count++;
    }
    vm->chapter = scene;
    vm->cursor = cursor;
    vm->pending_count = 0;
    vm->choice_block = 0;
    if (!(s->flags & GAL_SCENE_SYSTEM) && !(s->flags & GAL_SCENE_TRIAL_INIT)) {
        vm->root = scene;
        vm->trial = (s->flags & GAL_SCENE_TRIAL) ? 1 : 0;
        unlock_chapter_scene(vm, scene);
    }
    vm->ended = 0;
    vm->scene_epoch++;
    return true;
}

static bool dynamic_jump(gal_vm_t *vm, uint16_t template_str)
{
    const char *tmpl = gal_pack_str(vm->pack, template_str);
    char out[160];
    size_t at = 0;
    for (size_t i = 0; tmpl[i] && at + 1 < sizeof(out); ++i) {
        if (tmpl[i] == '{') {
            const char *close = strchr(tmpl + i + 1, '}');
            if (close) {
                char name[48];
                const size_t n = (size_t)(close - (tmpl + i + 1));
                if (n >= sizeof(name)) return false;
                memcpy(name, tmpl + i + 1, n);
                name[n] = 0;
                const uint16_t id = gal_expr_find(vm->pack, name);
                char value[64] = "";
                if (id != GAL_NONE && id < vm->var_count) {
                    const gal_val_t *v = &vm->vars[id];
                    if (v->type == GAL_VAL_STR && v->str != GAL_NONE) {
                        snprintf(value, sizeof(value), "%s", gal_pack_str(vm->pack, v->str));
                    } else if (v->type == GAL_VAL_BOOL) {
                        snprintf(value, sizeof(value), "%s", v->num != 0 ? "true" : "");
                    } else if (v->num != 0) {
                        snprintf(value, sizeof(value), "%d", (int)v->num);
                    }
                }
                for (size_t k = 0; value[k] && at + 1 < sizeof(out); ++k) out[at++] = value[k];
                i = (size_t)(close - tmpl);
                continue;
            }
        }
        out[at++] = tmpl[i];
    }
    out[at] = 0;
    const char *last = strrchr(out, '/');
    const char *scene_name = last ? last + 1 : out;
    for (uint16_t i = 0; i < vm->pack->count[GAL_SEC_SCENES]; ++i) {
        if (strcmp(gal_pack_str(vm->pack, (uint16_t)gal_pack_scene(vm->pack, i)->id), scene_name) == 0) {
            return jump_to(vm, i, 0, false);
        }
    }
    return false;
}

void gal_vm_init(gal_vm_t *vm, const gal_pack_t *pack)
{
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
    vm->pack = pack;
    vm->var_count = pack ? pack->count[GAL_SEC_VARS] : 0;
    if (vm->var_count > GAL_MAX_VARS) vm->var_count = GAL_MAX_VARS;
    vm->clue_image = GAL_NONE;
    vm->clue_background = GAL_NONE;
    vm->background = GAL_NONE;
    vm->regular_background = GAL_NONE;
    vm->row_count = 0;
    vm->error = NULL;
    vm->expr.pack = pack;
    vm->expr.vars = vm->vars;
    vm->expr.var_count = vm->var_count;
    vm->book_count = 0;
    if (pack) {
        for (uint32_t i = 0; i < pack->count[GAL_SEC_BOOKKEYS] && i < GAL_MAX_BOOK; ++i) {
            vm->book[i] = 0;
        }
    }
}

bool gal_vm_start(gal_vm_t *vm, uint16_t chapter, bool story, bool legacy_visuals,
                  const char **err)
{
    if (err) *err = NULL;
    if (!vm || !vm->pack) { if (err) *err = "虚拟机未初始化"; return false; }
    const gal_scene_t *s = gal_pack_scene(vm->pack, chapter);
    if (!s) { if (err) *err = "场景不存在"; return false; }
    memset(vm->sprites, 0, sizeof(vm->sprites));
    memset(vm->bodies, 0, sizeof(vm->bodies));
    memset(vm->layers, 0, sizeof(vm->layers));
    memset(vm->stack, 0, sizeof(vm->stack));
    memset(vm->pending, 0, sizeof(vm->pending));
    memset(vm->book_order, 0, sizeof(vm->book_order));
    vm->sprite_count = 0;
    vm->body_count = 0;
    vm->layer_count = 0;
    vm->serial = 0;
    vm->stack_count = 0;
    vm->pending_count = 0;
    vm->book_count = 0;
    vm->choices_made = 0;
    vm->active_valid = 0;
    vm->active_count = 0;
    vm->row_count = 0;
    vm->clue_image = GAL_NONE;
    vm->clue_background = GAL_NONE;
    vm->background = GAL_NONE;
    vm->regular_background = GAL_NONE;
    vm->chapter = chapter;
    vm->root = chapter;
    vm->cursor = 0;
    vm->focus = GAL_NONE;
    vm->speaker = GAL_NONE;
    vm->text_actor = GAL_NONE;
    vm->text_off = 0;
    vm->text_len = 0;
    vm->link_first = 0;
    vm->link_count = 0;
    vm->debate = 0;
    vm->end_kind = GAL_NONE;
    vm->ended = 0;
    vm->trial_fail = 0;
    vm->trial = (s->flags & GAL_SCENE_TRIAL) ? 1 : 0;
    vm->initial_book = 1;
    vm->debug_direct = !story;
    vm->progression_story = story ? 1 : 0;
    vm->legacy_visuals = legacy_visuals ? 1 : 0;
    vm->autosave_due = 0;
    vm->choice_block = 0;
    vm->block_end = 0;
    vm->block_continuation = 0;
    vm->error = NULL;
    vm->scene_epoch++;
    memset(vm->unlock_gallery, 0, sizeof(vm->unlock_gallery));
    memset(vm->unlock_chapter, 0, sizeof(vm->unlock_chapter));
    gal_expr_init(&vm->expr, vm->pack, vm->vars, vm->var_count, story);
    unlock_chapter_scene(vm, chapter);
    return true;
}

static uint8_t build_choice_rows(gal_vm_t *vm, const gal_op_t *o, uint8_t *kind_out)
{
    uint8_t n = 0;
    const uint16_t menu = o->c;
    if (kind_out) *kind_out = GAL_ROW_ITEM;
    // 1) offer 收集的分支
    for (uint8_t i = 0; i < vm->pending_count && n < GAL_MAX_ROWS; ++i) {
        const gal_pending_t *p = &vm->pending[i];
        if (!p->used || p->menu != menu) continue;
        gal_row_t *r = &vm->rows[n++];
        memset(r, 0, sizeof(*r));
        r->text_str = p->text_str;
        r->text_is_str = 1;
        r->to = p->to;
        r->end = p->end;
        r->kind = GAL_ROW_BRANCH;
        r->branch = 1;
    }
    if (n) {
        if (kind_out) *kind_out = GAL_ROW_BRANCH;
        return n;
    }
    // 2) 脚本选项
    for (uint16_t i = 0; i < o->b && n < GAL_MAX_ROWS; ++i) {
        const gal_item_t *it = gal_pack_item(vm->pack, o->a + i);
        if (!it || it->lock) continue;
        if (it->when != GAL_NONE) {
            gal_val_t v;
            if (!gal_expr_eval(&vm->expr, it->when, &v) || !gal_val_bool(&v)) continue;
        }
        gal_row_t *r = &vm->rows[n++];
        memset(r, 0, sizeof(*r));
        r->text_off = it->text_off;
        r->text_len = it->text_len;
        r->to = it->to;
        r->end = it->end;
        r->kind = it->branch ? GAL_ROW_BRANCH : GAL_ROW_ITEM;
        r->branch = it->branch;
    }
    if (n) {
        if (kind_out) *kind_out = GAL_ROW_ITEM;
        return n;
    }
    // 3) 同一位置的菜单重开:复用上次选项(源 activeChoices)
    if (vm->active_valid && vm->active_chapter == vm->chapter && vm->active_cursor == vm->cursor) {
        n = vm->active_count;
        memcpy(vm->rows, vm->active_rows, (size_t)n * sizeof(vm->rows[0]));
        if (kind_out && n) *kind_out = vm->rows[0].kind;
        return n;
    }
    return 0;
}

static uint8_t build_map_rows(gal_vm_t *vm, const gal_op_t *o)
{
    uint8_t n = 0;
    int32_t flag = 0;
    if (o->c != GAL_NONE && o->c < vm->var_count) flag = gal_val_int(&vm->vars[o->c]);
    const uint32_t mask = ((uint32_t)1u << (o->b & 31)) - 1u;
    const uint32_t bits = ((uint32_t)flag) & mask;
    // fresh 在前、visited 在后,组内保持源顺序
    for (int pass = 0; pass < 2; ++pass) {
        for (uint16_t i = 0; i < o->b && n < GAL_MAX_ROWS; ++i) {
            const bool visited = (bits >> i) & 1u;
            if ((pass == 0) == visited) continue;
            const uint16_t *aid = (const uint16_t *)vm->pack->sec[GAL_SEC_MAP_AREAS] + o->a + i;
            const gal_area_t *area = gal_pack_area(vm->pack, *aid);
            if (!area) continue;
            gal_row_t *r = &vm->rows[n++];
            memset(r, 0, sizeof(*r));
            r->text_str = (uint16_t)area->name;
            r->text_is_str = 1;
            r->to = (uint16_t)(vm->cursor + 1);
            r->value = *aid;
            r->source_index = i;
            r->visited = visited ? 1 : 0;
            r->kind = GAL_ROW_MAP;
        }
    }
    return n;
}

static bool category_in(const gal_pack_t *p, uint16_t first, uint16_t count, uint8_t cat)
{
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *c = (const uint8_t *)p->sec[GAL_SEC_EVIDENCE_CATS] + first + i;
        if (*c == cat) return true;
    }
    return false;
}

static uint8_t build_evidence_rows(gal_vm_t *vm, const gal_op_t *o)
{
    uint8_t n = 0;
    for (uint16_t k = 0; k < vm->book_count && n < GAL_MAX_ROWS; ++k) {
        const uint16_t key = vm->book_order[k];
        const uint8_t version = vm->book[key];
        if (!version) continue;
        const gal_bookkey_t *bk = gal_pack_bookkey(vm->pack, key);
        if (!bk) continue;
        if (!category_in(vm->pack, o->a, o->b, (uint8_t)bk->category)) continue;
        const gal_book_t *entry = gal_pack_book_find(vm->pack, (uint8_t)bk->category,
                                                     (uint16_t)bk->id, version);
        if (!entry) continue;
        gal_row_t *r = &vm->rows[n++];
        memset(r, 0, sizeof(*r));
        r->text_off = entry->name_off;
        r->text_len = entry->name_len;
        r->to = (uint16_t)(vm->cursor + 1);
        r->value = key;
        r->kind = GAL_ROW_EVIDENCE;
    }
    return n;
}

static bool book_update(gal_vm_t *vm, uint16_t key, uint16_t version)
{
    if (key >= vm->pack->count[GAL_SEC_BOOKKEYS] || key >= GAL_MAX_BOOK) return false;
    if (version == 0) {
        if (vm->book[key]) {
            vm->book[key] = 0;
            for (uint16_t i = 0; i < vm->book_count; ++i) {
                if (vm->book_order[i] == key) {
                    memmove(vm->book_order + i, vm->book_order + i + 1,
                            (size_t)(vm->book_count - i - 1));
                    vm->book_count--;
                    break;
                }
            }
        }
        return true;
    }
    if (!vm->book[key]) {
        if (vm->book_count >= GAL_MAX_BOOK) return false;
        vm->book_order[vm->book_count++] = (uint8_t)key;
    }
    vm->book[key] = (uint8_t)version;
    return true;
}

static void frame_text(gal_vm_t *vm, const gal_op_t *o, gal_frame_t *f)
{
    vm->speaker = o->a;
    vm->text_actor = (o->c != GAL_NONE && gal_pack_str(vm->pack, o->c)[0]) ? o->c : GAL_NONE;
    vm->text_off = o->d;
    vm->text_len = o->b;
    vm->debate = (o->flags & 1) ? 1 : 0;
    const uint32_t op_index = cur_scene(vm)->op_start + vm->cursor;
    const gal_link_op_t *lo = gal_pack_link_for_op(vm->pack, op_index);
    vm->link_first = lo ? lo->first : 0;
    vm->link_count = lo ? lo->count : 0;
    scene_bodies(vm, o);
    f->type = GAL_FRAME_TEXT;
    f->speaker = vm->speaker;
    f->actor = vm->text_actor;
    f->text_off = vm->text_off;
    f->text_len = vm->text_len;
    f->debate = vm->debate;
    f->link_first = vm->link_first;
    f->link_count = vm->link_count;
}

gal_frame_type_t gal_vm_read(gal_vm_t *vm, gal_frame_t *f)
{
    if (!vm || !f) return GAL_FRAME_ERROR;
    memset(f, 0, sizeof(*f));
    f->speaker = GAL_NONE;
    f->actor = GAL_NONE;
    f->end_kind = GAL_NONE;
    f->error = NULL;
    if (!vm->pack) {
        f->type = GAL_FRAME_ERROR;
        f->error = ERR("虚拟机未初始化");
        return f->type;
    }
    if (vm->error) {
        f->type = GAL_FRAME_ERROR;
        f->error = vm->error;
        return f->type;
    }
    if (vm->ended) {
        f->type = GAL_FRAME_END;
        f->end_kind = vm->end_kind;
        return f->type;
    }
    for (int step = 0; step < 96; ++step) {
        if (vm->ended) {
            f->type = GAL_FRAME_END;
            f->end_kind = vm->end_kind;
            return f->type;
        }
        const gal_scene_t *sc = cur_scene(vm);
        if (!sc) {
            f->type = GAL_FRAME_ERROR;
            f->error = ERR("当前场景不存在");
            return f->type;
        }
        if (vm->cursor >= sc->op_count) {
            vm->ended = 1;
            f->type = GAL_FRAME_END;
            f->end_kind = vm->end_kind;
            return f->type;
        }
        if (vm->choice_block && vm->cursor == vm->block_end) {
            vm->cursor = vm->block_continuation;
            vm->choice_block = 0;
            continue;
        }
        const uint32_t op_index = sc->op_start + vm->cursor;
        const gal_op_t *o = gal_pack_op(vm->pack, op_index);
        if (!o) {
            f->type = GAL_FRAME_ERROR;
            f->error = ERR("剧情指令缺失");
            return f->type;
        }
        apply_moves(vm, op_index);

        switch (o->op) {
        case GAL_OP_TEXT:
            frame_text(vm, o, f);
            return f->type;
        case GAL_OP_CHOICE: {
            uint8_t kind = GAL_ROW_ITEM;
            const uint8_t n = build_choice_rows(vm, o, &kind);
            if (n == 0) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("当前选项为空");
                return f->type;
            }
            vm->active_valid = 1;
            vm->active_chapter = vm->chapter;
            vm->active_cursor = vm->cursor;
            vm->active_count = n;
            memcpy(vm->active_rows, vm->rows, (size_t)n * sizeof(vm->rows[0]));
            vm->row_count = n;
            f->type = GAL_FRAME_CHOICE;
            f->choice_kind = kind;
            f->row_count = n;
            return f->type;
        }
        case GAL_OP_MAP: {
            const uint8_t n = build_map_rows(vm, o);
            if (n == 0) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("当前没有可调查的地点");
                return f->type;
            }
            vm->row_count = n;
            f->type = GAL_FRAME_CHOICE;
            f->choice_kind = GAL_ROW_MAP;
            f->row_count = n;
            return f->type;
        }
        case GAL_OP_EVIDENCE: {
            const uint8_t n = build_evidence_rows(vm, o);
            if (n == 0) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("当前没有可提交的证据，请先完成调查");
                return f->type;
            }
            vm->row_count = n;
            f->type = GAL_FRAME_CHOICE;
            f->choice_kind = GAL_ROW_EVIDENCE;
            f->row_count = n;
            return f->type;
        }
        case GAL_OP_EXECUTION: {
            memset(&vm->rows[0], 0, sizeof(vm->rows[0]));
            vm->rows[0].kind = GAL_ROW_CONTINUE;
            vm->rows[0].to = (uint16_t)(vm->cursor + 1);
            vm->rows[0].text_is_str = 1;
            vm->rows[0].text_str = GAL_NONE;
            vm->row_count = 1;
            f->type = GAL_FRAME_CHOICE;
            f->choice_kind = GAL_ROW_CONTINUE;
            f->row_count = 1;
            return f->type;
        }
        case GAL_OP_BG:
            modify_background(vm, o);
            vm->cursor++;
            break;
        case GAL_OP_CHAR:
            modify_character(vm, o);
            if (vm->error) {
                f->type = GAL_FRAME_ERROR;
                f->error = vm->error;
                return f->type;
            }
            vm->cursor++;
            break;
        case GAL_OP_VISIBILITY:
            set_scene_visibility(vm, o->a, o->b, (o->flags & 1) != 0);
            vm->cursor++;
            break;
        case GAL_OP_HIDE_ALL:
            hide_all(vm, (o->flags & 1) != 0);
            vm->cursor++;
            break;
        case GAL_OP_FOCUS:
            vm->focus = o->a;
            vm->cursor++;
            break;
        case GAL_OP_OFFER:
            if (!(o->flags & 1)) {
                const uint16_t menu = o->c;
                const uint16_t to = (uint16_t)(vm->cursor + 1);
                for (uint8_t i = 0; i < vm->pending_count; ++i) {
                    if (vm->pending[i].used && vm->pending[i].menu == menu && vm->pending[i].to == to) {
                        memmove(vm->pending + i, vm->pending + i + 1,
                                (size_t)(vm->pending_count - i - 1) * sizeof(vm->pending[0]));
                        vm->pending_count--;
                        break;
                    }
                }
                if (vm->pending_count < GAL_MAX_PENDING) {
                    gal_pending_t *p = &vm->pending[vm->pending_count++];
                    p->used = 1;
                    p->text_str = (uint16_t)o->d;
                    p->to = to;
                    p->end = o->b;
                    p->menu = menu;
                }
            }
            vm->cursor = o->b;
            break;
        case GAL_OP_IF: {
            gal_val_t v;
            if (!gal_expr_eval(&vm->expr, o->a, &v)) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("条件表达式求值失败");
                return f->type;
            }
            vm->cursor = gal_val_bool(&v) ? (uint16_t)(vm->cursor + 1) : o->b;
            break;
        }
        case GAL_OP_SET: {
            gal_val_t v;
            if (o->a >= vm->var_count || !gal_expr_eval(&vm->expr, o->b, &v)) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("赋值表达式求值失败");
                return f->type;
            }
            vm->vars[o->a] = v;
            vm->cursor++;
            break;
        }
        case GAL_OP_FLAG: {
            if (o->a >= vm->var_count) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("标记变量不存在");
                return f->type;
            }
            int32_t v = gal_val_int(&vm->vars[o->a]);
            v = (v | (int32_t)(1u << (o->b & 31)));
            vm->vars[o->a].type = GAL_VAL_NUM;
            vm->vars[o->a].num = (float)v;
            vm->cursor++;
            break;
        }
        case GAL_OP_JUMP:
        case GAL_OP_CALL:
            if (!jump_to(vm, o->a, o->b, o->op == GAL_OP_CALL)) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("剧情跳转失败");
                return f->type;
            }
            break;
        case GAL_OP_RESET_BOOK: {
            const uint16_t direct_id = gal_expr_find(vm->pack, "isDebugDirectJump");
            bool direct = false;
            if (direct_id != GAL_NONE && direct_id < vm->var_count) {
                const gal_val_t *v = &vm->vars[direct_id];
                direct = v->type == GAL_VAL_STR ? v->str != GAL_NONE : v->num != 0.0f;
            }
            if (!(o->flags & 1) || vm->initial_book || direct) {
                vm->initial_book = 0;
                if (!jump_to(vm, o->a, o->b, true)) {
                    f->type = GAL_FRAME_ERROR;
                    f->error = ERR("手册重置跳转失败");
                    return f->type;
                }
            } else {
                vm->cursor++;
            }
            break;
        }
        case GAL_OP_RETURN:
            if (vm->stack_count == 0) {
                vm->cursor++;
            } else {
                vm->stack_count--;
                vm->chapter = vm->stack[vm->stack_count][0];
                vm->cursor = vm->stack[vm->stack_count][1];
                vm->pending_count = 0;
                vm->scene_epoch++;
            }
            break;
        case GAL_OP_CANCEL_CALL:
            if (vm->stack_count) vm->stack_count--;
            vm->cursor++;
            break;
        case GAL_OP_BOOK_CLEAR:
            memset(vm->book, 0, sizeof(vm->book));
            vm->book_count = 0;
            vm->cursor++;
            break;
        case GAL_OP_BOOK_UPDATE:
            if (!book_update(vm, o->a, o->b)) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("手册条目更新失败");
                return f->type;
            }
            vm->cursor++;
            break;
        case GAL_OP_RECORD_CHOICE:
            vm->choices_made++;
            vm->cursor++;
            break;
        case GAL_OP_DAMAGE: {
            const uint16_t hp_id = gal_vm_var_id(vm, "currentHp");
            int32_t hp = hp_id != GAL_NONE ? gal_val_int(&vm->vars[hp_id]) : 10;
            if (hp == 0) hp = 10;
            hp -= 1;
            if (hp_id != GAL_NONE) {
                vm->vars[hp_id].type = GAL_VAL_NUM;
                vm->vars[hp_id].num = (float)hp;
            }
            if (hp <= 0) {
                // engine.js:74 does not advance cursor on a fatal damage;
                // endingKind is the literal 'trialFail'.
                uint16_t kind = GAL_NONE;
                for (uint16_t si = 0; si < vm->pack->count[GAL_SEC_STR]; ++si) {
                    if (strcmp(gal_pack_str(vm->pack, si), "trialFail") == 0) {
                        kind = si;
                        break;
                    }
                }
                vm->ended = 1;
                vm->trial_fail = 1;
                vm->end_kind = kind;
                f->type = GAL_FRAME_END;
                f->end_kind = kind;
                return f->type;
            }
            vm->cursor++;
            break;
        }
        case GAL_OP_CLUE_IMAGE:
            if (o->a != GAL_NONE) {
                vm->clue_background = vm->background;
                vm->clue_image = o->a;
                vm->background = o->a;
                vm->body_count = 0;
            }
            vm->cursor++;
            break;
        case GAL_OP_CLUE_HIDE:
            vm->clue_image = GAL_NONE;
            vm->background = (vm->clue_background != GAL_NONE) ? vm->clue_background
                             : vm->regular_background;
            vm->clue_background = GAL_NONE;
            vm->cursor++;
            break;
        case GAL_OP_AUTOSAVE:
            vm->autosave_due = 1;
            vm->cursor++;
            break;
        case GAL_OP_PAUSE:
            vm->speaker = GAL_NONE;
            vm->text_actor = GAL_NONE;
            vm->text_off = o->d;
            vm->text_len = o->b;
            vm->debate = 0;
            vm->link_first = vm->link_count = 0;
            f->type = GAL_FRAME_TEXT;
            f->speaker = vm->speaker;
            f->actor = vm->text_actor;
            f->text_off = vm->text_off;
            f->text_len = vm->text_len;
            return f->type;
        case GAL_OP_STOP:
        case GAL_OP_END:
            vm->ended = 1;
            vm->end_kind = o->a;
            f->type = GAL_FRAME_END;
            f->end_kind = vm->end_kind;
            return f->type;
        case GAL_OP_DYNAMIC_JUMP:
            if (!dynamic_jump(vm, o->a)) {
                f->type = GAL_FRAME_ERROR;
                f->error = ERR("动态剧情跳转失败");
                return f->type;
            }
            break;
        default:
            f->type = GAL_FRAME_ERROR;
            f->error = ERR("未知剧情指令");
            return f->type;
        }
    }
    f->type = GAL_FRAME_YIELD;
    return f->type;
}

void gal_vm_advance(gal_vm_t *vm)
{
    if (!vm || vm->ended) return;
    vm->cursor++;
    vm->link_count = 0;
    vm->debate = 0;
}

bool gal_vm_choose(gal_vm_t *vm, uint8_t row, const char **err)
{
    if (err) *err = NULL;
    if (!vm || row >= vm->row_count) { if (err) *err = "选项不存在"; return false; }
    const gal_row_t r = vm->rows[row];
    const gal_scene_t *sc = cur_scene(vm);
    if (!sc || r.to >= sc->op_count) { if (err) *err = "选项目标越界"; return false; }
    vm->choices_made++;
    vm->pending_count = 0;
    if (r.kind == GAL_ROW_MAP) {
        const gal_area_t *a = gal_pack_area(vm->pack, r.value);
        const uint16_t id = gal_vm_var_id(vm, "chosenAreaId");
        if (a && id != GAL_NONE) {
            vm->vars[id].type = GAL_VAL_STR;
            vm->vars[id].num = 0;
            vm->vars[id].str = (uint16_t)a->id;
        }
    } else if (r.kind == GAL_ROW_EVIDENCE) {
        const gal_bookkey_t *bk = gal_pack_bookkey(vm->pack, r.value);
        const uint16_t id = gal_vm_var_id(vm, "chosenEvidenceId");
        if (bk && id != GAL_NONE) {
            vm->vars[id].type = GAL_VAL_STR;
            vm->vars[id].num = 0;
            vm->vars[id].str = (uint16_t)bk->id;
        }
    }
    if (r.branch) {
        vm->choice_block = 1;
        vm->block_end = r.end;
        vm->block_continuation = (uint16_t)(vm->cursor + 1);
    }
    vm->cursor = r.to;
    vm->ended = 0;
    vm->link_count = 0;
    vm->debate = 0;
    return true;
}

bool gal_vm_object_to(gal_vm_t *vm, uint8_t link, const char **err)
{
    if (err) *err = NULL;
    if (!vm || link >= vm->link_count) { if (err) *err = "反驳位置无效"; return false; }
    const gal_link_t *l = gal_pack_link(vm->pack, (uint16_t)(vm->link_first + link));
    if (!l) { if (err) *err = "反驳目标不存在"; return false; }
    if (!jump_to(vm, l->scene, l->cursor, true)) {
        if (err) *err = "反驳跳转失败";
        return false;
    }
    return true;
}

const char *gal_vm_frame_text(const gal_vm_t *vm, const gal_frame_t *frame,
                              char *buf, size_t cap)
{
    if (!vm || !frame || !buf || cap == 0) return "";
    if (gal_pack_text_utf8(vm->pack, frame->text_off, frame->text_len, buf, cap) < 0) {
        snprintf(buf, cap, "%s", "");
    }
    return buf;
}

const char *gal_vm_row_text(const gal_vm_t *vm, const gal_row_t *row, char *buf, size_t cap)
{
    if (!vm || !row || !buf || cap == 0) return "";
    if (row->text_is_str) {
        snprintf(buf, cap, "%s", row->text_str == GAL_NONE ? "" : gal_pack_str(vm->pack, row->text_str));
        return buf;
    }
    if (gal_pack_text_utf8(vm->pack, row->text_off, row->text_len, buf, cap) < 0) {
        snprintf(buf, cap, "%s", "");
    }
    return buf;
}

uint16_t gal_vm_book_key_count(const gal_vm_t *vm)
{
    return vm ? vm->book_count : 0;
}

uint8_t gal_vm_book_version(const gal_vm_t *vm, uint16_t key)
{
    if (!vm || key >= GAL_MAX_BOOK) return 0;
    return vm->book[key];
}

const gal_book_t *gal_vm_book_entry(const gal_vm_t *vm, uint16_t key)
{
    if (!vm || key >= vm->book_count) return NULL;
    const uint16_t k = vm->book_order[key];
    const uint8_t version = vm->book[k];
    if (!version) return NULL;
    const gal_bookkey_t *bk = gal_pack_bookkey(vm->pack, k);
    if (!bk) return NULL;
    return gal_pack_book_find(vm->pack, (uint8_t)bk->category, (uint16_t)bk->id, version);
}

const gal_area_t *gal_vm_map_area(const gal_vm_t *vm, uint16_t op_a, uint16_t index)
{
    if (!vm) return NULL;
    const uint16_t *aid = (const uint16_t *)vm->pack->sec[GAL_SEC_MAP_AREAS] + op_a + index;
    return gal_pack_area(vm->pack, *aid);
}

bool gal_vm_gallery_unlocked(const gal_vm_t *vm, uint16_t item)
{
    if (!vm || item >= vm->pack->count[GAL_SEC_GALLERY_ITEMS]) return false;
    return (vm->unlock_gallery[item >> 3] & (1u << (item & 7))) != 0;
}

bool gal_vm_chapter_unlocked(const gal_vm_t *vm, uint16_t entry)
{
    if (!vm || entry >= vm->pack->count[GAL_SEC_CHAPTERS]) return false;
    return (vm->unlock_chapter[entry >> 3] & (1u << (entry & 7))) != 0;
}

void gal_vm_merge_unlock(gal_vm_t *vm, const uint8_t *gallery, const uint8_t *chapters)
{
    if (!vm) return;
    if (gallery) for (size_t i = 0; i < sizeof(vm->unlock_gallery); ++i) vm->unlock_gallery[i] |= gallery[i];
    if (chapters) for (size_t i = 0; i < sizeof(vm->unlock_chapter); ++i) vm->unlock_chapter[i] |= chapters[i];
}

uint16_t gal_vm_var_id(const gal_vm_t *vm, const char *name)
{
    return vm ? gal_expr_find(vm->pack, name) : GAL_NONE;
}

int32_t gal_vm_var_int(const gal_vm_t *vm, const char *name)
{
    if (!vm) return 0;
    const uint16_t id = gal_expr_find(vm->pack, name);
    if (id == GAL_NONE || id >= vm->var_count) return 0;
    return gal_val_int(&vm->vars[id]);
}
