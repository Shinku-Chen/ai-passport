// main/dracu_scn.c —— 剧本包解析实现(纯字节映射 + 分块解压,不依赖 ESP-IDF)。
#include "dracu_scn.h"

#include "dracu_inflate.h"

#include <string.h>

#define DRACU_HEADER_SIZE 20u
#define DRACU_SECTION_SIZE 16u
// 分支表记录尺寸(与 tools/dracu_scn_pack.py 一致)
#define DRACU_NO_NEXT_REC 8u
#define DRACU_NO_BACK_REC 8u
#define DRACU_END_REC 6u
#define DRACU_COND_REC 4u
#define DRACU_HIDDEN_REC 14u
#define DRACU_RULE_REC 10u
#define DRACU_LITERAL_REC 2u
#define DRACU_CHOICE_HEAD 8u
#define DRACU_CHOICE_SLOT 12u
#define DRACU_CHAPTER_REC 8u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// --------------------------------------------------------------------------
// 解块
// --------------------------------------------------------------------------

static const uint8_t *block_record(const dracu_scn_t *scn, uint32_t index)
{
    if (index >= scn->block_count) {
        return NULL;
    }
    return scn->blocks + (uint32_t)index * 12u;
}

// 解开第 index 块;命中缓存时直接返回缓冲。失败返回 NULL。
static const uint8_t *load_block(dracu_scn_t *scn, uint32_t index, bool is_page)
{
    if (scn->scratch == NULL || scn->scratch_size < 256) {
        return NULL;
    }
    int32_t *cache = is_page ? &scn->cached_page_block : &scn->cached_text_block;
    if (*cache == (int32_t)index) {
        return scn->scratch;
    }
    const uint8_t *record = block_record(scn, index);
    if (record == NULL) {
        return NULL;
    }
    uint32_t off = rd32(record);
    uint32_t comp = rd32(record + 4);
    uint32_t raw = rd32(record + 8);
    if (off > scn->payload_size || comp > scn->payload_size - off || raw > scn->scratch_size) {
        return NULL;
    }
    uint32_t produced = dracu_inflate(scn->payload + off, comp, scn->scratch, scn->scratch_size);
    if (produced != raw) {
        *cache = -1;
        return NULL;
    }
    *cache = (int32_t)index;
    return scn->scratch;
}

// --------------------------------------------------------------------------
// 打开
// --------------------------------------------------------------------------

