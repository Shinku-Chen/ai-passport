// main/starry_render.h —— 直推面板的逐条带合成器。
//
// 版面(竖屏 240x320,与 tools/starry_pack.py 的常量一一对应):
//
//   y   0..213  画面区:背景 JPEG 铺满整屏 + 立绘(JPEG + 1bpp 遮罩)
//   y 214..319  文本框:压在画面上的半透明底板(源版面的文本框同样压在画面上)
//   y 118..213  立绘可见带(打包时就裁好,设备端不再缩放)
//   右上角      电量胶囊(画面区上,自带小块底图缓冲,刷新只重绘这一小块)
//
// 为什么不用 LVGL:整屏 240x320 RGB565 = 150KB,这块板子没有 PSRAM,静态缓冲区
// 按面板几何算下来放不下(见 docs/reference/shinku-chen/release-artifact-verification.md)。
// 这里改成"一行条带一行条带地画":背景解码 -> 立绘合成 -> 文本框 -> 圆角遮罩 ->
// 大端 RGB565 推给 esp_lcd_panel_draw_bitmap。所有绘图原语在 starry_gfx.c(纯逻辑),
// 本文件只管缓冲、JPEG 与推屏。
#pragma once

#include "starry_font.h"
#include "starry_model.h"
#include "starry_gfx.h"
#include "starry_pack.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>

#define STARRY_SCREEN_W 240
#define STARRY_SCREEN_H 320
#define STARRY_BOX_Y 214
#define STARRY_BOX_H (STARRY_SCREEN_H - STARRY_BOX_Y)         // 106
// 立绘缓冲上限:打包器(tools/starry_pack.py 的 SPRITE_MAX_W/H)把立绘裁到不透明边界后
// 等比缩放到这个框内,贴右边、底边贴屏幕底 —— 下半身会落在半透明文本框下面,而不是被截断。
// 改这里必须同步改打包器的两个常量(tests/test_starry_app_static.py 会核对)。
#define STARRY_SPRITE_MAX_W 168
#define STARRY_SPRITE_H 252
// 条带高度:整屏重绘时每笔 SPI 传输的行数。BSP 的 max_transfer_sz 是 240x80x2,
// 所以 80 行是单笔上限 —— 取满把整屏重绘的"接缝"从 8 处减到 4 处(接缝会表现为
// 一条细噪声线)。配合 push_strip() 里的"每笔等完成",行为与 esp_lvgl_port 一致。
#define STARRY_STRIP_ROWS 80
// 左右边距 16:正好等于 13 个全角字(26 单位 x 16px = 208px),正文/列表/章节标签全部左对齐。
#define STARRY_TEXT_X 16
#define STARRY_TEXT_W (STARRY_SCREEN_W - 2 * STARRY_TEXT_X)   // 208
// 说话人名字放在【文本框外面的画面区】左下角,不挤占正文空间,也不遮文字。
#define STARRY_NAME_AREA_H 24
#define STARRY_NAME_Y (STARRY_BOX_Y - 30)                     // 184:画面区底部
#define STARRY_NAME_X STARRY_TEXT_X
#define STARRY_TEXT_Y (STARRY_BOX_Y + 8)                      // 222:文本框内正文起点
#define STARRY_BODY_MARGIN_BOTTOM 4
// 正文行距:16px 字号 19px(5 行),20px 字号 24px(4 行),都落在文本框里且留出边距。
#define STARRY_SMALL_LINE_PITCH 19
#define STARRY_LARGE_LINE_PITCH 24
// 左上角章节标签(画面区)。底板是【整行宽】的:标签只占 x=CHAPTER_X 起的 120px,
// 但刷新时整行都要从底板恢复,否则同一行两侧会留下上次推屏的残影。
#define STARRY_CHAPTER_X STARRY_TEXT_X
#define STARRY_CHAPTER_Y 6
#define STARRY_CHAPTER_TEXT_W 120
#define STARRY_CHAPTER_H 22
#define STARRY_BATTERY_X 182
#define STARRY_BATTERY_Y 6
#define STARRY_BATTERY_W 52
#define STARRY_BATTERY_H 20
#define STARRY_RADIUS 30        // 与 components/bsp 的 BSP_LVGL_SCREEN_RADIUS 一致
#define STARRY_LIST_MAX_ROWS 8
// 文本框内的列表(标题菜单 / 游戏内菜单):按条数平分可用高度,最多 24px 一行、
// 最多 5 项(游戏内菜单 = 保存/读取/跳过章节/返回标题/返回)。
#define STARRY_BOX_LIST_ROWS 5
#define STARRY_BOX_LIST_ROW_H 24
#define STARRY_BOX_LIST_TOP (STARRY_BOX_Y + 4)

// 排版参数(行宽单位数 / 每屏行数)定义在 main/starry_model.h:那是分页的契约,
// 这里只关心像素几何 —— 单位到像素的换算见 starry_gfx.c(半角单位 = 字号/2)。

