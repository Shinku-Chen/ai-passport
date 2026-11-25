// Interpreter of the already-converted Watch graph, not the original PC VM.
#include "nine_vm.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool has(const uint8_t *bits, uint16_t key)
{
    return (bits[key / 8] & (uint8_t)(1u << (key % 8))) != 0;
}
static void mark(uint8_t *bits, uint16_t key)
{
    bits[key / 8] |= (uint8_t)(1u << (key % 8));
}
static const nine_view_t *error(nine_vm_t *v, const char *s)
{
    memset(&v->view, 0, sizeof(v->view));
    v->view.kind = NINE_VM_ERROR;
    v->view.error = s;
    return &v->view;
}

// ---------------------------------------------------------------- history
static uint32_t hist_entry_size(const nine_vm_t *v)
{
    // 9-byte header + at most key_count run and key_count progress pairs.
    uint32_t n = 9u + 12u * (uint32_t)v->pack->key_count;
    return (n + 3u) & ~3u;
}
static uint8_t *hist_at(const nine_vm_t *v, unsigned i)
{
    return v->history + (size_t)i * v->history_entry;
}
static void hist_put16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static uint16_t hist_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void hist_put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}
static uint32_t hist_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void remember(nine_vm_t *v)
{
    if (!v->history || !v->history_entry) return;
    if (v->history_count == NINE_HISTORY_MAX) {
        memmove(v->history, v->history + v->history_entry,
                (NINE_HISTORY_MAX - 1u) * v->history_entry);
        v->history_count--;
    }
    uint8_t *e = hist_at(v, v->history_count);
    memset(e, 0, v->history_entry);
    hist_put16(e, v->state.node);
    hist_put32(e + 2, v->state.index);
    e[6] = v->state.preview ? 1 : 0;
    uint8_t *p = e + 9;
    uint16_t n = 0;
    for (uint16_t k = 0; k < v->pack->key_count; ++k) {
        if (!has(v->state.flag_set, k)) continue;
        hist_put16(p, k);
        hist_put32(p + 2, (uint32_t)v->state.flag[k]);
        p += 6;
        n++;
    }
    e[7] = (uint8_t)n;
    n = 0;
    for (uint16_t k = 0; k < v->pack->key_count; ++k) {
        if (!has(v->state.progress_set, k)) continue;
        hist_put16(p, k);
        hist_put32(p + 2, (uint32_t)v->state.progress[k]);
        p += 6;
        n++;
    }
    e[8] = (uint8_t)n;
    v->history_count++;
}

static bool restore_checkpoint(nine_vm_t *v, const uint8_t *e)
{
    uint16_t node = hist_get16(e);
    uint32_t index = hist_get32(e + 2);
    uint16_t nrun = e[7], nprog = e[8];
    if (node != NINE_NODE_NONE) {
        nine_node_t n;
        if (!nine_pack_node(v->pack, node, &n) || index > n.frame_count) return false;
    } else if (index) {
        return false;
    }
    // Keep the newest permanent progress: going back may never revoke an ending.
    uint8_t current[NINE_KEY_MAX / 8];
    int32_t current_values[NINE_KEY_MAX];
    memcpy(current, v->state.progress_set, sizeof(current));
    memcpy(current_values, v->state.progress, sizeof(current_values));
    memset(v->state.flag, 0, sizeof(v->state.flag));
    memset(v->state.flag_set, 0, sizeof(v->state.flag_set));
    memset(v->state.progress, 0, sizeof(v->state.progress));
    memset(v->state.progress_set, 0, sizeof(v->state.progress_set));
    const uint8_t *p = e + 9;
    for (uint16_t i = 0; i < nrun; ++i, p += 6) {
        uint16_t k = hist_get16(p);
        if (k >= v->pack->key_count) return false;
        v->state.flag[k] = (int32_t)hist_get32(p + 2);
        mark(v->state.flag_set, k);
    }
    for (uint16_t i = 0; i < nprog; ++i, p += 6) {
        uint16_t k = hist_get16(p);
        if (k >= v->pack->key_count) return false;
        v->state.progress[k] = (int32_t)hist_get32(p + 2);
        mark(v->state.progress_set, k);
    }
    for (uint16_t k = 0; k < v->pack->key_count; ++k) {
        if (has(current, k)) {
            v->state.progress[k] = current_values[k];
            mark(v->state.progress_set, k);
        }
    }
    v->state.node = node;
    v->state.index = index;
    v->state.preview = e[6] != 0;
    return true;
}

