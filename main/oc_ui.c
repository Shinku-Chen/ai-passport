// main/oc_ui.c —— 对讲机界面实现(见 oc_ui.h)。
//
// 布局(240x320 竖屏):
//   ┌───────────────────────────────┐
//   │ ● 就绪            12:34  85%  │  状态栏:状态灯 + 状态字 + 时间 + 电量
//   ├───────────────────────────────┤
//   │  我: 今天天气怎么样            │  对话区:可滚动的消息气泡(角色 + 正文)
//   │  助手: 今天多云,最高 24 度…   │  最多保留最近 6 条,自动滚到最新
//   ├───────────────────────────────┤
//   │ 长按 OK 说话 · 短按设置        │  提示行:操作指引 / 错误提示
//   └───────────────────────────────┘
//
// 内存约束(无 PSRAM):每条消息最多显示 1024 字节(约 340 个汉字),超出截断并提示去
// App 看全文;只保留最近 6 条气泡,避免长回复把 LVGL 堆吃光。
#include "oc_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "bsp_pins.h"   // BSP_LCD_W/H:屏幕逻辑尺寸(红/绿两态提示要按整屏居中)
#include "esp_log.h"
#include "lvgl.h"
#include "oc_proto.h"
#include "oc_settings.h"

// 完整 GB2312 字库(6763 汉字 + 全角/数学/单位符号 + ASCII):对讲机显示的是 App 识别
// 文本与网关回复,内容不可预测,所以用全量字库而不是子集,避免出现缺字方框。
// 16 px / 4 bpp 的 PLAIN 位图,由 tools/intercom_font.py 生成到 assets/fonts/
// (旧的 14 px / 2 bpp 版本屏上太糊,已整体替换)。
LV_FONT_DECLARE(lv_font_intercom_cjk_16);

#define OC_UI_FONT (&lv_font_intercom_cjk_16)

static const char *TAG = "oc_ui";

// 对话区保留的气泡数。4 个:屏上高度只放得下约 4 条短消息,而**每个气泡要在 LVGL 池里占
// 约 700–1300 字节**(对象+两个标签+文本副本),6 个会把 16–20KB 的池吃光 → 绘制分配失败
// → 花屏 / panic 重启(真机实测:池 空闲 从 6152 掉到 1652 后崩)。
#define OC_UI_BUBBLES 4U
// 单条消息显示上限(字节,UTF-8)。1024B ≈ 340 个汉字,远超过一屏;对话区可滚动查看。
// 单条气泡显示上限 2048 B(≈680 汉字;原先 1 KB ≈340 汉字)。上限与协议层的
// OC_TEXT_PAYLOAD_MAX / OC_TEXT_MERGE_CAP 对齐:一条消息在链路上最多 2048 B,
// 现在它们能**完整显示**,不再在 1 KB 处截断。
// 代价是 4 个气泡的静态缓冲从 4 KB 变成 8 KB —— 这 4 KB 来自 BSP 绘图缓冲
// 40->24 行的让出(见 components/bsp/src/bsp_display_lvgl.c 的注释与真机水位数据)。
//
// 关键:**文本放在固件自己的静态缓冲里**(见 s_bubble_text + lv_label_set_text_static),
// 不再复制进 LVGL 池 —— 所以加大这个上限几乎不花池,只花静态 DRAM(本机还剩 70+KB)。
#define OC_UI_TEXT_MAX 2048U

// UP/DOWN 一次滚动的行数(用户要求 8 行);实际像素 = 8 × 当前字体行高,不写死。
#define OC_UI_SCROLL_LINES 8
#define OC_UI_BG 0x0E1116
#define OC_UI_BAR_BG 0x161B22
#define OC_UI_PANEL 0x1B222C
#define OC_UI_INK 0xE6EDF3
#define OC_UI_INK_READY 0xFFFFFF   // 「就绪」两行都用纯白
#define OC_UI_INK_DIM 0x8B949E
#define OC_UI_ACCENT_U 0x6FC3FF          // 用户文本
#define OC_UI_PAIRING_PANEL_TIMEOUT_MS 90000U   // 配对码面板最长停留(超时自动隐藏)
#define OC_UI_ACCENT_A 0x9BE9A8          // 助手文本
#define OC_UI_ACCENT_R 0xFFC66D          // 系统提示
#define OC_UI_DOT_OK 0x3FB950
#define OC_UI_DOT_WARN 0xD29922
#define OC_UI_DOT_OFF 0x8B949E
#define OC_UI_DOT_BUSY 0x58A6FF
#define OC_UI_BG_RECORD 0x5C0F0F        // 按下准备中:整屏转红(即时反馈,松开/切绿后恢复)
#define OC_UI_BG_RECORD_READY 0x0F5C0F  // 真正可说话:整屏转绿(亮度观感与红底一致)