// 配色沿用 ATRI 移植版的样式表(源工程 index.ux/detail.ux:深蓝 + 天蓝 + 近白字)。
// 括号里是 RGB888 原值,便于对照。
#define STARRY_COL_ACCENT 0x561Cu     // #50C0E7 天蓝:选中行/选项底色、描边
#define STARRY_COL_DEEP 0x0AB7u       // #0A55BC 深蓝
#define STARRY_COL_BOX 0x0883u        // #08131F 文本框兜底色
#define STARRY_COL_PANEL 0x0062u      // #050C14 整页底色
#define STARRY_COL_ROW 0x11EBu        // #123C5E 列表行底色
#define STARRY_COL_ROW_SEL STARRY_COL_ACCENT
#define STARRY_COL_TEXT 0xF7BFu       // #F2F6FA 正文
#define STARRY_COL_NAME 0xF7BFu       // 说话人(名字用正文白,靠底板压出层次)
#define STARRY_COL_DIM 0x8D9Au        // #8FB3D0 次要文字
#define STARRY_COL_DARK 0x0083u       // #06121E 选中行上的深色字
#define STARRY_COL_BLACK 0x0000u
#define STARRY_COL_PLATE 0x0883u      // 名字底板(RGB565 = #08131F)
#define STARRY_PLATE_ALPHA 190
#define STARRY_PLATE_PAD 8            // 角色名底板左右各留的边距

// 正文带:ATRI 版把它做成"蓝色渐变 + 立绘压在带子上面 + 只给文字几行盖淡暗帘"。
// 带子从 STARRY_BOX_Y - STARRY_BOX_LEAD_IN 开始渐入,避免出现硬边。
#define STARRY_BOX_LEAD_IN 26
#define STARRY_BAND_R 73              // 源工程 text_bg 采样色 #498AD9
#define STARRY_BAND_G 138
#define STARRY_BAND_B 217
#define STARRY_BAND_ALPHA_TOP 110     // 带顶不透明度
#define STARRY_BAND_ALPHA_BOTTOM 232  // 带底不透明度
#define STARRY_SCRIM_R 6              // 文字区浅暗帘 #06101９
#define STARRY_SCRIM_G 16
#define STARRY_SCRIM_B 25
#define STARRY_SCRIM_ALPHA_TOP 30
#define STARRY_SCRIM_ALPHA_BOTTOM 110

#define STARRY_ROW_SEL_ALPHA 255      // 选中行/选项:天蓝实底 + 深色字
#define STARRY_ROW_ALPHA 180          // 未选中行底色强度(#123C5E 约 70%)
#define STARRY_BATTERY_NONE (-1000)   // 不在"解码时一起画"的浮层里画电量
#define STARRY_MENU_DIM_ALPHA 96      // 菜单/选项页在原带子上再压一点,让行更清楚

typedef struct {
    uint16_t *sprite;      // STARRY_SPRITE_MAX_W × STARRY_SPRITE_H
    uint16_t *box;         // 218..320 行(102 行)× STARRY_SCREEN_W
    uint16_t *strip;       // STARRY_STRIP_ROWS × STARRY_SCREEN_W,推屏用
    // 扣住的首条带(0..STARRY_STRIP_ROWS 行)。章节/电量在第一段、正文在最后一段,
    // 直接按解码顺序推屏会让两个角比正文早约 120ms 变化;扣住它、等解码完最后推,
    // 文字就一起上屏了。
    uint16_t *hold;
    uint16_t *name_bg;     // STARRY_SCREEN_W × STARRY_NAME_AREA_H,名字区画面底图
    uint16_t *chapter_bg;  // STARRY_SCREEN_W × STARRY_CHAPTER_H,章节标签那几行的整行底图
    uint8_t *jpeg_work;    // ROM TJpgDec 工作区(esp_jpeg 用 3100B,这里给足)
    uint32_t jpeg_work_size;
} starry_render_buffers_t;

typedef struct {
    uint32_t bytes;   // 缓冲字节数,便于自检
    uint32_t pixels;
} starry_render_plan_t;

// 计算所需缓冲字节数(供 main 静态分配与自检;不依赖任何硬件)。
void starry_render_plan(starry_render_plan_t *plan);

typedef struct {
    const starry_pack_t *pack;
    const starry_font_t *font_small;
    const starry_font_t *font_large;
    starry_render_buffers_t buf;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;   // 用于注册"传输完成"回调(每笔推屏等完成)
    SemaphoreHandle_t trans_done;
    uint16_t shown_bg;       // 当前背景(去重,避免重复解码)
    uint16_t shown_sprite;
    uint16_t decoded_sprite; // 立绘缓冲里那张的编号
    bool backdrops_valid;    // 名字/章节/文本框这几块底图是否已经填过内容
    int hold_rows;           // >0 表示首条带正扣在 buf.hold 里,等解码结束再推
    // 当前该显示的文字浮层。整屏重画(解码背景)时要把它们直接画进条带,否则文字会
    // "先消失、等解码完再出现",看起来就是闪一下。
    char overlay_chapter[24];
    char overlay_name[STARRY_TEXT_BUFFER];
    uint16_t overlay_name_color;
    int overlay_battery;     // 当前电量百分比(STARRY_BATTERY_NONE = 不画)
    bool overlay_auto;       // 自动阅读中:画面区右下角常驻一个"自动"标签
    char overlay_body[STARRY_TEXT_BUFFER];
    size_t overlay_body_visible;
    const starry_font_t *overlay_body_font;
    int overlay_body_units;
    uint32_t last_decode_ms;
} starry_render_t;