// ---------------------------------------------------------------- init
bool nine_vm_init(nine_vm_t *v, nine_pack_t *p)
{
    if (!v || !p || !p->data) return false;
    memset(v, 0, sizeof(*v));
    v->pack = p;
    v->state.node = p->start;
    v->history_entry = hist_entry_size(v);
    size_t bytes = (size_t)v->history_entry * NINE_HISTORY_MAX;
    v->history = (uint8_t *)malloc(bytes);
    if (!v->history) {
        // The story remains fully playable; only "previous line" is unavailable.
        v->history_entry = 0;
    }
    return true;
}

void nine_vm_deinit(nine_vm_t *v)
{
    if (!v) return;
    free(v->history);
    v->history = NULL;
    v->history_entry = 0;
    v->history_count = 0;
}

int32_t nine_vm_condition_value(const nine_vm_t *v, uint16_t key)
{
    if (!v || !v->pack || key >= v->pack->key_count) return 0;
    // The source overlays only the loop flag into conditions; clear_true and
    // titleBg remain permanent progress and are not visible to graph branches.
    if (key == v->pack->loop_key) return v->state.progress[key] ? 1 : 0;
    return v->state.flag[key];
}

bool nine_vm_matches(const nine_vm_t *v, uint32_t first, uint32_t count)
{
    bool stack[64];
    unsigned depth = 0;
    if (!v || !v->pack || first > v->pack->condition_count ||
        count > v->pack->condition_count - first) return false;
    if (!count) return true;
    for (uint32_t i = 0; i < count; ++i) {
        nine_condition_t c;
        if (!nine_pack_condition(v->pack, first + i, &c)) return false;
        if (c.op == NINE_COND_AND) {
            if (c.value < 0 || (unsigned)c.value > depth) return false;
            bool out = true;
            for (int32_t j = 0; j < c.value; ++j) out = stack[--depth] && out;
            if (depth == 64) return false;
            stack[depth++] = out;
            continue;
        }
        if (depth == 64) return false;
        int32_t cur = nine_vm_condition_value(v, c.key);
        bool out;
        switch (c.op) {
        case NINE_COND_TRUE: out = true; break;
        case NINE_COND_EQ: out = cur == c.value; break;
        case NINE_COND_NE: out = cur != c.value; break;
        case NINE_COND_LT: out = cur < c.value; break;
        case NINE_COND_LE: out = cur <= c.value; break;
        case NINE_COND_GT: out = cur > c.value; break;
        case NINE_COND_GE: out = cur >= c.value; break;
        default: return false;
        }
        stack[depth++] = out;
    }
    return depth == 1 && stack[0];
}

static bool apply(nine_vm_t *v, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        nine_effect_t ef;
        if (!nine_pack_effect(v->pack, first + i, &ef) ||
            ef.key >= v->pack->key_count) return false;
        bool permanent = nine_pack_key_persistent(v->pack, ef.key);
        // Chapter preview keeps its own run flags but never writes permanent keys.
        if (permanent && v->state.preview) continue;
        int32_t *values = permanent ? v->state.progress : v->state.flag;
        int64_t val = ef.value;
        if (ef.add) val += values[ef.key];
        if (val < INT32_MIN || val > INT32_MAX) return false;
        if (values[ef.key] != (int32_t)val) {
            values[ef.key] = (int32_t)val;
            if (permanent) v->progress_dirty = true;
        }
        mark(permanent ? v->state.progress_set : v->state.flag_set, ef.key);
    }
    return true;
}

bool nine_vm_start(nine_vm_t *v, uint16_t node, bool preview)
{
    if (!v || !v->pack) return false;
    if (node == NINE_NODE_NONE) node = v->pack->start;
    if (node >= v->pack->node_count) return false;
    memset(v->state.flag, 0, sizeof(v->state.flag));
    memset(v->state.flag_set, 0, sizeof(v->state.flag_set));
    v->state.node = node;
    v->state.index = 0;
    v->state.preview = preview;
    v->history_count = 0;
    return nine_vm_locate(v)->kind != NINE_VM_ERROR;
}

