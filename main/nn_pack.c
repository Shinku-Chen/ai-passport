// main/nn_pack.c —— 资源包解析（纯逻辑,宿主可测）。
//
// 校验策略是 **fail closed**：包在 .rodata 里，正常构建不会坏；但打包脚本、
// 截断或替换出错时必须当场拒绝，而不是让图像静默读到下一段的字节。
// 这里拒绝的具体情形：
//   * 魔数/版本/总长不符；段数超出已知范围或段表越界；
//   * 段表自身与数据重叠、段类型未知或重复、任意两段重叠；
//   * 记录数 × 记录大小超出该段（含乘法溢出）；
//   * 图像记录的 off/jpeg_len/mask_len 超出该段的**数据区**（不含记录表）。
#include "nn_pack.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// count * rec 是否会溢出/超过 bytes
static bool fits_records(uint32_t count, uint32_t rec_size, uint32_t bytes)
{
    if (rec_size != 0 && count > UINT32_MAX / rec_size) return false;
    return count * rec_size <= bytes;
}

typedef struct {
    uint32_t type;
    uint32_t off;
    uint32_t count;
    uint32_t bytes;
} section_t;

// 已知段类型白名单。NN_SEC_COUNT 只是段表容量上限（32），不是合法类型范围；
// 用容量当上界会让 22..31 这些“未知类型”混过去。
static bool section_type_known(uint32_t type)
{
    switch (type) {
    case NN_SEC_TEXT:
    case NN_SEC_META:
    case NN_SEC_IMAGE:
    case NN_SEC_SCENE:
    case NN_SEC_OP:
    case NN_SEC_SPEAKER:
    case NN_SEC_CHOICE:
    case NN_SEC_ENDING:
        return true;
    default:
        return false;
    }
}

// 读段表并做与顺序无关的结构校验。
static bool parse_sections(const uint8_t *blob, size_t size, section_t *out, uint32_t *out_n)
{
    if (size < 20) return false;
    if (memcmp(blob, NN_PACK_MAGIC, 6) != 0) return false;
    if (rd32(blob + 12) != size) return false;

    const uint32_t sections = rd32(blob + 16);
    // 已知段类型只有 NN_SEC_COUNT 个，段数不可能超过它
    if (sections == 0 || sections > NN_SEC_COUNT) return false;
    if (!fits_records(sections, 16u, (uint32_t)size - 20u)) return false;
    const uint32_t table_end = 20u + 16u * sections;

    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t *rec = blob + 20 + 16 * i;
        section_t s;
        s.type = rd32(rec);
        s.off = rd32(rec + 4);
        s.count = rd32(rec + 8);
        s.bytes = rd32(rec + 12);
        if (!section_type_known(s.type)) return false;
        // 段不能覆盖段表本身
        if (s.off < table_end) return false;
        if (s.off > size || s.bytes > size - s.off) return false;
        for (uint32_t k = 0; k < i; ++k) {
            if (out[k].type == s.type) return false;                  // 重复段类型
            const uint32_t a_end = out[k].off + out[k].bytes;
            const uint32_t b_end = s.off + s.bytes;
            if (s.off < a_end && out[k].off < b_end) return false;    // 段重叠
        }
        out[i] = s;
    }
    *out_n = sections;
    return true;
}

// 文本引用校验：偏移/长度必须落在 text 段内，并且 off+len 处必须正好是 NUL。
// 所有字符串在打包时都以 NUL 结尾，所以“越界但没有崩溃”的坏包必须在这里被拒绝，
// 而不是留一个访问时才返回 NULL 的半可用包。
static bool text_ref_ok(const nn_pack_t *pack, uint32_t off, uint32_t len)
{
    if (off > pack->text_size) return false;
    if (len > pack->text_size - off) return false;
    if (len == 0) return true;
    if (off + len >= pack->text_size) return false;     // 末尾必须有 NUL
    return pack->text[off + len] == (char)0;
}

static const section_t *find(const section_t *sections, uint32_t n, uint32_t type)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (sections[i].type == type) return &sections[i];
    }
    return NULL;
}