static struct {
    lv_obj_t *scr;
    lv_obj_t *dot;
    lv_obj_t *state;
    lv_obj_t *gw_dot;
    lv_obj_t *gw;
    lv_obj_t *clock;
    lv_obj_t *batt;
    lv_obj_t *conv;
    lv_obj_t *turn_hint;             // 红/绿两态的居中大字提示
    lv_obj_t *pair_panel;
    lv_obj_t *pair_label;
    uint32_t pair_ms;             // 配对码面板已显示的毫秒数(0=未显示;1s 计时器累加,超时兜底)
    lv_obj_t *bubbles[OC_UI_BUBBLES];
    /** 每个气泡的文本缓冲(静态 DRAM):配合 lv_label_set_text_static,不占 LVGL 池。 */
    unsigned bubble_next;
    unsigned bubble_used;
    // 设置页另起一屏:进入时整屏切换,退出时切回对讲屏。
    lv_obj_t *set_scr;
    lv_obj_t *set_title;
    lv_obj_t *set_body;
    lv_obj_t *set_hint;
    bool settings_active;
    lv_timer_t *timer;
    uint32_t idle_ms;             // 距离上次活动的毫秒数(1s 计时器累加)
    bool backlight_on;
    int battery;                  // -1 = 不可用
    int64_t epoch;                // 0 = 未同步
    int last_clock_minutes;       // 只在分钟变化时刷新标签
    char device_name[24];
    // 两种状态:设备(BLE 链路)与网关(手机上报)。两者都就绪才显示单个「就绪」。
    oc_ui_state_t dev_state;
    char dev_detail[48];
    oc_ui_gateway_state_t gw_state;
    char gw_detail[96];
} s_ui;

static const char *state_text(oc_ui_state_t state)
{
    switch (state) {
    case OC_UI_STATE_IDLE: return "未连接";
    case OC_UI_STATE_CONNECTING: return "连接中";
    case OC_UI_STATE_PAIRING: return "配对中";
    case OC_UI_STATE_READY: return "就绪";
    case OC_UI_STATE_RECORDING: return "准备中";
    case OC_UI_STATE_RECORDING_READY: return "可说话";
    case OC_UI_STATE_SENDING: return "发送中";
    case OC_UI_STATE_RECEIVING: return "接收中";
    default: return "";
    }
}

static uint32_t state_color(oc_ui_state_t state)
{
    switch (state) {
    case OC_UI_STATE_READY: return OC_UI_DOT_OK;
    case OC_UI_STATE_RECORDING_READY: return OC_UI_DOT_OK;
    case OC_UI_STATE_RECORDING: return OC_UI_DOT_BUSY;
    case OC_UI_STATE_SENDING:
    case OC_UI_STATE_RECEIVING: return OC_UI_DOT_BUSY;
    case OC_UI_STATE_PAIRING: return OC_UI_DOT_WARN;
    default: return OC_UI_DOT_OFF;
    }
}

static const char *gw_text(oc_ui_gateway_state_t state)
{
    switch (state) {
    case OC_UI_GATEWAY_READY: return "就绪";
    case OC_UI_GATEWAY_CONNECTING: return "连接中";
    case OC_UI_GATEWAY_WORKING: return "工作中";
    case OC_UI_GATEWAY_OFFLINE: return "异常";
    default: return "未知";
    }
}

static uint32_t gw_color(oc_ui_gateway_state_t state)
{
    switch (state) {
    case OC_UI_GATEWAY_READY: return OC_UI_DOT_OK;
    case OC_UI_GATEWAY_CONNECTING:
    case OC_UI_GATEWAY_WORKING: return OC_UI_DOT_BUSY;
    case OC_UI_GATEWAY_OFFLINE: return OC_UI_DOT_WARN;
    default: return OC_UI_DOT_OFF;
    }
}

