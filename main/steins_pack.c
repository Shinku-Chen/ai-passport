// main/steins_pack.c —— 资源包只读访问层的实现(纯 C,不依赖 ESP-IDF)。
#include "steins_pack.h"

#include <string.h>

#define STEINS_REC_STRT 6u
#define STEINS_REC_NODE 15u
#define STEINS_REC_CHUNK 8u
#define STEINS_REC_FRAME 10u
#define STEINS_REC_VIEW 4u
#define STEINS_REC_LAYER 12u
#define STEINS_REC_ASSET 40u
#define STEINS_REC_CHOICE 12u
#define STEINS_REC_CITEM 9u
#define STEINS_REC_CHAPTER 8u
#define STEINS_REC_GAL 8u
#define STEINS_REC_GENTRY 4u
#define STEINS_REC_WLINE 10u
#define STEINS_REC_WCHANGE 8u
#define STEINS_REC_UI 4u
#define STEINS_REC_BREAK 4u

// 固件用 EMBED_FILES 的链接器符号;宿主机测试用 steins_pack_host_set() 注册镜像。
#if defined(ESP_PLATFORM)
extern const uint8_t _binary_steins_pack_bin_start[] asm("_binary_steins_pack_bin_start");
extern const uint8_t _binary_steins_pack_bin_end[] asm("_binary_steins_pack_bin_end");
#else
static const uint8_t *s_host_blob;
static uint32_t s_host_size;

void steins_pack_host_set(const uint8_t *data, uint32_t size)
{
    s_host_blob = data;
    s_host_size = size;
}
#endif

bool steins_pack_open(steins_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (!pack || !data || size < 32u) return false;
    memset(pack, 0, sizeof(*pack));
    if (memcmp(data, STEINS_PACK_MAGIC, 8) != 0) return false;
    const uint32_t version = steins_rd32(data + 8);
    const uint32_t total = steins_rd32(data + 12);
    const uint32_t sections = steins_rd32(data + 16);
    // v3 是主剧情包,v4 追加手机段;两者都能被同一 reader 打开,
    // 旧的 v3 包在新代码上照常工作(手机 getter 返回 0/false)。
    if (version != 3u && version != 4u && version != 5u) return false;
    if (total != size) return false;
    if (sections == 0u || sections > STEINS_SEC_COUNT) return false;
    if (sections > (size - 32u) / 16u) return false;
    const uint32_t directory_end = 32u + sections * 16u;
    bool seen[STEINS_SEC_COUNT];
    memset(seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t *rec = data + 32u + i * 16u;
        const uint32_t type = steins_rd32(rec);
        const uint32_t off = steins_rd32(rec + 4);
        const uint32_t count = steins_rd32(rec + 8);
        const uint32_t len = steins_rd32(rec + 12);
        if (type == 0u || type >= STEINS_SEC_COUNT) return false;
        if (seen[type]) return false;                  // 重复段类型:拒绝
        seen[type] = true;
        // 段必须整体落在段表之后;用减法判范围,避免 off+len 回绕。
        if (off < directory_end || off > size) return false;
        if (len > size - off) return false;
        pack->sec_off[type] = off;
        pack->sec_count[type] = count;
        pack->sec_size[type] = len;
    }
    // 非空顶层段两两不得重叠(零长段允许共点;BLOB 内的载荷共享是段内的事,不受影响)。
    for (uint32_t a = 0; a < sections; ++a) {
        const uint32_t ta = steins_rd32(data + 32u + a * 16u);
        const uint32_t oa = steins_rd32(data + 32u + a * 16u + 4);
        const uint32_t la = steins_rd32(data + 32u + a * 16u + 12);
        if (la == 0u) continue;
        for (uint32_t b = a + 1u; b < sections; ++b) {
            const uint32_t ob = steins_rd32(data + 32u + b * 16u + 4);
            const uint32_t lb = steins_rd32(data + 32u + b * 16u + 12);
            if (lb == 0u) continue;
            if (oa < ob + lb && ob < oa + la) return false;
        }
        (void)ta;
    }
    pack->data = data;
    pack->size = size;
    pack->sec_version = (uint8_t)(steins_rd32(data + 8u) & 0xFFu);
    // v5:压缩 TXT —— 块表必须自洽(块数 = ceil(raw/2048),偏移连续,长度非零)。
    if (pack->sec_version >= 5u) {
        const uint32_t blk = STEINS_SEC_TXTBLK, raw = STEINS_SEC_TXTRAW;
        if (pack->sec_off[blk] == 0u || pack->sec_off[raw] == 0u ||
            pack->sec_count[blk] != pack->sec_size[blk] / 8u || pack->sec_size[raw] < 4u) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
        const uint32_t raw_len = steins_rd32(data + pack->sec_off[raw]);
        if (raw_len == 0u || (raw_len + 2047u) / 2048u != pack->sec_count[blk]) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
        uint32_t cur = 0;
        for (uint32_t i = 0; i < pack->sec_count[blk]; ++i) {
            const uint8_t *b = data + pack->sec_off[blk] + i * 8u;
            const uint32_t coff = steins_rd32(b);
            const uint16_t clen = steins_rd16(b + 4);
            if (coff != cur || clen == 0u || clen > pack->sec_size[STEINS_SEC_TXT] - coff) {
                memset(pack, 0, sizeof(*pack));
                return false;
            }
            cur = coff + clen;
        }
        if (cur != pack->sec_size[STEINS_SEC_TXT]) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
        pack->txt_raw_len = raw_len;
    } else {
        pack->txt_raw_len = pack->sec_size[STEINS_SEC_TXT];
    }
    // 必需段:没有它们连一帧都读不出来。
    if (pack->sec_off[STEINS_SEC_STRT] == 0u || pack->sec_off[STEINS_SEC_TXT] == 0u ||
        pack->sec_off[STEINS_SEC_NODE] == 0u || pack->sec_off[STEINS_SEC_FRAME] == 0u ||
        pack->sec_off[STEINS_SEC_ASSET] == 0u || pack->sec_off[STEINS_SEC_META] == 0u) {
        memset(pack, 0, sizeof(*pack));
        return false;
    }
    // 每条定长表的 size 必须正好等于 count * 记录长度,防止截断包被当成好包。
    const struct {
        uint32_t sec, rec;
    } fixed[] = {
        {STEINS_SEC_STRT, STEINS_REC_STRT},   {STEINS_SEC_NODE, STEINS_REC_NODE},
        {STEINS_SEC_CHUNK, STEINS_REC_CHUNK}, {STEINS_SEC_FRAME, STEINS_REC_FRAME},
        {STEINS_SEC_VIEW, STEINS_REC_VIEW},   {STEINS_SEC_LAYER, STEINS_REC_LAYER},
        {STEINS_SEC_ASSET, STEINS_REC_ASSET}, {STEINS_SEC_CHOICE, STEINS_REC_CHOICE},
        {STEINS_SEC_CITEM, STEINS_REC_CITEM}, {STEINS_SEC_CHAPTER, STEINS_REC_CHAPTER},
        {STEINS_SEC_GAL, STEINS_REC_GAL},     {STEINS_SEC_GENTRY, STEINS_REC_GENTRY},
        {STEINS_SEC_WLINE, STEINS_REC_WLINE}, {STEINS_SEC_WCHANGE, STEINS_REC_WCHANGE},
        {STEINS_SEC_UI, STEINS_REC_UI},       {STEINS_SEC_BREAK, STEINS_REC_BREAK},
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        const uint32_t sec = fixed[i].sec;
        if (pack->sec_off[sec] == 0u) continue;
        if ((uint64_t)pack->sec_count[sec] * fixed[i].rec != pack->sec_size[sec]) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
    }
    // CHUNKREF 是 u16 数组:用除法先卡上限,再用余数验整,避免 count*2 回绕。
    if (pack->sec_off[STEINS_SEC_CHUNKREF] != 0u) {
        const uint32_t bytes = pack->sec_size[STEINS_SEC_CHUNKREF];
        if (pack->sec_count[STEINS_SEC_CHUNKREF] > bytes / 2u) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
        if (bytes - pack->sec_count[STEINS_SEC_CHUNKREF] * 2u != 0u) {
            memset(pack, 0, sizeof(*pack));
            return false;
        }
    }
    if (pack->sec_count[STEINS_SEC_BREAK] != pack->sec_size[STEINS_SEC_BREAK] / 4u ||
        pack->sec_size[STEINS_SEC_BREAK] % 4u != 0u) {
        memset(pack, 0, sizeof(*pack));
        return false;
    }
    return true;
}

