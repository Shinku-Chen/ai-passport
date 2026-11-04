// main/eden_pack.c —— 资源包只读解析实现(纯字节映射,不依赖 ESP-IDF)。
#include "eden_pack.h"

#include <string.h>

#define EDEN_HEADER_SIZE 20u
#define EDEN_SECTION_SIZE 16u
#define EDEN_OP_SIZE 12u
#define EDEN_IMAGE_SIZE 16u
#define EDEN_FRAME_SIZE 6u
#define EDEN_PATCH_SIZE 6u
#define EDEN_CHAP_SIZE 8u
#define EDEN_GAL_SIZE 6u
#define EDEN_SCENE_SIZE 8u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// 段的"记录大小",用来把 count 与 size 对齐校验(防止半截记录被当成合法数据)。
static uint32_t record_size(uint8_t type)
{
    switch (type) {
    case EDEN_SEC_TXIDX: return 4;
    case EDEN_SEC_OPS: return EDEN_OP_SIZE;
    case EDEN_SEC_SCENE: return EDEN_SCENE_SIZE;
    case EDEN_SEC_IMAGE: return EDEN_IMAGE_SIZE;
    case EDEN_SEC_FRAME: return EDEN_FRAME_SIZE;
    case EDEN_SEC_PATCH: return EDEN_PATCH_SIZE;
    case EDEN_SEC_CHAP: return EDEN_CHAP_SIZE;
    case EDEN_SEC_GAL: return EDEN_GAL_SIZE;
    default: return 1;   // TEXT / BLOB 是字节流
    }
}

bool eden_pack_section(const eden_pack_t *pack, uint8_t type,
                       const uint8_t **data, uint32_t *count, uint32_t *size)
{
    if (pack == NULL || type >= EDEN_SEC_COUNT) {
        return false;
    }
    const eden_section_t *section = &pack->sections[type];
    if (data) {
        *data = pack->base + section->offset;
    }
    if (count) {
        *count = section->count;
    }
    if (size) {
        *size = section->size;
    }
    return true;
}

bool eden_pack_open(eden_pack_t *pack, const void *data, uint32_t size)
{
    if (pack == NULL || data == NULL || size < EDEN_HEADER_SIZE) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, EDEN_PACK_MAGIC, 8) != 0) {
        return false;
    }
    const uint16_t version = rd16(bytes + 8);
    const uint16_t header_size = rd16(bytes + 10);
    const uint16_t section_count = rd16(bytes + 12);
    const uint32_t total = rd32(bytes + 16);
    if (version != EDEN_PACK_VERSION || total != size) {
        return false;
    }
    if (section_count > EDEN_SEC_COUNT) {
        return false;
    }
    if (header_size < EDEN_HEADER_SIZE + (uint32_t)section_count * EDEN_SECTION_SIZE ||
        header_size > size) {
        return false;
    }

    memset(pack, 0, sizeof(*pack));
    pack->base = bytes;
    pack->size = size;
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *record = bytes + EDEN_HEADER_SIZE + (uint32_t)index * EDEN_SECTION_SIZE;
        const uint32_t type = rd32(record);
        const uint32_t offset = rd32(record + 4);
        const uint32_t count = rd32(record + 8);
        const uint32_t length = rd32(record + 12);
        if (type >= EDEN_SEC_COUNT) {
            return false;
        }
        if (offset < header_size || offset > size || length > size - offset) {
            return false;
        }
        if (type != EDEN_SEC_TEXT && type != EDEN_SEC_BLOB) {
            const uint32_t unit = record_size((uint8_t)type);
            if (unit != 0 && length % unit != 0) {
                return false;
            }
            if (unit != 0 && length / unit < count) {
                return false;
            }
        }
        pack->sections[type].offset = offset;
        pack->sections[type].count = count;
        pack->sections[type].size = length;
    }

    // 必备段:没有它们固件无法启动,直接判为坏包。
    for (uint8_t type = EDEN_SEC_TEXT; type <= EDEN_SEC_GAL; type++) {
        if (pack->sections[type].size == 0) {
            return false;
        }
    }
    return true;
}