bool dracu_scn_open(dracu_scn_t *scn, const uint8_t *data, uint32_t size)
{
    if (scn == NULL || data == NULL || size < DRACU_HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, DRACU_SCN_MAGIC, 8) != 0) {
        return false;
    }
    uint16_t version = rd16(data + 8);
    uint16_t header_size = rd16(data + 10);
    uint16_t section_count = rd16(data + 12);
    uint32_t total = rd32(data + 16);
    if (version != DRACU_SCN_VERSION || total != size) {
        return false;
    }
    if (header_size < DRACU_HEADER_SIZE + (uint32_t)section_count * DRACU_SECTION_SIZE ||
        (uint64_t)header_size > size) {
        return false;
    }

    dracu_scn_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;
    view.cached_page_block = -1;
    view.cached_text_block = -1;
    const uint8_t *branch = NULL;
    uint32_t branch_size = 0;
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *section = data + DRACU_HEADER_SIZE + (uint32_t)index * DRACU_SECTION_SIZE;
        uint32_t type = rd32(section);
        uint32_t offset = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t length = rd32(section + 12);
        if (offset > size || length > size - offset) {
            return false;
        }
        switch (type) {
        case DRACU_SCN_SEC_CHAR:
            if (length < 4) {
                return false;
            }
            view.chars = data + offset + 4;
            view.char_count = count;
            if (4u + count * 2u > length) {
                return false;
            }
            break;
        case DRACU_SCN_SEC_STR:
            if (length < 4) {
                return false;
            }
            view.strings = data + offset;
            view.string_count = count;
            break;
        case DRACU_SCN_SEC_BLOCK:
            if (length < 12) {
                return false;
            }
            view.blocks = data + offset + 12;
            view.block_count = rd16(data + offset);
            view.page_block_count = rd16(data + offset + 2);
            view.page_count = rd32(data + offset + 4);
            view.pages_per_block = rd32(data + offset + 8);
            if (view.pages_per_block == 0 ||
                12u + (uint64_t)view.block_count * 12u > length) {
                return false;
            }
            break;
        case DRACU_SCN_SEC_BLOB:
            view.payload = data + offset;
            view.payload_size = length;
            break;
        case DRACU_SCN_SEC_BRANCH:
            branch = data + offset;
            branch_size = length;
            break;
        case DRACU_SCN_SEC_CHOICE:
            if (length < 4) {
                return false;
            }
            view.choices = data + offset + 4;
            view.choice_count = count;
            if (4u + (uint64_t)count * (DRACU_CHOICE_HEAD + DRACU_CHOICE_MAX * DRACU_CHOICE_SLOT) >
                length) {
                return false;
            }
            break;
        case DRACU_SCN_SEC_CHAPTERS:
            if (length < 4) {
                return false;
            }
            view.chapters = data + offset + 4;
            view.chapter_count = count;
            if (4u + (uint64_t)count * DRACU_CHAPTER_REC > length) {
                return false;
            }
            break;
        case DRACU_SCN_SEC_META:
            view.meta = data + offset;
            view.meta_size = length;
            break;
        default:
            break;
        }
    }
    if (view.chars == NULL || view.strings == NULL || view.blocks == NULL || view.payload == NULL) {
        return false;
    }
    // 分支表:一段连排的变长表,按写入顺序顺次切开
    if (branch != NULL) {
        const uint8_t *p = branch;
        const uint8_t *end = branch + branch_size;
        uint32_t count;
#define DRACU_TAKE(len, target_count)                                     \
    do {                                                                  \
        if (p + 4 > end) {                                                \
            return false;                                                 \
        }                                                                 \
        (target_count) = rd32(p);                                         \
        p += 4;                                                           \
        if (p + (uint64_t)(target_count) * (len) > end) {                  \
            return false;                                                 \
        }                                                                 \
    } while (0)
        DRACU_TAKE(DRACU_NO_NEXT_REC, count);
        view.no_next = p;
        view.no_next_count = count;
        p += (uint64_t)count * DRACU_NO_NEXT_REC;
        DRACU_TAKE(DRACU_NO_BACK_REC, count);
        view.no_back = p;
        view.no_back_count = count;
        p += (uint64_t)count * DRACU_NO_BACK_REC;
        DRACU_TAKE(DRACU_END_REC, count);
        view.ends = p;
        view.end_count = count;
        p += (uint64_t)count * DRACU_END_REC;
        DRACU_TAKE(DRACU_COND_REC, count);
        view.cond_pages = p;
        view.cond_count = count;
        p += (uint64_t)count * DRACU_COND_REC;
        DRACU_TAKE(DRACU_HIDDEN_REC, count);
        view.hidden = p;
        view.hidden_count = count;
        p += (uint64_t)count * DRACU_HIDDEN_REC;
        DRACU_TAKE(DRACU_RULE_REC, count);
        view.rules = p;
        view.rule_count = count;
        p += (uint64_t)count * DRACU_RULE_REC;
        DRACU_TAKE(DRACU_LITERAL_REC, count);
        view.literals = p;
        view.literal_count = count;
        p += (uint64_t)count * DRACU_LITERAL_REC;
#undef DRACU_TAKE
    }
    *scn = view;
    return true;
}

void dracu_scn_attach(dracu_scn_t *scn, uint8_t *scratch, uint32_t size)
{
    if (scn == NULL) {
        return;
    }
    scn->scratch = scratch;
    scn->scratch_size = size;
    scn->cached_page_block = -1;
    scn->cached_text_block = -1;
}

// --------------------------------------------------------------------------
// 读取
// --------------------------------------------------------------------------

bool dracu_scn_page_valid(const dracu_scn_t *scn, uint32_t page)
{
    return scn != NULL && page >= 1 && page <= scn->page_count;
}

static uint32_t page_block_index(const dracu_scn_t *scn, uint32_t page)
{
    return scn->pages_per_block ? (page - 1) / scn->pages_per_block : 0;
}