// 一行的内容:左侧文字 + 右侧取值 + 是否置灰。
typedef struct {
    const char *label;
    const char *value;
    uint8_t dim;
} starry_row_t;

bool starry_render_init(starry_render_t *r, const starry_pack_t *pack,
                        esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                        const starry_font_t *font_small, const starry_font_t *font_large,
                        const starry_render_buffers_t *buffers);

// 画一整屏:背景 + 立绘 + 文本框底板。背景/立绘与当前显示相同则直接返回 false。
bool starry_render_scene(starry_render_t *r, uint16_t bg, uint16_t sprite);

// 说话人:底板上画一块名牌 + 名字(只在改名时重绘)。
void starry_render_name(starry_render_t *r, const char *name, const char *const previous,
                        const starry_font_t *font);
// 自动阅读指示(常驻浮层,随整屏重画一起上屏)。
void starry_render_auto(starry_render_t *r, bool on);

// 在同一个位置画任意一行带底板的文字(选项提问用);空串 = 擦掉。
void starry_render_plate(starry_render_t *r, const char *text, uint16_t color,
                         const starry_font_t *font);
// 正文:按 visible_bytes 逐字显示(打字机);visible_bytes 超过文本长度即全显。
void starry_render_body(starry_render_t *r, const char *text, size_t visible_bytes,
                        const starry_font_t *font, int units_per_line);
// 选项页:提示文本 + 两个选项(选项页盖住正文区)。
void starry_render_choices(starry_render_t *r, const char *prompt, const char *const *choices,
                           int count, int selected, const starry_font_t *font);
// 只登记当前章节标签、不重画:换章时要在解码背景之前调用,这样整屏重画直接带上新章号,
// 不会出现"先闪一下旧章号、再改成新章号"。
void starry_render_chapter_set(starry_render_t *r, const char *text);
// 章节标签(画面区左上角),登记 + 立即重画那一条;text 传空串 = 擦掉标签。
void starry_render_chapter(starry_render_t *r, const char *text, const starry_font_t *font);
// 把"当前该显示的文字浮层"画到指定表面上(章节 + 角色名 + 正文)。
// 表面会按自己的行范围裁剪,所以整屏解码时每个条带调一次即可。
void starry_render_overlays(starry_render_t *r, starry_surface_t *surface);

// 把画面区上的浮层(角色名、章节标签)擦回背景;离开阅读页时必须调,否则它们会留在画面上。
void starry_render_clear_overlays(starry_render_t *r);
// 用文本框底板重刷整个文本框区域(214..320)。离开菜单/进入正文页时用,避免行条残留。
void starry_render_box_clear(starry_render_t *r);
// 电量胶囊(画面区右上角);percent < 0 表示读数不可用。
void starry_render_battery(starry_render_t *r, int percent);

// 正文带在 y 行的不透明度 / 文字区浅暗帘在 y 行的不透明度(0..255)。
uint8_t starry_band_alpha_at(int y);
uint8_t starry_scrim_alpha_at(int y);
// 整屏纯色(过场/结局/首次提示页的底)。
void starry_render_fill(starry_render_t *r, uint16_t color);
// 整屏纯色 + 若干行列表(设置/存档/关于页)。selected < 0 表示无选中行。
void starry_render_full_list(starry_render_t *r, const char *title, const starry_row_t *rows,
                             int count, int selected, const starry_font_t *font, uint16_t bg);
// 在文本框区域内画列表(标题页菜单、游戏内菜单),底图用文本框底板。
void starry_render_box_list(starry_render_t *r, const starry_row_t *rows, int count, int selected,
                            const starry_font_t *font);
// 整屏纯色 + 居中文字(章节过场卡、结局、提示)。
void starry_render_center(starry_render_t *r, const char *title, const char *subtitle,
                          const starry_font_t *font, uint16_t bg);
// 状态提示条("已保存"/"这一场景无法跳过")。
// over_box=true 画在文本框底板上(菜单/标题页),false 用纯色底(整页列表页)。
void starry_render_notice(starry_render_t *r, const char *text, const starry_font_t *font,
                          bool over_box);
// 整页界面上的电量胶囊:底板是纯色而不是画面。
void starry_render_battery_solid(starry_render_t *r, int percent, uint16_t bg);
