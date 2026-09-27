// main/tsxx_pack.c —— 资源包只读解析。
// 平坦模式(整块在内存)不依赖 ESP-IDF,可在宿主机上测试;分区模式用 esp_partition_mmap。
#include "tsxx_pack.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
static const char *TAG = "tsxx_pack";
#endif

// 段类型,必须与 tools/tsxx_pack.py 的 SEC_* 一致。
enum {
    SEC_SYM = 0, SEC_TEXT, SEC_TOFF, SEC_TLEN,
    SEC_PBG, SEC_PSPK, SEC_PSPR, SEC_PFLAG,
    SEC_PCGB, SEC_PCG,
    SEC_BGNAME, SEC_SPKNAME, SEC_SPRNAME, SEC_CGNAME,
    SEC_CHOICE, SEC_CHOICEOPT,
    SEC_BG, SEC_FG, SEC_EVB, SEC_EVC, SEC_CGDIR,
    SEC_META, SEC_COUNT
};

#define ENTRY_BG 12u
#define ENTRY_FG 24u
#define ENTRY_EVB 12u
#define ENTRY_EVC 12u
#define ENTRY_CGDIR 20u
#define ENTRY_CHOICE 12u
#define ENTRY_CHOICEOPT 12u
#define HEADER_SIZE 20u
#define SECTION_ENTRY 16u

// 资源包的检查点间隔,必须与 tools/tsxx_pack.py 的 CHECKPOINT_PAGES 一致。
#define CHECKPOINT_PAGES 256u

// 段表条目里的 位置/条数/长度。
typedef struct {
    uint32_t off;
    uint32_t count;
    uint32_t size;
} tsxx_section_t;

_Static_assert(SEC_COUNT == TSXX_PACK_SECTIONS, "段表条数必须与 tsxx_pack.h 一致");

static uint16_t rd16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// 位图前 page 页里有几张事件图。
static uint32_t bitmap_rank(const uint8_t *bits, uint32_t page)
{
    uint32_t rank = 0;
    const uint32_t whole = page >> 3;
    for (uint32_t i = 0; i < whole; ++i) {
        rank += (uint32_t)__builtin_popcount((unsigned)bits[i]);
    }
    for (uint32_t bit = 0; bit < (page & 7u); ++bit) {
        rank += (bits[whole] >> bit) & 1u;
    }
    return rank;
}

// 段表项:{ type u32, offset u32, count u32, size u32 };核对它落在包内。
static bool section_entry(const uint8_t *table, uint32_t pack_size, uint32_t type,
                          tsxx_section_t *out)
{
    if (table == NULL || out == NULL || type >= SEC_COUNT) {
        return false;
    }
    const uint8_t *entry = table + (type * SECTION_ENTRY);
    const uint32_t offset = rd32(entry + 4);
    const uint32_t count = rd32(entry + 8);
    const uint32_t size = rd32(entry + 12);
    if (offset > pack_size || size > pack_size - offset) {
        return false;
    }
    out->off = offset;
    out->count = count;
    out->size = size;
    return true;
}

