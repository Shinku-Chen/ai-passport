// main/demo_eat_what.c —— 「今天吃啥」:按下任意键开始滚动,松开定格在结果上。
//
// 交互约定(本页自定义,不与菜单冲突):
//   按下任意键(UP / DOWN / OK) = 抽签:随机选一种类型,再从该类型的随机一帧开始滚动
//   松开                       = 停在最后一次显示的帧上(即结果)
//   长按 OK                    = 切换渲染路径(LVGL 局部刷新 / 直刷 panel),上板 A/B 用
//
// 两种类型(素材各是一段帧序列,画面里自带菜名/店名):
//   类型 0「PLACE」= 去处盘(麦当劳/玻璃屋/热炒/寿司…,10 fps)
//   类型 1「DISH」 = 菜品盘(沙县小吃/兰州拉面/汉堡/火锅…,20 fps)
// 即「两种类型的图片随机展示」:每次抽签先随机决定类型,再在该类型里滚动。
//
// 实现要点:
//   - 素材以 LVGL LV_COLOR_FORMAT_I8 索引色二进制经 CMake EMBED_FILES 嵌入 flash,
//     见 main/eat_what_g1.bin / eat_what_g2.bin(由 tools/generate_eat_what_assets.py 生成)。
//   - 每帧 = 256 色调色板(1024B) + 像素索引(RES*RES B) ;各帧连续拼接,帧 i 的
//     数据偏移 = i * EAT_WHAT_FRAME_BYTES。
//   - 按键回调跑在 button 组件的任务里,不能阻塞;播放循环用 lv_timer 跑在 LVGL 任务
//     (天然持锁)。检测「松开」不依赖 RELEASE 事件(本 BSP 无),而是轮询
//     bsp_button_read_mv():松开态约 3300mV,按住约 <2000mV。
//   - 随机数用 esp_random()(硬件 RNG),抽类型/抽起始帧各取一次,互不相关。
//   - C3 无 PSRAM,只保留 2~3 个懒构建的 lv_image_dsc_t(每帧一个,常驻约 40 字节),
//     图像数据本身在 flash,不占 RAM。
#include "demo.h"
#include "bsp_button.h"
#include "bsp_display.h"         // 统一屏显接口
#include "bsp_pins.h"            // BSP_LCD_W / 分辨率宏来源
#include "bsp_battery.h"         // CW2017 电量计:右上角电量显示
#include "ui_pixel.h"
#include "ui_eatwhat_math.h"
#include "ui_eatwhat_render.h"   // 候选快速刷新(局部+隔行),供上板 A/B
#include "eat_what_assets.h"
#include "autopower.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_random.h"          // esp_random():抽签用的硬件随机数

#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "demo_eatwhat";

// 两段素材(两种抽签类型)的元信息。
typedef struct {
    const char        *name;           // 日志用中文名
    const char        *label;          // 提示条用 ASCII 标签(LVGL 只有西文字库)
    uint32_t           frames;
    uint32_t           delay_ms;       // 每帧显示时长
    const uint8_t     *data;           // 指向 EMBED_FILES 生成符号
} eat_what_anim_t;

// 索引必须与 ui_eatwhat_math.h 的 EATWHAT_TYPE_COUNT / ui_eatwhat_pick_type() 对齐。
static const eat_what_anim_t s_anims[EATWHAT_TYPE_COUNT] = {
    { .name = "去处盘", .label = "PLACE", .frames = EAT_WHAT_G1_FRAMES,
      .delay_ms = EAT_WHAT_G1_DELAY_MS,
      .data = _binary_eat_what_g1_bin_start },
    { .name = "菜品盘", .label = "DISH", .frames = EAT_WHAT_G2_FRAMES,
      .delay_ms = EAT_WHAT_G2_DELAY_MS,
      .data = _binary_eat_what_g2_bin_start },
};

static lv_obj_t            *s_scr;
static lv_obj_t            *s_img;
static lv_obj_t            *s_badge;      // 底部提示条(panel 容器)
static lv_obj_t            *s_badge_label;  // 提示条里的 label(真正设文本的对象)
static lv_timer_t          *s_timer;
static lv_obj_t            *s_batt_label;   // 右上角电量文本
static lv_timer_t          *s_batt_timer;   // 电量刷新定时器
static lv_image_dsc_t       s_dsc;        // 当前帧描述符(懒复用,不每帧分配字段)
static int                  s_cur_anim;   // 当前播放的素材索引;-1 = 未播放
static int                  s_cur_frame;  // 当前显示的帧号
static bool                 s_playing;    // 是否正在循环播放
// 候选快速刷新开关:false=LVGL 原路径(默认); true=eatwhat_render 局部+隔行直刷。
// 进页默认关,由 OK 单击在两种路径间切换,便于上板对比速度与撕裂。
static bool                 s_fast_render;

