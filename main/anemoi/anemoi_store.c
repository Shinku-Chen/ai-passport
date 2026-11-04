// main/anemoi/anemoi_store.c —— 存档编解码实现（纯 C，宿主机可测）。
#include "anemoi_store.h"

#include <stdio.h>
#include <string.h>

#define SLOT_HEADER 44u
#define PROGRESS_HEADER 24u
#define SLOT_TAIL 4u
#define PROGRESS_TAIL 4u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

size_t anem_slot_blob_size(void)
{
    return SLOT_HEADER + ANEM_ACTOR_MAX * 4u + ANEM_BODY_MAX * 6u + ANEM_VAR_MAX * 8u + SLOT_TAIL;
}

size_t anem_progress_blob_size(void)
{
    return PROGRESS_HEADER + (ANEM_CG_BITS / 8u) + (ANEM_ENDING_BITS / 8u) + ANEM_VAR_MAX * 8u +
           PROGRESS_TAIL;
}



// ---------------------------------------------------------------- 固定内部堆工作区
// 单次分配：union{slot, progress} + 序列化 blob。运行期只此一份，busy 防重入；
// 分配失败时所有存档操作返回 false（调用方给出可见失败提示），不影响 live 状态。
#if defined(ANEM_HOST_TEST)
static uint8_t s_host_arena[16384];
#define arena_alloc(n) ((void *)((void)s_host_arena, (void *)s_host_arena))
#else
#include "esp_heap_caps.h"
#define arena_alloc(n) heap_caps_malloc((n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

typedef union {
    anem_slot_t slot;
    anem_progress_t progress;
} anem_work_t;

static anem_work_t *s_work;
static uint8_t *s_work_blob;
static bool s_union_busy;
static bool s_blob_busy;
static bool s_work_init_done;

#define ANEM_WORK_BLOB 4608u

bool anem_store_workspace_init(void)
{
    if (s_work_init_done) {
        return s_work != NULL;
    }
    s_work_init_done = true;
    const size_t need = sizeof(anem_work_t) + ANEM_WORK_BLOB;
    void *mem = arena_alloc(need);
    if (mem == NULL) {
        return false;
    }
    s_work = (anem_work_t *)mem;
    s_work_blob = (uint8_t *)mem + sizeof(anem_work_t);
    memset(s_work, 0, need);
    return true;
}

bool anem_store_workspace_acquire(void)
{
    if (!s_work || s_union_busy) {
        return false;
    }
    s_union_busy = true;
    return true;
}

void anem_store_workspace_release(void)
{
    s_union_busy = false;
}

// blob 是 store 内部资源：每次 IO 短暂持有；重入（例如存储回调里再存档）会被拒绝。
static bool blob_acquire(void)
{
    if (!s_work_blob || s_blob_busy) {
        return false;
    }
    s_blob_busy = true;
    return true;
}

static void blob_release(void)
{
    s_blob_busy = false;
}

anem_slot_t *anem_store_workspace_slot(void)
{
    return s_work ? &s_work->slot : NULL;
}

anem_progress_t *anem_store_workspace_progress(void)
{
    return s_work ? &s_work->progress : NULL;
}