// 重画状态栏与底部提示(调用时必须已持 LVGL 锁)。
// 规则:设备与网关都就绪 → 只显示一个「就绪」;否则分别列出设备与网关状态,
// 并把网关不可用的具体原因顶到底部提示行(那里能换行,看得全)。
// 【诊断】LVGL 池快照:定位“多轮对话后池被吃光 → 花屏/重启”。
// 用法:在可疑的分配路径前后各打一条,比较 空闲/块数 就能看出是谁在吃池。
static void ui_mem_dbg(const char *where)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    ESP_LOGI(TAG, "  [池] %-18s 空闲=%u 最大块=%u 用量=%u%% 块=%u/%u 碎片=%u%%",
             where, (unsigned)m.free_size, (unsigned)m.free_biggest_size,
             (unsigned)m.used_pct, (unsigned)m.used_cnt, (unsigned)m.free_cnt,
             (unsigned)m.frag_pct);
}

static void refresh_status(void)
{
    bool gw_ready = (s_ui.gw_state == OC_UI_GATEWAY_READY);

    if (s_ui.state == NULL) {
        return;
    }

    // 两行状态常驻显示(不因“都就绪”而隐藏网关那行):
    // 用户要知道“设备通、网关也通”,而不是只能看到一个“就绪”。
    const char *dev_text = s_ui.dev_detail[0] ? s_ui.dev_detail : state_text(s_ui.dev_state);
    lv_label_set_text_fmt(s_ui.state, "设备 %s", dev_text);
    // 「就绪」与红/绿两态(整屏有色)用纯白,其余保持浅灰
    bool dev_white = (s_ui.dev_state == OC_UI_STATE_READY) ||
                     (s_ui.dev_state == OC_UI_STATE_RECORDING) ||
                     (s_ui.dev_state == OC_UI_STATE_RECORDING_READY);
    lv_obj_set_style_text_color(s_ui.state, lv_color_hex(dev_white ? OC_UI_INK_READY : OC_UI_INK), 0);
    lv_obj_set_style_bg_color(s_ui.dot, lv_color_hex(state_color(s_ui.dev_state)), 0);

    if (s_ui.gw != NULL) {
        lv_obj_clear_flag(s_ui.gw_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_ui.gw_dot, lv_color_hex(gw_color(s_ui.gw_state)), 0);
        lv_obj_set_style_text_color(s_ui.gw,
                                    lv_color_hex(gw_ready ? OC_UI_INK_READY : OC_UI_INK_DIM), 0);
        if (s_ui.gw_state == OC_UI_GATEWAY_UNKNOWN) {
            lv_label_set_text(s_ui.gw, "网关 未知");
        } else if (gw_ready) {
            lv_label_set_text(s_ui.gw, "网关 就绪");
        } else {
            // 状态词与 App 侧保持一致(连接中/工作中/异常),具体原因放到底部提示行。
            lv_label_set_text_fmt(s_ui.gw, "网关 %s", gw_text(s_ui.gw_state));
        }
    }

    // 底部不再有提示行:对话区一直占满到屏幕底部,提示信息走状态栏或系统消息。
}

// 整屏主题:按下准备中 = 红、真正可说话 = 绿,其余 = 常规底色。
// 红/绿两态额外显示居中大字,因为状态栏只有几个字,放不下完整句子。
static void apply_turn_theme(oc_ui_state_t state)
{
    uint32_t bg = OC_UI_BG;
    // 红/绿两态只改整屏底色:轮次状态文字只出现在顶部「设备」那一行(设备 准备中 / 设备 可说话),
    // 屏中间不再放任何提示(用户要求:不要居中大字,只在设备状态显示)。
    if (state == OC_UI_STATE_RECORDING) {
        bg = OC_UI_BG_RECORD;
    } else if (state == OC_UI_STATE_RECORDING_READY) {
        bg = OC_UI_BG_RECORD_READY;
    }

    if (s_ui.scr != NULL) {
        lv_obj_set_style_bg_color(s_ui.scr, lv_color_hex(bg), 0);
    }
    if (s_ui.conv != NULL) {
        lv_obj_set_style_bg_color(s_ui.conv, lv_color_hex(bg), 0);
    }
    if (s_ui.turn_hint != NULL) {
        lv_obj_add_flag(s_ui.turn_hint, LV_OBJ_FLAG_HIDDEN);   // 居中提示恒隐藏(保留控件以免其它路径引用)
    }
}

static void apply_backlight(void)
{
    uint8_t bright = oc_settings_brightness();
    bsp_display_backlight(bright);
    s_ui.backlight_on = bright > 0;
}

