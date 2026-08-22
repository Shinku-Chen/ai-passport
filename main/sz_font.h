// main/sz_font.h — 生字卡用到的两个自定义 LVGL 字体。
// sz_big: 212 个汉字, 56px, bpp2 (Flash ~388KB, RAM=0)
// sz_small: ASCII + 拼音声调字母, 20px, bpp4 (Flash ~41KB, RAM=0)
// 均由 tools/sz_gen/gen_font.sh 经 lv_font_conv 生成, 见 main/fonts/*.c
#pragma once

#include "lvgl.h"

extern const lv_font_t sz_big;
extern const lv_font_t sz_small;
