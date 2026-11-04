// main/nn_save_codec.c —— 存档记录编解码（纯逻辑）。
#include "nn_save_codec.h"

#include <string.h>

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 定长记录布局（小端）：
//   0  magic u32 | 4 version u8 | 5 pad u8 | 6 payload_len u16
//   8  scene u16 | 10 op u16 | 12 bg u16 | 14 cg u16 | 16 sprite u16
//   18 ending_mask u8 | 19 pad u8 | 20 stamp u32 | 24 cg_mask u16
//   26 chapter[12] | 38 preview[28] | 66..71 pad
bool nn_save_encode(const nn_save_t *save, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!save || !out || cap < NN_SAVE_RECORD_BYTES) return false;
    uint8_t buf[NN_SAVE_RECORD_BYTES];
    memset(buf, 0, sizeof(buf));
    wr32(buf + 0, NN_SAVE_RECORD_MAGIC);
    buf[4] = (uint8_t)NN_SAVE_RECORD_VERSION;
    buf[5] = 0;
    wr16(buf + 6, (uint16_t)sizeof(nn_save_t));
    wr16(buf + 8, save->scene);
    wr16(buf + 10, save->op);
    wr16(buf + 12, save->bg);
    wr16(buf + 14, save->cg);
    wr16(buf + 16, save->sprite);
    buf[18] = save->ending_mask;
    buf[19] = 0;
    wr32(buf + 20, save->stamp);
    wr16(buf + 24, save->cg_mask);
    memcpy(buf + 26, save->chapter, sizeof(save->chapter));
    memcpy(buf + 38, save->preview, sizeof(save->preview));
    memcpy(out, buf, sizeof(buf));
    if (out_len) *out_len = sizeof(buf);
    return true;
}

bool nn_save_decode(const uint8_t *blob, size_t len, nn_save_t *out)
{
    if (!blob || !out || len != NN_SAVE_RECORD_BYTES) return false;
    if (rd32(blob + 0) != NN_SAVE_RECORD_MAGIC) return false;
    if (blob[4] != NN_SAVE_RECORD_VERSION) return false;
    if (rd16(blob + 6) != sizeof(nn_save_t)) return false;

    memset(out, 0, sizeof(*out));
    out->scene = rd16(blob + 8);
    out->op = rd16(blob + 10);
    out->bg = rd16(blob + 12);
    out->cg = rd16(blob + 14);
    out->sprite = rd16(blob + 16);
    out->ending_mask = blob[18];
    out->stamp = rd32(blob + 20);
    out->cg_mask = rd16(blob + 24);
    memcpy(out->chapter, blob + 26, sizeof(out->chapter));
    memcpy(out->preview, blob + 38, sizeof(out->preview));
    // 记录里的两条字符串可能没有结尾（旧数据/被写坏），解码后强制补上
    out->chapter[sizeof(out->chapter) - 1] = '\0';
    out->preview[sizeof(out->preview) - 1] = '\0';
    return true;
}

bool nn_save_record_fits_pack(const nn_save_t *save, const nn_pack_t *pack)
{
    if (!save || !pack) return false;
    if (save->scene >= pack->scene_count) return false;
    if (save->op > pack->scenes[save->scene].op_count) return false;
    if (save->bg != NN_NONE16) {
        const nn_image_t *img = nn_pack_image(pack, save->bg);
        if (!img) return false;
    }
    if (save->cg != NN_NONE16) {
        const nn_image_t *img = nn_pack_image(pack, save->cg);
        if (!img) return false;
    }
    if (save->sprite != NN_NONE16) {
        const nn_image_t *img = nn_pack_image(pack, save->sprite);
        if (!img) return false;
    }
    return true;
}
