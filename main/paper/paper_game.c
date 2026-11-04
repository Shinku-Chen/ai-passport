// main/paper/paper_game.c —— 剧情状态机、历史、解锁与存档（严格校验版）。
//
// 游标语义（与 paper-rw5 src/common/engine.js 对齐）：
//   cursor       = 下一条要执行的 op（场景内相对下标）
//   text_op      = 当前正在显示的那条 TEXT op（存档从它恢复，避免跳句）
//   choice_op    = 当前停住的 CHOICE op（存档从它恢复，读档会重新给出选项）
#include "paper_game.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "paper_game";
#define NVS_NS "paper"
#define SAVE_MAGIC 0x50415052u
#define SAVE_VER 3

static paper_step_t s_step;
static bool nvs_ready(void);

typedef struct {
    uint32_t crc;          // 覆盖 crc 之后的全部字节
    uint32_t magic;
    uint16_t ver;
    uint16_t scene_id;
    uint32_t resume_cursor;  // 指向 TEXT 或 CHOICE op
    uint16_t background;
    uint16_t sprites[3];
    uint8_t speaker;
    uint8_t endings_seen;
    uint8_t story_mode;
    int32_t vars[192];
    uint8_t cg[10];
    uint32_t stamp;
} paper_save_t;

#define SAVE_FIELDS_OFF offsetof(paper_save_t, magic)
#define SAVE_CRC_LEN (sizeof(paper_save_t) - SAVE_FIELDS_OFF)

