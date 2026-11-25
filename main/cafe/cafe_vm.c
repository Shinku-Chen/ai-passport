// main/cafe/cafe_vm.c —— 源 engine.js 的 C 等价实现。
#include "cafe_vm.h"

#include "cafe_pack.h"

#include <string.h>

static uint8_t base_flag_count(void)
{
    return (uint8_t)cafe_meta_u32(2);
}

static uint8_t total_flag_count(void)
{
    return (uint8_t)cafe_meta_u32(1);
}

static uint16_t node_count(void)
{
    return (uint16_t)cafe_meta_u32(0);
}

static void remember(cafe_vm_t *vm)
{
    cafe_vm_snapshot_t *s = &vm->history[vm->history_head];
    s->node = vm->node;
    s->index = vm->index;
    s->progress = vm->progress;
    s->progression = vm->progression;
    s->last_complete = vm->last_complete;
    memcpy(s->flags, vm->flags, sizeof(s->flags));
    vm->history_head = (uint8_t)((vm->history_head + 1) % CAFE_VM_HISTORY);
    if (vm->history_count < CAFE_VM_HISTORY) vm->history_count++;
}

static void clear_history(cafe_vm_t *vm)
{
    vm->history_count = 0;
    vm->history_head = 0;
}

void cafe_vm_init(cafe_vm_t *vm, uint32_t permanent_progress)
{
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
    vm->node = 0;
    vm->index = 0;
    vm->progress = permanent_progress & ((1u << CAFE_PROGRESS_BITS) - 1u);
    vm->progression = CAFE_PROGRESSION_STORY;
    vm->last_complete = 0xFF;
}

void cafe_vm_new_game(cafe_vm_t *vm, uint32_t permanent_progress)
{
    cafe_vm_init(vm, permanent_progress);
}

void cafe_vm_checkpoint(const cafe_vm_t *vm, cafe_vm_snapshot_t *out)
{
    if (!vm || !out) return;
    out->node = vm->node;
    out->index = vm->index;
    out->progress = vm->progress;
    out->progression = vm->progression;
    out->last_complete = vm->last_complete;
    memcpy(out->flags, vm->flags, sizeof(out->flags));
}

bool cafe_vm_restore(cafe_vm_t *vm, const cafe_vm_snapshot_t *snap)
{
    cafe_node_t n;
    if (!vm || !snap) return false;
    if (snap->node != CAFE_NODE_ID_NONE && !cafe_node_get(snap->node, &n)) return false;
    if (snap->node != CAFE_NODE_ID_NONE && snap->index > n.frames) return false;
    if (snap->progression > CAFE_PROGRESSION_CHAPTER) return false;
    if (snap->progress & ~((1u << CAFE_PROGRESS_BITS) - 1u)) return false;
    if (snap->last_complete != 0xFF && snap->last_complete >= CAFE_PROGRESS_BITS) return false;
    vm->node = snap->node;
    vm->index = snap->index;
    vm->progress = snap->progress;
    vm->progression = snap->progression;
    vm->last_complete = snap->last_complete;
    memcpy(vm->flags, snap->flags, sizeof(vm->flags));
    clear_history(vm);
    return true;
}

bool cafe_vm_check_any_clear(const cafe_vm_t *vm)
{
    const uint32_t mask = (1u << CAFE_PROG_KANNA) | (1u << CAFE_PROG_NATSUME) |
                          (1u << CAFE_PROG_NOZOMI) | (1u << CAFE_PROG_MEI) |
                          (1u << CAFE_PROG_SUZUNE);
    return (vm->progress & mask) != 0;
}

uint8_t cafe_vm_flag(const cafe_vm_t *vm, uint8_t flag_id)
{
    if (!vm) return 0;
    const uint8_t base = base_flag_count();
    const uint8_t total = total_flag_count();
    if (flag_id < base) return vm->flags[flag_id];
    if (flag_id >= total) return 0;
    // flagRules: 对目标派生 key 累加所有匹配规则。规则表很小(19 条)，
    // 逐条读到失败即可，避免在 flash 元数据里再存一份 count。
    uint16_t sum = 0;
    for (uint16_t i = 0;; ++i) {
        uint8_t key = 0, cmp = 0, value = 0, add = 0;
        if (!cafe_rule_get(i, &key, &cmp, &value, &add)) break;
        if (key == flag_id && cmp < base && vm->flags[cmp] == value) sum = (uint16_t)(sum + add);
    }
    return (uint8_t)sum;
}

bool cafe_vm_matches(const cafe_vm_t *vm, uint16_t when_index)
{
    if (when_index == CAFE_WHEN_NONE) return true;
    cafe_when_t w;
    if (!cafe_when_get(when_index, &w)) return false;
    bool ok = true;
    for (uint8_t i = 0; i < w.count; ++i) {
        const uint8_t flag = w.comp[i][0];
        const uint8_t op = w.comp[i][1];
        const uint8_t value = w.comp[i][2];
        const bool equal = cafe_vm_flag(vm, flag) == value;
        if ((op == 0 && !equal) || (op != 0 && equal)) {
            ok = false;
            break;
        }
    }
    if (ok && w.check_any) ok = cafe_vm_check_any_clear(vm);
    return w.negate ? !ok : ok;
}

static int complete_node(cafe_vm_t *vm, const cafe_node_t *n)
{
    if (vm->progression == CAFE_PROGRESSION_CHAPTER) return 0;
    for (uint8_t i = 0; i < n->complete_count; ++i) {
        uint16_t bit = 0;
        if (!cafe_complete_get((uint16_t)(n->complete_first + i), &bit)) return -1;
        if (bit >= CAFE_PROGRESS_BITS) return -1;
        if (!(vm->progress & (1u << bit))) {
            vm->progress |= 1u << bit;
            vm->last_complete = (uint8_t)bit;
        }
    }
    return 0;
}

