// main/gal_save.h —— 存档编码/解码(纯 C,不依赖 ESP-IDF/NVS)。
//
// 存档只保存“恢复剧情所需的状态”和少量 UI 位置;文本由当前指令重新解出,
// 不复制台词。解码先写入调用方给的临时 VM,校验全部通过后由调用方整体替换,
// 因此坏档/CRC 不符不会改动当前有效状态。
#pragma once

#include "gal_vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAL_SAVE_MAX 3824   // measured worst real state; NVS still 31 slots

typedef struct {
    uint8_t scene_mode;   // 0=正文, 1=普通选项页, 2=反驳/link 选择页
    uint8_t page;         // 当前文本页
    uint8_t choice_row;   // 选项页光标
    uint8_t _pad;
} gal_save_ui_t;

// 编码当前 VM 与 UI 状态。成功返回 true 并写 out_len。
bool gal_save_encode(const gal_vm_t *vm, const gal_save_ui_t *ui,
                     uint8_t *out, size_t cap, size_t *out_len);

// 解码到临时 VM。vm_out 必须先 gal_vm_init() 且与 pack 绑定。
// 失败返回 false,vm_out 内容无意义;成功时填充 vm_out 与 ui_out。
bool gal_save_decode(const uint8_t *data, size_t len, const gal_pack_t *pack,
                     gal_vm_t *vm_out, gal_save_ui_t *ui_out, const char **err);

// 从已解码的 VM/UI 生成一行预览(不二次解码)。
bool gal_save_preview_vm(const gal_pack_t *pack, const gal_vm_t *vm,
                         const gal_save_ui_t *ui, char *out, size_t cap);
// 解码到调用方提供的 scratch 后生成预览;scratch 由宿主/应用复用。
bool gal_save_preview(const gal_pack_t *pack, const uint8_t *data, size_t len,
                      gal_vm_t *scratch, char *out, size_t cap);
