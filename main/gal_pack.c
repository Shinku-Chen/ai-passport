// main/gal_pack.c —— 资源包校验与只读访问。纯 C,不依赖 ESP-IDF/LVGL。
#include "gal_pack.h"

#include <string.h>

#define PACK_HEADER 24u
#define PACK_SECT_ENTRY 16u

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t count;
    uint32_t size;
} pack_section_t;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t unit_size(int sec)
{
    switch (sec) {
    case GAL_SEC_META: return 4;
    case GAL_SEC_SCENES: return 20;
    case GAL_SEC_ROUTES: return 8;
    case GAL_SEC_CHAPTERS: return 14;
    case GAL_SEC_OPS: return 12;
    case GAL_SEC_LINKS: return 12;
    case GAL_SEC_ITEMS: return 16;
    case GAL_SEC_POSES: return 20;
    case GAL_SEC_CHAR_DETAILS: return 12;
    case GAL_SEC_MOVES: return 6;
    case GAL_SEC_MOVE_ENTRIES: return 8;
    case GAL_SEC_VIS: return 2;
    case GAL_SEC_AREAS: return 8;
    case GAL_SEC_EVIDENCE_CATS: return 1;
    case GAL_SEC_MAP_AREAS: return 2;
    case GAL_SEC_EXPRS: return 6;
    case GAL_SEC_EXPR_DATA: return 1;
    case GAL_SEC_IMAGES: return 40;
    case GAL_SEC_IMGDATA: return 1;
    case GAL_SEC_BOOK: return 28;
    case GAL_SEC_GALLERY: return 10;
    case GAL_SEC_GALLERY_ITEMS: return 4;
    case GAL_SEC_VARS: return 12;
    case GAL_SEC_LINK_OPS: return 8;
    case GAL_SEC_MOVE_OPS: return 6;
    case GAL_SEC_BOOKKEYS: return 20;
    case GAL_SEC_STROFF: return 4;
    case GAL_SEC_STR: return 1;
    case GAL_SEC_TEXTS: return 2;
    default: return 0;
    }
}

#define FAIL(msg) do { if (err) *err = (msg); return false; } while (0)

static bool text_range_ok(const gal_pack_t *p, uint32_t off, uint16_t len)
{
    if (!p || !p->sec[GAL_SEC_TEXTS]) return false;
    if (off > p->count[GAL_SEC_TEXTS]) return false;
    if ((uint64_t)off + (uint64_t)len > (uint64_t)p->count[GAL_SEC_TEXTS]) return false;
    const uint16_t *units = (const uint16_t *)p->sec[GAL_SEC_TEXTS];
    for (uint16_t i = 0; i < len; ++i) {
        const uint16_t u = units[off + i];
        if (u == 0) return false;  // shared payload is fine, declared range NUL is not
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= len) return false;
            const uint16_t low = units[off + i + 1];
            if (low < 0xDC00 || low > 0xDFFF) return false;
            ++i;  // valid surrogate pair
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;  // isolated low surrogate
        }
    }
    return true;
}

static bool image_ok(const gal_pack_t *p, uint16_t id)
{
    return id < p->count[GAL_SEC_IMAGES];
}

