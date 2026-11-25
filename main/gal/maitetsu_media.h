// main/gal/maitetsu_media.h —— 媒体包的 LVGL 接入层。
//
// 场景 = 背景图 + 最多两个已经与背景合成好的补丁（透明与表情在打包时按源
// 层级合成）。图像描述符只指向包内数据；真正的像素由 maitetsu_pack 按
// 30 行条带解码，LVGL 拿到的永远是 ≤320x30 的 RGB565 缓冲。
//
// 生命周期：所有描述符与 maitetsu_pack_image() 返回的按图描述都是静态
// （描述符在 .bss、按图描述在生成的 Flash 常量表），在整个进程内有效。
#pragma once

#include "lvgl.h"
#include "maitetsu_assets.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 用给定的媒体包内容初始化（host 测试与内嵌二进制共用）。
// 必须在持有 BSP LVGL 锁时调用：它会注册 LVGL 图像解码器。
bool maitetsu_media_init_from(const uint8_t *data, size_t size);

// 固件入口：使用链接进来的 assets/maitetsu/maitetsu_media.bin。
bool maitetsu_media_init(void);

bool maitetsu_media_ready(void);
const char *maitetsu_media_error(void);

// 解码失败（CRC 不符、条带损坏）后置位；app 在 tick 里读取并切到可见错误。
bool maitetsu_media_faulted(void);
const char *maitetsu_media_fault_text(void);

// 图像描述符（下标来自媒体包）；非法下标返回 NULL。
const lv_image_dsc_t *maitetsu_media_image(uint16_t index);

const maitetsu_scene_t *maitetsu_media_scene(uint16_t scene);
const lv_image_dsc_t *maitetsu_media_scene_bg(uint16_t scene);
const lv_image_dsc_t *maitetsu_media_scene_patch(uint16_t scene, uint8_t patch);

const lv_image_dsc_t *maitetsu_media_cg(uint16_t cg);
const lv_image_dsc_t *maitetsu_media_cg_thumb(uint16_t cg);
const lv_image_dsc_t *maitetsu_media_title(void);

// 媒体包的场景/补丁/图像数量，供测试与日志核对。
uint16_t maitetsu_media_image_count(void);
uint16_t maitetsu_media_scene_count(void);