// 校验段表与各段的长度关系,填出计数/长度与每段的绝对偏移;不建立任何指针。
// 平坦模式与分区模式共用:两边都对"22 个段、各段长度、计数是否匹配"有同样的要求。
static bool pack_layout(const uint8_t *table, uint32_t pack_size, tsxx_pack_t *view)
{
    tsxx_section_t secs[SEC_COUNT];
    for (uint32_t type = 0; type < SEC_COUNT; ++type) {
        if (!section_entry(table, pack_size, type, &secs[type])) {
            return false;
        }
        view->sec_off[type] = secs[type].off;
    }

    // SYM:u32 x sym_count。
    if (secs[SEC_SYM].count == 0 || secs[SEC_SYM].size % 4u != 0u ||
        secs[SEC_SYM].count != secs[SEC_SYM].size / 4u) {
        return false;
    }
    view->sym_count = secs[SEC_SYM].count;
    view->text_size = secs[SEC_TEXT].size;

    // 页表:TLEN 每页 1 字节,TOFF 每 256 页一个 u32 检查点(外加结尾的一个)。
    view->page_count = secs[SEC_TLEN].count;
    if (view->page_count == 0 || secs[SEC_TLEN].size != view->page_count) {
        return false;
    }
    if (secs[SEC_TOFF].count != (view->page_count + CHECKPOINT_PAGES - 1u) / CHECKPOINT_PAGES + 1u) {
        return false;
    }

    const struct {
        uint32_t type;
        bool bitmap;
    } arrays[] = {
        { SEC_PBG, false },
        { SEC_PSPK, false },
        { SEC_PSPR, false },
        { SEC_PFLAG, false },
        { SEC_PCGB, true },
    };
    for (size_t i = 0; i < sizeof(arrays) / sizeof(arrays[0]); ++i) {
        const tsxx_section_t *sec = &secs[arrays[i].type];
        const uint32_t expect = arrays[i].bitmap ? (view->page_count + 7u) / 8u : view->page_count;
        if (sec->count != expect || sec->size != expect) {
            return false;
        }
    }

    // PCG:u16 x 事件图页数(按页序排列的 CGDIR 下标)。
    if (secs[SEC_PCG].size % 2u != 0u) {
        return false;
    }
    view->cg_page_count = secs[SEC_PCG].count;
    if (view->cg_page_count != secs[SEC_PCG].size / 2u) {
        return false;
    }

    // 四个名字表都是 \n 分隔的裸字节串。
    view->bg_name_size = secs[SEC_BGNAME].size;
    view->spk_name_size = secs[SEC_SPKNAME].size;
    view->spr_name_size = secs[SEC_SPRNAME].size;
    view->cg_name_size = secs[SEC_CGNAME].size;

    // 选项表项:{ page u32, count u8, pad u8, pad u16, first u32 }。
    if (secs[SEC_CHOICE].size % ENTRY_CHOICE != 0u) {
        return false;
    }
    view->choice_count = secs[SEC_CHOICE].count;
    if (view->choice_count != secs[SEC_CHOICE].size / ENTRY_CHOICE) {
        return false;
    }
    if (secs[SEC_CHOICEOPT].size % ENTRY_CHOICEOPT != 0u) {
        return false;
    }
    view->choice_opt_count = secs[SEC_CHOICEOPT].count;
    if (view->choice_opt_count != secs[SEC_CHOICEOPT].size / ENTRY_CHOICEOPT) {
        return false;
    }

    // 带目录的段:[目录 count*stride][数据],目录项里的 off 相对数据区。
    const struct {
        uint32_t type;
        uint32_t stride;
        uint32_t *count;
        uint32_t *data_size;
    } blobs[] = {
        { SEC_BG, ENTRY_BG, &view->bg_count, &view->bg_data_size },
        { SEC_FG, ENTRY_FG, &view->fg_count, &view->fg_data_size },
        { SEC_EVB, ENTRY_EVB, &view->evb_count, &view->evb_data_size },
        { SEC_EVC, ENTRY_EVC, &view->evc_count, &view->evc_data_size },
    };
    for (size_t i = 0; i < sizeof(blobs) / sizeof(blobs[0]); ++i) {
        const tsxx_section_t *sec = &secs[blobs[i].type];
        if (sec->count > sec->size / blobs[i].stride) {   // 防 count * stride 溢出
            return false;
        }
        *blobs[i].count = sec->count;
        *blobs[i].data_size = sec->size - sec->count * blobs[i].stride;
    }

    // CGDIR:{ kind u8, pad u8, pad u16, base u32, x,y,w,h u16, img u32 }。
    const tsxx_section_t *cgdir = &secs[SEC_CGDIR];
    if (cgdir->count > cgdir->size / ENTRY_CGDIR) {
        return false;
    }
    view->cg_count = cgdir->count;
    if (view->cg_count * ENTRY_CGDIR != cgdir->size) {
        return false;
    }

    view->meta_size = secs[SEC_META].size;
    return true;
}