void anem_slot_clear(anem_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

void anem_progress_clear(anem_progress_t *progress)
{
    if (progress != NULL) {
        memset(progress, 0, sizeof(*progress));
        progress->version = ANEM_PROGRESS_VERSION;
    }
}

static bool value_valid(anem_value_t v)
{
    return v.kind <= ANEM_VAL_ARR && (v.kind != ANEM_VAL_ARR || v.len <= 8);
}

static bool slot_valid(const anem_slot_t *slot)
{
    if (slot->chapter == 0 || slot->cursor > 0xFFFFFFu || slot->page > ANEM_PAGE_MAX ||
        slot->mode > 1 || slot->progression > 1 || slot->body_count > ANEM_BODY_MAX ||
        slot->var_count > ANEM_VAR_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < slot->body_count; i++) {
        if (!slot->bodies[i].valid && slot->bodies[i].name != 0) {
            return false;
        }
    }
    for (uint16_t i = 0; i < slot->var_count; i++) {
        if (!value_valid(slot->vars[i])) {
            return false;
        }
    }
    return true;
}

bool anem_slot_encode(const anem_slot_t *slot, uint8_t *buf, size_t size)
{
    if (slot == NULL || buf == NULL || size != anem_slot_blob_size() || !slot_valid(slot)) {
#ifdef ANEM_STORE_DEBUG
        fprintf(stderr, "encode reject: size=%zu want=%zu valid=%d chapter=%u cursor=%u var=%u\n",
                size, anem_slot_blob_size(), slot ? slot_valid(slot) : -1,
                slot ? slot->chapter : 0, slot ? (unsigned)slot->cursor : 0,
                slot ? slot->var_count : 0);
#endif
        return false;
    }
    memset(buf, 0, size);
    uint8_t *p = buf;
    wr32(p, 0x414E454Du);           // "ANEM"
    wr16(p + 4, ANEM_SAVE_VERSION);
    wr16(p + 6, (uint16_t)size);
    p[8] = slot->valid ? 1 : 0;
    p[9] = slot->mode;
    p[10] = slot->progression;
    p[11] = slot->body_count;
    p += 12;
    wr16(p, slot->chapter);
    wr16(p + 2, slot->root_chapter);
    wr32(p + 4, slot->cursor);
    wr16(p + 8, slot->page);
    wr16(p + 10, slot->speaker);
    wr16(p + 12, slot->text);
    wr16(p + 14, slot->regular_bg);
    wr16(p + 16, slot->cg);
    wr32(p + 20, slot->changed);
    p[24] = slot->changed_cg ? 1 : 0;
    p[25] = 0;
    wr16(p + 26, slot->var_count);
    wr32(p + 28, slot->saved_at);
    p += 32;
    for (int i = 0; i < ANEM_ACTOR_MAX; i++) {
        wr16(p + i * 4, slot->dress[i]);
        wr16(p + i * 4 + 2, slot->pose[i]);
    }
    p += ANEM_ACTOR_MAX * 4;
    for (int i = 0; i < ANEM_BODY_MAX; i++) {
        wr16(p + i * 6, slot->bodies[i].name);
        wr16(p + i * 6 + 2, slot->bodies[i].dress);
        p[i * 6 + 4] = slot->bodies[i].valid ? 1 : 0;
    }
    p += ANEM_BODY_MAX * 6;
    for (int i = 0; i < ANEM_VAR_MAX; i++) {
        wr32(p + i * 8, (uint32_t)slot->vars[i].num);
        wr16(p + i * 8 + 4, slot->vars[i].text);
        p[i * 8 + 6] = slot->vars[i].kind;
        p[i * 8 + 7] = slot->vars[i].len;
    }
    p += ANEM_VAR_MAX * 8;
    wr32(p, crc32_ieee(buf, (size_t)(p - buf)));
    return true;
}

// 只读语义校验，等价于 slot_valid，但不写 caller、不查 >= var_count 的尾项。
static bool slot_wire_valid(const uint8_t *buf, size_t size)
{
    if (buf == NULL || size != anem_slot_blob_size()) return false;
    if (rd32(buf) != 0x414E454Du || rd16(buf + 4) != ANEM_SAVE_VERSION ||
        rd16(buf + 6) != (uint16_t)size) return false;
    if (rd32(buf + size - SLOT_TAIL) != crc32_ieee(buf, size - SLOT_TAIL)) return false;
    if (buf[8] == 0) return false;
    if (buf[9] > 1 || buf[10] > 1) return false;
    if (rd16(buf + 12) == 0) return false;
    if (rd32(buf + 16) > 0xFFFFFFu) return false;
    if (rd16(buf + 20) > ANEM_PAGE_MAX) return false;
    const uint8_t body_count = buf[11];
    const uint16_t var_count = rd16(buf + 12 + 26);
    if (body_count > ANEM_BODY_MAX || var_count > ANEM_VAR_MAX) return false;
    const uint8_t *bodies = buf + 12 + 32 + ANEM_ACTOR_MAX * 4;
    for (uint8_t i = 0; i < body_count; i++) {
        if (bodies[i * 6 + 4] == 0 && rd16(bodies + i * 6) != 0) return false;
    }
    const uint8_t *vars = bodies + ANEM_BODY_MAX * 6;
    for (uint16_t i = 0; i < var_count; i++) {
        anem_value_t v;
        v.num = (int32_t)rd32(vars + i * 8);
        v.text = rd16(vars + i * 8 + 4);
        v.kind = vars[i * 8 + 6];
        v.len = vars[i * 8 + 7];
        if (!value_valid(v)) return false;
    }
    return true;
}

// progress：同样只查 i < var_count。
static bool progress_wire_valid(const uint8_t *buf, size_t size)
{
    if (buf == NULL || size != anem_progress_blob_size()) return false;
    if (rd32(buf) != 0x414E5052u || rd16(buf + 4) != ANEM_PROGRESS_VERSION ||
        rd16(buf + 6) != (uint16_t)size) return false;
    if (rd32(buf + size - PROGRESS_TAIL) != crc32_ieee(buf, size - PROGRESS_TAIL)) return false;
    const uint16_t var_count = rd16(buf + 10);
    if (var_count > ANEM_VAR_MAX) return false;
    const uint8_t *vars = buf + PROGRESS_HEADER + ANEM_CG_BITS / 8 + ANEM_ENDING_BITS / 8;
    for (uint16_t i = 0; i < ANEM_VAR_MAX; i++) {
        anem_value_t v;
        v.num = (int32_t)rd32(vars + i * 8);
        v.text = rd16(vars + i * 8 + 4);
        v.kind = vars[i * 8 + 6];
        v.len = vars[i * 8 + 7];
        if (!value_valid(v)) return false;
    }
    return true;
}

bool anem_slot_decode(const uint8_t *buf, size_t size, anem_slot_t *slot)
{
    if (buf == NULL || slot == NULL || size != anem_slot_blob_size()) {
        return false;
    }
    if (rd32(buf) != 0x414E454Du || rd16(buf + 4) != ANEM_SAVE_VERSION ||
        rd16(buf + 6) != (uint16_t)size) {
        return false;
    }
    if (rd32(buf + size - SLOT_TAIL) != crc32_ieee(buf, size - SLOT_TAIL)) {
        return false;
    }
    if (!slot_wire_valid(buf, size)) {
        return false;   // 失败不改 caller
    }
    anem_slot_t *outp = slot;   // 两阶段：头/CRC 已验，直接提交 caller
    memset(outp, 0, sizeof(*outp));
    anem_slot_t *const out = outp;
    out->valid = buf[8] != 0;
    out->mode = buf[9];
    out->progression = buf[10];
    out->body_count = buf[11];
    const uint8_t *p = buf + 12;
    out->chapter = rd16(p);
    out->root_chapter = rd16(p + 2);
    out->cursor = rd32(p + 4);
    out->page = rd16(p + 8);
    out->speaker = rd16(p + 10);
    out->text = rd16(p + 12);
    out->regular_bg = rd16(p + 14);
    out->cg = rd16(p + 16);
    out->changed = rd32(p + 20);
    out->changed_cg = p[24] != 0;
    out->var_count = rd16(p + 26);
    out->saved_at = rd32(p + 28);
    p += 32;
    for (int i = 0; i < ANEM_ACTOR_MAX; i++) {
        out->dress[i] = (uint8_t)rd16(p + i * 4);
        out->pose[i] = rd16(p + i * 4 + 2);
    }
    p += ANEM_ACTOR_MAX * 4;
    for (int i = 0; i < ANEM_BODY_MAX; i++) {
        out->bodies[i].name = rd16(p + i * 6);
        out->bodies[i].dress = rd16(p + i * 6 + 2);
        out->bodies[i].valid = p[i * 6 + 4] != 0;
    }
    p += ANEM_BODY_MAX * 6;
    for (int i = 0; i < ANEM_VAR_MAX; i++) {
        out->vars[i].num = (int32_t)rd32(p + i * 8);
        out->vars[i].text = rd16(p + i * 8 + 4);
        out->vars[i].kind = p[i * 8 + 6];
        out->vars[i].len = p[i * 8 + 7];
    }

    
    return true;
}

void anem_slot_from_state(anem_slot_t *slot, const anem_script_t *script, uint8_t mode,
                          uint16_t page, uint32_t saved_at)
{
    if (slot == NULL || script == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->mode = mode;
    slot->progression = script->st.progression;
    slot->chapter = script->st.chapter;
    slot->root_chapter = script->st.root_chapter;
    slot->cursor = script->st.cursor;
    slot->page = page;
    slot->speaker = script->st.speaker;
    slot->text = script->st.text;
    slot->regular_bg = script->st.regular_bg;
    slot->cg = script->st.cg;
    slot->changed = script->st.changed;
    slot->changed_cg = script->st.changed_cg ? 1 : 0;
    slot->body_count = script->st.body_count;
    slot->var_count = script->st.var_count;
    slot->saved_at = saved_at;
    memcpy(slot->bodies, script->st.bodies, sizeof(slot->bodies));
    memcpy(slot->dress, script->st.dress, sizeof(slot->dress));
    memcpy(slot->pose, script->st.pose, sizeof(slot->pose));
    memcpy(slot->vars, script->st.vars, sizeof(slot->vars));
}

bool anem_slot_apply(const anem_slot_t *slot, anem_script_t *script)
{
    if (slot == NULL || script == NULL || !slot->valid || !slot_valid(slot)) {
        return false;
    }
    script->st.progression = slot->progression;
    script->st.chapter = slot->chapter;
    script->st.root_chapter = slot->root_chapter;
    script->st.cursor = slot->cursor;
    script->st.speaker = slot->speaker;
    script->st.text = slot->text;
    script->st.regular_bg = slot->regular_bg;
    script->st.cg = slot->cg;
    script->st.changed = slot->changed;
    script->st.changed_cg = slot->changed_cg != 0;
    script->st.ended = false;
    script->st.body_count = slot->body_count;
    script->st.var_count = slot->var_count;
    memcpy(script->st.bodies, slot->bodies, sizeof(script->st.bodies));
    memcpy(script->st.dress, slot->dress, sizeof(script->st.dress));
    memcpy(script->st.pose, slot->pose, sizeof(script->st.pose));
    memset(script->st.vars, 0, sizeof(script->st.vars));
    memcpy(script->st.vars, slot->vars, sizeof(slot->vars));
    script->call_count = 0;
    script->board_count = 0;
    script->choice_count = 0;
    script->history_count = 0;
    script->history_next = 0;
    script->history_text_count = 0;
    script->failed = false;
    script->notice[0] = 0;
    return true;
}

size_t anem_slot_preview(const anem_slot_t *slot, const anem_pack_t *pack, char *out, size_t cap)
{
    if (slot == NULL || !slot->valid || out == NULL || cap == 0) {
        return 0;
    }
    anem_chap_t chap;
    const char *name = "";
    if (anem_pack_chapter_for(pack, slot->root_chapter ? slot->root_chapter : slot->chapter,
                              &chap)) {
        name = anem_pack_text(pack, chap.name);
        if (name == NULL) {
            name = "";
        }
    }
    const char *line = anem_pack_text(pack, slot->text);
    if (line == NULL) {
        line = "";
    }
    int written = snprintf(out, cap, "%s · %s", name, line);
    if (written < 0) {
        out[0] = 0;
        return 0;
    }
    if ((size_t)written >= cap) {
        out[cap - 1] = 0;
        written = (int)cap - 1;
    }
    return (size_t)written;
}

static bool progress_var_key(const anem_pack_t *pack, uint16_t index, char *out, size_t cap)
{
    const char *name = anem_var_name(pack, index);
    if (name == NULL) {
        return false;
    }
    if ((name[0] == 'g' || name[0] == 'z') && name[1] == ':') {
        snprintf(out, cap, "%s", name);
        return true;
    }
    return false;
}

void anem_progress_from_script(anem_progress_t *progress, const anem_script_t *script)
{
    if (progress == NULL || script == NULL) {
        return;
    }
    // 直接写 caller（caller 用 arena 中的 progress），不再在栈上放 4,120 B 的整份进度。
    anem_progress_clear(progress);
    progress->level = anem_script_unlock_level(script);
    progress->var_count = script->st.var_count;
    for (uint16_t i = 0; i < script->st.var_count && i < ANEM_VAR_MAX; i++) {
        char key[24];
        if (progress_var_key(script->pack, i, key, sizeof(key))) {
            progress->vars[i] = script->st.vars[i];
        } else {
            memset(&progress->vars[i], 0, sizeof(progress->vars[i]));
        }
    }
    (void)ANEM_CG_BITS;
}

bool anem_progress_apply(const anem_progress_t *progress, anem_script_t *script)
{
    if (progress == NULL || script == NULL) {
        return false;
    }
    if (progress->var_count != script->st.var_count) {
        return false;
    }
    for (uint16_t i = 0; i < progress->var_count && i < ANEM_VAR_MAX; i++) {
        char key[24];
        if (progress_var_key(script->pack, i, key, sizeof(key))) {
            script->st.vars[i] = progress->vars[i];
        }
    }
    return true;
}

bool anem_progress_has_cg(const anem_progress_t *progress, uint8_t index)
{
    if (progress == NULL || index >= ANEM_CG_BITS) {
        return false;
    }
    return (progress->cg[index / 8] & (1u << (index % 8))) != 0;
}

void anem_progress_mark_cg(anem_progress_t *progress, uint8_t index)
{
    if (progress != NULL && index < ANEM_CG_BITS) {
        progress->cg[index / 8] |= (uint8_t)(1u << (index % 8));
    }
}

bool anem_progress_has_ending(const anem_progress_t *progress, uint8_t index)
{
    if (progress == NULL || index >= ANEM_ENDING_BITS) {
        return false;
    }
    return (progress->ending[index / 8] & (1u << (index % 8))) != 0;
}

void anem_progress_mark_ending(anem_progress_t *progress, uint8_t index)
{
    if (progress != NULL && index < ANEM_ENDING_BITS) {
        progress->ending[index / 8] |= (uint8_t)(1u << (index % 8));
    }
}

bool anem_progress_encode(const anem_progress_t *progress, uint8_t *buf, size_t size)
{
    if (progress == NULL || buf == NULL || size != anem_progress_blob_size()) {
        return false;
    }
    memset(buf, 0, size);
    uint8_t *p = buf;
    wr32(p, 0x414E5052u);  // "ANPR"
    wr16(p + 4, ANEM_PROGRESS_VERSION);
    wr16(p + 6, (uint16_t)size);
    p[8] = progress->level;
    wr16(p + 10, progress->var_count);
    p += PROGRESS_HEADER;
    memcpy(p, progress->cg, ANEM_CG_BITS / 8);
    p += ANEM_CG_BITS / 8;
    memcpy(p, progress->ending, ANEM_ENDING_BITS / 8);
    p += ANEM_ENDING_BITS / 8;
    for (int i = 0; i < ANEM_VAR_MAX; i++) {
        wr32(p + i * 8, (uint32_t)progress->vars[i].num);
        wr16(p + i * 8 + 4, progress->vars[i].text);
        p[i * 8 + 6] = progress->vars[i].kind;
        p[i * 8 + 7] = progress->vars[i].len;
    }
    p += ANEM_VAR_MAX * 8;
    wr32(p, crc32_ieee(buf, (size_t)(p - buf)));
    return true;
}

bool anem_progress_decode(const uint8_t *buf, size_t size, anem_progress_t *progress)
{
    if (buf == NULL || progress == NULL || size != anem_progress_blob_size()) {
        return false;
    }
    if (rd32(buf) != 0x414E5052u || rd16(buf + 4) != ANEM_PROGRESS_VERSION ||
        rd16(buf + 6) != (uint16_t)size) {
        return false;
    }
    if (rd32(buf + size - PROGRESS_TAIL) != crc32_ieee(buf, size - PROGRESS_TAIL)) {
        return false;
    }
    if (!progress_wire_valid(buf, size)) {
        return false;   // 失败不改 caller
    }
    anem_progress_t *outp = progress;
    memset(outp, 0, sizeof(*outp));
    anem_progress_t *const out = outp;
    out->version = ANEM_PROGRESS_VERSION;
    out->level = buf[8];
    out->var_count = rd16(buf + 10);   // 上限已由 progress_wire_valid 保证
    const uint8_t *p = buf + PROGRESS_HEADER;
    memcpy(out->cg, p, ANEM_CG_BITS / 8);
    p += ANEM_CG_BITS / 8;
    memcpy(out->ending, p, ANEM_ENDING_BITS / 8);
    p += ANEM_ENDING_BITS / 8;
    for (int i = 0; i < ANEM_VAR_MAX; i++) {
        out->vars[i].num = (int32_t)rd32(p + i * 8);
        out->vars[i].text = rd16(p + i * 8 + 4);
        out->vars[i].kind = p[i * 8 + 6];
        out->vars[i].len = p[i * 8 + 7];
    }
    return true;
}

// ---------------------------------------------------------------- 读写

static const anem_store_io_t *s_io;

void anem_store_bind(const anem_store_io_t *io)
{
    s_io = io;
}

const anem_store_io_t *anem_store_current_io(void)
{
    return s_io;
}

static bool io_read(const char *key, void *out, size_t size)
{
    const anem_store_io_t *io = anem_store_current_io();
    size_t got = 0;
    return io != NULL && io->read != NULL && io->read(key, out, size, &got) && got == size;
}

static bool io_write(const char *key, const void *data, size_t size)
{
    const anem_store_io_t *io = anem_store_current_io();
    return io != NULL && io->write != NULL && io->write(key, data, size);
}

static void slot_key(uint8_t index, char *out, size_t cap)
{
    snprintf(out, cap, "anemoi_save_%02u", (unsigned)index);
}

bool anem_store_load_slot(uint8_t index, anem_slot_t *slot)
{
    if (index >= ANEM_SLOT_COUNT || slot == NULL) {
        return false;
    }
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_slot_blob_size();
    if (size > ANEM_WORK_BLOB) {
        blob_release();
        return false;
    }
    char key[24];
    slot_key(index, key, sizeof(key));
    if (!io_read(key, buf, size)) {
        blob_release();
        return false;
    }
    const bool ok = anem_slot_decode(buf, size, slot);
    blob_release();
    return ok;
}

bool anem_store_save_slot(uint8_t index, const anem_slot_t *slot)
{
    if (index >= ANEM_SLOT_COUNT || slot == NULL) {
        return false;
    }
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_slot_blob_size();
    if (size > ANEM_WORK_BLOB || !anem_slot_encode(slot, buf, size)) {
        blob_release();
        return false;
    }
    char key[24];
    slot_key(index, key, sizeof(key));
    if (!io_write(key, buf, size)) {
        blob_release();
        return false;
    }
    blob_release();
    return true;
}

bool anem_store_load_auto(anem_slot_t *slot)
{
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_slot_blob_size();
    if (size > ANEM_WORK_BLOB || !io_read("anemoi_auto", buf, size)) {
        blob_release();
        return false;
    }
    const bool ok = anem_slot_decode(buf, size, slot);
    blob_release();
    return ok;
}

bool anem_store_save_auto(const anem_slot_t *slot)
{
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_slot_blob_size();
    if (size > ANEM_WORK_BLOB || !anem_slot_encode(slot, buf, size)) {
        blob_release();
        return false;
    }
    if (!io_write("anemoi_auto", buf, size)) {
        blob_release();
        return false;
    }
    blob_release();
    return true;
}

bool anem_store_load_progress(anem_progress_t *progress)
{
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_progress_blob_size();
    if (size > ANEM_WORK_BLOB || !io_read("anemoi_progress", buf, size)) {
        blob_release();
        return false;
    }
    const bool ok = anem_progress_decode(buf, size, progress);
    blob_release();
    return ok;
}

bool anem_store_save_progress(const anem_progress_t *progress)
{
    if (!blob_acquire()) {
        return false;
    }
    uint8_t *buf = s_work_blob;
    const size_t size = anem_progress_blob_size();
    if (size > ANEM_WORK_BLOB || !anem_progress_encode(progress, buf, size)) {
        blob_release();
        return false;
    }
    if (!io_write("anemoi_progress", buf, size)) {
        blob_release();
        return false;
    }
    blob_release();
    return true;
}
