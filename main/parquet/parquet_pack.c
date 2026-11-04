// main/parquet/parquet_pack.c —— 资源包解析 / 校验 / 正文块解压。
//
// 只在启动时解析一次表头；运行期所有访问都是只读指针 + 边界检查。任何越界都返回
// NULL/false，由上层显示"素材读取失败"而不是崩溃或静默缺图。
#include "parquet_pack.h"

#include <stdio.h>
#include <string.h>

#include "parquet_inflate.h"

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) {
        size_t n = strlen(msg);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(err, msg, n);
        err[n] = 0;
    }
}

static bool range_ok(uint32_t off, uint32_t len, uint32_t size)
{
    return off <= size && len <= size - off;
}

// 长度乘法先防溢出（当前包头计数是 u16，仍按通用写法防御）。
static bool mul_ok(uint32_t count, uint32_t elem, uint32_t *out)
{
    if (count != 0 && elem > UINT32_MAX / count) {
        return false;
    }
    *out = count * elem;
    return true;
}

typedef struct {
    uint32_t off;
    uint32_t len;
} pq_span_t;

// 段结构校验：每段必须位于 [header, size]，非零长段按起点排序后不得重叠。
// 零长段只校验起点（例如某个计数为 0 时的表），不参与重叠判断。
static bool spans_ok(pq_span_t *spans, int count, uint32_t size, uint32_t header)
{
    for (int i = 0; i < count; i++) {
        if (spans[i].off < header || spans[i].off > size ||
            spans[i].len > size - spans[i].off) {
            return false;
        }
    }
    for (int i = 1; i < count; i++) {
        pq_span_t key = spans[i];
        int j = i - 1;
        while (j >= 0 && spans[j].off > key.off) {
            spans[j + 1] = spans[j];
            j--;
        }
        spans[j + 1] = key;
    }
    uint32_t end = 0;
    bool have = false;
    for (int i = 0; i < count; i++) {
        if (spans[i].len == 0) {
            continue;
        }
        if (have && spans[i].off < end) {
            return false;
        }
        end = spans[i].off + spans[i].len;   // 已被 range_ok 约束，不会溢出
        have = true;
    }
    return true;
}

#define PQ_IMG_HEADER 64u
#define PQ_SCN_HEADER 128u