static uint32_t crc32_buf(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

bool paper_game_init(paper_game_t *g)
{
    memset(g, 0, sizeof(*g));
    if (!paper_pack_init(&g->pack)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        g->sprites[i] = 0xFFFF;
    }
    g->background = 0xFFFF;
    g->choice_op = -1;
    g->text_op = -1;
    return true;
}

static void history_push(paper_game_t *g, uint8_t speaker, const char *text)
{
    if (!text) {
        return;
    }
    paper_history_entry_t *e = &g->history[g->history_head];
    e->slot = g->scene_id;
    const char *sp = paper_speaker_name(&g->pack, speaker);
    snprintf(e->speaker, sizeof(e->speaker), "%s", sp ? sp : "");
    snprintf(e->text, sizeof(e->text), "%s", text);
    g->history_head = (g->history_head + 1) % PAPER_HISTORY_MAX;
    if (g->history_count < PAPER_HISTORY_MAX) {
        g->history_count++;
    }
}

bool paper_game_new(paper_game_t *g)
{
    if (!paper_game_start(g, g->pack.flow_scene)) {
        return false;
    }
    g->story_mode = true;
    // 源 progress.js：只把 sf.* 周目变量带进新周目（tf.scene_mode 等运行时变量清零）
    for (int i = 1; i < 192; i++) {
        g->vars[i] = g->persist_vars[i];
    }
    return true;
}

bool paper_game_start(paper_game_t *g, uint16_t scene_id)
{
    if (!paper_scene_find(&g->pack, scene_id)) {
        return false;
    }
    g->scene_id = scene_id;
    g->story_mode = false;      // 章节入口：只携带已有 sf.*，不回写周目记录
    g->cursor = 0;
    g->text_op = -1;
    g->choice_op = -1;
    g->ended = false;
    g->background = 0xFFFF;
    for (int i = 0; i < 3; i++) {
        g->sprites[i] = 0xFFFF;
    }
    g->speaker = 0;
    memset(g->vars, 0, sizeof(g->vars));
    // 携带已有的 sf.* 周目变量（源 withProgress）；是否回写由 story_mode 决定
    for (int i = 1; i < 192; i++) {
        g->vars[i] = g->persist_vars[i];
    }
    g->history_count = 0;
    g->history_head = 0;
    return true;
}

static const paper_scene_t *scene_of(const paper_game_t *g)
{
    return paper_scene_find(&g->pack, g->scene_id);
}

static bool cond_true(const paper_game_t *g, uint8_t kind)
{
    switch (kind) {
    case 0:
        return false;              // 常量 false：ifFalse 必跳
    case 1:
        return g->vars[0] != 0;    // tf.scene_mode（变量表固定顺序第 0 项）
    case 2:
        return g->vars[1] == 1;    // sf.pv == 1
    default:
        return false;
    }
}

const paper_step_t *paper_game_step(paper_game_t *g)
{
    memset(&s_step, 0, sizeof(s_step));
    const paper_scene_t *sc = scene_of(g);
    if (!sc) {
        s_step.kind = PAPER_STEP_ERROR;
        return &s_step;
    }
    for (int guard = 0; guard < 1024; guard++) {
        if (g->cursor >= sc->op_count) {
            g->ended = true;
            s_step.kind = PAPER_STEP_END;
            s_step.end_scene = g->scene_id;
            return &s_step;
        }
        const paper_op_t *op = &g->pack.ops[sc->first_op + g->cursor];
        switch (op->kind) {
        case PAPER_OP_TEXT: {
            const char *text = paper_text_get(&g->pack, op->c, op->b);
            if (!text) {
                g->cursor++;
                continue;
            }
            g->speaker = op->a;
            g->text_op = (int32_t)g->cursor;
            g->choice_op = -1;
            history_push(g, op->a, text);
            s_step.kind = PAPER_STEP_TEXT;
            s_step.speaker = op->a;
            s_step.text = text;
            s_step.background = g->background;
            memcpy(s_step.sprites, g->sprites, sizeof(s_step.sprites));
            g->cursor++;   // 下一条
            return &s_step;
        }
        case PAPER_OP_SPRITE:
            if (op->a < 3) {
                g->sprites[op->a] = op->b;
            }
            break;
        case PAPER_OP_BACKGROUND:
            if (op->a) {
                for (int i = 0; i < 3; i++) {
                    g->sprites[i] = 0xFFFF;
                }
            }
            g->background = op->b;
            break;
        case PAPER_OP_CLEARSPRITES:
            for (int i = 0; i < 3; i++) {
                g->sprites[i] = 0xFFFF;
            }
            break;
        case PAPER_OP_SET:
            if (op->b < 192) {
                int32_t val = (int32_t)op->c;
                g->vars[op->b] = val;
                // 源 progress.js：只有 sf.* 进周目记录，且只在主线阅读时回写
                if (op->b >= 1 && g->story_mode) {
                    bool had = (g->persist_set[op->b >> 3] >> (op->b & 7)) & 1;
                    g->persist_set[op->b >> 3] |= (uint8_t)(1u << (op->b & 7));
                    // "有记录"本身也是持久化状态：0 首次落记录同样要写盘
                    if (!had || g->persist_vars[op->b] != val) {
                        g->persist_vars[op->b] = val;
                        g->meta_dirty = true;
                    }
                }
            }
            break;
        case PAPER_OP_IFFALSE:
            if (!cond_true(g, op->a)) {
                if (op->c < sc->op_count) {
                    g->cursor = op->c;
                    continue;
                }
            }
            break;
        case PAPER_OP_JUMPSCENE: {
            const paper_scene_t *dst = paper_scene_find(&g->pack, op->b);
            if (!dst || op->c >= dst->op_count) {
                s_step.kind = PAPER_STEP_ERROR;
                return &s_step;
            }
            g->scene_id = op->b;
            g->cursor = op->c;
            sc = dst;
            continue;
        }
        case PAPER_OP_CHOICE:
            // 停住：cursor 不动，choice_op 记住位置；choose() 按它取选项
            g->choice_op = (int32_t)g->cursor;
            g->text_op = -1;
            s_step.kind = PAPER_STEP_CHOICE;
            s_step.choice = op->b;
            s_step.choice_count = paper_choice_count(&g->pack, op->b);
            s_step.background = g->background;
            memcpy(s_step.sprites, g->sprites, sizeof(s_step.sprites));
            return &s_step;
        case PAPER_OP_END:
            g->ended = true;
            s_step.kind = PAPER_STEP_END;
            s_step.end_scene = g->scene_id;
            return &s_step;
        default:
            s_step.kind = PAPER_STEP_ERROR;
            return &s_step;
        }
        g->cursor++;
    }
    s_step.kind = PAPER_STEP_ERROR;
    return &s_step;
}

bool paper_game_choose(paper_game_t *g, int index)
{
    const paper_scene_t *sc = scene_of(g);
    if (!sc || g->choice_op < 0 || g->choice_op >= (int32_t)sc->op_count) {
        return false;
    }
    const paper_op_t *op = &g->pack.ops[sc->first_op + g->choice_op];
    if (op->kind != PAPER_OP_CHOICE) {
        return false;
    }
    const paper_choice_item_t *item = paper_choice_item(&g->pack, op->b, index);
    if (!item || item->to >= sc->op_count) {
        return false;
    }
    g->cursor = item->to;
    g->choice_op = -1;
    g->text_op = -1;
    g->ended = false;
    return true;
}

int paper_game_history_count(const paper_game_t *g) { return g->history_count; }

const paper_history_entry_t *paper_game_history_at(const paper_game_t *g, int i)
{
    if (i < 0 || i >= g->history_count) {
        return NULL;
    }
    int idx = (g->history_head - 1 - i + PAPER_HISTORY_MAX * 2) % PAPER_HISTORY_MAX;
    return &g->history[idx];
}

bool paper_game_load_meta(paper_game_t *g)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t buf[15 + sizeof(g->persist_vars) + sizeof(g->persist_set)];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(h, "meta", buf, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(buf)) {
        return false;
    }
    uint32_t crc = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
                   ((uint32_t)buf[3] << 24);
    if (crc != crc32_buf(buf + 4, sizeof(buf) - 4)) {
        ESP_LOGW(TAG, "解锁存档 CRC 不符，忽略");
        return false;
    }
    memcpy(g->cg_unlocked, buf + 4, 10);
    g->endings_seen = buf[14];
    memcpy(g->persist_vars, buf + 15, sizeof(g->persist_vars));
    memcpy(g->persist_set, buf + 15 + sizeof(g->persist_vars), sizeof(g->persist_set));
    return true;
}