// 常驻段(段表与类型 0..15)的指针:base 是包基址(平坦)或常驻映射基址(分区)。
static void bind_resident(tsxx_pack_t *view, const uint8_t *base)
{
    view->syms = base + view->sec_off[SEC_SYM];
    view->text = base + view->sec_off[SEC_TEXT];
    view->toff = base + view->sec_off[SEC_TOFF];
    view->tlen = base + view->sec_off[SEC_TLEN];
    view->pbg = base + view->sec_off[SEC_PBG];
    view->pspk = base + view->sec_off[SEC_PSPK];
    view->pspr = base + view->sec_off[SEC_PSPR];
    view->pflag = base + view->sec_off[SEC_PFLAG];
    view->pcgb = base + view->sec_off[SEC_PCGB];
    view->pcg = base + view->sec_off[SEC_PCG];
    view->bg_names = base + view->sec_off[SEC_BGNAME];
    view->spk_names = base + view->sec_off[SEC_SPKNAME];
    view->spr_names = base + view->sec_off[SEC_SPRNAME];
    view->cg_names = base + view->sec_off[SEC_CGNAME];
    view->choices = base + view->sec_off[SEC_CHOICE];
    view->choice_opts = base + view->sec_off[SEC_CHOICEOPT];
}

// 解析 "<宽>x<高>":整串必须是这个格式,否则返回 false。
static bool parse_size(const uint8_t *text, uint32_t len, uint16_t *w, uint16_t *h)
{
    uint32_t value = 0;
    uint32_t digits = 0;
    uint32_t i = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        value = value * 10u + (uint32_t)(text[i] - '0');
        ++digits;
        ++i;
    }
    if (digits == 0 || value == 0 || value > 0xFFFFu || i >= len || text[i] != 'x') {
        return false;
    }
    const uint16_t width = (uint16_t)value;
    value = 0;
    digits = 0;
    ++i;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        value = value * 10u + (uint32_t)(text[i] - '0');
        ++digits;
        ++i;
    }
    if (digits == 0 || value == 0 || value > 0xFFFFu || i != len) {
        return false;
    }
    *w = width;
    *h = (uint16_t)value;
    return true;
}

// 在 META(key=value 逐行)里找 <key>=<宽>x<高>。按整行匹配,所以 background= /
// title_bg= 这类"名字里含 key"的行不会被误认成 bg=。
static bool meta_size(const uint8_t *meta, uint32_t size, const char *key, uint16_t *w,
                      uint16_t *h)
{
    const uint32_t key_len = (uint32_t)strlen(key);
    uint32_t pos = 0;
    while (pos < size) {
        uint32_t end = pos;
        while (end < size && meta[end] != '\n') {
            ++end;
        }
        if (end > pos + key_len + 1u && memcmp(meta + pos, key, key_len) == 0 &&
            meta[pos + key_len] == '=' &&
            parse_size(meta + pos + key_len + 1u, end - pos - key_len - 1u, w, h)) {
            return true;
        }
        pos = end + 1u;
    }
    return false;
}

// 把 META 里的三个尺寸搬进结构体:
//   art=        画布尺寸,必须与编译期常量一致,否则整层坐标都会错位;
//   bg=/event=  背景/事件图的存储尺寸,可以比画布小(合成时放大),不能比画布大
//               (固件只有放大路径)。缺这两行(旧包)时按 1:1 处理。
static bool meta_load_sizes(tsxx_pack_t *view)
{
    uint16_t w = 0;
    uint16_t h = 0;
    if (!meta_size(view->meta, view->meta_size, "art", &w, &h) || w != TSXX_ART_W ||
        h != TSXX_ART_H) {
        return false;
    }
    view->bg_w = TSXX_ART_W;
    view->bg_h = TSXX_ART_H;
    view->ev_w = TSXX_ART_W;
    view->ev_h = TSXX_ART_H;
    if (meta_size(view->meta, view->meta_size, "bg", &w, &h)) {
        if (w > TSXX_ART_W || h > TSXX_ART_H) {
            return false;
        }
        view->bg_w = w;
        view->bg_h = h;
    }
    if (meta_size(view->meta, view->meta_size, "event", &w, &h)) {
        if (w > TSXX_ART_W || h > TSXX_ART_H) {
            return false;
        }
        view->ev_w = w;
        view->ev_h = h;
    }
    return true;
}