bool steins_pack_open_embedded(steins_pack_t *pack)
{
#if defined(ESP_PLATFORM)
    const uint8_t *start = _binary_steins_pack_bin_start;
    const uint8_t *end = _binary_steins_pack_bin_end;
    if (!start || !end || end <= start) return false;
    return steins_pack_open(pack, start, (uint32_t)(end - start));
#else
    if (!pack || !s_host_blob || s_host_size == 0u) return false;
    return steins_pack_open(pack, s_host_blob, s_host_size);
#endif
}

uint32_t steins_pack_total(const steins_pack_t *pack)
{
    return pack ? pack->size : 0u;
}

uint32_t steins_pack_count(const steins_pack_t *pack, uint32_t section)
{
    if (!pack || section >= STEINS_SEC_COUNT) return 0u;
    return pack->sec_count[section];
}

static const uint8_t *rec(const steins_pack_t *pack, uint32_t section, uint32_t index,
                          uint32_t rec_size)
{
    if (!pack || section >= STEINS_SEC_COUNT) return NULL;
    if (index >= pack->sec_count[section]) return NULL;
    if (pack->sec_off[section] == 0u) return NULL;
    return pack->data + pack->sec_off[section] + index * rec_size;
}

bool steins_pack_node(const steins_pack_t *pack, uint16_t index, steins_node_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_NODE, index, STEINS_REC_NODE);
    if (!p || !out) return false;
    out->id_str = steins_rd16(p);
    out->count = steins_rd32(p + 2);
    out->chunk_first = steins_rd16(p + 6);
    out->chunk_count = p[8];
    out->break_first = p[9];
    out->break_count = p[10];
    out->choice = steins_rd16(p + 11);
    out->ending = steins_rd16(p + 13);
    return true;
}

bool steins_pack_chunk(const steins_pack_t *pack, uint16_t index, steins_chunk_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_CHUNK, index, STEINS_REC_CHUNK);
    if (!p || !out) return false;
    out->first_frame = steins_rd32(p);
    out->count = steins_rd16(p + 4);
    return true;
}

bool steins_pack_node_chunk(const steins_pack_t *pack, const steins_node_t *node,
                            uint16_t n, uint16_t *chunk_out)
{
    if (!pack || !node || !chunk_out || n >= node->chunk_count) return false;
    const uint8_t *p = rec(pack, STEINS_SEC_CHUNKREF, (uint32_t)node->chunk_first + n, 2u);
    if (!p) return false;
    *chunk_out = steins_rd16(p);
    return true;
}

