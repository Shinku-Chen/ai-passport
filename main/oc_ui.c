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

#define OC_UI_BUBBLES 6U                 // 对话区保留的气泡数
#define OC_UI_TEXT_MAX 1024U             // 单条消息显示上限(字节,UTF-8)
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
    if (state != prev) {
        apply_turn_theme(state);
    }
    refresh_status();
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

static const char *role_name(char role)
{
    if (role == 'U') return "我";
    if (role == 'R') return "提示";
    return "助手";
}

static void bubble_delete_oldest(void)
{
    if (s_ui.bubbles[s_ui.bubble_next] != NULL) {
        lv_obj_delete(s_ui.bubbles[s_ui.bubble_next]);
        s_ui.bubbles[s_ui.bubble_next] = NULL;
    }
}

void oc_ui_append(char role, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    if (!bsp_lvgl_lock(800)) {
        return;
    }

    bubble_delete_oldest();
    lv_obj_t *box = lv_obj_create(s_ui.conv);
    lv_obj_set_width(box, 216);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(OC_UI_PANEL), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(role_color(role)), 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *who = lv_label_create(box);
    lv_obj_set_style_text_font(who, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(who, lv_color_hex(role_color(role)), 0);
    lv_label_set_text(who, role_name(role));
    lv_obj_align(who, LV_ALIGN_TOP_LEFT, 0, 0);

    // 长文本按显示上限截断,且不切在多字节 UTF-8 字符中间。
    char buf[OC_UI_TEXT_MAX + 16];
    size_t len = strlen(text);
    size_t keep = len;
    bool clipped = false;
    if (keep > OC_UI_TEXT_MAX) {
        keep = oc_utf8_safe_len((const uint8_t *)text, OC_UI_TEXT_MAX);
        clipped = true;
    }
    memcpy(buf, text, keep);
    buf[keep] = '\0';
    if (clipped) {
        strncat(buf, "…(全文见 App)", sizeof(buf) - keep - 1);
    }

    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_style_text_font(body, OC_UI_FONT, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(OC_UI_INK), 0);
    lv_obj_set_width(body, 202);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, buf);
    // 角色行占一整行(16 px 字 + 4 px 行距 = 20 px),正文从第二行开始。
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);

    s_ui.bubbles[s_ui.bubble_next] = box;
    s_ui.bubble_next = (s_ui.bubble_next + 1U) % OC_UI_BUBBLES;
    if (s_ui.bubble_used < OC_UI_BUBBLES) {
        s_ui.bubble_used++;
    }

    lv_obj_scroll_to_view(box, LV_ANIM_OFF);   // 新消息总是滚到可见
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
    lv_obj_scroll_by_bounded(s_ui.conv, 0, direction > 0 ? 80 : -80, LV_ANIM_ON);
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