const nine_view_t *nine_vm_locate(nine_vm_t *v)
{
    if (!v || !v->pack) return NULL;
    for (unsigned step = 0; step < 500; ++step) {
        memset(&v->view, 0, sizeof(v->view));
        v->view.node = v->state.node;
        v->view.index = v->state.index;
        if (v->state.node == NINE_NODE_NONE) {
            v->view.kind = NINE_VM_END;
            return &v->view;
        }
        nine_node_t n;
        if (!nine_pack_node(v->pack, v->state.node, &n)) return error(v, "missing node");
        if (v->state.index > n.frame_count) return error(v, "frame cursor out of bounds");
        if (v->state.index < n.frame_count) {
            nine_frame_t f;
            v->view.frame = n.first_frame + v->state.index;
            if (!nine_pack_frame(v->pack, v->view.frame, &f))
                return error(v, "missing frame");
            v->view.kind = f.kind == NINE_FRAME_OP ? NINE_VM_OP : NINE_VM_TEXT;
            return &v->view;
        }
        if (!apply(v, n.first_effect, n.effect_count)) return error(v, "invalid effect");
        if (n.choice_count) {
            v->view.kind = NINE_VM_CHOICE;
            if (v->pack->filter_choices) {
                for (uint32_t i = 0; i < n.choice_count; ++i) {
                    nine_edge_t edge;
                    if (!nine_pack_edge(v->pack, true, n.first_choice + i, &edge))
                        return error(v, "missing choice");
                    if (nine_vm_matches(v, edge.first_condition, edge.condition_count))
                        v->view.choices[v->view.choice_count++] = n.first_choice + i;
                }
                // Source visibleChoices(): if every option was filtered out, all
                // options are offered again instead of dead-locking the story.
                if (!v->view.choice_count) {
                    for (uint32_t i = 0; i < n.choice_count; ++i)
                        v->view.choices[v->view.choice_count++] = n.first_choice + i;
                }
            } else {
                for (uint32_t i = 0; i < n.choice_count; ++i)
                    v->view.choices[v->view.choice_count++] = n.first_choice + i;
            }
            return &v->view;
        }
        bool found = false;
        for (uint32_t i = 0; i < n.next_count; ++i) {
            nine_edge_t edge;
            if (!nine_pack_edge(v->pack, false, n.first_next + i, &edge))
                return error(v, "missing next");
            if (nine_vm_matches(v, edge.first_condition, edge.condition_count)) {
                // Source next edges carry no effects. Reject a pack that adds them
                // instead of silently expanding the source semantics.
                if (edge.effect_count) return error(v, "unexpected next effect");
                v->state.node = edge.to;
                v->state.index = 0;
                found = true;
                break;
            }
        }
        if (!found) return error(v, "no matching next");
    }
    return error(v, "graph hop limit");
}

const nine_view_t *nine_vm_advance(nine_vm_t *v)
{
    const nine_view_t *p = nine_vm_locate(v);
    if (!p) return NULL;
    if (p->kind != NINE_VM_TEXT && p->kind != NINE_VM_OP) return p;
    remember(v);
    v->state.index++;
    return nine_vm_locate(v);
}

const nine_view_t *nine_vm_choose(nine_vm_t *v, uint32_t index)
{
    const nine_view_t *p = nine_vm_locate(v);
    if (!p) return NULL;
    if (p->kind != NINE_VM_CHOICE || index >= p->choice_count)
        return error(v, "invalid choice");
    nine_edge_t e;
    if (!nine_pack_edge(v->pack, true, p->choices[index], &e))
        return error(v, "missing selected edge");
    remember(v);
    if (!apply(v, e.first_effect, e.effect_count)) return error(v, "invalid choice effect");
    v->state.node = e.to;
    v->state.index = 0;
    return nine_vm_locate(v);
}

const nine_view_t *nine_vm_previous(nine_vm_t *v)
{
    if (!v || !v->pack) return NULL;
    if (v->history_count) {
        uint8_t *e = hist_at(v, v->history_count - 1u);
        v->history_count--;
        if (!restore_checkpoint(v, e)) return error(v, "invalid checkpoint");
    }
    return nine_vm_locate(v);
}