bool steins_pack_frame(const steins_pack_t *pack, uint32_t index, steins_frame_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_FRAME, index, STEINS_REC_FRAME);
    if (!p || !out) return false;
    out->text = steins_rd16(p);
    out->speaker = steins_rd16(p + 2);
    out->view = steins_rd16(p + 4);
    out->aux = steins_rd16(p + 6);
    out->title = steins_rd16(p + 8);
    return true;
}

bool steins_pack_view(const steins_pack_t *pack, uint16_t index, steins_view_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_VIEW, index, STEINS_REC_VIEW);
    if (!p || !out) return false;
    out->count = p[0];
    out->layer_first = steins_rd16(p + 2);
    return true;
}

bool steins_pack_layer(const steins_pack_t *pack, uint16_t index, steins_layer_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_LAYER, index, STEINS_REC_LAYER);
    if (!p || !out) return false;
    out->asset = steins_rd16(p);
    out->x = (int16_t)steins_rd16(p + 2);
    out->y = (int16_t)steins_rd16(p + 4);
    out->w = steins_rd16(p + 6);
    out->h = steins_rd16(p + 8);
    out->opacity = steins_rd16(p + 10);
    return true;
}

bool steins_pack_asset(const steins_pack_t *pack, uint16_t index, steins_asset_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_ASSET, index, STEINS_REC_ASSET);
    if (!p || !out) return false;
    out->off = steins_rd32(p);
    out->len = steins_rd32(p + 4);
    out->mask_off = steins_rd32(p + 8);
    out->mask_len = steins_rd32(p + 12);
    out->w = steins_rd16(p + 16);
    out->h = steins_rd16(p + 18);
    out->mw = steins_rd16(p + 20);
    out->mh = steins_rd16(p + 22);
    out->cx = steins_rd16(p + 24);
    out->cy = steins_rd16(p + 26);
    out->cw = steins_rd16(p + 28);
    out->ch = steins_rd16(p + 30);
    out->ow = steins_rd16(p + 32);
    out->oh = steins_rd16(p + 34);
    out->flags = steins_rd16(p + 36);
    if (pack->sec_off[STEINS_SEC_BLOB] == 0u) return false;
    if (out->len == 0u) return false;
    if (out->off > pack->sec_size[STEINS_SEC_BLOB] ||
        out->len > pack->sec_size[STEINS_SEC_BLOB] - out->off) {
        return false;
    }
    if (out->mask_len != 0u &&
        (out->mask_off > pack->sec_size[STEINS_SEC_BLOB] ||
         out->mask_len > pack->sec_size[STEINS_SEC_BLOB] - out->mask_off)) {
        return false;
    }
    if (out->w == 0u || out->h == 0u || out->cw == 0u || out->ch == 0u ||
        out->ow == 0u || out->oh == 0u) {
        return false;
    }
    if (out->mask_len != 0u && (out->mw == 0u || out->mh == 0u)) return false;
    return true;
}

bool steins_pack_choice(const steins_pack_t *pack, uint16_t index, steins_choice_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_CHOICE, index, STEINS_REC_CHOICE);
    if (!p || !out) return false;
    out->heading = steins_rd16(p);
    out->category = steins_rd16(p + 2);
    out->first_item = steins_rd16(p + 4);
    out->count = p[6];
    out->kind = p[7];
    return true;
}

bool steins_pack_citem(const steins_pack_t *pack, uint16_t index, steins_citem_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_CITEM, index, STEINS_REC_CITEM);
    if (!p || !out) return false;
    out->text = steins_rd16(p);
    out->label = steins_rd16(p + 2);
    out->to = steins_rd16(p + 4);
    out->action = p[6];
    return true;
}

bool steins_pack_chapter(const steins_pack_t *pack, uint16_t index, steins_chapter_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_CHAPTER, index, STEINS_REC_CHAPTER);
    if (!p || !out) return false;
    out->name = steins_rd16(p);
    out->node = steins_rd16(p + 2);
    out->cursor = steins_rd32(p + 4);
    return true;
}

bool steins_pack_gal(const steins_pack_t *pack, uint16_t index, steins_gal_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_GAL, index, STEINS_REC_GAL);
    if (!p || !out) return false;
    out->label = steins_rd16(p);
    out->first = steins_rd16(p + 2);
    out->count = steins_rd16(p + 4);
    return true;
}

bool steins_pack_gentry(const steins_pack_t *pack, uint16_t index, uint16_t *asset_out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_GENTRY, index, STEINS_REC_GENTRY);
    if (!p || !asset_out) return false;
    *asset_out = steins_rd16(p);
    return true;
}

bool steins_pack_wline(const steins_pack_t *pack, uint16_t node, steins_wline_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_WLINE, node, STEINS_REC_WLINE);
    if (!p || !out) return false;
    out->node = steins_rd16(p);
    out->first = steins_rd16(p + 2);
    out->count = p[4];
    out->rate = (int32_t)steins_rd32(p + 6);
    return true;
}

bool steins_pack_wchange(const steins_pack_t *pack, uint16_t index, steins_wchange_t *out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_WCHANGE, index, STEINS_REC_WCHANGE);
    if (!p || !out) return false;
    out->cursor = (int32_t)steins_rd32(p);   // seed 行是 -1,按源原序保留
    out->rate = (int32_t)steins_rd32(p + 4);
    return true;
}

