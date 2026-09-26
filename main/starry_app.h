// main/starry_app.h —— 应用状态机:页面切换、按键语义、自动存档、电量刷新。
//
// 按键语义(三键):
//   标题/列表页: 上/下 移动光标,确定 进入,长按确定 返回
//   正文页:     确定 推进(打字中=立即显示全文),长按确定 打开菜单,上/下 翻页
//   选项页:     上/下 选择,确定 确认;长按确定 打开菜单
#pragma once

#include "bsp_button.h"
#include "starry_render.h"
#include "starry_save.h"

#include <stdbool.h>
#include <stdint.h>

#define STARRY_TITLE_ROWS 4
#define STARRY_MENU_ROWS 5
#define STARRY_SETTING_ROWS 5
#define STARRY_NAME_MAX 96

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} starry_key_t;

typedef enum {
    STARRY_PAGE_TIPS = 0,   // 首次启动的操作提示
    STARRY_PAGE_TITLE,      // 标题:标题画面 + 菜单
    STARRY_PAGE_READING,    // 阅读:画面 + 文本框
    STARRY_PAGE_CHOICES,    // 选项
    STARRY_PAGE_MENU,       // 游戏内菜单
    STARRY_PAGE_SETTINGS,   // 设置
    STARRY_PAGE_SLOTS,      // 存档 / 读档位
    STARRY_PAGE_ABOUT,      // 关于
    STARRY_PAGE_CHAPTER,    // 章节过场卡
    STARRY_PAGE_ENDING,     // 结局
} starry_page_t;

typedef struct {
    starry_pack_t pack;
    starry_player_t player;
    starry_font_t font_small;   // 字形包由应用持有,渲染器只引用
    starry_font_t font_large;
    starry_render_t render;
    starry_settings_t settings;

    starry_page_t page;
    starry_page_t return_page;   // 设置/存档从哪一页进来
    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;
    int choice_sel;

    // 打字机:当前页正文 + 已经显示到第几个字节
    char text[STARRY_TEXT_BUFFER];
    size_t text_total;
    size_t text_visible;
    uint32_t type_accum_ms;
    bool text_complete;

    // 上次画过的状态:说话人名牌与电量只在变化时重绘
    char drawn_name[STARRY_NAME_MAX];
    int drawn_battery;
    bool box_dirty;              // 从整页界面回到阅读页,文本框要整体重画

    bool fast_forward;           // 长按上:按住期间快进
    uint32_t ff_accum_ms;
    bool auto_mode;              // 长按下:自动阅读(每 STARRY_AUTO_LINE_MS 推进一句)
    uint32_t auto_accum_ms;
    uint16_t drawn_chapter;      // 左上角章节标签已经画过的章节 id

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;
    uint32_t notice_ms;
    char notice[32];
    bool have_auto;              // 存在自动续读存档
    uint16_t chapter_id;         // 当前章节的源脚本编号
    uint16_t chapter_index;
} starry_app_t;

typedef struct {
    uint16_t *sprite;
    uint16_t *box;
    uint16_t *strip;
    uint16_t *hold;
    uint16_t *name_bg;      // STARRY_SCREEN_W × STARRY_NAME_AREA_H
    uint16_t *chapter_bg;   // STARRY_CHAPTER_W × STARRY_CHAPTER_H
    uint8_t *jpeg_work;
    uint32_t jpeg_work_size;
} starry_app_buffers_t;

// 初始化:打开资源包与 NVS、建渲染器、进入首个页面(首次启动是提示页)。
bool starry_app_init(starry_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                     const uint8_t *font_small, uint32_t font_small_size,
                     const uint8_t *font_large, uint32_t font_large_size,
                     const starry_app_buffers_t *buffers);

// 字号对应的排版参数(行宽单位数 / 每屏行数)。
starry_layout_t starry_layout_for(const starry_app_t *app);

// 处理一次按键(在输入任务里调用)。
void starry_app_key(starry_app_t *app, const starry_key_t *key);

// 周期性 tick:打字机推进、电量刷新间隔。
void starry_app_tick(starry_app_t *app, uint32_t elapsed_ms);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t starry_app_idle_ms(const starry_app_t *app);

// 进入深睡前保存进度。
void starry_app_before_sleep(starry_app_t *app);
