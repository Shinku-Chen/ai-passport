// main/gal/maitetsu_assets.h —— 生成文件，请勿手改。
#pragma once

#include <stdint.h>

#include "maitetsu_pack.h"

#define MAITETSU_SCENE_NONE 0xFFFFu

typedef struct {
    uint16_t bg;          // 媒体包图像下标，MAITETSU_SCENE_NONE = 纯黑
    uint8_t patch_count;  // 叠在背景上的补丁数
    uint16_t first_patch; // 本场景第一个补丁的下标
} maitetsu_scene_t;

typedef struct {
    uint16_t image;       // 已经与背景合成好的不透明补丁
    uint16_t x;
    uint16_t y;
} maitetsu_patch_t;

extern const maitetsu_scene_t maitetsu_scenes[];
extern const int maitetsu_scene_count;
extern const maitetsu_patch_t maitetsu_patches[];
extern const int maitetsu_patch_count;
extern const uint16_t maitetsu_cg_images[];
extern const uint16_t maitetsu_cg_thumbs[];
extern const int maitetsu_cg_count;
extern const uint16_t maitetsu_title_media;

// 稳定的按图描述（Flash 常量）：maitetsu_pack_image() 直接返回它的元素，
// 生命周期是整个进程，不存在“指向同一临时对象”的别名。
extern const maitetsu_image_t maitetsu_pack_images[];
extern const int maitetsu_pack_images_count;
