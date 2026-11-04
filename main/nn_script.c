// main/nn_script.c —— 剧本运行时实现（纯逻辑）。
#include "nn_script.h"

#include <string.h>

// 单次 nn_rt_run() 最多执行多少条操作。正常一句对白前面最多几条状态操作；
// 这个上限用来把“跳转成环”变成可见失败，而不是让设备卡死。
#define NN_RUN_STEP_LIMIT 4096

void nn_rt_start(nn_rt_t *rt, const nn_pack_t *pack)
{
    memset(rt, 0, sizeof(*rt));
    rt->pack = pack;
    rt->scene = 0;
    rt->op = 0;
    rt->view.bg = NN_NONE16;
    rt->view.cg = NN_NONE16;
    rt->view.sprite = NN_NONE16;
    rt->speaker = NN_NONE16;
    rt->text = NULL;
    rt->text_len = 0;
    rt->ending_id = 0;
}

bool nn_rt_seek(nn_rt_t *rt, uint16_t scene, uint16_t op, const nn_view_t *view)
{
    if (!rt || !rt->pack) return false;
    if (scene >= rt->pack->scene_count) return false;
    if (op > rt->pack->scenes[scene].op_count) return false;
    rt->scene = scene;
    rt->op = op;
    if (view) rt->view = *view;
    return true;
}

static const nn_op_t *op_at(const nn_rt_t *rt)
{
    const nn_scene_t *s = &rt->pack->scenes[rt->scene];
    if (rt->op >= s->op_count) return NULL;
    return &rt->pack->ops[s->first_op + rt->op];
}

nn_step_t nn_rt_run(nn_rt_t *rt)
{
    if (!rt || !rt->pack) return NN_STEP_BROKEN;
    rt->steps = 0;
    while (true) {
        if (++rt->steps > NN_RUN_STEP_LIMIT) return NN_STEP_BROKEN;
        const nn_op_t *op = op_at(rt);
        if (!op) {
            // 跑到场景末尾而没有跳转/结局：编译期不该出现，运行期报错而非静默续播
            return NN_STEP_BROKEN;
        }
        switch (op->type) {
        case NN_OP_SAY: {
            const char *txt = nn_pack_text(rt->pack, op->b, op->c);
            if (!txt) return NN_STEP_BROKEN;
            rt->text = txt;
            rt->text_len = (uint16_t)op->c;
            rt->speaker = op->a;
            rt->op++;
            return NN_STEP_SAY;
        }
        case NN_OP_BG:
            rt->view.bg = op->a;
            rt->view.cg = NN_NONE16;
            rt->op++;
            break;
        case NN_OP_EV:
            rt->view.cg = op->a;
            rt->op++;
            break;
        case NN_OP_SPRITE:
            rt->view.sprite = op->a;
            rt->op++;
            break;
        case NN_OP_CHAPTER: {
            const char *txt = nn_pack_text(rt->pack, op->b, op->c);
            if (!txt) return NN_STEP_BROKEN;
            rt->view.chapter = txt;
            rt->view.chapter_len = (uint16_t)op->c;
            rt->op++;
            break;
        }
        case NN_OP_FX:
            // blackout：清掉画面，等价于“黑场一瞬”
            rt->view.bg = NN_NONE16;
            rt->view.cg = NN_NONE16;
            rt->view.sprite = NN_NONE16;
            rt->op++;
            break;
        case NN_OP_JUMP:
            if (op->a >= rt->pack->scene_count) return NN_STEP_BROKEN;
            if (op->b > rt->pack->scenes[op->a].op_count) return NN_STEP_BROKEN;
            rt->scene = op->a;
            rt->op = (uint16_t)op->b;
            break;
        case NN_OP_CHOICE:
            if (op->a > rt->pack->choice_count ||
                op->b > rt->pack->choice_count - op->a) return NN_STEP_BROKEN;
            rt->choice_first = (uint16_t)op->a;
            rt->choice_count = (uint8_t)op->b;
            rt->op++;
            return NN_STEP_CHOICE;
        case NN_OP_END:
            if (op->a >= rt->pack->ending_count) return NN_STEP_BROKEN;
            rt->ending_id = (uint8_t)op->a;
            rt->op++;
            return NN_STEP_END;
        default:
            return NN_STEP_BROKEN;
        }
    }
}

bool nn_rt_choose(nn_rt_t *rt, uint8_t option)
{
    if (!rt || !rt->pack) return false;
    if (option >= rt->choice_count) return false;
    const nn_choice_t *c = &rt->pack->choices[rt->choice_first + option];
    if (c->scene >= rt->pack->scene_count) return false;
    if (c->op > rt->pack->scenes[c->scene].op_count) return false;
    rt->scene = c->scene;
    rt->op = c->op;
    rt->choice_count = 0;
    return true;
}

const char *nn_rt_choice_text(const nn_rt_t *rt, uint8_t option, uint16_t *len)
{
    if (!rt || !rt->pack || option >= rt->choice_count) return NULL;
    const nn_choice_t *c = &rt->pack->choices[rt->choice_first + option];
    const char *txt = nn_pack_text(rt->pack, c->text_off, c->text_len);
    if (txt && len) *len = c->text_len;
    return txt;
}

const char *nn_rt_ending_name(const nn_pack_t *pack, uint8_t ending_id, uint16_t *len)
{
    if (!pack || ending_id >= pack->ending_count) return NULL;
    const nn_ending_t *e = &pack->endings[ending_id];
    const char *txt = nn_pack_text(pack, e->name_off, e->name_len);
    if (txt && len) *len = e->name_len;
    return txt;
}

const char *nn_rt_ending_text(const nn_pack_t *pack, uint8_t ending_id, uint16_t *len)
{
    if (!pack || ending_id >= pack->ending_count) return NULL;
    const nn_ending_t *e = &pack->endings[ending_id];
    const char *txt = nn_pack_text(pack, e->text_off, e->text_len);
    if (txt && len) *len = e->text_len;
    return txt;
}