bool steins_pack_break(const steins_pack_t *pack, uint16_t index, uint32_t *cursor_out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_BREAK, index, STEINS_REC_BREAK);
    if (!p || !cursor_out) return false;
    *cursor_out = steins_rd32(p);
    return true;
}

uint32_t steins_pack_break_count(const steins_pack_t *pack)
{
    return pack ? pack->sec_count[STEINS_SEC_BREAK] : 0u;
}

uint32_t steins_pack_choice_count(const steins_pack_t *pack)
{
    return pack ? pack->sec_count[STEINS_SEC_CHOICE] : 0u;
}

#if defined(ESP_PLATFORM)
// target:IDF 5.5.3 自己的头(components/esp_rom/include/miniz.h)+ ROM 的
// tinfl_decompress(esp32c3.rom.ld: 0x400000f4),7 参、带 out_start。
// 注意:target 的 tinfl_decompressor(10992 B)与 host miniz 3.1(8376 B)布局不同,
// 两个 ABI 的 state 绝不互通、绝不强转,也不能“同 revision”互喂。
#include "miniz.h"
#define STEINS_TINFL_DEC(d, in, inlen, out, start, outlen, flags) \
    tinfl_decompress((d), (in), (inlen), (start), (out), (outlen), (flags))
#else
// host:测试专用 pinned miniz 3.1.0(tests/vendor/miniz),生产固件不链接。
#include "miniz.h"
#define STEINS_TINFL_DEC(d, in, inlen, out, start, outlen, flags) \
    tinfl_decompress((d), (in), (inlen), (start), (out), (outlen), (flags))
#endif

#define STEINS_TXT_BLOCK 2048u
#define STEINS_STR_MAX   1006u   /* 最长串 996 u16,留余量 */
static tinfl_decompressor s_tinfl;          /* 有界 BSS state:target=IDF 头,host=pinned miniz,两 ABI 不互通 */
static uint8_t s_win[2u * STEINS_TXT_BLOCK]; /* 两块窗口:跨块串最多跨 1 个边界 */
static uint16_t s_work[STEINS_STR_MAX + 2u];  /* 串工作区:返回给调用方 */
static uint32_t s_work_pack;                  /* 工作区归属哪个 pack */

static uint16_t steins_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// 解压第 idx 块到 dest;严格:长度精确、输入恰好消耗、DONE、CRC 匹配。
static bool steins_txt_block(const steins_pack_t *pack, uint32_t idx, uint8_t *dest)
{
    if (!pack || idx >= pack->sec_count[STEINS_SEC_TXTBLK]) return false;
    const uint8_t *tbl = pack->data + pack->sec_off[STEINS_SEC_TXTBLK] + idx * 8u;
    const uint32_t coff = steins_rd32(tbl);
    const uint16_t clen = steins_rd16(tbl + 4);
    const uint16_t crc = steins_rd16(tbl + 6);
    if (clen == 0u || coff + clen > pack->sec_size[STEINS_SEC_TXT]) return false;
    const uint32_t want = (idx + 1u == pack->sec_count[STEINS_SEC_TXTBLK])
                              ? pack->txt_raw_len - idx * STEINS_TXT_BLOCK
                              : STEINS_TXT_BLOCK;
    if (want == 0u || want > STEINS_TXT_BLOCK) return false;
    const uint8_t *in = pack->data + pack->sec_off[STEINS_SEC_TXT] + coff;
    size_t in_len = clen, out_len = want;
    tinfl_init(&s_tinfl);
    const int st = STEINS_TINFL_DEC(&s_tinfl, in, &in_len, dest, dest, &out_len,
                                    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || out_len != want || in_len != clen) return false;
    return steins_crc16(dest, want) == crc;
}

static const uint16_t *steins_str_v5(const steins_pack_t *pack, uint32_t off,
                                     uint16_t len, uint16_t *len_out)
{
    if (len > STEINS_STR_MAX) return NULL;
    if (len == 0u) {              /* 空串:返回一个稳定的空终止符,不触碰解压 */
        s_work[0] = 0u;
        if (len_out) *len_out = 0u;
        return s_work;
    }
    const uint32_t need = (uint32_t)len * 2u;
    if (off > pack->txt_raw_len || need > pack->txt_raw_len - off) return NULL;
    const uint32_t first = off / STEINS_TXT_BLOCK;
    const uint32_t last = (off + need - 1u) / STEINS_TXT_BLOCK;
    if (last > first + 1u) return NULL;   /* 最长串 < 1 块,不会跨 3 块 */
    for (uint32_t b = first; b <= last; ++b) {
        if (!steins_txt_block(pack, b, s_win + (b - first) * STEINS_TXT_BLOCK)) return NULL;
    }
    const uint8_t *src = s_win + (off - first * STEINS_TXT_BLOCK);
    uint16_t *dst = s_work;
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = (uint16_t)(src[i * 2u] | ((uint16_t)src[i * 2u + 1u] << 8));
    }
    s_work_pack = (uint32_t)(uintptr_t)pack;
    if (len_out) *len_out = len;
    return dst;
}

const uint16_t *steins_pack_str(const steins_pack_t *pack, uint16_t index, uint16_t *len_out)
{
    const uint8_t *p = rec(pack, STEINS_SEC_STRT, index, STEINS_REC_STRT);
    if (!p) return NULL;
    const uint32_t off = steins_rd32(p);
    const uint16_t len = steins_rd16(p + 4);
    if (pack->sec_version >= 5u) {
        return steins_str_v5(pack, off, len, len_out);
    }
    if (off > pack->sec_size[STEINS_SEC_TXT] ||
        (uint32_t)len * 2u > pack->sec_size[STEINS_SEC_TXT] - off) {
        return NULL;
    }
    if (len_out) *len_out = len;
    return (const uint16_t *)(const void *)(pack->data + pack->sec_off[STEINS_SEC_TXT] + off);
}

