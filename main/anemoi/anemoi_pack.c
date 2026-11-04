// main/anemoi/anemoi_pack.c —— 资源包解析实现（不依赖 ESP-IDF，可在宿主机测试）。
#include "anemoi_pack.h"

#include <string.h>
#include <strings.h>

#define ANEM_SECTION_RECORD 16u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// 表驱动 CRC32（IEEE 802.3，与逐位实现结果一致）。
// 逐位版本对一个 7 MB 包要 5 秒以上，会把 IDLE 饿死触发 task_wdt；查表版快约 8 倍。
static uint32_t s_crc_table[256];
static bool s_crc_table_ready;

static void crc32_table_init(void)
{
    if (s_crc_table_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
        s_crc_table[i] = c;
    }
    s_crc_table_ready = true;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    crc32_table_init();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ s_crc_table[(crc ^ data[i]) & 0xFFu];
    }
    return ~crc;
}

bool anem_pack_section(const anem_pack_t *pack, uint8_t type, const uint8_t **data,
                       uint32_t *count, uint32_t *size)
{
    if (pack == NULL || pack->base == NULL || type >= ANEM_SEC_COUNT) {
        return false;
    }
    const anem_section_t *sec = &pack->sec[type];
    if (sec->off > pack->size || sec->size > pack->size - sec->off) {
        return false;
    }
    if (data) {
        *data = pack->base + sec->off;
    }
    if (count) {
        *count = sec->count;
    }
    if (size) {
        *size = sec->size;
    }
    return true;
}

static bool record_count(const anem_pack_t *pack, uint8_t type, uint32_t rec_size,
                         uint32_t *count, const uint8_t **data)
{
    uint32_t n = 0, bytes = 0;
    if (!anem_pack_section(pack, type, data, &n, &bytes)) {
        return false;
    }
    if (bytes != n * rec_size) {
        return false;
    }
    if (count) {
        *count = n;
    }
    return true;
}

// ---------------------------------------------------------------- 引用自检
//
// 打开包时把“所有跨段 id”走一遍：文本 id 必须落在 TEXT 的字符串数内、变量 id
// 落在 VAR 内、表达式/条件/调用表偏移落在 EXPR 内、场景/entry/图像下标不越界。
// 这条检查专门用来拒绝“文本表定稿后才追加字符串”这类悬空 id —— 它曾经让
// 136 个变量名全部取不到（见 tests/test_anemoi_pack.c 的悬空 id 负例）。

static uint32_t expr_span(const anem_pack_t *pack, uint32_t off);
static bool expr_ids_ok(const anem_pack_t *pack, uint32_t off, uint32_t strings,
                        uint32_t var_count, int depth);

static uint32_t expr_span(const anem_pack_t *pack, uint32_t off)
{
    const uint8_t *base = anem_pack_expr_base(pack);
    const uint32_t size = anem_pack_expr_size(pack);
    if (base == NULL || off >= size) {
        return 0;
    }
    const uint8_t *p = base + off;
    switch (p[0]) {
    case 1:
        return 5;
    case 2:
    case 3:
        return 3;
    case 9: {  // E_VARDYN: base id + 索引表达式
        const uint32_t sub = expr_span(pack, off + 3);
        return sub ? 3 + sub : 0;
    }
    case 4: {
        if (off + 3 > size) return 0;
        const uint16_t n = rd16(p + 1);
        uint32_t len = 3;
        for (uint16_t i = 0; i < n; i++) {
            const uint32_t sub = expr_span(pack, off + len);
            if (sub == 0) return 0;
            len += sub;
        }
        return len;
    }
    case 5: {
        const uint32_t sub = expr_span(pack, off + 2);
        return sub ? 2 + sub : 0;
    }
    case 6: {
        const uint32_t a = expr_span(pack, off + 2);
        if (a == 0) return 0;
        const uint32_t b = expr_span(pack, off + 2 + a);
        return b ? 2 + a + b : 0;
    }
    case 7: {
        if (off + 5 > size) return 0;
        const uint8_t has_recv = p[3];
        const uint8_t argc = p[4];
        uint32_t len = 5;
        if (has_recv) {
            const uint32_t sub = expr_span(pack, off + len);
            if (sub == 0) return 0;
            len += sub;
        }
        for (uint8_t i = 0; i < argc; i++) {
            const uint32_t sub = expr_span(pack, off + len);
            if (sub == 0) return 0;
            len += sub;
        }
        return len;
    }
    case 8: {
        if (off + 3 > size) return 0;
        const uint8_t argc = p[2];
        uint32_t len = 3;
        uint32_t sub = expr_span(pack, off + len);
        if (sub == 0) return 0;
        len += sub;
        for (uint8_t i = 0; i < argc; i++) {
            sub = expr_span(pack, off + len);
            if (sub == 0) return 0;
            len += sub;
        }
        return len;
    }
    default:
        return 0;
    }
}