bool dracu_scn_page(dracu_scn_t *scn, uint32_t page, dracu_page_t *out)
{
    if (scn == NULL || out == NULL || !dracu_scn_page_valid(scn, page)) {
        return false;
    }
    uint32_t index = page_block_index(scn, page);
    if (index >= scn->page_block_count) {
        return false;
    }
    const uint8_t *block = load_block(scn, index, true);
    if (block == NULL) {
        return false;
    }
    uint32_t within = ((page - 1) % scn->pages_per_block) * 16u;
    if (scn->pages_per_block * 16u > scn->scratch_size || within + 16u > scn->scratch_size) {
        return false;
    }
    const uint8_t *record = block + within;
    out->text_off = rd32(record);
    out->text_len = rd16(record + 4);
    out->cg = rd16(record + 6);
    out->sprite = rd16(record + 8);
    out->bg = record[10];
    out->sd = record[11];
    out->name = record[12];
    out->flags = record[13];
    out->cs = record[14];
    out->fs = record[15];
    return true;
}

// 字符表项 -> UTF-8(最多 3 字节,全部落在 BMP)
static size_t encode_char(uint16_t code, char *out)
{
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (code >> 12));
    out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code & 0x3F));
    return 3;
}

static uint16_t char_at(const dracu_scn_t *scn, uint16_t index)
{
    if (index >= scn->char_count) {
        return '?';
    }
    return rd16(scn->chars + (uint32_t)index * 2u);
}
// 找到覆盖正文流偏移 off 的块;把块内偏移写进 within。失败返回 NULL。
static const uint8_t *text_block_for(dracu_scn_t *scn, uint32_t off, uint32_t *within)
{
    uint32_t base = 0;
    for (uint32_t index = scn->page_block_count; index < scn->block_count; index++) {
        const uint8_t *record = block_record(scn, index);
        uint32_t raw = rd32(record + 8);
        if (off < base + raw) {
            const uint8_t *block = load_block(scn, index, false);
            if (block == NULL) {
                return NULL;
            }
            *within = off - base;
            return block;
        }
        base += raw;
    }
    return NULL;
}

