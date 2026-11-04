// main/parquet/parquet_game.c —— 剧情状态机 / NVS 存档 / 全局周目记录。
//
// NVS 约定：
//   namespace "pqsave"  s01..s30 + auto  存档（版本 + 长度 + CRC + 全字段校验）
//   namespace "pqmeta"  prog             全局周目记录（4 个 sf.* 标记 + 37 个 CG 组 + 3 个结局）
//   namespace "pqcfg"   cfg              阅读速度 / 自动速度 / 标题背景
// NVS 打不开时只降级为"本次运行不落盘"，绝不擦除或覆盖其它数据。
#include "parquet_game.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "pq_game";

#define SAVE_NS "pqsave"
#define META_NS "pqmeta"
#define CFG_NS "pqcfg"

#define SAVE_MAGIC 0x56535150u   // "PQSV"
#define META_MAGIC 0x31505150u   // "PQP1"
#define CFG_MAGIC 0x31435150u    // "PQC1"
#define SAVE_VERSION 2
#define META_VERSION 2
#define CFG_VERSION 2

#define NO_OP 0xFFFFFFFFu

// ---------------------------------------------------------------- 工具
static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void copy_utf8_prefix(char *dst, size_t cap, const char *src, size_t max_bytes)
{
    if (cap == 0) {
        return;
    }
    size_t n = 0;
    if (src) {
        while (src[n] && n < max_bytes && n + 1 < cap) {
            unsigned char c = (unsigned char)src[n];
            size_t bytes = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (n + bytes > max_bytes || n + bytes + 1 > cap) {
                break;
            }
            n += bytes;
        }
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

static int var_index(const pq_game_t *game, uint16_t index)
{
    return index < game->pack.var_count && index < PQ_VAR_MAX ? (int)index : -1;
}

static bool var_is_story_pack(const pq_pack_t *pack, uint16_t index)
{
    const char *name = pq_var_name(pack, index);
    return strncmp(name, "sf.", 3) == 0;
}

static bool var_is_story(const pq_game_t *game, uint16_t index)
{
    return var_is_story_pack(&game->pack, index);
}

static void set_var(pq_game_t *game, uint16_t index, int32_t value, bool persist)
{
    int i = var_index(game, index);
    if (i < 0) {
        return;
    }
    game->vars[i] = value;
    game->var_set |= (uint8_t)(1u << i);
    if (persist && var_is_story(game, index)) {
        if (!(game->pvar_set & (1u << i)) || game->pvars[i] != value) {
            game->pvars[i] = value;
            game->pvar_set |= (uint8_t)(1u << i);
            game->meta_dirty = true;
        }
    }
}

static void apply_global_vars(pq_game_t *game)
{
    for (int i = 0; i < PQ_VAR_MAX && i < game->pack.var_count; i++) {
        if (game->pvar_set & (1u << i)) {
            game->vars[i] = game->pvars[i];
            game->var_set |= (uint8_t)(1u << i);
        }
    }
}

static void history_push(pq_game_t *game, uint16_t speaker, uint8_t tag, const char *text)
{
    int slot;
    if (game->hist_count < PQ_HISTORY_MAX) {
        slot = game->hist_head;
        game->hist_count++;
    } else {
        slot = game->hist_head;
    }
    game->history[slot].speaker = speaker;
    game->history[slot].tag = tag;
    copy_utf8_prefix(game->history[slot].text, sizeof(game->history[slot].text), text,
                     sizeof(game->history[slot].text) - 1);
    game->hist_head = (game->hist_head + 1) % PQ_HISTORY_MAX;
}

// ---------------------------------------------------------------- NVS
static bool nvs_read_blob(const char *ns, const char *key, void *buf, size_t cap,
                          size_t *out_len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    if (out_len) {
        *out_len = len;
    }
    return true;
}

static bool nvs_write_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

// ---------------------------------------------------------------- 存档格式
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t total;
    uint16_t scene;
    uint16_t tag;
    uint8_t mode;
    uint8_t story;
    uint8_t sel;
    uint8_t page;
    uint16_t visual;      // 最近展示的视觉组合（scene 98 这类纯选项场景靠它恢复画面）
    uint32_t text_op;
    uint32_t cursor;
    uint32_t saved_at;
    uint8_t var_count;
    uint8_t var_ids[PQ_VAR_MAX];
    int32_t var_vals[PQ_VAR_MAX];
    char preview[PQ_PREVIEW_BYTES];
    uint32_t crc;
} pq_save_blob_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t total;
    uint8_t pvar_set;
    uint8_t reserved;
    uint16_t ending_seen;
    uint64_t cg_seen;
    int32_t pvars[PQ_VAR_MAX];
    uint32_t crc;
} pq_meta_blob_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t total;
    uint8_t speed;
    uint8_t auto_speed;
    uint8_t title_bg;
    uint8_t font;
    uint32_t crc;
} pq_cfg_blob_t;