bool paper_game_save_meta(paper_game_t *g)
{
    if (!nvs_ready()) {
        g->meta_dirty = true;
        ESP_LOGW(TAG, "NVS 不可用：解锁进度待重试");
        return false;
    }
    uint8_t buf[15 + sizeof(g->persist_vars) + sizeof(g->persist_set)];
    memcpy(buf + 4, g->cg_unlocked, 10);
    buf[14] = g->endings_seen;
    memcpy(buf + 15, g->persist_vars, sizeof(g->persist_vars));
    memcpy(buf + 15 + sizeof(g->persist_vars), g->persist_set, sizeof(g->persist_set));
    uint32_t crc = crc32_buf(buf + 4, sizeof(buf) - 4);
    buf[0] = (uint8_t)crc;
    buf[1] = (uint8_t)(crc >> 8);
    buf[2] = (uint8_t)(crc >> 16);
    buf[3] = (uint8_t)(crc >> 24);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        g->meta_dirty = true;
        ESP_LOGW(TAG, "解锁进度写盘失败（open），待重试");
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "meta", buf, sizeof(buf));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        g->meta_dirty = true;
        ESP_LOGW(TAG, "解锁进度写盘失败（%s），待重试", esp_err_to_name(err));
        return false;
    }
    g->meta_dirty = false;
    return true;
}

void paper_game_flush_meta(paper_game_t *g)
{
    if (g->meta_dirty) {
        (void)paper_game_save_meta(g);
    }
}

bool paper_game_cg_unlocked(const paper_game_t *g, int group)
{
    if (group < 0 || group >= 80) {
        return false;
    }
    return (g->cg_unlocked[group >> 3] >> (group & 7)) & 1;
}

void paper_game_unlock_cg(paper_game_t *g, int group)
{
    if (group < 0 || group >= 80 || paper_game_cg_unlocked(g, group)) {
        return;
    }
    g->cg_unlocked[group >> 3] |= (uint8_t)(1u << (group & 7));
    g->meta_dirty = true;
    (void)paper_game_save_meta(g);   // 失败保留 dirty，稍后 flush 重试
}

int paper_game_unlock_image(paper_game_t *g, uint16_t image_index)
{
    int group = paper_image_cg_group(&g->pack, image_index);
    if (group >= 0) {
        paper_game_unlock_cg(g, group);
    }
    return group;
}

int paper_game_endings_seen(const paper_game_t *g) { return g->endings_seen; }

void paper_game_capture_progress(paper_game_t *g)
{
    for (int i = 1; i < 192; i++) {
        g->persist_vars[i] = g->vars[i];
    }
    g->meta_dirty = true;
    (void)paper_game_save_meta(g);
}

bool paper_nvs_available(void) { return nvs_ready(); }

bool paper_game_var_recorded(const paper_game_t *g, int index)
{
    if (index < 0 || index >= 192) {
        return false;
    }
    return (g->persist_set[index >> 3] >> (index & 7)) & 1;
}

// ---------------------------------------------------------------- 存档
static void save_key(int slot, char *buf, size_t cap)
{
    snprintf(buf, cap, "slot%d", slot);
}

static bool nvs_ready(void)
{
    static bool ready;
    static bool tried;
    if (tried) {
        return ready;
    }
    tried = true;
    // 只初始化一次；不在错误路径上擦除 NVS（避免吞掉用户其它数据）
    esp_err_t err = nvs_flash_init();
    ready = (err == ESP_OK);
    if (!ready) {
        ESP_LOGW(TAG, "NVS 不可用（%s）：本次运行不提供存档，但不擦除现有数据",
                 esp_err_to_name(err));
    }
    return ready;
}

static bool save_valid(const paper_game_t *g, const paper_save_t *sv)
{
    const paper_scene_t *sc = paper_scene_find(&g->pack, sv->scene_id);
    if (!sc) {
        return false;
    }
    if (sv->resume_cursor >= sc->op_count) {
        return false;
    }
    const paper_op_t *op = &g->pack.ops[sc->first_op + sv->resume_cursor];
    if (op->kind != PAPER_OP_TEXT && op->kind != PAPER_OP_CHOICE) {
        return false;
    }
    if (sv->background != 0xFFFF && sv->background >= g->pack.entry_count) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (sv->sprites[i] != 0xFFFF && sv->sprites[i] >= g->pack.entry_count) {
            return false;
        }
    }
    if (sv->speaker >= g->pack.speaker_count) {
        return false;
    }
    return true;
}