// 1s 计时器(跑在 LVGL 任务里):背光超时 + 状态栏时间。
static void ui_timer(lv_timer_t *timer)
{
    (void)timer;
    s_ui.idle_ms += 1000;

    uint16_t timeout_s = oc_settings_backlight_timeout_s();
    if (s_ui.backlight_on && s_ui.idle_ms >= (uint32_t)timeout_s * 1000U) {
        // 背光熄灭不影响 BLE 连接与录音:链路常连,按键会把屏幕点亮。
        bsp_display_backlight(0);
        s_ui.backlight_on = false;
    }

    // 配对码面板兜底:正常路径在加密完成/订阅完成/断开时都会立刻隐藏它(见 oc_app.c 的事件处理),
    // 但用户可能在手机上取消配对、或对端卡在加密阶段 —— 面板不能无限期挡住对话界面。
    if (s_ui.pair_ms > 0) {
        s_ui.pair_ms += 1000;
        if (s_ui.pair_ms > OC_UI_PAIRING_PANEL_TIMEOUT_MS) {
            s_ui.pair_ms = 0;
            if (s_ui.pair_panel != NULL) {
                lv_obj_add_flag(s_ui.pair_panel, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_ui.epoch > 0 && s_ui.clock != NULL) {
        // 设备没有时区数据库,按东八区(用户所在地)换算显示。
        int minutes = (int)(((s_ui.epoch + 8 * 3600) % 86400) / 60);
        if (minutes != s_ui.last_clock_minutes) {
            s_ui.last_clock_minutes = minutes;
            lv_label_set_text_fmt(s_ui.clock, "%02d:%02d", minutes / 60, minutes % 60);
        }
    }
}

void oc_ui_note_activity(void)
{
    s_ui.idle_ms = 0;
    if (!s_ui.backlight_on) {
        apply_backlight();
    }
}

void oc_ui_apply_brightness(void)
{
    apply_backlight();
}

// ---- 构建对讲屏 ----
static void build_conv_area(lv_obj_t *parent)
{
    s_ui.conv = lv_obj_create(parent);
    lv_obj_set_size(s_ui.conv, 232, 264);
    lv_obj_align(s_ui.conv, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_ui.conv, lv_color_hex(OC_UI_BG), 0);
    lv_obj_set_style_border_width(s_ui.conv, 0, 0);
    lv_obj_set_style_pad_all(s_ui.conv, 6, 0);
    lv_obj_set_style_pad_row(s_ui.conv, 6, 0);
    lv_obj_set_flex_flow(s_ui.conv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ui.conv, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.conv, LV_SCROLLBAR_MODE_AUTO);
    // 关掉回弹与惯性:历史只应该从首条滚到末条,到头就停(有回弹/惯性时会“滚过头”)。
    lv_obj_remove_flag(s_ui.conv, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

esp_err_t oc_ui_init(const char *device_name)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.battery = -1;
    s_ui.last_clock_minutes = -1;
    if (device_name != NULL) {
        snprintf(s_ui.device_name, sizeof(s_ui.device_name), "%s", device_name);
    }

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 加锁失败,界面未建立");
        return ESP_FAIL;
    }

    s_ui.scr = lv_obj_create(NULL);
    // 屏幕尺寸必须显式设定:lv_obj_create(NULL) 建的是「挂在当前屏上的普通对象」,
    // 不给尺寸时它的内容盒很小,LV_ALIGN_CENTER 就只能在那个小盒里居中
    // (真机现象:红/绿两态的居中提示跑到了顶部状态栏那一条)。
    lv_obj_set_size(s_ui.scr, BSP_LCD_W, BSP_LCD_H);
    lv_obj_set_style_bg_color(s_ui.scr, lv_color_hex(OC_UI_BG), 0);
    lv_obj_set_style_pad_all(s_ui.scr, 0, 0);
    lv_obj_clear_flag(s_ui.scr, LV_OBJ_FLAG_SCROLLABLE);

    // 状态栏:两行 —— 第一行「设备」(含时间/电量),第二行「网关」。
    lv_obj_t *bar = lv_obj_create(s_ui.scr);
    lv_obj_set_size(bar, 240, 46);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(OC_UI_BAR_BG), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.dot = lv_obj_create(bar);
    lv_obj_set_size(s_ui.dot, 8, 8);
    lv_obj_align(s_ui.dot, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(s_ui.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.dot, 0, 0);
    lv_obj_set_style_bg_color(s_ui.dot, lv_color_hex(OC_UI_DOT_OFF), 0);

    s_ui.state = lv_label_create(bar);
    lv_obj_set_style_text_font(s_ui.state, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.state, lv_color_hex(OC_UI_INK), 0);
    lv_obj_set_width(s_ui.state, 128);
    lv_label_set_long_mode(s_ui.state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ui.state, LV_ALIGN_TOP_LEFT, 22, 2);
    lv_label_set_text(s_ui.state, state_text(OC_UI_STATE_IDLE));

    s_ui.gw_dot = lv_obj_create(bar);
    lv_obj_set_size(s_ui.gw_dot, 8, 8);
    lv_obj_align(s_ui.gw_dot, LV_ALIGN_TOP_LEFT, 8, 29);
    lv_obj_set_style_radius(s_ui.gw_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.gw_dot, 0, 0);
    lv_obj_set_style_bg_color(s_ui.gw_dot, lv_color_hex(OC_UI_DOT_OFF), 0);

    s_ui.gw = lv_label_create(bar);
    lv_obj_set_style_text_font(s_ui.gw, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.gw, lv_color_hex(OC_UI_INK_DIM), 0);
    lv_obj_set_width(s_ui.gw, 210);
    lv_label_set_long_mode(s_ui.gw, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ui.gw, LV_ALIGN_TOP_LEFT, 22, 23);
    lv_label_set_text(s_ui.gw, "");

    s_ui.batt = lv_label_create(bar);
    lv_obj_set_style_text_font(s_ui.batt, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.batt, lv_color_hex(OC_UI_INK_DIM), 0);
    lv_obj_align(s_ui.batt, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_label_set_text(s_ui.batt, "--");

    s_ui.clock = lv_label_create(bar);
    lv_obj_set_style_text_font(s_ui.clock, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.clock, lv_color_hex(OC_UI_INK_DIM), 0);
    lv_obj_align(s_ui.clock, LV_ALIGN_TOP_RIGHT, -52, 2);
    lv_label_set_text(s_ui.clock, "");

    build_conv_area(s_ui.scr);

    // 红/绿两态的居中大字:整屏有色时顶在最前面,常规状态下隐藏。
    s_ui.turn_hint = lv_label_create(s_ui.scr);
    lv_obj_set_style_text_font(s_ui.turn_hint, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.turn_hint, lv_color_hex(OC_UI_INK_READY), 0);
    lv_obj_set_width(s_ui.turn_hint, 224);
    lv_obj_set_style_text_align(s_ui.turn_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.turn_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.turn_hint, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_ui.scr);
    refresh_status();   // 初始状态:设备未连接、网关未知

    s_ui.timer = lv_timer_create(ui_timer, 1000, NULL);
    s_ui.backlight_on = true;
    apply_backlight();
    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "界面就绪(%s)", s_ui.device_name[0] ? s_ui.device_name : "无名");
    return ESP_OK;
}

// ---- 状态栏与提示 ----
void oc_ui_set_state(oc_ui_state_t state, const char *detail)
{
    oc_ui_state_t prev = s_ui.dev_state;
    s_ui.dev_state = state;
    if (detail != NULL) {
        snprintf(s_ui.dev_detail, sizeof(s_ui.dev_detail), "%s", detail);
    } else {
        s_ui.dev_detail[0] = '\0';
    }
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    // 状态一变就重算整屏主题:红→绿(两个都是轮次态)也必须切换,
    // 所以只比较状态本身,不比较“是否处于轮次态”。
    ui_mem_dbg("set_state:before");
    if (state != prev) {
        apply_turn_theme(state);
    }
    refresh_status();
    ui_mem_dbg("set_state:after");
    bsp_lvgl_unlock();
}

oc_ui_state_t oc_ui_get_state(void)
{
    return s_ui.dev_state;
}

void oc_ui_set_gateway_state(oc_ui_gateway_state_t state, const char *detail)
{
    s_ui.gw_state = state;
    if (detail != NULL && detail[0] != '\0') {
        snprintf(s_ui.gw_detail, sizeof(s_ui.gw_detail), "%s", detail);
    } else {
        s_ui.gw_detail[0] = '\0';
    }
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    refresh_status();
    bsp_lvgl_unlock();
}


void oc_ui_set_battery(int percent)
{
    s_ui.battery = percent;
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    if (s_ui.batt != NULL) {
        if (percent < 0) {
            lv_label_set_text(s_ui.batt, "--");   // 读不到就不画数字,避免显示 0% 误导
        } else {
            lv_label_set_text_fmt(s_ui.batt, "%d%%", percent);
        }
    }
    bsp_lvgl_unlock();
}

void oc_ui_set_time(int64_t epoch_seconds)
{
    s_ui.epoch = epoch_seconds;
    s_ui.last_clock_minutes = -1;   // 强制下一拍刷新
}

// ---- 对话区 ----
static uint32_t role_color(char role)
{
    if (role == 'U') return OC_UI_ACCENT_U;
    if (role == 'R') return OC_UI_ACCENT_R;
    return OC_UI_ACCENT_A;
}

static void bubble_delete_oldest(void)
{
    if (s_ui.bubbles[s_ui.bubble_next] != NULL) {
        lv_obj_delete(s_ui.bubbles[s_ui.bubble_next]);
        s_ui.bubbles[s_ui.bubble_next] = NULL;
    }
}

// 每个气泡一个静态文本缓冲(与 bubbles[] 同环形下标):文字不进 LVGL 池。
static char s_bubble_text[OC_UI_BUBBLES][OC_UI_TEXT_MAX];

// 每个气泡一个静态文本缓冲(与 bubbles[] 同环形下标):文字**不进 LVGL 池**。
// 背景:LVGL 9 没有 set_text_static 之外的省内存手段,而文本一旦复制进池,
// 一条长回复就能吃掉 1KB+ 池空间(真机:6 条气泡把 16KB 池吃光 → 花屏/panic)。
static char s_bubble_text[OC_UI_BUBBLES][OC_UI_TEXT_MAX];

void oc_ui_append(char role, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    if (!bsp_lvgl_lock(800)) {
        return;
    }

    // 兜底:池快见底时先清掉全部气泡再建新的 —— 宁可少几条历史,也不能让 LVGL 分配失败
    //(失败会直接花屏甚至 panic 重启:真机实测 空闲<1.7KB 之后崩)。
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        if (mon.free_size < 4096U) {
            ESP_LOGW(TAG, "LVGL 池将满(空闲 %u),清空对话区以腾出空间", (unsigned)mon.free_size);
            oc_ui_clear_conversation();
        }
    }

    // 复用最旧的槽位:先删对象,再用它的静态文本缓冲写新内容。
    unsigned idx = s_ui.bubble_next;
    if (s_ui.bubbles[idx] != NULL) {
        lv_obj_delete(s_ui.bubbles[idx]);
        s_ui.bubbles[idx] = NULL;
    }
    char *buf = s_bubble_text[idx];

    // 正文直接顶格写:气泡边框颜色已经区分说话人(用户蓝 / 助手绿 / 系统提示琥珀),
    // 设备屏只有 320x240,原来首行那句「我/助手/提示」白占一行 —— 用户要求去掉。
    // 整体不超过 OC_UI_TEXT_MAX,且不切在多字节字符中间。
    // 截断时必须**先给省略号留出空间**:否则 keep 正好填满缓冲,"…" 一个字节都写不进去,
    // 屏上就是「无声截断」(真机反馈)。保留 3 字节后 strncat 余量必然 ≥3,
    // 也不会把省略号本身截成半个 UTF-8 字符。
    // 注意:中文省略号是**六个点**(两个 U+2026 拼成「……」),单个 … 只有三点,
    // 在 16 px 字上不够醒目(用户反馈)。预留字节数用 sizeof 算,换写法会自动跟上。
    static const char ELLIPSIS[] = "……";
    size_t room = (OC_UI_TEXT_MAX > 1U) ? (OC_UI_TEXT_MAX - 1U) : 0U;
    size_t len = strlen(text);
    bool clipped = len > room;
    size_t keep = len;
    if (clipped) {
        const size_t reserve = sizeof(ELLIPSIS) - 1U;   // 省略号占的字节数(不含结尾 NUL)
        size_t copy_room = room > reserve ? room - reserve : 0U;
        keep = oc_utf8_safe_len((const uint8_t *)text, copy_room);
    }
    memcpy(buf, text, keep);
    buf[keep] = '\0';
    if (clipped) {
        // 尾巴只留一个省略号:设备屏很窄,"(全文见 App)" 会把有用内容挤掉。
        // 完整正文在 App 的对话列表里。
        strncat(buf, ELLIPSIS, OC_UI_TEXT_MAX - keep - 1U);
    }

    // 一个 label 就是一个气泡:背景/边框/圆角直接加在它身上,省掉"盒子 + 角色标签"两个对象。
    // 角色靠边框颜色区分(见 role_color),不在文本里再写「我/助手」。
    lv_obj_t *body = lv_label_create(s_ui.conv);
    lv_obj_set_width(body, 216);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_color(body, lv_color_hex(OC_UI_PANEL), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(role_color(role)), 0);
    lv_obj_set_style_radius(body, 6, 0);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_style_text_font(body, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(OC_UI_INK), 0);
    // 文本**不复制**进 LVGL 池:缓冲由固件静态持有,随气泡槽位复用。
    lv_label_set_text_static(body, buf);

    s_ui.bubbles[idx] = body;
    s_ui.bubble_next = (idx + 1U) % OC_UI_BUBBLES;
    if (s_ui.bubble_used < OC_UI_BUBBLES) {
        s_ui.bubble_used++;
    }

    // 新消息定位到**这条气泡的头部**(用户要求):先看到第一行,而不是直接跳到末尾。
    // lv_obj_scroll_to_view() 对「比视口还高的对象」会贴到底部,所以这里显式算位移:
    // 用屏幕坐标求「气泡顶边 → 视口顶边」的差值,再交给 lib 做边界钳制
    // (短消息在底部时钳制后自然显示末条,长消息则停在它的第一行)。
    {
        lv_area_t bubble_area;
        lv_area_t view_area;
        lv_obj_get_coords(body, &bubble_area);
        lv_obj_get_coords(s_ui.conv, &view_area);
        lv_obj_scroll_by_bounded(s_ui.conv, 0, bubble_area.y1 - view_area.y1, LV_ANIM_OFF);
    }
    ESP_LOGI(TAG, "气泡[%u]: 正文 %u/%u 字节%s", idx, (unsigned)keep, (unsigned)len,
                 clipped ? "(已截断,尾部加省略号)" : "");
    ui_mem_dbg("append:after");
    bsp_lvgl_unlock();
}

void oc_ui_clear_conversation(void)
{
    if (!bsp_lvgl_lock(800)) {
        return;
    }
    for (unsigned i = 0; i < OC_UI_BUBBLES; i++) {
        if (s_ui.bubbles[i] != NULL) {
            lv_obj_delete(s_ui.bubbles[i]);
            s_ui.bubbles[i] = NULL;
        }
    }
    s_ui.bubble_next = 0;
    s_ui.bubble_used = 0;
    bsp_lvgl_unlock();
}

void oc_ui_scroll(int direction)
{
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    // direction > 0 = 往前(更旧):内容下移,靠近首条;
    //   direction < 0 = 往后(更新):内容上移,靠近末条。
    // 到首/末条就不再移动:在边缘继续按不应有任何滚动手感。
    if (direction > 0 && lv_obj_get_scroll_top(s_ui.conv) <= 0) {
        bsp_lvgl_unlock();
        return;
    }
    if (direction < 0 && lv_obj_get_scroll_bottom(s_ui.conv) <= 0) {
        bsp_lvgl_unlock();
        return;
    }
    // 一次滚 OC_UI_SCROLL_LINES 行(用户要求):行高从字体取,换字体/字号不会走样。
    const int32_t step = (int32_t)lv_font_get_line_height(OC_UI_FONT) * OC_UI_SCROLL_LINES;
    lv_obj_scroll_by_bounded(s_ui.conv, 0, direction > 0 ? step : -step, LV_ANIM_ON);
    bsp_lvgl_unlock();
}

// ---- 配对码面板 ----
void oc_ui_show_pairing(uint32_t passkey)
{
    // 注意:加锁失败时绝不能去 unlock —— 这不是自己的锁,错误释放会把 LVGL 的递归
    // 互斥量状态弄坏,后续所有加锁都会超时(表现为界面卡住、事件队列堆满)。
    if (!bsp_lvgl_lock(800)) {
        return;
    }
    if (s_ui.scr == NULL) {
        bsp_lvgl_unlock();
        return;
    }
    if (s_ui.pair_panel == NULL) {
        s_ui.pair_panel = lv_obj_create(s_ui.scr);
        // 面板里的提示共 6 行 × 20 px = 120 px,留出内边距不裁切。
        lv_obj_set_size(s_ui.pair_panel, 216, 148);
        lv_obj_center(s_ui.pair_panel);
        lv_obj_set_style_bg_color(s_ui.pair_panel, lv_color_hex(OC_UI_PANEL), 0);
        lv_obj_set_style_border_width(s_ui.pair_panel, 2, 0);
        lv_obj_set_style_border_color(s_ui.pair_panel, lv_color_hex(OC_UI_DOT_WARN), 0);
        lv_obj_set_style_radius(s_ui.pair_panel, 8, 0);
        lv_obj_clear_flag(s_ui.pair_panel, LV_OBJ_FLAG_SCROLLABLE);

        s_ui.pair_label = lv_label_create(s_ui.pair_panel);
        lv_obj_set_style_text_font(s_ui.pair_label, OC_UI_FONT, 0);
        lv_obj_set_style_text_color(s_ui.pair_label, lv_color_hex(OC_UI_INK), 0);
        lv_obj_set_style_text_align(s_ui.pair_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_ui.pair_label);
    }
    lv_obj_clear_flag(s_ui.pair_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_ui.pair_label, "配对中\n\n配对密码\n%06u\n\n请在手机上输入", (unsigned)passkey);
    lv_obj_move_foreground(s_ui.pair_panel);
    s_ui.pair_ms = 1;   // 启动兜底计时(>0 即视为正在显示)
    bsp_lvgl_unlock();
}

void oc_ui_hide_pairing(void)
{
    if (!bsp_lvgl_lock(800)) {
        return;
    }
    if (s_ui.pair_panel != NULL) {
        lv_obj_add_flag(s_ui.pair_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_ui.pair_ms = 0;   // 停掉兜底计时
    bsp_lvgl_unlock();
}

// ---- 设置页 ----
void oc_ui_settings_open(const char *title, const char *body, const char *hint)
{
    if (!bsp_lvgl_lock(800)) {
        return;
    }
    if (s_ui.set_scr == NULL) {
        s_ui.set_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_ui.set_scr, lv_color_hex(OC_UI_BG), 0);
        lv_obj_set_style_pad_all(s_ui.set_scr, 0, 0);
        lv_obj_clear_flag(s_ui.set_scr, LV_OBJ_FLAG_SCROLLABLE);

        s_ui.set_title = lv_label_create(s_ui.set_scr);
        lv_obj_set_style_text_font(s_ui.set_title, OC_UI_FONT, 0);
        lv_obj_set_style_text_color(s_ui.set_title, lv_color_hex(OC_UI_INK), 0);
        lv_obj_align(s_ui.set_title, LV_ALIGN_TOP_LEFT, 12, 12);

        s_ui.set_body = lv_label_create(s_ui.set_scr);
        lv_obj_set_style_text_font(s_ui.set_body, OC_UI_FONT, 0);
        lv_obj_set_style_text_color(s_ui.set_body, lv_color_hex(OC_UI_INK), 0);
        lv_obj_set_width(s_ui.set_body, 216);
        lv_label_set_long_mode(s_ui.set_body, LV_LABEL_LONG_WRAP);
        lv_obj_align(s_ui.set_body, LV_ALIGN_TOP_LEFT, 12, 48);

        s_ui.set_hint = lv_label_create(s_ui.set_scr);
        lv_obj_set_style_text_font(s_ui.set_hint, OC_UI_FONT, 0);
        lv_obj_set_style_text_color(s_ui.set_hint, lv_color_hex(OC_UI_INK_DIM), 0);
        lv_obj_set_width(s_ui.set_hint, 216);
        lv_label_set_long_mode(s_ui.set_hint, LV_LABEL_LONG_WRAP);
        lv_obj_align(s_ui.set_hint, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    }
    lv_label_set_text(s_ui.set_title, title != NULL ? title : "");
    lv_label_set_text(s_ui.set_body, body != NULL ? body : "");
    lv_label_set_text(s_ui.set_hint, hint != NULL ? hint : "");
    lv_screen_load(s_ui.set_scr);
    s_ui.settings_active = true;
    bsp_lvgl_unlock();
}

void oc_ui_settings_update(const char *body)
{
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    if (s_ui.set_body != NULL) {
        lv_label_set_text(s_ui.set_body, body != NULL ? body : "");
    }
    bsp_lvgl_unlock();
}

void oc_ui_settings_close(void)
{
    if (!bsp_lvgl_lock(800)) {
        return;
    }
    if (s_ui.scr != NULL) {
        lv_screen_load(s_ui.scr);
    }
    s_ui.settings_active = false;
    bsp_lvgl_unlock();
}

bool oc_ui_settings_active(void)
{
    return s_ui.settings_active;
}

void oc_ui_log_memory(void)
{
    if (!bsp_lvgl_lock(0)) {
        return;
    }
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    bsp_lvgl_unlock();
    // used_pct=当前占用百分比, max_used=历史峰值(界面全部建完后的峰值最值得看)。
    ESP_LOGI(TAG, "LVGL 池: 已用=%u%% 峰值=%u 空闲=%u 总量=%u 碎片=%u%%",
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.free_size,
             (unsigned)mon.total_size, (unsigned)mon.frag_pct);
}