uint16_t eden_pack_scene_count(const eden_pack_t *pack)
{
    // 场景表下标 0 是空位,场景号 1 起。
    if (pack->sections[EDEN_SEC_SCENE].count == 0) {
        return 0;
    }
    return (uint16_t)(pack->sections[EDEN_SEC_SCENE].count - 1);
}

uint16_t eden_pack_chapter_count(const eden_pack_t *pack)
{
    return (uint16_t)pack->sections[EDEN_SEC_CHAP].count;
}

uint16_t eden_pack_gallery_count(const eden_pack_t *pack)
{
    return (uint16_t)pack->sections[EDEN_SEC_GAL].count;
}

uint16_t eden_pack_frame_count(const eden_pack_t *pack)
{
    return (uint16_t)pack->sections[EDEN_SEC_FRAME].count;
}

uint16_t eden_pack_text_count(const eden_pack_t *pack)
{
    const uint32_t count = pack->sections[EDEN_SEC_TXIDX].count;
    return count ? (uint16_t)(count - 1) : 0;
}

bool eden_pack_scene_range(const eden_pack_t *pack, uint16_t scene,
                           uint32_t *first, uint32_t *count)
{
    if (scene == 0 || scene > eden_pack_scene_count(pack)) {
        return false;
    }
    const uint8_t *table = pack->base + pack->sections[EDEN_SEC_SCENE].offset;
    const uint32_t start = rd32(table + (uint32_t)scene * EDEN_SCENE_SIZE);
    const uint32_t end = rd32(table + (uint32_t)scene * EDEN_SCENE_SIZE + 4);
    if (end < start || end > pack->sections[EDEN_SEC_OPS].count) {
        return false;
    }
    if (first) {
        *first = start;
    }
    if (count) {
        *count = end - start;
    }
    return true;
}

uint16_t eden_pack_scene_index_of(const eden_pack_t *pack, uint16_t scene)
{
    return (scene >= 1 && scene <= eden_pack_scene_count(pack)) ? scene : 0;
}

bool eden_pack_op(const eden_pack_t *pack, uint32_t index, eden_op_t *out)
{
    if (index >= pack->sections[EDEN_SEC_OPS].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_OPS].offset +
                            index * EDEN_OP_SIZE;
    out->kind = record[0];
    out->a = record[1];
    out->b = rd16(record + 2);
    out->c = rd32(record + 4);
    out->d = rd32(record + 8);
    return true;
}

const char *eden_pack_text(const eden_pack_t *pack, uint32_t index)
{
    if (index + 1 > eden_pack_text_count(pack)) {
        return NULL;
    }
    const uint8_t *offsets = pack->base + pack->sections[EDEN_SEC_TXIDX].offset;
    const uint32_t start = rd32(offsets + index * 4);
    const uint32_t end = rd32(offsets + (index + 1) * 4);
    const uint32_t size = pack->sections[EDEN_SEC_TEXT].size;
    if (start >= size || end > size || end < start) {
        return NULL;
    }
    // 包内字符串自带 NUL;再确认一次,坏包不会把非字符串数据当文本返回。
    const uint8_t *text = pack->base + pack->sections[EDEN_SEC_TEXT].offset + start;
    if (end > start && text[end - start - 1] != 0) {
        return NULL;
    }
    return (const char *)text;
}

bool eden_pack_image(const eden_pack_t *pack, uint16_t index, eden_pack_image_t *out)
{
    if (index >= pack->sections[EDEN_SEC_IMAGE].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_IMAGE].offset +
                            (uint32_t)index * EDEN_IMAGE_SIZE;
    const uint32_t offset = rd32(record);
    const uint32_t length = rd32(record + 4);
    if (length == 0 || offset > pack->sections[EDEN_SEC_BLOB].size ||
        length > pack->sections[EDEN_SEC_BLOB].size - offset) {
        return false;
    }
    out->data = pack->base + pack->sections[EDEN_SEC_BLOB].offset + offset;
    out->len = length;
    out->width = rd16(record + 8);
    out->height = rd16(record + 10);
    out->content_w = rd16(record + 12);
    out->content_h = rd16(record + 14);
    if (out->width == 0 || out->height == 0 ||
        out->content_w > out->width || out->content_h > out->height) {
        return false;
    }
    return true;
}