bool paper_game_save(paper_game_t *g, int slot)
{
    const paper_scene_t *sc = scene_of(g);
    if (slot < 0 || slot > PAPER_SLOT_AUTO || !sc || !nvs_ready()) {
        return false;
    }
    int32_t resume = g->choice_op >= 0 ? g->choice_op : g->text_op;
    if (resume < 0 || resume >= (int32_t)sc->op_count) {
        return false;
    }
    paper_save_t sv;
    memset(&sv, 0, sizeof(sv));
    sv.magic = SAVE_MAGIC;
    sv.ver = SAVE_VER;
    sv.scene_id = g->scene_id;
    sv.resume_cursor = (uint32_t)resume;
    sv.background = g->background;
    memcpy(sv.sprites, g->sprites, sizeof(sv.sprites));
    sv.speaker = g->speaker;
    sv.endings_seen = g->endings_seen;
    sv.story_mode = g->story_mode ? 1 : 0;
    memcpy(sv.vars, g->vars, sizeof(sv.vars));
    memcpy(sv.cg, g->cg_unlocked, sizeof(sv.cg));
    sv.stamp = (uint32_t)(esp_timer_get_time() / 1000000);
    sv.crc = crc32_buf((const uint8_t *)&sv + SAVE_FIELDS_OFF, SAVE_CRC_LEN);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    char key[12];
    save_key(slot, key, sizeof(key));
    esp_err_t err = nvs_set_blob(h, key, &sv, sizeof(sv));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool load_raw(int slot, paper_save_t *sv)
{
    if (slot < 0 || slot > PAPER_SLOT_AUTO || !nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[12];
    save_key(slot, key, sizeof(key));
    size_t len = sizeof(*sv);
    esp_err_t err = nvs_get_blob(h, key, sv, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*sv)) {
        return false;
    }
    if (sv->magic != SAVE_MAGIC) {
        return false;
    }
    if (sv->ver != SAVE_VER) {
        ESP_LOGW(TAG, "存档 %d 版本 %u 与当前 %u 不符，拒绝加载", slot, sv->ver, SAVE_VER);
        return false;
    }
    uint32_t crc = crc32_buf((const uint8_t *)sv + SAVE_FIELDS_OFF, SAVE_CRC_LEN);
    if (crc != sv->crc) {
        ESP_LOGW(TAG, "存档 %d CRC 不符", slot);
        return false;
    }
    return true;
}

bool paper_game_slot_exists(int slot)
{
    paper_save_t sv;
    return load_raw(slot, &sv);
}

bool paper_game_load(paper_game_t *g, int slot)
{
    paper_save_t sv;
    if (!load_raw(slot, &sv) || !save_valid(g, &sv)) {
        return false;
    }
    g->scene_id = sv.scene_id;
    g->story_mode = sv.story_mode ? true : false;
    g->cursor = sv.resume_cursor;
    g->text_op = -1;
    g->choice_op = -1;
    g->background = sv.background;
    memcpy(g->sprites, sv.sprites, sizeof(g->sprites));
    g->speaker = sv.speaker;
    g->ended = false;
    // 源 engine.js restore + withProgress 语义：
    //   主线档：按"有记录的 sf.* 键"用最新全局值覆盖（0 也是合法值，不做 OR）；
    //   章节档：用存档自身变量，不回写全局。
    memcpy(g->vars, sv.vars, sizeof(g->vars));
    if (g->story_mode) {
        for (int i = 1; i < 192; i++) {
            if (g->persist_set[i >> 3] & (1u << (i & 7))) {
                g->vars[i] = g->persist_vars[i];
            }
        }
    }
    for (int i = 0; i < 10; i++) {
        g->cg_unlocked[i] |= sv.cg[i];
    }
    g->endings_seen |= sv.endings_seen;
    g->history_count = 0;
    g->history_head = 0;
    paper_game_save_meta(g);
    return true;
}

bool paper_game_slot_brief(int slot, const paper_pack_t *pack, char *out, size_t cap)
{
    paper_save_t sv;
    if (!load_raw(slot, &sv)) {
        return false;
    }
    const paper_scene_t *sc = paper_scene_find(pack, sv.scene_id);
    snprintf(out, cap, "%s", sc ? paper_scene_name(pack, sc) : "（场景无效）");
    return true;
}

void paper_game_delete_slot(int slot)
{
    if (slot < 0 || slot > PAPER_SLOT_AUTO || !nvs_ready()) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[12];
        save_key(slot, key, sizeof(key));
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}