bool gal_pack_open(gal_pack_t *pack, const uint8_t *data, uint32_t size, const char **err)
{
    if (err) *err = NULL;
    if (!pack || !data) FAIL("资源包为空");
    memset(pack, 0, sizeof(*pack));
    if (size < PACK_HEADER + PACK_SECT_ENTRY * GAL_PACK_SECTIONS) FAIL("资源包过短");
    if (memcmp(data, GAL_PACK_MAGIC, 8) != 0) FAIL("资源包标识不符");
    const uint32_t version = rd32(data + 8);
    const uint32_t total = rd32(data + 12);
    const uint32_t parts = rd32(data + 16);
    if (version != GAL_PACK_VERSION) FAIL("资源包版本不支持");
    if (parts != GAL_PACK_SECTIONS) FAIL("资源包段数不符");
    if (total != size) FAIL("资源包长度字段不符");
    pack->base = data;
    pack->size = size;

    uint32_t ranges[GAL_PACK_SECTIONS][2];
    uint32_t range_count = 0;
    for (uint32_t i = 0; i < parts; ++i) {
        const uint8_t *e = data + PACK_HEADER + i * PACK_SECT_ENTRY;
        const uint32_t type = rd32(e), off = rd32(e + 4), count = rd32(e + 8), bytes = rd32(e + 12);
        if (type != i) FAIL("资源包段顺序或重复类型不符");
        if ((uint64_t)off + (uint64_t)bytes > size) FAIL("资源包段越界");
        if (bytes != 0 && off < PACK_HEADER + PACK_SECT_ENTRY * parts) {
            FAIL("资源包段起点落入目录");
        }
        const uint32_t unit = unit_size((int)type);
        if (unit == 0) FAIL("资源包段类型未知");
        if (unit > 1 && bytes % unit != 0) FAIL("资源包段长度不是记录整数倍");
        if (unit == 1 && count != bytes && type != GAL_SEC_STR &&
            type != GAL_SEC_EXPR_DATA && type != GAL_SEC_IMGDATA) {
            FAIL("资源包字节段计数不符");
        }
        if (unit > 1 && count != bytes / unit) FAIL("资源包段计数不符");
        if (bytes) {
            for (uint32_t j = 0; j < range_count; ++j) {
                const uint64_t a0 = ranges[j][0], a1 = a0 + ranges[j][1];
                if ((uint64_t)off < a1 && a0 < (uint64_t)off + bytes) {
                    FAIL("资源包非零段重叠");
                }
            }
            ranges[range_count][0] = off;
            ranges[range_count][1] = bytes;
            range_count++;
        }
        pack->sec[type] = bytes ? data + off : NULL;
        pack->count[type] = count;
        pack->len[type] = bytes;
    }

    // META
    if (!pack->sec[GAL_SEC_META] || pack->count[GAL_SEC_META] != 26) FAIL("资源包缺少 META");
    for (int i = 0; i < 26; ++i) pack->meta[i] = rd32(pack->sec[GAL_SEC_META] + 4 * i);
    if (pack->meta[0] != GAL_PACK_VERSION) FAIL("资源包 META 版本不符");
    const uint32_t scenes = pack->meta[2], routes = pack->meta[3], chapters = pack->meta[4];
    const uint32_t ops = pack->meta[5], books = pack->meta[6], groups = pack->meta[7];
    const uint32_t galitems = pack->meta[8], images = pack->meta[9];
    const uint32_t vars = pack->meta[12], text_units = pack->meta[13];
    const uint32_t exprs = pack->meta[14], poses = pack->meta[15], links = pack->meta[16];
    const uint32_t items = pack->meta[17], moves = pack->meta[18], vis = pack->meta[19];
    const uint32_t areas = pack->meta[20], evcats = pack->meta[21], mapareas = pack->meta[22];
    const uint32_t chars = pack->meta[23], link_ops = pack->meta[24], move_ops = pack->meta[25];

    if (pack->count[GAL_SEC_SCENES] != scenes) FAIL("SCENES 计数不符");
    if (pack->count[GAL_SEC_ROUTES] != routes) FAIL("ROUTES 计数不符");
    if (pack->count[GAL_SEC_CHAPTERS] != chapters) FAIL("CHAPTERS 计数不符");
    if (pack->count[GAL_SEC_OPS] != ops) FAIL("OPS 计数不符");
    if (pack->count[GAL_SEC_LINKS] != links) FAIL("LINKS 计数不符");
    if (pack->count[GAL_SEC_ITEMS] != items) FAIL("ITEMS 计数不符");
    if (pack->count[GAL_SEC_POSES] != poses) FAIL("POSES 计数不符");
    if (pack->count[GAL_SEC_CHAR_DETAILS] != chars) FAIL("CHAR_DETAILS 计数不符");
    if (pack->count[GAL_SEC_MOVES] != moves) FAIL("MOVES 计数不符");
    if (pack->count[GAL_SEC_VIS] != vis) FAIL("VIS 计数不符");
    if (pack->count[GAL_SEC_AREAS] != areas) FAIL("AREAS 计数不符");
    if (pack->count[GAL_SEC_EVIDENCE_CATS] != evcats) FAIL("EVIDENCE_CATS 计数不符");
    if (pack->count[GAL_SEC_MAP_AREAS] != mapareas) FAIL("MAP_AREAS 计数不符");
    if (pack->count[GAL_SEC_EXPRS] != exprs) FAIL("EXPRS 计数不符");
    if (pack->count[GAL_SEC_IMAGES] != images) FAIL("IMAGES 计数不符");
    if (pack->count[GAL_SEC_BOOK] != books) FAIL("BOOK 计数不符");
    if (pack->count[GAL_SEC_GALLERY] != groups) FAIL("GALLERY 计数不符");
    if (pack->count[GAL_SEC_GALLERY_ITEMS] != galitems) FAIL("GALLERY_ITEMS 计数不符");
    if (pack->count[GAL_SEC_VARS] != vars) FAIL("VARS 计数不符");
    if (pack->count[GAL_SEC_TEXTS] != text_units) FAIL("TEXTS 计数不符");
    if (pack->count[GAL_SEC_LINK_OPS] != link_ops) FAIL("LINK_OPS 计数不符");
    if (pack->count[GAL_SEC_MOVE_OPS] != move_ops) FAIL("MOVE_OPS 计数不符");
    if (pack->count[GAL_SEC_STROFF] != pack->count[GAL_SEC_STR]) FAIL("STROFF 计数不符");
    if (scenes == 0 || ops == 0 || vars > GAL_MAX_VARS) FAIL("资源包规模非法");
    const uint32_t initial = pack->meta[1];
    const uint32_t title_a = pack->meta[10], title_b = pack->meta[11];
    if (initial >= scenes) FAIL("初始场景非法");
    if (title_a >= images || title_b >= images) FAIL("标题图非法");

    // 字符串偏移表:单调、在 STR 段内且 NUL 结尾。
    const uint32_t *stroff = (const uint32_t *)pack->sec[GAL_SEC_STROFF];
    const char *strs = (const char *)pack->sec[GAL_SEC_STR];
    if (!stroff || !strs) FAIL("缺少字符串段");
    uint32_t prev = 0;
    for (uint32_t i = 0; i < pack->count[GAL_SEC_STR]; ++i) {
        const uint32_t off = stroff[i];
        if (off < prev) FAIL("字符串偏移不单调");
        prev = off;
        // 字符串段的长度没有单独字段,用下一个偏移/包尾夹逼,再确认 NUL。
        const uint32_t limit = (i + 1 < pack->count[GAL_SEC_STR]) ? stroff[i + 1]
                              : pack->len[GAL_SEC_STR];
        if (off >= limit) FAIL("字符串偏移越界");
        if (!memchr(strs + off, 0, limit - off)) FAIL("字符串缺少 NUL");
    }

    // 场景与指令范围、跳转目标、文本范围。
    for (uint32_t i = 0; i < scenes; ++i) {
        const gal_scene_t *s = (const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + i;
        if (s->id >= pack->count[GAL_SEC_STR] || s->name >= pack->count[GAL_SEC_STR])
            FAIL("场景字符串非法");
        if ((uint64_t)s->op_start + s->op_count > ops) FAIL("场景指令范围越界");
        if (s->op_count == 0) FAIL("场景为空");
    }
    for (uint32_t i = 0; i < chapters; ++i) {
        const gal_chapter_t *c = (const gal_chapter_t *)pack->sec[GAL_SEC_CHAPTERS] + i;
        if (c->scene >= scenes) FAIL("章节入口场景非法");
        if (c->name >= pack->count[GAL_SEC_STR]) FAIL("章节名字符串非法");
    }
    for (uint32_t i = 0; i < routes; ++i) {
        const gal_route_t *r = (const gal_route_t *)pack->sec[GAL_SEC_ROUTES] + i;
        if (r->name >= pack->count[GAL_SEC_STR]) FAIL("路线名字符串非法");
        if ((uint32_t)r->first + r->count > chapters) FAIL("路线章节范围越界");
        if (r->count > GAL_PACK_MAX_ROUTE_ENTRIES) FAIL("路线章节超过界面容量");
    }
    for (uint32_t i = 0; i < exprs; ++i) {
        const gal_expr_t *e = (const gal_expr_t *)pack->sec[GAL_SEC_EXPRS] + i;
        if ((uint64_t)e->off + e->len > pack->count[GAL_SEC_EXPR_DATA] || e->len == 0)
            FAIL("表达式范围越界");
    }
    for (uint32_t i = 0; i < vars; ++i) {
        const gal_var_t *v = (const gal_var_t *)pack->sec[GAL_SEC_VARS] + i;
        if (v->name >= pack->count[GAL_SEC_STR] || v->type > 2) FAIL("变量表非法");
    }
    for (uint32_t i = 0; i < poses; ++i) {
        const gal_pose_t *p = (const gal_pose_t *)pack->sec[GAL_SEC_POSES] + i;
        if (p->actor >= pack->count[GAL_SEC_STR] || p->appearance >= pack->count[GAL_SEC_STR] ||
            p->sub >= pack->count[GAL_SEC_STR]) FAIL("立绘姿态字符串非法");
        if (p->image != GAL_NONE && !image_ok(pack, p->image)) FAIL("立绘图片非法");
        if (p->default_image != GAL_NONE && !image_ok(pack, p->default_image)) FAIL("默认立绘非法");
    }
    for (uint32_t i = 0; i < chars; ++i) {
        const gal_char_t *c = (const gal_char_t *)pack->sec[GAL_SEC_CHAR_DETAILS] + i;
        if (c->pose >= poses) FAIL("立绘姿态索引非法");
        if (c->scene_x != GAL_NONE && c->scene_x >= exprs) FAIL("立绘场景表达式非法");
        if (c->scene_y != GAL_NONE && c->scene_y >= exprs) FAIL("立绘场景表达式非法");
        if (c->world_y != GAL_NONE && c->world_y >= exprs) FAIL("立绘位置表达式非法");
        if (c->scale != GAL_NONE && c->scale >= exprs) FAIL("立绘缩放表达式非法");
    }
    for (uint32_t i = 0; i < moves; ++i) {
        const gal_move_t *m = (const gal_move_t *)pack->sec[GAL_SEC_MOVES] + i;
        if ((uint32_t)m->first + m->count > pack->count[GAL_SEC_MOVE_ENTRIES]) FAIL("移动表越界");
    }
    for (uint32_t i = 0; i < pack->count[GAL_SEC_MOVE_ENTRIES]; ++i) {
        const gal_move_entry_t *m = (const gal_move_entry_t *)pack->sec[GAL_SEC_MOVE_ENTRIES] + i;
        if (m->actor >= pack->count[GAL_SEC_STR] || m->expr >= exprs || m->kind > 3)
            FAIL("移动项非法");
    }
    for (uint32_t i = 0; i < links; ++i) {
        const gal_link_t *l = (const gal_link_t *)pack->sec[GAL_SEC_LINKS] + i;
        if (l->scene >= scenes || l->cursor >= ((const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + l->scene)->op_count)
            FAIL("反驳目标越界");
        if (!text_range_ok(pack, l->text_off, l->text_len)) FAIL("反驳文本越界");
    }
    for (uint32_t i = 0; i < items; ++i) {
        const gal_item_t *it = (const gal_item_t *)pack->sec[GAL_SEC_ITEMS] + i;
        if (it->scene >= scenes) FAIL("选项场景非法");
        const uint32_t len = ((const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + it->scene)->op_count;
        if (it->to >= len || it->end > len) FAIL("选项目标越界");
        if (!text_range_ok(pack, it->text_off, it->text_len)) FAIL("选项文本越界");
        if (it->when != GAL_NONE && it->when >= exprs) FAIL("选项条件表达式非法");
    }
    for (uint32_t i = 0; i < link_ops; ++i) {
        const gal_link_op_t *lo = (const gal_link_op_t *)pack->sec[GAL_SEC_LINK_OPS] + i;
        if (lo->op_index >= ops || (uint32_t)lo->first + lo->count > links) FAIL("反驳表越界");
        if (lo->count > GAL_PACK_MAX_LINK_ROWS) FAIL("反驳选项超过界面容量");
        if (i && lo->op_index <= ((const gal_link_op_t *)pack->sec[GAL_SEC_LINK_OPS] + i - 1)->op_index)
            FAIL("反驳表未排序");
    }
    for (uint32_t i = 0; i < move_ops; ++i) {
        const gal_move_op_t *mo = (const gal_move_op_t *)pack->sec[GAL_SEC_MOVE_OPS] + i;
        if (mo->op_index >= ops || mo->move >= moves) FAIL("移动表越界");
        if (i && mo->op_index <= ((const gal_move_op_t *)pack->sec[GAL_SEC_MOVE_OPS] + i - 1)->op_index)
            FAIL("移动表未排序");
    }

    // 逐指令校验(需要所属场景长度)
    for (uint32_t si = 0; si < scenes; ++si) {
        const gal_scene_t *sc = (const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + si;
        for (uint32_t k = 0; k < sc->op_count; ++k) {
            const gal_op_t *o = (const gal_op_t *)pack->sec[GAL_SEC_OPS] + sc->op_start + k;
            switch (o->op) {
            case GAL_OP_TEXT:
                if (o->a >= pack->count[GAL_SEC_STR] || o->c >= pack->count[GAL_SEC_STR]) FAIL("对白字符串非法");
                if (!text_range_ok(pack, o->d, o->b)) FAIL("对白文本越界");
                break;
            case GAL_OP_CHAR:
                if (o->a >= chars) FAIL("立绘指令非法");
                break;
            case GAL_OP_BG:
                if (o->a >= pack->count[GAL_SEC_STR] || o->c >= pack->count[GAL_SEC_STR]) FAIL("背景字符串非法");
                if (!image_ok(pack, o->b)) FAIL("背景图片非法");
                break;
            case GAL_OP_FOCUS:
                if (o->a >= pack->count[GAL_SEC_STR]) FAIL("焦点字符串非法");
                break;
            case GAL_OP_CALL:
            case GAL_OP_JUMP:
                if (o->a >= scenes || o->b >= ((const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + o->a)->op_count)
                    FAIL("跳转目标越界");
                break;
            case GAL_OP_OFFER:
                if (o->a >= sc->op_count || o->b >= sc->op_count) FAIL("分支目标越界");
                if (o->d >= pack->count[GAL_SEC_STR]) FAIL("分支文本非法");
                break;
            case GAL_OP_VISIBILITY:
                if ((uint32_t)o->a + o->b > vis) FAIL("可见性表越界");
                for (uint32_t n = 0; n < o->b; ++n) {
                    const uint16_t *id = (const uint16_t *)pack->sec[GAL_SEC_VIS] + o->a + n;
                    if (*id >= pack->count[GAL_SEC_STR]) FAIL("可见性字符串非法");
                }
                break;
            case GAL_OP_CHOICE:
                if ((uint32_t)o->a + o->b > items) FAIL("选项表越界");
                if (o->b > GAL_PACK_MAX_CHOICE_ROWS) FAIL("选项行超过界面容量");
                break;
            case GAL_OP_IF:
                if (o->a >= exprs || o->b > sc->op_count) FAIL("条件跳转非法");
                break;
            case GAL_OP_SET:
                if (o->a >= vars || o->b >= exprs) FAIL("赋值指令非法");
                break;
            case GAL_OP_RESET_BOOK:
                if (o->a >= scenes || o->b >= ((const gal_scene_t *)pack->sec[GAL_SEC_SCENES] + o->a)->op_count)
                    FAIL("手册重置目标非法");
                break;
            case GAL_OP_CLUE_IMAGE:
                if (o->a != GAL_NONE && !image_ok(pack, o->a)) FAIL("线索图片非法");
                if (o->b >= pack->count[GAL_SEC_STR]) FAIL("线索字符串非法");
                break;
            case GAL_OP_EVIDENCE:
                if ((uint32_t)o->a + o->b > evcats) FAIL("证据类别越界");
                for (uint32_t n = 0; n < o->b; ++n) {
                    const uint8_t *cat = (const uint8_t *)pack->sec[GAL_SEC_EVIDENCE_CATS] + o->a + n;
                    if (*cat > GAL_CAT_MAP) FAIL("证据类别非法");
                }
                break;
            case GAL_OP_FLAG:
                if (o->a >= vars || o->b > 31) FAIL("标记指令非法");
                break;
            case GAL_OP_END:
            case GAL_OP_STOP:
                if (o->a >= pack->count[GAL_SEC_STR]) FAIL("结局字符串非法");
                break;
            case GAL_OP_MAP:
                if ((uint32_t)o->a + o->b > mapareas || o->c >= vars) FAIL("地图指令非法");
                if (o->b > GAL_PACK_MAX_MAP_ROWS) FAIL("地图行超过界面容量");
                for (uint32_t n = 0; n < o->b; ++n) {
                    const uint16_t *aid = (const uint16_t *)pack->sec[GAL_SEC_MAP_AREAS] + o->a + n;
                    if (*aid >= areas) FAIL("地图区域非法");
                }
                break;
            case GAL_OP_PAUSE:
                if (!text_range_ok(pack, o->d, o->b)) FAIL("暂停文本越界");
                break;
            case GAL_OP_DYNAMIC_JUMP:
                if (o->a >= pack->count[GAL_SEC_STR]) FAIL("动态跳转模板非法");
                break;
            case GAL_OP_BOOK_UPDATE:
                if (o->a >= pack->count[GAL_SEC_BOOKKEYS] || o->b > 255) FAIL("手册更新非法");
                break;
            case GAL_OP_RETURN:
            case GAL_OP_DAMAGE:
            case GAL_OP_CANCEL_CALL:
            case GAL_OP_AUTOSAVE:
            case GAL_OP_CLUE_HIDE:
            case GAL_OP_RECORD_CHOICE:
            case GAL_OP_BOOK_CLEAR:
            case GAL_OP_EXECUTION:
            case GAL_OP_HIDE_ALL:
                break;
            default:
                FAIL("未知剧情指令");
            }
        }
    }

    for (uint32_t i = 0; i < areas; ++i) {
        const gal_area_t *a = (const gal_area_t *)pack->sec[GAL_SEC_AREAS] + i;
        if (a->id >= pack->count[GAL_SEC_STR] || a->name >= pack->count[GAL_SEC_STR]) FAIL("区域字符串非法");
    }
    for (uint32_t i = 0; i < images; ++i) {
        const gal_imgrec_t *im = (const gal_imgrec_t *)pack->sec[GAL_SEC_IMAGES] + i;
        if (im->name >= pack->count[GAL_SEC_STR]) FAIL("图片名字符串非法");
        if (im->kind > GAL_IMG_PORTRAIT) FAIL("图片类型非法");
        if (im->gallery != GAL_NONE && im->gallery >= galitems) FAIL("图片鉴赏下标非法");
        if (im->data_len == 0 || (uint64_t)im->data_off + im->data_len > pack->len[GAL_SEC_IMGDATA]) FAIL("图片数据越界");
        if (im->aux_len && (uint64_t)im->aux_off + im->aux_len > pack->len[GAL_SEC_IMGDATA]) FAIL("图片条带越界");
        if (im->mask_len && (uint64_t)im->mask_off + im->mask_len > pack->len[GAL_SEC_IMGDATA]) FAIL("遮罩越界");
        if (im->w == 0 || im->h == 0 || im->w > 320 || im->h > 240) FAIL("图片尺寸非法");
        if (im->kind == GAL_IMG_FULL) {
            if (im->w != 320 || im->h != 150 || im->aux_len == 0) FAIL("整屏图片尺寸非法");
        } else if (im->kind == GAL_IMG_PORTRAIT) {
            if (im->w > 150 || im->h > 150) FAIL("立绘尺寸非法");
            const uint32_t stride = (im->w + 7u) / 8u;
            if (im->mask_len < stride * im->h) FAIL("立绘遮罩长度不足");
        } else {
            if (im->w != 320 || im->h != 150) FAIL("手册图片尺寸非法");
        }
    }
    for (uint32_t i = 0; i < books; ++i) {
        const gal_book_t *b = (const gal_book_t *)pack->sec[GAL_SEC_BOOK] + i;
        if (b->category > GAL_CAT_MAP || b->id >= pack->count[GAL_SEC_STR]) FAIL("手册条目非法");
        if (!text_range_ok(pack, b->name_off, b->name_len)) FAIL("手册名字越界");
        if (!text_range_ok(pack, b->text_off, b->text_len)) FAIL("手册正文越界");
        if (b->version == 0) FAIL("手册版本非法");
        if (b->image != GAL_NONE && !image_ok(pack, b->image)) FAIL("手册图片非法");
    }
    for (uint32_t i = 0; i < pack->count[GAL_SEC_BOOKKEYS]; ++i) {
        const gal_bookkey_t *k = (const gal_bookkey_t *)pack->sec[GAL_SEC_BOOKKEYS] + i;
        if (k->category > GAL_CAT_MAP || k->id >= pack->count[GAL_SEC_STR]) FAIL("手册键非法");
        if (!text_range_ok(pack, k->name_off, k->name_len)) FAIL("手册键名字越界");
        if ((uint32_t)k->first + k->count > books || k->count == 0) FAIL("手册键条目越界");
        for (uint32_t n = 0; n < k->count; ++n) {
            const gal_book_t *b = (const gal_book_t *)pack->sec[GAL_SEC_BOOK] + k->first + n;
            if (b->category != k->category || b->id != k->id) FAIL("手册键分组不符");
        }
    }
    for (uint32_t i = 0; i < groups; ++i) {
        const gal_gallery_group_t *g = (const gal_gallery_group_t *)pack->sec[GAL_SEC_GALLERY] + i;
        if (!text_range_ok(pack, g->label_off, g->label_len)) FAIL("鉴赏标签越界");
        if ((uint32_t)g->first + g->count > galitems || g->count == 0) FAIL("鉴赏分组越界");
    }
    for (uint32_t i = 0; i < galitems; ++i) {
        const gal_gallery_item_t *gi = (const gal_gallery_item_t *)pack->sec[GAL_SEC_GALLERY_ITEMS] + i;
        if (!image_ok(pack, gi->image) || gi->group >= groups) FAIL("鉴赏条目非法");
    }
    return true;
}

const char *gal_pack_str(const gal_pack_t *p, uint16_t id)
{
    if (!p || id >= p->count[GAL_SEC_STR]) return "";
    const uint32_t *offs = (const uint32_t *)p->sec[GAL_SEC_STROFF];
    return (const char *)p->sec[GAL_SEC_STR] + offs[id];
}

uint16_t gal_pack_move_for_op(const gal_pack_t *p, uint32_t op_index)
{
    if (!p || !p->sec[GAL_SEC_MOVE_OPS]) return GAL_NONE;
    uint32_t lo = 0, hi = p->count[GAL_SEC_MOVE_OPS];
    const gal_move_op_t *tab = (const gal_move_op_t *)p->sec[GAL_SEC_MOVE_OPS];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (tab[mid].op_index == op_index) return tab[mid].move;
        if (tab[mid].op_index < op_index) lo = mid + 1; else hi = mid;
    }
    return GAL_NONE;
}

const gal_link_op_t *gal_pack_link_for_op(const gal_pack_t *p, uint32_t op_index)
{
    if (!p || !p->sec[GAL_SEC_LINK_OPS]) return NULL;
    uint32_t lo = 0, hi = p->count[GAL_SEC_LINK_OPS];
    const gal_link_op_t *tab = (const gal_link_op_t *)p->sec[GAL_SEC_LINK_OPS];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (tab[mid].op_index == op_index) return &tab[mid];
        if (tab[mid].op_index < op_index) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

#define GETSEC(T, S, I) ((!p || (I) >= p->count[S]) ? NULL : (const T *)p->sec[S] + (I))

const gal_scene_t *gal_pack_scene(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_scene_t, GAL_SEC_SCENES, id); }
const gal_op_t *gal_pack_op(const gal_pack_t *p, uint32_t id) { return (!p || id >= p->count[GAL_SEC_OPS]) ? NULL : (const gal_op_t *)p->sec[GAL_SEC_OPS] + id; }
const gal_link_t *gal_pack_link(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_link_t, GAL_SEC_LINKS, id); }
const gal_item_t *gal_pack_item(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_item_t, GAL_SEC_ITEMS, id); }
const gal_pose_t *gal_pack_pose(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_pose_t, GAL_SEC_POSES, id); }
const gal_char_t *gal_pack_char(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_char_t, GAL_SEC_CHAR_DETAILS, id); }
const gal_move_t *gal_pack_move(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_move_t, GAL_SEC_MOVES, id); }
const gal_move_entry_t *gal_pack_move_entry(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_move_entry_t, GAL_SEC_MOVE_ENTRIES, id); }
const gal_area_t *gal_pack_area(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_area_t, GAL_SEC_AREAS, id); }
const gal_imgrec_t *gal_pack_image(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_imgrec_t, GAL_SEC_IMAGES, id); }
const gal_bookkey_t *gal_pack_bookkey(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_bookkey_t, GAL_SEC_BOOKKEYS, id); }
const gal_book_t *gal_pack_book(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_book_t, GAL_SEC_BOOK, id); }
const gal_gallery_group_t *gal_pack_gallery_group(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_gallery_group_t, GAL_SEC_GALLERY, id); }
const gal_gallery_item_t *gal_pack_gallery_item(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_gallery_item_t, GAL_SEC_GALLERY_ITEMS, id); }
const gal_var_t *gal_pack_var(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_var_t, GAL_SEC_VARS, id); }
const gal_link_op_t *gal_pack_link_op(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_link_op_t, GAL_SEC_LINK_OPS, id); }
const gal_move_op_t *gal_pack_move_op(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_move_op_t, GAL_SEC_MOVE_OPS, id); }
const gal_expr_t *gal_pack_expr(const gal_pack_t *p, uint16_t id) { return GETSEC(gal_expr_t, GAL_SEC_EXPRS, id); }