size_t dracu_scn_text(dracu_scn_t *scn, uint32_t off, uint16_t len, char *out, size_t capacity)
{
    if (scn == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (off == DRACU_SCN_NO_TEXT || len == 0) {
        return 0;
    }
    uint32_t within = 0;
    const uint8_t *block = text_block_for(scn, off, &within);
    if (block == NULL) {
        return 0;
    }
    size_t written = 0;
    for (uint16_t index = 0; index < len; index++) {
        if (within + (uint32_t)index * 2u + 2u > scn->scratch_size) {
            break;
        }
        char encoded[3];
        size_t size = encode_char(char_at(scn, rd16(block + within + (uint32_t)index * 2u)),
                                 encoded);
        if (written + size + 1u > capacity) {
            break;
        }
        memcpy(out + written, encoded, size);
        written += size;
    }
    out[written] = '\0';
    return written;
}

size_t dracu_scn_string(dracu_scn_t *scn, uint16_t id, char *out, size_t capacity)
{
    if (scn == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (id >= scn->string_count) {
        return 0;
    }
    // 字符串表:u32 count,随后每条 { u16 长度, 长度个 u16 字符码 }
    const uint8_t *record = scn->strings + 4;
    const uint8_t *end = scn->strings + scn->blob_size;
    for (uint32_t index = 0; index <= id; index++) {
        if (record + 2 > end) {
            return 0;
        }
        uint16_t length = rd16(record);
        if (record + 2 + (uint32_t)length * 2u > scn->blob + scn->blob_size) {
            return 0;
        }
        if (index == id) {
            size_t written = 0;
            for (uint16_t slot = 0; slot < length; slot++) {
                char encoded[3];
                size_t size = encode_char(
                    char_at(scn, rd16(record + 2 + (uint32_t)slot * 2u)), encoded);
                if (written + size + 1u > capacity) {
                    break;
                }
                memcpy(out + written, encoded, size);
                written += size;
            }
            out[written] = '\0';
            return written;
        }
        record += 2 + (uint32_t)length * 2u;
    }
    return 0;
}

bool dracu_scn_choice(dracu_scn_t *scn, uint32_t page, dracu_choice_t *out)
{
    if (scn == NULL || out == NULL || scn->choices == NULL) {
        return false;
    }
    const uint8_t *record = scn->choices;
    for (uint32_t index = 0; index < scn->choice_count; index++) {
        uint32_t entry_page = rd32(record);
        if (entry_page == page) {
            memset(out, 0, sizeof(*out));
            out->page = page;
            out->count = record[4];
            if (out->count > DRACU_CHOICE_MAX) {
                out->count = DRACU_CHOICE_MAX;
            }
            const uint8_t *slot = record + DRACU_CHOICE_HEAD;
            for (uint8_t choice = 0; choice < DRACU_CHOICE_MAX; choice++) {
                out->text_off[choice] = rd32(slot);
                out->text_len[choice] = rd16(slot + 4);
                out->target[choice] = rd32(slot + 8);
                slot += DRACU_CHOICE_SLOT;
            }
            return true;
        }
        record += DRACU_CHOICE_HEAD + DRACU_CHOICE_MAX * DRACU_CHOICE_SLOT;
    }
    return false;
}

uint32_t dracu_scn_next_page(const dracu_scn_t *scn, uint32_t page)
{
    if (scn == NULL) {
        return 0;
    }
    const uint8_t *record = scn->no_next;
    for (uint32_t index = 0; index < scn->no_next_count; index++, record += DRACU_NO_NEXT_REC) {
        if (rd32(record) == page) {
            return rd32(record + 4);
        }
    }
    return 0;
}

uint32_t dracu_scn_back_page(const dracu_scn_t *scn, uint32_t page)
{
    if (scn == NULL) {
        return 0;
    }
    const uint8_t *record = scn->no_back;
    for (uint32_t index = 0; index < scn->no_back_count; index++, record += DRACU_NO_BACK_REC) {
        if (rd32(record) == page) {
            return rd32(record + 4);
        }
    }
    return 0;
}

bool dracu_scn_end_name(const dracu_scn_t *scn, uint32_t page, uint16_t *name_id)
{
    if (scn == NULL) {
        return false;
    }
    const uint8_t *record = scn->ends;
    for (uint32_t index = 0; index < scn->end_count; index++, record += DRACU_END_REC) {
        if (rd32(record) == page) {
            if (name_id != NULL) {
                *name_id = rd16(record + 4);
            }
            return true;
        }
    }
    return false;
}

int dracu_scn_cond_index(const dracu_scn_t *scn, uint32_t page)
{
    if (scn == NULL) {
        return -1;
    }
    for (uint32_t index = 0; index < scn->cond_count; index++) {
        if (rd32(scn->cond_pages + index * DRACU_COND_REC) == page) {
            return (int)index;
        }
    }
    return -1;
}

uint8_t dracu_scn_cond_options(const dracu_scn_t *scn, uint32_t page)
{
    dracu_choice_t choice;
    dracu_scn_t *mutable_scn = (dracu_scn_t *)scn;
    if (!dracu_scn_choice(mutable_scn, page, &choice)) {
        return 0;
    }
    return choice.count;
}

bool dracu_scn_hidden_page(const dracu_scn_t *scn, uint32_t page, const uint8_t *choice,
                           uint32_t *target)
{
    if (scn == NULL) {
        return false;
    }
    const uint8_t *record = scn->hidden;
    for (uint32_t index = 0; index < scn->hidden_count; index++, record += DRACU_HIDDEN_REC) {
        if (rd32(record) != page) {
            continue;
        }
        uint32_t rule_off = rd32(record + 4);
        uint8_t rule_count = record[8];
        uint32_t fallback = rd32(record + 10);
        for (uint8_t rule = 0; rule < rule_count; rule++) {
            const uint8_t *entry = scn->rules + (rule_off + rule) * DRACU_RULE_REC;
            uint32_t lit_off = rd32(entry);
            uint8_t lit_count = entry[4];
            uint32_t rule_target = rd32(entry + 6);
            bool matched = true;
            for (uint8_t slot = 0; slot < lit_count; slot++) {
                const uint8_t *literal = scn->literals + (lit_off + slot) * DRACU_LITERAL_REC;
                uint8_t cond = literal[0];
                uint8_t value = literal[1];
                if (choice == NULL || choice[cond] != value) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                if (target != NULL) {
                    *target = rule_target;
                }
                return true;
            }
        }
        if (target != NULL) {
            *target = fallback;
        }
        return true;
    }
    return false;
}

uint32_t dracu_scn_chapter_count(const dracu_scn_t *scn)
{
    return scn == NULL ? 0 : scn->chapter_count;
}

bool dracu_scn_chapter(const dracu_scn_t *scn, uint32_t index, dracu_chapter_t *out)
{
    if (scn == NULL || out == NULL || scn->chapters == NULL || index >= scn->chapter_count) {
        return false;
    }
    const uint8_t *record = scn->chapters + index * DRACU_CHAPTER_REC;
    out->page = rd32(record);
    out->name = rd16(record + 4);
    out->route = record[6];
    return true;
}