static void slot_key(int slot, char *out, size_t cap)
{
    if (slot == PQ_SLOT_AUTO) {
        snprintf(out, cap, "auto");
    } else {
        snprintf(out, cap, "s%02d", slot + 1);
    }
}

static bool save_blob_valid_pack(const pq_pack_t *pack, const pq_save_blob_t *b, size_t len)
{
    if (len != sizeof(*b) || b->magic != SAVE_MAGIC || b->version != SAVE_VERSION ||
        b->total != sizeof(*b)) {
        return false;
    }
    uint32_t crc = crc32_le(0, (const uint8_t *)b, offsetof(pq_save_blob_t, crc));
    if (crc != b->crc) {
        return false;
    }
    const pq_scene_t *scene = pq_scene_by_id(pack, b->scene);
    if (!scene || b->mode > 1 || b->page > 8 || b->var_count > PQ_VAR_MAX ||
        b->cursor > scene->op_count) {
        return false;
    }
    if (b->mode == 0 && b->text_op >= scene->op_count) {
        return false;
    }
    if (b->mode == 1 && b->cursor >= scene->op_count) {
        return false;
    }
    if (b->visual != 0xFFFF && b->visual >= pack->visual_count) {
        return false;
    }
    for (uint8_t i = 0; i < b->var_count; i++) {
        if (b->var_ids[i] >= pack->var_count) {
            return false;
        }
    }
    if (memchr(b->preview, 0, sizeof(b->preview)) == NULL) {
        return false;
    }
    return true;
}

static void fill_save_blob(const pq_game_t *game, pq_save_blob_t *b, uint8_t page,
                           const char *preview)
{
    memset(b, 0, sizeof(*b));
    b->magic = SAVE_MAGIC;
    b->version = SAVE_VERSION;
    b->total = sizeof(*b);
    b->scene = game->scene;
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    b->tag = scene ? scene->tag : 0;
    b->mode = game->mode;
    b->story = game->story ? 1 : 0;
    b->sel = game->choice_sel;
    b->page = page;
    b->visual = game->current_visual;
    b->text_op = game->text_op;
    b->cursor = game->cursor;
    b->saved_at = (uint32_t)(esp_timer_get_time() / 1000000);
    uint8_t n = 0;
    for (int i = 0; i < PQ_VAR_MAX && i < game->pack.var_count; i++) {
        if (game->var_set & (1u << i)) {
            b->var_ids[n] = (uint8_t)i;
            b->var_vals[n] = game->vars[i];
            n++;
        }
    }
    b->var_count = n;
    copy_utf8_prefix(b->preview, sizeof(b->preview), preview ? preview : "",
                     sizeof(b->preview) - 1);
    b->crc = crc32_le(0, (const uint8_t *)b, offsetof(pq_save_blob_t, crc));
}

// ---------------------------------------------------------------- 生命周期
#ifdef PQ_PACK_USE_HOST_BLOBS
static const uint8_t *s_host_img;
static size_t s_host_img_size;
static const uint8_t *s_host_scn;
static size_t s_host_scn_size;

bool pq_host_set_pack(const uint8_t *img, size_t img_size, const uint8_t *scn,
                      size_t scn_size)
{
    s_host_img = img;
    s_host_img_size = img_size;
    s_host_scn = scn;
    s_host_scn_size = scn_size;
    return img && scn && img_size > 64 && scn_size > 128;
}
#endif