const uint8_t *gal_pack_expr_data(const gal_pack_t *p, uint16_t id, uint16_t *len)
{
    const gal_expr_t *e = gal_pack_expr(p, id);
    if (!e) { if (len) *len = 0; return NULL; }
    if (len) *len = e->len;
    return (const uint8_t *)p->sec[GAL_SEC_EXPR_DATA] + e->off;
}

int gal_pack_text_utf8(const gal_pack_t *p, uint32_t off, uint16_t len, char *out, size_t cap)
{
    // Contract: for cap > 0 every failure path leaves a NUL-terminated,
    // valid UTF-8 prefix; no caller may observe an unterminated buffer.
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!p || !text_range_ok(p, off, len)) return -1;
    const uint16_t *units = (const uint16_t *)p->sec[GAL_SEC_TEXTS];
    size_t at = 0;
    for (uint16_t i = 0; i < len; ++i) {
        uint32_t cp = units[off + i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= len) { out[at] = 0; return -1; }
            const uint32_t low = units[off + ++i];
            if (low < 0xDC00 || low > 0xDFFF) { out[at] = 0; return -1; }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            out[at] = 0;
            return -1;
        }
        const size_t need = cp < 0x80 ? 1u : cp < 0x800 ? 2u :
                            cp < 0x10000 ? 3u : 4u;
        if (at + need + 1 > cap) {
            out[at] = 0;
            return -1;
        }
        if (need == 1) {
            out[at++] = (char)cp;
        } else if (need == 2) {
            out[at++] = (char)(0xC0 | (cp >> 6));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        } else if (need == 3) {
            out[at++] = (char)(0xE0 | (cp >> 12));
            out[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[at++] = (char)(0xF0 | (cp >> 18));
            out[at++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[at] = 0;
    return (int)at;
}

int gal_pack_book_name_utf8(const gal_pack_t *p, const gal_book_t *b, char *out, size_t cap)
{
    if (!b) return -1;
    return gal_pack_text_utf8(p, b->name_off, b->name_len, out, cap);
}

int gal_pack_book_text_utf8(const gal_pack_t *p, const gal_book_t *b, char *out, size_t cap)
{
    if (!b) return -1;
    return gal_pack_text_utf8(p, b->text_off, b->text_len, out, cap);
}

const gal_book_t *gal_pack_book_find(const gal_pack_t *p, uint8_t category, uint16_t id_str,
                                     uint16_t version)
{
    if (!p || !p->sec[GAL_SEC_BOOKKEYS]) return NULL;
    for (uint32_t i = 0; i < p->count[GAL_SEC_BOOKKEYS]; ++i) {
        const gal_bookkey_t *k = (const gal_bookkey_t *)p->sec[GAL_SEC_BOOKKEYS] + i;
        if (k->category != category || k->id != id_str) continue;
        for (uint32_t n = 0; n < k->count; ++n) {
            const gal_book_t *b = (const gal_book_t *)p->sec[GAL_SEC_BOOK] + k->first + n;
            if (b->version == version) return b;
        }
        return NULL;
    }
    return NULL;
}