bool eden_pack_frame(const eden_pack_t *pack, uint16_t index, eden_frame_t *out)
{
    if (index >= pack->sections[EDEN_SEC_FRAME].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_FRAME].offset +
                            (uint32_t)index * EDEN_FRAME_SIZE;
    out->base = rd16(record);
    out->patch_first = rd16(record + 2);
    out->patch_count = record[4];
    if (out->patch_count != 0 &&
        (uint32_t)out->patch_first + out->patch_count > pack->sections[EDEN_SEC_PATCH].count) {
        return false;
    }
    if (out->base >= pack->sections[EDEN_SEC_IMAGE].count) {
        return false;
    }
    return true;
}

bool eden_pack_patch(const eden_pack_t *pack, uint16_t index, eden_patch_t *out)
{
    if (index >= pack->sections[EDEN_SEC_PATCH].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_PATCH].offset +
                            (uint32_t)index * EDEN_PATCH_SIZE;
    out->image = rd16(record);
    out->x = rd16(record + 2);
    out->y = rd16(record + 4);
    return out->image < pack->sections[EDEN_SEC_IMAGE].count;
}

bool eden_pack_frame_checked(const eden_pack_t *pack, uint16_t index,
                             uint16_t frame_w, uint16_t frame_h, uint16_t patch_w_max,
                             eden_frame_t *out)
{
    if (!eden_pack_frame(pack, index, out)) {
        return false;
    }
    eden_pack_image_t base;
    if (!eden_pack_image(pack, out->base, &base)) {
        return false;
    }
    if (base.content_w > frame_w || base.content_h > frame_h ||
        base.width > frame_w || base.height > frame_h) {
        return false;
    }
    for (uint16_t offset = 0; offset < out->patch_count; offset++) {
        eden_patch_t patch;
        if (!eden_pack_patch(pack, (uint16_t)(out->patch_first + offset), &patch)) {
            return false;
        }
        eden_pack_image_t image;
        if (!eden_pack_image(pack, patch.image, &image)) {
            return false;
        }
        if (image.content_w == 0 || image.content_h == 0 ||
            image.width > patch_w_max || image.content_w > patch_w_max) {
            return false;
        }
        // 用 u32 相加,避免 u16 回绕把越界坐标算成合法值。
        if ((uint32_t)patch.x + image.content_w > frame_w ||
            (uint32_t)patch.y + image.content_h > frame_h) {
            return false;
        }
    }
    return true;
}

bool eden_pack_chapter(const eden_pack_t *pack, uint16_t index, eden_chapter_t *out)
{
    if (index >= pack->sections[EDEN_SEC_CHAP].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_CHAP].offset +
                            (uint32_t)index * EDEN_CHAP_SIZE;
    out->scene = rd16(record);
    out->name = rd16(record + 2);
    out->op = rd16(record + 4);
    return eden_pack_scene_range(pack, out->scene, NULL, NULL) &&
           out->op < pack->sections[EDEN_SEC_OPS].count;
}

bool eden_pack_gallery(const eden_pack_t *pack, uint16_t index, eden_gallery_t *out)
{
    if (index >= pack->sections[EDEN_SEC_GAL].count) {
        return false;
    }
    const uint8_t *record = pack->base + pack->sections[EDEN_SEC_GAL].offset +
                            (uint32_t)index * EDEN_GAL_SIZE;
    out->frame = rd16(record);
    out->group = rd16(record + 2);
    out->label = rd16(record + 4);
    return out->frame < pack->sections[EDEN_SEC_FRAME].count;
}