bool tsxx_pack_open(tsxx_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (pack == NULL || data == NULL || size < HEADER_SIZE + SEC_COUNT * SECTION_ENTRY) {
        return false;
    }
    if (memcmp(data, TSXX_PACK_MAGIC, 8) != 0) {
        return false;
    }
    if (rd32(data + 8) != TSXX_PACK_VERSION) {
        return false;
    }
    if (rd32(data + 12) != size) {
        return false;
    }
    if (rd32(data + 16) != SEC_COUNT) {
        return false;
    }

    tsxx_pack_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;

    if (!pack_layout(data + HEADER_SIZE, size, &view)) {
        return false;
    }
    bind_resident(&view, data);
    view.meta = data + view.sec_off[SEC_META];
    // 图片段在平坦模式下也直接用包内偏移寻址(map_window 返回 blob + off),
    // 这里把目录/数据指针留空,避免分区模式下出现第二套失效的地址。
    if (!meta_load_sizes(&view)) {
        return false;
    }

    *pack = view;
    return true;
}

// 分区模式:把包内 [off, off+len) 放进滑动窗口,返回窗口内的指针。
// 平坦模式直接返回 blob + off(不调 IDF)。
// 返回的指针只在下一次 map_window() 之前有效 —— 调用方拿到就立刻解码。
static const uint8_t *map_window(const tsxx_pack_t *pack, uint32_t off, uint32_t len)
{
    if (len == 0 || off > pack->blob_size || len > pack->blob_size - off) {
        return NULL;
    }
#ifdef ESP_PLATFORM
    if (pack->partitioned) {
        const uint32_t end = off + len;
        if (pack->window != NULL && off >= pack->window_off &&
            end <= pack->window_off + pack->window_len) {
            return pack->window + (off - pack->window_off);
        }

        // 窗口至少 64 KiB(一整页)、最多 256 KiB,且不越过包尾:
        // 同一个目录/数据区里的连续访问能命中同一窗口。
        uint32_t span = len;
        if (span < TSXX_PACK_WINDOW_MIN) {
            span = TSXX_PACK_WINDOW_MIN;
        }
        if (span > TSXX_PACK_WINDOW_SIZE) {
            span = TSXX_PACK_WINDOW_SIZE;
        }
        if (span > pack->blob_size - off) {
            span = pack->blob_size - off;
        }

        // 读接口都是 const:这里改的只是窗口缓存,不改包内容。
        tsxx_pack_t *mutable_pack = (tsxx_pack_t *)(uintptr_t)pack;
        if (mutable_pack->window != NULL) {
            esp_partition_munmap(mutable_pack->window_map);
            mutable_pack->window = NULL;
            mutable_pack->window_len = 0;
            mutable_pack->window_map = 0;
        }
        const void *mapped = NULL;
        esp_partition_mmap_handle_t handle = 0;
        const esp_err_t err = esp_partition_mmap(mutable_pack->partition, off, span,
                                                 ESP_PARTITION_MMAP_DATA, &mapped, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "滑动窗口映射 [%u, %u) 失败(%s)", (unsigned)off,
                     (unsigned)(off + span), esp_err_to_name(err));
            return NULL;
        }
        mutable_pack->window = (const uint8_t *)mapped;
        mutable_pack->window_off = off;
        mutable_pack->window_len = span;
        mutable_pack->window_map = handle;
        return mutable_pack->window;
    }
#endif
    return pack->blob + off;
}