bool pq_game_init(pq_game_t *game)
{
    memset(game, 0, sizeof(*game));
    game->text_op = NO_OP;
    game->nvs_ok = true;
    char err[64] = {0};
    const uint8_t *img_data;
    size_t img_size;
    const uint8_t *scn_data;
    size_t scn_size;
#ifdef PQ_PACK_USE_HOST_BLOBS
    if (!s_host_img) {
        snprintf(game->init_error, sizeof(game->init_error), "宿主测试未加载资源包");
        return false;
    }
    img_data = s_host_img;
    img_size = s_host_img_size;
    scn_data = s_host_scn;
    scn_size = s_host_scn_size;
#else
    extern const uint8_t parquet_img_bin_start[] asm("_binary_parquet_img_bin_start");
    extern const uint8_t parquet_img_bin_end[] asm("_binary_parquet_img_bin_end");
    extern const uint8_t parquet_scn_bin_start[] asm("_binary_parquet_scn_bin_start");
    extern const uint8_t parquet_scn_bin_end[] asm("_binary_parquet_scn_bin_end");
    img_data = parquet_img_bin_start;
    img_size = (size_t)(parquet_img_bin_end - parquet_img_bin_start);
    scn_data = parquet_scn_bin_start;
    scn_size = (size_t)(parquet_scn_bin_end - parquet_scn_bin_start);
#endif
    if (!pq_pack_init(&game->pack, img_data, img_size, scn_data, scn_size, err,
                      sizeof(err))) {
        snprintf(game->init_error, sizeof(game->init_error), "%s",
                 err[0] ? err : "资源包解析失败");
        ESP_LOGE(TAG, "资源包初始化失败：%s", game->init_error);
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(META_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_close(h);
    } else {
        game->nvs_ok = false;
        ESP_LOGW(TAG, "NVS 不可用：本次运行不保存进度");
    }
    // 全局周目记录
    pq_meta_blob_t meta;
    size_t len = 0;
    if (nvs_read_blob(META_NS, "prog", &meta, sizeof(meta), &len) &&
        len == sizeof(meta) && meta.magic == META_MAGIC && meta.version == META_VERSION &&
        meta.total == sizeof(meta) &&
        crc32_le(0, (const uint8_t *)&meta, offsetof(pq_meta_blob_t, crc)) == meta.crc) {
        game->pvar_set = meta.pvar_set & 0xFF;
        memcpy(game->pvars, meta.pvars, sizeof(game->pvars));
        game->cg_seen = meta.cg_seen;
        game->ending_seen = meta.ending_seen;
    }
    game->meta_dirty = false;
    return true;
}

bool pq_game_nvs_ok(const pq_game_t *game) { return game->nvs_ok; }

void pq_game_start(pq_game_t *game, uint16_t scene, bool story)
{
    game->scene = scene;
    game->cursor = 0;
    game->text_op = NO_OP;
    game->mode = 0;
    game->choice_sel = 0;
    game->current_visual = 0xFFFF;
    game->story = story;
    memset(game->vars, 0, sizeof(game->vars));
    game->var_set = 0;
    game->hist_count = 0;
    game->hist_head = 0;
    apply_global_vars(game);
}

static void finish_run(pq_game_t *game, pq_step_t *step)
{
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    if (game->story && scene && scene->clear_var != 0xFF) {
        set_var(game, scene->clear_var, 1, true);
    }
    int ending = game->story ? pq_ending_index(&game->pack, game->scene) : -1;
    if (ending >= 0 && ending < 16) {
        if (!(game->ending_seen & (1u << ending))) {
            game->ending_seen |= (uint16_t)(1u << ending);
            game->meta_dirty = true;
        }
    }
    step->kind = PQ_STEP_END;
    step->scene = game->scene;
    step->op = game->cursor;
    step->choice = 0xFFFF;
    step->choice_count = 0;
    step->tag = scene ? scene->tag : 0;
    step->text = pq_ending_name(&game->pack, game->scene);
    game->mode = 0;
    game->text_op = NO_OP;
    game->cursor = scene ? scene->op_count : 0;
}

bool pq_game_step(pq_game_t *game, pq_step_t *step)
{
    memset(step, 0, sizeof(*step));
    step->speaker = 0xFFFF;
    step->kind = PQ_STEP_NONE;
    if (game->mode == 1) {
        // 待选：重复返回同一个选项，不推进
        const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
        const pq_op_t *op = pq_op(&game->pack, scene, game->cursor);
        if (!op || op->kind != PQ_OP_CHOICE) {
            step->kind = PQ_STEP_ERROR;
            step->error = "待选状态损坏";
            return false;
        }
        const pq_choice_t *choice = pq_choice(&game->pack, op->a);
        step->kind = PQ_STEP_CHOICE;
        step->scene = game->scene;
        step->op = game->cursor;
        step->choice = op->a;
        step->choice_count = choice ? choice->count : 0;
        step->tag = scene->tag;
        return true;
    }
    for (int guard = 0; guard < 256; guard++) {
        const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
        if (!scene) {
            step->kind = PQ_STEP_ERROR;
            step->error = "场景不存在";
            return false;
        }
        if (game->cursor >= scene->op_count) {
            finish_run(game, step);
            return true;
        }
        const pq_op_t *op = pq_op(&game->pack, scene, game->cursor);
        if (!op) {
            step->kind = PQ_STEP_ERROR;
            step->error = "op 越界";
            return false;
        }
        if (op->kind == PQ_OP_JUMP) {
            const pq_scene_t *target = pq_scene_by_id(&game->pack, op->a);
            if (!target || op->c >= target->op_count) {
                step->kind = PQ_STEP_ERROR;
                step->error = "跳转目标无效";
                return false;
            }
            game->scene = op->a;
            game->cursor = op->c;
            game->text_op = NO_OP;
            continue;
        }
        if (op->kind == PQ_OP_CHOICE) {
            const pq_choice_t *choice = pq_choice(&game->pack, op->a);
            if (!choice) {
                step->kind = PQ_STEP_ERROR;
                step->error = "选项表无效";
                return false;
            }
            game->mode = 1;
            if (game->choice_sel >= choice->count) {
                game->choice_sel = 0;
            }
            step->kind = PQ_STEP_CHOICE;
            step->scene = game->scene;
            step->op = game->cursor;
            step->choice = op->a;
            step->choice_count = choice->count;
            step->tag = scene->tag;
            return true;
        }
        if (op->kind == PQ_OP_END) {
            finish_run(game, step);
            return true;
        }
        // TEXT
        const pq_visual_t *visual = pq_visual(&game->pack, op);
        if (!visual) {
            step->kind = PQ_STEP_ERROR;
            step->error = "视觉表无效";
            return false;
        }
        const char *text = pq_text_get(&game->pack, op->c, (uint16_t)op->d);
        if (!text) {
            step->kind = PQ_STEP_ERROR;
            step->error = "正文块解码失败";
            return false;
        }
        game->current_visual = op->a;
        step->kind = PQ_STEP_TEXT;
        step->scene = game->scene;
        step->op = game->cursor;
        step->speaker = op->b;
        step->visual = op->a;
        step->tag = op->tag;
        step->text = text;
        game->text_op = game->cursor;
        game->cursor++;
        game->mode = 0;
        history_push(game, op->b, op->tag, text);
        return true;
    }
    step->kind = PQ_STEP_ERROR;
    step->error = "跳转次数过多";
    return false;
}

int pq_game_choice_count(const pq_game_t *game)
{
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    const pq_op_t *op = pq_op(&game->pack, scene, game->cursor);
    if (!op || op->kind != PQ_OP_CHOICE) {
        return 0;
    }
    const pq_choice_t *choice = pq_choice(&game->pack, op->a);
    return choice ? choice->count : 0;
}

const pq_choice_item_t *pq_game_choice_item(const pq_game_t *game, int index)
{
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    const pq_op_t *op = pq_op(&game->pack, scene, game->cursor);
    if (!op || op->kind != PQ_OP_CHOICE) {
        return NULL;
    }
    const pq_choice_t *choice = pq_choice(&game->pack, op->a);
    return choice ? pq_choice_item(&game->pack, choice, (uint16_t)index) : NULL;
}

bool pq_game_choose(pq_game_t *game, int index)
{
    const pq_choice_item_t *item = pq_game_choice_item(game, index);
    if (!item || game->mode != 1) {
        return false;
    }
    if (item->has_effect) {
        set_var(game, item->var, item->value, true);
    }
    game->choice_sel = (uint8_t)index;
    game->scene = item->scene;
    game->cursor = item->to;
    game->text_op = NO_OP;
    game->mode = 0;
    return true;
}

const pq_visual_t *pq_game_visual(const pq_game_t *game)
{
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    const pq_op_t *op = pq_op(&game->pack, scene, game->text_op);
    return pq_visual(&game->pack, op);
}

int pq_game_history_count(const pq_game_t *game) { return game->hist_count; }

const pq_history_t *pq_game_history_at(const pq_game_t *game, int index)
{
    if (index < 0 || index >= game->hist_count) {
        return NULL;
    }
    int start = (game->hist_head - game->hist_count + PQ_HISTORY_MAX * 2) % PQ_HISTORY_MAX;
    int slot = (start + index) % PQ_HISTORY_MAX;
    return &game->history[slot];
}

bool pq_game_var(const pq_game_t *game, uint16_t index, int32_t *out)
{
    int i = var_index(game, index);
    if (i < 0 || !(game->var_set & (1u << i))) {
        return false;
    }
    if (out) {
        *out = game->vars[i];
    }
    return true;
}

bool pq_game_global_var(const pq_game_t *game, uint16_t index, int32_t *out)
{
    int i = var_index(game, index);
    if (i < 0 || !(game->pvar_set & (1u << i))) {
        return false;
    }
    if (out) {
        *out = game->pvars[i];
    }
    return true;
}

bool pq_game_global_var_recorded(const pq_game_t *game, uint16_t index)
{
    int i = var_index(game, index);
    return i >= 0 && (game->pvar_set & (1u << i)) != 0;
}

void pq_game_mark_cg(pq_game_t *game, uint16_t asset_id)
{
    int group = pq_gallery_of_asset(&game->pack, asset_id);
    if (group < 0 || group >= 64) {
        return;
    }
    uint64_t bit = 1ull << group;
    if (!(game->cg_seen & bit)) {
        game->cg_seen |= bit;
        game->meta_dirty = true;
    }
}

bool pq_game_cg_unlocked(const pq_game_t *game, uint16_t group)
{
    if (group >= 64) {
        return false;   // 不存在的组不算已解锁
    }
    return (game->cg_seen & (1ull << group)) != 0;
}

uint16_t pq_game_ending_count(const pq_game_t *game)
{
    return game->pack.ending_name_count;
}

bool pq_game_ending_seen(const pq_game_t *game, uint16_t name_index)
{
    return name_index < 16 && (game->ending_seen & (1u << name_index)) != 0;
}

// ---------------------------------------------------------------- 存档
bool pq_game_save(pq_game_t *game, int slot, uint8_t page)
{
    if (!game->nvs_ok || slot < 0 || slot > PQ_SLOT_AUTO) {
        return false;
    }
    const char *preview = "";
    const pq_scene_t *scene = pq_scene_by_id(&game->pack, game->scene);
    const pq_op_t *op = pq_op(&game->pack, scene, game->text_op);
    if (op) {
        preview = pq_text_get(&game->pack, op->c, (uint16_t)op->d);
    }
    pq_save_blob_t blob;
    fill_save_blob(game, &blob, page, preview);
    char key[8];
    slot_key(slot, key, sizeof(key));
    return nvs_write_blob(SAVE_NS, key, &blob, sizeof(blob));
}

bool pq_game_load(pq_game_t *game, int slot, uint8_t *page_out)
{
    if (slot < 0 || slot > PQ_SLOT_AUTO) {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof(key));
    pq_save_blob_t blob;
    size_t len = 0;
    if (!nvs_read_blob(SAVE_NS, key, &blob, sizeof(blob), &len)) {
        return false;
    }
    if (!save_blob_valid_pack(&game->pack, &blob, len)) {
        return false;
    }
    // 先在新状态的小数组里算好，再一次性提交：pq_game_t 里含 8 KB 正文块缓冲，
    // 不能整份复制到 8 KB 的输入任务栈上（会栈溢出）。
    int32_t vars[PQ_VAR_MAX] = {0};
    int32_t pvars[PQ_VAR_MAX];
    memcpy(pvars, game->pvars, sizeof(pvars));
    uint8_t var_set = 0;
    uint8_t pvar_set = game->pvar_set;
    bool dirty = game->meta_dirty;
    for (uint8_t i = 0; i < blob.var_count; i++) {
        uint16_t vi = blob.var_ids[i];
        int idx = var_index(game, vi);
        if (idx < 0) {
            continue;
        }
        vars[idx] = blob.var_vals[i];
        var_set |= (uint8_t)(1u << idx);
        // 读旧档只合并新增位：全局已记录的 sf.* 不被旧档覆盖。
        if (var_is_story_pack(&game->pack, vi) && !(pvar_set & (1u << idx))) {
            pvars[idx] = blob.var_vals[i];
            pvar_set |= (uint8_t)(1u << idx);
            dirty = true;
        }
    }
    for (int i = 0; i < PQ_VAR_MAX && i < game->pack.var_count; i++) {
        if (pvar_set & (1u << i)) {
            vars[i] = pvars[i];
            var_set |= (uint8_t)(1u << i);
        }
    }
    uint32_t cursor = blob.cursor;
    if (blob.mode == 0) {
        // 回到保存时显示的那一句：从该 op 重新执行（视觉快照随 op 恢复）。
        cursor = blob.text_op;
    } else {
        const pq_scene_t *scene = pq_scene_by_id(&game->pack, blob.scene);
        if (!scene || blob.cursor >= scene->op_count) {
            return false;
        }
    }
    memcpy(game->vars, vars, sizeof(vars));
    memcpy(game->pvars, pvars, sizeof(pvars));
    game->var_set = var_set;
    game->pvar_set = pvar_set;
    game->meta_dirty = dirty;
    game->current_visual = blob.visual;
    game->scene = blob.scene;
    game->story = blob.story != 0;
    game->mode = blob.mode;
    game->choice_sel = blob.sel;
    game->cursor = cursor;
    game->text_op = NO_OP;
    game->hist_count = 0;
    game->hist_head = 0;
    if (page_out) {
        *page_out = blob.page;
    }
    return true;
}

bool pq_game_slot_peek(int slot, const pq_pack_t *pack, pq_save_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot > PQ_SLOT_AUTO || !pack) {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof(key));
    pq_save_blob_t blob;
    size_t len = 0;
    if (!nvs_read_blob(SAVE_NS, key, &blob, sizeof(blob), &len) || len != sizeof(blob)) {
        return false;
    }
    if (!save_blob_valid_pack(pack, &blob, len)) {
        out->valid = false;
        return true;   // 存在但损坏：UI 显示"存档无法读取"
    }
    out->valid = true;
    out->scene = blob.scene;
    out->page = blob.page;
    out->mode = blob.mode;
    out->tag = blob.tag;
    out->story = blob.story;
    memcpy(out->preview, blob.preview, sizeof(out->preview));
    out->saved_at = blob.saved_at;
    return true;
}

