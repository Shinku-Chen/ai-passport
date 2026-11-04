// main/gal_model.c —— 剧情虚拟机实现(纯逻辑,不依赖 ESP-IDF/LVGL)。
#include "gal_model.h"

#include <string.h>

#define GAL_MAX_INSTRUCTIONS 20000

bool gal_cond_ok(const gal_play_t *play, const gal_pack_t *pack, uint16_t cond_id)
{
    if (cond_id == GAL_NONE) return true;
    if (cond_id >= pack->cond_count) return false;
    const gal_action_t *c = &pack->conds[cond_id];
    if (c->var >= GAL_MAX_VARS) return false;
    const int16_t lhs = play->vars[c->var];
    const int16_t rhs = c->value;
    switch (c->op) {
    case GAL_CMP_EQ: return lhs == rhs;
    case GAL_CMP_NE: return lhs != rhs;
    case GAL_CMP_GE: return lhs >= rhs;
    case GAL_CMP_GT: return lhs > rhs;
    case GAL_CMP_LE: return lhs <= rhs;
    case GAL_CMP_LT: return lhs < rhs;
    default: return false;
    }
}

static void apply_action(gal_play_t *play, const gal_action_t *a)
{
    if (a->var >= GAL_MAX_VARS) return;
    switch (a->op) {
    case GAL_OP_SET: play->vars[a->var] = a->value; break;
    case GAL_OP_ADD: play->vars[a->var] = (int16_t)(play->vars[a->var] + a->value); break;
    case GAL_OP_SUB: play->vars[a->var] = (int16_t)(play->vars[a->var] - a->value); break;
    default: break;
    }
}

void gal_play_reset(gal_play_t *play)
{
    if (!play) return;
    memset(play, 0, sizeof(*play));
    play->bg = GAL_NONE;
    play->cg = GAL_NONE;
    play->chapter = 0;
    for (int i = 0; i < 3; ++i) play->spr[i] = GAL_NONE;
}

static void clear_sprites(gal_play_t *play)
{
    for (int i = 0; i < 3; ++i) play->spr[i] = GAL_NONE;
}

gal_step_t gal_play_next(gal_play_t *play, const gal_pack_t *pack)
{
    gal_step_t step = { GAL_STEP_ERROR, 0 };
    if (!play || !pack) return step;
    for (int guard = 0; guard < GAL_MAX_INSTRUCTIONS; ++guard) {
        if (play->pc >= pack->event_count) {
            step.kind = GAL_STEP_END;
            return step;
        }
        const gal_event_t *ev = &pack->events[play->pc];
        switch (ev->kind) {
        case GAL_EV_LINE:
            if (ev->b >= pack->line_count) { step.kind = GAL_STEP_ERROR; return step; }
            step.kind = GAL_STEP_LINE;
            step.index = ev->b;
            play->pc++;
            return step;
        case GAL_EV_BG:
            if (ev->b >= pack->image_count) { step.kind = GAL_STEP_ERROR; return step; }
            play->bg = ev->b;
            play->cg = GAL_NONE;
            play->pc++;
            break;
        case GAL_EV_CG:
            if (ev->b >= pack->image_count) { step.kind = GAL_STEP_ERROR; return step; }
            play->cg = ev->b;
            play->pc++;
            break;
        case GAL_EV_SPRITE: {
            const int slot = ev->a;
            if (slot > 2) { step.kind = GAL_STEP_ERROR; return step; }
            if (ev->b != GAL_NONE && ev->b >= pack->image_count) {
                step.kind = GAL_STEP_ERROR;
                return step;
            }
            play->spr[slot] = ev->b;
            play->pc++;
            break;
        }
        case GAL_EV_CHOICE:
            if (ev->b >= pack->choice_count) { step.kind = GAL_STEP_ERROR; return step; }
            step.kind = GAL_STEP_CHOICE;
            step.index = ev->b;
            play->pc++;             // 选择后由 gal_play_choose 覆盖 pc
            return step;
        case GAL_EV_GOTO: {
            const bool take = gal_cond_ok(play, pack, ev->c);
            if (take) {
                if (ev->b >= pack->event_count) { step.kind = GAL_STEP_ERROR; return step; }
                if (ev->a & GAL_GOTO_CLEAR_SPRITES) clear_sprites(play);
                play->pc = ev->b;
            } else {
                play->pc++;
            }
            break;
        }
        case GAL_EV_VAR:
            if (ev->a >= GAL_MAX_VARS) { step.kind = GAL_STEP_ERROR; return step; }
            {
                gal_action_t act = { ev->a, ev->b, (int16_t)ev->c };
                apply_action(play, &act);
            }
            play->pc++;
            break;
        case GAL_EV_CHAPTER:
            if (ev->b >= pack->chapter_count) { step.kind = GAL_STEP_ERROR; return step; }
            play->chapter = ev->b;
            step.kind = GAL_STEP_CHAPTER;
            step.index = ev->b;
            play->pc++;
            return step;
        case GAL_EV_ENDING:
            if (ev->b >= pack->ending_count) { step.kind = GAL_STEP_ERROR; return step; }
            step.kind = GAL_STEP_ENDING;
            step.index = ev->b;
            play->pc++;
            return step;
        case GAL_EV_UNLOCK_END:
            if (ev->b >= pack->ending_count) { step.kind = GAL_STEP_ERROR; return step; }
            step.kind = GAL_STEP_UNLOCK_END;
            step.index = ev->b;
            play->pc++;
            return step;
        case GAL_EV_END:
            step.kind = GAL_STEP_END;
            return step;
        default:
            step.kind = GAL_STEP_ERROR;
            return step;
        }
    }
    step.kind = GAL_STEP_ERROR;
    return step;
}