int steins_pack_str_utf8(const steins_pack_t *pack, uint16_t index, char *out, size_t cap)
{
    if (!out || cap == 0u) return -1;
    // 无论成功还是失败,只要 cap > 0,返回时 out 一定是一个合法的、以 NUL 结尾的
    // 前缀(失败时是"能完整编码出来的一段")。调用方拿到 -1 时可以安全显示这个前缀,
    // 也不会出现"上一个字符串的尾巴留在缓冲区"的情况。
    out[0] = '\0';
    uint16_t len = 0;
    const uint16_t *src = steins_pack_str(pack, index, &len);
    if (!src) return -1;
    size_t used = 0;
    for (uint16_t i = 0; i < len; ++i) {
        uint32_t cp = src[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            // 孤立代理:停在这里,保留已经写好的合法前缀(要 NUL 结尾)。
            if (i + 1u >= len) {
                out[used] = 0;
                return -1;
            }
            const uint32_t lo = src[i + 1u];
            if (lo < 0xDC00u || lo > 0xDFFFu) {
                out[used] = 0;
                return -1;
            }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            ++i;
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            out[used] = 0;
            return -1;
        }
        char buf[4];
        size_t n;
        if (cp < 0x80u) {
            buf[0] = (char)cp;
            n = 1;
        } else if (cp < 0x800u) {
            buf[0] = (char)(0xC0u | (cp >> 6));
            buf[1] = (char)(0x80u | (cp & 0x3Fu));
            n = 2;
        } else if (cp < 0x10000u) {
            buf[0] = (char)(0xE0u | (cp >> 12));
            buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            buf[2] = (char)(0x80u | (cp & 0x3Fu));
            n = 3;
        } else {
            buf[0] = (char)(0xF0u | (cp >> 18));
            buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            buf[3] = (char)(0x80u | (cp & 0x3Fu));
            n = 4;
        }
        if (used + n + 1u > cap) {
            out[used] = 0;                      // 放不下:保留并终止已写入的前缀
            return -1;
        }
        memcpy(out + used, buf, n);
        used += n;
    }
    out[used] = '\0';
    return (int)used;
}

const uint16_t *steins_pack_node_id(const steins_pack_t *pack, uint16_t node_index,
                                    uint16_t *len_out)
{
    steins_node_t node;
    if (!steins_pack_node(pack, node_index, &node)) return NULL;
    return steins_pack_str(pack, node.id_str, len_out);
}

static const char *meta_find(const steins_pack_t *pack, const char *key, size_t *len_out)
{
    if (!pack || pack->sec_off[STEINS_SEC_META] == 0u) return NULL;
    const char *meta = (const char *)(pack->data + pack->sec_off[STEINS_SEC_META]);
    const uint32_t size = pack->sec_size[STEINS_SEC_META];
    const size_t key_len = strlen(key);
    uint32_t i = 0;
    while (i < size) {
        uint32_t end = i;
        while (end < size && meta[end] != '\n') ++end;
        if (end - i > key_len && memcmp(meta + i, key, key_len) == 0 &&
            meta[i + key_len] == '=') {
            if (len_out) *len_out = end - i - key_len - 1u;
            return meta + i + key_len + 1u;
        }
        i = end + 1u;
    }
    return NULL;
}

int steins_pack_meta_int(const steins_pack_t *pack, const char *key, int fallback)
{
    size_t len = 0;
    const char *value = meta_find(pack, key, &len);
    if (!value || len == 0u) return fallback;
    int result = 0;
    bool negative = false;
    size_t i = 0;
    if (value[0] == '-') {
        negative = true;
        i = 1;
    }
    for (; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9') return fallback;
        result = result * 10 + (value[i] - '0');
        if (result > 1000000) return fallback;
    }
    return negative ? -result : result;
}

bool steins_pack_meta_str(const steins_pack_t *pack, const char *key, char *out, size_t cap)
{
    size_t len = 0;
    const char *value = meta_find(pack, key, &len);
    if (!value || !out || cap == 0u) return false;
    if (len + 1u > cap) return false;
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

uint16_t steins_pack_ui(const steins_pack_t *pack, const char *key)
{
    if (!pack || !key) return STEINS_NONE;
    for (uint32_t i = 0; i < pack->sec_count[STEINS_SEC_UI]; ++i) {
        const uint8_t *p = rec(pack, STEINS_SEC_UI, i, STEINS_REC_UI);
        if (!p) return STEINS_NONE;
        uint16_t len = 0;
        const uint16_t *k = steins_pack_str(pack, steins_rd16(p), &len);
        if (!k) continue;
        const size_t want = strlen(key);
        if (len != want) continue;
        bool same = true;
        for (size_t j = 0; j < want; ++j) {
            if (k[j] != (uint16_t)(uint8_t)key[j]) {
                same = false;
                break;
            }
        }
        if (same) return steins_rd16(p + 2);
    }
    return STEINS_NONE;
}

// ---------------------------------------------------------------- v4 手机内容
// 这些访问器在"包里还没有对应段"时返回 false/0:打包器与本文件同一次改动里一起落,
// 在那之前调用方拿到的是明确的 false,不是假数据。
static const uint8_t *mobile_rec(const steins_pack_t *pack, uint32_t section,
                                 uint32_t index, uint32_t rec_size)
{
    if (!pack || section >= STEINS_SEC_COUNT) return NULL;
    if (pack->sec_off[section] == 0u) return NULL;
    if (index >= pack->sec_count[section]) return NULL;
    if ((uint64_t)(index + 1u) * rec_size > pack->sec_size[section]) return NULL;
    return pack->data + pack->sec_off[section] + (size_t)index * rec_size;
}

uint16_t steins_pack_mail_count(const steins_pack_t *pack)
{
    return pack ? (uint16_t)pack->sec_count[STEINS_SEC_MAIL] : 0u;
}

bool steins_pack_mail(const steins_pack_t *pack, uint16_t id, steins_mail_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MAIL, id, 12u);
    if (!p || !out) return false;
    out->sender = steins_rd16(p);
    out->title = steins_rd16(p + 2);
    out->body = steins_rd16(p + 4);
    out->succ_first = steins_rd16(p + 6);
    out->type = p[8];
    out->succ_count = p[9];
    if (out->succ_first > pack->sec_count[STEINS_SEC_MAIL_SUCC] ||
        out->succ_count > pack->sec_count[STEINS_SEC_MAIL_SUCC] - out->succ_first) {
        return false;
    }
    return true;
}

