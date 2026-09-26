// main/starry_pack.c —— 资源包解析(纯逻辑,无 ESP-IDF 依赖)。
#include "starry_pack.h"

#include <string.h>

#define PACK_HEADER_SIZE 20u          // magic(8) + version(4) + total(4) + section_count(4)
#define PACK_SECTION_SIZE 16u
#define STARRY_BG_ENTRY 14u           // { off, len, w, h, flags }
#define STARRY_FG_ENTRY 26u           // { color_off, color_len, mask_off, mask_len, w, h, x,
                                      //   y, owner }
#define STARRY_SCENE_ENTRY 16u
#define STARRY_DLG_ENTRY 14u
#define STARRY_CHAPTER_ENTRY 12u
#define STARRY_NAME_ENTRY 8u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool section_span(uint32_t size, uint32_t off, uint32_t len)
{
    if (off > size) return false;
    return len <= size - off;
}

bool starry_pack_open(starry_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (!pack || !data || size < PACK_HEADER_SIZE) return false;
    if (memcmp(data, STARRY_PACK_MAGIC, 8) != 0) return false;
    if (rd32(data + 8) != STARRY_PACK_VERSION) return false;
    if (rd32(data + 12) != size) return false;

    const uint32_t section_count = rd32(data + 16);
    if (section_count == 0 || section_count > 32) return false;
    if (PACK_HEADER_SIZE + section_count * PACK_SECTION_SIZE > size) return false;

    starry_pack_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;

    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *rec = data + PACK_HEADER_SIZE + i * PACK_SECTION_SIZE;
        const uint32_t type = rd32(rec);
        const uint32_t off = rd32(rec + 4);
        const uint32_t count = rd32(rec + 8);
        const uint32_t sec_size = rd32(rec + 12);
        if (!section_span(size, off, sec_size)) return false;
        const uint8_t *body = data + off;
        switch (type) {
        case 0: view.text = body; view.text_size = sec_size; break;
        case 1:
            if (sec_size < count * STARRY_NAME_ENTRY) return false;
            view.names = body;
            view.name_count = count;
            break;
        case 2:
            if (sec_size < count * STARRY_CHAPTER_ENTRY) return false;
            view.chapters = body;
            view.chapter_count = count;
            break;
        case 3:
            if (sec_size < count * STARRY_SCENE_ENTRY) return false;
            view.scenes = body;
            view.scene_count = count;
            break;
        case 4:
            if (sec_size < count * STARRY_DLG_ENTRY) return false;
            view.dialogues = body;
            view.dialogue_count = count;
            break;
        case 5:
            if (sec_size < count * STARRY_BG_ENTRY) return false;
            view.bgs = body;
            view.bg_data = body + count * STARRY_BG_ENTRY;
            view.bg_data_size = sec_size - count * STARRY_BG_ENTRY;
            view.bg_count = count;
            break;
        case 6:
            if (sec_size < count * STARRY_FG_ENTRY) return false;
            view.fgs = body;
            view.fg_data = body + count * STARRY_FG_ENTRY;
            view.fg_data_size = sec_size - count * STARRY_FG_ENTRY;
            view.fg_count = count;
            break;
        case 7: view.meta = body; view.meta_size = sec_size; break;
        default: break;   // 未知段:忽略,便于向后兼容
        }
    }

    if (!view.text || !view.chapters || !view.scenes || !view.dialogues) return false;
    if (view.chapter_count > STARRY_NONE || view.scene_count > STARRY_NONE ||
        view.dialogue_count > STARRY_NONE) {
        return false;   // 记录下标用 u16 表达,超了就是包不兼容
    }

    *pack = view;
    return true;
}

void starry_pack_chapter(const starry_pack_t *pack, uint16_t index, starry_chapter_t *out)
{
    if (!pack || !out) return;
    memset(out, 0, sizeof(*out));
    out->next = STARRY_NONE;
    out->first_scene = STARRY_NONE;
    out->first_dlg = STARRY_NONE;
    if (!pack->chapters || index >= pack->chapter_count) return;
    const uint8_t *p = pack->chapters + (size_t)index * STARRY_CHAPTER_ENTRY;
    out->id = rd16(p);
    out->first_scene = rd16(p + 2);
    out->scene_count = rd16(p + 4);
    out->first_dlg = rd16(p + 6);
    out->dlg_count = rd16(p + 8);
    out->next = rd16(p + 10);
}

void starry_pack_scene(const starry_pack_t *pack, uint16_t index, starry_scene_t *out)
{
    if (!pack || !out) return;
    memset(out, 0, sizeof(*out));
    out->bg = STARRY_NONE;
    out->choice_name[0] = STARRY_NONE;
    out->choice_name[1] = STARRY_NONE;
    out->choice_target[0] = STARRY_NONE;
    out->choice_target[1] = STARRY_NONE;
    if (!pack->scenes || index >= pack->scene_count) return;
    const uint8_t *p = pack->scenes + (size_t)index * STARRY_SCENE_ENTRY;
    out->bg = rd16(p);
    out->choice_count = p[2];
    out->first_dlg = rd16(p + 4);
    out->dlg_count = rd16(p + 6);
    out->choice_name[0] = rd16(p + 8);
    out->choice_name[1] = rd16(p + 10);
    out->choice_target[0] = rd16(p + 12);
    out->choice_target[1] = rd16(p + 14);
}

