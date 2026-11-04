// tests/host/test_parquet.c —— 生产 C 的资源/剧情/存档/图像回归。
//
// 覆盖：包结构、全部 op 与正文解码、路线/分支/结局、章节模式、存档往返与故障注入、
// 全局周目记录、CG 解锁、全部图片解码（与 parquet_verify.py 独立参考解码逐像素/
// 网格对照）、坏图与分配失败。
#include "parquet_game.h"
#include "parquet_gfx.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                               \
    do {                                                                                 \
        g_checks++;                                                                      \
        long long _a = (long long)(a), _b = (long long)(b);                              \
        if (_a != _b) {                                                                  \
            printf("FAIL %s:%d: %s=%lld != %s=%lld\n", __FILE__, __LINE__, #a, _a, #b,   \
                   _b);                                                                  \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        printf("short read %s\n", path);
        exit(2);
    }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

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

// ---------------------------------------------------------------- 路线
typedef struct {
    int16_t ending_name;
    uint16_t end_scene;
    int steps;
    int choices;
    int32_t vars[PQ_VAR_MAX];
    uint8_t var_set;
} walk_t;

static bool run_to_end(pq_game_t *g, unsigned choice_mask, walk_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ending_name = -1;
    unsigned bits = 0;
    for (int i = 0; i < 300000; i++) {
        pq_step_t step;
        if (!pq_game_step(g, &step)) {
            return false;
        }
        out->steps++;
        if (step.kind == PQ_STEP_CHOICE) {
            out->choices++;
            int pick = (int)((choice_mask >> bits) & 1u);
            bits++;
            if (pick >= step.choice_count) {
                pick = 0;
            }
            if (!pq_game_choose(g, pick)) {
                return false;
            }
            continue;
        }
        if (step.kind == PQ_STEP_END) {
            out->ending_name = pq_ending_index(&g->pack, step.scene);
            out->end_scene = step.scene;
            out->var_set = g->var_set;
            memcpy(out->vars, g->vars, sizeof(out->vars));
            return true;
        }
        if (step.kind != PQ_STEP_TEXT) {
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 包结构
static void check_pack(pq_pack_t *pack)
{
    CHECK_EQ_INT(pack->scene_count, 107);
    CHECK_EQ_INT(pack->op_count, 11205);
    CHECK_EQ_INT(pack->visual_count, 2623);
    CHECK_EQ_INT(pack->text_block_count, 361);   // 32-op 块中无正文的块不占表
    CHECK_EQ_INT(pack->chapter_count, 37);
    CHECK_EQ_INT(pack->route_count, 3);
    CHECK_EQ_INT(pack->gallery_group_count, 37);
    CHECK_EQ_INT(pack->gallery_member_count, 367);
    CHECK_EQ_INT(pack->choice_count, 2);
    CHECK_EQ_INT(pack->choice_item_count, 4);
    CHECK_EQ_INT(pack->ending_count, 107);
    CHECK_EQ_INT(pack->speaker_count, 54);
    CHECK_EQ_INT(pack->var_count, 5);
    CHECK_EQ_INT(pack->initial, 5);
    CHECK_EQ_INT(pack->flow, 5);
    CHECK_EQ_INT(pack->side, 1);
    CHECK_EQ_INT(pack->after, 93);
    CHECK_EQ_INT(pack->img_entry_count, 1510);
    CHECK_EQ_INT(pack->sprite_count, 979);
    CHECK_EQ_INT(pack->cutin_count, 101);

    int texts = 0, jumps = 0, choices = 0, ends = 0;
    for (uint32_t i = 0; i < pack->op_count; i++) {
        const pq_op_t *op = &pack->ops[i];
        switch (op->kind) {
        case PQ_OP_TEXT: {
            texts++;
            CHECK(op->a < pack->visual_count);
            CHECK(op->tag < pack->tag_count);
            CHECK(op->b == 0xFFFF || op->b < pack->speaker_count);
            CHECK(op->d < 512);   // 运行期正文缓冲 512B（PQ_TEXT_MAX）
            const char *text = pq_text_get(pack, op->c, (uint16_t)op->d);
            static int s_text_null_logged;
            if (!text && s_text_null_logged < 5) {
                s_text_null_logged++;
                printf("text null op=%u off=%u len=%u\n", i, (unsigned)op->c,
                       (unsigned)op->d);
            }
            CHECK(text != NULL);
            if (text) {
                CHECK_EQ_INT(strlen(text), op->d);
                CHECK(text[0] != 0);
            }
            const pq_visual_t *v = &pack->visuals[op->a];
            if (v->bg != 0xFFFF) {
                CHECK(pq_asset_image(pack, v->bg) != PQ_NO_IMAGE);
            }
            for (int b = 0; b < 3; b++) {
                if (v->body[b] != 0xFFFF) {
                    CHECK(pq_asset_sprite(pack, v->body[b]) != NULL);
                }
            }
            if (v->cutin != 0xFFFF) {
                CHECK(pq_asset_cutin(pack, v->cutin) != NULL);
            }
            break;
        }
        case PQ_OP_JUMP:
            jumps++;
            CHECK(pq_scene_by_id(pack, op->a) != NULL);
            break;
        case PQ_OP_CHOICE: {
            choices++;
            const pq_choice_t *c = pq_choice(pack, op->a);
            CHECK(c != NULL);
            if (c) {
                CHECK(c->count >= 1);
                for (uint16_t k = 0; k < c->count; k++) {
                    const pq_choice_item_t *it = pq_choice_item(pack, c, k);
                    CHECK(it != NULL);
                    if (it) {
                        CHECK(pq_scene_by_id(pack, it->scene) != NULL);
                        CHECK(pq_text_get(pack, it->text_off, it->text_len) != NULL);
                    }
                }
            }
            break;
        }
        case PQ_OP_END:
            ends++;
            break;
        default:
            CHECK(0);
            break;
        }
    }
    CHECK_EQ_INT(texts, 11098);
    CHECK_EQ_INT(jumps, 101);
    CHECK_EQ_INT(choices, 2);
    CHECK_EQ_INT(ends, 4);

    for (uint16_t s = 0; s < pack->scene_count; s++) {
        const pq_scene_t *sc = &pack->scenes[s];
        CHECK(sc->op_count > 0);
        CHECK((uint32_t)sc->first_op + sc->op_count <= pack->op_count);
        CHECK(sc->tag < pack->tag_count);
        CHECK(sc->clear_var == 0xFF || sc->clear_var < pack->var_count);
        CHECK(strlen(pq_scene_by_id(pack, sc->id) ? "x" : "") == 1);
    }
    // 立绘几何：身体/表情都在画布内，锚点有效
    for (uint16_t i = 0; i < pack->sprite_count; i++) {
        const pq_sprite_t *sp = &pack->sprites[i];
        const pq_img_entry_t *body = pq_img_entry(pack, sp->body);
        CHECK(body != NULL && body->kind == PQ_IMG_BLOCK);
        if (body) {
            CHECK(sp->body_dx >= 0 && sp->body_dy >= 0);
            CHECK(sp->body_dx + body->w <= sp->canvas_w);
            CHECK(sp->body_dy + body->h <= 150);
        }
        CHECK(sp->anchor_x >= 0);
        CHECK(sp->anchor_x <= (int)sp->canvas_w);
        if (sp->patch != PQ_NO_IMAGE) {
            const pq_img_entry_t *patch = pq_img_entry(pack, sp->patch);
            CHECK(patch != NULL && patch->kind == PQ_IMG_PATCH);
            if (patch) {
                CHECK(sp->patch_dx >= 0 && sp->patch_dy >= 0);
                CHECK(sp->patch_dx + patch->w <= sp->canvas_w);
                CHECK(sp->patch_dy + patch->h <= 150);
            }
        }
    }
    // 鉴赏组标签与成员
    for (uint16_t gi = 0; gi < pack->gallery_group_count; gi++) {
        CHECK(pq_gallery_label(pack, gi)[0] != 0);
        CHECK(pq_gallery_count(pack, gi) > 0);
        for (uint16_t k = 0; k < pq_gallery_count(pack, gi); k++) {
            uint16_t asset = pq_gallery_member(pack, gi, k);
            CHECK(pq_asset_image(pack, asset) != PQ_NO_IMAGE);
            CHECK_EQ_INT(pq_gallery_of_asset(pack, asset), (int)gi);
        }
    }
    // 章节/路线
    int total = 0;
    for (uint16_t r = 0; r < pack->route_count; r++) {
        total += pq_route_chapter_count(pack, r);
        CHECK(pq_route_name(pack, r)[0] != 0);
    }
    CHECK_EQ_INT(total, 37);
    printf("chapters at %u size %u ch1: off=%u scene=%u route=%u res=%u", (unsigned)((const uint8_t *)pack->chapters - pack->scn), (unsigned)sizeof(pq_chapter_t), (unsigned)pack->chapters[1].label_off, (unsigned)pack->chapters[1].scene, (unsigned)pack->chapters[1].route, (unsigned)pack->chapters[1].reserved);
    printf("%c", 10);
    for (uint16_t c = 0; c < pack->chapter_count; c++) {
        uint16_t sc = pq_chapter_scene(pack, c);
        if (!pq_scene_by_id(pack, sc) && c < 6) {
            printf("chapter %u -> scene %u", (unsigned)c, (unsigned)sc);
            printf(" label=");
            printf("%s", pq_chapter_label(pack, c));
            printf("%c", 10);
        }
        CHECK(pq_scene_by_id(pack, sc) != NULL);
    }
}

// ---------------------------------------------------------------- 路线回归
static void check_routes(pq_game_t *g)
{
    host_nvs_reset();
    CHECK(pq_game_init(g));
    int side_var = pq_var_find(&g->pack, "sf.parquet_side_clear");
    int main_var = pq_var_find(&g->pack, "sf.parquet_main_clear");
    int rin_var = pq_var_find(&g->pack, "sf.parquet_after_rin_clear");
    int tsu_var = pq_var_find(&g->pack, "sf.parquet_after_tsu_clear");
    CHECK(side_var >= 0 && main_var >= 0 && rin_var >= 0 && tsu_var >= 0);

    walk_t w;
    pq_game_start(g, g->pack.side, true);   // SIDE STORY
    CHECK(run_to_end(g, 0, &w));
    CHECK_EQ_INT(w.end_scene, 4);
    CHECK(w.ending_name >= 0);
    CHECK(strcmp(pq_ending_name(&g->pack, 4), "SIDE STORY") == 0);
    CHECK(pq_game_global_var(g, (uint16_t)side_var, NULL));
    CHECK(!pq_game_global_var(g, (uint16_t)main_var, NULL));

    pq_game_start(g, g->pack.flow, true);   // 正篇
    CHECK(run_to_end(g, 0, &w));
    CHECK_EQ_INT(w.end_scene, 92);
    CHECK(strcmp(pq_ending_name(&g->pack, 92), "PARQUET") == 0);
    CHECK(pq_game_global_var(g, (uint16_t)main_var, NULL));

    // 后日谈：两个选项分别到 100/104 → 103/107
    pq_game_start(g, g->pack.after, true);
    CHECK(run_to_end(g, 0, &w));
    CHECK_EQ_INT(w.choices, 1);
    CHECK_EQ_INT(w.end_scene, 103);
    int32_t sel = 0;
    int sel_var = pq_var_find(&g->pack, "s015*chapter7_sel1");
    CHECK(sel_var >= 0);
    CHECK(pq_game_var(g, (uint16_t)sel_var, &sel));
    CHECK_EQ_INT(sel, 1);
    CHECK(pq_game_global_var(g, (uint16_t)rin_var, NULL));
    CHECK(!pq_game_global_var(g, (uint16_t)tsu_var, NULL));

    pq_game_start(g, g->pack.after, true);
    CHECK(run_to_end(g, 1, &w));
    CHECK_EQ_INT(w.end_scene, 107);
    CHECK(pq_game_global_var(g, (uint16_t)tsu_var, NULL));
    CHECK(pq_game_global_var(g, (uint16_t)rin_var, NULL));

    // 孤儿选择节点 99（源里存在但主流程不经过）：直接进入，两条都能走通
    for (unsigned pick = 0; pick < 2; pick++) {
        pq_game_start(g, 99, true);
        CHECK(run_to_end(g, pick, &w));
        CHECK_EQ_INT(w.choices, 1);
        CHECK(w.end_scene == (pick ? 107 : 103));
    }

    // 章节模式：不加周目标记，不记结局
    host_nvs_reset();
    CHECK(pq_game_init(g));
    uint16_t seen_before = g->ending_seen;
    pq_game_start(g, 100, false);
    CHECK(run_to_end(g, 0, &w));
    CHECK_EQ_INT(w.end_scene, 103);
    CHECK(!pq_game_global_var(g, (uint16_t)rin_var, NULL));
    CHECK_EQ_INT(g->ending_seen, seen_before);

    // 全章节入口：每个章节都能读到 END 或停在选择
    for (uint16_t c = 0; c < g->pack.chapter_count; c++) {
        pq_game_start(g, pq_chapter_scene(&g->pack, c), false);
        bool ok = false;
        for (int i = 0; i < 20000; i++) {
            pq_step_t step;
            if (!pq_game_step(g, &step)) {
                break;
            }
            if (step.kind == PQ_STEP_END || step.kind == PQ_STEP_CHOICE) {
                ok = true;
                break;
            }
        }
        CHECK(ok);
    }

    // 选项文案与落点
    const pq_choice_t *c = pq_choice(&g->pack, 0);
    CHECK(c != NULL);
    if (c) {
        const pq_choice_item_t *a = pq_choice_item(&g->pack, c, 0);
        const pq_choice_item_t *b = pq_choice_item(&g->pack, c, 1);
        CHECK(a && b);
        if (a && b) {
            CHECK(strcmp(pq_text_get(&g->pack, a->text_off, a->text_len), "叫醒茨木") == 0);
            CHECK(strcmp(pq_text_get(&g->pack, b->text_off, b->text_len), "叫醒城门") == 0);
            CHECK_EQ_INT(a->scene, 100);
            CHECK_EQ_INT(b->scene, 104);
            CHECK(a->has_effect && a->value == 1);
            CHECK(b->has_effect && b->value == 2);
        }
    }
}

// ---------------------------------------------------------------- 存档
static void check_saves(pq_game_t *g)
{
    host_nvs_reset();
    CHECK(pq_game_init(g));
    pq_game_start(g, 5, true);
    pq_step_t step;
    for (int i = 0; i < 40; i++) {
        CHECK(pq_game_step(g, &step));
        if (step.kind == PQ_STEP_TEXT) {
            break;
        }
    }
    uint16_t scene0 = g->scene;
    uint32_t op0 = g->text_op;
    const char *text0 = step.text;
    char keep[256];
    snprintf(keep, sizeof(keep), "%s", text0);

    CHECK(pq_game_save(g, 0, 1));
    CHECK(pq_game_slot_exists(0));
    pq_save_info_t info;
    CHECK(pq_game_slot_peek(0, &g->pack, &info));
    CHECK(info.valid);
    CHECK_EQ_INT(info.scene, scene0);
    CHECK_EQ_INT(info.mode, 0);
    CHECK(strstr(info.preview, keep) != NULL || info.preview[0] != 0);

    // 推进若干句后读档：必须回到保存的那一句、页
    for (int i = 0; i < 3; i++) {
        CHECK(pq_game_step(g, &step));
    }
    CHECK(g->text_op != op0);
    uint8_t page = 9;
    CHECK(pq_game_load(g, 0, &page));
    CHECK_EQ_INT(page, 1);
    CHECK_EQ_INT(g->scene, scene0);
    CHECK_EQ_INT(g->text_op, 0xFFFFFFFF);
    CHECK_EQ_INT(g->cursor, op0);

    // 待选存档：98 号场景的 choice
    pq_game_start(g, 98, true);
    CHECK(pq_game_step(g, &step));
    CHECK_EQ_INT(step.kind, PQ_STEP_CHOICE);
    g->choice_sel = 1;
    CHECK(pq_game_save(g, 1, 0));
    g->choice_sel = 0;
    pq_game_choose(g, 0);   // 选了「叫醒茨木」
    CHECK_EQ_INT(g->scene, 100);
    uint8_t page2 = 0;
    CHECK(pq_game_load(g, 1, &page2));
    CHECK_EQ_INT(g->scene, 98);
    CHECK_EQ_INT(g->mode, 1);
    CHECK_EQ_INT(g->choice_sel, 1);
    CHECK(pq_game_step(g, &step));
    CHECK_EQ_INT(step.kind, PQ_STEP_CHOICE);
    CHECK_EQ_INT(step.choice_count, 2);

    // 待选存档要带上当时的视觉组合（scene 98 这类纯选项场景没有前一条正文可查）
    pq_game_start(g, 95, true);   // 95 的正文 → 96/97 → 98 选项
    for (int i = 0; i < 100; i++) {
        if (!pq_game_step(g, &step) || step.kind != PQ_STEP_TEXT) {
            break;
        }
    }
    CHECK_EQ_INT(step.kind, PQ_STEP_CHOICE);
    uint16_t saved_visual = g->current_visual;
    CHECK(saved_visual < g->pack.visual_count);
    CHECK(pq_game_save(g, 9, 0));
    (void)pq_game_choose(g, 0);
    CHECK_EQ_INT(g->scene, 100);
    CHECK(pq_game_load(g, 9, &page2));
    CHECK_EQ_INT(g->scene, 98);
    CHECK_EQ_INT(g->current_visual, saved_visual);
    CHECK(pq_game_step(g, &step));
    CHECK_EQ_INT(step.kind, PQ_STEP_CHOICE);

    // 保存失败：NVS 写入失败时返回 false，旧档不变，当前状态不变
    uint8_t before[256];
    size_t before_len = 0;
    CHECK(host_nvs_get("pqsave", "s01", before, sizeof(before), &before_len));
    uint16_t scene_before = g->scene;
    host_nvs_set_fail(HOST_NVS_FAIL_WRITE);
    CHECK(!pq_game_save(g, 0, 0));
    host_nvs_set_fail(HOST_NVS_OK);
    uint8_t after[256];
    size_t after_len = 0;
    CHECK(host_nvs_get("pqsave", "s01", after, sizeof(after), &after_len));
    CHECK_EQ_INT(before_len, after_len);
    CHECK(before_len > 0 && memcmp(before, after, before_len) == 0);
    CHECK_EQ_INT(g->scene, scene_before);

    // 坏档：CRC/字段被破坏 → 拒绝，且当前状态不变
    pq_game_start(g, 5, true);
    CHECK(pq_game_step(g, &step));
    uint16_t keep_scene = g->scene;
    host_nvs_corrupt("pqsave", "s01", 7);
    CHECK(!pq_game_load(g, 0, &page));
    CHECK_EQ_INT(g->scene, keep_scene);
    CHECK(pq_game_slot_peek(0, &g->pack, &info));
    CHECK(!info.valid);   // 存在但损坏

    // 截断的 blob → 拒绝
    uint8_t tiny[8] = {0};
    CHECK(host_nvs_put("pqsave", "s02", tiny, sizeof(tiny)));
    CHECK(!pq_game_load(g, 1, &page));

    // 版本不符 → 拒绝
    CHECK(pq_game_save(g, 2, 0));
    uint8_t blob[256];
    size_t blob_len = 0;
    CHECK(host_nvs_get("pqsave", "s03", blob, sizeof(blob), &blob_len));
    blob[4] = 9;   // version 低字节
    CHECK(host_nvs_put("pqsave", "s03", blob, blob_len));
    CHECK(!pq_game_load(g, 2, &page));

    // 空栏位
    CHECK(!pq_game_load(g, 29, &page));
    pq_save_info_t empty;
    CHECK(!pq_game_slot_peek(29, &g->pack, &empty));
    CHECK(!pq_game_slot_exists(29));
}

// ---------------------------------------------------------------- 周目记录 / CG
static void check_meta(pq_game_t *g)
{
    host_nvs_reset();
    CHECK(pq_game_init(g));
    CHECK_EQ_INT(g->pvar_set, 0);
    int side_var = pq_var_find(&g->pack, "sf.parquet_side_clear");
    walk_t w;
    pq_game_start(g, g->pack.side, true);
    CHECK(run_to_end(g, 0, &w));
    CHECK(g->meta_dirty);
    CHECK(pq_game_save_meta(g));
    CHECK(!g->meta_dirty);

    // 重新初始化 = 冷启动：全局记录从 NVS 读回
    pq_game_t g2;
    CHECK(pq_game_init(&g2));
    CHECK(pq_game_global_var(&g2, (uint16_t)side_var, NULL));

    // 读旧档不回退全局：旧档保存时还没有 side 标记
    pq_game_start(g, 5, true);
    pq_step_t step;
    for (int i = 0; i < 30; i++) {
        CHECK(pq_game_step(g, &step));
        if (step.kind == PQ_STEP_TEXT) {
            break;
        }
    }
    CHECK(pq_game_save(g, 5, 0));
    CHECK(pq_game_load(g, 5, NULL));
    CHECK(pq_game_global_var(g, (uint16_t)side_var, NULL));

    // CG 解锁：来源是真实剧本里的 CG 背景；解锁位随全局记录持久化
    host_nvs_reset();
    CHECK(pq_game_init(g));
    uint16_t group = 0;
    uint16_t asset = pq_gallery_member(&g->pack, group, 0);
    CHECK(!pq_game_cg_unlocked(g, group));
    pq_game_mark_cg(g, asset);
    CHECK(pq_game_cg_unlocked(g, group));
    CHECK(g->meta_dirty);
    CHECK(pq_game_save_meta(g));
    CHECK(pq_game_init(&g2));
    CHECK(pq_game_cg_unlocked(&g2, group));

    // 37 个鉴赏组必须能各自记录（早期 32 位位图会把 32..36 组误判为已解锁）
    CHECK(!pq_game_cg_unlocked(&g2, 36));
    uint16_t last_asset = pq_gallery_member(&g2.pack, 36, 0);
    CHECK(last_asset != 0xFFFF);
    pq_game_mark_cg(&g2, last_asset);
    CHECK(pq_game_cg_unlocked(&g2, 36));
    CHECK(!pq_game_cg_unlocked(&g2, 37));
    CHECK(pq_game_save_meta(&g2));
    pq_game_t g3;
    CHECK(pq_game_init(&g3));
    CHECK(pq_game_cg_unlocked(&g3, 36));
    CHECK(!pq_game_cg_unlocked(&g3, 37));

    // 未被引用的资源 id 不应标记任何 CG
    uint16_t unused = 1415;
    if (pq_asset_image(&g->pack, unused) == PQ_NO_IMAGE &&
        pq_asset_sprite(&g->pack, unused) == NULL) {
        pq_game_mark_cg(&g2, unused);
        CHECK_EQ_INT(pq_gallery_of_asset(&g2.pack, unused), -1);
    }
}

// ---------------------------------------------------------------- 图像
typedef struct {
    int index;
    int kind;
    int w;
    int h;
    uint32_t crc;
    int grid[48];
} fixture_t;

static int load_fixture(const char *path, fixture_t **out)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fputs("missing fixture", stdout);
        return 0;
    }
    char line[4096];
    int n = 0, cap = 0;
    fixture_t *arr = NULL;
    while (fgets(line, sizeof(line), fh)) {
        if (line[0] == '#' || line[0] == 10) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            arr = realloc(arr, sizeof(fixture_t) * (size_t)cap);
        }
        fixture_t *f = &arr[n];
        const char *p = line;
        char *endp = NULL;
        long long vals[53];
        int got = 0;
        while (got < 53) {
            long long v = strtoll(p, &endp, 10);
            if (endp == p) {
                break;
            }
            vals[got++] = v;
            p = endp;
        }
        if (got != 53) {
            fputs("bad fixture line", stdout);
            free(arr);
            fclose(fh);
            return 0;
        }
        f->index = (int)vals[0];
        f->kind = (int)vals[1];
        f->w = (int)vals[2];
        f->h = (int)vals[3];
        f->crc = (uint32_t)vals[4];
        for (int i = 0; i < 48; i++) {
            f->grid[i] = (int)vals[5 + i];
        }
        n++;
    }
    fclose(fh);
    *out = arr;
    return n;
}

static void rgb888(uint16_t c, int *r, int *g, int *b)
{
    int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static void check_images(pq_game_t *g, const char *fixture_path)
{
    pq_pack_t *pack = &g->pack;
    CHECK(pq_gfx_init());
    // 双带画布：两条 320x120（各 76,800 B），拼成虚拟 320x240 整屏。
    CHECK_EQ_INT(pq_gfx_band_count(), 2);
    CHECK_EQ_INT(pq_gfx_canvas_height(), PQ_CANVAS_H);
    CHECK_EQ_INT(pq_gfx_canvas(0)->header.h, PQ_BAND_H);
    CHECK_EQ_INT(pq_gfx_canvas(1)->header.h, PQ_BAND_H);
    CHECK_EQ_INT(pq_gfx_canvas(1)->data_size, 76800u);
    printf("canvas: %dx%d = %zu B, heap free %u B\n", PQ_ART_W, pq_gfx_canvas_height(),
           (size_t)PQ_ART_W * (size_t)pq_gfx_canvas_height() * 2u,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    // 回退路径：第二条带拿不到 → 单块 320x150；连 76,800 都拿不到 → 初始化失败。
    {
        pq_gfx_test_drop_buffers();
        host_heap_fail_size(76800);         // 76,800 一律拿不到 → 回退单块 320x150
        CHECK(pq_gfx_init());
        CHECK_EQ_INT(pq_gfx_band_count(), 1);
        CHECK_EQ_INT(pq_gfx_canvas_height(), PQ_ART_H);
        pq_gfx_test_drop_buffers();
        host_heap_fail_size(0);
        host_heap_fail_after(0);            // 一次都不许成功
        CHECK(!pq_gfx_init());
        host_heap_fail_after(-1);
        CHECK(pq_gfx_init());
        CHECK_EQ_INT(pq_gfx_band_count(), 2);
    }
    int decoded = 0;
    for (uint16_t i = 0; i < pack->img_entry_count; i++) {
        const pq_img_entry_t *e = pq_img_entry(pack, i);
        CHECK(e != NULL);
        if (!e) {
            continue;
        }
        pq_gfx_clear(0x0000);
        int dx = 0, dy = 0;
        if (e->kind == PQ_IMG_JPEG) {
            dx = (320 - (int)e->w) / 2;
            dy = (150 - (int)e->h) / 2;
        }
        if (!pq_gfx_draw_image(pack, i, dx, dy)) {
            printf("decode failed image %u kind %u %ux%u\n", i, e->kind, e->w, e->h);
            g_fail++;
            continue;
        }
        decoded++;
    }
    CHECK_EQ_INT(decoded, 1510);

    // cover：非 320 宽的 JPEG（如 267x150 / 250x150）必须铺满 320x150，四边不留黑。
    {
        int covered = 0, bars = 0;
        for (uint16_t i = 0; i < pack->img_entry_count; i++) {
            const pq_img_entry_t *e = pq_img_entry(pack, i);
            if (!e || e->kind != PQ_IMG_JPEG || e->w < 200 || e->h < 100) continue;
            pq_gfx_clear(0x0000);
            if (!pq_gfx_draw_jpeg_cover(pack, i, 0, 0, 320, pq_gfx_canvas_height())) continue;
            covered++;
            static uint16_t flat[320 * PQ_CANVAS_H];
            while (true) { pq_gfx_test_flatten(flat); break; }
            const uint16_t *canvas = flat;
            int empty = 0;
            const int ch = pq_gfx_canvas_height();
            for (int x = 0; x < 320; x++) {
                if (canvas[0 * 320 + x] == 0) empty++;              // 顶行
                if (canvas[(ch - 1) * 320 + x] == 0) empty++;       // 底行（整屏最底）
            }
            for (int y = 0; y < ch; y++) {
                if (canvas[y * 320 + 0] == 0) empty++;              // 左列
                if (canvas[y * 320 + 319] == 0) empty++;            // 右列
            }
            if (empty > 2 * 320 + 2 * ch) {                        // 全部边框像素
                if (bars < 3) printf("cover left bars: image %u %ux%u empty=%d\n", i,
                                     (unsigned)e->w, (unsigned)e->h, empty);
                bars++;
            }
        }
        printf("cover: %d images, %d with empty border\n", covered, bars);
        CHECK_EQ_INT(bars, 0);
        CHECK(covered > 300);
    }

    fixture_t *fx = NULL;
    int n = load_fixture(fixture_path, &fx);
    CHECK_EQ_INT(n, 1510);
    int mismatched = 0, tol_fail = 0;
    if (n == 1510) {
        for (int k = 0; k < n; k++) {
            fixture_t *f = &fx[k];
            const pq_img_entry_t *e = pq_img_entry(pack, (uint16_t)f->index);
            if (!e || (int)e->kind != f->kind || (int)e->w != f->w || (int)e->h != f->h) {
                mismatched++;
                continue;
            }
            pq_gfx_clear(0x0000);
            int dx = 0, dy = 0;
            if (e->kind == PQ_IMG_JPEG) {
                dx = (320 - (int)e->w) / 2;
                dy = (150 - (int)e->h) / 2;
            }
            if (!pq_gfx_draw_image(pack, (uint16_t)f->index, dx, dy)) {
                mismatched++;
                continue;
            }
            static uint16_t flat2[320 * PQ_CANVAS_H];
            pq_gfx_test_flatten(flat2);
            const uint16_t *canvas = flat2;
            if (e->kind != PQ_IMG_JPEG) {
                uint8_t *rgba = malloc((size_t)e->w * e->h * 4);
                for (int y = 0; y < (int)e->h; y++) {
                    for (int x = 0; x < (int)e->w; x++) {
                        int r, gg, b;
                        rgb888(canvas[y * 320 + x], &r, &gg, &b);
                        uint8_t *p = rgba + ((size_t)y * e->w + x) * 4;
                        p[0] = (uint8_t)r;
                        p[1] = (uint8_t)gg;
                        p[2] = (uint8_t)b;
                        p[3] = 255;
                    }
                }
                uint32_t crc = crc32_le(0, rgba, (size_t)e->w * e->h * 4);
                free(rgba);
                if (crc != f->crc) {
                    if (mismatched < 5) {
                        printf("pixel mismatch image %d kind %d %dx%d crc %08x != %08x\n",
                               f->index, f->kind, f->w, f->h, crc, f->crc);
                    }
                    mismatched++;
                }
            } else {
                // JPEG：4x4 网格点与 Pillow 解码结果对照。不同 IDCT/色度上采样在
                // 高对比边缘会有个别像素偏差，因此按整图平均误差 + 单点上限判定。
                int sum = 0, maxdiff = 0;
                for (int j = 0; j < 4; j++) {
                    for (int i2 = 0; i2 < 4; i2++) {
                        int x = dx + (int)((i2 + 0.5) * e->w / 4);
                        int y = dy + (int)((j + 0.5) * e->h / 4);
                        if (x >= 320) x = 319;
                        if (y >= 150) y = 149;
                        int r, gg, b;
                        rgb888(canvas[y * 320 + x], &r, &gg, &b);
                        int d0 = abs(r - f->grid[(j * 4 + i2) * 3 + 0]);
                        int d1 = abs(gg - f->grid[(j * 4 + i2) * 3 + 1]);
                        int d2 = abs(b - f->grid[(j * 4 + i2) * 3 + 2]);
                        sum += d0 + d1 + d2;
                        if (d0 > maxdiff) maxdiff = d0;
                        if (d1 > maxdiff) maxdiff = d1;
                        if (d2 > maxdiff) maxdiff = d2;
                    }
                }
                if (sum > 48 * 12 || maxdiff > 48) {
                    if (tol_fail < 5) {
                        printf("jpeg mismatch image %d mean=%d max=%d", f->index,
                               sum / 48, maxdiff);
                        printf("%c", 10);
                    }
                    tol_fail++;
                }
            }
        }
    }
    free(fx);
    CHECK_EQ_INT(mismatched, 0);
    CHECK_EQ_INT(tol_fail, 0);
}

static void check_gfx_faults(pq_game_t *g)
{
    pq_pack_t *pack = &g->pack;
    // 找一张 PATCH 与一张 BLOCK
    int patch_idx = -1, block_idx = -1, jpeg_idx = -1;
    for (uint16_t i = 0; i < pack->img_entry_count; i++) {
        int kind = pack->img_entries[i].kind;
        if (kind == PQ_IMG_PATCH && patch_idx < 0) patch_idx = i;
        if (kind == PQ_IMG_BLOCK && block_idx < 0) block_idx = i;
        if (kind == PQ_IMG_JPEG && jpeg_idx < 0) jpeg_idx = i;
    }
    CHECK(patch_idx >= 0 && block_idx >= 0 && jpeg_idx >= 0);

    // 短载荷 → 明确失败
    uint8_t tiny[8] = {0};
    CHECK(!pq_gfx_test_draw_payload(PQ_IMG_PATCH, tiny, sizeof(tiny), 0, 0));
    CHECK(!pq_gfx_test_draw_payload(PQ_IMG_BLOCK, tiny, sizeof(tiny), 0, 0));

    // 声明长度不符的 PATCH
    const pq_img_entry_t *pe = pq_img_entry(pack, (uint16_t)patch_idx);
    const uint8_t *pp = pq_img_data(pack, pe);
    uint8_t *copy = malloc(pe->data_len);
    memcpy(copy, pp, pe->data_len);
    uint32_t bad_len = 0x00FFFFFF;
    memcpy(copy + 8, &bad_len, 4);
    CHECK(!pq_gfx_test_draw_payload(PQ_IMG_PATCH, copy, pe->data_len, 0, 0));
    free(copy);

    // 损坏的 zlib 流 → 失败且不改画布
    const pq_img_entry_t *be = pq_img_entry(pack, (uint16_t)block_idx);
    const uint8_t *bp = pq_img_data(pack, be);
    copy = malloc(be->data_len);
    memcpy(copy, bp, be->data_len);
    for (uint32_t i = be->data_len / 2; i < be->data_len / 2 + 8; i++) {
        copy[i] ^= 0xFF;
    }
    pq_gfx_clear(0x1234);
    CHECK(!pq_gfx_test_draw_payload(PQ_IMG_BLOCK, copy, be->data_len, 0, 0));
    CHECK_EQ_INT(pq_gfx_canvas_data()[0], 0x1234);   // 第 0 条带的首像素
    free(copy);

    // 截断的 JPEG（直接改包内字节再还原）
    const pq_img_entry_t *je = pq_img_entry(pack, (uint16_t)jpeg_idx);
    uint8_t *img_mut = (uint8_t *)pack->img;
    uint32_t cut = je->data_off + je->data_len - 20;
    uint8_t saved[20];
    memcpy(saved, img_mut + cut, 20);
    memset(img_mut + cut, 0xA5, 20);
    CHECK(!pq_gfx_draw_jpeg(pack, (uint16_t)jpeg_idx, 0, 0));
    memcpy(img_mut + cut, saved, 20);
    CHECK(pq_gfx_draw_jpeg(pack, (uint16_t)jpeg_idx, 0, 0));

    // 画布分配失败 → init 返回 false（不崩溃）
    pq_gfx_deinit();
    host_heap_set_limit(1024);
    CHECK(!pq_gfx_init());
    host_heap_set_limit(0);
    CHECK(pq_gfx_init());
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: %s <parquet_img.bin> <parquet_scn.bin> [fixtures/images.txt]\n",
               argv[0]);
        return 2;
    }
    size_t img_size = 0, scn_size = 0;
    uint8_t *img = read_file(argv[1], &img_size);
    uint8_t *scn = read_file(argv[2], &scn_size);
    printf("pack: img %zu B, scn %zu B\n", img_size, scn_size);
    CHECK(pq_host_set_pack(img, img_size, scn, scn_size));

    host_nvs_reset();
    pq_game_t game;
    CHECK(pq_game_init(&game));
    printf("pack...\n");
    check_pack(&game.pack);
    printf("routes...\n");
    check_routes(&game);
    printf("saves...\n");
    check_saves(&game);
    printf("meta...\n");
    check_meta(&game);
    printf("images...\n");
    check_images(&game, argc > 3 ? argv[3] : "tests/parquet/fixtures/images.txt");
    printf("faults...\n");
    check_gfx_faults(&game);

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    free(img);
    free(scn);
    return g_fail ? 1 : 0;
}