static bool expr_ids_ok(const anem_pack_t *pack, uint32_t off, uint32_t strings,
                        uint32_t var_count, int depth)
{
    if (depth > 14) {
        return false;
    }
    const uint8_t *base = anem_pack_expr_base(pack);
    const uint32_t size = anem_pack_expr_size(pack);
    if (base == NULL || off >= size) {
        return false;
    }
    const uint8_t *p = base + off;
    switch (p[0]) {
    case 1:
        return off + 5 <= size;
    case 2:
        return off + 3 <= size && rd16(p + 1) < strings;
    case 3:
        return off + 3 <= size && rd16(p + 1) < var_count;
    case 9: {
        if (off + 3 > size || rd16(p + 1) >= strings) {
            return false;
        }
        return expr_ids_ok(pack, off + 3, strings, var_count, depth + 1);
    }
    case 4: {
        if (off + 3 > size) return false;
        const uint16_t n = rd16(p + 1);
        uint32_t q = off + 3;
        for (uint16_t i = 0; i < n; i++) {
            const uint32_t len = expr_span(pack, q);
            if (len == 0 || !expr_ids_ok(pack, q, strings, var_count, depth + 1)) return false;
            q += len;
        }
        return q <= size;
    }
    case 5:
        return off + 2 <= size && expr_ids_ok(pack, off + 2, strings, var_count, depth + 1);
    case 6: {
        const uint32_t a = expr_span(pack, off + 2);
        if (a == 0) return false;
        return expr_ids_ok(pack, off + 2, strings, var_count, depth + 1) &&
               expr_ids_ok(pack, off + 2 + a, strings, var_count, depth + 1);
    }
    case 7: {
        if (off + 5 > size || rd16(p + 1) >= strings) return false;
        const uint8_t has_recv = p[3];
        const uint8_t argc = p[4];
        uint32_t q = off + 5;
        if (has_recv) {
            const uint32_t len = expr_span(pack, q);
            if (len == 0 || !expr_ids_ok(pack, q, strings, var_count, depth + 1)) return false;
            q += len;
        }
        for (uint8_t i = 0; i < argc; i++) {
            const uint32_t len = expr_span(pack, q);
            if (len == 0 || !expr_ids_ok(pack, q, strings, var_count, depth + 1)) return false;
            q += len;
        }
        return true;
    }
    case 8: {
        if (off + 3 > size) return false;
        const uint8_t argc = p[2];
        uint32_t q = off + 3;
        uint32_t len = expr_span(pack, q);
        if (len == 0 || !expr_ids_ok(pack, q, strings, var_count, depth + 1)) return false;
        q += len;
        for (uint8_t i = 0; i < argc; i++) {
            len = expr_span(pack, q);
            if (len == 0 || !expr_ids_ok(pack, q, strings, var_count, depth + 1)) return false;
            q += len;
        }
        return true;
    }
    default:
        return false;
    }
}