bool pq_pack_init(pq_pack_t *pack, const uint8_t *img, size_t img_size,
                  const uint8_t *scn, size_t scn_size, char *err, size_t err_cap)
{
    memset(pack, 0, sizeof(*pack));
    pack->text_buf_block = 0xFFFF;
    if (!img || !scn || img_size < PQ_IMG_HEADER || scn_size < PQ_SCN_HEADER) {
        set_err(err, err_cap, "资源包过短");
        return false;
    }
    if (img_size > UINT32_MAX || scn_size > UINT32_MAX) {
        set_err(err, err_cap, "资源包过大");
        return false;
    }
    if (memcmp(img, "PQIMG1\0\0", 8) != 0) {
        set_err(err, err_cap, "图片包标识错误");
        return false;
    }
    if (memcmp(scn, "PQSCN1\0\0", 8) != 0) {
        set_err(err, err_cap, "剧本包标识错误");
        return false;
    }
    pack->img = img;
    pack->img_size = (uint32_t)img_size;
    pack->scn = scn;
    pack->scn_size = (uint32_t)scn_size;

    // ---- 图片包
    {
        uint16_t version;
        const uint8_t *p = img + 8;
        memcpy(&version, p, 2);
        p += 2;
        if (version != 1) {
            set_err(err, err_cap, "图片包版本不兼容");
            return false;
        }
        uint16_t vals[6];
        memcpy(vals, p, sizeof(vals));
        p += sizeof(vals);
        pack->img_entry_count = vals[0];
        pack->sprite_count = vals[1];
        pack->cutin_count = vals[2];
        pack->asset_count = vals[3];
        pack->title_count = (uint8_t)vals[4];
        uint32_t offs[8];
        memcpy(offs, p, sizeof(offs));
        const uint32_t off_entries = offs[0], off_sprites = offs[1], off_cutins = offs[2];
        const uint32_t off_titles = offs[3], off_asset = offs[4], off_asset_cutin = offs[5];
        const uint32_t off_data = offs[6], data_size = offs[7];
        if (pack->img_entry_count == 0 || pack->sprite_count == 0 || pack->asset_count == 0) {
            set_err(err, err_cap, "图片包为空");
            return false;
        }
        uint32_t len_entries, len_sprites, len_cutins, len_titles, len_asset;
        if (!mul_ok(pack->img_entry_count, 16, &len_entries) ||
            !mul_ok(pack->sprite_count, 16, &len_sprites) ||
            !mul_ok(pack->cutin_count, 12, &len_cutins) ||
            !mul_ok(pack->title_count, 10, &len_titles) ||
            !mul_ok(pack->asset_count, 2, &len_asset)) {
            set_err(err, err_cap, "图片包段长度溢出");
            return false;
        }
        pq_span_t spans[7] = {
            {off_entries, len_entries},
            {off_sprites, len_sprites},
            {off_cutins, len_cutins},
            {off_titles, len_titles},
            {off_asset, len_asset},
            {off_asset_cutin, len_asset},
            {off_data, data_size},
        };
        if (!spans_ok(spans, 7, pack->img_size, PQ_IMG_HEADER)) {
            set_err(err, err_cap, "图片包段越界/重叠");
            return false;
        }
        pack->img_entries = (const pq_img_entry_t *)(img + off_entries);
        pack->sprites = (const pq_sprite_t *)(img + off_sprites);
        pack->cutins = (const pq_cutin_t *)(img + off_cutins);
        pack->titles = (const pq_title_t *)(img + off_titles);
        pack->asset_map = (const uint16_t *)(img + off_asset);
        pack->asset_cutin = (const uint16_t *)(img + off_asset_cutin);
        for (uint16_t i = 0; i < pack->img_entry_count; i++) {
            const pq_img_entry_t *e = &pack->img_entries[i];
            if (e->kind > PQ_IMG_PATCH) {
                set_err(err, err_cap, "图片类型未知");
                return false;
            }
            if (!range_ok(e->data_off, e->data_len, pack->img_size) ||
                e->w == 0 || e->h == 0) {
                set_err(err, err_cap, "图片条目越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->sprite_count; i++) {
            const pq_sprite_t *s = &pack->sprites[i];
            if (s->body >= pack->img_entry_count ||
                (s->patch != PQ_NO_IMAGE && s->patch >= pack->img_entry_count)) {
                set_err(err, err_cap, "立绘条目越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->cutin_count; i++) {
            if (pack->cutins[i].image >= pack->img_entry_count) {
                set_err(err, err_cap, "cut-in 条目越界");
                return false;
            }
        }
        for (uint8_t i = 0; i < pack->title_count; i++) {
            if (pack->titles[i].image >= pack->img_entry_count) {
                set_err(err, err_cap, "标题图条目越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->asset_count; i++) {
            uint16_t v = pack->asset_map[i];
            if (v != PQ_NO_IMAGE && !(v & 0x8000) && v >= pack->img_entry_count) {
                set_err(err, err_cap, "资源映射越界");
                return false;
            }
            if (v != PQ_NO_IMAGE && (v & 0x8000) &&
                (v & 0x7FFF) >= pack->sprite_count) {
                set_err(err, err_cap, "资源立绘映射越界");
                return false;
            }
            uint16_t c = pack->asset_cutin[i];
            if (c != PQ_NO_IMAGE && c >= pack->cutin_count) {
                set_err(err, err_cap, "资源 cut-in 映射越界");
                return false;
            }
        }
    }

    // ---- 剧本包
    {
        const uint8_t *p = scn;
        if (memcmp(p, "PQSCN1\0\0", 8) != 0) {
            set_err(err, err_cap, "剧本包标识错误");
            return false;
        }
        p += 8;
        uint16_t h[19];
        memcpy(h, p, sizeof(h));
        p += sizeof(h);
        if (h[0] != 1) {
            set_err(err, err_cap, "剧本包版本不兼容");
            return false;
        }
        pack->scene_count = h[1];
        pack->op_count = h[2];
        pack->text_block_count = h[3];
        pack->speaker_count = h[4];
        pack->chapter_count = h[5];
        pack->route_count = h[6];
        pack->tag_count = h[7];
        pack->var_count = h[8];
        pack->visual_count = h[9];
        pack->gallery_group_count = h[10];
        pack->choice_count = h[11];
        pack->ending_count = h[12];
        pack->choice_item_count = h[13];
        pack->gallery_member_count = h[14];
        pack->initial = h[15];
        pack->flow = h[16];
        pack->side = h[17];
        pack->after = h[18];
        uint32_t o[18];
        memcpy(o, p, sizeof(o));
        p += sizeof(o);
        const uint32_t off_scenes = o[0], off_ops = o[1], off_visuals = o[2];
        const uint32_t off_text_blocks = o[3], off_text_data = o[4], off_tags = o[5];
        const uint32_t off_speakers = o[6], off_chapters = o[7], off_routes = o[8];
        const uint32_t off_choices = o[9], off_choice_items = o[10];
        const uint32_t off_endings = o[11], off_gallery = o[12], off_members = o[13];
        const uint32_t off_ending_names = o[14], off_vars = o[15];
        const uint32_t off_strings = o[16], strings_size = o[17];
        if (pack->scene_count == 0 || pack->op_count == 0 || pack->text_block_count == 0 ||
            pack->choice_count == 0 || pack->gallery_group_count == 0) {
            set_err(err, err_cap, "剧本包为空");
            return false;
        }
        uint32_t len_scenes, len_ops, len_visuals, len_blocks, len_tags, len_speakers;
        uint32_t len_chapters, len_routes, len_choices, len_choice_items, len_endings;
        uint32_t len_gallery, len_members, len_vars;
        if (!mul_ok(pack->scene_count, 12, &len_scenes) ||
            !mul_ok(pack->op_count, 16, &len_ops) ||
            !mul_ok(pack->visual_count, 12, &len_visuals) ||
            !mul_ok(pack->text_block_count, 12, &len_blocks) ||
            !mul_ok(pack->tag_count, 4, &len_tags) ||
            !mul_ok(pack->speaker_count, 4, &len_speakers) ||
            !mul_ok(pack->chapter_count, 8, &len_chapters) ||
            !mul_ok(pack->route_count, 8, &len_routes) ||
            !mul_ok(pack->choice_count, 4, &len_choices) ||
            !mul_ok(pack->choice_item_count, 18, &len_choice_items) ||
            !mul_ok(pack->ending_count, 4, &len_endings) ||
            !mul_ok(pack->gallery_group_count, 8, &len_gallery) ||
            !mul_ok(pack->gallery_member_count, 2, &len_members) ||
            !mul_ok(pack->var_count, 4, &len_vars) ||
            off_tags < off_text_data) {
            set_err(err, err_cap, "剧本包段长度溢出/顺序非法");
            return false;
        }
        const uint32_t len_text_data = off_tags - off_text_data;
        if (!range_ok(off_scenes, len_scenes, pack->scn_size) ||
            !range_ok(off_ops, len_ops, pack->scn_size) ||
            !range_ok(off_visuals, len_visuals, pack->scn_size) ||
            !range_ok(off_text_blocks, len_blocks, pack->scn_size) ||
            !range_ok(off_text_data, len_text_data, pack->scn_size) ||
            !range_ok(off_tags, len_tags, pack->scn_size) ||
            !range_ok(off_speakers, len_speakers, pack->scn_size) ||
            !range_ok(off_chapters, len_chapters, pack->scn_size) ||
            !range_ok(off_routes, len_routes, pack->scn_size) ||
            !range_ok(off_choices, len_choices, pack->scn_size) ||
            !range_ok(off_choice_items, len_choice_items, pack->scn_size) ||
            !range_ok(off_endings, len_endings, pack->scn_size) ||
            !range_ok(off_gallery, len_gallery, pack->scn_size) ||
            !range_ok(off_members, len_members, pack->scn_size) ||
            !range_ok(off_ending_names, 4, pack->scn_size) ||
            !range_ok(off_vars, len_vars, pack->scn_size) ||
            !range_ok(off_strings, strings_size, pack->scn_size)) {
            set_err(err, err_cap, "剧本包段越界");
            return false;
        }
        pack->scenes = (const pq_scene_t *)(scn + off_scenes);
        pack->ops = (const pq_op_t *)(scn + off_ops);
        pack->visuals = (const pq_visual_t *)(scn + off_visuals);
        pack->text_blocks = (const pq_text_block_t *)(scn + off_text_blocks);
        pack->text_data = scn + off_text_data;
        pack->text_data_size = off_tags > off_text_data ? off_tags - off_text_data : 0;
        pack->tag_offs = (const uint32_t *)(scn + off_tags);
        pack->speaker_offs = (const uint32_t *)(scn + off_speakers);
        pack->chapters = (const pq_chapter_t *)(scn + off_chapters);
        pack->routes = (const pq_route_t *)(scn + off_routes);
        pack->choices = (const pq_choice_t *)(scn + off_choices);
        pack->choice_items = (const pq_choice_item_t *)(scn + off_choice_items);
        pack->endings = (const pq_ending_t *)(scn + off_endings);
        pack->galleries = (const pq_gallery_t *)(scn + off_gallery);
        pack->gallery_members = (const uint16_t *)(scn + off_members);
        pack->ending_name_offs = (const uint32_t *)(scn + off_ending_names);
        pack->var_offs = (const uint32_t *)(scn + off_vars);
        pack->strings = (const char *)(scn + off_strings);
        pack->strings_size = strings_size;
        pack->ending_name_count = 0;
        for (uint16_t i = 0; i < pack->ending_count; i++) {
            uint16_t idx = pack->endings[i].name_index;
            if (idx + 1u > pack->ending_name_count) {
                pack->ending_name_count = (uint16_t)(idx + 1u);
            }
        }
        if (pack->ending_name_count == 0) {
            set_err(err, err_cap, "结局名表为空");
            return false;
        }
        {
            uint32_t len_end_names;
            if (!mul_ok(pack->ending_name_count, 4, &len_end_names)) {
                set_err(err, err_cap, "结局名表长度溢出");
                return false;
            }
            pq_span_t spans[17] = {
                {off_scenes, len_scenes},
                {off_ops, len_ops},
                {off_visuals, len_visuals},
                {off_text_blocks, len_blocks},
                {off_text_data, len_text_data},
                {off_tags, len_tags},
                {off_speakers, len_speakers},
                {off_chapters, len_chapters},
                {off_routes, len_routes},
                {off_choices, len_choices},
                {off_choice_items, len_choice_items},
                {off_endings, len_endings},
                {off_gallery, len_gallery},
                {off_members, len_members},
                {off_ending_names, len_end_names},
                {off_vars, len_vars},
                {off_strings, strings_size},
            };
            if (!spans_ok(spans, 17, pack->scn_size, PQ_SCN_HEADER)) {
                set_err(err, err_cap, "剧本包段越界/重叠");
                return false;
            }
        }
        // 字符串偏移必须落在字符串段内
        for (uint16_t i = 0; i < pack->tag_count; i++) {
            if (pack->tag_offs[i] >= strings_size) {
                set_err(err, err_cap, "章节标签偏移越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->speaker_count; i++) {
            if (pack->speaker_offs[i] >= strings_size) {
                set_err(err, err_cap, "说话人偏移越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->chapter_count; i++) {
            if (pack->chapters[i].label_off >= strings_size) {
                set_err(err, err_cap, "章节名偏移越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->route_count; i++) {
            if (pack->routes[i].name_off >= strings_size) {
                set_err(err, err_cap, "路线名偏移越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->ending_name_count; i++) {
            if (pack->ending_name_offs[i] >= strings_size) {
                set_err(err, err_cap, "结局名偏移越界");
                return false;
            }
        }
        for (uint16_t i = 0; i < pack->var_count; i++) {
            if (pack->var_offs[i] >= strings_size) {
                set_err(err, err_cap, "变量名偏移越界");
                return false;
            }
        }
        // 场景与 op 的基本一致性
        for (uint16_t i = 0; i < pack->scene_count; i++) {
            const pq_scene_t *s = &pack->scenes[i];
            if ((uint32_t)s->first_op + s->op_count > pack->op_count || s->op_count == 0) {
                set_err(err, err_cap, "场景 op 范围越界");
                return false;
            }
            if (s->tag >= pack->tag_count) {
                set_err(err, err_cap, "场景章节标签越界");
                return false;
            }
            if (s->clear_var != 0xFF && s->clear_var >= pack->var_count) {
                set_err(err, err_cap, "场景 clearFlag 越界");
                return false;
            }
        }
        // op 引用检查
        for (uint32_t i = 0; i < pack->op_count; i++) {
            const pq_op_t *op = &pack->ops[i];
            if (op->kind > PQ_OP_END) {
                set_err(err, err_cap, "op 类型未知");
                return false;
            }
            if (op->kind == PQ_OP_TEXT) {
                if (op->a >= pack->visual_count ||
                    (op->b != 0xFFFF && op->b >= pack->speaker_count) ||
                    op->tag >= pack->tag_count || op->d >= sizeof(pack->text_buf)) {
                    set_err(err, err_cap, "text op 越界");
                    return false;
                }
            } else if (op->kind == PQ_OP_JUMP) {
                if (!pq_scene_by_id(pack, op->a)) {
                    set_err(err, err_cap, "jump 目标场景不存在");
                    return false;
                }
            } else if (op->kind == PQ_OP_CHOICE) {
                if (op->a >= pack->choice_count) {
                    set_err(err, err_cap, "choice 下标越界");
                    return false;
                }
            }
        }
        // 正文块
        uint32_t expect_logical = 0;
        for (uint16_t i = 0; i < pack->text_block_count; i++) {
            const pq_text_block_t *b = &pack->text_blocks[i];
            if (b->raw_len > sizeof(((pq_pack_t *)0)->text_buf)) {
                set_err(err, err_cap, "正文块超过缓冲上限");
                return false;
            }
            if (!range_ok(b->comp_off, b->comp_len, pack->text_data_size)) {
                set_err(err, err_cap, "正文块偏移越界");
                return false;
            }
            if (b->logical_off < expect_logical) {
                set_err(err, err_cap, "正文块逻辑偏移非递增");
                return false;
            }
            expect_logical = b->logical_off + b->raw_len;
        }
        for (uint16_t i = 0; i < pack->scene_count; i++) {
            if (!pq_scene_by_id(pack, pack->scenes[i].id)) {
                set_err(err, err_cap, "场景表 id 异常");
                return false;
            }
        }
        if (!pq_scene_by_id(pack, pack->initial) || !pq_scene_by_id(pack, pack->flow) ||
            !pq_scene_by_id(pack, pack->side) || !pq_scene_by_id(pack, pack->after)) {
            set_err(err, err_cap, "入口场景不存在");
            return false;
        }
        for (uint16_t i = 0; i < pack->choice_count; i++) {
            const pq_choice_t *c = &pack->choices[i];
            if ((uint32_t)c->first_item + c->count > pack->choice_item_count || c->count == 0) {
                set_err(err, err_cap, "选项范围越界");
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- 访问器
int pq_scene_index(const pq_pack_t *pack, uint16_t id)
{
    for (uint16_t i = 0; i < pack->scene_count; i++) {
        if (pack->scenes[i].id == id) {
            return i;
        }
    }
    return -1;
}

const pq_scene_t *pq_scene_by_id(const pq_pack_t *pack, uint16_t id)
{
    int i = pq_scene_index(pack, id);
    return i < 0 ? NULL : &pack->scenes[i];
}

const pq_op_t *pq_op(const pq_pack_t *pack, const pq_scene_t *scene, uint32_t index)
{
    if (!scene || index >= scene->op_count) {
        return NULL;
    }
    uint32_t global = (uint32_t)scene->first_op + index;
    if (global >= pack->op_count) {
        return NULL;
    }
    return &pack->ops[global];
}

const pq_visual_t *pq_visual(const pq_pack_t *pack, const pq_op_t *op)
{
    if (!op || op->kind != PQ_OP_TEXT || op->a >= pack->visual_count) {
        return NULL;
    }
    return &pack->visuals[op->a];
}

const char *pq_string(const pq_pack_t *pack, uint32_t off)
{
    if (off >= pack->strings_size) {
        return "";
    }
    const char *s = pack->strings + off;
    const char *end = pack->strings + pack->strings_size;
    if (memchr(s, 0, (size_t)(end - s)) == NULL) {
        return "";
    }
    return s;
}

const char *pq_speaker(const pq_pack_t *pack, uint16_t index)
{
    if (index == 0xFFFF || index >= pack->speaker_count) {
        return "";
    }
    return pq_string(pack, pack->speaker_offs[index]);
}

const char *pq_tag_label(const pq_pack_t *pack, uint16_t index)
{
    if (index >= pack->tag_count) {
        return "";
    }
    return pq_string(pack, pack->tag_offs[index]);
}

const char *pq_var_name(const pq_pack_t *pack, uint16_t index)
{
    if (index >= pack->var_count) {
        return "";
    }
    return pq_string(pack, pack->var_offs[index]);
}

int pq_var_find(const pq_pack_t *pack, const char *name)
{
    if (!name) {
        return -1;
    }
    for (uint16_t i = 0; i < pack->var_count; i++) {
        if (strcmp(pq_string(pack, pack->var_offs[i]), name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *pq_chapter_label(const pq_pack_t *pack, uint16_t index)
{
    if (index >= pack->chapter_count) {
        return "";
    }
    return pq_string(pack, pack->chapters[index].label_off);
}

uint16_t pq_chapter_scene(const pq_pack_t *pack, uint16_t index)
{
    return index < pack->chapter_count ? pack->chapters[index].scene : 0xFFFF;
}

uint8_t pq_chapter_route(const pq_pack_t *pack, uint16_t index)
{
    return index < pack->chapter_count ? pack->chapters[index].route : 0xFF;
}

const char *pq_route_name(const pq_pack_t *pack, uint16_t index)
{
    if (index >= pack->route_count) {
        return "";
    }
    return pq_string(pack, pack->routes[index].name_off);
}

uint16_t pq_route_first_chapter(const pq_pack_t *pack, uint16_t index)
{
    return index < pack->route_count ? pack->routes[index].first_chapter : 0;
}

uint16_t pq_route_chapter_count(const pq_pack_t *pack, uint16_t index)
{
    return index < pack->route_count ? pack->routes[index].chapter_count : 0;
}

int pq_ending_index(const pq_pack_t *pack, uint16_t scene)
{
    for (uint16_t i = 0; i < pack->ending_count; i++) {
        if (pack->endings[i].scene == scene) {
            return pack->endings[i].name_index;
        }
    }
    return -1;
}

const char *pq_ending_name(const pq_pack_t *pack, uint16_t scene)
{
    int idx = pq_ending_index(pack, scene);
    if (idx < 0 || idx >= pack->ending_name_count) {
        return "";
    }
    return pq_string(pack, pack->ending_name_offs[idx]);
}

const char *pq_gallery_label(const pq_pack_t *pack, uint16_t group)
{
    if (group >= pack->gallery_group_count) {
        return "";
    }
    return pq_string(pack, pack->galleries[group].label_off);
}

uint16_t pq_gallery_first(const pq_pack_t *pack, uint16_t group)
{
    return group < pack->gallery_group_count ? pack->galleries[group].first_member : 0;
}

uint16_t pq_gallery_count(const pq_pack_t *pack, uint16_t group)
{
    return group < pack->gallery_group_count ? pack->galleries[group].count : 0;
}

uint16_t pq_gallery_member(const pq_pack_t *pack, uint16_t group, uint16_t i)
{
    if (group >= pack->gallery_group_count || i >= pq_gallery_count(pack, group)) {
        return 0xFFFF;
    }
    return pack->gallery_members[pack->galleries[group].first_member + i];
}

int pq_gallery_of_asset(const pq_pack_t *pack, uint16_t asset_id)
{
    for (uint16_t g = 0; g < pack->gallery_group_count; g++) {
        uint16_t first = pack->galleries[g].first_member;
        uint16_t count = pack->galleries[g].count;
        for (uint16_t i = 0; i < count; i++) {
            if (pack->gallery_members[first + i] == asset_id) {
                return (int)g;
            }
        }
    }
    return -1;
}

int pq_cg_group_of_scene_image(const pq_pack_t *pack, uint16_t asset_id)
{
    return pq_gallery_of_asset(pack, asset_id);
}

const pq_choice_t *pq_choice(const pq_pack_t *pack, uint16_t index)
{
    return index < pack->choice_count ? &pack->choices[index] : NULL;
}

const pq_choice_item_t *pq_choice_item(const pq_pack_t *pack, const pq_choice_t *choice,
                                       uint16_t i)
{
    if (!choice || i >= choice->count) {
        return NULL;
    }
    uint32_t idx = (uint32_t)choice->first_item + i;
    return idx < pack->choice_item_count ? &pack->choice_items[idx] : NULL;
}

const pq_img_entry_t *pq_img_entry(const pq_pack_t *pack, uint16_t image_index)
{
    return image_index < pack->img_entry_count ? &pack->img_entries[image_index] : NULL;
}

const uint8_t *pq_img_data(const pq_pack_t *pack, const pq_img_entry_t *entry)
{
    if (!entry || !range_ok(entry->data_off, entry->data_len, pack->img_size)) {
        return NULL;
    }
    return pack->img + entry->data_off;
}

uint16_t pq_asset_image(const pq_pack_t *pack, uint16_t asset_id)
{
    if (asset_id >= pack->asset_count) {
        return PQ_NO_IMAGE;
    }
    uint16_t v = pack->asset_map[asset_id];
    if (v == PQ_NO_IMAGE || (v & 0x8000)) {
        return PQ_NO_IMAGE;
    }
    return v;
}

const pq_sprite_t *pq_asset_sprite(const pq_pack_t *pack, uint16_t asset_id)
{
    if (asset_id >= pack->asset_count) {
        return NULL;
    }
    uint16_t v = pack->asset_map[asset_id];
    if (v == PQ_NO_IMAGE || !(v & 0x8000)) {
        return NULL;
    }
    uint16_t idx = v & 0x7FFF;
    return idx < pack->sprite_count ? &pack->sprites[idx] : NULL;
}

const pq_cutin_t *pq_asset_cutin(const pq_pack_t *pack, uint16_t asset_id)
{
    if (asset_id >= pack->asset_count) {
        return NULL;
    }
    uint16_t v = pack->asset_cutin[asset_id];
    return v < pack->cutin_count ? &pack->cutins[v] : NULL;
}

const pq_title_t *pq_title(const pq_pack_t *pack, uint8_t index)
{
    return index < pack->title_count ? &pack->titles[index] : NULL;
}

// ---------------------------------------------------------------- 正文
static int block_for(uint32_t logical_off, const pq_pack_t *pack)
{
    // 找到最后一个 logical_off <= 目标的块，再确认目标落在块内。
    // 空块（raw_len=0）在打包时被丢弃，但这里仍按"可重复起点"防御，避免
    // 二分落到空块上导致正文读取失败。
    int lo = 0, hi = (int)pack->text_block_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pack->text_blocks[mid].logical_off <= logical_off) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) {
        return -1;
    }
    for (int i = best; i >= 0 && i > best - 8; i--) {
        const pq_text_block_t *b = &pack->text_blocks[i];
        if (b->logical_off <= logical_off && logical_off < b->logical_off + b->raw_len) {
            return i;
        }
        if (b->logical_off < pack->text_blocks[best].logical_off) {
            break;
        }
    }
    return -1;
}

static bool load_block(pq_pack_t *pack, uint16_t block)
{
    if (pack->text_buf_block == block) {
        return true;
    }
    if (block >= pack->text_block_count) {
        return false;
    }
    const pq_text_block_t *b = &pack->text_blocks[block];
    if (b->raw_len > sizeof(pack->text_buf)) {
        return false;
    }
    if (!range_ok(b->comp_off, b->comp_len, pack->text_data_size)) {
        return false;
    }
    size_t produced = pq_inflate(pack->text_buf, b->raw_len, pack->text_data + b->comp_off,
                                 b->comp_len);
    if (produced != b->raw_len) {
        pack->text_buf_block = 0xFFFF;
        return false;
    }
    pack->text_buf_block = block;
    pack->text_buf_len = b->raw_len;
    return true;
}

const char *pq_text_get(pq_pack_t *pack, uint32_t logical_off, uint16_t len)
{
    int block = block_for(logical_off, pack);
    if (block < 0 || !load_block(pack, (uint16_t)block)) {
        return NULL;
    }
    const pq_text_block_t *b = &pack->text_blocks[block];
    uint32_t local = logical_off - b->logical_off;
    if (local >= b->raw_len) {
        return NULL;
    }
    const char *s = (const char *)pack->text_buf + local;
    uint32_t avail = b->raw_len - local;
    if (len == 0 || len >= avail) {
        return NULL;
    }
    if (memchr(s, 0, len) != NULL || s[len] != 0) {
        return NULL;   // 声明长度内不能有 NUL，且下一字节必须是 NUL
    }
    return s;
}

bool pq_text_block_contains(pq_pack_t *pack, uint32_t logical_off)
{
    int block = block_for(logical_off, pack);
    return block >= 0 && load_block(pack, (uint16_t)block);
}