int gal_choice_visible(const gal_pack_t *pack, const gal_play_t *play, uint16_t group,
                       uint16_t *out, int cap)
{
    if (!pack || !play || !out || cap <= 0 || group >= pack->choice_count) return 0;
    const gal_choice_t *ch = &pack->choices[group];
    int n = 0;
    for (uint8_t i = 0; i < ch->opt_count && n < cap; ++i) {
        const uint16_t global = (uint16_t)(ch->first_opt + i);
        if (global >= pack->option_count) continue;
        if (!gal_cond_ok(play, pack, pack->options[global].cond)) continue;
        out[n++] = global;
    }
    return n;
}

bool gal_choice_pick(gal_play_t *play, const gal_pack_t *pack, uint16_t group,
                     int visible_index)
{
    if (!play || !pack) return false;
    if (play->pc == 0) return false;
    const uint16_t ev_index = (uint16_t)(play->pc - 1);
    if (ev_index >= pack->event_count) return false;
    const gal_event_t *ev = &pack->events[ev_index];
    if (ev->kind != GAL_EV_CHOICE || ev->b != group) return false;
    if (group >= pack->choice_count) return false;
    uint16_t visible[8];
    const int n = gal_choice_visible(pack, play, group, visible, 8);
    if (visible_index < 0 || visible_index >= n) return false;
    const int relative = (int)(visible[visible_index] - pack->choices[group].first_opt);
    return gal_play_choose(play, pack, relative);
}

bool gal_play_choose(gal_play_t *play, const gal_pack_t *pack, int index)
{
    if (!play || !pack) return false;
    if (play->pc == 0) return false;
    const uint16_t ev_index = (uint16_t)(play->pc - 1);
    if (ev_index >= pack->event_count) return false;
    const gal_event_t *ev = &pack->events[ev_index];
    if (ev->kind != GAL_EV_CHOICE || ev->b >= pack->choice_count) return false;
    const gal_choice_t *ch = &pack->choices[ev->b];
    if (index < 0 || index >= (int)ch->opt_count) return false;
    const gal_option_t *opt = &pack->options[ch->first_opt + index];
    if (!gal_cond_ok(play, pack, opt->cond)) return false;
    for (int i = 0; i < opt->act_count; ++i) {
        const uint32_t ai = (uint32_t)opt->act_first + (uint32_t)i;
        if (ai < pack->action_count) apply_action(play, &pack->actions[ai]);
    }
    if (opt->target >= pack->event_count) return false;
    play->pc = opt->target;
    return true;
}

const char *gal_play_line(const gal_play_t *play, const gal_pack_t *pack, uint16_t *speaker)
{
    if (!play || !pack || play->pc == 0) return "";
    const uint16_t ev_index = (uint16_t)(play->pc - 1);
    if (ev_index >= pack->event_count) return "";
    const gal_event_t *ev = &pack->events[ev_index];
    if (ev->kind != GAL_EV_LINE || ev->b >= pack->line_count) return "";
    if (speaker) *speaker = pack->lines[ev->b].speaker;
    return gal_line_text(pack, ev->b);
}