uint8_t steins_pack_mail_succ(const steins_pack_t *pack, const steins_mail_t *mail,
                              uint16_t *out, uint8_t cap, bool type3_only)
{
    if (!pack || !mail || !out || cap == 0u) return 0u;
    uint8_t written = 0;
    for (uint8_t i = 0; i < mail->succ_count && written < cap; ++i) {
        const uint8_t *p = mobile_rec(pack, STEINS_SEC_MAIL_SUCC,
                                      (uint32_t)mail->succ_first + i, 2u);
        if (!p) break;
        const uint16_t succ = steins_rd16(p);
        steins_mail_t target;
        if (!steins_pack_mail(pack, succ, &target)) continue;
        if (type3_only && target.type != 3u) continue;
        out[written++] = succ;
    }
    return written;
}

bool steins_pack_mail_stamp(const steins_pack_t *pack, uint16_t id, uint16_t *lo, uint16_t *hi)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MAIL_STAMP, id, 4u);
    if (!p || !lo || !hi) return false;
    *lo = steins_rd16(p);
    *hi = steins_rd16(p + 2);
    return true;
}

bool steins_pack_mobile_file(const steins_pack_t *pack, uint16_t node, uint8_t *file_out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MNODE, node, 6u);
    if (!p || !file_out) return false;
    *file_out = p[0];
    return p[0] != 0xFFu;
}

bool steins_pack_node_source(const steins_pack_t *pack, uint16_t node, uint16_t *source_out)
{
    if (pack->sec_count[STEINS_SEC_MMAP] == 0u || !source_out) return false;
    for (uint32_t i = 0; i < pack->sec_count[STEINS_SEC_MMAP]; ++i) {
        const uint8_t *p = pack->data + pack->sec_off[STEINS_SEC_MMAP] + i * 6u;
        if (steins_rd16(p + 2) == node) {
            *source_out = steins_rd16(p);
            return true;
        }
    }
    return false;
}

bool steins_pack_mrow_range(const steins_pack_t *pack, uint16_t node,
                            uint16_t *first_row, uint16_t *row_count)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MNODE, node, 6u);
    if (!p || !first_row || !row_count) return false;
    if (p[0] == 0xFFu) return false;
    const uint16_t first = steins_rd16(p + 2);
    const uint16_t count = steins_rd16(p + 4);
    if (first > pack->sec_count[STEINS_SEC_MROW] ||
        count > pack->sec_count[STEINS_SEC_MROW] - first) {
        return false;
    }
    *first_row = first;
    *row_count = count;
    return true;
}

uint16_t steins_pack_mrow_count(const steins_pack_t *pack)
{
    return pack ? (uint16_t)pack->sec_count[STEINS_SEC_MROW] : 0u;
}

bool steins_pack_mrow(const steins_pack_t *pack, uint16_t index, steins_mrow_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MROW, index, 12u);
    if (!p || !out) return false;
    out->cursor = (int32_t)steins_rd32(p);   // seed 行是 -1,按源原序保留
    out->event_off = steins_rd32(p + 4);
    out->file = p[8];
    out->event_count = p[9];
    if (out->event_off > pack->sec_size[STEINS_SEC_MEVT]) return false;
    return true;
}

bool steins_pack_mevent(const steins_pack_t *pack, const steins_mrow_t *row, uint8_t index,
                        steins_mevent_t *out)
{
    if (!pack || !row || !out || index >= row->event_count) return false;
    if (pack->sec_off[STEINS_SEC_MEVT] == 0u) return false;
    const uint8_t *base = pack->data + pack->sec_off[STEINS_SEC_MEVT];
    const uint32_t size = pack->sec_size[STEINS_SEC_MEVT];
    uint32_t off = row->event_off;
    for (uint8_t i = 0; i < index; ++i) {
        if (off + 2u > size) return false;
        const uint32_t argc = base[off + 1u];
        off += 2u + argc * 2u;
        if (off > size) return false;
    }
    if (off + 2u > size) return false;
    const uint8_t kind = base[off];
    const uint8_t argc = base[off + 1u];
    if (kind >= STEINS_EV_KIND_COUNT || argc > STEINS_MEVENT_MAX_ARGS) return false;
    if (off + 2u + argc * 2u > size) return false;
    out->kind = kind;
    out->argc = argc;
    for (uint8_t i = 0; i < argc; ++i) out->arg[i] = steins_rd16(base + off + 2u + i * 2u);
    return true;
}