bool tsxx_pack_open_partition(tsxx_pack_t *pack, const char *label)
{
#ifdef ESP_PLATFORM
    if (pack == NULL || label == NULL) {
        return false;
    }
    // 失败时 pack 处于"未打开"状态;调用方只看返回值,不会再用它。
    memset(pack, 0, sizeof(*pack));

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到资源分区 %s", label);
        return false;
    }

    // 先读包头 512 字节:段表在 [20, 372),足够判完魔数/版本/总长/22 个段/各段长度,
    // 不用等真正取图时才报错。
    uint8_t header[512];
    if (part->size < sizeof(header) ||
        esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK) {
        ESP_LOGE(TAG, "读不到资源分区 %s 的包头", label);
        return false;
    }
    if (memcmp(header, TSXX_PACK_MAGIC, 8) != 0 || rd32(header + 8) != TSXX_PACK_VERSION ||
        rd32(header + 16) != SEC_COUNT) {
        ESP_LOGE(TAG, "资源分区 %s 不是 TSXX 资源包", label);
        return false;
    }
    const uint32_t pack_size = rd32(header + 12);
    if (pack_size < sizeof(header) || pack_size > part->size) {
        ESP_LOGE(TAG, "资源包 %u 字节与分区 %s(%u 字节)不符", (unsigned)pack_size, label,
                 (unsigned)part->size);
        return false;
    }

    pack->blob_size = pack_size;
    if (!pack_layout(header + HEADER_SIZE, pack_size, pack)) {
        ESP_LOGE(TAG, "资源分区 %s 的段表不自洽", label);
        return false;
    }

    // 常驻映射只覆盖脚本区间 [0, SEC_BG 偏移);图片区间靠滑动窗口。
    // 绝不整包映射 —— 那正是上一版让引导加载器映射 rodata 段时复位的根因。
    const uint32_t resident_size = pack->sec_off[SEC_BG];
    if (resident_size < sizeof(header) || resident_size > TSXX_PACK_RESIDENT_LIMIT) {
        ESP_LOGE(TAG, "脚本区间 %u 字节超出常驻映射上限 %u", (unsigned)resident_size,
                 (unsigned)TSXX_PACK_RESIDENT_LIMIT);
        return false;
    }

    // META 在包末尾,不能常驻映射:拷一份进结构体,顺便补 NUL 方便当字符串用。
    if (pack->meta_size == 0 || pack->meta_size > TSXX_PACK_META_MAX) {
        ESP_LOGE(TAG, "META %u 字节超出上限 %u", (unsigned)pack->meta_size,
                 (unsigned)TSXX_PACK_META_MAX);
        return false;
    }
    if (esp_partition_read(part, pack->sec_off[SEC_META], pack->meta_buf, pack->meta_size) !=
        ESP_OK) {
        ESP_LOGE(TAG, "读不到资源分区 %s 的 META 段", label);
        return false;
    }
    pack->meta_buf[pack->meta_size] = '\0';
    pack->meta = pack->meta_buf;   // meta_buf 已在 pack 里,地址不会再变
    if (!meta_load_sizes(pack)) {
        ESP_LOGE(TAG, "资源包的 META 尺寸与固件不符(需要 art=%ux%u,且 bg/event 不超过它)",
                 TSXX_ART_W, TSXX_ART_H);
        return false;
    }

    const void *resident = NULL;
    esp_partition_mmap_handle_t handle = 0;
    const esp_err_t err = esp_partition_mmap(part, 0, resident_size, ESP_PARTITION_MMAP_DATA,
                                             &resident, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "常驻映射 %u 字节失败(%s)", (unsigned)resident_size, esp_err_to_name(err));
        return false;
    }

    pack->partitioned = true;
    pack->partition = part;
    pack->resident = (const uint8_t *)resident;
    pack->resident_size = resident_size;
    pack->resident_map = handle;
    pack->blob = (const uint8_t *)resident;
    bind_resident(pack, pack->blob);
    ESP_LOGI(TAG, "资源包已挂载:常驻 %u 字节(脚本区间),图片走 %u KiB 滑动窗口",
             (unsigned)resident_size, (unsigned)(TSXX_PACK_WINDOW_SIZE / 1024u));
    ESP_LOGI(TAG, "尺寸:画布 %ux%u(1:1),背景 %ux%u,事件图 %ux%u(合成时放大)",
             TSXX_ART_W, TSXX_ART_H, pack->bg_w, pack->bg_h, pack->ev_w, pack->ev_h);
    return true;
#else
    (void)pack;
    (void)label;
    return false;
#endif
}

uint32_t tsxx_pack_pages(const tsxx_pack_t *pack)
{
    return pack->page_count;
}

uint8_t tsxx_pack_bg_count(const tsxx_pack_t *pack)
{
    return (uint8_t)pack->bg_count;
}

uint8_t tsxx_pack_sprite_count(const tsxx_pack_t *pack)
{
    return (uint8_t)pack->fg_count;
}