int gal_var_index(const gal_pack_t *pack, const char *name)
{
    if (!pack || !name) return -1;
    // 变量名在 META 里(JSON),运行时不需要解析;这里按已知顺序返回。
    // 打包脚本固定按首次出现顺序编号:true_end_flag, bad_end_flag, true_end_finish。
    if (strcmp(name, "true_end_flag") == 0) return 0;
    if (strcmp(name, "bad_end_flag") == 0) return 1;
    if (strcmp(name, "true_end_finish") == 0) return 2;
    return -1;
}

int16_t gal_var_get(const gal_play_t *play, int index)
{
    if (!play || index < 0 || index >= GAL_MAX_VARS) return 0;
    return play->vars[index];
}

void gal_unlock_clear(gal_unlock_t *u)
{
    if (u) memset(u, 0, sizeof(*u));
}

static bool bit_get(const uint8_t *bits, int bytes, int index)
{
    if (index < 0 || index >= bytes * 8) return false;
    return (bits[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

static void bit_set(uint8_t *bits, int bytes, int index)
{
    if (index < 0 || index >= bytes * 8) return;
    bits[index >> 3] = (uint8_t)(bits[index >> 3] | (uint8_t)(1u << (index & 7)));
}

bool gal_unlock_cg_get(const gal_unlock_t *u, int index) { return u && bit_get(u->cg, GAL_UNLOCK_CG_BYTES, index); }
void gal_unlock_cg_set(gal_unlock_t *u, int index) { if (u) bit_set(u->cg, GAL_UNLOCK_CG_BYTES, index); }
bool gal_unlock_chapter_get(const gal_unlock_t *u, int index) { return u && bit_get(u->chapter, GAL_UNLOCK_CH_BYTES, index); }
void gal_unlock_chapter_set(gal_unlock_t *u, int index) { if (u) bit_set(u->chapter, GAL_UNLOCK_CH_BYTES, index); }
bool gal_unlock_ending_get(const gal_unlock_t *u, int index) { return u && bit_get(u->ending, GAL_UNLOCK_END_BYTES, index); }
void gal_unlock_ending_set(gal_unlock_t *u, int index) { if (u) bit_set(u->ending, GAL_UNLOCK_END_BYTES, index); }

void gal_unlock_merge(gal_unlock_t *dst, const gal_unlock_t *src)
{
    if (!dst || !src) return;
    for (int i = 0; i < GAL_UNLOCK_CG_BYTES; ++i) dst->cg[i] |= src->cg[i];
    for (int i = 0; i < GAL_UNLOCK_CH_BYTES; ++i) dst->chapter[i] |= src->chapter[i];
    for (int i = 0; i < GAL_UNLOCK_END_BYTES; ++i) dst->ending[i] |= src->ending[i];
}

bool gal_unlock_any(const gal_unlock_t *u)
{
    if (!u) return false;
    for (int i = 0; i < GAL_UNLOCK_CG_BYTES; ++i) if (u->cg[i]) return true;
    for (int i = 0; i < GAL_UNLOCK_CH_BYTES; ++i) if (u->chapter[i]) return true;
    for (int i = 0; i < GAL_UNLOCK_END_BYTES; ++i) if (u->ending[i]) return true;
    return false;
}

int gal_unlock_count(const uint8_t *bits, int bytes)
{
    int n = 0;
    if (!bits) return 0;
    for (int i = 0; i < bytes * 8; ++i) if (bit_get(bits, bytes, i)) n++;
    return n;
}

int gal_gallery_index_for_image(const gal_pack_t *pack, uint16_t image)
{
    if (!pack || image == GAL_NONE) return -1;
    for (uint32_t i = 0; i < pack->gallery_count; ++i) {
        if (pack->gallery[i].image == image) return (int)i;
    }
    return -1;
}

int gal_sprite_x(int slot, int w, int screen_w)
{
    int x = (screen_w - w) / 2;
    if (slot == GAL_POS_LEFT) x = 6;
    else if (slot == GAL_POS_RIGHT) x = screen_w - w - 6;
    if (x < 0) x = 0;
    if (x + w > screen_w) x = screen_w - w;
    return x < 0 ? 0 : x;
}

void gal_utf8_truncate(char *dst, const char *src, int max_bytes)
{
    if (!dst || !src || max_bytes <= 0) return;
    int bytes = 0;
    int last = 0;
    while (src[bytes] && bytes < max_bytes) {
        const unsigned char c = (unsigned char)src[bytes];
        int step = 1;
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (bytes + step > max_bytes) break;
        bytes += step;
        last = bytes;
    }
    memcpy(dst, src, (size_t)last);
    dst[last] = 0;
}