bool pq_game_slot_exists(int slot)
{
    if (slot < 0 || slot > PQ_SLOT_AUTO) {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(SAVE_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 0;
}

bool pq_game_delete_slot(int slot)
{
    if (slot < 0 || slot > PQ_SLOT_AUTO) {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(SAVE_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return false;
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// ---------------------------------------------------------------- 全局记录 / 设置
bool pq_game_save_meta(pq_game_t *game)
{
    if (!game->nvs_ok) {
        return false;
    }
    pq_meta_blob_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = META_MAGIC;
    meta.version = META_VERSION;
    meta.total = sizeof(meta);
    meta.pvar_set = game->pvar_set;
    meta.ending_seen = game->ending_seen;
    meta.cg_seen = game->cg_seen;
    memcpy(meta.pvars, game->pvars, sizeof(meta.pvars));
    meta.crc = crc32_le(0, (const uint8_t *)&meta, offsetof(pq_meta_blob_t, crc));
    if (!nvs_write_blob(META_NS, "prog", &meta, sizeof(meta))) {
        return false;
    }
    game->meta_dirty = false;
    return true;
}

void pq_game_flush_meta(pq_game_t *game)
{
    if (game->meta_dirty) {
        (void)pq_game_save_meta(game);
    }
}

bool pq_settings_load(pq_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->speed = 1;
    out->auto_speed = 1;
    out->title_bg = 0;
    pq_cfg_blob_t cfg;
    size_t len = 0;
    if (!nvs_read_blob(CFG_NS, "cfg", &cfg, sizeof(cfg), &len) || len != sizeof(cfg) ||
        cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION ||
        cfg.total != sizeof(cfg) ||
        crc32_le(0, (const uint8_t *)&cfg, offsetof(pq_cfg_blob_t, crc)) != cfg.crc) {
        return false;
    }
    if (cfg.speed <= 3) {
        out->speed = cfg.speed;
    }
    if (cfg.auto_speed <= 2) {
        out->auto_speed = cfg.auto_speed;
    }
    if (cfg.title_bg <= 2) {
        out->title_bg = cfg.title_bg;
    }
    if (cfg.font <= 1) {
        out->font = cfg.font;
    }
    return true;
}

bool pq_settings_save(const pq_settings_t *settings)
{
    pq_cfg_blob_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CFG_MAGIC;
    cfg.version = CFG_VERSION;
    cfg.total = sizeof(cfg);
    cfg.speed = settings->speed;
    cfg.auto_speed = settings->auto_speed;
    cfg.title_bg = settings->title_bg;
    cfg.font = settings->font;
    cfg.crc = crc32_le(0, (const uint8_t *)&cfg, offsetof(pq_cfg_blob_t, crc));
    return nvs_write_blob(CFG_NS, "cfg", &cfg, sizeof(cfg));
}