bool nn_pack_init(nn_pack_t *pack, const uint8_t *blob, size_t size)
{
    if (!pack) return false;
    memset(pack, 0, sizeof(*pack));
    if (!blob || size < 20) return false;
    if (rd32(blob + 8) != NN_PACK_VERSION) return false;

    section_t sections[NN_SEC_COUNT];
    uint32_t nsec = 0;
    if (!parse_sections(blob, size, sections, &nsec)) return false;

    // pack->blob/size 必须赋值：nn_pack_image_data() 的二次边界靠它计算。
    pack->blob = blob;
    pack->size = size;

    const section_t *text = find(sections, nsec, NN_SEC_TEXT);
    const section_t *image = find(sections, nsec, NN_SEC_IMAGE);
    const section_t *scene = find(sections, nsec, NN_SEC_SCENE);
    const section_t *op = find(sections, nsec, NN_SEC_OP);
    const section_t *choice = find(sections, nsec, NN_SEC_CHOICE);
    const section_t *ending = find(sections, nsec, NN_SEC_ENDING);
    const section_t *speaker = find(sections, nsec, NN_SEC_SPEAKER);

    if (!text || !image || !scene || !op || !choice || !ending) return false;

    pack->text = blob + text->off;
    pack->text_size = text->bytes;

    // 图像段 = 记录表 + 紧邻的数据区；数据区的合法范围必须扣掉记录表。
    if (!fits_records(image->count, 20u, image->bytes)) return false;
    const uint32_t image_table = image->count * 20u;
    const uint32_t image_data_bytes = image->bytes - image_table;
    pack->images = (const nn_image_t *)(blob + image->off);
    pack->image_count = image->count;
    for (uint32_t i = 0; i < image->count; ++i) {
        const uint8_t *rec = (const uint8_t *)pack->images + 20u * i;
        const uint32_t off = rd32(rec + 0);
        const uint32_t jlen = rd32(rec + 4);
        const uint32_t mlen = rd32(rec + 8);
        if (off > image_data_bytes || jlen > image_data_bytes - off) return false;
        if (mlen > image_data_bytes - off - jlen) return false;
    }

    if (!fits_records(scene->count, 12u, scene->bytes)) return false;
    pack->scenes = (const nn_scene_t *)(blob + scene->off);
    pack->scene_count = scene->count;

    if (!fits_records(op->count, 12u, op->bytes)) return false;
    pack->ops = (const nn_op_t *)(blob + op->off);
    pack->op_count = op->count;

    if (!fits_records(choice->count, 12u, choice->bytes)) return false;
    pack->choices = (const nn_choice_t *)(blob + choice->off);
    pack->choice_count = choice->count;

    if (!fits_records(ending->count, 16u, ending->bytes)) return false;
    pack->endings = (const nn_ending_t *)(blob + ending->off);
    pack->ending_count = ending->count;

    if (speaker) {
        if (!fits_records(speaker->count, 8u, speaker->bytes)) return false;
        pack->speakers = blob + speaker->off;
        pack->speaker_count = speaker->count;
    }
    const section_t *meta = find(sections, nsec, NN_SEC_META);
    if (meta) {
        pack->meta = (const char *)(blob + meta->off);
        pack->meta_size = meta->bytes;
    }

    if (pack->scene_count == 0 || pack->op_count == 0) return false;

    // 场景 op 区间必须落在 op 表内
    for (uint32_t i = 0; i < pack->scene_count; ++i) {
        const uint32_t first = pack->scenes[i].first_op;
        const uint32_t cnt = pack->scenes[i].op_count;
        if (first > pack->op_count || cnt > pack->op_count - first) return false;
    }
    // 跳转与选项目标必须在范围内
    for (uint32_t i = 0; i < pack->op_count; ++i) {
        const nn_op_t *o = &pack->ops[i];
        if (o->type == NN_OP_JUMP) {
            if (o->a >= pack->scene_count) return false;
            if (o->b > pack->scenes[o->a].op_count) return false;
        } else if (o->type == NN_OP_CHOICE) {
            if (o->a > pack->choice_count || o->b > pack->choice_count - o->a) return false;
            for (uint32_t k = 0; k < o->b; ++k) {
                const nn_choice_t *c = &pack->choices[o->a + k];
                if (c->scene >= pack->scene_count) return false;
                if (c->op > pack->scenes[c->scene].op_count) return false;
            }
        } else if (o->type == NN_OP_END) {
            if (o->a >= pack->ending_count) return false;
        } else if (o->type == NN_OP_SPRITE || o->type == NN_OP_BG || o->type == NN_OP_EV) {
            if (o->a != NN_NONE16 && o->a >= pack->image_count) return false;
        } else if (o->type > NN_OP_FX) {
            return false;   // 未知操作码：宁可拒绝也不要静默跳过
        }
        if ((o->type == NN_OP_SAY || o->type == NN_OP_CHAPTER)) {
            if (!text_ref_ok(pack, o->b, o->c)) return false;
        }
    }
    // 记录内的文本引用（场景名 / 选项 / 结局 / 说话人）同样在 init 阶段校验
    for (uint32_t i = 0; i < pack->scene_count; ++i) {
        if (!text_ref_ok(pack, pack->scenes[i].name_off, pack->scenes[i].name_len)) return false;
    }
    for (uint32_t i = 0; i < pack->choice_count; ++i) {
        if (!text_ref_ok(pack, pack->choices[i].text_off, pack->choices[i].text_len)) return false;
    }
    for (uint32_t i = 0; i < pack->ending_count; ++i) {
        if (!text_ref_ok(pack, pack->endings[i].name_off, pack->endings[i].name_len)) return false;
        if (!text_ref_ok(pack, pack->endings[i].text_off, pack->endings[i].text_len)) return false;
    }
    for (uint32_t i = 0; i < pack->speaker_count; ++i) {
        const uint8_t *rec = pack->speakers + 8u * i;
        if (!text_ref_ok(pack, rd32(rec), (uint32_t)(rec[4] | (rec[5] << 8)))) return false;
    }
    return true;
}

