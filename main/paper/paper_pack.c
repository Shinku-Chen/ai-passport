// main/paper/paper_pack.c —— 资源包解析与正文块解压（严格边界校验版）。
#include "paper_pack.h"

#include "esp_log.h"
#include "miniz.h"

#include <string.h>

static const char *TAG = "paper_pack";

// ROM miniz 的 mem_to_mem 辅助函数会把 ~11 KB 的 tinfl_decompressor 放在调用者栈上。
// 这里把状态放静态区（.bss），只调用底层 tinfl_decompress，避免输入任务栈被吃掉。
static tinfl_decompressor s_inflate;
static bool s_inflate_busy;

static size_t inflate_into(void *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    if (s_inflate_busy) {
        return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    }
    s_inflate_busy = true;
    tinfl_init(&s_inflate);
    size_t in_bytes = src_len;
    size_t out_bytes = dst_len;
    tinfl_status st = tinfl_decompress(&s_inflate, (const mz_uint8 *)src, &in_bytes,
                                       (mz_uint8 *)dst, (mz_uint8 *)dst, &out_bytes,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER |
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    size_t produced = (st == TINFL_STATUS_DONE) ? out_bytes : TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    s_inflate_busy = false;
    return produced;
}

#ifdef PAPER_PACK_USE_HOST_BLOBS
// 宿主测试：由测试程序把两个 .bin 读进内存后交给 paper_pack_set_host_blobs()
extern const uint8_t *paper_host_img;
extern uint32_t paper_host_img_len;
extern const uint8_t *paper_host_scn;
extern uint32_t paper_host_scn_len;
static const uint8_t *img_begin(void) { return paper_host_img; }
static const uint8_t *img_end(void) { return paper_host_img + paper_host_img_len; }
static const uint8_t *scn_begin(void) { return paper_host_scn; }
static const uint8_t *scn_end(void) { return paper_host_scn + paper_host_scn_len; }
#else
extern const uint8_t paper_img_bin_start[] asm("_binary_paper_img_bin_start");
extern const uint8_t paper_img_bin_end[] asm("_binary_paper_img_bin_end");
extern const uint8_t paper_scn_bin_start[] asm("_binary_paper_scn_bin_start");
extern const uint8_t paper_scn_bin_end[] asm("_binary_paper_scn_bin_end");
static const uint8_t *img_begin(void) { return paper_img_bin_start; }
static const uint8_t *img_end(void) { return paper_img_bin_end; }
static const uint8_t *scn_begin(void) { return paper_scn_bin_start; }
static const uint8_t *scn_end(void) { return paper_scn_bin_end; }
#endif

// 正文块缓存：一块最大 16 KB（打包器保证行不跨块）
#define TEXT_CACHE_BYTES (16 * 1024 + 64)
static uint8_t s_text_cache[TEXT_CACHE_BYTES];
static const uint8_t *s_text_cache_src;
static uint32_t s_text_cache_len;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// 段范围检查：返回 false 表示越界
static bool in_range(uint32_t off, uint32_t len, uint32_t size, const char *what)
{
    if (off > size || len > size || off + len > size) {
        ESP_LOGE(TAG, "%s 越界: off=%u len=%u size=%u", what, (unsigned)off, (unsigned)len,
                 (unsigned)size);
        return false;
    }
    return true;
}

bool paper_pack_init(paper_pack_t *pack)
{
    memset(pack, 0, sizeof(*pack));
    pack->img = img_begin();
    pack->img_size = (uint32_t)(img_end() - img_begin());
    pack->scn = scn_begin();
    pack->scn_size = (uint32_t)(scn_end() - scn_begin());

    // ---------------------------------------------------------------- 图片包
    if (pack->img_size < 20 || memcmp(pack->img, "PAPRIMG1", 8) != 0) {
        ESP_LOGE(TAG, "图片包 magic 不对（size=%u）", (unsigned)pack->img_size);
        return false;
    }
    const uint8_t *h = pack->img;
    uint16_t img_ver = rd16(h + 8);
    pack->entry_count = rd16(h + 10);
    uint32_t names_bytes = rd32(h + 12);
    uint32_t group_off = rd32(h + 16);
    if (img_ver != 1) {
        ESP_LOGE(TAG, "图片包版本 %u 不支持", img_ver);
        return false;
    }
    uint32_t table_bytes = 24u * pack->entry_count;
    if (!in_range(20, table_bytes, pack->img_size, "图片表")) {
        return false;
    }
    pack->entries = (const paper_img_entry_t *)(h + 20);
    uint32_t names_off = 20 + table_bytes;
    if (!in_range(names_off, names_bytes, pack->img_size, "图片名段")) {
        return false;
    }
    pack->img_names = (const char *)(h + names_off);
    uint32_t body_off = names_off + names_bytes;
    for (uint32_t i = 0; i < pack->entry_count; i++) {
        const paper_img_entry_t *e = &pack->entries[i];
        if (e->name_off >= names_bytes) {
            ESP_LOGE(TAG, "图片 %u 名字偏移越界", (unsigned)i);
            return false;
        }
        if (e->data_off < body_off || e->data_len == 0 ||
            !in_range(e->data_off, e->data_len, pack->img_size, "图片数据")) {
            ESP_LOGE(TAG, "图片 %u data 越界", (unsigned)i);
            return false;
        }
        if (e->kind > 2) {
            ESP_LOGE(TAG, "图片 %u kind=%u 不支持", (unsigned)i, e->kind);
            return false;
        }
        if (e->kind == 2) {
            if (e->extra_len != 10 || e->data_len < 10 + 8) {
                ESP_LOGE(TAG, "图片 %u PATCH 头长度异常 extra=%u len=%u", (unsigned)i,
                         e->extra_len, (unsigned)e->data_len);
                return false;
            }
            const uint8_t *b = h + e->data_off;
            uint16_t base = rd16(b);
            if (base >= pack->entry_count || pack->entries[base].kind != 1) {
                ESP_LOGE(TAG, "图片 %u PATCH base=%u 不是 BLOCK", (unsigned)i, base);
                return false;
            }
        }
    }
    if (group_off) {
        if (!in_range(group_off, 2u * pack->entry_count, pack->img_size, "图片-CG 映射")) {
            return false;
        }
        pack->img_group = (const uint16_t *)(h + group_off);
    }

    // ---------------------------------------------------------------- 剧本包
    if (pack->scn_size < 92 || memcmp(pack->scn, "PAPRSCN1", 8) != 0) {
        ESP_LOGE(TAG, "剧本包 magic 不对（size=%u）", (unsigned)pack->scn_size);
        return false;
    }
    const uint8_t *s = pack->scn;
    uint16_t scn_ver = rd16(s + 8);
    if (scn_ver != 3) {
        ESP_LOGE(TAG, "剧本包版本 %u 不支持", scn_ver);
        return false;
    }
    pack->scene_count = rd16(s + 10);
    pack->speaker_count = rd16(s + 12);
    pack->text_block_count = rd16(s + 14);
    pack->cg_count = rd16(s + 16);
    pack->chapter_count = rd16(s + 20);
    pack->choice_count = rd16(s + 22);
    pack->ending_count = rd16(s + 24);
    pack->flow_scene = rd16(s + 28);
    pack->initial_scene = rd16(s + 30);

    const uint8_t *o = s + 32;
    uint32_t ops_off = rd32(o + 0);
    uint32_t ops_bytes = rd32(o + 4);
    uint32_t textidx_off = rd32(o + 8);
    uint32_t textdata_off = rd32(o + 12);
    uint32_t scene_tbl_off = rd32(o + 16);
    uint32_t scene_names_off = rd32(o + 20);
    uint32_t speaker_off = rd32(o + 24);
    uint32_t cg_names_off = rd32(o + 28);
    uint32_t cg_members_off = rd32(o + 32);
    uint32_t chapters_off = rd32(o + 36);
    uint32_t chapter_meta_off = rd32(o + 40);
    uint32_t choices_off = rd32(o + 44);
    uint32_t endings_off = rd32(o + 48);
    uint32_t ending_scenes_off = rd32(o + 52);
    uint32_t total = rd32(o + 56);
    if (total > pack->scn_size) {
        ESP_LOGE(TAG, "剧本包声明 %u > 实际 %u", (unsigned)total, (unsigned)pack->scn_size);
        return false;
    }

    // 各段都在 [0,total) 内
    if (!in_range(scene_tbl_off, 20u * pack->scene_count, total, "场景表") ||
        !in_range(scene_names_off, 0, total, "场景名") ||
        !in_range(speaker_off, 4u * pack->speaker_count, total, "说话人偏移表") ||
        !in_range(cg_names_off, 4u * pack->cg_count, total, "CG 名偏移表") ||
        !in_range(cg_members_off, 4u * pack->cg_count, total, "CG 成员偏移表") ||
        !in_range(chapters_off, 4u * pack->chapter_count, total, "章节偏移表") ||
        !in_range(chapter_meta_off, 8u * pack->chapter_count, total, "章节元数据") ||
        !in_range(choices_off, 4u * pack->choice_count, total, "选项偏移表") ||
        !in_range(endings_off, 4u * pack->ending_count, total, "结局偏移表") ||
        !in_range(ending_scenes_off, 2u * pack->ending_count, total, "结局场景表") ||
        !in_range(ops_off, ops_bytes, total, "op 流") ||
        !in_range(textidx_off, 16u * pack->text_block_count, total, "正文索引") ||
        !in_range(textdata_off, total - textdata_off, total, "正文数据")) {
        return false;
    }
    if (ops_bytes % 8 != 0) {
        ESP_LOGE(TAG, "op 流长度不是 8 的倍数");
        return false;
    }
    pack->ops = (const paper_op_t *)(s + ops_off);
    pack->op_count = ops_bytes / 8;

    pack->scenes = (const paper_scene_t *)(s + scene_tbl_off);
    pack->scene_names = (const char *)(s + scene_names_off);
    pack->text_blocks = (const paper_text_block_t *)(s + textidx_off);
    pack->text_data = s + textdata_off;
    pack->text_data_size = total - textdata_off;

    pack->speaker_offs = (const uint32_t *)(s + speaker_off);
    pack->speaker_blob = (const char *)(s + speaker_off + 4u * pack->speaker_count);

    pack->cg_offs = (const uint32_t *)(s + cg_names_off);
    pack->cg_blob = (const char *)(s + cg_names_off + 4u * pack->cg_count);
    pack->cg_member_offs = (const uint32_t *)(s + cg_members_off);
    pack->cg_members = s + cg_members_off + 4u * pack->cg_count;
    pack->cg_members_size = cg_members_off + 4u * pack->cg_count < total
                                ? total - (cg_members_off + 4u * pack->cg_count)
                                : 0;

    pack->chapter_offs = (const uint32_t *)(s + chapters_off);
    pack->chapter_blob = (const char *)(s + chapters_off + 4u * pack->chapter_count);
    pack->chapter_meta = (const uint16_t *)(s + chapter_meta_off);

    pack->choice_offs = (const uint32_t *)(s + choices_off);
    pack->choice_blob = s + choices_off + 4u * pack->choice_count;

    pack->ending_offs = (const uint32_t *)(s + endings_off);
    pack->ending_blob = (const char *)(s + endings_off + 4u * pack->ending_count);
    pack->ending_scene = (const uint16_t *)(s + ending_scenes_off);

    // 场景 / op 交叉校验
    for (uint32_t i = 0; i < pack->scene_count; i++) {
        const paper_scene_t *sc = &pack->scenes[i];
        if (sc->first_op + sc->op_count > pack->op_count) {
            ESP_LOGE(TAG, "场景 %u op 范围越界 (%u+%u > %u)", sc->id, (unsigned)sc->first_op,
                     (unsigned)sc->op_count, (unsigned)pack->op_count);
            return false;
        }
        for (uint32_t k = 0; k < sc->op_count; k++) {
            const paper_op_t *op = &pack->ops[sc->first_op + k];
            if (op->kind > PAPER_OP_CHOICE) {
                ESP_LOGE(TAG, "场景 %u op %u 未知 kind %u", sc->id, (unsigned)k, op->kind);
                return false;
            }
            if (op->kind == PAPER_OP_JUMPSCENE) {
                const paper_scene_t *dst = NULL;
                for (uint32_t j = 0; j < pack->scene_count; j++) {
                    if (pack->scenes[j].id == op->b) {
                        dst = &pack->scenes[j];
                        break;
                    }
                }
                if (!dst || op->c >= dst->op_count) {
                    ESP_LOGE(TAG, "场景 %u op %u 跳转目标非法", sc->id, (unsigned)k);
                    return false;
                }
            }
            if (op->kind == PAPER_OP_CHOICE && op->b >= pack->choice_count) {
                ESP_LOGE(TAG, "场景 %u op %u 选项下标越界", sc->id, (unsigned)k);
                return false;
            }
            if ((op->kind == PAPER_OP_BACKGROUND || op->kind == PAPER_OP_SPRITE) &&
                op->b != 0xFFFF && op->b >= pack->entry_count) {
                ESP_LOGE(TAG, "场景 %u op %u 图片下标越界", sc->id, (unsigned)k);
                return false;
            }
        }
    }

    // 正文块
    for (uint32_t i = 0; i < pack->text_block_count; i++) {
        const paper_text_block_t *b = &pack->text_blocks[i];
        if (b->comp_off + b->comp_len > pack->text_data_size || b->raw_len > TEXT_CACHE_BYTES) {
            ESP_LOGE(TAG, "正文块 %u 越界/过大", (unsigned)i);
            return false;
        }
    }

    ESP_LOGI(TAG, "解压状态 %u B（静态区，不占任务栈）",
             (unsigned)sizeof(tinfl_decompressor));
    ESP_LOGI(TAG, "新游戏入口 scene=%u（源 flowScene）", (unsigned)pack->flow_scene);
    ESP_LOGI(TAG, "包就绪: 图 %u 张 (%u KiB) / 场景 %u / op %u / 说话人 %u / 正文块 %u / CG %u 组 / 章节 %u / 结局 %u",
             (unsigned)pack->entry_count, (unsigned)(pack->img_size / 1024),
             (unsigned)pack->scene_count, (unsigned)pack->op_count,
             (unsigned)pack->speaker_count, (unsigned)pack->text_block_count,
             (unsigned)pack->cg_count, (unsigned)pack->chapter_count,
             (unsigned)pack->ending_count);
    return true;
}

int paper_scene_index(const paper_pack_t *pack, uint16_t scene_id)
{
    for (uint32_t i = 0; i < pack->scene_count; i++) {
        if (pack->scenes[i].id == scene_id) {
            return (int)i;
        }
    }
    return -1;
}

const paper_scene_t *paper_scene_find(const paper_pack_t *pack, uint16_t scene_id)
{
    int i = paper_scene_index(pack, scene_id);
    return i < 0 ? NULL : &pack->scenes[i];
}

const char *paper_scene_name(const paper_pack_t *pack, const paper_scene_t *scene)
{
    return pack->scene_names + scene->name_off;
}

const char *paper_speaker_name(const paper_pack_t *pack, int index)
{
    if (index < 0 || (uint32_t)index >= pack->speaker_count) {
        return "";
    }
    return pack->speaker_blob + pack->speaker_offs[index];
}

const char *paper_text_get(paper_pack_t *pack, uint32_t logical_off, uint16_t len)
{
    uint32_t lo = 0, hi = pack->text_block_count;
    if (hi == 0) {
        return NULL;
    }
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (pack->text_blocks[mid].logical_off <= logical_off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const paper_text_block_t *b = &pack->text_blocks[lo];
    if (logical_off < b->logical_off || logical_off - b->logical_off + len >= b->raw_len) {
        ESP_LOGW(TAG, "正文越界: off=%u len=%u block=%u", (unsigned)logical_off, len,
                 (unsigned)lo);
        return NULL;
    }
    const uint8_t *src = pack->text_data + b->comp_off;
    if (s_text_cache_src != src || s_text_cache_len != b->comp_len) {
        size_t out = inflate_into(s_text_cache, sizeof(s_text_cache), src, b->comp_len);
        if (out == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || out != b->raw_len) {
            ESP_LOGE(TAG, "正文块解压失败 (block=%u, %u B, out=%u)", (unsigned)lo, b->comp_len,
                     (unsigned)out);
            s_text_cache_src = NULL;
            return NULL;
        }
        s_text_cache_src = src;
        s_text_cache_len = b->comp_len;
    }
    return (const char *)s_text_cache + (logical_off - b->logical_off);
}

const paper_img_entry_t *paper_image(const paper_pack_t *pack, uint16_t index)
{
    if (index >= pack->entry_count) {
        return NULL;
    }
    return &pack->entries[index];
}

const uint8_t *paper_image_data(const paper_pack_t *pack, const paper_img_entry_t *e)
{
    return pack->img + e->data_off;
}

const uint8_t *paper_image_payload(const paper_pack_t *pack, const paper_img_entry_t *e)
{
    return pack->img + e->data_off + e->extra_len;
}

paper_img_patch_t paper_image_patch(const paper_pack_t *pack, const paper_img_entry_t *e)
{
    paper_img_patch_t p = {0, 0, 0, 0, 0};
    if (e->kind != 2 || e->extra_len != 10) {
        return p;
    }
    const uint8_t *b = pack->img + e->data_off;
    p.base = rd16(b + 0);
    p.x = rd16(b + 2);
    p.y = rd16(b + 4);
    p.w = rd16(b + 6);
    p.h = rd16(b + 8);
    return p;
}

int paper_image_cg_group(const paper_pack_t *pack, uint16_t index)
{
    if (!pack->img_group || index >= pack->entry_count) {
        return -1;
    }
    uint16_t g = pack->img_group[index];
    return g == 0xFFFF ? -1 : (int)g;
}

const char *paper_cg_name(const paper_pack_t *pack, uint32_t group)
{
    if (group >= pack->cg_count) {
        return "";
    }
    return pack->cg_blob + pack->cg_offs[group];
}

static const uint8_t *cg_members_of(const paper_pack_t *pack, uint32_t group)
{
    if (group >= pack->cg_count) {
        return NULL;
    }
    uint32_t off = pack->cg_member_offs[group];
    if (off + 2 > pack->cg_members_size) {
        return NULL;
    }
    return pack->cg_members + off;
}

int paper_cg_member_count(const paper_pack_t *pack, uint32_t group)
{
    const uint8_t *m = cg_members_of(pack, group);
    if (!m) {
        return 0;
    }
    uint16_t n = rd16(m);
    uint32_t off = (uint32_t)(m - pack->cg_members) + 2 + 2u * n;
    return off <= pack->cg_members_size ? n : 0;
}

uint16_t paper_cg_member(const paper_pack_t *pack, uint32_t group, int i)
{
    const uint8_t *m = cg_members_of(pack, group);
    if (!m) {
        return 0xFFFF;
    }
    int n = paper_cg_member_count(pack, group);
    if (i < 0 || i >= n) {
        return 0xFFFF;
    }
    return rd16(m + 2 + 2 * i);
}

uint16_t paper_cg_first_image(const paper_pack_t *pack, uint32_t group)
{
    return paper_cg_member(pack, group, 0);
}

const char *paper_chapter_name(const paper_pack_t *pack, uint32_t index)
{
    if (index >= pack->chapter_count) {
        return "";
    }
    return pack->chapter_blob + pack->chapter_offs[index];
}

uint16_t paper_chapter_scene(const paper_pack_t *pack, uint32_t index)
{
    if (index >= pack->chapter_count) {
        return 0;
    }
    return pack->chapter_meta[index * 4 + 2];
}

uint16_t paper_chapter_route(const paper_pack_t *pack, uint32_t index)
{
    if (index >= pack->chapter_count) {
        return 0;
    }
    return pack->chapter_meta[index * 4 + 1];
}

const char *paper_ending_name(const paper_pack_t *pack, uint32_t index)
{
    if (index >= pack->ending_count) {
        return "";
    }
    return pack->ending_blob + pack->ending_offs[index];
}

uint16_t paper_ending_scene(const paper_pack_t *pack, uint32_t index)
{
    return index < pack->ending_count ? pack->ending_scene[index] : 0;
}

int paper_choice_count(const paper_pack_t *pack, uint32_t choice_index)
{
    if (choice_index >= pack->choice_count) {
        return 0;
    }
    uint32_t off = pack->choice_offs[choice_index];
    return pack->choice_blob[off];
}

const paper_choice_item_t *paper_choice_item(const paper_pack_t *pack, uint32_t choice_index, int i)
{
    if (choice_index >= pack->choice_count) {
        return NULL;
    }
    uint32_t off = pack->choice_offs[choice_index];
    int n = pack->choice_blob[off];
    if (i < 0 || i >= n) {
        return NULL;
    }
    return (const paper_choice_item_t *)(pack->choice_blob + off + 1 +
                                         i * sizeof(paper_choice_item_t));
}