static bool pack_ids_ok(const anem_pack_t *pack)
{
    uint32_t tx_count = 0;
    if (!anem_pack_section(pack, ANEM_SEC_TXIDX, NULL, &tx_count, NULL) || tx_count < 2) {
        return false;
    }
    const uint32_t strings = tx_count - 1;
    const uint32_t var_count = pack->sec[ANEM_SEC_VAR].count;
    const uint32_t image_count = pack->sec[ANEM_SEC_IMAGE].count;
    const uint32_t op_count = pack->sec[ANEM_SEC_OPS].count;

    for (uint32_t i = 0; i < var_count; i++) {
        const uint8_t *row = pack->base + pack->sec[ANEM_SEC_VAR].off + i * 2;
        if (rd16(row) >= strings) {
            return false;
        }
    }
    const struct {
        uint8_t sec;
        uint32_t rec;
        uint8_t offs[3];
        uint8_t count;
    } name_tables[] = {
        { ANEM_SEC_POSE, 24, { 0, 0, 0 }, 1 },
        { ANEM_SEC_ACTOR, 8, { 0, 0, 0 }, 1 },
        { ANEM_SEC_CHAP, 12, { 2, 4, 6 }, 3 },
        { ANEM_SEC_REQ, 8, { 0, 2, 4 }, 3 },
        { ANEM_SEC_GAL, 8, { 2, 4, 0 }, 2 },
        { ANEM_SEC_HELPER, 12, { 0, 2, 0 }, 2 },
        { ANEM_SEC_NAMEMAP, 4, { 0, 0, 0 }, 1 },
    };
    for (size_t t = 0; t < sizeof(name_tables) / sizeof(name_tables[0]); t++) {
        const uint32_t rec = name_tables[t].rec;
        const uint32_t n = pack->sec[name_tables[t].sec].size / rec;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *row = pack->base + pack->sec[name_tables[t].sec].off + i * rec;
            for (uint8_t k = 0; k < name_tables[t].count; k++) {
                if (rd16(row + name_tables[t].offs[k]) >= strings) {
                    return false;
                }
            }
        }
    }
    const uint32_t cond_count = pack->sec[ANEM_SEC_COND].count;
    const uint32_t call_count = pack->sec[ANEM_SEC_CALL].count;
    const uint32_t set_count = pack->sec[ANEM_SEC_SET].count;
    const uint32_t choice_count = pack->sec[ANEM_SEC_CHOICE].count;
    const uint32_t namelist_count = pack->sec[ANEM_SEC_NAMELIST].count;
    const uint32_t entry_count = pack->sec[ANEM_SEC_ENTRY].count;
    for (uint32_t i = 0; i < op_count; i++) {
        anem_op_t op;
        if (!anem_pack_op(pack, i, &op)) {
            return false;
        }
        switch (op.kind) {
        case ANEM_OP_TEXT:
            if (op.a != ANEM_NONE16 && op.a >= strings) return false;
            if (op.b >= strings) return false;
            break;
        case ANEM_OP_BG:
            if (op.a >= strings) return false;
            break;
        case ANEM_OP_PORTRAIT:
            if ((uint32_t)op.a + op.b > namelist_count) return false;
            break;
        case ANEM_OP_IF_FALSE:
        case ANEM_OP_IF_TRUE:
            if (op.a >= cond_count || op.b >= op_count) return false;
            break;
        case ANEM_OP_GOTO:
        case ANEM_OP_CALL:
            if (op.b >= op_count) return false;
            break;
        case ANEM_OP_CHOICE:
            if (op.a >= choice_count) return false;
            break;
        case ANEM_OP_SET:
            if (op.a >= set_count) return false;
            break;
        case ANEM_OP_NATIVE:
            if (op.a >= call_count) return false;
            break;
        case ANEM_OP_SET_DYN:
            if (op.a >= strings || op.b >= anem_pack_expr_size(pack) ||
                op.c >= anem_pack_expr_size(pack)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    for (uint32_t i = 0; i < set_count; i++) {
        anem_set_t row;
        if (!anem_pack_set(pack, (uint16_t)i, &row) || row.var >= var_count ||
            row.expr >= anem_pack_expr_size(pack)) {
            return false;
        }
        if (!expr_ids_ok(pack, row.expr, strings, var_count, 0)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < choice_count; i++) {
        anem_choice_t row;
        if (!anem_pack_choice(pack, (uint16_t)i, &row)) {
            return false;
        }
        const uint32_t offs[3] = { row.labels, row.enabled, row.target };
        for (int k = 0; k < 3; k++) {
            if (offs[k] >= anem_pack_expr_size(pack) ||
                !expr_ids_ok(pack, offs[k], strings, var_count, 0)) {
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < cond_count; i++) {
        uint32_t off = 0;
        if (!anem_pack_cond(pack, (uint16_t)i, &off) ||
            !expr_ids_ok(pack, off, strings, var_count, 0)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < call_count; i++) {
        uint32_t off = 0;
        if (!anem_pack_call(pack, (uint16_t)i, &off) ||
            !expr_ids_ok(pack, off, strings, var_count, 0)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < namelist_count; i++) {
        uint32_t off = 0;
        if (!anem_pack_namelist(pack, (uint16_t)i, &off) ||
            !expr_ids_ok(pack, off, strings, var_count, 0)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->sec[ANEM_SEC_POSE].count; i++) {
        const uint8_t *row = pack->base + pack->sec[ANEM_SEC_POSE].off + i * 24;
        if (rd16(row + 4) >= image_count || rd16(row + 6) >= image_count) {
            return false;
        }
    }
    for (uint32_t i = 0; i < entry_count; i++) {
        anem_entry_t row;
        const uint8_t *raw = pack->base + pack->sec[ANEM_SEC_ENTRY].off + i * 8;
        memcpy(&row, raw, sizeof(row));
        if (row.op != ANEM_NONE32 && row.op >= op_count) {
            return false;
        }
    }
    return true;
}

bool anem_pack_open(anem_pack_t *pack, const void *data, size_t size)
{
    if (pack == NULL || data == NULL) {
        return false;
    }
    if (size < ANEM_HEADER_SIZE) {
        return false;
    }
    const uint8_t *base = (const uint8_t *)data;
    if (memcmp(base, ANEM_PACK_MAGIC, 7) != 0) {
        return false;
    }
    const uint16_t version = rd16(base + 8);
    const uint16_t header_size = rd16(base + 10);
    const uint16_t section_count = rd16(base + 12);
    const uint32_t total = rd32(base + 16);
    if (version != ANEM_PACK_VERSION || header_size != ANEM_HEADER_SIZE) {
        return false;
    }
    if (section_count != ANEM_SEC_COUNT || total != size) {
        return false;
    }
    if (ANEM_HEADER_SIZE + (uint32_t)section_count * ANEM_SECTION_RECORD > size) {
        return false;
    }
    anem_pack_t view;
    memset(&view, 0, sizeof(view));
    view.base = base;
    view.size = total;
    view.crc32 = rd32(base + 20);
    for (uint16_t i = 0; i < section_count; i++) {
        const uint8_t *rec = base + ANEM_HEADER_SIZE + (uint32_t)i * ANEM_SECTION_RECORD;
        const uint32_t type = rd32(rec);
        if (type >= ANEM_SEC_COUNT) {
            return false;
        }
        const uint32_t off = rd32(rec + 4);
        const uint32_t count = rd32(rec + 8);
        const uint32_t bytes = rd32(rec + 12);
        if (off > total || bytes > total - off) {
            return false;
        }
        view.sec[type].off = off;
        view.sec[type].count = count;
        view.sec[type].size = bytes;
    }
    // 段记录必须按类型升序且齐全（生成器保证；这里用来拒绝畸形包）。
    for (uint16_t i = 0; i < section_count; i++) {
        const uint8_t *rec = base + ANEM_HEADER_SIZE + (uint32_t)i * ANEM_SECTION_RECORD;
        if (rd32(rec) != i) {
            return false;
        }
    }
    const uint32_t crc = crc32_ieee(base + ANEM_HEADER_SIZE, total - ANEM_HEADER_SIZE);
    if (crc != view.crc32) {
        return false;
    }
    // 每段的固定记录长度校验。
    static const uint8_t rec_size[ANEM_SEC_COUNT] = {
        [ANEM_SEC_OPS] = 12, [ANEM_SEC_SCENE] = 4, [ANEM_SEC_SCNID] = 2,
        [ANEM_SEC_ENTRY] = 8, [ANEM_SEC_CHAP] = 12, [ANEM_SEC_VAR] = 2,
        [ANEM_SEC_COND] = 4, [ANEM_SEC_CALL] = 4, [ANEM_SEC_SET] = 8,
        [ANEM_SEC_CHOICE] = 16, [ANEM_SEC_NAMELIST] = 4, [ANEM_SEC_POSE] = 24,
        [ANEM_SEC_ACTOR] = 8, [ANEM_SEC_IMAGE] = 16, [ANEM_SEC_REQ] = 8,
        [ANEM_SEC_HELPER] = 12, [ANEM_SEC_MICRO] = 12, [ANEM_SEC_GAL] = 8,
        [ANEM_SEC_NAMEMAP] = 4, [ANEM_SEC_TXIDX] = 4,
    };
    for (uint8_t type = 0; type < ANEM_SEC_COUNT; type++) {
        const uint32_t rec = rec_size[type];
        if (rec == 0) {
            continue;
        }
        if (view.sec[type].size != view.sec[type].count * rec) {
            return false;
        }
    }
    if (view.sec[ANEM_SEC_SCENE].count == 0) {
        return false;
    }
    if (view.sec[ANEM_SEC_SCENE].count != view.sec[ANEM_SEC_SCNID].count + 1) {
        return false;
    }
    *pack = view;
    if (!pack_ids_ok(pack)) {
        return false;
    }
    return true;
}

uint32_t anem_pack_text_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_TXIDX].count : 0;
}

uint32_t anem_pack_op_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_OPS].count : 0;
}

uint32_t anem_pack_scene_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_SCNID].count : 0;
}

uint32_t anem_pack_image_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_IMAGE].count : 0;
}

