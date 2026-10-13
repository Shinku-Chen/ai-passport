// main/gal/gal_content.h —— 框架骨架的占位内容。
//
// 骨架固件刻意不携带任何具体作品：这里只有一组演示用的占位对白与文案，
// 用来驱动启动页、标题页、阅读器、设置面板与关于页。接入真实作品时，
// 把本文件替换为内容层（或资源包读取层）即可。
#pragma once

typedef struct {
    const char *speaker;   // NULL 或空串 = 旁白
    const char *text;      // UTF-8 正文（一段 = 一屏，内部按行/页自动切分）
} gal_line_t;

#define GAL_CONTENT_TITLE    "GalGame 框架"
#define GAL_CONTENT_SUBTITLE "占位内容 · 骨架演示"

#define GAL_CONTENT_ABOUT \
    "GalGame 框架 · 骨架固件\n" \
    "AI Passport · 240 x 320\n" \
    "\n" \
    "本固件不含任何具体作品内容,只用来验证框架:\n" \
    "启动页、标题页、阅读器与设置面板。\n" \
    "\n" \
    "排版:对话框面板贴底、每页固定行数、字号两档\n" \
    "(16px / 20px),对话框透明度可在设置里调节。\n" \
    "\n" \
    "接入真实作品时,替换 main/gal/gal_content.h 与\n" \
    "内容层即可,界面与交互保持不变."

extern const gal_line_t gal_content_lines[];
extern const int gal_content_line_count;
