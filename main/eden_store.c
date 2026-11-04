// main/eden_store.c —— 持久化的纯逻辑层:记录编解码 + 校验 + 位操作。
//
// 这里没有一行 NVS 调用:读写通过 eden_store_io_t 走,固件绑定 eden_store_nvs.c 的适配器,
// 宿主机测试绑定假适配器来注入"提交失败/记录损坏"。这样测试覆盖的是同一份生产逻辑。
#include "eden_store.h"

#include <string.h>

// 默认适配器(固件由 eden_store_nvs.c 提供,宿主机测试由测试文件提供)。
extern const eden_store_io_t eden_store_nvs_io;

static const eden_store_io_t *s_io = &eden_store_nvs_io;

#define NS_SAVE "edensave"
#define NS_PROGRESS "edenprog"
#define NS_UNLOCK "edenulck"

// 头部版本 + 载荷。版本放在最前面,读的时候先比版本再看载荷。
typedef struct {
    uint16_t version;
    eden_slot_t slots[EDEN_SAVE_SLOTS];
} slot_blob_t;

typedef struct {
    uint16_t version;
    eden_progress_t progress;
} progress_blob_t;

typedef struct {
    uint16_t version;
    eden_unlock_t unlock;
} unlock_blob_t;

// 平台上结构体可能有填充;blob 长度以 sizeof 为准并在头文件里暴露给测试断言。
size_t eden_store_slots_blob_size(void)
{
    return sizeof(slot_blob_t);
}

size_t eden_store_progress_blob_size(void)
{
    return sizeof(progress_blob_t);
}

size_t eden_store_unlock_blob_size(void)
{
    return sizeof(unlock_blob_t);
}

void eden_store_bind(const eden_store_io_t *io)
{
    s_io = io != NULL ? io : &eden_store_nvs_io;
}

const eden_store_io_t *eden_store_current_io(void)
{
    return s_io;
}