uint32_t anem_pack_pose_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_POSE].count : 0;
}

uint32_t anem_pack_chapter_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_CHAP].count : 0;
}

uint32_t anem_pack_gallery_count(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_GAL].count : 0;
}

const char *anem_pack_text(const anem_pack_t *pack, uint16_t id)
{
    const uint8_t *idx = NULL;
    const uint8_t *pool = NULL;
    uint32_t idx_count = 0, idx_size = 0, pool_size = 0;
    if (!anem_pack_section(pack, ANEM_SEC_TXIDX, &idx, &idx_count, &idx_size) ||
        !anem_pack_section(pack, ANEM_SEC_TEXT, &pool, NULL, &pool_size)) {
        return NULL;
    }
    if (id == ANEM_NONE16 || (uint32_t)id + 1 >= idx_count) {
        return NULL;
    }
    const uint32_t start = rd32(idx + (uint32_t)id * 4);
    const uint32_t end = rd32(idx + (uint32_t)id * 4 + 4);
    if (start >= pool_size || end > pool_size || end <= start) {
        return NULL;
    }
    if (pool[end - 1] != 0) {
        return NULL;
    }
    return (const char *)(pool + start);
}

bool anem_pack_op(const anem_pack_t *pack, uint32_t index, anem_op_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_OPS, sizeof(anem_op_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(anem_op_t), sizeof(*out));
    return true;
}