// 图片矩形原点(LVGL 布局与实际 panel 直画共用):居中 240 宽,Y 略下移让出标题,底部留出提示条。
static eatwhat_rect_origin_t s_img_origin() {
    eatwhat_rect_origin_t o = { (240 - EAT_WHAT_RES) / 2, 48 };
    return o;
}

// 从 flash 数据里取第 frame 帧,填进 s_dsc。图像宽度/高度都是 EAT_WHAT_RES。
static const uint8_t *frame_ptr(const eat_what_anim_t *anim, uint32_t frame)
{
    return anim->data + (size_t)frame * EAT_WHAT_FRAME_BYTES;
}

static void show_frame(int anim_idx, uint32_t frame)
{
    const eat_what_anim_t *anim = &s_anims[anim_idx];
    const uint8_t *data = frame_ptr(anim, frame);

    if (s_fast_render) {
        // 候选快速路径:绕过 LVGL,直接用 panel 只刷图片矩形,并隔行两趟。
        // 每趟只有一半像素 → 单趟 SPI 时间减半,但两趟之间有半帧撕裂。
        eatwhat_render_draw_frame(data, s_img_origin(), true);
        eatwhat_render_finish_odd(data, s_img_origin());
    } else {
        // LVGL 原路径:图片 obj 局部刷新(PARTIAL 模式只重绘图片区)。
        s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf     = LV_COLOR_FORMAT_I8;
        s_dsc.header.flags  = 0;
        s_dsc.header.w      = EAT_WHAT_RES;
        s_dsc.header.h      = EAT_WHAT_RES;
        s_dsc.header.stride = EAT_WHAT_RES;      // 每行字节数 = 每像素 1 字节索引
        s_dsc.header.reserved_2 = 0;
        s_dsc.data_size     = EAT_WHAT_FRAME_BYTES;
        s_dsc.data          = data;
        lv_image_set_src(s_img, &s_dsc);
        lv_obj_invalidate(s_img);
    }

    s_cur_anim  = anim_idx;
    s_cur_frame = (int)frame;
}

// 刷新底部提示条。两行文本,字体是 montserrat 14(仅西文),故文案用 ASCII。
static void show_badge(const char *line1, const char *line2)
{
    if (s_badge_label) lv_label_set_text_fmt(s_badge_label, "%s\n%s", line1, line2);
}

// 播放定时器:先轮询 ADC 判断按键是否已松开,未松开才推进一帧。
// 跑在 LVGL 任务,已在锁内,可直接操作 LVGL 对象。
static void on_tick(lv_timer_t *t)
{
    (void)t;
    // 松开检测优先:这样停住的就是用户最后一次看到的帧,而不是「再跳一格」后的帧。
    // 按住某键时 ADC 约 <2000mV,松开约 3300mV。
    int mv = bsp_button_read_mv();
    if (ui_eatwhat_is_released(mv)) {
        const eat_what_anim_t *anim = &s_anims[s_cur_anim];
        s_playing = false;
        if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
        show_badge("RESULT", anim->label);
        ESP_LOGI(TAG, "松手定格:%s 第 %d 帧(%dmV)", anim->name, s_cur_frame, mv);
        return;
    }

    autopower_notify_activity();       // 仍按住播放,算活动,避免自动关机打断
    const eat_what_anim_t *anim = &s_anims[s_cur_anim];
    show_frame(s_cur_anim, ui_eatwhat_next_frame((uint32_t)s_cur_frame, anim->frames));
}

// 开始一轮抽签:anim_idx 已由调用方随机定好,这里再随机决定从哪一帧起步。
static void start_play(int anim_idx)
{
    const eat_what_anim_t *anim = &s_anims[anim_idx];
    uint32_t first = ui_eatwhat_start_frame(esp_random(), anim->frames);
    ESP_LOGI(TAG, "抽签:%s(%u 帧,每帧 %u ms),从第 %u 帧开始滚动",
             anim->name, anim->frames, anim->delay_ms, first);

    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    s_playing = true;
    show_frame(anim_idx, first);
    show_badge("ROLLING...", anim->label);
    s_timer = lv_timer_create(on_tick, anim->delay_ms, NULL);
}