int cafe_vm_locate(cafe_vm_t *vm, uint16_t *node_out, uint16_t *index_out,
                   uint16_t *choice_first, uint8_t *choice_count)
{
    if (!vm) return CAFE_LOC_ERROR;
    for (int steps = 0; steps < 1000; ++steps) {
        if (vm->node == CAFE_NODE_ID_NONE) return CAFE_LOC_END;
        if (vm->node >= node_count()) return CAFE_LOC_ERROR;
        cafe_node_t n;
        if (!cafe_node_get(vm->node, &n)) return CAFE_LOC_ERROR;
        if (vm->index < n.frames) {
            uint8_t kind = 0;
            if (!cafe_frame_kind(vm->node, vm->index, &kind)) return CAFE_LOC_ERROR;
            if (node_out) *node_out = vm->node;
            if (index_out) *index_out = vm->index;
            if (choice_first) *choice_first = 0;
            if (choice_count) *choice_count = 0;
            return kind == 1 ? CAFE_LOC_OP : CAFE_LOC_TEXT;
        }
        if (complete_node(vm, &n) != 0) return CAFE_LOC_ERROR;
        if (n.choice_count > 0) {
            if (node_out) *node_out = vm->node;
            if (index_out) *index_out = vm->index;
            if (choice_first) *choice_first = n.choice_first;
            if (choice_count) *choice_count = n.choice_count;
            return CAFE_LOC_CHOICE;
        }
        bool found = false;
        for (uint8_t i = 0; i < n.next_count; ++i) {
            cafe_next_t next;
            if (!cafe_next_get((uint16_t)(n.next_first + i), &next)) return CAFE_LOC_ERROR;
            if (!cafe_vm_matches(vm, next.when)) continue;
            vm->node = next.to;
            vm->index = 0;
            found = true;
            break;
        }
        if (!found) return CAFE_LOC_ERROR;
    }
    return CAFE_LOC_ERROR;
}

int cafe_vm_advance(cafe_vm_t *vm)
{
    const int loc = cafe_vm_locate(vm, NULL, NULL, NULL, NULL);
    if (loc != CAFE_LOC_TEXT && loc != CAFE_LOC_OP) return loc;
    remember(vm);
    if (vm->index < 0xFFFF) vm->index++;
    return cafe_vm_locate(vm, NULL, NULL, NULL, NULL);
}

int cafe_vm_choose(cafe_vm_t *vm, uint8_t option_index)
{
    uint16_t first = 0;
    uint8_t count = 0;
    const int loc = cafe_vm_locate(vm, NULL, NULL, &first, &count);
    if (loc != CAFE_LOC_CHOICE || option_index >= count) return CAFE_LOC_ERROR;
    cafe_choice_t choice;
    if (!cafe_choice_get((uint16_t)(first + option_index), &choice)) return CAFE_LOC_ERROR;
    remember(vm);
    if (choice.flag < total_flag_count()) vm->flags[choice.flag] = choice.value;
    vm->node = choice.to;
    vm->index = 0;
    return cafe_vm_locate(vm, NULL, NULL, NULL, NULL);
}

static const char *node_chapter(uint16_t node)
{
    cafe_node_t n;
    if (!cafe_node_get(node, &n)) return "";
    return cafe_pack_string(n.chapter_off);
}

int cafe_vm_skip_chapter(cafe_vm_t *vm)
{
    uint16_t node = 0;
    uint16_t index = 0;
    const int initial = cafe_vm_locate(vm, &node, &index, NULL, NULL);
    if (initial != CAFE_LOC_TEXT) return initial;
    const char *chapter = node_chapter(node);
    remember(vm);
    for (int steps = 0; steps < 1000; ++steps) {
        cafe_node_t n;
        if (!cafe_node_get(vm->node, &n)) return CAFE_LOC_ERROR;
        // 源实现: 找当前节点内第一个大于 index 的 op；没有则到节点尾。
        uint16_t stop = n.frames;
        // pack 保留了 op 帧；直接扫帧找 op。
        for (uint16_t i = (uint16_t)(vm->index + 1); i < n.frames; ++i) {
            uint8_t kind = 0;
            if (!cafe_frame_kind(vm->node, i, &kind)) return CAFE_LOC_ERROR;
            if (kind == 1) {
                stop = i;
                break;
            }
        }
        vm->index = stop;
        const int loc = cafe_vm_locate(vm, &node, &index, NULL, NULL);
        if (loc != CAFE_LOC_TEXT) return loc;
        if (strcmp(node_chapter(node), chapter) != 0) return loc;
    }
    return CAFE_LOC_ERROR;
}

int cafe_vm_previous(cafe_vm_t *vm)
{
    if (!vm || vm->history_count == 0) return cafe_vm_locate(vm, NULL, NULL, NULL, NULL);
    vm->history_head = (uint8_t)((vm->history_head + CAFE_VM_HISTORY - 1) % CAFE_VM_HISTORY);
    const cafe_vm_snapshot_t *s = &vm->history[vm->history_head];
    vm->node = s->node;
    vm->index = s->index;
    vm->progress = s->progress;
    vm->progression = s->progression;
    vm->last_complete = s->last_complete;
    memcpy(vm->flags, s->flags, sizeof(vm->flags));
    vm->history_count--;
    return cafe_vm_locate(vm, NULL, NULL, NULL, NULL);
}

bool cafe_vm_has_history(const cafe_vm_t *vm)
{
    return vm && vm->history_count > 0;
}