uint16_t tsxx_pack_cg_count(const tsxx_pack_t *pack)
{
    return (uint16_t)pack->evb_count;
}

// 页正文在码流里的起始字节偏移:从最近的检查点按字符数走一遍。
static uint32_t text_offset(const tsxx_pack_t *pack, uint32_t page)
{
    const uint32_t block = page / CHECKPOINT_PAGES;
    uint32_t skip = 0;
    for (uint32_t i = block * CHECKPOINT_PAGES; i < page; ++i) {
        skip += pack->tlen[i];
    }
    uint32_t pos = rd32(pack->toff + block * 4u);
    while (skip > 0) {
        pos += (pack->text[pos] == 0) ? 3u : 1u;
        --skip;
    }
    return pos;
}

bool tsxx_pack_page(const tsxx_pack_t *pack, uint32_t index, tsxx_page_t *out)
{
    if (index >= pack->page_count) {
        return false;
    }
    out->bg = pack->pbg[index];
    out->speaker = pack->pspk[index];
    out->sprite = pack->pspr[index];
    out->flags = pack->pflag[index];
    out->text_len = pack->tlen[index];
    out->text_off = out->text_len ? text_offset(pack, index) : 0;
    out->has_cg = (pack->pcgb[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
    return true;
}

uint32_t tsxx_pack_cg_rank(const tsxx_pack_t *pack, uint32_t page)
{
    if (page > pack->page_count) {
        page = pack->page_count;
    }
    return bitmap_rank(pack->pcgb, page);
}

uint16_t tsxx_pack_cg_at(const tsxx_pack_t *pack, uint32_t rank)
{
    if (rank >= pack->cg_page_count) {
        return TSXX_NONE16;
    }
    return rd16(pack->pcg + rank * 2u);
}

uint32_t tsxx_pack_cg_total(const tsxx_pack_t *pack)
{
    return pack->cg_page_count;
}

// 解一个符号(1 字节码或 0x00 + u16),返回其 UTF-8 字节数与码位。
static uint32_t decode_symbol(const tsxx_pack_t *pack, uint32_t *pos, uint32_t *codepoint)
{
    const uint8_t code = pack->text[*pos];
    uint32_t id;
    if (code == 0) {
        id = rd16(pack->text + *pos + 1);
        *pos += 3;
    } else {
        id = (uint32_t)code - 1u;
        *pos += 1;
    }
    *codepoint = (id < pack->sym_count) ? rd32(pack->syms + id * 4u) : 0xFFFDu;
    return 1;
}

static size_t emit_utf8(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xC0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xE0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

size_t tsxx_pack_text(const tsxx_pack_t *pack, const tsxx_page_t *page, uint32_t skip,
                      uint32_t max_chars, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (page == NULL || page->text_len == 0 || skip >= page->text_len) {
        return 0;
    }
    uint32_t pos = page->text_off;
    uint32_t written = 0;
    const uint32_t limit = max_chars ? (skip + max_chars) : page->text_len;
    for (uint32_t i = 0; i < page->text_len; ++i) {
        uint32_t codepoint = 0;
        decode_symbol(pack, &pos, &codepoint);
        if (i < skip || i >= limit) {
            continue;
        }
        char encoded[4];
        const size_t len = emit_utf8(codepoint, encoded);
        if (written + len >= capacity) {   // 留出结尾 NUL
            break;
        }
        memcpy(out + written, encoded, len);
        written += len;
    }
    out[written] = '\0';
    return written;
}

size_t tsxx_pack_name(const tsxx_pack_t *pack, uint8_t table, uint16_t id, char *out,
                      size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    const uint8_t *blob = NULL;
    uint32_t size = 0;
    switch (table) {
    case TSXX_TABLE_BG:
        blob = pack->bg_names;
        size = pack->bg_name_size;
        break;
    case TSXX_TABLE_SPEAKER:
        blob = pack->spk_names;
        size = pack->spk_name_size;
        break;
    case TSXX_TABLE_SPRITE:
        blob = pack->spr_names;
        size = pack->spr_name_size;
        break;
    case TSXX_TABLE_EVENT:
        blob = pack->cg_names;
        size = pack->cg_name_size;
        break;
    default:
        return 0;
    }
    uint32_t index = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= size; ++i) {
        if (i != size && blob[i] != '\n') {
            continue;
        }
        if (index == id) {
            const uint32_t len = i - start;
            const uint32_t copy = (len < capacity - 1) ? len : (uint32_t)(capacity - 1);
            memcpy(out, blob + start, copy);
            out[copy] = '\0';
            return copy;
        }
        ++index;
        start = i + 1;
    }
    return 0;
}

// 带目录的段里的第 index 张图。目录与数据都用包内绝对偏移(分区模式算窗口)。
// 返回的 jpeg 指针是 map_window() 的结果:调用方拿到就立刻解码。
static bool image_at(const tsxx_pack_t *pack, uint32_t dir_off, uint32_t stride,
                     uint32_t data_off, uint32_t data_size, uint32_t index, tsxx_image_t *out)
{
    const uint8_t *entry = map_window(pack, dir_off + index * stride, stride);
    if (entry == NULL) {
        return false;
    }
    const uint32_t offset = rd32(entry);
    const uint32_t length = rd32(entry + 4);
    const uint16_t w = rd16(entry + 8);
    const uint16_t h = rd16(entry + 10);
    if (length == 0 || offset > data_size || length > data_size - offset) {
        return false;
    }
    const uint8_t *jpeg = map_window(pack, data_off + offset, length);
    if (jpeg == NULL) {
        return false;
    }
    out->jpeg = jpeg;
    out->jpeg_len = length;
    out->w = w;
    out->h = h;
    return true;
}

bool tsxx_pack_bg(const tsxx_pack_t *pack, uint8_t id, tsxx_image_t *out)
{
    if (id >= pack->bg_count) {
        return false;
    }
    const uint32_t dir_off = pack->sec_off[SEC_BG];
    return image_at(pack, dir_off, ENTRY_BG, dir_off + pack->bg_count * ENTRY_BG,
                    pack->bg_data_size, id, out);
}

bool tsxx_pack_sprite(const tsxx_pack_t *pack, uint8_t id, tsxx_sprite_t *out)
{
    if (id >= pack->fg_count) {
        return false;
    }
    const uint8_t *entry = map_window(pack, pack->sec_off[SEC_FG] + (uint32_t)id * ENTRY_FG,
                                      ENTRY_FG);
    if (entry == NULL) {
        return false;
    }
    const uint32_t body_off = rd32(entry);
    const uint32_t body_len = rd32(entry + 4);
    const uint32_t mask_off = rd32(entry + 8);
    const uint32_t mask_len = rd32(entry + 12);
    const uint16_t w = rd16(entry + 16);
    const uint16_t h = rd16(entry + 18);
    const uint16_t x = rd16(entry + 20);
    const uint16_t y = rd16(entry + 22);
    if (body_len == 0 || body_off > pack->fg_data_size ||
        body_len > pack->fg_data_size - body_off) {
        return false;
    }
    if (mask_len == 0 || mask_len != ((uint32_t)(w + 7u) / 8u) * h ||
        mask_off > pack->fg_data_size || mask_len > pack->fg_data_size - mask_off) {
        return false;
    }
    // jpeg 与 mask 必须同时有效:一次窗口映射同时覆盖两者(它们只相差几 KiB)。
    const uint32_t lo = body_off < mask_off ? body_off : mask_off;
    const uint32_t body_end = body_off + body_len;
    const uint32_t mask_end = mask_off + mask_len;
    const uint32_t hi = body_end > mask_end ? body_end : mask_end;
    const uint8_t *data = map_window(pack, pack->sec_off[SEC_FG] + pack->fg_count * ENTRY_FG + lo,
                                     hi - lo);
    if (data == NULL) {
        return false;
    }
    out->jpeg = data + (body_off - lo);
    out->jpeg_len = body_len;
    out->mask = data + (mask_off - lo);
    out->mask_len = mask_len;
    out->w = w;
    out->h = h;
    out->x = x;
    out->y = y;
    return true;
}

bool tsxx_pack_cg(const tsxx_pack_t *pack, uint16_t id, tsxx_cg_t *out)
{
    if (id >= pack->cg_count) {
        return false;
    }
    const uint8_t *entry =
        map_window(pack, pack->sec_off[SEC_CGDIR] + (uint32_t)id * ENTRY_CGDIR, ENTRY_CGDIR);
    if (entry == NULL) {
        return false;
    }
    out->kind = entry[0];
    out->base = rd32(entry + 4);
    out->x = rd16(entry + 8);
    out->y = rd16(entry + 10);
    out->w = rd16(entry + 12);
    out->h = rd16(entry + 14);
    out->img = rd32(entry + 16);
    if (out->kind == TSXX_CG_FRAME) {
        return out->img < pack->evb_count;
    }
    if (out->kind == TSXX_CG_PATCH) {
        return out->img < pack->evc_count && out->base != id;
    }
    return false;
}

bool tsxx_pack_cg_image(const tsxx_pack_t *pack, const tsxx_cg_t *cg, tsxx_image_t *out)
{
    if (cg->kind == TSXX_CG_FRAME) {
        const uint32_t dir_off = pack->sec_off[SEC_EVB];
        return image_at(pack, dir_off, ENTRY_EVB, dir_off + pack->evb_count * ENTRY_EVB,
                        pack->evb_data_size, cg->img, out);
    }
    if (cg->kind == TSXX_CG_PATCH) {
        const uint32_t dir_off = pack->sec_off[SEC_EVC];
        return image_at(pack, dir_off, ENTRY_EVC, dir_off + pack->evc_count * ENTRY_EVC,
                        pack->evc_data_size, cg->img, out);
    }
    return false;
}

const char *tsxx_pack_meta(const tsxx_pack_t *pack)
{
    return (const char *)pack->meta;
}

uint8_t tsxx_pack_title_bg(const tsxx_pack_t *pack)
{
    static const char key[] = "title_bg=";
    const char *meta = (const char *)pack->meta;
    for (uint32_t i = 0; i + sizeof(key) - 1u <= pack->meta_size; ++i) {
        if (memcmp(meta + i, key, sizeof(key) - 1u) != 0) {
            continue;
        }
        uint32_t value = 0;
        uint32_t j = i + sizeof(key) - 1u;
        while (j < pack->meta_size && meta[j] >= '0' && meta[j] <= '9') {
            value = value * 10u + (uint32_t)(meta[j] - '0');
            ++j;
        }
        return (uint8_t)value;
    }
    return 0;
}

// 选项表按页升序排列,用二分查找。
static const uint8_t *find_choice(const tsxx_pack_t *pack, uint32_t page)
{
    uint32_t low = 0;
    uint32_t high = pack->choice_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2u;
        const uint8_t *entry = pack->choices + mid * ENTRY_CHOICE;
        const uint32_t value = rd32(entry);
        if (value == page) {
            return entry;
        }
        if (value < page) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return NULL;
}

uint8_t tsxx_pack_choice_count(const tsxx_pack_t *pack, uint32_t page)
{
    const uint8_t *entry = find_choice(pack, page);
    return entry ? entry[4] : 0u;
}

bool tsxx_pack_choice_option(const tsxx_pack_t *pack, uint32_t page, uint8_t slot,
                             char *out, size_t capacity, uint32_t *target_page)
{
    const uint8_t *entry = find_choice(pack, page);
    if (entry == NULL) {
        return false;
    }
    const uint8_t count = entry[4];
    if (slot >= count) {
        return false;
    }
    const uint32_t first = rd32(entry + 8);
    if ((uint32_t)slot >= pack->choice_opt_count || first > pack->choice_opt_count - slot) {
        return false;
    }
    const uint8_t *option = pack->choice_opts + (first + slot) * ENTRY_CHOICEOPT;
    const uint32_t offset = rd32(option);
    const uint32_t length = option[4];
    const uint32_t target = rd32(option + 8);
    if (out != NULL && capacity > 0) {
        tsxx_page_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.text_len = (uint8_t)length;
        fake.text_off = offset;
        tsxx_pack_text(pack, &fake, 0, 0, out, capacity);
    }
    if (target_page != NULL) {
        *target_page = target;
    }
    return true;
}
