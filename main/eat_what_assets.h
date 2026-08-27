// main/eat_what_assets.h —— 由 tools/generate_eat_what_assets.py 生成,勿手改。
// 两份素材的 LVGL I8 索引帧数据,经 main/CMakeLists.txt 的 EMBED_FILES 嵌入 flash。
// 布局:每帧 = 256色调色板(1024B) + 像素索引(RES*RES 字节),连续拼接。
#pragma once

#define EAT_WHAT_RES           200
#define EAT_WHAT_FRAME_BYTES   41024   // 每帧字节 = 调色板 + 像素
#define EAT_WHAT_PALETTE_BYTES 1024

#define EAT_WHAT_G1_FRAMES     20            // 引导: 今天午餐要吃什么呢?
#define EAT_WHAT_G1_DELAY_MS   100             // 原速 10 fps
#define EAT_WHAT_G2_FRAMES     21            // 食物: 今天吃什么选择器
#define EAT_WHAT_G2_DELAY_MS   50              // 原速 20 fps

// ESP-IDF EMBED_FILES 生成的符号(指向 flash 中的 .rodata)。
extern const uint8_t _binary_eat_what_g1_bin_start[];
extern const uint8_t _binary_eat_what_g1_bin_end[];
extern const uint8_t _binary_eat_what_g2_bin_start[];
extern const uint8_t _binary_eat_what_g2_bin_end[];