void starry_pack_dialogue(const starry_pack_t *pack, uint16_t index, starry_dialogue_t *out)
{
    if (!pack || !out) return;
    memset(out, 0, sizeof(*out));
    out->name = STARRY_NONE;
    out->sprite = STARRY_FG_KEEP;
    if (!pack->dialogues || index >= pack->dialogue_count) return;
    const uint8_t *p = pack->dialogues + (size_t)index * STARRY_DLG_ENTRY;
    out->text_off = rd32(p);
    out->text_len = rd16(p + 4);
    out->name = rd16(p + 6);
    out->sprite = rd16(p + 8);
    out->flags = p[10];
    out->jump = p[11];
    out->arg = rd16(p + 12);
}

// 按 UTF-8 字符边界截断:返回可放下的字节数(不拆多字节字符)。
static size_t utf8_fit(const uint8_t *src, size_t len, size_t room)
{
    size_t used = 0;
    while (used < len) {
        const uint8_t lead = src[used];
        size_t step = 1;
        if ((lead & 0xE0u) == 0xC0u) step = 2;
        else if ((lead & 0xF0u) == 0xE0u) step = 3;
        else if ((lead & 0xF8u) == 0xF0u) step = 4;
        if (used + step > len) break;      // 源数据尾部残缺,丢弃
        if (used + step > room) break;
        used += step;
    }
    return used;
}

static size_t copy_string(const uint8_t *src, uint32_t src_len, char *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    if (!src || src_len == 0) {
        out[0] = '\0';
        return 0;
    }
    const size_t room = capacity - 1;
    const size_t take = src_len <= room ? src_len : utf8_fit(src, src_len, room);
    memcpy(out, src, take);
    out[take] = '\0';
    return take;
}

size_t starry_pack_text(const starry_pack_t *pack, uint32_t off, uint32_t len, char *out,
                        size_t capacity)
{
    if (!pack || !pack->text || off > pack->text_size) return copy_string(NULL, 0, out, capacity);
    if (len > pack->text_size - off) len = pack->text_size - off;
    return copy_string(pack->text + off, len, out, capacity);
}

size_t starry_pack_name(const starry_pack_t *pack, uint16_t id, char *out, size_t capacity)
{
    if (!pack || !pack->names || id >= pack->name_count) {
        return copy_string(NULL, 0, out, capacity);
    }
    const uint8_t *rec = pack->names + (size_t)id * STARRY_NAME_ENTRY;
    return starry_pack_text(pack, rd32(rec), rd16(rec + 4), out, capacity);
}

bool starry_pack_bg(const starry_pack_t *pack, uint16_t id, starry_bg_t *out)
{
    if (!pack || !out) return false;
    memset(out, 0, sizeof(*out));
    if (!pack->bgs || !pack->bg_data || id >= pack->bg_count) return false;
    const uint8_t *rec = pack->bgs + (size_t)id * STARRY_BG_ENTRY;
    const uint32_t off = rd32(rec);
    const uint32_t len = rd32(rec + 4);
    if (off > pack->bg_data_size || len > pack->bg_data_size - off) return false;
    out->jpeg = pack->bg_data + off;
    out->jpeg_len = len;
    out->w = rd16(rec + 8);
    out->h = rd16(rec + 10);
    out->no_sprite = (rd16(rec + 12) & STARRY_BG_FLAG_NO_SPRITE) != 0;
    return true;
}

bool starry_pack_fg(const starry_pack_t *pack, uint16_t id, starry_fg_t *out)
{
    if (!pack || !out) return false;
    memset(out, 0, sizeof(*out));
    if (!pack->fgs || !pack->fg_data || id >= pack->fg_count) return false;
    const uint8_t *rec = pack->fgs + (size_t)id * STARRY_FG_ENTRY;
    const uint32_t color_off = rd32(rec);
    const uint32_t color_len = rd32(rec + 4);
    const uint32_t mask_off = rd32(rec + 8);
    const uint32_t mask_len = rd32(rec + 12);
    if (color_off > pack->fg_data_size || color_len > pack->fg_data_size - color_off) return false;
    if (mask_off > pack->fg_data_size || mask_len > pack->fg_data_size - mask_off) return false;
    out->color = pack->fg_data + color_off;
    out->color_len = color_len;
    out->mask = pack->fg_data + mask_off;
    out->mask_len = mask_len;
    out->w = rd16(rec + 16);
    out->h = rd16(rec + 18);
    out->x = rd16(rec + 20);
    out->y = rd16(rec + 22);
    // 归属角色:打包器按"谁引用了这张立绘"统计得出,是"本人说话才显示"规则的唯一依据。
    // 漏读会让所有立绘都退化成 name 表 0 号(诺瓦),立绘几乎不再出现。
    out->owner = rd16(rec + 24);
    return out->w > 0 && out->h > 0;
}

int starry_pack_find_chapter(const starry_pack_t *pack, uint16_t id)
{
    if (!pack || !pack->chapters) return -1;
    for (uint32_t i = 0; i < pack->chapter_count; ++i) {
        if (starry_pack_chapter_id(pack, (uint16_t)i) == id) return (int)i;
    }
    return -1;
}

uint16_t starry_pack_chapter_id(const starry_pack_t *pack, uint16_t index)
{
    starry_chapter_t ch;
    starry_pack_chapter(pack, index, &ch);
    return ch.id;
}

int starry_pack_chapter_of_scene(const starry_pack_t *pack, uint16_t scene, uint16_t *scene_out)
{
    if (!pack || !pack->chapters) return -1;
    for (uint32_t i = 0; i < pack->chapter_count; ++i) {
        starry_chapter_t ch;
        starry_pack_chapter(pack, (uint16_t)i, &ch);
        if (scene >= ch.first_scene && scene < (uint16_t)(ch.first_scene + ch.scene_count)) {
            if (scene_out) *scene_out = (uint16_t)(scene - ch.first_scene);
            return (int)i;
        }
    }
    return -1;
}