bool anem_pack_scene_range(const anem_pack_t *pack, uint16_t scene, uint32_t *first,
                           uint32_t *count)
{
    const uint8_t *slots = NULL;
    const uint8_t *ids = NULL;
    uint32_t slot_count = 0, slot_size = 0, id_size = 0;
    if (!anem_pack_section(pack, ANEM_SEC_SCENE, &slots, &slot_count, &slot_size) ||
        !anem_pack_section(pack, ANEM_SEC_SCNID, &ids, NULL, &id_size)) {
        return false;
    }
    if (slot_count != id_size / 2 + 1) {
        return false;
    }
    for (uint32_t i = 0; i + 1 < slot_count; i++) {
        if (rd16(ids + i * 2) != scene) {
            continue;
        }
        const uint32_t start = rd32(slots + i * 4);
        const uint32_t end = rd32(slots + (i + 1) * 4);
        if (end < start || end > pack->sec[ANEM_SEC_OPS].count) {
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
    return false;
}

uint32_t anem_pack_entry_op(const anem_pack_t *pack, uint16_t scene, uint16_t entry)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_ENTRY, sizeof(anem_entry_t), &count, &data)) {
        return ANEM_NONE32;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_entry_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        if (rec.scene == scene && rec.entry == entry) {
            return rec.op;
        }
    }
    return ANEM_NONE32;
}