const nine_view_t *nine_vm_skip_chapter(nine_vm_t *v)
{
    const nine_view_t *p = nine_vm_locate(v);
    if (!p || p->kind != NINE_VM_TEXT) return p;
    nine_node_t initial;
    if (!nine_pack_node(v->pack, v->state.node, &initial)) return error(v, "missing chapter");
    remember(v);
    for (unsigned step = 0; step < 500; ++step) {
        nine_node_t n;
        if (!nine_pack_node(v->pack, v->state.node, &n)) return error(v, "missing skip node");
        uint32_t to = n.frame_count;
        for (uint32_t i = v->state.index + 1; i < n.frame_count; ++i) {
            nine_frame_t f;
            if (!nine_pack_frame(v->pack, n.first_frame + i, &f))
                return error(v, "invalid skip frame");
            if (f.kind == NINE_FRAME_OP) { to = i; break; }
        }
        v->state.index = to;
        p = nine_vm_locate(v);
        if (!p) return NULL;
        if (p->kind != NINE_VM_TEXT) return p;
        if (!nine_pack_node(v->pack, v->state.node, &n)) return error(v, "missing chapter node");
        if (n.chapter != initial.chapter) return p;
    }
    return error(v, "chapter hop limit");
}

bool nine_vm_current_frame(const nine_vm_t *v, nine_frame_t *out)
{
    return v && (v->view.kind == NINE_VM_TEXT || v->view.kind == NINE_VM_OP) &&
           nine_pack_frame(v->pack, v->view.frame, out);
}

int32_t nine_vm_progress_value(const nine_vm_t *v, uint16_t key)
{
    if (!v || key >= v->pack->key_count) return 0;
    return v->state.progress[key];
}

bool nine_vm_ending_reached(const nine_vm_t *v, const char *id)
{
    if (!v || !id) return false;
    int key = nine_pack_find_key(v->pack, id);
    if (key < 0) return false;
    return v->state.progress[key] != 0;
}

bool nine_vm_progress_dirty(const nine_vm_t *v) { return v && v->progress_dirty; }
void nine_vm_clear_progress_dirty(nine_vm_t *v) { if (v) v->progress_dirty = false; }

// ---------------------------------------------------------------- records
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
#define SAVE_MAGIC 0x3153564eu      // "NVS1"
#define PROGRESS_MAGIC 0x3150564eu  // "NVP1"

static size_t progress_size(uint32_t keys) { return 24 + (size_t)keys * 6 + 4; }

static size_t encode_pairs(uint8_t *out, const int32_t *values, const uint8_t *set,
                           uint32_t keys)
{
    size_t n = 0;
    for (uint32_t k = 0; k < keys; ++k) {
        if (!has(set, (uint16_t)k)) continue;
        out[n * 6] = (uint8_t)k;
        out[n * 6 + 1] = (uint8_t)(k >> 8);
        write32(out + n * 6 + 2, (uint32_t)values[k]);
        n++;
    }
    return n;
}

size_t nine_vm_save_encode(const nine_vm_t *v, uint16_t page, uint8_t *out, size_t cap)
{
    if (!v || !v->pack || !out) return 0;
    uint32_t keys = v->pack->key_count;
    size_t size = 32 + (size_t)keys * 12 + 4;
    if (cap < size) return 0;
    memset(out, 0, size);
    write32(out, SAVE_MAGIC);
    write32(out + 4, 2);
    write32(out + 8, v->pack->episode);
    write32(out + 12, v->pack->identity);
    write32(out + 16, v->state.node);
    write32(out + 20, v->state.index);
    write32(out + 24, page);
    write32(out + 28, v->state.preview ? 1u : 0u);
    size_t nrun = encode_pairs(out + 32, v->state.flag, v->state.flag_set, keys);
    size_t nprog = encode_pairs(out + 32 + nrun * 6, v->state.progress,
                                v->state.progress_set, keys);
    size_t used = 32 + (nrun + nprog) * 6;
    write32(out + used, (uint32_t)(nrun | nprog << 16));
    used += 4;
    write32(out + used, crc32(out, used));
    used += 4;
    return used;
}

static bool record_ok(const nine_pack_t *p, const uint8_t *d, size_t size,
                      uint32_t magic, size_t min)
{
    return p && d && size >= min && size <= 40u + (size_t)p->key_count * 12u &&
           read32(d) == magic && read32(d + 4) == 2 &&
           read32(d + 8) == p->episode && read32(d + 12) == p->identity &&
           read32(d + size - 4) == crc32(d, size - 4);
}