void eden_slot_clear(eden_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

void eden_progress_clear(eden_progress_t *progress)
{
    if (progress != NULL) {
        memset(progress, 0, sizeof(*progress));
    }
}

void eden_unlock_clear(eden_unlock_t *unlock)
{
    if (unlock != NULL) {
        memset(unlock, 0, sizeof(*unlock));
    }
}

void eden_unlock_grant_cg(eden_unlock_t *unlock, uint16_t group)
{
    if (unlock == NULL || group >= EDEN_CG_GROUPS) {
        return;
    }
    unlock->cg[group / 8] = (uint8_t)(unlock->cg[group / 8] | (1u << (group % 8)));
}

bool eden_unlock_has_cg(const eden_unlock_t *unlock, uint16_t group)
{
    if (unlock == NULL || group >= EDEN_CG_GROUPS) {
        return false;
    }
    return (unlock->cg[group / 8] & (1u << (group % 8))) != 0;
}

void eden_unlock_grant_chapter(eden_unlock_t *unlock, uint16_t index)
{
    if (unlock != NULL && index < 32) {
        unlock->chapter |= 1u << index;
    }
}

bool eden_unlock_has_chapter(const eden_unlock_t *unlock, uint16_t index)
{
    return unlock != NULL && index < 32 && (unlock->chapter & (1u << index)) != 0;
}

void eden_unlock_grant_ending(eden_unlock_t *unlock, uint16_t index)
{
    if (unlock != NULL && index < 32) {
        unlock->ending |= 1u << index;
    }
}

bool eden_unlock_has_ending(const eden_unlock_t *unlock, uint16_t index)
{
    return unlock != NULL && index < 32 && (unlock->ending & (1u << index)) != 0;
}

// ---------------------------------------------------------------- 校验
// 字符串必须在容量内以 NUL 结尾:整块记录来自 NVS,可能被旧固件或损坏写坏。
static bool text_bounded(const char *text, size_t capacity)
{
    size_t index = 0;
    while (index < capacity && text[index] != '\0') {
        index++;
    }
    return index < capacity;
}

static bool slot_valid(const eden_slot_t *slot)
{
    if (slot->valid > 1) {
        return false;
    }
    if (slot->valid == 0) {
        return true;
    }
    if (slot->scene == 0 || slot->page >= EDEN_SLOT_PAGE_MAX) {
        return false;
    }
    if (!text_bounded(slot->chapter_text, sizeof(slot->chapter_text)) ||
        !text_bounded(slot->preview, sizeof(slot->preview))) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 编解码
bool eden_store_encode_slots(const eden_slot_t slots[EDEN_SAVE_SLOTS], uint8_t *buf, size_t size)
{
    if (slots == NULL || buf == NULL || size < sizeof(slot_blob_t)) {
        return false;
    }
    slot_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = (uint16_t)EDEN_SAVE_VERSION;
    memcpy(blob.slots, slots, sizeof(blob.slots));
    memcpy(buf, &blob, sizeof(blob));
    return true;
}

bool eden_store_decode_slots(const uint8_t *buf, size_t size, eden_slot_t slots[EDEN_SAVE_SLOTS])
{
    if (buf == NULL || slots == NULL || size != sizeof(slot_blob_t)) {
        return false;
    }
    slot_blob_t blob;
    memcpy(&blob, buf, sizeof(blob));
    if (blob.version != (uint16_t)EDEN_SAVE_VERSION) {
        return false;
    }
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        if (!slot_valid(&blob.slots[index])) {
            return false;
        }
    }
    memcpy(slots, blob.slots, sizeof(blob.slots));
    return true;
}

bool eden_store_encode_progress(const eden_progress_t *progress, uint8_t *buf, size_t size)
{
    if (progress == NULL || buf == NULL || size < sizeof(progress_blob_t)) {
        return false;
    }
    progress_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = (uint16_t)EDEN_PROGRESS_VERSION;
    blob.progress = *progress;
    memcpy(buf, &blob, sizeof(blob));
    return true;
}

bool eden_store_decode_progress(const uint8_t *buf, size_t size, eden_progress_t *progress)
{
    if (buf == NULL || progress == NULL || size != sizeof(progress_blob_t)) {
        return false;
    }
    progress_blob_t blob;
    memcpy(&blob, buf, sizeof(blob));
    if (blob.version != (uint16_t)EDEN_PROGRESS_VERSION || blob.progress.valid > 1) {
        return false;
    }
    if (blob.progress.valid != 0 &&
        (blob.progress.scene == 0 || blob.progress.page >= EDEN_SLOT_PAGE_MAX)) {
        return false;
    }
    *progress = blob.progress;
    return true;
}

bool eden_store_encode_unlock(const eden_unlock_t *unlock, uint8_t *buf, size_t size)
{
    if (unlock == NULL || buf == NULL || size < sizeof(unlock_blob_t)) {
        return false;
    }
    unlock_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = (uint16_t)EDEN_UNLOCK_VERSION;
    blob.unlock = *unlock;
    memcpy(buf, &blob, sizeof(blob));
    return true;
}

bool eden_store_decode_unlock(const uint8_t *buf, size_t size, eden_unlock_t *unlock)
{
    if (buf == NULL || unlock == NULL || size != sizeof(unlock_blob_t)) {
        return false;
    }
    unlock_blob_t blob;
    memcpy(&blob, buf, sizeof(blob));
    if (blob.version != (uint16_t)EDEN_UNLOCK_VERSION) {
        return false;
    }
    *unlock = blob.unlock;
    return true;
}

// ---------------------------------------------------------------- 存储
bool eden_store_load_slots(eden_slot_t slots[EDEN_SAVE_SLOTS])
{
    if (slots == NULL || s_io == NULL || s_io->read == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(slot_blob_t)];
    if (!s_io->read(NS_SAVE, "slots", buffer, sizeof(buffer))) {
        return false;
    }
    return eden_store_decode_slots(buffer, sizeof(buffer), slots);
}

bool eden_store_save_slots(const eden_slot_t slots[EDEN_SAVE_SLOTS])
{
    if (slots == NULL || s_io == NULL || s_io->write == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(slot_blob_t)];
    if (!eden_store_encode_slots(slots, buffer, sizeof(buffer))) {
        return false;
    }
    return s_io->write(NS_SAVE, "slots", buffer, sizeof(buffer));
}

bool eden_store_load_progress(eden_progress_t *progress)
{
    if (progress == NULL || s_io == NULL || s_io->read == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(progress_blob_t)];
    if (!s_io->read(NS_PROGRESS, "pos", buffer, sizeof(buffer))) {
        return false;
    }
    return eden_store_decode_progress(buffer, sizeof(buffer), progress);
}

bool eden_store_save_progress(const eden_progress_t *progress)
{
    if (progress == NULL || s_io == NULL || s_io->write == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(progress_blob_t)];
    if (!eden_store_encode_progress(progress, buffer, sizeof(buffer))) {
        return false;
    }
    return s_io->write(NS_PROGRESS, "pos", buffer, sizeof(buffer));
}

bool eden_store_load_unlock(eden_unlock_t *unlock)
{
    if (unlock == NULL || s_io == NULL || s_io->read == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(unlock_blob_t)];
    if (!s_io->read(NS_UNLOCK, "bits", buffer, sizeof(buffer))) {
        return false;
    }
    return eden_store_decode_unlock(buffer, sizeof(buffer), unlock);
}

bool eden_store_save_unlock(const eden_unlock_t *unlock)
{
    if (unlock == NULL || s_io == NULL || s_io->write == NULL) {
        return false;
    }
    uint8_t buffer[sizeof(unlock_blob_t)];
    if (!eden_store_encode_unlock(unlock, buffer, sizeof(buffer))) {
        return false;
    }
    return s_io->write(NS_UNLOCK, "bits", buffer, sizeof(buffer));
}