bool anem_pack_chapter(const anem_pack_t *pack, uint32_t index, anem_chap_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_CHAP, sizeof(anem_chap_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_chapter_for(const anem_pack_t *pack, uint16_t scene, anem_chap_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_CHAP, sizeof(anem_chap_t), &count, &data)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_chap_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        if (rec.scene == scene) {
            *out = rec;
            return true;
        }
    }
    return false;
}

uint16_t anem_pack_scene_by_source(const anem_pack_t *pack, const char *source)
{
    if (source == NULL) {
        return ANEM_NONE16;
    }
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_CHAP, sizeof(anem_chap_t), &count, &data)) {
        return ANEM_NONE16;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_chap_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        const char *name = anem_pack_text(pack, rec.source);
        if (name != NULL && strcasecmp(name, source) == 0) {
            return rec.scene;
        }
    }
    return ANEM_NONE16;
}

bool anem_pack_image(const anem_pack_t *pack, uint16_t id, anem_image_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_IMAGE, sizeof(anem_image_t), &count, &data)) {
        return false;
    }
    if (id == ANEM_NONE16 || id >= count) {
        return false;
    }
    memcpy(out, data + (size_t)id * sizeof(*out), sizeof(*out));
    if (out->off > pack->sec[ANEM_SEC_BLOB].size ||
        out->len > pack->sec[ANEM_SEC_BLOB].size - out->off) {
        return false;
    }
    return true;
}

const uint8_t *anem_pack_blob(const anem_pack_t *pack, uint32_t off, uint32_t len)
{
    if (pack == NULL || off > pack->sec[ANEM_SEC_BLOB].size ||
        len > pack->sec[ANEM_SEC_BLOB].size - off) {
        return NULL;
    }
    return pack->base + pack->sec[ANEM_SEC_BLOB].off + off;
}

bool anem_pack_exists_image(const anem_pack_t *pack, const char *name)
{
    if (pack == NULL || name == NULL) {
        return false;
    }
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_NAMEMAP, sizeof(anem_namemap_t), &count, &data)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_namemap_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        const char *text = anem_pack_text(pack, rec.name);
        if (text != NULL && strcasecmp(text, name) == 0) {
            return (rec.image & 0x8000u) == 0;  // 别名（纯 base 名）按“不存在”回答
        }
    }
    return false;
}

bool anem_pack_image_entry(const anem_pack_t *pack, const char *name, uint16_t *text_id,
                           uint16_t *image_id)
{
    if (pack == NULL || name == NULL) {
        return false;
    }
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_NAMEMAP, sizeof(anem_namemap_t), &count, &data)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_namemap_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        const char *text = anem_pack_text(pack, rec.name);
        if (text != NULL && strcasecmp(text, name) == 0) {
            if (text_id) {
                *text_id = rec.name;
            }
            if (image_id) {
                *image_id = (uint16_t)(rec.image & 0x7FFFu);
            }
            return true;
        }
    }
    return false;
}