const char *nn_pack_text(const nn_pack_t *pack, uint32_t off, uint32_t len)
{
    if (!pack || !pack->text) return NULL;
    if (off > pack->text_size || len > pack->text_size - off) return NULL;
    return (const char *)pack->text + off;
}

const nn_image_t *nn_pack_image(const nn_pack_t *pack, uint16_t id)
{
    if (!pack || !pack->images || id >= pack->image_count) return NULL;
    return &pack->images[id];
}

const uint8_t *nn_pack_image_data(const nn_pack_t *pack, const nn_image_t *img)
{
    if (!pack || !img || !pack->images || !pack->blob) return NULL;
    if (img < pack->images || img >= pack->images + pack->image_count) return NULL;

    // 数据区紧跟在图像记录表之后；二次边界用 pack->blob/size 计算，
    // 这样即使调用方传入别的表的记录也不会越界。
    const uint8_t *base = (const uint8_t *)pack->images;
    const uint8_t *data = base + (size_t)pack->image_count * 20u;
    const size_t data_off = (size_t)(data - pack->blob);
    if (data_off > pack->size) return NULL;
    const uint32_t limit = (uint32_t)(pack->size - data_off);

    const uint32_t off = img->off;
    if (off > limit || img->jpeg_len > limit - off) return NULL;
    if (img->mask_len > limit - off - img->jpeg_len) return NULL;
    return data + off;
}

const char *nn_pack_scene_name(const nn_pack_t *pack, uint32_t scene, uint16_t *len)
{
    if (!pack || scene >= pack->scene_count) return NULL;
    const nn_scene_t *s = &pack->scenes[scene];
    const char *txt = nn_pack_text(pack, s->name_off, s->name_len);
    if (txt && len) *len = s->name_len;
    return txt;
}

const char *nn_pack_speaker(const nn_pack_t *pack, uint16_t id, uint16_t *len)
{
    if (!pack || id == NN_NONE16 || !pack->speakers || id >= pack->speaker_count) return NULL;
    const uint8_t *rec = pack->speakers + 8u * id;
    const uint32_t off = rd32(rec);
    const uint16_t n = (uint16_t)(rec[4] | (rec[5] << 8));
    const char *txt = nn_pack_text(pack, off, n);
    if (txt && len) *len = n;
    return txt;
}