bool nine_vm_save_peek(const nine_pack_t *p, const uint8_t *d, size_t size,
                       uint16_t *node, uint32_t *index, uint16_t *page, bool *preview)
{
    if (!record_ok(p, d, size, SAVE_MAGIC, 40)) return false;
    uint32_t n = read32(d + 16), i = read32(d + 20), pg = read32(d + 24);
    if (pg > UINT16_MAX) return false;
    nine_node_t nr;
    if (n != NINE_NODE_NONE && (n >= p->node_count || !nine_pack_node(p, (uint16_t)n, &nr) ||
                                i > nr.frame_count)) return false;
    if (n == NINE_NODE_NONE && i) return false;
    if (node) *node = (uint16_t)n;
    if (index) *index = i;
    if (page) *page = (uint16_t)pg;
    if (preview) *preview = read32(d + 28) != 0;
    return true;
}

bool nine_vm_save_decode(nine_vm_t *v, const uint8_t *d, size_t size, uint16_t *page)
{
    if (!v || !v->pack) return false;
    uint16_t node, pg;
    uint32_t index;
    bool preview;
    if (!nine_vm_save_peek(v->pack, d, size, &node, &index, &pg, &preview)) return false;
    uint32_t counts = read32(d + size - 8);
    uint16_t nrun = (uint16_t)counts, nprog = (uint16_t)(counts >> 16);
    uint32_t keys = v->pack->key_count;
    size_t used = 32 + ((size_t)nrun + nprog) * 6 + 8;
    if (used != size) return false;
    nine_state_t next;
    memset(&next, 0, sizeof(next));
    next.node = node;
    next.index = index;
    next.preview = preview;
    const uint8_t *p = d + 32;
    for (uint16_t i = 0; i < nrun; ++i, p += 6) {
        uint16_t k = (uint16_t)(p[0] | p[1] << 8);
        if (k >= keys || has(next.flag_set, k)) return false;
        next.flag[k] = (int32_t)read32(p + 2);
        mark(next.flag_set, k);
    }
    for (uint16_t i = 0; i < nprog; ++i, p += 6) {
        uint16_t k = (uint16_t)(p[0] | p[1] << 8);
        if (k >= keys || has(next.progress_set, k)) return false;
        next.progress[k] = (int32_t)read32(p + 2);
        mark(next.progress_set, k);
    }
    // Permanent progress never rolls back on load. A preview save is isolated.
    if (!preview) {
        for (uint16_t k = 0; k < keys; ++k) {
            if (has(v->state.progress_set, k)) {
                next.progress[k] = v->state.progress[k];
                mark(next.progress_set, k);
            }
        }
    }
    v->state = next;
    v->history_count = 0;
    if (page) *page = pg;
    memset(&v->view, 0, sizeof(v->view));
    return true;
}

size_t nine_vm_progress_encode(const nine_vm_t *v, uint8_t *out, size_t cap)
{
    if (!v || !v->pack || !out) return 0;
    uint32_t keys = v->pack->key_count;
    size_t size = progress_size(keys);
    if (cap < size) return 0;
    memset(out, 0, size);
    write32(out, PROGRESS_MAGIC);
    write32(out + 4, 2);
    write32(out + 8, v->pack->episode);
    write32(out + 12, v->pack->identity);
    size_t n = encode_pairs(out + 24, v->state.progress, v->state.progress_set, keys);
    size_t used = 24 + n * 6;
    write32(out + 20, (uint32_t)n);
    write32(out + used, crc32(out, used));
    used += 4;
    return used;
}

bool nine_vm_progress_decode(nine_vm_t *v, const uint8_t *d, size_t size)
{
    if (!v || !v->pack || !record_ok(v->pack, d, size, PROGRESS_MAGIC, 28)) return false;
    uint32_t n = read32(d + 20);
    if (size != 24 + (size_t)n * 6 + 4) return false;
    uint8_t set[NINE_KEY_MAX / 8];
    int32_t values[NINE_KEY_MAX];
    memset(set, 0, sizeof(set));
    memset(values, 0, sizeof(values));
    const uint8_t *p = d + 24;
    for (uint32_t i = 0; i < n; ++i, p += 6) {
        uint16_t k = (uint16_t)(p[0] | p[1] << 8);
        if (k >= v->pack->key_count || has(set, k)) return false;
        values[k] = (int32_t)read32(p + 2);
        mark(set, k);
    }
    memcpy(v->state.progress, values, sizeof(values));
    memcpy(v->state.progress_set, set, sizeof(set));
    return true;
}