uint16_t anem_pack_image_by_name(const anem_pack_t *pack, const char *name)
{
    if (name == NULL) {
        return ANEM_NONE16;
    }
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_NAMEMAP, sizeof(anem_namemap_t), &count, &data)) {
        return ANEM_NONE16;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_namemap_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        const char *text = anem_pack_text(pack, rec.name);
        if (text != NULL && strcasecmp(text, name) == 0) {
            return (uint16_t)(rec.image & 0x7FFFu);
        }
    }
    return ANEM_NONE16;
}

bool anem_pack_actor(const anem_pack_t *pack, uint16_t index, anem_actor_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_ACTOR, sizeof(anem_actor_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

uint16_t anem_pack_actor_default_dress(const anem_pack_t *pack, const char *actor)
{
    if (actor == NULL || actor[0] == 0) {
        return 1;
    }
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_ACTOR, sizeof(anem_actor_t), &count, &data)) {
        return 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        anem_actor_t rec;
        memcpy(&rec, data + (size_t)i * sizeof(rec), sizeof(rec));
        const char *name = anem_pack_text(pack, rec.name);
        if (name != NULL && strcasecmp(name, actor) == 0) {
            return rec.default_dress ? rec.default_dress : 1;
        }
    }
    return 1;
}

static const anem_pose_t *pose_table(const anem_pack_t *pack, uint32_t *count)
{
    const uint8_t *data = NULL;
    if (!record_count(pack, ANEM_SEC_POSE, sizeof(anem_pose_t), count, &data)) {
        return NULL;
    }
    return (const anem_pose_t *)data;
}

// 包内 POSE 按 (名字 UTF-8 字节序, dress 升序) 排列，二分查找。
static bool pose_row(const anem_pack_t *pack, const char *name, int dress, anem_pose_t *out)
{
    uint32_t count = 0;
    const anem_pose_t *rows = pose_table(pack, &count);
    if (rows == NULL || name == NULL || name[0] == 0) {
        return false;
    }
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const char *row_name = anem_pack_text(pack, rows[mid].name);
        if (row_name == NULL) {
            return false;
        }
        const int cmp = strcmp(row_name, name);
        if (cmp < 0 || (cmp == 0 && rows[mid].dress < (uint16_t)dress)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < count) {
        const char *row_name = anem_pack_text(pack, rows[lo].name);
        if (row_name != NULL && strcmp(row_name, name) == 0 &&
            rows[lo].dress == (uint16_t)dress) {
            *out = rows[lo];
            return true;
        }
    }
    return false;
}

static bool prefix_default_variant(const char *name, char *out, size_t cap)
{
    const char *underscore = strrchr(name, '_');
    if (underscore == NULL || underscore == name || underscore[1] == 0) {
        return false;
    }
    const size_t prefix_len = (size_t)(underscore - name);
    if (prefix_len + 4 >= cap) {
        return false;
    }
    memcpy(out, name, prefix_len);
    out[prefix_len] = '_';
    out[prefix_len + 1] = '0';
    out[prefix_len + 2] = '1';
    out[prefix_len + 3] = 0;
    return true;
}

// 源 poseKey()：n@d -> <prefix>_01@d -> n@<演员默认 dress>（最后一档由 default_dress 传入）。
bool anem_pack_pose(const anem_pack_t *pack, const char *name, int dress, int default_dress,
                    anem_pose_view_t *out)
{
    if (pack == NULL || name == NULL || out == NULL || name[0] == 0) {
        return false;
    }
    anem_pose_t rec;
    char alt[64];
    const bool have_alt = prefix_default_variant(name, alt, sizeof(alt));
    if (!pose_row(pack, name, dress, &rec)) {
        if (!(have_alt && pose_row(pack, alt, dress, &rec))) {
            if (!pose_row(pack, name, default_dress, &rec)) {
                if (!(have_alt && pose_row(pack, alt, default_dress, &rec))) {
                    return false;
                }
            }
        }
    }
    out->body = rec.body;
    out->face = rec.face;
    out->fx = rec.fx;
    out->fy = rec.fy;
    out->fw = rec.fw;
    out->fh = rec.fh;
    out->base_w = rec.base_w;
    out->base_h = rec.base_h;
    out->lay0 = rec.lay0;
    out->lay1 = rec.lay1;
    return true;
}