bool steins_pack_seed_desc(const steins_pack_t *pack, uint16_t desc,
                           uint16_t *first_id, uint16_t *count)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_SEED, desc, 4u);
    if (!p || !first_id || !count) return false;
    const uint16_t first = steins_rd16(p);
    const uint16_t n = steins_rd16(p + 2);
    if (first > pack->sec_count[STEINS_SEC_SEED_ID] ||
        n > pack->sec_count[STEINS_SEC_SEED_ID] - first) {
        return false;
    }
    *first_id = first;
    *count = n;
    return true;
}

bool steins_pack_seed_id(const steins_pack_t *pack, uint16_t desc, uint16_t index,
                         uint16_t *id_out)
{
    uint16_t first = 0, count = 0;
    if (!id_out || !steins_pack_seed_desc(pack, desc, &first, &count)) return false;
    if (index >= count) return false;
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_SEED_ID, (uint32_t)first + index, 2u);
    if (!p) return false;
    *id_out = steins_rd16(p);
    return *id_out < STEINS_MAIL_IDS;
}

bool steins_pack_call_desc(const steins_pack_t *pack, uint16_t desc, uint16_t *site_str,
                           uint16_t *kind_str, uint16_t *first_item, uint8_t *item_count,
                           uint8_t *owner, bool *waiting)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CALLDESC, desc, 8u);
    if (!p) return false;
    const uint16_t first = steins_rd16(p + 4);
    const uint8_t count = p[6];
    const uint8_t flags = p[7];
    if (first > pack->sec_count[STEINS_SEC_CALLDITEM] ||
        count > pack->sec_count[STEINS_SEC_CALLDITEM] - first) {
        return false;
    }
    if (site_str) *site_str = steins_rd16(p);
    if (kind_str) *kind_str = steins_rd16(p + 2);
    if (first_item) *first_item = first;
    if (item_count) *item_count = count;
    if (owner) *owner = (uint8_t)(flags >> 1);
    if (waiting) *waiting = (flags & 1u) != 0u;
    return true;
}

bool steins_pack_call_ditem(const steins_pack_t *pack, uint16_t index, uint16_t *id_out,
                            uint16_t *action_str, uint16_t *text_str)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CALLDITEM, index, 6u);
    if (!p) return false;
    if (id_out) *id_out = steins_rd16(p);
    if (action_str) *action_str = steins_rd16(p + 2);
    if (text_str) *text_str = steins_rd16(p + 4);
    return true;
}

bool steins_pack_pres_key(const steins_pack_t *pack, uint16_t node, uint16_t row_index,
                          uint8_t event_index, uint16_t *key_index)
{
    if (!key_index) return false;
    const uint32_t pres_count = pack->sec_count[STEINS_SEC_PRES];
    if (pres_count == 0u) return false;
    uint16_t first = 0, count = 0;
    if (!steins_pack_mrow_range(pack, node, &first, &count)) return false;
    if (row_index >= count) return false;
    steins_mrow_t row;
    if (!steins_pack_mrow(pack, (uint16_t)(first + row_index), &row)) return false;
    if (event_index >= row.event_count) return false;
    // event_index 是该行**全部事件**中的绝对位置(不是“第 N 个 presentation”);
    // 一旦越过 row.event_count 就停,绝不扫到 MEVT 段末。
    const uint8_t *ev = pack->data + pack->sec_off[STEINS_SEC_MEVT] + row.event_off;
    const uint8_t *end = ev + pack->sec_size[STEINS_SEC_MEVT] - row.event_off;
    uint8_t index = 0;
    while (index < row.event_count) {
        if (ev + 2u > end) return false;
        const uint8_t kind = ev[0];
        const uint8_t argc = ev[1];
        if ((size_t)(end - ev) < 2u + (size_t)argc * 2u) return false;
        const uint8_t *args = ev + 2;
        if (index == event_index) {
            if (kind != 10u || argc != 3u) return false;
            {
                const uint16_t id = steins_rd16(args);
                const uint8_t pkind = args[2] & 0xFFu;
                const uint8_t owner = args[4] & 0xFFu;
                uint16_t ch = 0;
                if (!steins_pack_node_source(pack, node, &ch)) return false;
                for (uint32_t i = 0; i < pres_count; ++i) {
                    const uint8_t *p = pack->data + pack->sec_off[STEINS_SEC_PRES] + i * 10u;
                    if (steins_rd16(p) == ch && (int32_t)steins_rd32(p + 2) == row.cursor &&
                        steins_rd16(p + 6) == id && p[8] == pkind && p[9] == owner) {
                        *key_index = (uint16_t)i;
                        return true;
                    }
                }
                return false;
            }
        }
        ev = args + (size_t)argc * 2u;
        ++index;
    }
    return false;
}

bool steins_pack_choice_meta(const steins_pack_t *pack, uint16_t choice, uint16_t *source_str,
                             uint16_t *choice_id, uint8_t *kind)
{
    if (pack->sec_count[STEINS_SEC_CHOICE_META] == 0u) return false;
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CHOICE_META, choice, 8u);
    if (!p) return false;
    if (source_str) *source_str = steins_rd16(p);
    if (choice_id) *choice_id = steins_rd16(p + 2);
    if (kind) *kind = p[4];
    return true;
}