// 刷新右上角电量显示。lv_timer 跑在 LVGL 任务,已持锁。
static void battery_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_batt_label) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_batt_label, "-- %");
        lv_obj_set_style_text_color(s_batt_label, lv_color_hex(0x94A3B8), 0);
    } else {
        lv_label_set_text_fmt(s_batt_label, "%d %%", soc);
        // 低电量(<20%)变红,否则常态偏深
        lv_obj_set_style_text_color(s_batt_label,
            soc < 20 ? lv_color_hex(0xE43B2F) : lv_color_hex(UI_INK), 0);
    }
}

// 显示页面首帧(静止预览):进入页面时默认显示菜品盘首帧,提示按下即可抽签。
static void show_idle(void)
{
    s_playing = false;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    show_frame(1, 0);                              // 类型 1(菜品盘)首帧做待机预览
    show_badge("HOLD ANY KEY", "PLACE or DISH");
}

void demo_eat_what_enter(void)
{
    s_cur_anim  = -1;
    s_cur_frame = 0;
    s_playing   = false;
    s_timer     = NULL;
    s_fast_render  = false;                 // 默认 LVGL 原路径;OK 单击切换候选快速路径

    // 候选快速刷新器:拿 panel 句柄备用。失败则快速路径不可用,但 LVGL 路径照常。
    if (!eatwhat_render_init(bsp_display_panel())) {
        ESP_LOGW(TAG, "快速渲染器初始化失败(panel 不可用),候选路径停用");
    }

    s_scr = ui_pixel_screen_create("EAT WHAT");

    // 主画面:方形动画,居中放大(200 宽,接近满宽,边距 20)。Y=48 让出标题横幅。
    s_img = lv_image_create(s_scr);
    lv_obj_set_pos(s_img, (240 - EAT_WHAT_RES) / 2, 48);
    lv_obj_set_style_radius(s_img, 0, 0);
    lv_obj_set_style_border_width(s_img, 3, 0);
    lv_obj_set_style_border_color(s_img, lv_color_hex(UI_INK), 0);

    // 底部操作提示条:与图片同宽(200)并下移留出间距(图底=48+200=248,提示条从 258 起)。
    s_badge = ui_pixel_panel_create(s_scr, 20, 258, 200, 38, UI_PAPER);
    s_badge_label = lv_label_create(s_badge);
    lv_obj_set_style_text_font(s_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_badge_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_badge_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_badge_label);

    // 右上角电量显示:初始化电量计(幂等),读不到则显示 -- %。失败不阻塞主功能。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角显示 -- %%");
    }
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    // 右对齐到屏幕右上角,不遮挡左侧标题横幅(5..156)与顶部云(已移除)。
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -6, 10);
    battery_refresh(NULL);                         // 先立即显示一次
    s_batt_timer = lv_timer_create(battery_refresh, 2000, NULL);

    show_idle();
    lv_screen_load(s_scr);
}

void demo_eat_what_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_batt_timer) { lv_timer_delete(s_batt_timer); s_batt_timer = NULL; }
    s_playing = false;
    eatwhat_render_deinit();          // 释放候选渲染器的行缓冲
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL; s_img = NULL; s_badge = NULL; s_badge_label = NULL; s_batt_label = NULL;
    }
}

void demo_eat_what_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 按下任意键(UP/DOWN/OK):立即抽签并开始滚动,不等 CLICK 判定(否则有几百 ms 延迟)。
    if (ev == BSP_BTN_PRESS) {
        start_play(ui_eatwhat_pick_type(esp_random()));
        return;
    }

    // OK 长按:在 LVGL 原路径与候选快速路径(局部+隔行)间切换,便于上板 A/B。
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        s_fast_render = !s_fast_render;
        // 快速路径:LVGL 图片 obj 隐藏,由 panel 直画;回 LVGL 路径恢复显示。
        if (s_img) {
            if (s_fast_render) {
                lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
            }
        }
        show_badge(s_fast_render ? "FAST RENDER" : "LVGL RENDER",
                   s_fast_render ? "long OK toggles back" : "long OK toggles");
        // 重新显示当前帧,保证切换后画面正确。
        if (s_cur_anim >= 0) show_frame(s_cur_anim, (uint32_t)s_cur_frame);
        ESP_LOGI(TAG, "渲染路径切换:%s", s_fast_render ? "FAST(隔行)" : "LVGL");
    }
}