bool anem_pack_var(const anem_pack_t *pack, uint16_t index, uint16_t *text_id)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, ANEM_SEC_VAR, 2, &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    if (text_id) {
        *text_id = rd16(data + (size_t)index * 2);
    }
    return true;
}

bool anem_pack_set(const anem_pack_t *pack, uint16_t index, anem_set_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_SET, sizeof(anem_set_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_choice(const anem_pack_t *pack, uint16_t index, anem_choice_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL ||
        !record_count(pack, ANEM_SEC_CHOICE, sizeof(anem_choice_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_helper(const anem_pack_t *pack, uint32_t index, anem_helper_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL ||
        !record_count(pack, ANEM_SEC_HELPER, sizeof(anem_helper_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_micro(const anem_pack_t *pack, uint16_t index, anem_micro_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_MICRO, sizeof(anem_micro_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_gallery(const anem_pack_t *pack, uint16_t index, anem_gal_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_GAL, sizeof(anem_gal_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

bool anem_pack_request(const anem_pack_t *pack, uint32_t index, anem_req_t *out)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (out == NULL || !record_count(pack, ANEM_SEC_REQ, sizeof(anem_req_t), &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    memcpy(out, data + (size_t)index * sizeof(*out), sizeof(*out));
    return true;
}

const uint8_t *anem_pack_expr_base(const anem_pack_t *pack)
{
    return pack ? pack->base + pack->sec[ANEM_SEC_EXPR].off : NULL;
}

uint32_t anem_pack_expr_size(const anem_pack_t *pack)
{
    return pack ? pack->sec[ANEM_SEC_EXPR].size : 0;
}

bool anem_pack_expr_at(const anem_pack_t *pack, uint32_t off, const uint8_t **ptr)
{
    if (pack == NULL || off >= pack->sec[ANEM_SEC_EXPR].size) {
        return false;
    }
    if (ptr) {
        *ptr = pack->base + pack->sec[ANEM_SEC_EXPR].off + off;
    }
    return true;
}

static bool table_expr(const anem_pack_t *pack, uint8_t type, uint16_t index, uint32_t *expr_off)
{
    const uint8_t *data = NULL;
    uint32_t count = 0;
    if (!record_count(pack, type, 4, &count, &data)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    if (expr_off) {
        *expr_off = rd32(data + (size_t)index * 4);
    }
    return true;
}

bool anem_pack_cond(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off)
{
    return table_expr(pack, ANEM_SEC_COND, index, expr_off);
}

bool anem_pack_call(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off)
{
    return table_expr(pack, ANEM_SEC_CALL, index, expr_off);
}

bool anem_pack_namelist(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off)
{
    return table_expr(pack, ANEM_SEC_NAMELIST, index, expr_off);
}

bool anem_pack_meta(const anem_pack_t *pack, const char *key, const char **value,
                    uint32_t *out_len)
{
    if (pack == NULL || key == NULL) {
        return false;
    }
    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (!anem_pack_section(pack, ANEM_SEC_META, &data, NULL, &size) || data == NULL) {
        return false;
    }
    const size_t key_len = strlen(key);
    uint32_t pos = 0;
    while (pos < size) {
        uint32_t end = pos;
        while (end < size && data[end] != '\n') {
            end++;
        }
        if (end > pos + key_len && data[pos + key_len] == '=' &&
            memcmp(data + pos, key, key_len) == 0) {
            if (value) {
                *value = (const char *)(data + pos + key_len + 1);
            }
            if (out_len) {
                *out_len = end - (pos + key_len + 1);
            }
            return true;
        }
        pos = end + 1;
    }
    return false;
}