bool steins_pack_choice_outgoing(const steins_pack_t *pack, uint16_t choice, uint16_t *mail_id)
{
    if (!pack || !mail_id) return false;
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CHOICE_META, choice, 8u);
    if (!p) return false;
    // reply 的副作用是 CITEM.item.id;尾部那份 parent 是冗余保留字段,不作为 outgoing。
    if (p[4] == 3u) return false;
    const uint16_t id = steins_rd16(p + 6);
    if (id == 0xFFFFu || id >= STEINS_MAIL_IDS) return false;
    *mail_id = id;
    return true;
}

bool steins_pack_citem_meta(const steins_pack_t *pack, uint16_t item, uint16_t *item_id)
{
    if (pack->sec_count[STEINS_SEC_CITEM_META] == 0u) return false;
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CITEM_META, item, 4u);
    if (!p || !item_id) return false;
    *item_id = steins_rd16(p);
    return true;
}

uint16_t steins_pack_call_site_count(const steins_pack_t *pack)
{
    return pack ? (uint16_t)pack->sec_count[STEINS_SEC_CALLSITE] : 0u;
}

bool steins_pack_call_site(const steins_pack_t *pack, uint16_t index, steins_callsite_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CALLSITE, index, 12u);
    if (!p || !out) return false;
    out->site = steins_rd16(p);
    out->first_item = steins_rd16(p + 2);
    out->source_str = steins_rd16(p + 4);
    out->owner = p[6];
    out->kind = p[7];
    out->item_count = p[8];
    out->limit = p[9];
    if (out->first_item > pack->sec_count[STEINS_SEC_CALLITEM] ||
        out->item_count > pack->sec_count[STEINS_SEC_CALLITEM] - out->first_item) {
        return false;
    }
    return true;
}

bool steins_pack_call_item(const steins_pack_t *pack, uint16_t index, steins_callitem_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CALLITEM, index, 10u);
    if (!p || !out) return false;
    out->text = steins_rd16(p);
    out->id = steins_rd16(p + 2);
    out->parent = steins_rd16(p + 4);
    out->action = p[6];
    out->clip = p[7];
    out->join = p[8];
    return true;
}

uint16_t steins_pack_clip_count(const steins_pack_t *pack)
{
    return pack ? (uint16_t)pack->sec_count[STEINS_SEC_CLIP] : 0u;
}

bool steins_pack_clip(const steins_pack_t *pack, uint8_t clip, steins_clip_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CLIP, clip, 6u);
    if (!p || !out) return false;
    out->first_frame = steins_rd16(p);
    out->frame_count = steins_rd16(p + 2);
    out->return_frame = steins_rd16(p + 4);
    if (out->first_frame > pack->sec_count[STEINS_SEC_CFRAME] ||
        out->frame_count > pack->sec_count[STEINS_SEC_CFRAME] - out->first_frame) {
        return false;
    }
    return true;
}

uint16_t steins_pack_cframe_count(const steins_pack_t *pack)
{
    return pack ? (uint16_t)pack->sec_count[STEINS_SEC_CFRAME] : 0u;
}

bool steins_pack_cframe(const steins_pack_t *pack, uint16_t index, steins_cframe_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_CFRAME, index, 6u);
    if (!p || !out) return false;
    out->speaker = steins_rd16(p);
    out->text = steins_rd16(p + 2);
    return true;
}

bool steins_pack_mobile_window(const steins_pack_t *pack, uint16_t node, steins_window_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_WINDOW, node, 14u);
    if (!p || !out) return false;
    out->node = steins_rd16(p);
    out->id = steins_rd16(p + 2);
    out->limit = steins_rd16(p + 4);
    out->skip = steins_rd16(p + 6);
    out->read = steins_rd16(p + 8);
    out->first_opt = steins_rd16(p + 10);
    out->opt_count = p[12];
    if (out->first_opt > pack->sec_count[STEINS_SEC_WINOPT] ||
        out->opt_count > pack->sec_count[STEINS_SEC_WINOPT] - out->first_opt) {
        return false;
    }
    return true;
}

bool steins_pack_window_opt(const steins_pack_t *pack, uint16_t index, steins_winopt_t *out)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_WINOPT, index, 10u);
    if (!p || !out) return false;
    out->text = steins_rd16(p);
    out->id = steins_rd16(p + 2);
    out->parent = steins_rd16(p + 4);
    out->to = steins_rd16(p + 6);
    out->action = p[8];
    if (out->to >= pack->sec_count[STEINS_SEC_NODE]) return false;
    return true;
}

bool steins_pack_keyphone(const steins_pack_t *pack, uint16_t node, steins_keyphone_t *out)
{
    // 8 个 u16 = 16 B(最后一个是保留 pad),与打包器一致。
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_KEYPHONE, node, 16u);
    if (!p || !out) return false;
    out->node = steins_rd16(p);
    out->skip = steins_rd16(p + 2);
    out->go = steins_rd16(p + 4);
    out->limit = steins_rd16(p + 6);
    out->after = steins_rd16(p + 8);
    out->sent = steins_rd16(p + 10);
    out->source_str = steins_rd16(p + 12);
    return true;
}

bool steins_pack_node_map(const steins_pack_t *pack, uint16_t source_node, uint16_t *pack_node)
{
    const uint8_t *p = mobile_rec(pack, STEINS_SEC_MMAP, source_node, 6u);
    if (!p || !pack_node) return false;
    const uint16_t target = steins_rd16(p + 2);
    if (target >= pack->sec_count[STEINS_SEC_NODE]) return false;
    *pack_node = target;
    return true;
}
